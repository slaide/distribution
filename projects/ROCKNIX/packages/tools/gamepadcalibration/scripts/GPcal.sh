#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2024 ROCKNIX (https://github.com/ROCKNIX)

# Free the raw gamepad for calibration. SM8250 uses inputplumber; SM8750 routes
# through konkr-inputd, so release its exclusive grab for the duration.
case "$HW_DEVICE" in
    SM8250) systemctl stop inputplumber ;;
    SM8750) konkr-inputd --grab 0 ;;
esac

GPCAL_PATH="/usr/local/share/gpcal"

source /etc/profile

set_kill set "python3"

sway_fullscreen "python3" &

# Enable the python3 venv that has the pyxel library installed
# Note: the activate script relies on the CWD, thus the cd
# before
cd "$GPCAL_PATH"
source pyxel/bin/activate

# PYTHONDONTWRITEBYTECODE: We don't expect to do write files
# on a readonly mount point. Thus we tell Python to not try to
# write .pyc files on the import of source modules.
PYTHONDONTWRITEBYTECODE=1 python3 main.py

# Restore input routing. On SM8750 restart konkr-inputd so the virtual pad
# re-reads the freshly calibrated ABS ranges from the raw device.
case "$HW_DEVICE" in
    SM8750) systemctl restart konkr-inputd ;;
    *)      systemctl restart inputplumber ;;
esac
