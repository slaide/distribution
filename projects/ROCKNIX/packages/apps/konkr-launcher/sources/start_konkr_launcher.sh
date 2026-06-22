#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

### Frontend launcher wrapper: run konkr-launcher fullscreen under sway, the
### same way start_es.sh runs EmulationStation. The app_id below must match
### the swaymsg criteria passed to sway_fullscreen.
source /etc/profile

export SDL_VIDEO_WAYLAND_WMCLASS="konkr-launcher"
export SDL_VIDEO_X11_WMCLASS="konkr-launcher"

set_kill set "konkr-launcher"
sway_fullscreen "konkr-launcher" &

exec /usr/bin/konkr-launcher
