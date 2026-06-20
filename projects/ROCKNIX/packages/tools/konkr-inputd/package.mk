# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="konkr-inputd"
PKG_VERSION="1"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/ROCKNIX"
PKG_DEPENDS_TARGET="toolchain systemd"
PKG_SECTION="tools"
PKG_LONGDESC="KONKR Pocket FIT Elite input authority: grabs the MCU gamepad and \
system-button evdevs and synthesizes one virtual xb360 pad (+ keyboard) under \
full runtime control. Replaces the inputplumber composite on this device."
PKG_TOOLCHAIN="manual"

make_target() {
  ${CC} ${CFLAGS} ${LDFLAGS} -std=gnu23 -Wall -Wextra \
    ${PKG_DIR}/sources/konkr-inputd.c \
    -lsystemd \
    -o ${PKG_BUILD}/konkr-inputd
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/konkr-inputd ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/konkr-inputd

  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/sources/konkr-inputd.service \
     ${INSTALL}/usr/lib/systemd/system/

  mkdir -p ${INSTALL}/usr/lib/systemd/system-sleep
  cp ${PKG_DIR}/sources/konkr-inputd-resume \
     ${INSTALL}/usr/lib/systemd/system-sleep/konkr-inputd
  chmod 0755 ${INSTALL}/usr/lib/systemd/system-sleep/konkr-inputd
}

post_install() {
  enable_service konkr-inputd.service
}
