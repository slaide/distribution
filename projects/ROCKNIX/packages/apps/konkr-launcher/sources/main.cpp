// SPDX-License-Identifier: GPL-2.0
// konkr-launcher — an SDL2 + Dear ImGui game-launcher frontend.
//
// Selectable as the boot frontend via `system.frontend=custom` (see the
// SM8750 090-ui_service quirk). Layout: a header bar (brand, section tabs,
// clock, battery), a content area, and a footer with controller hints.
// Sections:
//   - Steam: opens Big Picture, plus the per-game FEX tuning editor,
//   - one per emulated system (Game Boy, GBC, GBA, NDS), each listing the
//     ROMs found under /storage/roms/<system>/ as an alphabetical grid,
//   - Tools: Device Controls, EmulationStation, and power actions.
//
// Navigation is handheld-first: LB/RB cycle sections (the header tabs are
// touch-only and invisible to the d-pad), focus lands on the first tile of
// a section, X rescans, A activates, B backs out of popups.
//
// Launching shells out to the same entrypoints EmulationStation uses
// (/usr/bin/runemu.sh, /usr/bin/start_steam.sh), so games run through the
// stock emulator stack. Still intentionally light: no scraped art, no
// metadata, no per-game options — discovery + launch + system status.

#include <SDL.h>
#include <dirent.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// read a small sysfs-style file into buf; false if unreadable
static bool read_small(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r > 0;
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
    const char *label;     // section title
    const char *tab;       // short header-tab label
    const char *exts;      // space-separated, leading-dot extensions
    const char *def_emu;   // fallback emulator if <sys>.emulator is unset
    const char *def_core;  // fallback core if <sys>.core is unset
};

// keep extensions in sync with config/emulators/<sys>.conf SYSTEM_EXTENSION
static const System SYSTEMS[] = {
    { "gb",  "Game Boy",         "GB",  ".gb .gbc .zip .7z", "retroarch", "gambatte" },
    { "gbc", "Game Boy Color",   "GBC", ".gb .gbc .zip .7z", "retroarch", "gambatte" },
    { "gba", "Game Boy Advance", "GBA", ".gba .zip .7z",     "retroarch", "mgba"     },
    { "nds", "Nintendo DS",      "DS",  ".nds .zip .7z",     "retroarch", "melonds"  },
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

// ROM library root; KONKR_ROMS overrides for desktop testing
static const char *roms_root()
{
    const char *e = getenv("KONKR_ROMS");
    return e ? e : "/storage/roms";
}

static std::vector<Rom> scan_roms(const System &s)
{
    std::vector<Rom> v;
    std::string dir = std::string(roms_root()) + "/" + s.key;
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

static void switch_to_es()
{
    // konkr-launcher.service has Restart=always, so exiting isn't enough: the
    // unit must be stopped. systemd-run detaches the sequence from our cgroup
    // (a plain backgrounded child would die with the service); the stop
    // terminates this process, no self-quit needed.
    (void)system("systemd-run --no-block /bin/sh -c "
                 "'systemctl start essway.service; "
                 "systemctl stop konkr-launcher.service'");
}

// --------------------------------------------------------------- theme

static inline ImVec4 rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xff) / 255.f, ((hex >> 8) & 0xff) / 255.f,
                  (hex & 0xff) / 255.f, a);
}

// Slate darks, one amber accent. The accent is reserved for "where you are":
// the nav cursor, the active tab underline, and the charging battery.
static const ImVec4 C_BG        = rgb(0x0E1116);
static const ImVec4 C_PANEL     = rgb(0x151A22);
static const ImVec4 C_TILE      = rgb(0x1B212B);
static const ImVec4 C_TILE_HI   = rgb(0x272F3C);
static const ImVec4 C_TEXT      = rgb(0xE8EAED);
static const ImVec4 C_DIM       = rgb(0x8A93A0);
static const ImVec4 C_ACCENT    = rgb(0xF0A83C);
static const ImVec4 C_ACCENT_BG = rgb(0xF0A83C, 0.16f);
static const ImVec4 C_WARN      = rgb(0xE05A4E);

