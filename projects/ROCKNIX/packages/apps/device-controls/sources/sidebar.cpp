// SPDX-License-Identifier: GPL-2.0
// device-controls --sidebar — Android-style quick-settings overlay.
//
// A separate windowing backend from the SDL2 control panel: a Wayland
// zwlr_layer_shell_v1 OVERLAY surface (renders above a fullscreen game) drawn
// with EGL/GLES2 + Dear ImGui. Launched by konkr-inputd's reserved KONKR
// long-press. Gamepad navigation arrives over the daemon socket in "intercept
// mode" (the game underneath is frozen); touch works directly.
//
// Only works under sway (the normal session). Steam's gamescope path stops
// sway, so there is no Wayland compositor to attach to — we detect that and
// exit cleanly (KONKR=Guide opens Steam's own menu there).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

// The generated client header names a request argument "namespace" (a C++
// keyword); rename it for the duration of this include only.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include "imgui.h"
#include "imgui_impl_opengl3.h"

// ----------------------------------------------------------- small helpers

static bool sb_read(const std::string &p, char *buf, size_t n)
{
    FILE *f = fopen(p.c_str(), "r");
    if (!f)
        return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return true;
}
static void sb_write(const std::string &p, const std::string &v)
{
    FILE *f = fopen(p.c_str(), "w");
    if (f) { fputs(v.c_str(), f); fclose(f); }
}
static void sb_run(const char *cmd) { if (system(cmd) == -1) {} }

// ---------------------------------------------------------- daemon socket

static int g_sock = -1;

static void daemon_connect()
{
    g_sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_sock < 0)
        return;
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/run/konkr-inputd.sock", sizeof a.sun_path - 1);
    if (connect(g_sock, (sockaddr *)&a, sizeof a) < 0) {
        close(g_sock);
        g_sock = -1;
    }
}
static void daemon_send(const char *s)
{
    if (g_sock >= 0)
        (void)!send(g_sock, s, strlen(s), MSG_NOSIGNAL);
}

// --------------------------------------------------------------- wayland

static wl_display *g_dpy;
static wl_compositor *g_comp;
static zwlr_layer_shell_v1 *g_layer_shell;
static wl_seat *g_seat;
static wl_pointer *g_pointer;
static wl_touch *g_touch;
static wl_surface *g_surf;
static zwlr_layer_surface_v1 *g_layer_surf;
static wl_egl_window *g_egl_win;
static EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
static EGLSurface g_egl_surf = EGL_NO_SURFACE;
static EGLContext g_egl_ctx = EGL_NO_CONTEXT;

static int  g_w = 900, g_h = 720;   // wide enough for the tab bar + settings
static bool g_configured = false;
static bool g_quit = false;
static double g_mx = 0, g_my = 0;

// ---- pointer ----
static void ptr_enter(void *, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x, wl_fixed_t y)
{ g_mx = wl_fixed_to_double(x); g_my = wl_fixed_to_double(y); }
static void ptr_leave(void *, wl_pointer *, uint32_t, wl_surface *) {}
static void ptr_motion(void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    g_mx = wl_fixed_to_double(x); g_my = wl_fixed_to_double(y);
    ImGui::GetIO().AddMousePosEvent((float)g_mx, (float)g_my);
}
static void ptr_button(void *, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state)
{
    if (button == 0x110 /*BTN_LEFT*/)
        ImGui::GetIO().AddMouseButtonEvent(0, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
static void ptr_axis(void *, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value)
{
    float v = (float)wl_fixed_to_double(value) / -10.0f;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        ImGui::GetIO().AddMouseWheelEvent(0, v);
}
static void ptr_frame(void *, wl_pointer *) {}
static void ptr_axis_src(void *, wl_pointer *, uint32_t) {}
static void ptr_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
static void ptr_axis_disc(void *, wl_pointer *, uint32_t, int32_t) {}
static const wl_pointer_listener ptr_listener = {
    ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis,
    ptr_frame, ptr_axis_src, ptr_axis_stop, ptr_axis_disc, nullptr, nullptr,
};

// ---- touch (mapped to the mouse for ImGui) ----
static void touch_down(void *, wl_touch *, uint32_t, uint32_t, wl_surface *, int32_t,
                       wl_fixed_t x, wl_fixed_t y)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y));
    io.AddMouseButtonEvent(0, true);
}
static void touch_up(void *, wl_touch *, uint32_t, uint32_t, int32_t)
{ ImGui::GetIO().AddMouseButtonEvent(0, false); }
static void touch_motion(void *, wl_touch *, uint32_t, int32_t, wl_fixed_t x, wl_fixed_t y)
{ ImGui::GetIO().AddMousePosEvent((float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y)); }
static void touch_frame(void *, wl_touch *) {}
static void touch_cancel(void *, wl_touch *) {}
static void touch_shape(void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) {}
static void touch_orient(void *, wl_touch *, int32_t, wl_fixed_t) {}
static const wl_touch_listener touch_listener = {
    touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orient,
};

