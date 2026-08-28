#!/bin/sh
# N31 appliance: run after FTL mount. Never hardcodes host sample paths.
#
# `gui` is the LVGL UI on the framebuffer. The terminal UI is still there as
# `ui` if you want it over ssh, where there is no framebuffer to draw on.
set -e
export TINYPOD_BACKEND="${TINYPOD_BACKEND:-alsa}"
if [ -n "$TINYPOD_MOUNT" ]; then
  exec tinypod --mount "$TINYPOD_MOUNT" --backend "$TINYPOD_BACKEND" gui
fi
exec tinypod --backend "$TINYPOD_BACKEND" gui
