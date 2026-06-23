// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)
//
// konkr-inputd — KONKR Pocket FIT Elite (SM8750) input authority.
//
// Grabs the two physical evdev sources that make up this device's controller
// and synthesizes a single virtual gamepad (+ a virtual keyboard) that the rest
// of the system consumes. By owning the grab we are the sole consumer of the
// raw devices, so button identity is unambiguous and fully under our control —
// no keycode collisions with real keyboards, no static capability-map YAML.
//
//   physical "AYANEO MCU Gamepad"   (mcu_joystick, SPI)   ─┐
//   physical "KONKR System Buttons" (konkr_sysbtn, UART)  ─┴─▶ konkr-inputd ─▶
//       virtual "Microsoft Xbox 360" pad  +  virtual keyboard
//
// The standard gamepad (sticks/triggers/dpad/face/shoulders) passes straight
// through. Six "extra" buttons run through the runtime remap engine: the
// gamepad's front-top-left (BTN_MODE) and the five KONKR system buttons
// (BTN_TRIGGER_HAPPY1..5). Each has a press (tap) action and an optional hold
// action. Defaults reproduce the historical inputplumber mapping:
//   FRONT_TL -> key F11   SYS_BR(HAPPY1) -> key F12
//   KONKR(HAPPY2) tap -> gamepad Guide   SYS3/4/5 -> key F13/F14/F15
//
// KONKR long-press (>=600 ms) is a RESERVED, non-rebindable opener for the
// device-controls sidebar, so a user can never remap themselves out of reach.
//
// The virtual gamepad clones the xb360 identity (BUS_USB 045e:028e v0001,
// "Microsoft Xbox 360") so the existing SDL gamecontrollerdb override applies
// without change. Force-feedback (FF_RUMBLE, memless on the MCU pad) is
// round-tripped: effects uploaded by consumers are replayed onto the MCU evdev.
//
// Runtime control is over a Unix SOCK_SEQPACKET socket (/run/konkr-inputd.sock).
// device-controls reads/edits bindings live and drives "intercept mode" (the
// sidebar gets gamepad nav while the game underneath is frozen). See protocol
// notes near handle_command().
//
// Robustness: sources are internal (SPI / UART serdev); the dynamic event that
// matters is suspend/resume, where the UART serdev can wedge or re-enumerate. A
// system-sleep hook pokes us with SIGUSR1 on resume to re-assert grabs; we also
// retry acquisition on a short timeout while a source slot is unfilled. The
// virtual devices persist for the daemon lifetime, so consumers never see the
// controller disconnect.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <systemd/sd-daemon.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define VPAD_FF_MAX   16
#define MAX_CLIENTS   4
#define OPENER_HOLD_MS 600

#define SOCK_PATH   "/run/konkr-inputd.sock"
#define CONFIG_DIR  "/storage/.config/konkr-inputd"
#define CONFIG_PATH CONFIG_DIR "/bindings.conf"

// ----------------------------------------------------------------- sources

struct source {
    const char *name;
    int fd;
    bool sysbtn;
};
static struct source sources[] = {
    { "AYANEO MCU Gamepad",   -1, false },
    { "KONKR System Buttons", -1, true  },
};
#define SRC_GAMEPAD (&sources[0])
#define SRC_SYSBTN  (&sources[1])

static int vpad_fd = -1;   // virtual gamepad (O_RDWR — receives FF requests)
static int vkbd_fd = -1;   // virtual keyboard
static int epfd    = -1;
static int sockfd  = -1;   // IPC listen socket

static int16_t ff_map[VPAD_FF_MAX];

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reconcile_needed = 0;

static bool intercept = false;      // sidebar open: divert nav, freeze game
static int  intercept_owner = -1;   // client fd that turned intercept on

// ------------------------------------------------------------- remap engine

enum action_type { ACT_NONE, ACT_GAMEPAD, ACT_KEY, ACT_COMMAND, ACT_SIDEBAR };

struct action {
    enum action_type type;
    int code;            // GAMEPAD: BTN_*, KEY: KEY_*
    char cmd[160];       // COMMAND
};

struct binding {
    struct action press; // tap / immediate
    bool has_hold;
    int  hold_ms;
    struct action hold;
};

// The six remappable "extra" inputs. Everything else on the gamepad passes
// through untouched.
enum si { SI_FRONT_TL, SI_SYS_BR, SI_KONKR, SI_SYS3, SI_SYS4, SI_SYS5, SI_COUNT };
struct si_info { const char *name; bool sysbtn; int code; };
static const struct si_info si_table[SI_COUNT] = {
    [SI_FRONT_TL] = { "FRONT_TL", false, BTN_MODE },
    [SI_SYS_BR]   = { "SYS_BR",   true,  BTN_TRIGGER_HAPPY1 },
    [SI_KONKR]    = { "KONKR",    true,  BTN_TRIGGER_HAPPY2 },
    [SI_SYS3]     = { "SYS3",     true,  BTN_TRIGGER_HAPPY3 },
    [SI_SYS4]     = { "SYS4",     true,  BTN_TRIGGER_HAPPY4 },
    [SI_SYS5]     = { "SYS5",     true,  BTN_TRIGGER_HAPPY5 },
};

