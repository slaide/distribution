# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="extra-firmware"
# slaide fork: upstream 99e17b0 + KONKR PFE sensor configs (746f066, stock vendor dump), served
# to the ADSP sensor PD by the hexagonrpcd package so the SSC gyro/accel
# enumerate. Additive only; other devices' firmware is identical to upstream.
PKG_VERSION="746f066cadbd67d1e94b208e65def4755a41e67e"
PKG_LICENSE="proprietary"
PKG_SITE="https://github.com/slaide/extra-firmware"
PKG_URL="https://github.com/slaide/extra-firmware/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="extra-firmware: Extra kernel firmware needed for ROCKNIX devices"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/$(get_full_firmware_dir)

  case "${DEVICE}" in
    "SM6115") cp -a SM6115/* ${INSTALL}/$(get_full_firmware_dir) ;;
    "SM8250") cp -a SM8250/* ${INSTALL}/$(get_full_firmware_dir) ;;
    "SM8750") cp -a SM8750/* ${INSTALL}/$(get_full_firmware_dir) ;;
  esac
}
