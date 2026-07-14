#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

steam_ensure_fex_config_template() {
  if [ ! -d "/storage/.config/fex-emu" ]; then
    cp -r "/usr/config/fex-emu" "/storage/.config/"
  fi
}

# Refresh the KONKR FEX-Tuned compatibility tool in the live Steam install, so
# existing Steam installs pick it up without a full reinstall. Idempotent.
steam_ensure_konkr_fextuned_tool() {
  local src="/usr/share/steam/konkr-fextuned"
  local dst="/storage/.local/share/Steam/compatibilitytools.d"
  [ -d "$src" ] || return 0
  [ -d "/storage/.local/share/Steam" ] || return 0
  mkdir -p "$dst"
  cp -rf "$src" "$dst/"
  chmod +x "$dst/konkr-fextuned/konkr-fex-run" 2>/dev/null || true
}

steam_prepare_storage_and_vdf() {
  mkdir -p /storage/roms/steam/steamapps
  local vdf="/storage/.local/share/Steam/steamapps/libraryfolders.vdf"
  if [ -f "$vdf" ]; then
    grep -q '"/storage/roms/steam"' "$vdf" || sed -i '$ s/}/\t"1" {"path" "\/storage\/roms\/steam"}\n}/' "$vdf"
  fi
}

steam_load_es_thunk_settings() {
  GAME=$(echo "${1}" | sed "s#^/.*/##")
  PLATFORM=$(echo "${2}" | sed "s#^/.*/##")
  ASOUND_LIB=$(get_setting asound_host_library "${PLATFORM}" "${GAME}")
  ASOUND_LIB=${ASOUND_LIB:-0}
  DRM_LIB=$(get_setting drm_host_library "${PLATFORM}" "${GAME}")
  DRM_LIB=${DRM_LIB:-0}
  VULKAN_LIB=$(get_setting vulkan_host_library "${PLATFORM}" "${GAME}")
  VULKAN_LIB=${VULKAN_LIB:-0}
  WAYLAND_LIB=$(get_setting wayland_client_host_library "${PLATFORM}" "${GAME}")
  WAYLAND_LIB=${WAYLAND_LIB:-0}
  GL_LIB=$(get_setting gl_host_library "${PLATFORM}" "${GAME}")
  GL_LIB=${GL_LIB:-0}
  GAMESCOPE=$(get_setting gamescope "${PLATFORM}" "${GAME}")
}

steam_write_fex_config_json() {
  local tmp
  tmp=$(mktemp)
  jq \
    --arg asound "$ASOUND_LIB" \
    --arg drm "$DRM_LIB" \
    --arg vulkan "$VULKAN_LIB" \
    --arg wayland "$WAYLAND_LIB" \
    --arg gl "$GL_LIB" \
    '.ThunksDB |= {
      asound: ($asound | tonumber),
      drm: ($drm | tonumber),
      Vulkan: ($vulkan | tonumber),
      WaylandClient: ($wayland | tonumber),
      GL: ($gl | tonumber)
    }' \
    /storage/.config/fex-emu/Config.json >"$tmp" &&
    mv "$tmp" /storage/.config/fex-emu/Config.json
}

steam_set_cpu_affinity() {
  local cores
  cores=$(get_setting "cores" "${PLATFORM}" "${GAME}")
  if [ "${cores}" = "little" ]; then
    EMUPERF="${SLOW_CORES}"
  elif [ "${cores}" = "big" ]; then
    EMUPERF="${FAST_CORES}"
  else
    unset EMUPERF
  fi
}

steam_debug_print() {
  echo "GAME set to: ${GAME}"
  echo "PLATFORM set to: ${PLATFORM}"
  echo "CPU CORES set to: ${EMUPERF}"
  echo "ASOUND HOST LIB set to: ${ASOUND_LIB}"
  echo "DRM HOST LIB set to: ${DRM_LIB}"
  echo "VULKAN HOST LIB set to: ${VULKAN_LIB}"
  echo "WAYLAND HOST LIB set to: ${WAYLAND_LIB}"
  echo "GL HOST LIB set to: ${GL_LIB}"
  echo "GAMESCOPE set to: ${GAMESCOPE}"
  echo "VSYNC set to: ${VSYNC}"
}

steam_read_sway_geometry() {
  eval "$(swaymsg -t get_outputs | jq -r '
    .[] | select(.focused == true) |
    "W=\(.current_mode.width) H=\(.current_mode.height) TRANSFORM=\(.transform)"
  ')"
}

steam_setup_environment() {
  TZ=$(timedatectl status | grep 'Time zone' | awk '{print $3}')
  [ -n "${TZ}" ] && export TZ
}

