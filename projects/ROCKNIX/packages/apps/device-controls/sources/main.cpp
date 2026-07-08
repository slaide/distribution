// SPDX-License-Identifier: GPL-2.0
// device-controls — on-device control app for the KONKR Pocket FIT Elite.
//
// SDL2 + Dear ImGui (SDL_Renderer backend), gamepad + touch navigation,
// launched from EmulationStation as a module. Fronts:
//   - gamepad tester (SDL_GameController state) + rumble test
//   - grip/shoulder-2 remap (konkr_sysbtn sysfs key_* attrs)
//   - analog-stick RGB (leds multicolor sysfs)
//   - audio volume (wpctl)
//   - fan (pwm-fan hwmon sysfs)
//   - power profiles (CPU/GPU max-freq caps via power_profile quirk script)
//   - system info (battery / temps / kernel)
//   - device info (model / SoC / RAM / storage media + what ROCKNIX runs from)
//
// `device-controls --restore` applies the saved grip + RGB config and exits
// (run by device-controls-restore.service at boot: the MCU key-configs are
// VOLATILE across MCU power loss, so saved mappings must be re-applied).

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
#include <algorithm>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "theme.h"

using json = nlohmann::json;

// Shared UI state declared extern in theme.h; defined here (this is the primary
// translation unit). sidebar.cpp reuses these — only one entry point runs per
// process, so each loads its own fonts into its own ImGui atlas.
float   g_ui        = 1.0f;
ImFont *g_font      = nullptr;
ImFont *g_font_bold = nullptr;
ImFont *g_font_small = nullptr;

static const char *KEY_DIR = "/sys/bus/serial/devices/serial1-0";
static const char *LED_DIR = "/sys/class/leds/konkr:rgb:joysticks";
static const char *CONF_PATH = "/storage/.config/device-controls.conf";

// ---------- small file/cmd helpers ----------

static bool read_file(const std::string &path, char *buf, size_t n)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return true;
}

static bool write_file(const std::string &path, const std::string &val)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return false;
    bool ok = fputs(val.c_str(), f) >= 0;
    fclose(f);
    return ok;
}

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
    return out;
}

// ---------- grip remap (konkr_sysbtn sysfs) ----------

struct KeySlot {
    const char *attr;   // sysfs attr name
    const char *label;  // UI label
    int func = -1;      // funcbyte as read (255 = factory)
    int target = -1;
    bool present = false;
};

static KeySlot g_keys[4] = {
    { "key_lc_back", "Back grip LEFT" },
    { "key_rc_back", "Back grip RIGHT" },
    { "key_lc_shoulder", "Shoulder-2 LEFT" },
    { "key_rc_shoulder", "Shoulder-2 RIGHT" },
};

// JoystickFunctionType target codes (HARDWARE.md §C-bin)
static const char *TARGET_NAMES[] = {
    "(unmapped)", "A", "B", "X", "Y", "Select", "Start",
    "DPad Up", "DPad Down", "DPad Left", "DPad Right",
    "LB", "RB", "L3", "R3", "LT", "RT",
    "LStick Up", "LStick Down", "LStick Left", "LStick Right",
    "RStick Up", "RStick Down", "RStick Left", "RStick Right",
};
static const int N_TARGETS = (int)(sizeof(TARGET_NAMES) / sizeof(*TARGET_NAMES));

static void key_read(KeySlot &k)
{
    char buf[64];
    k.present = read_file(std::string(KEY_DIR) + "/" + k.attr, buf, sizeof buf);
    if (k.present && sscanf(buf, "%d %d", &k.func, &k.target) != 2)
        k.present = false;
}

static bool key_write(const KeySlot &k, int func, int target)
{
    char v[32];
    snprintf(v, sizeof v, "%d %d", func, target);
    return write_file(std::string(KEY_DIR) + "/" + k.attr, v);
}

// ---------- system-button remap (konkr-inputd IPC) ----------
//
// The 5 UART system buttons + the SPI gamepad's front-top-left button are
// remapped live by the konkr-inputd daemon, which owns the physical evdevs.
// We talk to it over its Unix socket: read current bindings, push changes.
// No more YAML regeneration / bind-mount / service restart — changes are
// instant.

static const char *KONKR_SOCK = "/run/konkr-inputd.sock";

static int ipc_connect()
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, KONKR_SOCK, sizeof a.sun_path - 1);
    if (connect(fd, (sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Send one command, return the (single-message) reply.
static std::string ipc_cmd(const std::string &c)
{
    int fd = ipc_connect();
    if (fd < 0)
        return "";
    send(fd, c.c_str(), c.size(), MSG_NOSIGNAL);
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    std::string r;
    if (n > 0) { buf[n] = 0; r = buf; }
    close(fd);
    return r;
}

// GET → the daemon's current bindings, one "SET ..." line per message until END.
static std::vector<std::string> ipc_get_bindings()
{
    std::vector<std::string> out;
    int fd = ipc_connect();
    if (fd < 0)
        return out;
    send(fd, "GET", 3, MSG_NOSIGNAL);
    for (;;) {
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0)
            break;
        buf[n] = 0;
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (s == "END")
            break;
        out.push_back(s);
    }
    close(fd);
    return out;
}

// A selectable action for a system button. kind matches the daemon's action
// grammar; code is the evdev BTN_*/KEY_* value (ignored for none/sidebar).
struct SysTarget {
    const char *label;
    const char *kind;
    int code;
};
static const SysTarget SYS_TARGETS[] = {
    { "None",         "none",    0 },
    { "Guide",        "gamepad", BTN_MODE },
    { "A",            "gamepad", BTN_SOUTH },
    { "B",            "gamepad", BTN_EAST },
    { "X",            "gamepad", BTN_NORTH },
    { "Y",            "gamepad", BTN_WEST },
    { "L1",           "gamepad", BTN_TL },
    { "R1",           "gamepad", BTN_TR },
    { "L3",           "gamepad", BTN_THUMBL },
    { "R3",           "gamepad", BTN_THUMBR },
    { "Start",        "gamepad", BTN_START },
    { "Select",       "gamepad", BTN_SELECT },
    { "Key F11",      "key",     KEY_F11 },
    { "Key F12",      "key",     KEY_F12 },
    { "Key F13",      "key",     KEY_F13 },
    { "Key F14",      "key",     KEY_F14 },
    { "Key F15",      "key",     KEY_F15 },
};
static const int N_SYS_TARGETS = (int)(sizeof(SYS_TARGETS) / sizeof(*SYS_TARGETS));
static const char *SYS_TARGET_LABELS[N_SYS_TARGETS];

// Maps a daemon SemanticInput name to its UI row. KONKR's long-press is the
// reserved sidebar opener (not editable here); only its short-press is set.
struct SysBtn {
    const char *label;
    const char *si;   // konkr-inputd SemanticInput name
    int sel = 0;      // index into SYS_TARGETS
};
static SysBtn g_sysbtns[6] = {
    { "KONKR (home)",       "KONKR",    0 },
    { "Front-bottom-right", "SYS_BR",   0 },
    { "Front-top-left",     "FRONT_TL", 0 },
    { "Shoulder-2 LEFT",    "SYS3",     0 },
    { "Shoulder-2 RIGHT",   "SYS4",     0 },
    { "Front-top-right",    "SYS5",     0 },
};

// Sidebar opener state, mirrored from the daemon's OPENER line: which g_sysbtns
// row opens the sidebar, and whether on press or hold.
static int  g_opener_idx = 0;
static bool g_opener_hold = true;
static const char *g_sysbtn_labels[6];

static void opener_load()
{
    for (const std::string &l : ipc_get_bindings()) {
        char si[32], trig[16];
        if (sscanf(l.c_str(), "OPENER %31s %15s", si, trig) == 2) {
            for (int i = 0; i < 6; i++)
                if (!strcmp(g_sysbtns[i].si, si))
                    g_opener_idx = i;
            g_opener_hold = !strcmp(trig, "hold");
        }
    }
}
static void opener_send()
{
    std::string cmd = std::string("SET_OPENER ") + g_sysbtns[g_opener_idx].si +
                      (g_opener_hold ? " hold" : " press");
    ipc_cmd(cmd);
}

static int sys_target_index(const char *kind, int code)
{
    for (int i = 0; i < N_SYS_TARGETS; i++) {
        const SysTarget &t = SYS_TARGETS[i];
        if (strcmp(t.kind, kind) != 0)
            continue;
        if (!strcmp(kind, "none") || !strcmp(kind, "sidebar") || t.code == code)
            return i;
    }
    return 0;
}

// Pull current bindings from the daemon into g_sysbtns selections.
static void sysbtn_load()
{
    for (auto &sb : g_sysbtns)
        sb.sel = 0;
    for (const std::string &l : ipc_get_bindings()) {
        char si[32], sub[16], kind[16];
        int code = 0;
        int m = sscanf(l.c_str(), "SET %31s %15s %15s %d", si, sub, kind, &code);
        if (m < 3 || strcmp(sub, "press") != 0)
            continue;
        for (auto &sb : g_sysbtns)
            if (!strcmp(sb.si, si))
                sb.sel = sys_target_index(kind, code);
    }
}

// Push one row's selection to the daemon (live, persisted by the daemon).
static void sysbtn_send(const SysBtn &sb)
{
    const SysTarget &t = SYS_TARGETS[sb.sel];
    std::string cmd = std::string("SET ") + sb.si + " press ";
    if (!strcmp(t.kind, "none") || !strcmp(t.kind, "sidebar"))
        cmd += t.kind;
    else
        cmd += std::string(t.kind) + " " + std::to_string(t.code);
    ipc_cmd(cmd);
}

// ---------- config (saved grips + RGB + sysbtn targets), key=value ----------

struct Config {
    // -1 = not saved
    int key_func[4] = { -1, -1, -1, -1 };
    int key_target[4] = { -1, -1, -1, -1 };
    int rgb[3] = { -1, -1, -1 };
    int brightness = -1;
};

static Config g_conf;

static void conf_load(Config &c)
{
    char buf[1024];
    if (!read_file(CONF_PATH, buf, sizeof buf))
        return;
    char *save = nullptr;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(nullptr, "\n", &save)) {
        int a, b;
        for (int i = 0; i < 4; i++) {
            char pat[48];
            snprintf(pat, sizeof pat, "%s=%%d %%d", g_keys[i].attr);
            if (sscanf(l, pat, &a, &b) == 2) {
                c.key_func[i] = a;
                c.key_target[i] = b;
            }
        }
        sscanf(l, "rgb=%d %d %d", &c.rgb[0], &c.rgb[1], &c.rgb[2]);
        sscanf(l, "brightness=%d", &c.brightness);
    }
    // System-button bindings live in the daemon now, not this config.
}

static bool conf_save(const Config &c)
{
    std::string out;
    char line[96];
    for (int i = 0; i < 4; i++) {
        if (c.key_func[i] >= 0) {
            snprintf(line, sizeof line, "%s=%d %d\n", g_keys[i].attr,
                     c.key_func[i], c.key_target[i]);
            out += line;
        }
    }
    if (c.rgb[0] >= 0) {
        snprintf(line, sizeof line, "rgb=%d %d %d\n", c.rgb[0], c.rgb[1], c.rgb[2]);
        out += line;
        snprintf(line, sizeof line, "brightness=%d\n", c.brightness);
        out += line;
    }
    return write_file(CONF_PATH, out);
}

static void rgb_apply(int r, int g, int b, int brightness)
{
    char v[32];
    snprintf(v, sizeof v, "%d %d %d", r, g, b);
    write_file(std::string(LED_DIR) + "/multi_intensity", v);
    snprintf(v, sizeof v, "%d", brightness);
    write_file(std::string(LED_DIR) + "/brightness", v);
}

static int restore_mode()
{
    conf_load(g_conf);
    for (int i = 0; i < 4; i++)
        if (g_conf.key_func[i] >= 0)
            key_write(g_keys[i], g_conf.key_func[i], g_conf.key_target[i]);
    if (g_conf.rgb[0] >= 0)
        rgb_apply(g_conf.rgb[0], g_conf.rgb[1], g_conf.rgb[2], g_conf.brightness);
    return 0;
}

// ---------- fan (pwm-fan hwmon) ----------

static std::string find_pwm()
{
    DIR *d = opendir("/sys/class/hwmon");
    if (!d)
        return "";
    std::string found;
    for (dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        std::string base = std::string("/sys/class/hwmon/") + e->d_name;
        char name[64] = {};
        if (!read_file(base + "/name", name, sizeof name))
            continue;
        if (strstr(name, "fan")) {
            char tmp[16];
            if (read_file(base + "/pwm1", tmp, sizeof tmp)) {
                found = base + "/pwm1";
                break;
            }
        }
    }
    closedir(d);
    return found;
}

// ---------- fan curve (fancontrol daemon integration) ----------
//
// ROCKNIX runs /usr/bin/fancontrol (quirks/platforms/SM8750/bin/fancontrol)
// as a systemd service: every 3s it averages the cpu/gpu thermal zones and
// applies an interpolated curve selected by the "cooling.profile" setting. The
// "custom" profile sources /storage/.config/fancontrol.conf, so this tab
// edits that file; boot persistence comes from the daemon, not this app.

static const char *FAN_CONF = "/storage/.config/fancontrol.conf";

struct FanCurve {
    // speed[i] (pwm 0-255) applies above temp_c[i]; thresholds strictly
    // descending, [7] pinned at 0 as the daemon's catch-all
    int temp_c[8];
    int speed[8];
};

// keep in sync with quirks/platforms/SM8750/bin/fancontrol
static const FanCurve FAN_QUIET      = {{90, 85, 80, 75, 70, 65, 55, 0}, {255, 204, 153, 128, 102, 77, 51, 0}};
static const FanCurve FAN_MODERATE   = {{85, 80, 75, 70, 65, 60, 50, 0}, {255, 204, 153, 128, 102, 77, 51, 0}};
static const FanCurve FAN_AGGRESSIVE = {{80, 75, 70, 65, 60, 55, 45, 0}, {255, 204, 153, 128, 102, 77, 51, 0}};

static const char *FAN_PROFILES[] = { "quiet", "moderate", "aggressive", "custom" };

bool g_fan_manual = false;	// manual pwm test active (fancontrol stopped)

static bool fan_parse_array(const char *line, const char *key, int *out, int n, int div)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, key, strlen(key)))
        return false;
    const char *p = line + strlen(key);
    for (int i = 0; i < n; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p)
            return false;
        out[i] = (int)(v / div);
        p = end;
    }
    return true;
}