static float g_ui = 1.0f;                  // display scale (window height / 720)
static ImFont *g_font      = nullptr;      // body (null = ImGui default fallback)
static ImFont *g_font_bold = nullptr;      // brand + section titles
static ImFont *g_font_small = nullptr;     // footer hints

static void apply_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    const float u = g_ui;

    st.WindowPadding    = ImVec2(0, 0);          // header/footer sit flush
    st.FramePadding     = ImVec2(14 * u, 8 * u);
    st.ItemSpacing      = ImVec2(12 * u, 12 * u);
    st.ItemInnerSpacing = ImVec2(9 * u, 6 * u);
    st.ScrollbarSize    = 10 * u;
    st.IndentSpacing    = 24 * u;
    st.WindowRounding   = 0;
    st.ChildRounding    = 10 * u;
    st.FrameRounding    = 8 * u;
    st.PopupRounding    = 12 * u;
    st.GrabRounding     = 6 * u;
    st.ScrollbarRounding = 8 * u;
    st.WindowBorderSize = 0;
    st.ChildBorderSize  = 0;
    st.PopupBorderSize  = 1;
    st.FrameBorderSize  = 0;

    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg]         = C_BG;
    c[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]          = C_PANEL;
    c[ImGuiCol_Border]           = rgb(0x2A3340);
    c[ImGuiCol_Text]             = C_TEXT;
    c[ImGuiCol_TextDisabled]     = C_DIM;
    c[ImGuiCol_Button]           = C_TILE;
    c[ImGuiCol_ButtonHovered]    = C_TILE_HI;
    c[ImGuiCol_ButtonActive]     = rgb(0xF0A83C, 0.35f);
    c[ImGuiCol_FrameBg]          = C_TILE;
    c[ImGuiCol_FrameBgHovered]   = C_TILE_HI;
    c[ImGuiCol_FrameBgActive]    = C_TILE_HI;
    c[ImGuiCol_Header]           = C_ACCENT_BG;
    c[ImGuiCol_HeaderHovered]    = rgb(0xF0A83C, 0.26f);
    c[ImGuiCol_HeaderActive]     = rgb(0xF0A83C, 0.38f);
    c[ImGuiCol_NavCursor]        = C_ACCENT;
    c[ImGuiCol_CheckMark]        = C_ACCENT;
    c[ImGuiCol_SliderGrab]       = C_ACCENT;
    c[ImGuiCol_SliderGrabActive] = C_ACCENT;
    c[ImGuiCol_Separator]        = rgb(0x232A35);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]    = rgb(0x2A3340);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x39445A);
    c[ImGuiCol_ScrollbarGrabActive]  = rgb(0x39445A);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.65f);
}

// --------------------------------------------------------------- status

struct Battery { int cap = -1; bool charging = false; };
static Battery g_batt;

// first system battery under /sys/class/power_supply (type=Battery and not
// scope=Device, which is what peripheral batteries report); "" if none
static std::string find_battery_dir()
{
    std::string found;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return found;
    for (dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        std::string dir = std::string("/sys/class/power_supply/") + e->d_name;
        char buf[32];
        if (!read_small((dir + "/type").c_str(), buf, sizeof buf) ||
            strncmp(buf, "Battery", 7) != 0)
            continue;
        if (read_small((dir + "/scope").c_str(), buf, sizeof buf) &&
            strncmp(buf, "Device", 6) == 0)
            continue;
        found = dir;
        break;
    }
    closedir(d);
    return found;
}

static void battery_poll()
{
    static Uint32 last = 0;
    static std::string dir = find_battery_dir();
    Uint32 now = SDL_GetTicks();
    if (dir.empty() || (last != 0 && now - last < 4000))
        return;
    last = now;
    char buf[32];
    g_batt.cap = read_small((dir + "/capacity").c_str(), buf, sizeof buf)
                     ? atoi(buf) : -1;
    g_batt.charging = false;
    if (read_small((dir + "/status").c_str(), buf, sizeof buf))
        g_batt.charging = !strncmp(buf, "Charging", 8) || !strncmp(buf, "Full", 4);
}

