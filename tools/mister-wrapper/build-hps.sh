#!/usr/bin/env bash
# Build MiSTer_DonutDodo: upstream Main_MiSTer at UPSTREAM_COMMIT, plus the files
# in overlay/ and one inserted call in scheduler.cpp. MiSTer.ini
# `[DonutDodo] main=...` makes stock MiSTer exec this build when the DonutDodo
# core loads (see overlay/donutdodo_hook.cpp).
#
# No upstream file is vendored. The only upstream edit is inserted here, at
# build time, after the first `scheduler_wait_fpga_ready();` in
# scheduler_co_poll(), and the Makefile is upstream's with PRJ renamed; the
# build fails if either anchor is missing.
# Moving to a newer upstream is: UPSTREAM_COMMIT=<sha> build-hps.sh.
#
#   tools/mister-wrapper/build-hps.sh            -> build/mister-wrapper/MiSTer_DonutDodo
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/MiSTer-devel/Main_MiSTer.git}"
# Same pin as maldita.castilla-mister's MiSTer_Maldita (device-validated there).
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-3380931329b8acb442bd3d35a24d89f88641b7cf}"
OUT="${OUTPUT_DIR:-$ROOT/build/mister-wrapper}"
SRC="$OUT/src"
IMAGE="${WRAPPER_IMAGE:-donutdodo-mister-wrapper-hps}"
PRJ=MiSTer_DonutDodo

mkdir -p "$OUT"
if [ ! -d "$SRC/.git" ]; then
    git clone --filter=blob:none --no-checkout "$UPSTREAM_URL" "$SRC"
fi
git -C "$SRC" fetch -q origin "$UPSTREAM_COMMIT" 2>/dev/null || true
git -C "$SRC" checkout -q -f "$UPSTREAM_COMMIT"
git -C "$SRC" clean -qfdx

# --- the one upstream edit -------------------------------------------------------
awk '
    /^static void scheduler_co_poll\(void\)/ { inpoll = 1 }
    { print }
    inpoll && !done && /^[ \t]*scheduler_wait_fpga_ready\(\);[ \t]*$/ {
        print "\t\tdonutdodo_hook_poll(); // Donut Dodo main= hook (tools/mister-wrapper)"
        done = 1; inserts++
    }
    END { if (inserts != 1) exit 3 }
' "$SRC/scheduler.cpp" > "$SRC/scheduler.cpp.new" || {
    echo "build-hps.sh: anchor 'scheduler_wait_fpga_ready();' in scheduler_co_poll() not found in" >&2
    echo "  Main_MiSTer@$UPSTREAM_COMMIT scheduler.cpp - re-check the hook position before building." >&2
    exit 1
}
{ echo '#include "donutdodo_hook.h"'; cat "$SRC/scheduler.cpp.new"; } > "$SRC/scheduler.cpp"
rm "$SRC/scheduler.cpp.new"
cp "$HERE"/overlay/* "$SRC/"
# upstream's own Makefile, output renamed
sed 's/^PRJ = MiSTer$/PRJ = '"$PRJ"'/' "$SRC/Makefile" > "$SRC/Makefile.donutdodo"
grep -q "^PRJ = $PRJ\$" "$SRC/Makefile.donutdodo" || { echo "build-hps.sh: no 'PRJ = MiSTer' line in upstream Makefile" >&2; exit 1; }
git -C "$SRC" diff --stat

# --- build (Debian armhf cross toolchain, native arch: no QEMU) -------------------
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f "$HERE/Dockerfile.wrapper" "$HERE"
docker run --rm -u "$(id -u):$(id -g)" -v "$SRC:/src" -w /src "$IMAGE" \
    make -f Makefile.donutdodo BASE=arm-linux-gnueabihf -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

cp "$SRC/bin/$PRJ" "$OUT/$PRJ"
# The release and the CoresMenu toggle both gate on this string: a stock
# Main_MiSTer renamed to MiSTer_DonutDodo runs fine and never starts the game.
grep -q "/media/fat/games/DonutDodo/launch.sh" "$OUT/$PRJ" || { echo "hook string missing from $PRJ" >&2; exit 1; }
echo "built $OUT/$PRJ (Main_MiSTer@${UPSTREAM_COMMIT:0:7})"