static FanCurve fan_curve_load()
{
    FanCurve c = FAN_MODERATE;
    char buf[4096];
    if (!read_file(FAN_CONF, buf, sizeof buf))
        return c;
    int sp[8], tc[8];
    bool got_s = false, got_t = false;
    char *save = nullptr;
    // line-wise: the shipped conf template quotes example arrays in comments
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(nullptr, "\n", &save)) {
        const char *t = l;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '#')
            continue;
        got_s |= fan_parse_array(t, "SPEEDS=(", sp, 8, 1);
        got_t |= fan_parse_array(t, "TEMPS=(", tc, 8, 1000);
    }
    if (got_s && got_t) {
        memcpy(c.speed, sp, sizeof sp);
        memcpy(c.temp_c, tc, sizeof tc);
    }
    return c;
}

static bool fan_curve_save(const FanCurve &c)
{
    std::string out = "# written by device-controls (Fan tab)\nDEBUG=false\nSPEEDS=(";
    char num[16];
    for (int i = 0; i < 8; i++) {
        snprintf(num, sizeof num, "%s%d", i ? " " : "", c.speed[i]);
        out += num;
    }
    out += ")\nTEMPS=(";
    for (int i = 0; i < 8; i++) {
        snprintf(num, sizeof num, "%s%d", i ? " " : "", c.temp_c[i] * 1000);
        out += num;
    }
    out += ")\n";
    return write_file(FAN_CONF, out);
}

static int fan_profile_get()
{
    // get_setting is a /etc/profile shell function, not a binary
    std::string out = run_cmd(". /etc/profile 2>/dev/null; get_setting cooling.profile 2>/dev/null");
    for (int i = 0; i < 4; i++)
        if (!strncmp(out.c_str(), FAN_PROFILES[i], strlen(FAN_PROFILES[i])))
            return i;
    return 1;	// unset: the daemon falls back to moderate
}

static void fan_profile_set(int idx)
{
    char cmd[160];
    snprintf(cmd, sizeof cmd,
             ". /etc/profile 2>/dev/null; set_setting cooling.profile %s; systemctl restart fancontrol",
             FAN_PROFILES[idx]);
    run_cmd(cmd);
}

static double fan_temp_now()
{
    // same zone set the fancontrol quirk averages
    static std::vector<std::string> zones;
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
        DIR *d = opendir("/sys/devices/virtual/thermal");
        for (dirent *e; d && (e = readdir(d));) {
            if (strncmp(e->d_name, "thermal_zone", 12))
                continue;
            std::string base = std::string("/sys/devices/virtual/thermal/") + e->d_name;
            char type[64] = {};
            if (!read_file(base + "/type", type, sizeof type))
                continue;
            if (!strncmp(type, "cpu-", 4) || !strncmp(type, "cpuss-", 6) ||
                !strncmp(type, "gpuss", 5))
                zones.push_back(base + "/temp");
        }
        if (d)
            closedir(d);
    }
    double sum = 0;
    int n = 0;
    char buf[24];
    for (auto &z : zones)
        if (read_file(z, buf, sizeof buf)) {
            sum += atof(buf);
            n++;
        }
    return n ? sum / n / 1000.0 : 0.0;
}

static int fan_curve_eval(const FanCurve &c, double t)
{
    // mirror the daemon: piecewise-linear between curve points, flat past the
    // hottest point; a zero-speed point still means "off below here"
    if (t > c.temp_c[0])
        return c.speed[0];
    for (int i = 1; i < 8; i++) {
        if (t > c.temp_c[i]) {
            int s_lo = c.speed[i], t_lo = c.temp_c[i];
            int s_hi = c.speed[i - 1], t_hi = c.temp_c[i - 1];
            if (s_lo == 0)
                return 0;
            if (t_hi <= t_lo)
                return s_hi;
            return (int)(s_lo + (s_hi - s_lo) * (t - t_lo) / (t_hi - t_lo));
        }
    }
    return c.speed[7];
}