// battery glyph: rounded body + terminal nub + proportional fill (+ bolt when
// charging), drawn at `p`, `h` pixels tall. Returns the drawn width.
static float draw_battery_icon(ImDrawList *dl, ImVec2 p, float h, const Battery &b)
{
    const float u = g_ui;
    const float w = h * 1.85f;
    const float r = 2.5f * u;
    ImU32 frame = ImGui::GetColorU32(C_DIM);
    ImVec4 fillc = b.cap <= 15 ? C_WARN : b.cap <= 30 ? C_ACCENT : rgb(0xB9C2CE);
    if (b.charging)
        fillc = C_ACCENT;

    dl->AddRect(p, ImVec2(p.x + w, p.y + h), frame, r, 0, 1.5f * u);
    dl->AddRectFilled(ImVec2(p.x + w + 1.5f * u, p.y + h * 0.30f),
                      ImVec2(p.x + w + 3.5f * u, p.y + h * 0.70f), frame, 1.0f * u);
    float inset = 3.0f * u;
    float fw = (w - 2 * inset) * (b.cap < 0 ? 0 : b.cap) / 100.0f;
    if (fw > 0)
        dl->AddRectFilled(ImVec2(p.x + inset, p.y + inset),
                          ImVec2(p.x + inset + fw, p.y + h - inset),
                          ImGui::GetColorU32(fillc), 1.5f * u);
    if (b.charging) {
        // small bolt over the body, two triangles
        float cx = p.x + w * 0.5f, cy = p.y + h * 0.5f, s = h * 0.34f;
        ImU32 bolt = ImGui::GetColorU32(rgb(0x0E1116));
        dl->AddTriangleFilled(ImVec2(cx + s * 0.35f, cy - s),
                              ImVec2(cx - s * 0.55f, cy + s * 0.18f),
                              ImVec2(cx + s * 0.10f, cy + s * 0.18f), bolt);
        dl->AddTriangleFilled(ImVec2(cx - s * 0.35f, cy + s),
                              ImVec2(cx + s * 0.55f, cy - s * 0.18f),
                              ImVec2(cx - s * 0.10f, cy - s * 0.18f), bolt);
    }
    return w + 3.5f * u;
}

// --------------------------------------------------------------- sections

// section indices: 0 = Steam, 1..N_SYSTEMS = systems, last = Tools
static const int SEC_STEAM = 0;
static const int SEC_TOOLS = N_SYSTEMS + 1;
static const int N_SEC     = N_SYSTEMS + 2;

static const char *sec_tab_label(int s)
{
    if (s == SEC_STEAM) return "Steam";
    if (s == SEC_TOOLS) return "Tools";
    return SYSTEMS[s - 1].tab;
}

static int g_sec = SEC_STEAM;
static int g_focus_frames = 2;   // >0: force nav focus onto the section's first item
static bool g_modal_open = false;

static void switch_sec(int s)
{
    g_sec = (s % N_SEC + N_SEC) % N_SEC;
    g_focus_frames = 2;
}

// consume-once check used at each section's first focusable widget
static void focus_first_item()
{
    if (g_focus_frames > 0)
        ImGui::SetKeyboardFocusHere();
}

// --------------------------------------------------------------- ui

struct Tile {
    std::string label;
    std::function<void()> action;
    std::string overlay;    // if set, show this launch overlay before running
};

// ---- launch overlay ----
// Blocking launches (system() into steam/runemu) freeze the UI on the last
// presented frame until the target takes the display. Render an announcement
// panel for a few frames first, so that frozen frame clearly says what is
// happening instead of looking like a hang.
static std::string g_overlay_title, g_overlay_sub;
static std::function<void()> g_overlay_action;
static int g_overlay_frames = 0;

static void overlay_launch(const std::string &title, const std::string &sub,
                           std::function<void()> action)
{
    g_overlay_title = title;
    g_overlay_sub = sub;
    g_overlay_action = std::move(action);
    g_overlay_frames = 3;
}

