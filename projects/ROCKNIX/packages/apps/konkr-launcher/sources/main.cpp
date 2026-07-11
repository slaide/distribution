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
// The steamfex logic + the shared per-game editor UI, both also used by
// device-controls. Cross-package include (see this package's package.mk -I).
#include "steamfex_ui.h"

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
// (per-game tune/override/status state now lives in steamfex::draw_game_editor)
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
    g_sel = i;   // the shared editor loads/saves its own state keyed on the game
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
    if (g_sel < 0 || g_sel >= (int)g_games.size()) {
        ImGui::TextWrapped("Pick 'Default (all games)' to set the baseline every tuned "
                           "game inherits, or a game to override it.");
    } else {
        if (editor_focus)
            ImGui::SetKeyboardFocusHere();  // A on a game -> focus editor's first widget
        steamfex::draw_game_editor(g_games[g_sel]);
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