static struct binding bindings[SI_COUNT];

struct si_state {
    bool pressed;
    bool deferred_pending;   // waiting to decide tap vs hold
    bool hold_fired;
    int64_t press_ms;
};
static struct si_state si_state[SI_COUNT];

// Sidebar opener: which of the six inputs toggles the sidebar, and whether on
// press or hold. Runtime-configurable (device-controls "Sidebar" tab). The
// opener's chosen trigger toggles the sidebar instead of running that button's
// normal binding; for hold-mode, a short tap still runs the normal binding.
static int  opener_si = SI_KONKR;
static bool opener_hold = true;

// Match a real Xbox 360 pad's button set EXACTLY. Crucially, do NOT declare
// BTN_TL2/BTN_TR2: a real xpad has no digital trigger buttons, and adding them
// shifts SDL's ascending-evdev-code button indices by two, which breaks the
// gamecontrollerdb mapping for everything from "back" onward. Triggers are the
// analog ABS_Z/ABS_RZ axes (lefttrigger/righttrigger).
static const int vpad_keys[] = {
    BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
    BTN_TL, BTN_TR,
    BTN_SELECT, BTN_START, BTN_MODE,
    BTN_THUMBL, BTN_THUMBR,
};
static const int vpad_abs[] = {
    ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y,
};
static const int vkbd_keys[] = {
    KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
    KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
    KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,
    KEY_BRIGHTNESSUP, KEY_BRIGHTNESSDOWN,
    KEY_PLAYPAUSE, KEY_NEXTSONG, KEY_PREVIOUSSONG,
};

static void on_term(int sig)   { (void)sig; running = 0; }
static void on_resume(int sig) { (void)sig; reconcile_needed = 1; }

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    if (fd < 0)
        return;
    struct input_event ev = { .type = type, .code = code, .value = value };
    ssize_t r = write(fd, &ev, sizeof ev);
    (void)r;
}

// ------------------------------------------------------------------ IPC out

static int clients[MAX_CLIENTS];

