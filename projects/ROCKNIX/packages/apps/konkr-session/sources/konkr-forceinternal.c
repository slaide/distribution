// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)
//
// konkr-forceinternal <0|1> — set GAMESCOPE_DISPLAY_FORCE_INTERNAL on the :0
// root window of the persistent gamescope session (konkr-session).
//
// This is gamescope's own runtime output-preference switch (the SteamOS
// "internal display only" mechanism): steamcompmgr watches the property,
// updates g_bForceInternal and dirties the backend, and the DRM connector
// picker re-runs live — scanout migrates between the internal panel and an
// external display with NO gamescope restart, so running clients (emulators,
// Steam, games) are untouched. This replaces the old "restart konkr-session"
// apply path for the system.external_display toggle, which killed every
// session client mid-game.
//
// 1 = force internal (system.external_display=ignore)
// 0 = normal priority order, externals first (system.external_display=external)
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "0") && strcmp(argv[1], "1"))) {
        fprintf(stderr, "usage: %s <0|1>   (1 = force internal display)\n", argv[0]);
        return 2;
    }
    unsigned long val = (argv[1][0] == '1');

    Display *dpy = XOpenDisplay(NULL);   // DISPLAY, else fail
    if (!dpy) {
        fprintf(stderr, "konkr-forceinternal: cannot open display\n");
        return 1;
    }
    Atom prop = XInternAtom(dpy, "GAMESCOPE_DISPLAY_FORCE_INTERNAL", False);
    XChangeProperty(dpy, DefaultRootWindow(dpy), prop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&val, 1);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
