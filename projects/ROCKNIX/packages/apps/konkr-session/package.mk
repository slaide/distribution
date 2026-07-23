# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="konkr-session"
PKG_VERSION="1"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/ROCKNIX/distribution"
PKG_DEPENDS_TARGET="toolchain gamescope konkr-launcher libX11"
PKG_SECTION="apps"
PKG_LONGDESC="Persistent gamescope session compositor (Steam Deck gamescope-session model): one gamescope owns DRM and hosts the frontend (and Steam/emulators) as clients. Selected via system.compositor=gamescope; sway remains the fallback."
PKG_TOOLCHAIN="manual"

make_target() {
  # konkr-baselayer: session daemon that tags non-Steam windows so they show
  # under gamescope -e (emulators / ports / ES / tools). See the source header.
  ${CC} ${CFLAGS} ${LDFLAGS} -std=gnu11 -Wall \
    ${PKG_DIR}/sources/konkr-baselayer.c -lX11 \
    -o ${PKG_BUILD}/konkr-baselayer
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_DIR}/sources/konkr-session ${INSTALL}/usr/bin/
  cp ${PKG_DIR}/sources/konkr-session-client ${INSTALL}/usr/bin/
  cp ${PKG_BUILD}/konkr-baselayer ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/konkr-session ${INSTALL}/usr/bin/konkr-session-client ${INSTALL}/usr/bin/konkr-baselayer

  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/system.d/konkr-session.service \
     ${INSTALL}/usr/lib/systemd/system/
}