// interpolated-curve plot with touch/mouse-draggable control points; returns
// true if the curve changed. sel = point highlighted / edited by the sliders.
static bool fan_curve_plot(FanCurve &c, bool editable, int &sel,
                           double temp_now, int pwm_now)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size(avail.x > 100.0f ? avail.x : 100.0f, 180.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("fanplot", size);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);

    auto X = [&](double t) { return (float)(p0.x + t / 100.0 * size.x); };
    auto Y = [&](double s) { return (float)(p1.y - s / 255.0 * size.y); };

    const ImU32 col_grid = ImGui::GetColorU32(C_TILE);
    const ImU32 col_lbl  = ImGui::GetColorU32(C_DIM);
    dl->AddRectFilled(p0, p1, col_grid);
    dl->AddRect(p0, p1, ImGui::GetColorU32(dc_rgb(0x2A3340)), 0, 0, 1.0f);
    char lab[16];
    for (int t = 10; t <= 100; t += 10) {
        dl->AddLine(ImVec2(X(t), p0.y), ImVec2(X(t), p1.y),
                    IM_COL32(255, 255, 255, t % 20 ? 10 : 25));
        if (t % 20 == 0) {
            snprintf(lab, sizeof lab, "%dC", t);
            dl->AddText(ImVec2(X(t) + 2, p1.y - 16), col_lbl, lab);
        }
    }
    for (int s = 51; s <= 255; s += 51) {	// 20% steps
        dl->AddLine(ImVec2(p0.x, Y(s)), ImVec2(p1.x, Y(s)), IM_COL32(255, 255, 255, 18));
        snprintf(lab, sizeof lab, "%d%%", s * 100 / 255);
        dl->AddText(ImVec2(p0.x + 2, Y(s) + 1), col_lbl, lab);
    }

    // the curve, drawn exactly as the daemon evaluates it: linear segments
    // between points; a zero-speed floor stays off up to the point above it.
    // Cyan while editable (draggable data), dim grey when read-only.
    ImU32 ccol = editable ? ImGui::GetColorU32(C_INFO) : ImGui::GetColorU32(C_DIM);
    float prev_x, prev_y;
    int first = 6;
    if (c.speed[7] == 0) {
        dl->AddLine(ImVec2(X(0), Y(0)), ImVec2(X(c.temp_c[6]), Y(0)), ccol, 2.0f);
        dl->AddLine(ImVec2(X(c.temp_c[6]), Y(0)),
                    ImVec2(X(c.temp_c[6]), Y(c.speed[6])), ccol, 2.0f);
        prev_x = X(c.temp_c[6]);
        prev_y = Y(c.speed[6]);
        first = 5;
    } else {
        prev_x = X(0);
        prev_y = Y(c.speed[7]);
    }
    for (int i = first; i >= 0; i--) {
        float x = X(c.temp_c[i]);
        float y = Y(c.speed[i]);
        dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(x, y), ccol, 2.0f);
        prev_x = x;
        prev_y = y;
    }
    dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(p1.x, prev_y), ccol, 2.0f);

    // live markers: current temp line, actual pwm (filled), curve-commanded
    // pwm at that temp (hollow) — they separate while the daemon lags
    if (temp_now > 0.0) {
        float x = X(temp_now > 100.0 ? 100.0 : temp_now);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
                    ImGui::GetColorU32(dc_rgb(0xF0A83C, 0.63f)), 1.5f);
        dl->AddCircle(ImVec2(x, Y(fan_curve_eval(c, temp_now))), 6.0f,
                      ImGui::GetColorU32(dc_rgb(0xF0A83C, 0.78f)), 0, 1.5f);
        dl->AddCircleFilled(ImVec2(x, Y(pwm_now)), 4.0f, ImGui::GetColorU32(C_ACCENT));
    }

    bool changed = false;
    static int drag = -1;
    ImVec2 m = ImGui::GetIO().MousePos;
    if (editable) {
        if (ImGui::IsItemActivated()) {
            drag = -1;
            float best = 22.0f * 22.0f;
            for (int i = 0; i < 8; i++) {
                float dx = m.x - X(c.temp_c[i]), dy = m.y - Y(c.speed[i]);
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    drag = i;
                }
            }
            if (drag >= 0)
                sel = drag;
        }
        if (ImGui::IsItemActive() && drag >= 0) {
            int t = (int)((m.x - p0.x) / size.x * 100.0 + 0.5);
            int s = (int)((p1.y - m.y) / size.y * 255.0 + 0.5);
            s = s < 0 ? 0 : s > 255 ? 255 : s;
            if (drag < 7) {	// [7] temp pinned at 0
                int lo = c.temp_c[drag + 1] + 1;
                int hi = drag ? c.temp_c[drag - 1] - 1 : 100;
                t = t < lo ? lo : t > hi ? hi : t;
                if (t != c.temp_c[drag]) {
                    c.temp_c[drag] = t;
                    changed = true;
                }
            }
            if (s != c.speed[drag]) {
                c.speed[drag] = s;
                changed = true;
            }
        }
        if (ImGui::IsItemDeactivated())
            drag = -1;
    } else {
        drag = -1;
    }

    for (int i = 0; i < 8; i++) {
        ImVec2 pt(X(c.temp_c[i]), Y(c.speed[i]));
        if (editable && i == sel)
            dl->AddCircle(pt, 8.0f, IM_COL32(255, 255, 255, 220), 0, 1.5f);
        dl->AddCircleFilled(pt, 5.0f, i == drag ? IM_COL32(255, 255, 255, 255) : ccol);
    }
    return changed;
}

// ---------- UI tabs ----------

char g_last_key[32] = "";	// most recent SDL_KEYDOWN, for the tester

// Live state of the system buttons' default F-key outputs (F11..F15), for the
// tester's "System buttons" readout. Index 0=F11 .. 4=F15.
bool g_fkey[5] = { false, false, false, false, false };
static int fkey_index(int sym)
{
    switch (sym) {
    case SDLK_F11: return 0;
    case SDLK_F12: return 1;
    case SDLK_F13: return 2;
    case SDLK_F14: return 3;
    case SDLK_F15: return 4;
    default: return -1;
    }
}

// while set (SDL_GetTicks deadline), gamepad input is shown in the tester
// only: ImGui gamepad-nav and the quit combo are disabled
static Uint32 g_capture_until = 0;

static bool capturing()
{
    return g_capture_until && SDL_GetTicks() < g_capture_until;
}

static void tab_gamepad(SDL_GameController *gc)
{
    if (!gc) {
        ImGui::TextWrapped("No game controller detected.");
        return;
    }
    ImGui::Text("%s", SDL_GameControllerName(gc));

    if (capturing()) {
        ImGui::SameLine();
        ImGui::TextColored(C_ACCENT,
                           "CAPTURING %us — UI nav disabled",
                           (g_capture_until - SDL_GetTicks()) / 1000 + 1);
    } else {
        ImGui::SameLine();
        if (ImGui::Button("Capture gamepad 15 s"))
            g_capture_until = SDL_GetTicks() + 15000;
        ImGui::SameLine();
        ImGui::TextDisabled("(test buttons without moving the UI)");
    }
    ImGui::Separator();

    static const struct { SDL_GameControllerButton b; const char *n; } BTNS[] = {
        { SDL_CONTROLLER_BUTTON_A, "A" }, { SDL_CONTROLLER_BUTTON_B, "B" },
        { SDL_CONTROLLER_BUTTON_X, "X" }, { SDL_CONTROLLER_BUTTON_Y, "Y" },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB" },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB" },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
        { SDL_CONTROLLER_BUTTON_BACK, "Select" },
        { SDL_CONTROLLER_BUTTON_START, "Start" },
        { SDL_CONTROLLER_BUTTON_GUIDE, "Guide" },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, "Up" },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down" },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left" },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right" },
        { SDL_CONTROLLER_BUTTON_PADDLE1, "P1" },
        { SDL_CONTROLLER_BUTTON_PADDLE2, "P2" },
        { SDL_CONTROLLER_BUTTON_PADDLE3, "P3" },
        { SDL_CONTROLLER_BUTTON_PADDLE4, "P4" },
        { SDL_CONTROLLER_BUTTON_MISC1, "Misc" },
    };
    int col = 0;
    for (auto &bd : BTNS) {
        // Only show buttons this controller actually has (hides P1-P4/Misc etc.
        // that our pad's mapping doesn't include and that never fire).
        if (!SDL_GameControllerHasButton(gc, bd.b))
            continue;
        bool on = SDL_GameControllerGetButton(gc, bd.b);
        if (col++)
            ImGui::SameLine();
        if (col > 5)
            col = 0;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? C_OK : C_TILE);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? C_BG : C_DIM);
        ImGui::SmallButton(bd.n);
        ImGui::PopStyleColor(2);
    }

    ImGui::Separator();
    static const struct { SDL_GameControllerAxis a; const char *n; } AXES[] = {
        { SDL_CONTROLLER_AXIS_LEFTX, "LX" }, { SDL_CONTROLLER_AXIS_LEFTY, "LY" },
        { SDL_CONTROLLER_AXIS_RIGHTX, "RX" }, { SDL_CONTROLLER_AXIS_RIGHTY, "RY" },
        { SDL_CONTROLLER_AXIS_TRIGGERLEFT, "LT" },
        { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "RT" },
    };
    for (auto &ad : AXES) {
        Sint16 v = SDL_GameControllerGetAxis(gc, ad.a);
        // triggers are 0..32767; sticks are signed full-range
        bool trigger = ad.a == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                       ad.a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        float frac = trigger ? v / 32767.0f : (v + 32768) / 65535.0f;
        ImGui::ProgressBar(frac, ImVec2(-80, 0));
        ImGui::SameLine();
        ImGui::Text("%s %6d", ad.n, v);
    }
    ImGui::TextDisabled("(stick up/left = negative values: standard SDL convention)");
    ImGui::Separator();
    extern char g_last_key[32];
    ImGui::Text("Last keyboard key: %s", g_last_key[0] ? g_last_key : "(none)");

    ImGui::Separator();
    ImGui::Text("System buttons (live):");
    struct { const char *label; bool on; } sysrows[] = {
        { "KONKR (Guide)",           SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE) != 0 },
        { "Front-top-left  (F11)",   g_fkey[0] },
        { "Front-bottom-right (F12)", g_fkey[1] },
        { "Shoulder-2 LEFT  (F13)",  g_fkey[2] },
        { "Shoulder-2 RIGHT (F14)",  g_fkey[3] },
        { "Front-top-right (F15)",   g_fkey[4] },
    };
    for (auto &r : sysrows) {
        ImGui::TextColored(r.on ? C_OK : C_DIM, r.on ? "[#]" : "[ ]");
        ImGui::SameLine();
        ImGui::TextUnformatted(r.label);
    }
    ImGui::TextDisabled("(default routing; a remapped or opener button reports differently)");

    ImGui::Separator();
    static int strong = 65535, weak = 32768;
    ImGui::SliderInt("strong##rumble", &strong, 0, 65535);
    ImGui::SliderInt("weak##rumble", &weak, 0, 65535);
    if (ImGui::Button("Rumble 500 ms"))
        SDL_GameControllerRumble(gc, (Uint16)strong, (Uint16)weak, 500);
}

