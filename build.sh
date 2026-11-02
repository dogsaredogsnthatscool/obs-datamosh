#!/usr/bin/env bash
# Build datamosh-filter.dll for OBS on Windows (x64) from Linux, no MSVC needed.
#   ZIG=path/to/zig  OBS_SRC=path/to/obs-studio  ./build.sh [linux]
# OBS_SRC only needs the libobs/ headers (any OBS 30+ tag). "linux" also builds a .so against OBS_LIB for testing.
set -euo pipefail
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
: "${OBS_SRC:?set OBS_SRC to an obs-studio source tree}"
B=build
mkdir -p "$B/inc" dist

# headers the cmake build would normally generate
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > "$B/inc/obsconfig.h"
python3 - src/mosh.effect "$B/inc/mosh-effect.h" <<'EOF'
import sys, json
src = open(sys.argv[1]).read()
open(sys.argv[2], "w").write("static const char MOSH_EFFECT[] = " + "\n".join(json.dumps(l + "\n") for l in src.split("\n")) + ";\n")
EOF
CFLAGS=(-O2 -std=c11 -Wall -Wno-unused-parameter -I"$B/inc" -I"$OBS_SRC/libobs")

# Windows: import obs.dll through a .def of exactly the functions we call
"$ZIG" cc -target x86_64-windows-gnu "${CFLAGS[@]}" -c src/mosh-filter.c -o "$B/mosh-filter.obj"
{ echo "LIBRARY obs.dll"; echo "EXPORTS"
  nm -u "$B/mosh-filter.obj" | awk '{print $2}' | grep -E '^(obs_|gs_|b(z|m)alloc|bfree|blog|vec)' ; } > "$B/obs.def"
"$ZIG" dlltool -d "$B/obs.def" -l "$B/libobs.a" -m i386:x86-64
"$ZIG" cc -target x86_64-windows-gnu -shared -o dist/datamosh-filter.dll "$B/mosh-filter.obj" "$B/libobs.a" \
	-Wl,--export-all-symbols -s
echo "built dist/datamosh-filter.dll"

if [ "${1:-}" = linux ]; then
	: "${OBS_LIB:?set OBS_LIB to the dir holding libobs.so}"
	"$ZIG" cc -target x86_64-linux-gnu "${CFLAGS[@]}" -fPIC -shared -o "$B/datamosh-filter.so" src/mosh-filter.c \
		"$OBS_LIB/libobs.so.30"
	echo "built $B/datamosh-filter.so"
fi
