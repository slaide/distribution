// SPDX-License-Identifier: GPL-2.0
// konkr-launcher — a minimal SDL2 + Dear ImGui game-launcher frontend.
//
// Selectable as the boot frontend via `system.frontend=custom` (see the
// SM8750 090-ui_service quirk). Presents a tabbed, gamepad-navigable grid:
//   - one tab to open Steam Big Picture,
//   - one tab per emulated system (Game Boy, GBC, GBA, NDS), each listing the
//     ROMs found under /storage/roms/<system>/ as an alphabetical grid,
//   - a Tools tab (currently just Device Controls).
//
// Launching shells out to the same entrypoints EmulationStation uses
// (/usr/bin/runemu.sh, /usr/bin/start_steam.sh), so games run through the
// stock emulator stack. This is intentionally a stub: no scraped art, no
// metadata, no per-game options — just discovery + launch.

#include <SDL.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <cctype>
#include <algorithm>
#include <functional>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

// ----------------------------------------------------------- small helpers

static std::string run_cmd(const char *cmd)
{
    std::string out;
    FILE *p = popen(cmd, "r");
    if (!p)
        return out;
    char line[256];
    while (fgets(line, sizeof line, p))
        out += line;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// single-quote a string for safe interpolation into a /bin/sh command line
static std::string sh_quote(const std::string &s)
{
    std::string q = "'";
    for (char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    q += "'";
    return q;
}

// read `get_setting <key> <system>` (e.g. gba.emulator), trimmed; "" if unset
static std::string get_setting(const char *key, const char *system)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             ". /etc/profile 2>/dev/null; get_setting %s %s 2>/dev/null", key, system);
    return run_cmd(cmd);
}

