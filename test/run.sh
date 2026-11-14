#!/usr/bin/env bash
# Headless run of a filter on raw RGBA frames (see harness.c). Needs: ./build.sh linux first, Xvfb, libobs + libobs-opengl.
#   ZIG=zig OBS_SRC=... OBS_LIB=... test/run.sh filter_id frames.rgba W H '{"settings":1}' outprefix 30,60
set -euo pipefail
cd "$(dirname "$0")/.."
ZIG=${ZIG:-zig}
"$ZIG" cc -target x86_64-linux-gnu -O1 -Wno-unused-parameter -Ibuild/inc -I"$OBS_SRC/libobs" test/harness.c -o build/harness \
	"$OBS_LIB/libobs.so.30" "${X11_LIB:-/usr/lib/x86_64-linux-gnu/libX11.so.6}"
export LD_LIBRARY_PATH="$OBS_LIB:${LD_LIBRARY_PATH:-}"
xvfb-run -a -s "-screen 0 1280x720x24" build/harness "$PWD/build/datamosh-filter.so" "$@"
