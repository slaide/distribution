# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="konkr-session"
PKG_VERSION="1"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/ROCKNIX/distribution"
PKG_DEPENDS_TARGET="toolchain gamescope konkr-launcher"
PKG_SECTION="apps"
PKG_LONGDESC="Persistent gamescope session compositor (Steam Deck gamescope-session model): one gamescope owns DRM and hosts the frontend (and Steam/emulators) as clients. Selected via system.compositor=gamescope; sway remains the fallback."
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_DIR}/sources/konkr-session ${INSTALL}/usr/bin/
  cp ${PKG_DIR}/sources/konkr-session-client ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/konkr-session ${INSTALL}/usr/bin/konkr-session-client

  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/system.d/konkr-session.service \
     ${INSTALL}/usr/lib/systemd/system/
}