void tab_buttons()
{
    static bool loaded = false;
    if (!loaded) {
        for (auto &k : g_keys)
            key_read(k);
        for (int i = 0; i < N_SYS_TARGETS; i++)
            SYS_TARGET_LABELS[i] = SYS_TARGETS[i].label;
        for (int i = 0; i < 6; i++)
            g_sysbtn_labels[i] = g_sysbtns[i].label;
        sysbtn_load();    // current per-button bindings from konkr-inputd
        opener_load();    // which button is the sidebar opener
        loaded = true;
    }

    ImGui::TextWrapped("Back grips remap inside the MCU and re-apply at boot. "
                       "System buttons are routed live by konkr-inputd; one of "
                       "them opens/closes the sidebar (set below).");
    ImGui::Separator();

    ImGui::Text("Back grips (gamepad clones):");
    for (int i = 0; i < 2; i++) {	// back grips only; one remap path per button
        KeySlot &k = g_keys[i];
        ImGui::PushID(i);
        if (!k.present) {
            ImGui::Text("%s: sysfs not available", k.label);
            ImGui::PopID();
            continue;
        }
        int sel = (k.func == 1 && k.target < N_TARGETS) ? k.target : 0;
        ImGui::Text("%-28s", k.label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280);
        if (ImGui::Combo("##tgt", &sel, TARGET_NAMES, N_TARGETS)) {
            int func = sel ? 1 : 0; // JOYSTICK, or NONE to unmap
            if (key_write(k, func, sel)) {
                key_read(k);
                g_conf.key_func[i] = k.func;
                g_conf.key_target[i] = k.target;
                conf_save(g_conf);
            }
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Text("Sidebar opener:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("##op", &g_opener_idx, g_sysbtn_labels, 6))
        opener_send();
    ImGui::SameLine();
    if (ImGui::Checkbox("hold", &g_opener_hold))
        opener_send();
    ImGui::SameLine();
    ImGui::TextDisabled(g_opener_hold ? "(hold opens; tap = action)" : "(press opens)");

    ImGui::Separator();
    ImGui::Text("System buttons (live, no dropout):");
    for (int i = 0; i < 6; i++) {
        ImGui::PushID(100 + i);
        ImGui::Text("%-22s", g_sysbtns[i].label);
        ImGui::SameLine();
        if (i == g_opener_idx && !g_opener_hold) {
            // Press-opener consumes the whole button — no separate action.
            ImGui::TextDisabled("\xE2\x86\x92 opens sidebar");
        } else {
            ImGui::SetNextItemWidth(260);
            if (ImGui::Combo("##t", &g_sysbtns[i].sel, SYS_TARGET_LABELS, N_SYS_TARGETS))
                sysbtn_send(g_sysbtns[i]);   // apply instantly
            if (i == g_opener_idx) {         // hold-opener: this is the tap action
                ImGui::SameLine();
                ImGui::TextDisabled("(tap)");
            }
        }
        ImGui::PopID();
    }

    if (ImGui::Button("Restore defaults")) {
        ipc_cmd("RESET");
        sysbtn_load();
        opener_load();
    }
}

void tab_rgb()
{
    static float col[3] = { 1.0f, 0.0f, 0.0f };
    static int brightness = 255;
    static bool inited = false;
    if (!inited) {
        char buf[32];
        int r, g, b;
        if (read_file(std::string(LED_DIR) + "/multi_intensity", buf, sizeof buf) &&
            sscanf(buf, "%d %d %d", &r, &g, &b) == 3) {
            col[0] = r / 255.0f;
            col[1] = g / 255.0f;
            col[2] = b / 255.0f;
        }
        if (read_file(std::string(LED_DIR) + "/brightness", buf, sizeof buf))
            brightness = atoi(buf);
        inited = true;
    }

    if (ImGui::Button("Apply"))
        rgb_apply((int)(col[0] * 255), (int)(col[1] * 255),
                  (int)(col[2] * 255), brightness);
    ImGui::SameLine();
    if (ImGui::Button("Off"))
        rgb_apply(0, 0, 0, 0);
    ImGui::SameLine();
    ImGui::ColorButton("##preview", ImVec4(col[0], col[1], col[2], 1.0f));

    ImGui::SliderInt("brightness", &brightness, 0, 255);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::ColorPicker3("##rgb", col, ImGuiColorEditFlags_NoSidePreview);

    ImGui::Separator();
    if (ImGui::Button("Save as boot config")) {
        g_conf.rgb[0] = (int)(col[0] * 255);
        g_conf.rgb[1] = (int)(col[1] * 255);
        g_conf.rgb[2] = (int)(col[2] * 255);
        g_conf.brightness = brightness;
        conf_save(g_conf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear boot config")) {
        g_conf.rgb[0] = g_conf.rgb[1] = g_conf.rgb[2] = -1;
        g_conf.brightness = -1;
        conf_save(g_conf);
    }
}

static void tab_audio()
{
    static float vol = -1.0f;
    static bool muted = false;
    if (vol < 0) {
        // "Volume: 0.65 [MUTED]"
        std::string out = run_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
        float v;
        if (sscanf(out.c_str(), "Volume: %f", &v) == 1)
            vol = v;
        else
            vol = 0.5f;
        muted = out.find("MUTED") != std::string::npos;
    }
    if (ImGui::SliderFloat("volume", &vol, 0.0f, 1.0f, "%.2f")) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f", vol);
        run_cmd(cmd);
    }
    if (ImGui::Checkbox("mute", &muted)) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "wpctl set-mute @DEFAULT_AUDIO_SINK@ %d", muted ? 1 : 0);
        run_cmd(cmd);
    }
    if (ImGui::Button("Refresh"))
        vol = -1.0f;
}

void tab_fan()
{
    static std::string pwm = find_pwm();
    if (pwm.empty()) {
        ImGui::TextWrapped("No pwm-fan hwmon found.");
        return;
    }

    // frame countdown rather than SDL_GetTicks: the latter never advances in
    // the EGL sidebar (SDL isn't initialised there), which would freeze this
    // readout
    static int poll_in = 0;
    static double temp_now = 0.0;
    static int pwm_now = 0;
    if (poll_in <= 0) {
        poll_in = 30;
        temp_now = fan_temp_now();
        char buf[16];
        if (read_file(pwm, buf, sizeof buf))
            pwm_now = atoi(buf);
    }
    poll_in--;

    static int profile = -1;
    static FanCurve custom;
    static bool dirty = false;
    static int sel = 0;
    if (profile < 0) {
        profile = fan_profile_get();
        custom = fan_curve_load();
    }

    ImGui::Text("SoC %.1f C   fan %d/255 (%d%%)", temp_now, pwm_now, pwm_now * 100 / 255);
    ImGui::Separator();

    for (int i = 0; i < 4; i++) {
        if (i)
            ImGui::SameLine();
        if (ImGui::RadioButton(FAN_PROFILES[i], profile == i) && profile != i) {
            profile = i;
            if (i == 3) {
                // the daemon resets a custom profile to moderate if the conf
                // is missing, so make sure it exists before switching
                char probe[8];
                if (!read_file(FAN_CONF, probe, sizeof probe))
                    fan_curve_save(custom);
            }
            g_fan_manual = false;	// fan_profile_set restarts the daemon
            fan_profile_set(i);
        }
    }

    bool editable = (profile == 3);
    if (editable) {
        if (fan_curve_plot(custom, true, sel, temp_now, pwm_now))
            dirty = true;
    } else {
        FanCurve show = profile == 0 ? FAN_QUIET
                      : profile == 2 ? FAN_AGGRESSIVE
                                     : FAN_MODERATE;
        fan_curve_plot(show, false, sel, temp_now, pwm_now);
        ImGui::TextDisabled("built-in profile: read-only (select custom to edit)");
    }

    if (editable) {
        // gamepad-friendly editing of the touch-draggable points
        ImGui::AlignTextToFramePadding();
        ImGui::Text("point %d/8", sel + 1);
        ImGui::SameLine();
        if (ImGui::ArrowButton("selprev", ImGuiDir_Left))
            sel = (sel + 7) % 8;
        ImGui::SameLine();
        if (ImGui::ArrowButton("selnext", ImGuiDir_Right))
            sel = (sel + 1) % 8;

        if (sel < 7) {
            int lo = custom.temp_c[sel + 1] + 1;
            int hi = sel ? custom.temp_c[sel - 1] - 1 : 100;
            if (ImGui::SliderInt("above C", &custom.temp_c[sel], lo, hi))
                dirty = true;
        } else {
            ImGui::TextDisabled("below all thresholds (idle floor)");
        }
        if (ImGui::SliderInt("pwm##pt", &custom.speed[sel], 0, 255))
            dirty = true;

        if (ImGui::Button("Save & Apply")) {
            if (fan_curve_save(custom)) {
                g_fan_manual = false;
                fan_profile_set(3);
                dirty = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            custom = fan_curve_load();
            dirty = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy moderate")) {
            custom = FAN_MODERATE;
            dirty = true;
        }
        if (dirty) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved)");
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Manual test")) {
        if (ImGui::Checkbox("manual pwm (stops fancontrol)", &g_fan_manual))
            run_cmd(g_fan_manual ? "systemctl stop fancontrol"
                                 : "systemctl start fancontrol");
        if (g_fan_manual) {
            static int val = -1;
            char buf[16];
            if (val < 0 && read_file(pwm, buf, sizeof buf))
                val = atoi(buf);
            if (ImGui::SliderInt("pwm##manual", &val, 0, 255)) {
                snprintf(buf, sizeof buf, "%d", val);
                write_file(pwm, buf);
            }
        }
    }
}

// ---------- motion (IIO: qcom-ssc-imu) ----------

// The SSC accelerometer/gyroscope are exposed as a standard IIO device by
// the qcom_ssc_imu kernel driver. SDL has no Linux sensor backend, so this
// tab reads the IIO sysfs interface directly. Raw values are micro-SI
// (scale 1e-6): accel in m/s^2, gyro in rad/s.

static std::string find_iio_imu()
{
    DIR *d = opendir("/sys/bus/iio/devices");
    if (!d)
        return "";
    std::string found;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "iio:device", 10) != 0)
            continue;
        std::string base = std::string("/sys/bus/iio/devices/") + e->d_name;
        char buf[64];
        if (read_file(base + "/name", buf, sizeof buf)) {
            char *nl = strchr(buf, '\n');
            if (nl)
                *nl = 0;
            if (strcmp(buf, "qcom-ssc-imu") == 0) {
                found = base;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

static double iio_read(const std::string &base, const char *attr)
{
    char buf[32];
    if (!read_file(base + "/" + attr, buf, sizeof buf))
        return 0.0;
    return atof(buf);
}

static void plot_axis(const char *label, float *hist, int n, int *head,
                      float v, float range)
{
    hist[*head] = v;
    *head = (*head + 1) % n;
    char overlay[32];
    snprintf(overlay, sizeof overlay, "%s % 7.3f", label, v);
    ImGui::PushID(hist);
    ImGui::PlotLines("", hist, n, *head, overlay, -range, range,
                     ImVec2(0, 40));
    ImGui::PopID();
}

static void tab_motion()
{
    static std::string base = find_iio_imu();
    if (base.empty()) {
        ImGui::TextWrapped("No qcom-ssc-imu IIO device found. The ADSP sensor "
                           "PD must be running (hexagonrpcd-sensorspd.service) "
                           "for the accelerometer/gyroscope to enumerate.");
        if (ImGui::Button("Rescan"))
            base = find_iio_imu();
        return;
    }

    double as = iio_read(base, "in_accel_scale");
    double gs = iio_read(base, "in_anglvel_scale");
    float ax = iio_read(base, "in_accel_x_raw") * as;
    float ay = iio_read(base, "in_accel_y_raw") * as;
    float az = iio_read(base, "in_accel_z_raw") * as;
    float gx = iio_read(base, "in_anglvel_x_raw") * gs;
    float gy = iio_read(base, "in_anglvel_y_raw") * gs;
    float gz = iio_read(base, "in_anglvel_z_raw") * gs;
    float amag = sqrtf(ax * ax + ay * ay + az * az);

    ImGui::Text("%s   rate %d Hz", base.c_str(),
                (int)iio_read(base, "sampling_frequency"));
    ImGui::Separator();

    enum { N = 240 };
    static float hax[N] = {}, hay[N] = {}, haz[N] = {};
    static float hgx[N] = {}, hgy[N] = {}, hgz[N] = {};
    static int head = 0, ghead = 0;

    ImGui::Text("Accelerometer (m/s^2)   |a| = %6.3f  (1g = 9.81)", amag);
    int h = head;
    plot_axis("X", hax, N, &h, ax, 12.0f);
    h = head;
    plot_axis("Y", hay, N, &h, ay, 12.0f);
    h = head;
    plot_axis("Z", haz, N, &h, az, 12.0f);
    head = (head + 1) % N;

    ImGui::Separator();
    ImGui::Text("Gyroscope (rad/s)");
    int g = ghead;
    plot_axis("X", hgx, N, &g, gx, 5.0f);
    g = ghead;
    plot_axis("Y", hgy, N, &g, gy, 5.0f);
    g = ghead;
    plot_axis("Z", hgz, N, &g, gz, 5.0f);
    ghead = (ghead + 1) % N;
}

// ---------- device info ----------

static std::string human_gb(unsigned long long bytes)
{
    char s[32];
    snprintf(s, sizeof s, "%.1f GB", bytes / 1e9);
    return s;
}

static std::string human_gib(unsigned long long bytes)
{
    char s[32];
    snprintf(s, sizeof s, "%.1f GiB", bytes / 1073741824.0);
    return s;
}

// /sys/block/<dev>/size is in 512-byte sectors
static unsigned long long block_bytes(const std::string &dev)
{
    char buf[32];
    if (!read_file("/sys/block/" + dev + "/size", buf, sizeof buf))
        return 0;
    return strtoull(buf, nullptr, 10) * 512ULL;
}

static std::string mount_source(const char *mountpoint)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return "?";
    char dev[128], mp[128];
    std::string found = "(not mounted)";
    while (fscanf(f, "%127s %127s %*[^\n]\n", dev, mp) == 2) {
        if (!strcmp(mp, mountpoint)) {
            found = dev;
            break;
        }
    }
    fclose(f);
    return found;
}

static const char *medium_of(const std::string &dev)
{
    if (dev.rfind("/dev/sd", 0) == 0)
        return "internal UFS";
    if (dev.rfind("/dev/mmcblk", 0) == 0)
        return "microSD";
    return "?";
}

struct DevInfo {
    std::string model, soc, cpu, ram;
    std::string ufs_size, ufs_dev, ufs_ver;
    std::string sd_size;            // empty = no card
    std::string flash_src, storage_src;
};

static void devinfo_gather(DevInfo &d)
{
    char buf[256];

    if (read_file("/proc/device-tree/model", buf, sizeof buf))
        d.model = buf;

    // socinfo's machine attr carries the device name on this platform,
    // not the SoC — and this app is KONKR-specific anyway
    d.soc = "Qualcomm SM8750 (Snapdragon 8 Elite)";
    if (read_file("/sys/devices/soc0/revision", buf, sizeof buf)) {
        buf[strcspn(buf, "\n")] = 0;
        d.soc += " rev ";
        d.soc += buf;
    }

    // core count + the fastest policy's max clock
    int cores = 0;
    unsigned long maxkhz = 0;
    for (int i = 0; i < 16; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", i);
        if (read_file(p, buf, sizeof buf) || i == 0)
            cores++;
        else
            break;
        snprintf(p, sizeof p,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (read_file(p, buf, sizeof buf)) {
            unsigned long k = strtoul(buf, nullptr, 10);
            if (k > maxkhz)
                maxkhz = k;
        }
    }
    char cpu[64];
    snprintf(cpu, sizeof cpu, "%d cores, up to %.2f GHz", cores, maxkhz / 1e6);
    d.cpu = cpu;

    if (read_file("/proc/meminfo", buf, sizeof buf)) {
        unsigned long kb = 0;
        if (sscanf(buf, "MemTotal: %lu kB", &kb) == 1) {
            char r[32];
            snprintf(r, sizeof r, "%.1f GiB", kb / 1024.0 / 1024.0);
            d.ram = r;
        }
    }

    // internal UFS = sda; spec version from the ufshc device descriptor
    unsigned long long b = block_bytes("sda");
    if (b) {
        d.ufs_size = human_gb(b);
        char model[64] = {}, vendor[64] = {};
        read_file("/sys/block/sda/device/vendor", vendor, sizeof vendor);
        read_file("/sys/block/sda/device/model", model, sizeof model);
        vendor[strcspn(vendor, "\n")] = 0;
        model[strcspn(model, "\n")] = 0;
        d.ufs_dev = std::string(vendor) + " " + model;
    }
    DIR *pd = opendir("/sys/bus/platform/devices");
    if (pd) {
        for (dirent *e; (e = readdir(pd));) {
            if (!strstr(e->d_name, "ufs"))
                continue;
            std::string p = std::string("/sys/bus/platform/devices/") +
                            e->d_name +
                            "/device_descriptor/specification_version";
            if (read_file(p, buf, sizeof buf)) {
                // BCD, e.g. 0x0400 -> UFS 4.0
                unsigned v = (unsigned)strtoul(buf, nullptr, 16);
                char s[24];
                snprintf(s, sizeof s, "UFS %x.%x", (v >> 8) & 0xff, (v >> 4) & 0xf);
                d.ufs_ver = s;
                break;
            }
        }
        closedir(pd);
    }

    // microSD: any mmcblkN (skip eMMC boot/rpmb sub-devices)
    DIR *bd = opendir("/sys/block");
    if (bd) {
        for (dirent *e; (e = readdir(bd));) {
            if (strncmp(e->d_name, "mmcblk", 6) != 0 ||
                strstr(e->d_name, "boot") || strstr(e->d_name, "rpmb"))
                continue;
            b = block_bytes(e->d_name);
            if (b)
                d.sd_size = human_gb(b);
            break;
        }
        closedir(bd);
    }

    d.flash_src = mount_source("/flash");
    d.storage_src = mount_source("/storage");
}

// ---------- Storage ----------

struct PartInfo {
    std::string label, dev;
    unsigned long long bytes;
};

// draw a used/free bar for a mounted filesystem (live statvfs each frame)
static void storage_bar(const char *label, const char *mp)
{
    struct statvfs v;
    if (statvfs(mp, &v) != 0 || v.f_blocks == 0)
        return;
    unsigned long long bs = v.f_frsize;
    unsigned long long total = (unsigned long long)v.f_blocks * bs;
    unsigned long long avail = (unsigned long long)v.f_bavail * bs;
    unsigned long long used = total - (unsigned long long)v.f_bfree * bs;
    char ov[96];
    snprintf(ov, sizeof ov, "%.1f / %.1f GB used   %.1f GB free",
             used / 1e9, total / 1e9, avail / 1e9);
    ImGui::Text("%s", label);
    ImGui::ProgressBar(total ? (float)((double)used / total) : 0.f,
                       ImVec2(-1, 0), ov);
}

// ---------- Display ----------

static void tab_display()
{
    struct DMode { std::string res, exact, label; };
    static std::string out_name, phys, res, cur_refresh, status;
    static std::vector<DMode> modes;
    static bool inited = false;

    if (!inited) {
        out_name.clear(); phys.clear(); res.clear(); cur_refresh.clear();
        modes.clear();
        try {
            json arr = json::parse(run_cmd("wlr-randr --json 2>/dev/null"));
            const json *o = nullptr;
            for (const auto &e : arr)
                if (e.value("enabled", false)) { o = &e; break; }
            if (!o && !arr.empty())
                o = &arr.front();
            if (o) {
                out_name = o->value("name", std::string());
                auto ps = o->find("physical_size");
                if (ps != o->end()) {
                    int pw = ps->value("width", 0), ph = ps->value("height", 0);
                    if (pw > 0 && ph > 0) {
                        char b[32];
                        snprintf(b, sizeof b, "%dx%d mm", pw, ph);
                        phys = b;
                    }
                }
                auto ml = o->find("modes");
                if (ml != o->end()) {
                    for (const auto &m : *ml) {
                        int w = m.value("width", 0), h = m.value("height", 0);
                        double r = m.value("refresh", 0.0);
                        char rmode[24], exact[24], label[16];
                        snprintf(rmode, sizeof rmode, "%dx%d", w, h);
                        snprintf(exact, sizeof exact, "%.6f", r);  // verbatim for --mode
                        snprintf(label, sizeof label, "%.0f", r);  // rounded for the button
                        if (m.value("current", false)) {
                            cur_refresh = label;
                            res = rmode;
                        } else if (res.empty()) {
                            res = rmode;
                        }
                        bool dup = false;
                        for (const auto &dm : modes)
                            if (dm.res == rmode && dm.label == label)
                                dup = true;
                        if (!dup)
                            modes.push_back({ rmode, exact, label });
                    }
                }
            }
        } catch (const std::exception &) {
            /* malformed/empty wlr-randr output -> "no output" message below */
        }
        inited = true;
    }

    if (out_name.empty()) {
        ImGui::TextWrapped("No display output reported (wlr-randr unavailable).");
        if (ImGui::Button("Refresh"))
            inited = false;
        return;
    }

    ImGui::Text("Output:        %s", out_name.c_str());
    ImGui::Text("Resolution:    %s", res.empty() ? "?" : res.c_str());
    if (!phys.empty()) {
        int w = 0, h = 0;
        if (sscanf(phys.c_str(), "%dx%d", &w, &h) == 2 && w && h) {
            double diag = sqrt((double)w * w + (double)h * h) / 25.4;
            ImGui::Text("Physical size: %s  (%.1f\")", phys.c_str(), diag);
        } else {
            ImGui::Text("Physical size: %s", phys.c_str());
        }
    }
    ImGui::Text("Refresh rate:  %s Hz", cur_refresh.empty() ? "?" : cur_refresh.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Set refresh rate");
    for (size_t i = 0; i < modes.size(); i++) {
        bool active = modes[i].label == cur_refresh;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_BG);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dc_rgb(0xF0A83C, 0.26f));
            ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
        }
        char blbl[16];
        snprintf(blbl, sizeof blbl, "%s Hz", modes[i].label.c_str());
        if (ImGui::Button(blbl)) {
            char cmd[160];
            snprintf(cmd, sizeof cmd,
                     "wlr-randr --output %s --mode %s@%sHz 2>&1",
                     out_name.c_str(), modes[i].res.c_str(), modes[i].exact.c_str());
            status = run_cmd(cmd);
            inited = false;   // re-read current
        }
        if (active)
            ImGui::PopStyleColor(3);
        if (i + 1 < modes.size())
            ImGui::SameLine();
    }
    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button("Refresh info"))
        inited = false;
}

