# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="device-controls"
# PKG_VERSION pins the bundled Dear ImGui release (the app source lives in
# this package's sources/; PKG_URL only fetches ImGui).
PKG_VERSION="1.91.5"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/ocornut/imgui"
PKG_URL="https://github.com/ocornut/imgui/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_SOURCE_NAME="imgui-${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain SDL2 nlohmann-json wayland mesa libX11"
PKG_SECTION="apps"
PKG_LONGDESC="On-device control panel for SM8750 handhelds (SDL2 + Dear ImGui): gamepad tester, audio, fan curve and system info, plus device-specific grip remap, stick RGB and motion where present. The --sidebar mode is a Wayland layer-shell quick-settings overlay (EGL/GLES2) that draws over a running game."
PKG_TOOLCHAIN="manual"

make_target() {
  # Generate the wlr-layer-shell client glue from the vendored protocol. It
  # references xdg-shell (for the unused get_popup), so generate xdg-shell's
  # private-code too to satisfy the xdg_popup_interface symbol at link time.
  local proto="${PKG_DIR}/sources/protocols/wlr-layer-shell-unstable-v1.xml"
  local xdg="${PKG_DIR}/sources/protocols/xdg-shell.xml"
  ${TOOLCHAIN}/bin/wayland-scanner client-header "${proto}" \
    "${PKG_BUILD}/wlr-layer-shell-unstable-v1-client-protocol.h"
  ${TOOLCHAIN}/bin/wayland-scanner private-code "${proto}" \
    "${PKG_BUILD}/wlr-layer-shell-unstable-v1-protocol.c"
  ${TOOLCHAIN}/bin/wayland-scanner private-code "${xdg}" \
    "${PKG_BUILD}/xdg-shell-protocol.c"
  ${CC} ${CFLAGS} -I${PKG_BUILD} -c \
    "${PKG_BUILD}/wlr-layer-shell-unstable-v1-protocol.c" -o "${PKG_BUILD}/wlrls.o"
  ${CC} ${CFLAGS} -I${PKG_BUILD} -c \
    "${PKG_BUILD}/xdg-shell-protocol.c" -o "${PKG_BUILD}/xdgshell.o"

  # The sidebar uses the GLES2 ImGui backend; the define must apply to both the
  # backend .cpp and our include of its header.
  ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -DIMGUI_IMPL_OPENGL_ES2 \
    -I${PKG_BUILD} -I${PKG_BUILD}/backends \
    -I${SYSROOT_PREFIX}/usr/include/SDL2 -D_REENTRANT \
    ${PKG_DIR}/sources/main.cpp \
    ${PKG_DIR}/sources/sidebar.cpp \
    ${PKG_BUILD}/imgui.cpp \
    ${PKG_BUILD}/imgui_draw.cpp \
    ${PKG_BUILD}/imgui_tables.cpp \
    ${PKG_BUILD}/imgui_widgets.cpp \
    ${PKG_BUILD}/backends/imgui_impl_sdl2.cpp \
    ${PKG_BUILD}/backends/imgui_impl_sdlrenderer2.cpp \
    ${PKG_BUILD}/backends/imgui_impl_opengl3.cpp \
    ${PKG_BUILD}/wlrls.o ${PKG_BUILD}/xdgshell.o \
    -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lX11 \
    -o ${PKG_BUILD}/device-controls
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/device-controls ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/device-controls

  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/sources/device-controls-restore.service \
     ${INSTALL}/usr/lib/systemd/system/

  mkdir -p ${INSTALL}/usr/config/modules
  cp ${PKG_DIR}/scripts/* ${INSTALL}/usr/config/modules
  chmod 0755 "${INSTALL}/usr/config/modules/Device Controls.sh"
}

post_install() {
  enable_service device-controls-restore.service
}