static void seat_caps(void *, wl_seat *seat, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_pointer) {
        g_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer, &ptr_listener, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !g_touch) {
        g_touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(g_touch, &touch_listener, nullptr);
    }
}
static void seat_name(void *, wl_seat *, const char *) {}
static const wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, wl_compositor_interface.name))
        g_comp = (wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        g_layer_shell = (zwlr_layer_shell_v1 *)wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(g_seat, &seat_listener, nullptr);
    }
}
static void reg_remove(void *, wl_registry *, uint32_t) {}
static const wl_registry_listener reg_listener = { reg_global, reg_remove };

static void ls_configure(void *, zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h)
{
    zwlr_layer_surface_v1_ack_configure(s, serial);
    if (w > 0) g_w = (int)w;
    if (h > 0) g_h = (int)h;
    if (g_egl_win)
        wl_egl_window_resize(g_egl_win, g_w, g_h, 0, 0);
    g_configured = true;
}
static void ls_closed(void *, zwlr_layer_surface_v1 *) { g_quit = true; }
static const zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

// ----------------------------------------------------- daemon nav events

static void read_daemon_events()
{
    char buf[512];
    for (;;) {
        ssize_t n = recv(g_sock, buf, sizeof buf - 1, 0);
        if (n <= 0)
            break;
        buf[n] = 0;
        if (strncmp(buf, "EVENT close", 11) == 0) {   // opener pressed again
            g_quit = true;
            continue;
        }
        ImGuiIO &io = ImGui::GetIO();
        char dir[32];
        int v;
        if (sscanf(buf, "EVENT input %31s %d", dir, &v) == 2) {
            if (!strcmp(dir, "dpady")) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, v < 0);
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, v > 0);
            } else if (!strcmp(dir, "dpadx")) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, v < 0);
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, v > 0);
            } else if (!strcmp(dir, "accept")) {
                io.AddKeyEvent(ImGuiKey_GamepadFaceDown, v != 0);
            } else if (!strcmp(dir, "cancel")) {
                // B = ImGui nav "back" (exit a combo/widget), NOT close. Close
                // via the Close tab entry or by pressing the opener again.
                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, v != 0);
            }
        }
    }
}

// --------------------------------------------------------- quick controls

static int  get_brightness_pct();
static void set_brightness_pct(int pct);
static int  get_volume_pct();
static void set_volume_pct(int pct);

static std::string g_bl_dir;   // resolved /sys/class/backlight/<dev>

static void resolve_backlight()
{
    // first entry under /sys/class/backlight
    FILE *p = popen("ls -1 /sys/class/backlight 2>/dev/null | head -1", "r");
    if (!p)
        return;
    char name[64] = {};
    if (fgets(name, sizeof name, p)) {
        name[strcspn(name, "\r\n")] = 0;
        if (name[0])
            g_bl_dir = std::string("/sys/class/backlight/") + name;
    }
    pclose(p);
}
static int get_brightness_pct()
{
    if (g_bl_dir.empty())
        return -1;
    char cur[16], max[16];
    if (!sb_read(g_bl_dir + "/brightness", cur, sizeof cur) ||
        !sb_read(g_bl_dir + "/max_brightness", max, sizeof max))
        return -1;
    int c = atoi(cur), m = atoi(max);
    return m > 0 ? (c * 100 + m / 2) / m : -1;
}
static void set_brightness_pct(int pct)
{
    if (g_bl_dir.empty())
        return;
    char max[16];
    if (!sb_read(g_bl_dir + "/max_brightness", max, sizeof max))
        return;
    int m = atoi(max);
    int v = m * pct / 100;
    if (v < 1) v = 1;
    sb_write(g_bl_dir + "/brightness", std::to_string(v));
}
static int get_volume_pct()
{
    FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (!p)
        return -1;
    char buf[64] = {};
    int pct = -1;
    if (fgets(buf, sizeof buf, p)) {
        float v = 0;
        if (sscanf(buf, "Volume: %f", &v) == 1)
            pct = (int)(v * 100 + 0.5f);
    }
    pclose(p);
    return pct;
}
static void set_volume_pct(int pct)
{
    char cmd[96];
    snprintf(cmd, sizeof cmd, "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", pct);
    sb_run(cmd);
}

// ------------------------------------------------------------- ImGui draw

// Tabs reused from the main control panel (main.cpp) — they only touch ImGui +
// sysfs + the konkr-inputd socket, so they work inside the sidebar process too.
void tab_buttons();
void tab_fan();
void tab_rgb();
void tab_power();