// ---------- power profiles ----------
// keep in sync with quirks/platforms/SM8750/bin/power_profile
static const char *PP_BIN =
    "/usr/lib/autostart/quirks/platforms/SM8750/bin/power_profile";
static const char *POWER_PROFILES[]       = { "full", "balanced", "saver", "custom" };
static const char *POWER_PROFILE_LABELS[] = { "Full Power", "Balanced", "Power Saver", "Custom" };
enum { PP_CUSTOM = 3 };

static const char *GPU_DEVFREQ =
    "/sys/devices/platform/soc@0/3d00000.gpu/devfreq/3d00000.gpu";

static int power_profile_get()
{
    std::string out = run_cmd(". /etc/profile 2>/dev/null; "
                              "get_setting system.powerprofile 2>/dev/null");
    for (int i = 0; i < 4; i++)
        if (!strncmp(out.c_str(), POWER_PROFILES[i], strlen(POWER_PROFILES[i])))
            return i;
    return 0;	// unset: no cap == full
}

// parse a sysfs space/newline-separated frequency list, sorted ascending
static std::vector<int> read_freq_list(const std::string &path)
{
    std::vector<int> v;
    char buf[1024];
    if (read_file(path, buf, sizeof buf))
        for (char *t = strtok(buf, " \n"); t; t = strtok(nullptr, " \n")) {
            int f = atoi(t);
            if (f > 0)
                v.push_back(f);
        }
    std::sort(v.begin(), v.end());
    return v;
}