static void draw_overlay()
{
    if (g_overlay_frames <= 0)
        return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float u = g_ui;
    ImVec2 p0 = vp->WorkPos;
    ImVec2 p1 = ImVec2(p0.x + vp->WorkSize.x, p0.y + vp->WorkSize.y);
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 200));

    ImFont *tf = g_font_bold ? g_font_bold : ImGui::GetFont();
    float tfs = tf->FontSize;
    ImVec2 ts = tf->CalcTextSizeA(tfs, FLT_MAX, 0, g_overlay_title.c_str());
    ImVec2 ss = g_overlay_sub.empty() ? ImVec2(0, 0)
                                      : ImGui::CalcTextSize(g_overlay_sub.c_str());
    float pw = std::min(std::max(ts.x, ss.x) + 80 * u, vp->WorkSize.x - 40 * u);
    float ph = ts.y + (ss.y > 0 ? ss.y + 10 * u : 0) + 60 * u;
    ImVec2 c((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    ImVec2 a(c.x - pw / 2, c.y - ph / 2), b(c.x + pw / 2, c.y + ph / 2);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(C_PANEL), 14 * u);
    dl->AddRect(a, b, ImGui::GetColorU32(C_ACCENT), 14 * u, 0, 2 * u);
    float y = a.y + 30 * u;
    dl->AddText(tf, tfs, ImVec2(c.x - ts.x / 2, y), ImGui::GetColorU32(C_TEXT),
                g_overlay_title.c_str());
    if (ss.y > 0)
        dl->AddText(ImVec2(c.x - ss.x / 2, y + ts.y + 10 * u),
                    ImGui::GetColorU32(C_DIM), g_overlay_sub.c_str());
}

static void draw_grid(const std::vector<Tile> &tiles)
{
    const float fs = ImGui::GetFontSize();
    const ImGuiStyle &st = ImGui::GetStyle();
    const float spacing = st.ItemSpacing.x;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (int)((avail + spacing) / (fs * 11.0f + spacing));
    if (cols < 2) cols = 2;
    // stretch cells so rows fill the width edge-to-edge
    ImVec2 cell((avail - spacing * (cols - 1)) / cols, fs * 3.1f);
    const float pad = st.FramePadding.x;

    // marquee phase restarts whenever a different tile takes focus
    static int m_sec = -1, m_idx = -1;
    static double m_t0 = 0.0;

    for (size_t i = 0; i < tiles.size(); i++) {
        if (i % cols)
            ImGui::SameLine();
        ImGui::PushID((int)i);
        if (i == 0)
            focus_first_item();
        if (ImGui::Button("##tile", cell)) {
            if (!tiles[i].overlay.empty())
                overlay_launch(tiles[i].overlay, "", tiles[i].action);
            else
                g_pending = tiles[i].action;
        }
        bool focused = ImGui::IsItemFocused();
        ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const std::string &full = tiles[i].label;
        float inner_w = cell.x - 2 * pad;
        float text_w = ImGui::CalcTextSize(full.c_str()).x;
        float ty = rmin.y + (cell.y - fs) / 2;
        ImU32 tcol = ImGui::GetColorU32(ImGuiCol_Text);

        if (text_w <= inner_w) {
            dl->AddText(ImVec2(rmin.x + (cell.x - text_w) / 2, ty), tcol,
                        full.c_str());
        } else if (focused) {
            // too long + focused: marquee — dwell at each end, scroll between
            if (m_sec != g_sec || m_idx != (int)i) {
                m_sec = g_sec;
                m_idx = (int)i;
                m_t0 = ImGui::GetTime();
            }
            const float over = text_w - inner_w;
            const float speed = 40.0f * g_ui;   // px/s
            const double dwell = 0.8, travel = over / speed;
            double ph = fmod(ImGui::GetTime() - m_t0, 2 * (dwell + travel));
            float off = ph < dwell              ? 0.0f
                      : ph < dwell + travel     ? (float)((ph - dwell) * speed)
                      : ph < 2 * dwell + travel ? over
                      : over - (float)((ph - 2 * dwell - travel) * speed);
            dl->PushClipRect(ImVec2(rmin.x + pad, rmin.y),
                             ImVec2(rmax.x - pad, rmax.y), true);
            dl->AddText(ImVec2(rmin.x + pad - off, ty), tcol, full.c_str());
            dl->PopClipRect();
        } else {
            // too long + idle: static truncation
            std::string lbl = full;
            while (lbl.size() > 1 &&
                   ImGui::CalcTextSize((lbl + "..").c_str()).x > inner_w)
                lbl.pop_back();
            lbl += "..";
            float w = ImGui::CalcTextSize(lbl.c_str()).x;
            dl->AddText(ImVec2(rmin.x + (cell.x - w) / 2, ty), tcol, lbl.c_str());
        }
        ImGui::PopID();
    }
}