static void draw_quick()
{
    static int vol = -2, bright = -2;
    if (vol == -2) vol = get_volume_pct();
    if (bright == -2) bright = get_brightness_pct();
    if (vol >= 0) {
        ImGui::Text("Volume");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##vol", &vol, 0, 100, "%d%%"))
            set_volume_pct(vol);
    }
    if (bright >= 0) {
        ImGui::Text("Brightness");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##bri", &bright, 5, 100, "%d%%"))
            set_brightness_pct(bright);
    }
}

static void draw_ui(int uw, int uh)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)uw, (float)uh));
    ImGui::Begin("konkr-sidebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    static const char *names[] = { "Quick", "Power", "Buttons", "Fan", "RGB" };
    static int tab = 0;
    static bool enter_content = false;   // tab bar -> content, handed this frame
    static bool enter_tabbar = false;    // content -> tab bar, handed next frame
    float row = ImGui::GetFontSize() * 1.9f;

    // Left vertical tab bar (gamepad-navigable), plus Close at the bottom.
    ImGui::BeginChild("tabbar", ImVec2(ImGui::GetFontSize() * 7.0f, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    bool tabbar_focused = false;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(*names)); i++) {
        if (enter_tabbar && tab == i)
            ImGui::SetKeyboardFocusHere();   // return focus to the active tab
        if (ImGui::Selectable(names[i], tab == i, 0, ImVec2(0, row)))
            tab = i;
        if (ImGui::IsItemFocused())
            tabbar_focused = true;
    }
    ImGui::Dummy(ImVec2(0, row));
    if (ImGui::Selectable("Close", false, 0, ImVec2(0, row)))
        g_quit = true;
    if (ImGui::IsItemFocused())
        tabbar_focused = true;
    ImGui::EndChild();
    enter_tabbar = false;

    // D-pad Right crosses from the tab bar into the tab's content. Geometric
    // nav alone can miss it (no content widget lines up with the focused tab
    // row), so hand focus over explicitly. D-pad Left does the reverse.
    if (tabbar_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false))
        enter_content = true;
    if (!tabbar_focused && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false))
        enter_tabbar = true;

    ImGui::SameLine();
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    if (enter_content) {
        ImGui::SetKeyboardFocusHere();   // focus the first widget in the tab
        enter_content = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 14));
    switch (tab) {
    case 0: draw_quick();  break;
    case 1: tab_power();   break;
    case 2: tab_buttons(); break;
    case 3: tab_fan();     break;
    case 4: tab_rgb();     break;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::End();
}

// ----------------------------------------------------------------- run

static bool egl_setup()
{
    g_egl_dpy = eglGetDisplay((EGLNativeDisplayType)g_dpy);
    if (g_egl_dpy == EGL_NO_DISPLAY || !eglInitialize(g_egl_dpy, nullptr, nullptr))
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint num = 0;
    if (!eglChooseConfig(g_egl_dpy, cfg_attr, &cfg, 1, &num) || num < 1)
        return false;
    g_egl_win = wl_egl_window_create(g_surf, g_w, g_h);
    g_egl_surf = eglCreateWindowSurface(g_egl_dpy, cfg, (EGLNativeWindowType)g_egl_win, nullptr);
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_egl_ctx = eglCreateContext(g_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_egl_surf == EGL_NO_SURFACE || g_egl_ctx == EGL_NO_CONTEXT)
        return false;
    return eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx);
}

