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
for e in mosh pointcloud censor sticker trail; do
python3 - "src/$e.effect" "$B/inc/$e-effect.h" "${e^^}_EFFECT" <<'EOF'
import sys, json
src = open(sys.argv[1]).read()
open(sys.argv[2], "w").write("static const char " + sys.argv[3] + "[] = " + "\n".join(json.dumps(l + "\n") for l in src.split("\n")) + ";\n")
EOF
done
python3 -I data/cascade2h.py data/lbpcascade_frontalface_opencv.xml "$B/inc/face-cascade.h"
CFLAGS=(-O2 -std=c11 -Wall -Wno-unused-parameter -I"$B/inc" -I"$OBS_SRC/libobs")

# Windows: import obs.dll through a .def of exactly the functions we call
SRCS=(mosh-filter pointcloud-filter censor-filter sticker-filter trail-filter facetrack facedetect)
for c in "${SRCS[@]}"; do
	"$ZIG" cc -target x86_64-windows-gnu "${CFLAGS[@]}" -c "src/$c.c" -o "$B/$c.obj"
done
OBJS=("${SRCS[@]/#/$B/}"); OBJS=("${OBJS[@]/%/.obj}")
{ echo "LIBRARY obs.dll"; echo "EXPORTS"
  nm -u "${OBJS[@]}" | awk '{print $2}' | grep -E '^(obs_|gs_|os_|b(z|m)alloc|bfree|bmemdup|blog|vec)' | sort -u ; } > "$B/obs.def"
"$ZIG" dlltool -d "$B/obs.def" -l "$B/libobs.a" -m i386:x86-64
"$ZIG" cc -target x86_64-windows-gnu -shared -o dist/datamosh-filter.dll "${OBJS[@]}" "$B/libobs.a" \
	-Wl,--export-all-symbols -s
rm -f dist/*.lib # import lib zig leaves behind; nobody links against us
echo "built dist/datamosh-filter.dll"

if [ "${1:-}" = linux ]; then
	: "${OBS_LIB:?set OBS_LIB to the dir holding libobs.so}"
	"$ZIG" cc -target x86_64-linux-gnu "${CFLAGS[@]}" -fPIC -shared -o "$B/datamosh-filter.so" $(printf "src/%s.c " "${SRCS[@]}") -lm -lpthread \
		"$OBS_LIB/libobs.so.30"
	echo "built $B/datamosh-filter.so"
fi