static void section_title(const char *title, const char *sub)
{
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(title);
    if (g_font_bold) ImGui::PopFont();
    if (sub) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sub);
    }
    ImGui::Spacing();
}

static void draw_system_tab(const System &s, const std::vector<Rom> &roms)
{
    char sub[48];
    snprintf(sub, sizeof sub, "%zu game%s", roms.size(), roms.size() == 1 ? "" : "s");
    section_title(s.label, sub);

    if (roms.empty()) {
        ImGui::TextDisabled("No games yet.");
        ImGui::TextWrapped("Put %s files in %s/%s and press X to rescan.",
                           s.exts, roms_root(), s.key);
        return;
    }
    std::vector<Tile> tiles;
    tiles.reserve(roms.size());
    for (const Rom &r : roms) {
        std::string path = r.path;
        const System *sp = &s;
        tiles.push_back({ r.name, [sp, path]() { launch_rom(*sp, path); },
                          "Starting " + r.name });
    }
    draw_grid(tiles);
}

// ----------------------------------------------------- Steam section

static std::vector<steamfex::Game> g_games;   // [0] = Default sentinel, rest = games
static bool g_games_scanned = false;
static int g_sel = -1;                  // index into g_games, -1 = none
static steamfex::Tune g_tune;           // profile being edited (default or per-game)
static steamfex::Tune g_default_tune;   // snapshot of default.conf, for inherit hints
static bool g_has_override = false;     // selected game has its own <appid>.conf
static char g_extra_buf[4096] = { 0 };
static std::string g_status;
static int g_focus_editor_frames = 0;   // >0: focus the editor's first widget
static int g_focus_list_frames = 0;     // >0: focus the selected game in the list

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

    section_title("Steam", nullptr);

    focus_first_item();
    ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_BG);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0xF0A83C, 0.28f));
    bool open = ImGui::Button("Open Steam Big Picture", ImVec2(fs * 15.0f, fs * 2.4f));
    ImGui::PopStyleColor(2);
    if (open)
        overlay_launch("Starting Steam",
                       "Hang tight - Big Picture can take a while to appear.",
                       launch_steam);

    ImGui::Spacing();
    ImGui::TextDisabled("Per-game FEX tuning. Steam is closed here, so edits are safe.");
    ImGui::Spacing();

    // left: Default + game list
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::BeginChild("games", ImVec2(fs * 13.0f, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)g_games.size(); i++) {
        ImGui::PushID(i);
        if (g_focus_list_frames > 0 && i == g_sel)
            ImGui::SetKeyboardFocusHere();   // B in the editor returns here
        if (ImGui::Selectable(g_games[i].name.c_str(), i == g_sel)) {
            if (i != g_sel)
                steam_select(i);
            g_focus_editor_frames = 2;       // A on an entry enters the editor
        }
        ImGui::PopID();
        if (i == 0)
            ImGui::Separator();   // divide Default from the installed games
    }
    if (g_games.size() <= 1)
        ImGui::TextWrapped("No installed Steam games found yet.");
    ImGui::EndChild();

    ImGui::SameLine();

    // right: editor
    ImGui::BeginChild("editor", ImVec2(0, 0), ImGuiChildFlags_Borders);
    // B (or Esc) inside the editor hands focus back to the selected game.
    // ImGui consumes the same press as NavCancel during NewFrame and moves
    // focus out of the child BEFORE we run, so test the previous frame's
    // focus state, not the current one. Skipped while a popup owns focus
    // (B closes it) or a widget is being edited (B deactivates it first).
    static bool editor_was_focused = false;
    if (editor_was_focused && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        g_focus_list_frames = 2;
    editor_was_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    // consume-once-per-frame focus grab for the editor's first widget
    bool editor_focus = g_focus_editor_frames > 0;
    auto grab_editor_focus = [&editor_focus]() {
        if (editor_focus) {
            ImGui::SetKeyboardFocusHere();
            editor_focus = false;
        }
    };
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
            grab_editor_focus();
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
        grab_editor_focus();
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
    ImGui::PopStyleColor();
}