static void broadcast(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (len <= 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] >= 0)
            (void)!send(clients[i], buf, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void run_command(const char *cmd)
{
    if (!cmd[0])
        return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void open_sidebar(void)
{
    if (intercept)
        return;   // a sidebar already has the screen — don't stack another
    broadcast("EVENT sidebar_open reserved\n");
    // We run as a system service, below whatever compositor is up. If sway is
    // running, hand the sidebar its Wayland env (layer-shell overlay). If a game
    // is running under gamescope (sway stopped), hand it gamescope's overlay
    // XWayland (:0) instead — device-controls picks the matching backend.
    if (access("/var/run/0-runtime-dir/wayland-1", F_OK) == 0)
        run_command("XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 "
                    "/usr/bin/device-controls --sidebar");
    else if (access("/tmp/.X11-unix/X0", F_OK) == 0)
        run_command("XDG_RUNTIME_DIR=/var/run/0-runtime-dir DISPLAY=:0 "
                    "/usr/bin/device-controls --sidebar");
}

// Same input opens and closes: if a sidebar is up (it owns intercept), tell it
// to quit; otherwise launch one.
static void toggle_sidebar(void)
{
    if (intercept)
        broadcast("EVENT close\n");
    else
        open_sidebar();
}

// ----------------------------------------------------------- action engine

static void apply_action(const struct action *a, int value)
{
    switch (a->type) {
    case ACT_GAMEPAD:
        emit(vpad_fd, EV_KEY, (uint16_t)a->code, value);
        emit(vpad_fd, EV_SYN, SYN_REPORT, 0);
        break;
    case ACT_KEY:
        emit(vkbd_fd, EV_KEY, (uint16_t)a->code, value);
        emit(vkbd_fd, EV_SYN, SYN_REPORT, 0);
        break;
    case ACT_COMMAND:
        if (value)
            run_command(a->cmd);
        break;
    case ACT_SIDEBAR:
        if (value)
            open_sidebar();
        break;
    case ACT_NONE:
        break;
    }
}

static void apply_click(const struct action *a)
{
    apply_action(a, 1);
    apply_action(a, 0);
}

// EV_MSC raw control-id contract with the mcu_joystick / konkr_sysbtn drivers:
// each source event is EV_MSC/MSC_RAW with value = (id << 16) | (u16)(s16)value.
// The kernel devices expose no buttons/axes, so nothing classifies them as
// gamepads; we own all gamepad semantics here.
enum {
    MCU_ID_A = 0x01, MCU_ID_B, MCU_ID_X, MCU_ID_Y, MCU_ID_L1, MCU_ID_R1,
    MCU_ID_SELECT, MCU_ID_START, MCU_ID_MODE, MCU_ID_THUMBL, MCU_ID_THUMBR,
    MCU_ID_LX = 0x10, MCU_ID_LY, MCU_ID_RX, MCU_ID_RY, MCU_ID_LT, MCU_ID_RT,
    MCU_ID_HATX, MCU_ID_HATY,
    SYS_ID_1 = 0x20, SYS_ID_2, SYS_ID_3, SYS_ID_4, SYS_ID_5,
};

// Control ids that pass straight through to the virtual pad (everything except
// the six remappable inputs, which go to the engine — see route_msc).
static const struct { int id; uint16_t type; uint16_t code; } msc_outs[] = {
    { MCU_ID_A, EV_KEY, BTN_SOUTH }, { MCU_ID_B, EV_KEY, BTN_EAST },
    { MCU_ID_X, EV_KEY, BTN_NORTH }, { MCU_ID_Y, EV_KEY, BTN_WEST },
    { MCU_ID_L1, EV_KEY, BTN_TL },   { MCU_ID_R1, EV_KEY, BTN_TR },
    { MCU_ID_SELECT, EV_KEY, BTN_SELECT }, { MCU_ID_START, EV_KEY, BTN_START },
    { MCU_ID_THUMBL, EV_KEY, BTN_THUMBL }, { MCU_ID_THUMBR, EV_KEY, BTN_THUMBR },
    { MCU_ID_LX, EV_ABS, ABS_X },  { MCU_ID_LY, EV_ABS, ABS_Y },
    { MCU_ID_RX, EV_ABS, ABS_RX }, { MCU_ID_RY, EV_ABS, ABS_RY },
    { MCU_ID_LT, EV_ABS, ABS_Z },  { MCU_ID_RT, EV_ABS, ABS_RZ },
    { MCU_ID_HATX, EV_ABS, ABS_HAT0X }, { MCU_ID_HATY, EV_ABS, ABS_HAT0Y },
};

static void si_event(int i, int value)
{
    if (value == 2)   // ignore autorepeat
        return;
    struct binding *b = &bindings[i];
    struct si_state *st = &si_state[i];
    bool is_opener = (i == opener_si);

    // Opener-on-press: the press edge toggles the sidebar; consume it.
    if (is_opener && !opener_hold) {
        if (value == 1)
            toggle_sidebar();
        return;
    }

    // Opener-on-hold, or a user hold binding, defers to classify tap vs hold.
    bool deferred = b->has_hold || (is_opener && opener_hold);
    if (value == 1) {
        st->pressed = true;
        st->hold_fired = false;
        st->press_ms = now_ms();
        if (deferred)
            st->deferred_pending = true;
        else
            apply_action(&b->press, 1);
    } else {
        st->pressed = false;
        if (deferred) {
            if (st->deferred_pending && !st->hold_fired)
                apply_click(&b->press);    // short tap => normal press action
            st->deferred_pending = false;
        } else {
            apply_action(&b->press, 0);
        }
    }
}

// Fire hold actions for deferred buttons held past their threshold. Returns the
// nearest pending deadline in ms-from-now, or -1 if none pending.
static int check_holds(void)
{
    int64_t t = now_ms();
    int next = -1;
    for (int i = 0; i < SI_COUNT; i++) {
        struct si_state *st = &si_state[i];
        if (!st->deferred_pending || !st->pressed || st->hold_fired)
            continue;
        bool opener_byhold = (i == opener_si) && opener_hold;
        int ms = opener_byhold ? OPENER_HOLD_MS : bindings[i].hold_ms;
        int64_t remaining = (int64_t)ms - (t - st->press_ms);
        if (remaining <= 0) {
            if (opener_byhold) {
                toggle_sidebar();
            } else {
                const struct action *ha = &bindings[i].hold;
                if (ha->type == ACT_GAMEPAD || ha->type == ACT_KEY)
                    apply_click(ha);
                else
                    apply_action(ha, 1);
            }
            st->hold_fired = true;
            st->deferred_pending = false;
        } else if (next < 0 || remaining < next) {
            next = (int)remaining;
        }
    }
    return next;
}

// ----------------------------------------------------------- event routing

static bool pad_dirty;

static void flush_pad(void)
{
    if (pad_dirty) {
        emit(vpad_fd, EV_SYN, SYN_REPORT, 0);
        pad_dirty = false;
    }
}

// While the sidebar is open, gamepad nav is diverted to it and the game
// underneath is frozen. Translate the navigation-relevant outputs to events.
static void intercept_nav(uint16_t type, uint16_t code, int val)
{
    if (type == EV_ABS) {
        if (code == ABS_HAT0X) broadcast("EVENT input dpadx %d\n", val);
        else if (code == ABS_HAT0Y) broadcast("EVENT input dpady %d\n", val);
        return;   // sticks/triggers swallowed
    }
    switch (code) {
    case BTN_SOUTH: broadcast("EVENT input accept %d\n", val); break;
    case BTN_EAST:  broadcast("EVENT input cancel %d\n", val); break;
    case BTN_TL:    broadcast("EVENT input tabprev %d\n", val); break;
    case BTN_TR:    broadcast("EVENT input tabnext %d\n", val); break;
    default: break;   // swallow everything else
    }
}

// Decode one EV_MSC raw record from a source into the virtual pad / engine.
static void route_msc(int32_t packed)
{
    int id = (packed >> 16) & 0xff;
    int val = (int16_t)(packed & 0xffff);

    // The six remappable inputs go through the runtime engine.
    switch (id) {
    case MCU_ID_MODE: si_event(SI_FRONT_TL, val); return;
    case SYS_ID_1:    si_event(SI_SYS_BR, val);   return;
    case SYS_ID_2:    si_event(SI_KONKR, val);    return;
    case SYS_ID_3:    si_event(SI_SYS3, val);     return;
    case SYS_ID_4:    si_event(SI_SYS4, val);     return;
    case SYS_ID_5:    si_event(SI_SYS5, val);     return;
    }

    // Everything else is a standard pad control: pass through (or divert nav).
    for (size_t i = 0; i < ARRAY_SIZE(msc_outs); i++) {
        if (msc_outs[i].id != id)
            continue;
        if (intercept) {
            intercept_nav(msc_outs[i].type, msc_outs[i].code, val);
            return;
        }
        emit(vpad_fd, msc_outs[i].type, msc_outs[i].code, (int32_t)val);
        pad_dirty = true;
        return;
    }
}

static void route_event(const struct input_event *ev, bool from_sysbtn)
{
    (void)from_sysbtn;   // control ids are globally unique across both sources
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_REPORT)
            flush_pad();
    } else if (ev->type == EV_MSC && ev->code == MSC_RAW) {
        route_msc(ev->value);
    }
}