static int nearest_idx(const std::vector<int> &v, int val)
{
    int best = 0;
    long bd = -1;
    for (size_t i = 0; i < v.size(); i++) {
        long d = (long)v[i] - val;
        if (d < 0) d = -d;
        if (bd < 0 || d < bd) { bd = d; best = (int)i; }
    }
    return best;
}

// apply an absolute kHz cap to every CPU policy (clamped to each cluster's max)
static void cpu_cap_apply(int khz)
{
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    for (dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "policy", 6))
            continue;
        std::string base = std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name;
        char b[24];
        int hw = 0;
        if (read_file(base + "/cpuinfo_max_freq", b, sizeof b))
            hw = atoi(b);
        int t = (hw > 0 && khz > hw) ? hw : khz;
        write_file(base + "/scaling_max_freq", std::to_string(t));
    }
    if (d)
        closedir(d);
}

static void power_profile_set(int idx)
{
    char cmd[192];
    snprintf(cmd, sizeof cmd, "%s %s", PP_BIN, POWER_PROFILES[idx]);
    run_cmd(cmd);
}

static void power_profile_set_custom(int cpu, int gpu)
{
    char cmd[192];
    snprintf(cmd, sizeof cmd, "%s custom %d %d", PP_BIN, cpu, gpu);
    run_cmd(cmd);
}

// ---------- cpu governor ----------
// Independent of the freq cap: the cap sets the ceiling, the governor decides
// how aggressively the cores ride up to it. Not persisted — ES/runemu re-set
// the governor (performance) at game launch, so this is a live override only.

static std::vector<std::string> read_governor_list(const std::string &policy_base)
{
    std::vector<std::string> v;
    char buf[256];
    if (read_file(policy_base + "/scaling_available_governors", buf, sizeof buf))
        for (char *t = strtok(buf, " \n"); t; t = strtok(nullptr, " \n"))
            v.push_back(t);
    return v;
}

static std::string cpu_governor_get(const std::string &policy_base)
{
    char buf[64];
    if (read_file(policy_base + "/scaling_governor", buf, sizeof buf)) {
        buf[strcspn(buf, "\r\n")] = 0;
        return buf;
    }
    return "";
}

static void cpu_governor_set(const std::string &gov)
{
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    for (dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "policy", 6))
            continue;
        std::string base = std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name;
        write_file(base + "/scaling_governor", gov);
    }
    if (d)
        closedir(d);
}

