# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="konkr-launcher"
# PKG_VERSION pins the bundled Dear ImGui release (the app source lives in
# this package's sources/; PKG_URL only fetches ImGui).
PKG_VERSION="1.91.5"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/ocornut/imgui"
PKG_URL="https://github.com/ocornut/imgui/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_SOURCE_NAME="imgui-${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain SDL2 mesa"
PKG_SECTION="apps"
PKG_LONGDESC="Minimal SDL2 + Dear ImGui game-launcher frontend for SM8750 handhelds: a tabbed grid of Steam plus per-system ROM entries, and a Tools tab. Selectable as the boot frontend via system.frontend=custom."
PKG_TOOLCHAIN="manual"

make_target() {
  # ImGui sources are unpacked into PKG_BUILD; build the single binary against
  # the SDL2 + SDLRenderer2 backends (same stack as device-controls).
  ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 \
    -I${PKG_BUILD} -I${PKG_BUILD}/backends \
    -I${PKG_DIR}/../device-controls/sources \
    -I${SYSROOT_PREFIX}/usr/include/SDL2 -D_REENTRANT \
    ${PKG_DIR}/sources/main.cpp \
    ${PKG_BUILD}/imgui.cpp \
    ${PKG_BUILD}/imgui_draw.cpp \
    ${PKG_BUILD}/imgui_tables.cpp \
    ${PKG_BUILD}/imgui_widgets.cpp \
    ${PKG_BUILD}/backends/imgui_impl_sdl2.cpp \
    ${PKG_BUILD}/backends/imgui_impl_sdlrenderer2.cpp \
    -lSDL2 -lX11 \
    -o ${PKG_BUILD}/konkr-launcher
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/konkr-launcher ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/konkr-launcher
  cp ${PKG_DIR}/sources/start_konkr_launcher.sh ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/start_konkr_launcher.sh

  # The unit is started by name from the 090-ui_service quirk (UI_SERVICE);
  # it is intentionally NOT enabled, so it never competes with ES at boot.
  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/sources/konkr-launcher.service \
     ${INSTALL}/usr/lib/systemd/system/
}