static bool drain_source(int fd, bool from_sysbtn)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(fd, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return true;
        size_t cnt = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < cnt; i++)
            route_event(&evs[i], from_sysbtn);
    }
}

// ------------------------------------------------------------- force feedback

static void ff_upload(uint32_t request_id)
{
    struct uinput_ff_upload up = { .request_id = request_id };
    if (ioctl(vpad_fd, UI_BEGIN_FF_UPLOAD, &up) < 0)
        return;
    int vid = up.effect.id;
    up.retval = -ENODEV;
    if (SRC_GAMEPAD->fd >= 0 && vid >= 0 && vid < VPAD_FF_MAX) {
        struct ff_effect eff = up.effect;
        eff.id = (ff_map[vid] >= 0) ? ff_map[vid] : -1;
        if (ioctl(SRC_GAMEPAD->fd, EVIOCSFF, &eff) == 0) {
            ff_map[vid] = (int16_t)eff.id;
            up.retval = 0;
        } else {
            up.retval = -errno;
        }
    }
    ioctl(vpad_fd, UI_END_FF_UPLOAD, &up);
}

static void ff_erase(uint32_t request_id)
{
    struct uinput_ff_erase er = { .request_id = request_id };
    if (ioctl(vpad_fd, UI_BEGIN_FF_ERASE, &er) < 0)
        return;
    int vid = er.effect_id;
    if (SRC_GAMEPAD->fd >= 0 && vid >= 0 && vid < VPAD_FF_MAX && ff_map[vid] >= 0) {
        ioctl(SRC_GAMEPAD->fd, EVIOCRMFF, (void *)(intptr_t)ff_map[vid]);
        ff_map[vid] = -1;
    }
    er.retval = 0;
    ioctl(vpad_fd, UI_END_FF_ERASE, &er);
}

static void drain_vpad(void)
{
    struct input_event evs[32];
    for (;;) {
        ssize_t n = read(vpad_fd, evs, sizeof evs);
        if (n <= 0)
            return;
        size_t cnt = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < cnt; i++) {
            struct input_event *ev = &evs[i];
            if (ev->type == EV_UINPUT) {
                if (ev->code == UI_FF_UPLOAD)
                    ff_upload((uint32_t)ev->value);
                else if (ev->code == UI_FF_ERASE)
                    ff_erase((uint32_t)ev->value);
            } else if (ev->type == EV_FF && SRC_GAMEPAD->fd >= 0) {
                if (ev->code == FF_GAIN || ev->code == FF_AUTOCENTER)
                    emit(SRC_GAMEPAD->fd, EV_FF, ev->code, ev->value);
                else if (ev->code < VPAD_FF_MAX && ff_map[ev->code] >= 0)
                    emit(SRC_GAMEPAD->fd, EV_FF, (uint16_t)ff_map[ev->code], ev->value);
            }
        }
    }
}

// ----------------------------------------------------------- device discovery

static int find_device_by_name(const char *want)
{
    DIR *d = opendir("/dev/input");
    if (!d)
        return -1;
    int found = -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[256] = { 0 };
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strcmp(name, want) == 0) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