void tab_power()
{
    // one-time hardware discovery: the prime CPU cluster (highest ceiling) and
    // the discrete OPP lists that drive the sliders
    static bool inited = false;
    static std::string prime_policy;
    static std::vector<int> cpu_opps, gpu_opps;	// kHz / Hz, ascending
    static std::vector<std::string> govs;	// available CPU governors
    if (!inited) {
        inited = true;
        int best_hw = 0;
        DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
        for (dirent *e; d && (e = readdir(d));) {
            if (strncmp(e->d_name, "policy", 6))
                continue;
            std::string base = std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name;
            char b[24];
            int hw = 0;
            if (read_file(base + "/cpuinfo_max_freq", b, sizeof b))
                hw = atoi(b);
            if (hw > best_hw) { best_hw = hw; prime_policy = base; }
        }
        if (d)
            closedir(d);
        cpu_opps = read_freq_list(prime_policy + "/scaling_available_frequencies");
        gpu_opps = read_freq_list(std::string(GPU_DEVFREQ) + "/available_frequencies");
        govs = read_governor_list(prime_policy);
    }

    static int profile = -1;
    if (profile < 0)
        profile = power_profile_get();
    static int cpu_idx = 0, gpu_idx = 0;	// slider position = OPP index
    static int gov_idx = -1;	// combo position = governor index

    // Poll the live caps on a frame countdown rather than SDL_GetTicks (which
    // doesn't advance in the EGL sidebar) so the sliders stay synced to the
    // real cap in both the panel and the sidebar.
    static int poll_in = 0;
    static std::string cpu_lines, gpu_line;
    if (poll_in <= 0) {
        poll_in = 30;
        cpu_lines.clear();
        gpu_line.clear();
        char buf[24], line[96];

        DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
        std::vector<std::string> pols;
        for (dirent *e; d && (e = readdir(d));)
            if (!strncmp(e->d_name, "policy", 6))
                pols.push_back(e->d_name);
        if (d)
            closedir(d);
        std::sort(pols.begin(), pols.end());
        int prime_cap = 0;
        for (auto &p : pols) {
            std::string base = "/sys/devices/system/cpu/cpufreq/" + p;
            int cur = 0, cap = 0;
            if (read_file(base + "/scaling_cur_freq", buf, sizeof buf)) cur = atoi(buf);
            if (read_file(base + "/scaling_max_freq", buf, sizeof buf)) cap = atoi(buf);
            if (base == prime_policy) prime_cap = cap;
            snprintf(line, sizeof line, "%-8s %4d MHz now   cap %d MHz\n",
                     p.c_str(), cur / 1000, cap / 1000);
            cpu_lines += line;
        }
        int gcur = 0, gcap = 0;
        if (read_file(std::string(GPU_DEVFREQ) + "/cur_freq", buf, sizeof buf)) gcur = atoi(buf);
        if (read_file(std::string(GPU_DEVFREQ) + "/max_freq", buf, sizeof buf)) gcap = atoi(buf);
        snprintf(line, sizeof line, "GPU      %4d MHz now   cap %d MHz",
                 gcur / 1000000, gcap / 1000000);
        gpu_line = line;

        // point the sliders at the live cap, unless the user is dragging them
        if (!ImGui::IsAnyItemActive()) {
            if (!cpu_opps.empty()) cpu_idx = nearest_idx(cpu_opps, prime_cap);
            if (!gpu_opps.empty()) gpu_idx = nearest_idx(gpu_opps, gcap);
            std::string cur_gov = cpu_governor_get(prime_policy);
            for (size_t i = 0; i < govs.size(); i++)
                if (govs[i] == cur_gov) { gov_idx = (int)i; break; }
        }
    }
    poll_in--;

    ImGui::TextWrapped("Caps the maximum CPU and GPU frequency, trading peak "
                       "performance for battery life and lower heat. The cap "
                       "holds in games too.");
    ImGui::Separator();

    for (int i = 0; i < 4; i++) {
        if (i)
            ImGui::SameLine();
        if (ImGui::RadioButton(POWER_PROFILE_LABELS[i], profile == i) && profile != i) {
            profile = i;
            if (i == PP_CUSTOM)
                power_profile_set_custom(cpu_opps.empty() ? 0 : cpu_opps[cpu_idx],
                                         gpu_opps.empty() ? 0 : gpu_opps[gpu_idx]);
            else
                power_profile_set(i);
            poll_in = 0;	// re-read the new caps next frame
        }
    }

    // Sliders step over the real OPP list (valid frequencies only) and show the
    // resulting MHz + % of max. Editable only for Custom; presets display the
    // active cap read-only. Custom applies live while dragging and persists on
    // release.
    bool custom = (profile == PP_CUSTOM);
    char fmt[32];

    if (cpu_opps.empty()) {
        ImGui::TextDisabled("CPU: no cpufreq");
    } else {
        int mhz = cpu_opps[cpu_idx] / 1000;
        int pct = cpu_opps[cpu_idx] * 100 / cpu_opps.back();
        // %%%% -> a literal "%%" here, which ImGui's own printf renders as "%"
        snprintf(fmt, sizeof fmt, "%d MHz (%d%%%%)", mhz, pct);
        ImGui::BeginDisabled(!custom);
        if (ImGui::SliderInt("CPU max", &cpu_idx, 0, (int)cpu_opps.size() - 1, fmt) && custom)
            cpu_cap_apply(cpu_opps[cpu_idx]);
        if (custom && ImGui::IsItemDeactivatedAfterEdit()) {
            power_profile_set_custom(cpu_opps[cpu_idx], gpu_opps.empty() ? 0 : gpu_opps[gpu_idx]);
            poll_in = 0;
        }
        ImGui::EndDisabled();
    }

    if (gpu_opps.empty()) {
        ImGui::TextDisabled("GPU: no devfreq");
    } else {
        int mhz = gpu_opps[gpu_idx] / 1000000;
        int pct = (int)((long)gpu_opps[gpu_idx] * 100 / gpu_opps.back());
        snprintf(fmt, sizeof fmt, "%d MHz (%d%%%%)", mhz, pct);
        ImGui::BeginDisabled(!custom);
        if (ImGui::SliderInt("GPU max", &gpu_idx, 0, (int)gpu_opps.size() - 1, fmt) && custom)
            write_file(std::string(GPU_DEVFREQ) + "/max_freq", std::to_string(gpu_opps[gpu_idx]));
        if (custom && ImGui::IsItemDeactivatedAfterEdit()) {
            power_profile_set_custom(cpu_opps.empty() ? 0 : cpu_opps[cpu_idx], gpu_opps[gpu_idx]);
            poll_in = 0;
        }
        ImGui::EndDisabled();
    }

    // CPU governor — applies to every cluster, live. Not tied to the profile
    // above (the cap is the ceiling; the governor is the ramp policy). Reset by
    // ES/runemu on game launch, so it isn't persisted.
    ImGui::Separator();
    if (govs.empty()) {
        ImGui::TextDisabled("CPU governor: unavailable");
    } else {
        ImGui::TextUnformatted("CPU governor");
        ImGui::SetNextItemWidth(-1);
        const char *cur = (gov_idx >= 0 && gov_idx < (int)govs.size())
                              ? govs[gov_idx].c_str() : "(mixed)";
        if (ImGui::BeginCombo("##gov", cur)) {
            for (int i = 0; i < (int)govs.size(); i++)
                if (ImGui::Selectable(govs[i].c_str(), i == gov_idx)) {
                    gov_idx = i;
                    cpu_governor_set(govs[i]);
                    poll_in = 0;	// re-read live state next frame
                }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(cpu_lines.c_str());
    ImGui::TextUnformatted(gpu_line.c_str());
}

// ---------- boot frontend ----------
// Writes system.frontend; the SM8750 090-ui_service quirk reads it at boot to
// pick UI_SERVICE (es=EmulationStation, steam=Steam Big Picture session,
// konkr-launcher=our grid frontend). Takes effect on next reboot.
static const char *FRONTENDS[]       = { "es", "steam", "konkr-launcher" };
static const char *FRONTEND_LABELS[] = { "EmulationStation", "Steam (Big Picture)",
                                         "KONKR Launcher" };

static int frontend_get()
{
    std::string out = run_cmd(". /etc/profile 2>/dev/null; "
                              "get_setting system.frontend 2>/dev/null");
    for (int i = 0; i < 3; i++)
        if (!strncmp(out.c_str(), FRONTENDS[i], strlen(FRONTENDS[i])))
            return i;
    return 0;	// unset: default == es
}

static void frontend_set(int idx)
{
    char cmd[160];
    snprintf(cmd, sizeof cmd,
             ". /etc/profile 2>/dev/null; set_setting system.frontend %s", FRONTENDS[idx]);
    run_cmd(cmd);
}

// Merged System tab: boot-frontend selector + device / storage / live status.
// (Folds in what used to be the separate Storage and Device tabs — the names
// were too close to tell apart.)
static void tab_system()
{
    char buf[128];

    // ---- one-time hardware/partition discovery (Refresh resets it) ----
    static DevInfo dev;
    static std::vector<PartInfo> parts;
    static bool inited = false;
    if (!inited) {
        devinfo_gather(dev);
        parts.clear();
        DIR *pl = opendir("/dev/disk/by-partlabel");
        if (pl) {
            for (dirent *e; (e = readdir(pl));) {
                if (e->d_name[0] == '.')
                    continue;
                char lp[300];
                snprintf(lp, sizeof lp, "/dev/disk/by-partlabel/%s", e->d_name);
                char tgt[256];
                ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
                if (n <= 0)
                    continue;
                tgt[n] = 0;
                const char *base = strrchr(tgt, '/');
                base = base ? base + 1 : tgt;
                if (strncmp(base, "sda", 3) != 0)   // internal data LUN only
                    continue;
                unsigned long long b = block_bytes(std::string("sda/") + base);
                if (b < 64ULL * 1024 * 1024)        // hide tiny firmware parts
                    continue;
                parts.push_back({ e->d_name, base, b });
            }
            closedir(pl);
        }
        std::sort(parts.begin(), parts.end(),
                  [](const PartInfo &a, const PartInfo &b) { return a.bytes > b.bytes; });
        inited = true;
    }

    // ---- boot frontend ----
    static int fe = -1;
    if (fe < 0)
        fe = frontend_get();
    ImGui::TextUnformatted("Boot frontend");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##frontend", FRONTEND_LABELS[fe])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(FRONTEND_LABELS[i], i == fe)) {
                fe = i;
                frontend_set(i);
            }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Applies on next reboot.");

    // ---- device ----
    ImGui::SeparatorText("Device");
    ImGui::Text("Device:  %s", dev.model.c_str());
    ImGui::Text("SoC:     %s", dev.soc.c_str());
    ImGui::Text("CPU:     %s", dev.cpu.c_str());
    ImGui::Text("RAM:     %s", dev.ram.c_str());
    const char *boot_medium = medium_of(dev.flash_src);
    ImGui::Text("ROCKNIX runs from: %s", boot_medium);
    ImGui::TextDisabled("  /flash    %s", dev.flash_src.c_str());
    ImGui::TextDisabled("  /storage  %s (%s)", dev.storage_src.c_str(),
                        medium_of(dev.storage_src));

    // ---- storage ----
    ImGui::SeparatorText("Storage");
    storage_bar("Internal  (/storage)", "/storage");
    storage_bar("Boot      (/flash)", "/flash");
    if (mount_source("/storage/roms").rfind("/dev/mmc", 0) == 0)
        storage_bar("microSD", "/storage/roms");
    ImGui::Spacing();
    ImGui::TextDisabled("Internal drive partitions");
    ImGui::TextWrapped("The internal UFS is shared with Android; only ROCKNIX "
                       "and STORAGE are usable here. The rest is reserved for "
                       "the Android install.");
    for (auto &p : parts) {
        bool ours = (p.label == "ROCKNIX" || p.label == "STORAGE");
        if (!ours)
            ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
        ImGui::Text("  %-12s %9s   %s", p.label.c_str(), human_gib(p.bytes).c_str(),
                    ours ? "ROCKNIX" : "Android / reserved");
        if (!ours)
            ImGui::PopStyleColor();
    }

    // ---- live status ----
    ImGui::SeparatorText("Status");
    if (read_file("/sys/class/power_supply/battery/capacity", buf, sizeof buf)) {
        int cap = atoi(buf);
        char status[32] = "?";
        if (read_file("/sys/class/power_supply/battery/status", status, sizeof status))
            status[strcspn(status, "\n")] = 0;
        ImGui::Text("Battery: %d%% (%s)", cap, status);
    }
    for (int i = 0; i < 10; i++) {
        char path[64], type[64], temp[16];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        if (!read_file(path, type, sizeof type))
            break;
        type[strcspn(type, "\n")] = 0;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_file(path, temp, sizeof temp))
            ImGui::Text("%-24s %5.1f C", type, atoi(temp) / 1000.0);
    }
    static std::string uname = run_cmd("uname -r");
    ImGui::Text("Kernel: %s", uname.c_str());

    ImGui::Separator();
    if (ImGui::Button("Refresh"))
        inited = false;
}

