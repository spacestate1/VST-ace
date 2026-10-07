#!/bin/sh
# The REAPER extension SDK and the WDL headers it includes (swell's types),
# cloned beside this script into third_party/ -- not vendored into the repo.
# Override the build's idea of where they are with -DREAPER_SDK_DIR= and
# -DWDL_DIR= to use copies you already have.
set -e
cd "$(dirname "$0")"
mkdir -p third_party
[ -d third_party/reaper-sdk ] || git clone --depth 1 https://github.com/justinfrankel/reaper-sdk.git third_party/reaper-sdk
[ -d third_party/WDL ]        || git clone --depth 1 https://github.com/justinfrankel/WDL.git third_party/WDL
echo "ok: third_party/reaper-sdk, third_party/WDL"