static void ep_add(int fd) { struct epoll_event ee = { .events = EPOLLIN, .data.fd = fd }; epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ee); }
static void ep_del(int fd) { epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); }

static void drop_source(struct source *s)
{
    if (s->fd < 0)
        return;
    ep_del(s->fd);
    ioctl(s->fd, EVIOCGRAB, (void *)0);
    close(s->fd);
    s->fd = -1;
    fprintf(stderr, "konkr-inputd: lost '%s'\n", s->name);
}

static bool sources_grabbed = true;   // false while released for calibration

static void reconcile(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(sources); i++) {
        struct source *s = &sources[i];
        if (s->fd >= 0 && sources_grabbed && ioctl(s->fd, EVIOCGRAB, (void *)1) < 0)
            drop_source(s);
        if (s->fd < 0) {
            int fd = find_device_by_name(s->name);
            if (fd < 0)
                continue;
            if (sources_grabbed)
                ioctl(fd, EVIOCGRAB, (void *)1);
            s->fd = fd;
            ep_add(fd);
            if (!s->sysbtn)
                for (size_t k = 0; k < VPAD_FF_MAX; k++)
                    ff_map[k] = -1;
            fprintf(stderr, "konkr-inputd: acquired '%s'\n", s->name);
        }
    }
    broadcast("EVENT source_state %d %d\n", SRC_GAMEPAD->fd >= 0, SRC_SYSBTN->fd >= 0);
}

// Release / re-grab the physical sources without destroying the virtual pad —
// used by gamepad calibration, which needs to read the raw device.
static void set_grab(bool on)
{
    sources_grabbed = on;
    for (size_t i = 0; i < ARRAY_SIZE(sources); i++)
        if (sources[i].fd >= 0)
            ioctl(sources[i].fd, EVIOCGRAB, (void *)(intptr_t)(on ? 1 : 0));
}

// --------------------------------------------------------------- uinput sinks

static void default_absinfo(int code, struct input_absinfo *ai)
{
    *ai = (struct input_absinfo){ 0 };
    switch (code) {
    case ABS_X: case ABS_Y: case ABS_RX: case ABS_RY:
        ai->minimum = -32768; ai->maximum = 32767; ai->fuzz = 16; ai->flat = 128;
        break;
    case ABS_Z: case ABS_RZ:
        ai->minimum = 0; ai->maximum = 255;
        break;
    case ABS_HAT0X: case ABS_HAT0Y:
        ai->minimum = -1; ai->maximum = 1;
        break;
    default:
        break;
    }
}

static int setup_vpad(int src_fd)
{
    int fd = open("/dev/uinput", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < ARRAY_SIZE(vpad_keys); i++)
        ioctl(fd, UI_SET_KEYBIT, vpad_keys[i]);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (size_t i = 0; i < ARRAY_SIZE(vpad_abs); i++) {
        int code = vpad_abs[i];
        ioctl(fd, UI_SET_ABSBIT, code);
        struct uinput_abs_setup as = { .code = (uint16_t)code };
        if (src_fd < 0 || ioctl(src_fd, EVIOCGABS(code), &as.absinfo) < 0)
            default_absinfo(code, &as.absinfo);
        ioctl(fd, UI_ABS_SETUP, &as);
    }
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);

    struct uinput_setup us = {
        .id = { .bustype = BUS_USB, .vendor = 0x045E, .product = 0x028E, .version = 0x0001 },
        .ff_effects_max = VPAD_FF_MAX,
    };
    // Exact xpad/inputplumber name so SDL computes the same controller GUID and
    // the existing gamecontrollerdb mapping applies (don't "improve" this).
    strncpy(us.name, "Microsoft X-Box 360 pad", sizeof us.name - 1);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("uinput create (pad)");
        close(fd);
        return -1;
    }
    return fd;
}

static int setup_vkbd(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < ARRAY_SIZE(vkbd_keys); i++)
        ioctl(fd, UI_SET_KEYBIT, vkbd_keys[i]);
    struct uinput_setup us = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x4b4e, .version = 0x0001 },
    };
    strncpy(us.name, "KONKR Virtual Keyboard", sizeof us.name - 1);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("uinput create (kbd)");
        close(fd);
        return -1;
    }
    return fd;
}

// ------------------------------------------------------------ config + IPC

static int key_from_token(const char *t)   // KEY_* / BTN_* name or raw number
{
    static const struct { const char *n; int c; } names[] = {
        { "F11", KEY_F11 }, { "F12", KEY_F12 }, { "F13", KEY_F13 },
        { "F14", KEY_F14 }, { "F15", KEY_F15 }, { "F16", KEY_F16 },
        { "VOLUP", KEY_VOLUMEUP }, { "VOLDOWN", KEY_VOLUMEDOWN }, { "MUTE", KEY_MUTE },
        { "BRIGHTUP", KEY_BRIGHTNESSUP }, { "BRIGHTDOWN", KEY_BRIGHTNESSDOWN },
        { "GUIDE", BTN_MODE }, { "A", BTN_SOUTH }, { "B", BTN_EAST },
        { "X", BTN_NORTH }, { "Y", BTN_WEST }, { "START", BTN_START }, { "SELECT", BTN_SELECT },
    };
    for (size_t i = 0; i < ARRAY_SIZE(names); i++)
        if (strcmp(names[i].n, t) == 0)
            return names[i].c;
    return atoi(t);
}

