#!/bin/sh
# N31 appliance: run after FTL mount. Never hardcodes host sample paths.
set -e
export TINYPOD_BACKEND="${TINYPOD_BACKEND:-alsa}"
if [ -n "$TINYPOD_MOUNT" ]; then
  exec tinypod --mount "$TINYPOD_MOUNT" --backend "$TINYPOD_BACKEND" ui
fi
exec tinypod --backend "$TINYPOD_BACKEND" ui