// ====================================================================
//  Steam per-game FEX tuning
//
//  Edits per-game FEX profiles consumed by the KONKR FEX-Tuned compat tool
//  (/storage/.local/share/Steam/compatibilitytools.d/konkr-fextuned). A
//  profile is a plain env file at /storage/.config/fex-emu/per-game/<appid>.conf
//  exporting FEX_* (env wins over every FEX config file). Enabling tuning for a
//  game writes the game's CompatToolMapping in Steam's config.vdf; this is only
//  safe because konkr-launcher runs while Steam is closed (Steam launches on
//  demand from the Steam tab).
// ====================================================================
namespace steamfex {

static const char *PROFILE_DIR = "/storage/.config/fex-emu/per-game";
static const char *STEAM_ROOT  = "/storage/.local/share/Steam";
static const char *TOOL_NAME   = "konkr_fextuned";

// FEX boolean tuning knobs surfaced in the UI. Tri-state per game:
//   -1 = leave FEX default (don't write), 0 = off (=0), 1 = on (=1).
struct FexFlag { const char *env; const char *label; };
static const FexFlag FEX_BOOLS[] = {
    { "FEX_TSOENABLED",           "TSO (memory ordering)" },
    { "FEX_VECTORTSOENABLED",     "Vector TSO" },
    { "FEX_MEMCPYSETTSOENABLED",  "memcpy/memset TSO" },
    { "FEX_HALFBARRIERTSOENABLED","Half-barrier TSO" },
    { "FEX_X87REDUCEDPRECISION",  "x87 reduced precision" },
    { "FEX_MULTIBLOCK",           "Multiblock" },
    { "FEX_SMALLTSCSCALE",        "Small TSC scale" },
    { "FEX_VOLATILEMETADATA",     "Volatile metadata" },
    { "FEX_HIDEHYPERVISORBIT",    "Hide hypervisor bit" },
    { "FEX_NEEDSSECCOMP",         "NeedsSeccomp (anti-cheat)" },
};
static const int N_FEX_BOOLS = (int)(sizeof(FEX_BOOLS) / sizeof(*FEX_BOOLS));

// Preset names (combo index 0 = Custom = "don't touch on select").
static const char *PRESET_NAMES[] = {
    "Custom", "Stability", "Compatibility", "Intermediate",
    "PlayStation", "Performance", "Extreme", "Denuvo",
};
static const int N_PRESETS = (int)(sizeof(PRESET_NAMES) / sizeof(*PRESET_NAMES));

// Tri-state table, columns match FEX_BOOLS order; rows are presets 1..7.
// (GameNative's preset table, plus PlayStation.)  -1 default, 0 off, 1 on.
//
// PlayStation = Intermediate's fast TSO set but x87 at FULL precision: the
// PlayStation PC SDK anti-tamper (all Sony ports post-GoW-Ragnarök: HZD
// Remastered, Until Dawn, Spider-Man 2, TLOU2-R, ...) deliberately abuses
// x87 precision and hangs or corrupts pointers under reduced precision
// (FEX-Emu/FEX#4556, maintainer-verified per-game). Full TSO is NOT needed
// for these titles — x87 is the only load-bearing flag.
static const int PRESET_TRI[N_PRESETS][N_FEX_BOOLS] = {
    /* Custom        */ { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 },
    /* Stability     */ {  1, 1, 1, 1, 0, 0,-1,-1,-1,-1 },
    /* Compatibility */ {  1, 1, 1, 1, 0, 1,-1,-1,-1,-1 },
    /* Intermediate  */ {  1, 0, 0, 1, 1, 1,-1,-1,-1,-1 },
    /* PlayStation   */ {  1, 0, 0, 1, 0, 1,-1,-1,-1,-1 },
    /* Performance   */ {  0, 0, 0, 0, 1, 1,-1,-1,-1,-1 },
    /* Extreme       */ {  0, 0, 0, 0, 1, 1, 1, 1,-1,-1 },
    /* Denuvo        */ {  0, 0, 0, 0, 1, 1, 1, 1, 1,-1 },
};
// SMCChecks per preset: 0 default, 1 none, 2 mtrack, 3 full.
static const int PRESET_SMC[N_PRESETS] = { 0, 0, 0, 0, 0, 0, 0, 3 };
static const char *SMC_VALUES[] = { "", "none", "mtrack", "full" };

// A Turnip/Mesa/DXVK perf baseline. Deliberately NO TU_DEBUG flags and no
// shader-cache size cap: TU_DEBUG values are part of Turnip's shader-cache
// key, so toggling one invalidates the game's entire warm cache (a full
// pipeline recompile — minutes-to-hours of single-digit fps on FEX), and a
// cap below the game's working set (AAA caches run >0.7 GB) causes endless
// eviction churn. WINEESYNC/WINEFSYNC are NOT inert here: proton-cachyos is
// wine-tkg based, where sync is opt-in via these env vars (unlike Valve
// Proton) — without them sync falls back to wineserver and sync-heavy games
// hang or black-screen at launch.
static const char *RECOMMENDED_ENV =
    "MESA_VK_WSI_PRESENT_MODE=mailbox "
    "MESA_SHADER_CACHE_DISABLE=false mesa_glthread=true "
    "WINEESYNC=1 WINEFSYNC=1";

struct Tune {
    int tri[N_FEX_BOOLS];
    int smc;            // index into SMC_VALUES
    int preset;         // index into PRESET_NAMES
    std::string extra;  // freeform extra env lines, verbatim
    bool mapping;       // config.vdf currently maps this game to our tool
};

static void tune_defaults(Tune &t)
{
    for (int i = 0; i < N_FEX_BOOLS; i++) t.tri[i] = -1;
    t.smc = 0;
    t.preset = 0;
    t.extra.clear();
    t.mapping = false;
}

static void apply_preset(Tune &t, int p)
{
    if (p <= 0 || p >= N_PRESETS) return;
    for (int i = 0; i < N_FEX_BOOLS; i++) t.tri[i] = PRESET_TRI[p][i];
    t.smc = PRESET_SMC[p];
    t.preset = p;
}

// ---- file IO ----
static std::string read_file(const std::string &path)
{
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

static bool write_file(const std::string &path, const std::string &data)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// ---- minimal text VDF / KeyValues ----
struct KV {
    std::string key;
    std::string value;
    bool is_object = false;
    std::vector<KV> children;
};

struct VdfTok {
    const std::string &s;
    size_t i = 0;
    explicit VdfTok(const std::string &str) : s(str) {}
    // 0 = string token (in `out`), 1 = '{', 2 = '}', -1 = eof
    int next(std::string &out)
    {
        out.clear();
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            break;
        }
        if (i >= s.size()) return -1;
        char c = s[i];
        if (c == '{') { i++; return 1; }
        if (c == '}') { i++; return 2; }
        if (c == '"') {
            i++;
            while (i < s.size()) {
                char d = s[i];
                if (d == '\\' && i + 1 < s.size()) {
                    char e = s[i + 1];
                    out += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                    i += 2;
                    continue;
                }
                if (d == '"') { i++; break; }
                out += d;
                i++;
            }
            return 0;
        }
        while (i < s.size()) {
            char d = s[i];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '{' || d == '}')
                break;
            out += d;
            i++;
        }
        return 0;
    }
};