// Parse "<kind> [arg...]" into an action. `rest` is the remainder of the line.
static bool parse_action(char *toks, struct action *out)
{
    *out = (struct action){ .type = ACT_NONE };
    char *save = NULL;
    char *kind = strtok_r(toks, " \t", &save);
    if (!kind || strcmp(kind, "none") == 0)
        return true;
    if (strcmp(kind, "sidebar") == 0) {
        out->type = ACT_SIDEBAR;
        return true;
    }
    if (strcmp(kind, "command") == 0) {
        char *rest = strtok_r(NULL, "", &save);
        out->type = ACT_COMMAND;
        if (rest) {
            while (*rest == ' ') rest++;
            strncpy(out->cmd, rest, sizeof out->cmd - 1);
        }
        return true;
    }
    char *arg = strtok_r(NULL, " \t", &save);
    if (!arg)
        return false;
    if (strcmp(kind, "key") == 0)
        out->type = ACT_KEY;
    else if (strcmp(kind, "gamepad") == 0)
        out->type = ACT_GAMEPAD;
    else
        return false;
    out->code = key_from_token(arg);
    return true;
}

static int si_from_name(const char *n)
{
    for (int i = 0; i < SI_COUNT; i++)
        if (strcmp(si_table[i].name, n) == 0)
            return i;
    return -1;
}

// "OPENER <SI> <press|hold>" — pick the sidebar opener. Returns NULL or error.
static const char *apply_opener(char *args)
{
    char *save = NULL;
    char *siname = strtok_r(args, " \t", &save);
    char *trig = strtok_r(NULL, " \t", &save);
    if (!siname || !trig)
        return "usage: SET_OPENER <SI> <press|hold>";
    int si = si_from_name(siname);
    if (si < 0)
        return "unknown SI";
    if (strcmp(trig, "hold") == 0)
        opener_hold = true;
    else if (strcmp(trig, "press") == 0)
        opener_hold = false;
    else
        return "trigger must be press or hold";
    opener_si = si;
    return NULL;
}

static void load_defaults(void)
{
    opener_si = SI_KONKR;
    opener_hold = true;    // KONKR long-press opens the settings overlay
    for (int i = 0; i < SI_COUNT; i++)
        bindings[i] = (struct binding){ .press = { .type = ACT_NONE } };
    bindings[SI_FRONT_TL].press = (struct action){ .type = ACT_KEY, .code = KEY_F11 };
    bindings[SI_SYS_BR].press   = (struct action){ .type = ACT_KEY, .code = KEY_F12 };
    bindings[SI_KONKR].press    = (struct action){ .type = ACT_GAMEPAD, .code = BTN_MODE };
    bindings[SI_SYS3].press     = (struct action){ .type = ACT_KEY, .code = KEY_F13 };
    bindings[SI_SYS4].press     = (struct action){ .type = ACT_KEY, .code = KEY_F14 };
    bindings[SI_SYS5].press     = (struct action){ .type = ACT_KEY, .code = KEY_F15 };
}

// Apply a "SET <SI> press|hold|clear ..." command. Returns NULL on success or an
// error string. Used both by IPC and by config-file load (same grammar).
static const char *apply_set(char *args)
{
    char *save = NULL;
    char *siname = strtok_r(args, " \t", &save);
    char *what = strtok_r(NULL, " \t", &save);
    if (!siname || !what)
        return "usage: SET <SI> press|hold|clear ...";
    int si = si_from_name(siname);
    if (si < 0)
        return "unknown SI";

    if (strcmp(what, "clear") == 0) {
        bindings[si] = (struct binding){ .press = { .type = ACT_NONE } };
        return NULL;
    }
    if (strcmp(what, "press") == 0) {
        char *rest = strtok_r(NULL, "", &save);
        struct action a;
        if (!rest || !parse_action(rest, &a))
            return "bad action";
        bindings[si].press = a;
        return NULL;
    }
    if (strcmp(what, "hold") == 0) {
        if (si == SI_KONKR)
            return "KONKR hold is reserved (sidebar opener)";
        char *mss = strtok_r(NULL, " \t", &save);
        char *rest = strtok_r(NULL, "", &save);
        struct action a;
        if (!mss || !rest || !parse_action(rest, &a))
            return "usage: SET <SI> hold <ms> <action>";
        bindings[si].has_hold = true;
        bindings[si].hold_ms = atoi(mss);
        bindings[si].hold = a;
        return NULL;
    }
    return "unknown subcommand";
}