// Sway path: a wlr-layer-shell OVERLAY surface (EGL/GLES2).
static int run_sidebar_wayland()
{
    g_dpy = wl_display_connect(nullptr);
    if (!g_dpy) {
        fprintf(stderr, "sidebar: cannot connect to Wayland (gamescope session?)\n");
        return 1;
    }
    wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &reg_listener, nullptr);
    wl_display_roundtrip(g_dpy);   // bind globals
    wl_display_roundtrip(g_dpy);   // seat caps

    if (!g_comp || !g_layer_shell) {
        fprintf(stderr, "sidebar: compositor lacks layer-shell (not sway?)\n");
        return 1;
    }

    g_surf = wl_compositor_create_surface(g_comp);
    g_layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surf, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "konkr-sidebar");
    zwlr_layer_surface_v1_add_listener(g_layer_surf, &ls_listener, nullptr);
    zwlr_layer_surface_v1_set_anchor(g_layer_surf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(g_layer_surf, g_w, 0);   // height stretches
    zwlr_layer_surface_v1_set_exclusive_zone(g_layer_surf, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(g_layer_surf, 0);
    wl_surface_commit(g_surf);
    wl_display_roundtrip(g_dpy);   // wait for first configure
    if (!g_configured || !egl_setup()) {
        fprintf(stderr, "sidebar: EGL/surface setup failed\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.92f);  // widget metrics (padding/spacing)
    io.FontGlobalScale = 1.92f;              // text size
    ImGui_ImplOpenGL3_Init(nullptr);         // GLES2 (IMGUI_IMPL_OPENGL_ES2)

    resolve_backlight();
    daemon_connect();
    daemon_send("INTERCEPT 1");

    int wl_fd = wl_display_get_fd(g_dpy);
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (!g_quit) {
        wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);

        struct pollfd pfds[2] = {
            { wl_fd, POLLIN, 0 },
            { g_sock, (short)(g_sock >= 0 ? POLLIN : 0), 0 },
        };
        poll(pfds, 2, 16);
        if (pfds[0].revents & POLLIN)
            wl_display_dispatch(g_dpy);
        if (g_sock >= 0 && (pfds[1].revents & POLLIN))
            read_daemon_events();

        struct timespec nowt;
        clock_gettime(CLOCK_MONOTONIC, &nowt);
        float dt = (nowt.tv_sec - last.tv_sec) + (nowt.tv_nsec - last.tv_nsec) / 1e9f;
        last = nowt;
        io.DisplaySize = ImVec2((float)g_w, (float)g_h);
        io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        draw_ui(g_w, g_h);
        ImGui::Render();

        glViewport(0, 0, g_w, g_h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(g_egl_dpy, g_egl_surf);
    }

    daemon_send("INTERCEPT 0");
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    if (g_sock >= 0)
        close(g_sock);
    return 0;
}

// ---- gamescope path: SDL2 window on the overlay XWayland (:0) marked as the
// GAMESCOPE_EXTERNAL_OVERLAY, composited over the game. Nav comes from the
// daemon's intercept mode (gamescope owns input; the overlay is passive). ----
// X11/SDL headers go here, after all the Wayland code, so X11's macros (None,
// Bool, ...) can't clash with it.
#include <SDL.h>
#include <SDL_syswm.h>
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>

static void x11_mark_overlay(SDL_Window *win)
{
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_X11)
        return;
    Display *xd = wm.info.x11.display;
    Window xw = wm.info.x11.window;
    unsigned long one = 1, opaque = 0xffffffffUL;
    XChangeProperty(xd, xw, XInternAtom(xd, "GAMESCOPE_EXTERNAL_OVERLAY", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(xd, xw, XInternAtom(xd, "_NET_WM_WINDOW_OPACITY", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&opaque, 1);
    XFlush(xd);
}

static int run_sidebar_x11()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "sidebar(x11): SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DisplayMode dm;
    int sw = 1920, sh = 1080;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { sw = dm.w; sh = dm.h; }
    int w = (int)(sw * 0.50f);   // room for the tab bar + settings widgets
    if (w < 640)
        w = 640;
    // Placed at x=0: gamescope's --force-orientation rotation put a right-edge
    // window on the physical left, so anchor at the origin instead.
    SDL_Window *win = SDL_CreateWindow("konkr-sidebar", 0, 0, w, sh,
                                       SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "sidebar(x11): window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
                            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    x11_mark_overlay(win);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.92f);
    io.FontGlobalScale = 1.92f;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    // Don't let imgui_impl_sdl2 poll a controller — gamescope gives the overlay
    // no input focus, and its default gamepad poll would overwrite (with "all
    // released") the nav events we feed from the daemon's intercept every frame.
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);

    resolve_backlight();
    daemon_connect();
    daemon_send("INTERCEPT 1");

    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT)
                g_quit = true;
        }
        if (g_sock >= 0) {
            struct pollfd pfd = { g_sock, POLLIN, 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
                read_daemon_events();   // gamepad nav from the daemon intercept
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        // imgui_impl_sdl2 clears HasGamepad each frame (we run it with no SDL
        // pad); re-assert it so ImGui nav actually consumes the dpad/A events we
        // feed from the daemon's intercept.
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        ImGui::NewFrame();
        draw_ui(w, sh);
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    daemon_send("INTERCEPT 0");
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_sock >= 0)
        close(g_sock);
    return 0;
}

// Entry point: one sidebar at a time; pick the backend for the live session.
int run_sidebar()
{
    int lock_fd = open("/run/konkr-sidebar.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (lock_fd >= 0)
            close(lock_fd);
        return 0;   // another sidebar already owns the lock
    }
    if (getenv("WAYLAND_DISPLAY"))
        return run_sidebar_wayland();   // sway
    if (getenv("DISPLAY"))
        return run_sidebar_x11();       // gamescope overlay XWayland
    fprintf(stderr, "sidebar: no Wayland or X11 session\n");
    return 1;
}
