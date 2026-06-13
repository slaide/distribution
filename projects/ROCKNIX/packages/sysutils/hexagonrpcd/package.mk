# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="hexagonrpcd"
PKG_VERSION="dd9ac70c026e1bad93e8cffa3801255b8ceb551e"
PKG_LICENSE="GPL-3.0"
PKG_SITE="https://github.com/linux-msm/hexagonrpc"
PKG_URL="${PKG_SITE}/archive/${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_SECTION="sysutils"
PKG_LONGDESC="FastRPC reverse-tunnel daemon for Qualcomm DSPs. Serves the sensor registry to the ADSP sensor PD (SEE) so the SSC accelerometer/gyroscope enumerate; consumed by the in-kernel qcom_ssc_imu IIO driver."
PKG_TOOLCHAIN="meson"

# verbose build adds a per-RPC-call trace; off for the shipped daemon
PKG_MESON_OPTS_TARGET="-Dhexagonrpcd_verbose=false"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/.${TARGET_NAME}/hexagonrpcd/hexagonrpcd ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/hexagonrpcd
  cp ${PKG_DIR}/sources/hexagonrpcd-setup ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/hexagonrpcd-setup

  # hexagonrpcd links libhexagonrpc.so.0.4 dynamically
  mkdir -p ${INSTALL}/usr/lib
  cp -P ${PKG_BUILD}/.${TARGET_NAME}/libhexagonrpc/libhexagonrpc.so.0.4 ${INSTALL}/usr/lib/
  ln -sf libhexagonrpc.so.0.4 ${INSTALL}/usr/lib/libhexagonrpc.so.0

  mkdir -p ${INSTALL}/usr/lib/systemd/system
  cp ${PKG_DIR}/sources/hexagonrpcd-sensorspd.service \
     ${PKG_DIR}/sources/hexagonrpcd-resume.service \
     ${INSTALL}/usr/lib/systemd/system/

  # the daemon is started by udev when the ADSP fastrpc node appears
  mkdir -p ${INSTALL}/usr/lib/udev/rules.d
  cp ${PKG_DIR}/sources/60-hexagonrpcd.rules ${INSTALL}/usr/lib/udev/rules.d/
}

post_install() {
  enable_service hexagonrpcd-resume.service
}
