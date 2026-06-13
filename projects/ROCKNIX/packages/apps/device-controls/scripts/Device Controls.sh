#!/bin/bash

# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

# On-device control panel for SM8750 handhelds. Device-specific tabs (grips,
# stick RGB, motion) hide themselves at runtime when the hardware is absent.

source /etc/profile

set_kill set "device-controls"

sway_fullscreen "device-controls" &

/usr/bin/device-controls
