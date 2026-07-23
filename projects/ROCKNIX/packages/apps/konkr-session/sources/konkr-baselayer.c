// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)
//
// konkr-baselayer: makes non-Steam apps (emulators, PortMaster ports, ES, Tools
// apps) visible under the gamescope -e session.
//
// gamescope's Steam-integration mode (-e) only gives focus to a window whose
// Steam appid (the STEAM_GAME X property) is listed in the root window's
// GAMESCOPECTRL_BASELAYER_APPID; a window with appID==0 is not even focusable
// (steamcompmgr excludes it). Steam and Steam-launched games set STEAM_GAME
// themselves, and konkr-launcher / device-controls set it on their own windows,
// but the dozens of emulator / port / tool launch paths do not -- so they render
// invisibly (black) under -e.
//
// Rather than patch every launch script, this session daemon stamps a shared
// synthetic appid (0xFFFF0001, matching konkr-launcher's gamescope_register_baselayer)
// onto every non-Steam toplevel as it appears. Windows that already carry a
// STEAM_GAME (Steam, games, the self-registering frontend) are left untouched, so
// Steam's own focus stack is never disturbed. gamescope prefers the highest
// map_sequence among windows sharing an appid, so the newest such window (the
// emulator just launched) is shown, and the frontend reappears when it exits --
// exactly the launcher <-> device-controls behaviour, now automatic for all.
//
// It also seeds the root baselayer appid so a frontend that does not self-register
// (EmulationStation) still shows at boot; Steam overrides the baselayer while it
// runs and the launcher re-asserts it on the next start, so seeding once here does
// not fight Steam.
//
// Started by konkr-session-client for the lifetime of the session; targets the
// primary Xwayland ($DISPLAY, :0) where the frontend and emulators live.

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>

static const long KONKR_BASELAYER_APPID = 0xFFFF0001L;
static Atom g_steam_game;
static int g_debug;   // KONKR_BASELAYER_DEBUG=1 -> log each window considered

// True if the window already advertises a STEAM_GAME appid (Steam UI, a
// Steam-launched game, or a self-registering frontend) -- leave those alone.
static int has_steam_game(Display *dpy, Window w)
{
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(dpy, w, g_steam_game, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &n, &after, &data) == Success) {
        ok = (type == XA_CARDINAL && n >= 1);
        if (data)
            XFree(data);
    }
    return ok;
}

// Tag a genuine, un-tagged toplevel with the shared baselayer appid so gamescope
// -e will focus it. Skip override-redirect / non-drawable windows (menus,
// dropdowns, gamescope's own overlay planes) -- gamescope excludes those from
// focus anyway, and tagging them is pointless.
static void stamp(Display *dpy, Window w)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a))
        return;
    if (a.override_redirect || a.class != InputOutput)
        return;
    int already = has_steam_game(dpy, w);
    if (g_debug) {
        char *name = NULL;
        XFetchName(dpy, w, &name);
        fprintf(stderr, "konkr-baselayer: win 0x%lx \"%s\" %s\n", w,
                name ? name : "", already ? "already-tagged(skip)" : "STAMP");
        if (name)
            XFree(name);
    }
    if (already)
        return;
    XChangeProperty(dpy, w, g_steam_game, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&KONKR_BASELAYER_APPID, 1);
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);   // $DISPLAY -> the session's :0
    if (!dpy) {
        fprintf(stderr, "konkr-baselayer: cannot open X display\n");
        return 1;
    }

    g_debug = getenv("KONKR_BASELAYER_DEBUG") != NULL;
    g_steam_game = XInternAtom(dpy, "STEAM_GAME", False);
    Atom baselayer = XInternAtom(dpy, "GAMESCOPECTRL_BASELAYER_APPID", False);
    Window root = DefaultRootWindow(dpy);

    // Seed the baselayer so stamped windows are focusable (harmless: konkr-launcher
    // sets the same value, and Steam overrides it while running).
    XChangeProperty(dpy, root, baselayer, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&KONKR_BASELAYER_APPID, 1);

    // Watch for new toplevels, then tag whatever is already mapped (order avoids a
    // gap: an existing window is caught by the walk, a new one by the event).
    XSelectInput(dpy, root, SubstructureNotifyMask);

    Window r = 0, parent = 0, *kids = NULL;
    unsigned int nkids = 0;
    if (XQueryTree(dpy, root, &r, &parent, &kids, &nkids)) {
        for (unsigned int i = 0; i < nkids; i++)
            stamp(dpy, kids[i]);
        if (kids)
            XFree(kids);
    }
    XFlush(dpy);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);   // blocks; returns on X error/EOF -> we exit, supervisor restarts us
        if (ev.type == MapNotify)
            stamp(dpy, ev.xmap.window);
        else if (ev.type == CreateNotify)
            stamp(dpy, ev.xcreatewindow.window);
        XFlush(dpy);
    }
}