static void action_str(const struct action *a, char *out, size_t n)
{
    switch (a->type) {
    case ACT_GAMEPAD: snprintf(out, n, "gamepad %d", a->code); break;
    case ACT_KEY:     snprintf(out, n, "key %d", a->code); break;
    case ACT_COMMAND: snprintf(out, n, "command %s", a->cmd); break;
    case ACT_SIDEBAR: snprintf(out, n, "sidebar"); break;
    default:          snprintf(out, n, "none"); break;
    }
}

static void save_config(void)
{
    mkdir("/storage/.config", 0755);
    mkdir(CONFIG_DIR, 0755);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "# konkr-inputd bindings (numeric codes are evdev BTN_*/KEY_* values)\n");
    fprintf(f, "OPENER %s %s\n", si_table[opener_si].name, opener_hold ? "hold" : "press");
    for (int i = 0; i < SI_COUNT; i++) {
        char a[200];
        action_str(&bindings[i].press, a, sizeof a);
        fprintf(f, "SET %s press %s\n", si_table[i].name, a);
        if (bindings[i].has_hold) {
            action_str(&bindings[i].hold, a, sizeof a);
            fprintf(f, "SET %s hold %d %s\n", si_table[i].name, bindings[i].hold_ms, a);
        }
    }
    fclose(f);
}

static void load_config(void)
{
    load_defaults();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == 0)
            continue;
        if (strncmp(line, "OPENER ", 7) == 0)
            apply_opener(line + 7);
        else if (strncmp(line, "SET ", 4) == 0)
            apply_set(line + 4);
    }
    fclose(f);
}

// IPC protocol (one command per SOCK_SEQPACKET message, reply same way):
//   GET                         -> SET... lines, then "END"
//   SET <SI> press <action>     -> "OK" | "ERR <msg>"
//   SET <SI> hold <ms> <action> -> "OK" | "ERR <msg>"
//   SET <SI> clear              -> "OK"
//   RELOAD | SAVE | RESET       -> "OK"
//   INTERCEPT <0|1>             -> "OK"
//   GRAB <0|1>                  -> "OK"   (release/re-grab for calibration)
//   PING                        -> "PONG"
// action = none | sidebar | key <code> | gamepad <code> | command <str...>
static void handle_command(int cfd, char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, "PING") == 0) {
        send(cfd, "PONG\n", 5, MSG_NOSIGNAL);
        return;
    }
    if (strcmp(line, "GET") == 0) {
        char ohdr[128];
        int ol = snprintf(ohdr, sizeof ohdr, "OPENER %s %s\n",
                          si_table[opener_si].name, opener_hold ? "hold" : "press");
        send(cfd, ohdr, (size_t)ol, MSG_NOSIGNAL);
        for (int i = 0; i < SI_COUNT; i++) {
            char a[200], msg[256];
            action_str(&bindings[i].press, a, sizeof a);
            int l = snprintf(msg, sizeof msg, "SET %s press %s\n", si_table[i].name, a);
            send(cfd, msg, (size_t)l, MSG_NOSIGNAL);
            if (bindings[i].has_hold) {
                action_str(&bindings[i].hold, a, sizeof a);
                l = snprintf(msg, sizeof msg, "SET %s hold %d %s\n", si_table[i].name, bindings[i].hold_ms, a);
                send(cfd, msg, (size_t)l, MSG_NOSIGNAL);
            }
        }
        send(cfd, "END\n", 4, MSG_NOSIGNAL);
        return;
    }
    if (strncmp(line, "SET_OPENER ", 11) == 0) {
        const char *err = apply_opener(line + 11);
        if (err) {
            char msg[200];
            int l = snprintf(msg, sizeof msg, "ERR %s\n", err);
            send(cfd, msg, (size_t)l, MSG_NOSIGNAL);
        } else {
            save_config();
            send(cfd, "OK\n", 3, MSG_NOSIGNAL);
        }
        return;
    }
    if (strncmp(line, "SET ", 4) == 0) {
        const char *err = apply_set(line + 4);
        if (err) {
            char msg[200];
            int l = snprintf(msg, sizeof msg, "ERR %s\n", err);
            send(cfd, msg, (size_t)l, MSG_NOSIGNAL);
        } else {
            save_config();
            send(cfd, "OK\n", 3, MSG_NOSIGNAL);
        }
        return;
    }
    if (strcmp(line, "RELOAD") == 0) { load_config(); send(cfd, "OK\n", 3, MSG_NOSIGNAL); return; }
    if (strcmp(line, "SAVE") == 0)   { save_config(); send(cfd, "OK\n", 3, MSG_NOSIGNAL); return; }
    if (strcmp(line, "RESET") == 0)  { load_defaults(); save_config(); send(cfd, "OK\n", 3, MSG_NOSIGNAL); return; }
    if (strncmp(line, "INTERCEPT ", 10) == 0) {
        intercept = atoi(line + 10) != 0;
        intercept_owner = intercept ? cfd : -1;
        if (!intercept)
            flush_pad();
        send(cfd, "OK\n", 3, MSG_NOSIGNAL);
        return;
    }
    if (strncmp(line, "GRAB ", 5) == 0) { set_grab(atoi(line + 5) != 0); send(cfd, "OK\n", 3, MSG_NOSIGNAL); return; }
    send(cfd, "ERR unknown\n", 12, MSG_NOSIGNAL);
}

