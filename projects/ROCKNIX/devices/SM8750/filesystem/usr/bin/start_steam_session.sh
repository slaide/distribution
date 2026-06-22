#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

### Boot-to-Steam session frontend (system.frontend=steam).
###
### Steam's own launcher (start_steam.sh -> start_steam_arm64.sh) reads the
### live sway display geometry, stops sway, runs gamescope+Steam on DRM, and
### starts essway (EmulationStation) when Steam exits — so quitting Steam drops
### to ES. If Steam isn't installed/configured, fall straight back to ES rather
### than stranding the user on a black screen.
source /etc/profile

if [ ! -x /usr/bin/start_steam.sh ] || [ ! -d /storage/.local/share/Steam ]; then
  logger -t steam-session "Steam not installed; starting EmulationStation instead"
  exec systemctl start essway.service
fi

exec /usr/bin/start_steam.sh