// ------------------------------------------------------ Tools section

static const char *g_confirm_label = nullptr;   // non-null => open confirm popup
static const char *g_confirm_cmd = nullptr;

static void draw_tools_tab()
{
    section_title("Tools", nullptr);

    std::vector<Tile> tiles = {
        { "Device Controls",  [] { launch_tool("/usr/bin/device-controls"); },
                              "Opening Device Controls" },
        { "EmulationStation", [] { switch_to_es(); },
                              "Switching to EmulationStation" },
        { "Sleep",            [] { launch_tool("systemctl suspend"); } },
        { "Restart",          [] { g_confirm_label = "Restart";
                                   g_confirm_cmd = "systemctl reboot"; } },
        { "Power off",        [] { g_confirm_label = "Power off";
                                   g_confirm_cmd = "systemctl poweroff"; } },
    };
    draw_grid(tiles);

    if (g_confirm_label)
        ImGui::OpenPopup("##confirm");
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    g_modal_open = false;
    if (ImGui::BeginPopupModal("##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        g_modal_open = true;
        static const char *label = nullptr;
        static const char *cmd = nullptr;
        if (g_confirm_label) {          // just opened: latch and clear the request
            label = g_confirm_label;
            cmd = g_confirm_cmd;
            g_confirm_label = g_confirm_cmd = nullptr;
        }
        const float fs = ImGui::GetFontSize();
        ImGui::Dummy(ImVec2(fs, fs * 0.2f));
        ImGui::Text("%s now?", label);
        ImGui::Spacing();
        if (ImGui::Button(label, ImVec2(fs * 8.0f, fs * 2.2f))) {
            const char *c = cmd;
            g_pending = [c] { launch_tool(c); };
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs * 8.0f, fs * 2.2f)))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();   // land on Cancel; B also backs out
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------ header / footer

static void draw_header(float width, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::BeginChild("##hdr", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float u = g_ui;
    const float pad = 24 * u;
    const ImVec2 org = ImGui::GetWindowPos();

    // the whole bar is touch-clickable but invisible to d-pad navigation
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

    // brand
    ImFont *brand_font = g_font_bold ? g_font_bold : ImGui::GetFont();
    float brand_h = brand_font->FontSize;
    dl->AddText(brand_font, brand_h, ImVec2(org.x + pad, org.y + (height - brand_h) / 2),
                ImGui::GetColorU32(C_ACCENT), "KONKR");
    float x = pad + brand_font->CalcTextSizeA(brand_h, FLT_MAX, 0, "KONKR").x + 30 * u;

    // section tabs
    const float fs = ImGui::GetFontSize();
    for (int i = 0; i < N_SEC; i++) {
        const char *lbl = sec_tab_label(i);
        ImVec2 sz = ImGui::CalcTextSize(lbl);
        ImGui::SetCursorPos(ImVec2(x - 8 * u, 0));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(sz.x + 16 * u, height)))
            switch_sec(i);
        ImGui::PopID();
        bool active = (i == g_sec);
        dl->AddText(ImVec2(org.x + x, org.y + (height - fs) / 2),
                    ImGui::GetColorU32(active ? C_TEXT : C_DIM), lbl);
        if (active)
            dl->AddRectFilled(ImVec2(org.x + x - 2 * u, org.y + height - 4 * u),
                              ImVec2(org.x + x + sz.x + 2 * u, org.y + height),
                              ImGui::GetColorU32(C_ACCENT), 2 * u);
        x += sz.x + 32 * u;
    }
    ImGui::PopItemFlag();

    // right block: clock, then battery icon + percent
    char clock[8] = "";
    time_t t = time(nullptr);
    struct tm tm;
    if (localtime_r(&t, &tm))
        strftime(clock, sizeof clock, "%H:%M", &tm);

    char pct[16] = "";
    if (g_batt.cap >= 0)
        snprintf(pct, sizeof pct, "%d%%", g_batt.cap > 100 ? 100 : g_batt.cap);
    float icon_h = fs * 0.62f;
    float icon_w = g_batt.cap >= 0 ? icon_h * 1.85f + 3.5f * u : 0;
    float clock_w = ImGui::CalcTextSize(clock).x;
    float pct_w = ImGui::CalcTextSize(pct).x;
    float gap = 10 * u;
    float total = clock_w + (g_batt.cap >= 0 ? 2 * gap + icon_w + gap * 0.6f + pct_w : 0);

    float rx = org.x + width - pad - total;
    float cy = org.y + height / 2;
    dl->AddText(ImVec2(rx, cy - fs / 2), ImGui::GetColorU32(C_TEXT), clock);
    if (g_batt.cap >= 0) {
        rx += clock_w + 2 * gap;
        draw_battery_icon(dl, ImVec2(rx, cy - icon_h / 2), icon_h, g_batt);
        rx += icon_w + gap * 0.6f;
        dl->AddText(ImVec2(rx, cy - fs / 2), ImGui::GetColorU32(C_DIM), pct);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// one "chip + label" controller hint; advances the cursor
static void hint(const char *btn, const char *label)
{
    const float u = g_ui;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(btn);
    ImVec2 ls = ImGui::CalcTextSize(label);
    float h = ts.y + 5 * u;
    float w = std::max(h, ts.x + 12 * u);     // letters get a circle-ish pill
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(C_TILE_HI), h / 2);
    dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + 2.5f * u),
                ImGui::GetColorU32(C_TEXT), btn);
    dl->AddText(ImVec2(p.x + w + 7 * u, p.y + (h - ls.y) / 2),
                ImGui::GetColorU32(C_DIM), label);
    ImGui::Dummy(ImVec2(w + 7 * u + ls.x, h));
    ImGui::SameLine(0, 22 * u);
}