static int setup_socket(void)
{
    unlink(SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 4) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    chmod(SOCK_PATH, 0660);
    return fd;
}

static void client_close(int idx)
{
    int fd = clients[idx];
    if (fd < 0)
        return;
    if (fd == intercept_owner) {   // failsafe: restore game input
        intercept = false;
        intercept_owner = -1;
        flush_pad();
    }
    ep_del(fd);
    close(fd);
    clients[idx] = -1;
}

static void handle_client(int fd)
{
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) {
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] == fd) { client_close(i); break; }
        return;
    }
    buf[n] = 0;
    handle_command(fd, buf);
}

static void accept_client(void)
{
    int cfd = accept4(sockfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] < 0) {
            clients[i] = cfd;
            ep_add(cfd);
            return;
        }
    }
    close(cfd);   // table full
}

static bool is_client(int fd) { for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] == fd) return true; return false; }

// --------------------------------------------------------------------- main

// Connect to a running daemon and send one command (for CLI helpers like
// --grab, used by gamepad calibration). Prints the reply. Returns 0 on success.
static int client_send(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "konkr-inputd: daemon not running\n");
        close(fd);
        return 1;
    }
    send(fd, cmd, strlen(cmd), MSG_NOSIGNAL);
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n > 0) { buf[n] = 0; fputs(buf, stdout); }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--reset-config") == 0) {
        load_defaults();
        save_config();
        printf("konkr-inputd: bindings reset to defaults\n");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--grab") == 0) {
        char cmd[16];
        snprintf(cmd, sizeof cmd, "GRAB %d", atoi(argv[2]) != 0);
        return client_send(cmd);
    }

    struct sigaction term = { .sa_handler = on_term };
    sigaction(SIGINT, &term, NULL);
    sigaction(SIGTERM, &term, NULL);
    struct sigaction usr = { .sa_handler = on_resume };
    sigaction(SIGUSR1, &usr, NULL);
    signal(SIGCHLD, SIG_IGN);   // reap RUN_COMMAND children automatically

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i] = -1;
    for (size_t i = 0; i < VPAD_FF_MAX; i++)
        ff_map[i] = -1;

    load_config();

    for (size_t i = 0; i < ARRAY_SIZE(sources); i++) {
        int fd = find_device_by_name(sources[i].name);
        if (fd >= 0) {
            ioctl(fd, EVIOCGRAB, (void *)1);
            sources[i].fd = fd;
        } else {
            fprintf(stderr, "konkr-inputd: '%s' not present yet (will retry)\n", sources[i].name);
        }
    }

    vpad_fd = setup_vpad(SRC_GAMEPAD->fd);
    vkbd_fd = setup_vkbd();
    sockfd  = setup_socket();
    if (vpad_fd < 0 || vkbd_fd < 0) {
        fprintf(stderr, "konkr-inputd: failed to create virtual devices\n");
        return 1;
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(sources); i++)
        if (sources[i].fd >= 0)
            ep_add(sources[i].fd);
    ep_add(vpad_fd);
    if (sockfd >= 0)
        ep_add(sockfd);

    sd_notify(0, "READY=1");

    while (running) {
        if (reconcile_needed) {
            reconcile_needed = 0;
            reconcile();
        }
        int hold_to = check_holds();
        bool any_missing = SRC_GAMEPAD->fd < 0 || SRC_SYSBTN->fd < 0;
        int timeout = -1;
        if (hold_to >= 0)
            timeout = hold_to;
        if (any_missing && (timeout < 0 || timeout > 1000))
            timeout = 1000;

        struct epoll_event evs[8];
        int n = epoll_wait(epfd, evs, ARRAY_SIZE(evs), timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        if (n == 0) {
            if (any_missing)
                reconcile();
            continue;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == vpad_fd) {
                drain_vpad();
            } else if (fd == sockfd) {
                accept_client();
            } else if (is_client(fd)) {
                handle_client(fd);
            } else {
                struct source *s = (fd == SRC_SYSBTN->fd) ? SRC_SYSBTN : SRC_GAMEPAD;
                if (!drain_source(fd, s->sysbtn))
                    drop_source(s);
            }
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(sources); i++)
        if (sources[i].fd >= 0)
            ioctl(sources[i].fd, EVIOCGRAB, (void *)0);
    ioctl(vpad_fd, UI_DEV_DESTROY);
    ioctl(vkbd_fd, UI_DEV_DESTROY);
    unlink(SOCK_PATH);
    return 0;
}