static void parse_obj(VdfTok &t, std::vector<KV> &out)
{
    std::string tok;
    for (;;) {
        int r = t.next(tok);
        if (r == -1 || r == 2) return;
        if (r != 0) continue;
        KV node;
        node.key = tok;
        std::string v;
        int r2 = t.next(v);
        if (r2 == 1) {
            node.is_object = true;
            parse_obj(t, node.children);
        } else if (r2 == 0) {
            node.value = v;
        } else {
            out.push_back(node);
            return;     // '}' or eof right after a key
        }
        out.push_back(node);
    }
}

static KV parse_vdf(const std::string &text)
{
    KV root;
    root.is_object = true;
    VdfTok t(text);
    parse_obj(t, root.children);
    return root;
}

static void esc(std::string &o, const std::string &v)
{
    for (char c : v) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
}

static void serialize_obj(const std::vector<KV> &kids, std::string &o, int depth)
{
    std::string ind(depth, '\t');
    for (const KV &n : kids) {
        o += ind; o += '"'; esc(o, n.key); o += '"';
        if (n.is_object) {
            o += '\n'; o += ind; o += "{\n";
            serialize_obj(n.children, o, depth + 1);
            o += ind; o += "}\n";
        } else {
            o += "\t\t\""; esc(o, n.value); o += "\"\n";
        }
    }
}

static std::string serialize_vdf(const KV &root)
{
    std::string o;
    serialize_obj(root.children, o, 0);
    return o;
}

static bool ieq(const std::string &a, const char *b) { return strcasecmp(a.c_str(), b) == 0; }

static KV *kv_child(KV *p, const char *key, bool create)
{
    for (auto &c : p->children)
        if (ieq(c.key, key)) return &c;
    if (!create) return nullptr;
    KV n;
    n.key = key;
    n.is_object = true;
    p->children.push_back(n);
    return &p->children.back();
}

static const std::string *kv_value(const KV *p, const char *key)
{
    for (const auto &c : p->children)
        if (ieq(c.key, key)) return &c.value;
    return nullptr;
}

static KV *compat_mapping(KV &root, bool create)
{
    static const char *path[] = { "InstallConfigStore", "Software", "Valve",
                                  "Steam", "CompatToolMapping" };
    KV *p = &root;
    for (const char *k : path) {
        p = kv_child(p, k, create);
        if (!p) return nullptr;
    }
    return p;
}

static std::string config_vdf_path() { return std::string(STEAM_ROOT) + "/config/config.vdf"; }

static bool mapping_enabled(int appid)
{
    std::string cfg = read_file(config_vdf_path());
    if (cfg.empty()) return false;
    KV root = parse_vdf(cfg);
    KV *cm = compat_mapping(root, false);
    if (!cm) return false;
    char ids[16];
    snprintf(ids, sizeof ids, "%d", appid);
    KV *g = kv_child(cm, ids, false);
    if (!g) return false;
    const std::string *nm = kv_value(g, "name");
    return nm && ieq(*nm, TOOL_NAME);
}

// Map / unmap a game to our compat tool in config.vdf. Backs up first.
static bool set_mapping(int appid, bool enable)
{
    std::string path = config_vdf_path();
    std::string cfg = read_file(path);
    // An existing config.vdf that reads back empty means the read failed, not
    // that Steam has no config — bail rather than replace it with a skeleton.
    if (cfg.empty() && access(path.c_str(), F_OK) == 0)
        return false;
    KV root = parse_vdf(cfg);
    KV *cm = compat_mapping(root, true);
    if (!cm) return false;
    char ids[16];
    snprintf(ids, sizeof ids, "%d", appid);

    KV *g = nullptr;
    for (auto &c : cm->children)
        if (ieq(c.key, ids)) { g = &c; break; }

    if (enable) {
        if (!g) {
            KV n;
            n.key = ids;
            n.is_object = true;
            cm->children.push_back(n);
            g = &cm->children.back();
        }
        g->children.clear();
        auto add = [&](const char *k, const char *v) {
            KV n; n.key = k; n.value = v; g->children.push_back(n);
        };
        add("name", TOOL_NAME);
        add("config", "");
        add("priority", "250");
    } else {
        for (size_t i = 0; i < cm->children.size(); i++)
            if (ieq(cm->children[i].key, ids)) {
                cm->children.erase(cm->children.begin() + i);
                break;
            }
    }

    if (!cfg.empty())
        write_file(path + ".konkr.bak", cfg);
    return write_file(path, serialize_vdf(root));
}