static void draw_footer(float width, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::BeginChild("##ftr", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (g_font_small) ImGui::PushFont(g_font_small);

    const float u = g_ui;
    float line_h = ImGui::GetTextLineHeight() + 5 * u;
    ImGui::SetCursorPos(ImVec2(24 * u, (height - line_h) / 2));

    hint("LB/RB", "Section");
    hint("A", g_sec == SEC_STEAM || g_sec == SEC_TOOLS ? "Select" : "Launch");
    if (g_sec != SEC_TOOLS)
        hint("X", "Rescan");
    if (g_modal_open)
        hint("B", "Cancel");
    ImGui::NewLine();

    if (g_font_small) ImGui::PopFont();
    ImGui::PopItemFlag();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// --------------------------------------------------------------- main

int main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // KONKR_SHOT runs windowed at a fixed size so offscreen captures are
    // deterministic (fullscreen-desktop geometry depends on the video driver)
    SDL_Window *win = SDL_CreateWindow("konkr-launcher", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                                       getenv("KONKR_SHOT")
                                           ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
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

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    g_ui = h / 720.0f;
    if (g_ui < 1.0f) g_ui = 1.0f;

    // real vector fonts at native pixel size (the scaled bitmap default is the
    // single biggest "prototype look" offender); fall back to it if missing
    const char *FONT      = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
    const char *FONT_BOLD = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf";
    if (access(FONT, F_OK) == 0) {
        g_font = io.Fonts->AddFontFromFileTTF(FONT, roundf(21.0f * g_ui));
        g_font_small = io.Fonts->AddFontFromFileTTF(FONT, roundf(17.0f * g_ui));
    }
    if (access(FONT_BOLD, F_OK) == 0)
        g_font_bold = io.Fonts->AddFontFromFileTTF(FONT_BOLD, roundf(25.0f * g_ui));
    if (!g_font) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.6f * g_ui;
    }
    apply_style();

    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    // scan ROMs once at startup (cheap; rescans on demand via X)
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
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && !g_modal_open) {
                switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                    switch_sec(g_sec - 1);
                    break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    switch_sec(g_sec + 1);
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    if (g_sec == SEC_STEAM) {
                        steam_rescan();
                        steam_select(0);
                    } else if (g_sec != SEC_TOOLS) {
                        roms[g_sec - 1] = scan_roms(SYSTEMS[g_sec - 1]);
                    }
                    break;
                }
            }
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

        battery_poll();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("konkr-launcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const float hdr_h = 56 * g_ui;
        const float ftr_h = 44 * g_ui;
        draw_header(vp->WorkSize.x, hdr_h);

        ImGui::SetCursorPos(ImVec2(0, hdr_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(24 * g_ui, 18 * g_ui));
        ImGui::BeginChild("##content", ImVec2(0, vp->WorkSize.y - hdr_h - ftr_h),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        if (g_sec == SEC_STEAM)
            draw_steam_tab();
        else if (g_sec == SEC_TOOLS)
            draw_tools_tab();
        else
            draw_system_tab(SYSTEMS[g_sec - 1], roms[g_sec - 1]);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SetCursorPos(ImVec2(0, vp->WorkSize.y - ftr_h));
        draw_footer(vp->WorkSize.x, ftr_h);
        draw_overlay();

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, (Uint8)(C_BG.x * 255), (Uint8)(C_BG.y * 255),
                               (Uint8)(C_BG.z * 255), 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);

        // dev affordance: KONKR_SHOT=/path.bmp dumps a frame and exits
        // (KONKR_SHOT_SEC picks the section, KONKR_SHOT_FRAME the frame
        // [default 30]; pair with SDL_VIDEODRIVER=offscreen)
        if (const char *shot = getenv("KONKR_SHOT")) {
            static int frames = 0;
            if (const char *s = getenv("KONKR_SHOT_SEC"); s && frames == 0)
                switch_sec(atoi(s));
            if (getenv("KONKR_SHOT_OVERLAY") && g_overlay_frames == 0) {
                // keep the launch overlay armed (null action = never launches)
                g_overlay_title = "Starting Steam";
                g_overlay_sub = "Hang tight - Big Picture can take a while to appear.";
                g_overlay_frames = 2;
            }
            const char *fenv = getenv("KONKR_SHOT_FRAME");
            if (++frames == (fenv ? atoi(fenv) : 30)) {
                int rw = 0, rh = 0;
                SDL_GetRendererOutputSize(ren, &rw, &rh);
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(
                    0, rw, rh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (sf && SDL_RenderReadPixels(ren, nullptr, sf->format->format,
                                               sf->pixels, sf->pitch) == 0)
                    SDL_SaveBMP(sf, shot);
                if (sf)
                    SDL_FreeSurface(sf);
                g_quit = true;
            }
        }

        SDL_RenderPresent(ren);

        if (g_focus_frames > 0)
            g_focus_frames--;
        if (g_focus_editor_frames > 0)
            g_focus_editor_frames--;
        if (g_focus_list_frames > 0)
            g_focus_list_frames--;

        // launch once the overlay has been on screen for its full count, so
        // the frame frozen during the blocking launch is the announcement
        if (g_overlay_frames > 0 && --g_overlay_frames == 0) {
            g_pending = g_overlay_action;
            g_overlay_action = nullptr;
        }

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