steam_scope_reexec_if_needed() {
  if [ -z "$_STEAM_SCOPE" ]; then
    systemctl stop steam-bigpicture.scope 2>/dev/null || true
    exec systemd-run \
      --scope \
      --slice=system.slice \
      --unit=steam-bigpicture \
      --collect \
      -E _STEAM_SCOPE=1 \
      -E HOME="$HOME" \
      -E USER="$USER" \
      -E TZ="$TZ" \
      -- "${STEAM_MAIN_SCRIPT}" "$@"
  fi
}

steam_return_to_frontend() {
  # Return to the configured boot frontend after Steam exits, instead of always
  # dropping into EmulationStation. (system.frontend=steam still falls back to
  # ES, which is its documented behaviour.)
  case "$(get_setting system.frontend)" in
    konkr-launcher) systemctl start konkr-launcher.service ;;
    *)              systemctl start essway.service ;;
  esac
}

# --- external display (system.external_display) ---------------------------
# Echo the DRM connector name (e.g. DP-1) of the first connector matching one of
# the given type prefixes. Built-in panels (DSI/eDP/LVDS) are returned as-is;
# external types (DP/HDMI-A/DisplayPort) only when the connector is connected.
steam_drm_connector() {
  local t d name
  for t in "$@"; do
    for d in /sys/class/drm/card*-"${t}"-*; do
      [ -d "$d" ] || continue
      name="${d##*/}"; name="${name#card*-}"
      case "$t" in
        DSI|eDP|LVDS) echo "$name"; return 0 ;;
        *) [ "$(cat "$d/status" 2>/dev/null)" = "connected" ] && { echo "$name"; return 0; } ;;
      esac
    done
  done
}

# Preferred (first-listed) mode of a connected external connector, e.g. 1920x1080.
steam_drm_ext_mode() {
  local f m
  for f in /sys/class/drm/card*-DP-*/modes /sys/class/drm/card*-HDMI-A-*/modes; do
    [ -f "$f" ] || continue
    m=$(head -1 "$f" 2>/dev/null)
    [ -n "$m" ] && { echo "$m"; return 0; }
  done
}

# When an external display is attached, honor system.external_display for the
# gamescope session: point --prefer-output at the external (default) or pin it
# to the built-in panel ("ignore"). With nothing attached this is a no-op, so
# internal-only and internal-dual-screen devices are unaffected.
steam_external_display_prefer() {
  local external internal pref mode
  external=$(steam_drm_connector DP HDMI-A DisplayPort)
  [ -n "${external}" ] || return 0

  pref=$(get_setting system.external_display)
  [ -n "${pref}" ] || pref="external"
  internal=$(steam_drm_connector DSI eDP LVDS)

  if [ "${pref}" = "external" ]; then
    PREFER_OUTPUT="--prefer-output ${external}${internal:+,${internal}}"
    STEAM_EXTERNAL_ACTIVE=1
    mode=$(steam_drm_ext_mode)
    [ -n "${mode}" ] && { EXT_W="${mode%x*}"; EXT_H="${mode#*x}"; }
  elif [ -n "${internal}" ]; then
    PREFER_OUTPUT="--prefer-output ${internal}"
    STEAM_EXTERNAL_ACTIVE=0
  fi
}

steam_dual_screen_begin() {
  if [ "${DEVICE_HAS_DUAL_SCREEN}" = "true" ]; then
    swaymsg 'seat seat1 fallback true'
    PREFER_OUTPUT="--prefer-output $SDL_VIDEO_DISPLAY_PRIORITY"
  fi
  steam_external_display_prefer
}

steam_dual_screen_end() {
  if [ "${DEVICE_HAS_DUAL_SCREEN}" = "true" ]; then
    swaymsg 'seat seat1 fallback false'
  fi
}

steam_arm64_binfmt_and_proton_prep() {
  echo 0 >/proc/sys/fs/binfmt_misc/x86_64
  echo 0 >/proc/sys/fs/binfmt_misc/x86
  mkdir -p "/storage/.local/share/Steam/steamapps/common/Proton 11.0 (ARM64)/"
  cp -f "/usr/share/steam/toolmanifest.vdf" "/storage/.local/share/Steam/steamapps/common/Proton 11.0 (ARM64)/"
}

