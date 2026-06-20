# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

PKG_NAME="gamecontrollerdb"
PKG_VERSION="e1efb4bad8730b2c0c6316617cbd06b9def1192e"
PKG_LICENSE="GPL"
PKG_DEPENDS_TARGET="toolchain SDL2"
PKG_SITE="https://github.com/gabomdq/SDL_GameControllerDB"
PKG_URL="${PKG_SITE}.git"
PKG_LONGDESC="SDL Game Controller DB"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/config/SDL-GameControllerDB
  if [ -f "${PKG_DIR}/config/gamecontrollerdb.txt" ]; then
    cat ${PKG_DIR}/config/gamecontrollerdb.txt >${INSTALL}/usr/config/SDL-GameControllerDB/gamecontrollerdb.txt
  fi
  cat ${PKG_BUILD}/gamecontrollerdb.txt >>${INSTALL}/usr/config/SDL-GameControllerDB/gamecontrollerdb.txt

  # The konkr-inputd virtual "Microsoft X-Box 360 pad" (GUID version 0100):
  # SDL's BUILT-IN entry for it has the dpad hat bits inverted and lacks guide.
  # File entries replace built-ins, so appending the corrected mapping last wins.
  # No paddle: entries — this pad has only b0..b10, so declaring paddles makes
  # SDL_GameControllerHasButton report phantom paddles that never fire.
  echo '030081b85e0400008e02000001000000,Microsoft Xbox 360,a:b0,b:b1,x:b2,y:b3,back:b6,guide:b8,start:b7,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftshoulder:b4,rightshoulder:b5,leftstick:b9,rightstick:b10,lefttrigger:a2,righttrigger:a5,leftx:a0,lefty:a1,rightx:a3,righty:a4,platform:Linux,' >>${INSTALL}/usr/config/SDL-GameControllerDB/gamecontrollerdb.txt
}