// ---- profiles (.conf env files) ----
// The wrapper sources default.conf first, then <appid>.conf, so a per-game
// file overrides the default per-flag; a flag left "Inherit" (tri == -1, not
// written) falls back to default.conf (or FEX's global config). APPID 0 is the
// in-UI sentinel for the shared default profile.
static const int DEFAULT_APPID = 0;

static std::string profile_path(int appid)
{
    char p[256];
    if (appid == DEFAULT_APPID)
        snprintf(p, sizeof p, "%s/default.conf", PROFILE_DIR);
    else
        snprintf(p, sizeof p, "%s/%d.conf", PROFILE_DIR, appid);
    return p;
}

static bool profile_exists(int appid)
{
    return access(profile_path(appid).c_str(), F_OK) == 0;
}

static bool delete_profile(int appid)
{
    return remove(profile_path(appid).c_str()) == 0;
}

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Normalize a free-form env blob (space- and/or newline-separated KEY=VALUE,
// the form compat reports use) to one KEY=VALUE per line. Tokens without '='
// are dropped. Values containing spaces are unsupported (these tuning vars
// never contain spaces), keeping paste handling simple and predictable.
static std::string normalize_env(const std::string &in)
{
    std::string out;
    size_t i = 0, n = in.size();
    while (i < n) {
        while (i < n && isspace((unsigned char)in[i])) i++;
        size_t start = i;
        while (i < n && !isspace((unsigned char)in[i])) i++;
        if (i > start) {
            std::string tok = in.substr(start, i - start);
            if (tok.find('=') != std::string::npos) { out += tok; out += '\n'; }
        }
    }
    return out;
}

static void load_profile(int appid, Tune &t)
{
    tune_defaults(t);
    t.mapping = mapping_enabled(appid);
    std::string txt = read_file(profile_path(appid));
    std::string extra;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        std::string line = trim(txt.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            const std::string tag = "# preset=";
            if (line.rfind(tag, 0) == 0) {
                std::string nm = trim(line.substr(tag.size()));
                for (int i = 0; i < N_PRESETS; i++)
                    if (strcasecmp(nm.c_str(), PRESET_NAMES[i]) == 0) t.preset = i;
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (strcasecmp(key.c_str(), "FEX_SMCCHECKS") == 0) {
            for (int i = 1; i < 4; i++)
                if (strcasecmp(val.c_str(), SMC_VALUES[i]) == 0) t.smc = i;
            continue;
        }
        bool known = false;
        for (int i = 0; i < N_FEX_BOOLS; i++)
            if (strcasecmp(key.c_str(), FEX_BOOLS[i].env) == 0) {
                t.tri[i] = (val == "1") ? 1 : (val == "0") ? 0 : -1;
                known = true;
                break;
            }
        if (!known)
            extra += key + "=" + val + "\n";
    }
    t.extra = extra;
}

static bool save_profile(int appid, const Tune &t)
{
    char mk[300];
    snprintf(mk, sizeof mk, "mkdir -p %s", PROFILE_DIR);
    (void)system(mk);

    std::string o;
    o += "# konkr-launcher FEX profile (managed; FEX_* lines are rewritten on save)\n";
    o += std::string("# preset=") + PRESET_NAMES[t.preset] + "\n";
    for (int i = 0; i < N_FEX_BOOLS; i++)
        if (t.tri[i] != -1) {
            o += FEX_BOOLS[i].env;
            o += (t.tri[i] == 1) ? "=1\n" : "=0\n";
        }
    if (t.smc != 0) {
        o += "FEX_SMCCHECKS=";
        o += SMC_VALUES[t.smc];
        o += "\n";
    }
    std::string ex = normalize_env(t.extra);
    if (!ex.empty())
        o += "\n# extra env\n" + ex;   // normalize_env already newline-terminates
    return write_file(profile_path(appid), o);
}

// ---- installed-game discovery (appmanifest_*.acf) ----
struct Game { int appid; std::string name; };

static bool is_tool_name(const std::string &n)
{
    // Filter Steam's own compat/runtime/redist "apps" out of the game list.
    return n.rfind("Proton", 0) == 0 ||
           n.find("Steam Linux Runtime") != std::string::npos ||
           n.find("Steamworks Common Redistributables") != std::string::npos;
}

static std::vector<Game> scan_games()
{
    std::vector<Game> v;
    std::set<int> seen;
    const char *dirs[] = {
        "/storage/.local/share/Steam/steamapps",
        "/storage/roms/steam/steamapps",
    };
    for (const char *dir : dirs) {
        DIR *d = opendir(dir);
        if (!d) continue;
        for (dirent *e; (e = readdir(d));) {
            std::string fn = e->d_name;
            if (fn.rfind("appmanifest_", 0) != 0) continue;
            if (fn.size() < 5 || fn.substr(fn.size() - 4) != ".acf") continue;
            KV root = parse_vdf(read_file(std::string(dir) + "/" + fn));
            KV *st = root.children.empty() ? nullptr : &root.children[0];
            if (!st) continue;
            const std::string *ida = kv_value(st, "appid");
            const std::string *nm = kv_value(st, "name");
            if (!ida) continue;
            int id = atoi(ida->c_str());
            if (id == 0 || seen.count(id)) continue;
            std::string name = nm ? *nm : ("App " + *ida);
            if (is_tool_name(name)) continue;
            seen.insert(id);
            v.push_back({ id, name });
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end(), [](const Game &a, const Game &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return v;
}

} // namespace steamfex

// --------------------------------------------------------------- systems

struct System {
    const char *key;       // roms subdir + runemu -P platform
    const char *label;     // tab title
    const char *exts;      // space-separated, leading-dot extensions
    const char *def_emu;   // fallback emulator if <sys>.emulator is unset
    const char *def_core;  // fallback core if <sys>.core is unset
};

// keep extensions in sync with config/emulators/<sys>.conf SYSTEM_EXTENSION
static const System SYSTEMS[] = {
    { "gb",  "Game Boy",         ".gb .gbc .zip .7z", "retroarch", "gambatte" },
    { "gbc", "Game Boy Color",   ".gb .gbc .zip .7z", "retroarch", "gambatte" },
    { "gba", "Game Boy Advance", ".gba .zip .7z",     "retroarch", "mgba"     },
    { "nds", "Nintendo DS",      ".nds .zip .7z",     "retroarch", "melonds"  },
};
static const int N_SYSTEMS = (int)(sizeof(SYSTEMS) / sizeof(*SYSTEMS));

// ------------------------------------------------------------- rom scan

struct Rom {
    std::string name;   // display (filename without extension)
    std::string path;   // absolute path
};

static bool ext_matches(const std::string &fname, const char *exts)
{
    // lowercase suffix compare against each ".xyz" token in `exts`
    std::string lf = fname;
    std::transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
    std::string list = exts;
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(' ', i);
        if (j == std::string::npos)
            j = list.size();
        std::string e = list.substr(i, j - i);
        if (!e.empty() && lf.size() >= e.size() &&
            lf.compare(lf.size() - e.size(), e.size(), e) == 0)
            return true;
        i = j + 1;
    }
    return false;
}

static std::vector<Rom> scan_roms(const System &s)
{
    std::vector<Rom> v;
    std::string dir = std::string("/storage/roms/") + s.key;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return v;
    for (dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')          // skip hidden + . / ..
            continue;
        std::string fname = e->d_name;
        // skip obvious non-ROM helpers ES keeps in rom dirs
        if (fname == "gamelist.xml" || fname == "media" || fname == "images")
            continue;
        if (!ext_matches(fname, s.exts))
            continue;
        std::string name = fname;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        v.push_back({ name, dir + "/" + fname });
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const Rom &a, const Rom &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return v;
}

// ------------------------------------------------------------- launching

static bool g_quit = false;
static std::function<void()> g_pending;   // run after the frame, not mid-widget

static void launch_rom(const System &s, const std::string &path)
{
    std::string emu = get_setting("emulator", s.key);
    if (emu.empty()) emu = s.def_emu;
    std::string core = get_setting("core", s.key);
    if (core.empty()) core = s.def_core;
    std::string cmd = "/usr/bin/runemu.sh " + sh_quote(path) +
                      " -P" + s.key + " --core=" + core + " --emulator=" + emu;
    (void)system(cmd.c_str());
    // runemu blocks until the emulator exits; the launcher resumes after.
}

static void launch_steam()
{
    // start_steam.sh stops sway, runs gamescope+Steam, and starts essway
    // (EmulationStation) when Steam exits — so our compositor is gone on
    // return. Quit cleanly; ES is the frontend from here.
    (void)system("/usr/bin/start_steam.sh");
    g_quit = true;
}

static void launch_tool(const char *cmd)
{
    (void)system(cmd);
}

// --------------------------------------------------------------- ui

struct Tile {
    std::string label;
    std::function<void()> action;
};

static void draw_grid(const std::vector<Tile> &tiles)
{
    if (tiles.empty()) {
        ImGui::TextDisabled("Nothing here yet.");
        return;
    }
    const float fs = ImGui::GetFontSize();
    const ImVec2 cell(fs * 9.0f, fs * 4.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (int)((avail + spacing) / (cell.x + spacing));
    if (cols < 1) cols = 1;

    for (size_t i = 0; i < tiles.size(); i++) {
        if (i % cols)
            ImGui::SameLine();
        ImGui::PushID((int)i);
        // truncate long labels so they don't overflow the tile
        std::string lbl = tiles[i].label;
        if (lbl.size() > 28)
            lbl = lbl.substr(0, 26) + "..";
        if (ImGui::Button(lbl.c_str(), cell))
            g_pending = tiles[i].action;
        ImGui::PopID();
    }
}

static void draw_system_tab(const System &s, const std::vector<Rom> &roms)
{
    ImGui::Text("%s  -  %zu game%s", s.label, roms.size(), roms.size() == 1 ? "" : "s");
    ImGui::Separator();
    std::vector<Tile> tiles;
    tiles.reserve(roms.size());
    for (const Rom &r : roms) {
        std::string path = r.path;
        const System *sp = &s;
        tiles.push_back({ r.name, [sp, path]() { launch_rom(*sp, path); } });
    }
    if (roms.empty())
        ImGui::TextWrapped("No ROMs found in /storage/roms/%s.", s.key);
    else
        draw_grid(tiles);
}

// ----------------------------------------------------- Steam tuner tab

static std::vector<steamfex::Game> g_games;   // [0] = Default sentinel, rest = games
static bool g_games_scanned = false;
static int g_sel = -1;                  // index into g_games, -1 = none
static steamfex::Tune g_tune;           // profile being edited (default or per-game)
static steamfex::Tune g_default_tune;   // snapshot of default.conf, for inherit hints
static bool g_has_override = false;     // selected game has its own <appid>.conf
static char g_extra_buf[4096] = { 0 };
static std::string g_status;

static void steam_rescan()
{
    g_games = steamfex::scan_games();
    g_games.insert(g_games.begin(),
                   { steamfex::DEFAULT_APPID, "Default (all games)" });
}

static void steam_select(int i)
{
    g_sel = i;
    g_status.clear();
    // refresh the default snapshot so per-game "Inherit" hints stay current
    steamfex::load_profile(steamfex::DEFAULT_APPID, g_default_tune);
    if (i < 0 || i >= (int)g_games.size())
        return;
    int appid = g_games[i].appid;
    steamfex::load_profile(appid, g_tune);
    g_has_override = steamfex::profile_exists(appid);
    snprintf(g_extra_buf, sizeof g_extra_buf, "%s", g_tune.extra.c_str());
}

static void draw_steam_tab()
{
    using namespace steamfex;
    const float fs = ImGui::GetFontSize();

    if (!g_games_scanned) {
        steam_rescan();
        g_games_scanned = true;
        steam_select(0);   // open on the Default profile so the panel isn't empty
    }

    if (ImGui::Button("Open Steam Big Picture"))
        g_pending = launch_steam;
    ImGui::SameLine();
    if (ImGui::Button("Rescan games")) {
        steam_rescan();
        steam_select(-1);
    }
    ImGui::TextDisabled("Per-game FEX tuning. Steam is closed here, so edits are safe.");
    ImGui::Separator();

    // left: Default + game list
    ImGui::BeginChild("games", ImVec2(fs * 14.0f, 0), true);
    for (int i = 0; i < (int)g_games.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::Selectable(g_games[i].name.c_str(), i == g_sel) && i != g_sel)
            steam_select(i);
        ImGui::PopID();
        if (i == 0)
            ImGui::Separator();   // divide Default from the installed games
    }
    if (g_games.size() <= 1)
        ImGui::TextWrapped("No installed Steam games found yet.");
    ImGui::EndChild();

    ImGui::SameLine();

    // right: editor
    ImGui::BeginChild("editor", ImVec2(0, 0), true);
    if (g_sel < 0 || g_sel >= (int)g_games.size()) {
        ImGui::TextWrapped("Pick 'Default (all games)' to set the baseline every tuned "
                           "game inherits, or a game to override it.");
    } else {
        const Game &g = g_games[g_sel];
        const bool is_default = (g.appid == DEFAULT_APPID);

        if (is_default) {
            ImGui::TextUnformatted("Default profile");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Baseline for every game with FEX tuning enabled, "
                                "unless that game sets its own override.");
            ImGui::PopTextWrapPos();
        } else {
            ImGui::Text("%s", g.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(appid %d)", g.appid);
        }
        ImGui::Separator();

        if (!is_default) {
            bool en = g_tune.mapping;
            if (ImGui::Checkbox("Use FEX tuning for this game", &en)) {
                if (set_mapping(g.appid, en)) {
                    g_tune.mapping = en;
                    g_status = en ? "Enabled (config.vdf updated)."
                                  : "Disabled (stock Proton).";
                } else {
                    g_status = "config.vdf edit failed - set 'KONKR FEX-Tuned' "
                               "manually in Steam > Properties > Compatibility.";
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(g_has_override ? "[overriding Default]"
                                               : "[inheriting Default]");
            ImGui::Spacing();
        }

        // preset
        std::string presets;
        for (int i = 0; i < N_PRESETS; i++) {
            presets += PRESET_NAMES[i];
            presets.push_back('\0');
        }
        if (ImGui::Combo("Preset", &g_tune.preset, presets.c_str()) && g_tune.preset != 0)
            apply_preset(g_tune, g_tune.preset);

        ImGui::Separator();
        ImGui::TextDisabled(is_default ? "FEX flags  ('FEX default' = inherit global config)"
                                       : "FEX flags  ('Inherit' = take the Default value)");

        // first option is contextual: Default profile vs per-game override
        const char *tri_items = is_default ? "FEX default\0On\0Off\0"
                                           : "Inherit\0On\0Off\0";
        for (int i = 0; i < N_FEX_BOOLS; i++) {
            int idx = (g_tune.tri[i] == -1) ? 0 : (g_tune.tri[i] == 1 ? 1 : 2);
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(fs * 9.0f);
            if (ImGui::Combo(FEX_BOOLS[i].label, &idx, tri_items)) {
                g_tune.tri[i] = (idx == 0) ? -1 : (idx == 1 ? 1 : 0);
                g_tune.preset = 0;   // manual edit => Custom
            }
            if (!is_default && g_tune.tri[i] == -1) {
                int dv = g_default_tune.tri[i];
                ImGui::SameLine();
                ImGui::TextDisabled("(default: %s)",
                                    dv == 1 ? "On" : dv == 0 ? "Off" : "FEX default");
            }
            ImGui::PopID();
        }

        const char *smc_items = is_default ? "FEX default\0none\0mtrack\0full\0"
                                           : "Inherit\0none\0mtrack\0full\0";
        ImGui::SetNextItemWidth(fs * 9.0f);
        if (ImGui::Combo("SMC checks", &g_tune.smc, smc_items))
            g_tune.preset = 0;
        if (!is_default && g_tune.smc == 0) {
            int ds = g_default_tune.smc;
            ImGui::SameLine();
            ImGui::TextDisabled("(default: %s)", ds == 0 ? "FEX default" : SMC_VALUES[ds]);
        }

        ImGui::Separator();
        ImGui::TextDisabled(is_default
            ? "Environment variables  (applied to every game with tuning enabled)"
            : "Environment variables  (added on top of Default; same KEY overrides it)");
        ImGui::TextDisabled("One KEY=VALUE per line, or paste a space-separated string "
                            "from a compat report. This is the lever for render-bound "
                            "games (DXVK / Mesa / Turnip / wine).");
        if (!is_default && !g_default_tune.extra.empty() &&
            ImGui::TreeNode("Inherited from Default (read-only)")) {
            ImGui::TextDisabled("%s", g_default_tune.extra.c_str());
            ImGui::TreePop();
        }
        ImGui::InputTextMultiline("##extra", g_extra_buf, sizeof g_extra_buf,
                                  ImVec2(-1, fs * 6.0f));
        if (ImGui::Button("Insert recommended")) {
            size_t len = strlen(g_extra_buf);
            if (len && g_extra_buf[len - 1] != '\n' && len + 1 < sizeof g_extra_buf)
                g_extra_buf[len++] = '\n', g_extra_buf[len] = '\0';
            strncat(g_extra_buf, RECOMMENDED_ENV, sizeof g_extra_buf - strlen(g_extra_buf) - 1);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Turnip/Mesa/DXVK perf baseline");

        ImGui::Separator();
        const char *save_label = is_default ? "Save Default" : "Save override";
        if (ImGui::Button(save_label)) {
            g_tune.extra = g_extra_buf;
            bool ok = save_profile(g.appid, g_tune);
            if (ok) steam_select(g_sel);   // reload: refresh override/default state
            g_status = ok ? (is_default ? "Default saved." : "Override saved.")
                          : "Failed to write profile.";
        }
        if (!is_default && g_has_override) {
            ImGui::SameLine();
            if (ImGui::Button("Delete override (use Default)")) {
                delete_profile(g.appid);
                steam_select(g_sel);   // reload: all flags revert to Inherit
                g_status = "Override deleted; game now inherits Default.";
            }
        }
        if (!g_status.empty()) {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", g_status.c_str());
        }
    }
    ImGui::EndChild();
}

int main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("konkr-launcher", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                                    SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) {
        fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    float scale = (w >= 1600) ? 2.5f : 2.0f;
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.FontGlobalScale = scale;

    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    // scan ROMs once at startup (cheap; rescans on demand via the button)
    std::vector<std::vector<Rom>> roms(N_SYSTEMS);
    for (int i = 0; i < N_SYSTEMS; i++)
        roms[i] = scan_roms(SYSTEMS[i]);

    SDL_GameController *gc = nullptr;
    while (!g_quit) {
        Uint32 frame_start = SDL_GetTicks();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT)
                g_quit = true;
        }
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            if (gc)
                SDL_GameControllerClose(gc);
            gc = nullptr;
            for (int i = 0; i < SDL_NumJoysticks(); i++) {
                if (!SDL_IsGameController(i))
                    continue;
                const char *nm = SDL_GameControllerNameForIndex(i);
                if (nm && (strstr(nm, "AYANEO MCU") || strstr(nm, "KONKR System")))
                    continue;
                gc = SDL_GameControllerOpen(i);
                break;
            }
            if (gc)
                ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, &gc, 1);
            else
                ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoFirst);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("konkr-launcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Steam")) {
                draw_steam_tab();
                ImGui::EndTabItem();
            }
            for (int i = 0; i < N_SYSTEMS; i++) {
                if (ImGui::BeginTabItem(SYSTEMS[i].label)) {
                    draw_system_tab(SYSTEMS[i], roms[i]);
                    ImGui::EndTabItem();
                }
            }
            if (ImGui::BeginTabItem("Tools")) {
                ImGui::TextUnformatted("System tools.");
                ImGui::Separator();
                std::vector<Tile> t = {
                    { "Device Controls", [] { launch_tool("/usr/bin/device-controls"); } },
                };
                draw_grid(t);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Rescan ROMs")) {
            for (int i = 0; i < N_SYSTEMS; i++)
                roms[i] = scan_roms(SYSTEMS[i]);
        }

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 16, 20, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);

        // run a launch after the frame is presented, never mid-widget
        if (g_pending) {
            auto act = g_pending;
            g_pending = nullptr;
            act();
        }

        Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 16)
            SDL_Delay(16 - elapsed);
    }

    if (gc)
        SDL_GameControllerClose(gc);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