steam_launch_bigpicture() {
  local game_uri=""
  local force_orientation="left"
  local gamescope_mode_file="/storage/.config/gamescope/modes.cfg"
  if [ "${TRANSFORM}" = "90" ]; then
    force_orientation="right"
  elif [ "${TRANSFORM}" = "270" ]; then
    force_orientation="left"
  elif [ "${TRANSFORM}" = "normal" ]; then
    force_orientation="normal"
  fi

  # Panel-rotation flags for the built-in (portrait-mounted) display. An
  # external display is already landscape, so drop the rotation and adopt the
  # display's own geometry instead of the panel's.
  local rotation_args="--force-orientation ${force_orientation} --use-rotation-shader"
  if [ "${STEAM_EXTERNAL_ACTIVE:-0}" = "1" ]; then
    W="${EXT_W:-1920}"
    H="${EXT_H:-1080}"
    rotation_args=""
  fi

  if [[ "$1" == *.desktop && -f "$1" && "$(basename "$1")" != "Steam.desktop" ]]; then
    local exec_line
    exec_line=$(grep -m1 '^Exec=' "$1" | cut -d'=' -f2-)
    game_uri="${exec_line#steam } -silent"
  fi

  if [ "${GAMESCOPE}" != "0" ]; then
    mkdir -p "$(dirname "$gamescope_mode_file")"
    touch "$gamescope_mode_file"
  fi
  unset MESA_LOADER_DRIVER_OVERRIDE
  if [ "${STEAM_FLAVOR}" = "arm64" ]; then
    SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=/storage/.local/share/Steam/lib/aarch64-linux-gnu/ ${EMUPERF} /storage/.local/share/Steam/steamrtarm64/steam -steamdeck -exitsteam
    if [ "${GAMESCOPE}" = "0" ]; then
      SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=/storage/.local/share/Steam/lib/aarch64-linux-gnu/ ${EMUPERF} /storage/.local/share/Steam/steamrtarm64/steam -nofriendsui -noverifyfiles -nobootstrapupdate -skipinitialbootstrap -norepairfiles -noshaders ${game_uri:+"$game_uri"}
      exit 0
    else
      systemctl stop sway
      # No -r with --backend drm: gamescope scans out the panel's preferred
      # mode but paces vsynced clients at -r, so a mismatched -r (e.g. sway's
      # 60Hz on a 144Hz panel) beats against real vblanks and drops frames.
      GAMESCOPE_MODE_SAVE_FILE="${gamescope_mode_file}" GAMESCOPE_FAKE_OUTPUT_MM=508x286 env -u WAYLAND_DISPLAY LD_LIBRARY_PATH=/storage/.local/share/Steam/lib/aarch64-linux-gnu/ ${EMUPERF} \
        gamescope $PREFER_OUTPUT -W "$W" -H "$H" --xwayland-count 2 --mangoapp --backend drm ${rotation_args} -e -- \
        /storage/.local/share/Steam/steamrtarm64/steam -steamdeck -steamos3 -gamepadui -noverifyfiles -nobootstrapupdate -skipinitialbootstrap -norepairfiles -noshaders ${game_uri:+"$game_uri"}
      steam_return_to_frontend
      exit 0
    fi
  else
    FEX /usr/bin/steam -steamdeck -exitsteam
    if [ "${GAMESCOPE}" = "0" ]; then
      ${EMUPERF} FEX /usr/bin/steam -nofriendsui -noverifyfiles -nobootstrapupdate -skipinitialbootstrap -norepairfiles -noshaders ${game_uri:+"$game_uri"}
      exit 0
    else
      systemctl stop sway
      GAMESCOPE_MODE_SAVE_FILE="${gamescope_mode_file}" GAMESCOPE_FAKE_OUTPUT_MM=508x286 env -u WAYLAND_DISPLAY ${EMUPERF} \
        gamescope $PREFER_OUTPUT -W "$W" -H "$H" --xwayland-count 2 --mangoapp --backend drm ${rotation_args} -e -- \
        FEX /usr/bin/steam -steamdeck -steamos3 -gamepadui -noverifyfiles -nobootstrapupdate -skipinitialbootstrap -norepairfiles -noshaders ${game_uri:+"$game_uri"}
      steam_return_to_frontend
      exit 0
    fi
  fi
}

# Entry point from EmulationStation (not used when this file is sourced).
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  source /etc/profile
  GAME=$(echo "${1}" | sed "s#^/.*/##")
  PLATFORM=$(echo "${2}" | sed "s#^/.*/##")
  STEAM_VERSION=$(get_setting steam_version "${PLATFORM}" "${GAME}")
  STEAM_VERSION=${STEAM_VERSION:-"arm64"}
  echo "STEAM_VERSION set to: ${STEAM_VERSION}"
  if [ "${STEAM_VERSION}" = "arm64" ]; then
    exec /usr/bin/start_steam_arm64.sh "$@"
  else
    exec /usr/bin/start_steam_x86.sh "$@"
  fi
fi