// ---------- header / footer chrome (konkr-launcher family look) ----------

// first system battery under /sys/class/power_supply (type=Battery, not
// scope=Device which is what peripheral batteries report); "" if none
static std::string find_battery_dir()
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return "";
    std::string found;
    for (dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        std::string dir = std::string("/sys/class/power_supply/") + e->d_name;
        char buf[32];
        if (!read_file(dir + "/type", buf, sizeof buf) || strncmp(buf, "Battery", 7) != 0)
            continue;
        if (read_file(dir + "/scope", buf, sizeof buf) && strncmp(buf, "Device", 6) == 0)
            continue;
        found = dir;
        break;
    }
    closedir(d);
    return found;
}

static int  g_batt_cap = -1;
static bool g_batt_charging = false;

static void battery_poll()
{
    static Uint32 last = 0;
    static std::string dir = find_battery_dir();
    Uint32 now = SDL_GetTicks();
    if (dir.empty() || (last != 0 && now - last < 4000))
        return;
    last = now;
    char buf[32];
    g_batt_cap = read_file(dir + "/capacity", buf, sizeof buf) ? atoi(buf) : -1;
    g_batt_charging = false;
    if (read_file(dir + "/status", buf, sizeof buf))
        g_batt_charging = !strncmp(buf, "Charging", 8) || !strncmp(buf, "Full", 4);
}

// Fixed top bar: brand + app title (left), clock + battery (right). Decorative
// and touch-only — invisible to gamepad nav, so it never steals focus.
static void draw_header(float width, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::BeginChild("##hdr", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float u = g_ui;
    const float pad = 22 * u;
    const ImVec2 org = ImGui::GetWindowPos();
    const float fs = ImGui::GetFontSize();

    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

    ImFont *bf = g_font_bold ? g_font_bold : ImGui::GetFont();
    float bh = bf->FontSize;
    float by = org.y + (height - bh) / 2;
    dl->AddText(bf, bh, ImVec2(org.x + pad, by), ImGui::GetColorU32(C_ACCENT), "KONKR");
    float x = pad + bf->CalcTextSizeA(bh, FLT_MAX, 0, "KONKR").x + 14 * u;
    dl->AddText(ImVec2(org.x + x, org.y + (height - fs) / 2),
                ImGui::GetColorU32(C_TEXT), "Device Controls");

    // right block: clock, then battery icon + percent
    char clock[8] = "";
    time_t t = time(nullptr);
    struct tm tm;
    if (localtime_r(&t, &tm))
        strftime(clock, sizeof clock, "%H:%M", &tm);
    char pct[16] = "";
    if (g_batt_cap >= 0)
        snprintf(pct, sizeof pct, "%d%%", g_batt_cap > 100 ? 100 : g_batt_cap);
    float icon_h = fs * 0.62f;
    float icon_w = g_batt_cap >= 0 ? icon_h * 1.85f + 3.5f * u : 0;
    float clock_w = ImGui::CalcTextSize(clock).x;
    float pct_w = ImGui::CalcTextSize(pct).x;
    float gap = 10 * u;
    float total = clock_w + (g_batt_cap >= 0 ? 2 * gap + icon_w + gap * 0.6f + pct_w : 0);
    float rx = org.x + width - pad - total;
    float cy = org.y + height / 2;
    dl->AddText(ImVec2(rx, cy - fs / 2), ImGui::GetColorU32(C_TEXT), clock);
    if (g_batt_cap >= 0) {
        rx += clock_w + 2 * gap;
        dc_battery_icon(dl, ImVec2(rx, cy - icon_h / 2), icon_h,
                        g_batt_cap, g_batt_charging, u);
        rx += icon_w + gap * 0.6f;
        dl->AddText(ImVec2(rx, cy - fs / 2), ImGui::GetColorU32(C_DIM), pct);
    }

    ImGui::PopItemFlag();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Fixed bottom bar: controller-hint pills + a touch-only Quit button. NoNav, so
// gamepad quits via the L1+R1+Start combo (reminded by a pill); touch taps Quit.
// Returns true if Quit was tapped.
static bool draw_footer(float width, float height)
{
    bool quit = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::BeginChild("##ftr", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const float u = g_ui;

    if (g_font_small) ImGui::PushFont(g_font_small);
    float line_h = ImGui::GetTextLineHeight() + 5 * u;
    ImGui::SetCursorPos(ImVec2(22 * u, (height - line_h) / 2));
    dc_hint("A", "Select", u);
    dc_hint("B", "Back", u);
    dc_hint("LB+RB+Start", "Quit", u);
    if (g_font_small) ImGui::PopFont();

    // touch-only Quit on the right (NoNav keeps it off gamepad nav; the combo
    // is the gamepad path). Mouse/touch clicks still register under NoNav.
    float bw = 92 * u, bh = height - 12 * u;
    ImGui::SetCursorPos(ImVec2(width - bw - 22 * u, (height - bh) / 2));
    if (ImGui::Button("Quit", ImVec2(bw, bh)))
        quit = true;

    ImGui::PopItemFlag();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return quit;
}

// ---------- main ----------

int run_sidebar();   // sidebar.cpp — Wayland layer-shell quick-settings overlay

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--restore"))
        return restore_mode();
    if (argc > 1 && !strcmp(argv[1], "--sidebar"))
        return run_sidebar();

    conf_load(g_conf);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Device Controls", SDL_WINDOWPOS_UNDEFINED,
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

    // real vector fonts at native pixel size + the slate/amber launcher theme,
    // scaled to the panel height (replaces StyleColorsDark + the scaled bitmap
    // default font, which was the biggest "prototype look" tell)
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    g_ui = h / 720.0f;
    if (g_ui < 1.0f) g_ui = 1.0f;
    theme_load_fonts(io, g_ui);
    theme_apply_style(g_ui);

    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    SDL_GameController *gc = nullptr;
    bool quit = false;
    while (!quit) {
        // hard frame cap: vsync alone does not pace us when the window is
        // occluded (present returns immediately -> 100% CPU spin)
        Uint32 frame_start = SDL_GetTicks();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT)
                quit = true;
            if (ev.type == SDL_KEYDOWN)
                snprintf(g_last_key, sizeof g_last_key, "%s",
                         SDL_GetKeyName(ev.key.keysym.sym));
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int fi = fkey_index(ev.key.keysym.sym);
                if (fi >= 0)
                    g_fkey[fi] = (ev.type == SDL_KEYDOWN);
            }
        }
        // (re)open the first available controller, e.g. after hotplug
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            if (gc)
                SDL_GameControllerClose(gc);
            gc = nullptr;
            for (int i = 0; i < SDL_NumJoysticks(); i++) {
                if (!SDL_IsGameController(i))
                    continue;
                // Never open the raw konkr-inputd sources (grabbed → no events);
                // the udev rule normally hides them, this guards the boot window.
                const char *nm = SDL_GameControllerNameForIndex(i);
                if (nm && (strstr(nm, "AYANEO MCU") || strstr(nm, "KONKR System")))
                    continue;
                gc = SDL_GameControllerOpen(i);
                break;
            }
            // Pin ImGui's gamepad nav to OUR controller. Its default AutoFirst
            // mode would otherwise grab the (event-less, grabbed) raw AYANEO and
            // navigation would silently do nothing.
            if (gc)
                ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, &gc, 1);
            else
                ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoFirst);
        }

        if (capturing())
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        battery_poll();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        // full-screen frame with flush header/footer bars: zero window padding
        // on the outer window, real padding restored inside the content child
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Device Controls", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float hdr_h = 60 * g_ui;
        const float ftr_h = 48 * g_ui;
        draw_header(vp->WorkSize.x, hdr_h);

        // content: the tab bar + active tab in its own scrollable region, so the
        // header/footer stay pinned (tab content e.g. the color picker at
        // handheld scale can exceed the screen height)
        ImGui::SetCursorPos(ImVec2(0, hdr_h));
        ImGui::BeginChild("##content", ImVec2(0, vp->WorkSize.y - hdr_h - ftr_h),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        if (ImGui::BeginTabBar("tabs")) {
            /* Probe device-specific hardware once: this image is shared across
             * SM8750 handhelds, so hide the tabs whose hardware is absent. */
            static const bool has_mcu = access(KEY_DIR, F_OK) == 0;
            static const bool has_rgb = access(LED_DIR, F_OK) == 0;
            static const bool has_imu = !find_iio_imu().empty();
            static const bool has_fan = !find_pwm().empty();

            if (ImGui::BeginTabItem("Gamepad")) {
                tab_gamepad(gc);
                ImGui::EndTabItem();
            }
            if (has_mcu && ImGui::BeginTabItem("Buttons")) {
                tab_buttons();
                ImGui::EndTabItem();
            }
            if (has_rgb && ImGui::BeginTabItem("RGB")) {
                tab_rgb();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                tab_audio();
                ImGui::EndTabItem();
            }
            if (has_fan && ImGui::BeginTabItem("Fan")) {
                tab_fan();
                ImGui::EndTabItem();
            }
            if (has_imu && ImGui::BeginTabItem("Motion")) {
                tab_motion();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Power")) {
                tab_power();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                tab_system();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                tab_display();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::SetCursorPos(ImVec2(0, vp->WorkSize.y - ftr_h));
        if (draw_footer(vp->WorkSize.x, ftr_h))
            quit = true;
        if (gc && !capturing() &&
            SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
            SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
            SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))
            quit = true;

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, (Uint8)(C_BG.x * 255), (Uint8)(C_BG.y * 255),
                               (Uint8)(C_BG.z * 255), 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);

        Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 16)
            SDL_Delay(16 - elapsed);
    }

    if (g_fan_manual)
        run_cmd("systemctl start fancontrol");
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
