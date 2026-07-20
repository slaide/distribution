#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

STEAM_MAIN_SCRIPT=${0}
STEAM_FLAVOR=arm64

source /etc/profile
set_kill set "gamescope steam FEX"

# shellcheck source=start_steam.sh
. /usr/bin/start_steam.sh

steam_ensure_fex_config_template
steam_ensure_konkr_fextuned_tool
steam_prepare_storage_and_vdf
steam_load_es_thunk_settings "$@"
steam_write_fex_config_json
steam_set_cpu_affinity
steam_debug_print

steam_arm64_binfmt_and_proton_prep

# Persistent gamescope session (konkr-session owns DRM): launch Steam as a
# client of the running gamescope instead of tearing down sway and spawning a
# second gamescope. GAMESCOPE_WAYLAND_DISPLAY is set by that gamescope for its
# children (the launcher system()s this script), so it marks the session path.
# Skip the sway geometry read, the systemd-scope re-exec (it strips this env),
# and the dual-screen/return-to-frontend logic.
if [ -n "${GAMESCOPE_WAYLAND_DISPLAY}" ]; then
  set_kill set "steam FEX"   # quit-hotkey must NOT kill the session compositor
  steam_setup_environment
  steam_launch_in_session "$@"
  systemctl restart systemd-binfmt
  exit 0
fi

steam_read_sway_geometry
steam_setup_environment
steam_scope_reexec_if_needed "$@"
steam_dual_screen_begin
steam_launch_bigpicture "$@"
steam_dual_screen_end
systemctl restart systemd-binfmt
