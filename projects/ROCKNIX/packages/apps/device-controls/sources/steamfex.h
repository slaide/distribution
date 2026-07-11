// SPDX-License-Identifier: GPL-2.0
// Steam per-game FEX tuning — profile + config.vdf logic.
//
// Self-contained (no ImGui / SDL): reads and writes the same on-disk contract
// the konkr-launcher Steam tab uses — per-game env files under
//   /storage/.config/fex-emu/per-game/<appid>.conf
// consumed by the "KONKR FEX-Tuned" compat tool, plus the game's
// CompatToolMapping in Steam's config.vdf. This is a verbatim copy of
// konkr-launcher/sources/main.cpp's steamfex namespace so device-controls can
// edit the same profiles from its Steam tab / sidebar (the file format is the
// shared contract; both apps are independent readers/writers of it).
//
// NOTE: unlike the launcher (which only runs while Steam is closed), the
// device-controls sidebar can edit these live, in-game. Profile .conf edits are
// always safe and take effect on the game's next launch; the config.vdf mapping
// toggle is Steam's file and may need a Steam restart to register.
#pragma once

#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>

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

static std::string run_cmd(const char *cmd)
{
    std::string out;
    FILE *p = popen(cmd, "r");
    if (!p) return out;
    char line[256];
    while (fgets(line, sizeof line, p))
        out += line;
    pclose(p);
    return out;
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

// ---- stranded-save recovery (Proton "* BACKUP" folders) ----
// When Proton reconfigures a prefix on a Proton-version change it renames the
// game's Windows user-profile save dir to "<name> BACKUP" and leaves an empty
// redirect (game shows "New Game"); Steam's remotecache still marks it synced,
// so Cloud never re-downloads. Detect the BACKUP dirs and no-clobber-restore.

struct SaveBackup {
    std::string backup;   // ".../My Documents BACKUP"
    std::string live;     // ".../My Documents"
    std::string label;    // "My Documents"
    int files = 0;
};

static std::string prefix_userdir(int appid)
{
    const char *libs[] = {
        "/storage/.local/share/Steam/steamapps",
        "/storage/roms/steam/steamapps",
    };
    char sub[128];
    snprintf(sub, sizeof sub, "/compatdata/%d/pfx/drive_c/users/steamuser", appid);
    for (const char *lib : libs) {
        std::string p = std::string(lib) + sub;
        if (access(p.c_str(), F_OK) == 0)
            return p;
    }
    return "";
}

static std::vector<SaveBackup> scan_backups(int appid)
{
    std::vector<SaveBackup> out;
    std::string base = prefix_userdir(appid);
    if (base.empty()) return out;
    DIR *d = opendir(base.c_str());
    if (!d) return out;
    const std::string suf = " BACKUP";
    for (dirent *e; (e = readdir(d));) {
        std::string n = e->d_name;
        if (n.size() <= suf.size() ||
            n.compare(n.size() - suf.size(), suf.size(), suf) != 0)
            continue;
        SaveBackup b;
        b.backup = base + "/" + n;
        b.label  = n.substr(0, n.size() - suf.size());
        b.live   = base + "/" + b.label;
        std::string cnt = run_cmd(("find '" + b.backup + "' -type f 2>/dev/null | wc -l").c_str());
        b.files = atoi(cnt.c_str());
        if (b.files > 0) out.push_back(b);
    }
    closedir(d);
    return out;
}

// Fill files missing from the live dir, never overwrite (a newer post-rename
// save is preserved). Handles subdirs and spaces; follows the My Documents ->
// Documents symlink into the real target.
static void restore_backup(const SaveBackup &b)
{
    std::string c =
        "B='" + b.backup + "'; L='" + b.live + "'; "
        "cd \"$B\" 2>/dev/null && find . -type f | while IFS= read -r f; do "
        "d=\"$L/$f\"; [ -e \"$d\" ] || { mkdir -p \"$(dirname \"$d\")\"; cp -a \"$f\" \"$d\"; }; "
        "done";
    run_cmd(c.c_str());
}

static void remove_backup(const SaveBackup &b)
{
    run_cmd(("rm -rf '" + b.backup + "'").c_str());
}

} // namespace steamfex
