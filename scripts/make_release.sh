#!/bin/sh
# Assemble the SD-card release bundle (after cash.cow.dx-mister's make_release.sh).
#   RBF_SRC=<DonutDodo_YYYYMMDD.rbf> scripts/make_release.sh <tag>
# Output: build/release/DonutDodo-MiSTer-<tag>.zip (+ games/DonutDodo/sha256sums.txt inside)
#
# Inputs, all built from this repo's sources and pins (README.md "Building"):
#   ENGINE   FRT 2.1.0 / Godot 3.5.2 armhf template, pruned (scripts/build_engine.sh; patches 0002, 0006)
#   SDL2     SDL2 2.32.10 + patches 0001, 0003-0005, 0007 (null GL: no Mesa ships)
#   GLUE     libmisterglue.so, rebuilt here from src/ (make, armhf image)
#   HOOK_BIN MiSTer_hybrid, the main= hook (platform CI artifact, or
#            external/mister-hybrid-platform/device/main-hook/build-hps.sh); ships as
#            games/DonutDodo/platform/MiSTer_hybrid
#   RBF_SRC  DonutDodo core, maldita.castilla-mister branch donutdodo/fb-320x240, default core_variant
#   mister-port.toml, rendered by external/mister-hybrid-platform: launcher, platform/
#            (launch_lib.sh, mem_wc modules, DDR map, MiSTer_hybrid, hybrid.d entry),
#            Scripts entries, MGL
# The zip installs only under games/, Scripts/ and _Other/: the Downloader refuses
# linux/, screenshots/, savestates/ and downloader/ for every database but
# distribution_mister, and the check at the end fails the build on anything else.
# The game is NOT bundled: DonutDodo.pck is the user's own copy.
set -e
TAG=$1
[ -n "$TAG" ] || { echo "usage: RBF_SRC=<rbf> $0 <tag>"; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ENGINE=${ENGINE:-$ROOT/work/godot-3.5.2-prune/bin/godot.frt.opt.arm32v7}
SDL2=${SDL2:-$ROOT/work/build-sdl2/build/.libs/libSDL2-2.0.so.0.3200.10}
PLAT=$ROOT/external/mister-hybrid-platform
HOOK_BIN=${HOOK_BIN:-$PLAT/build/main-hook/MiSTer_hybrid}
IMAGE=${IMAGE:-gmloader-armhf-build:bullseye}
RBF_SRC=${RBF_SRC:?set RBF_SRC to the DonutDodo RBF (DonutDodo_*_YYYYMMDD.rbf)}
for f in "$ENGINE" "$SDL2" "$RBF_SRC"; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
done
# An SDL2 without patch 0007 would need Mesa, which does not ship.
grep -q "MiSTer null GL" "$SDL2" || { echo "$SDL2 has no null GL (patch 0007)"; exit 1; }
# A stock Main_MiSTer under this name loads the core and never starts the game;
# a platform v0.3.x hook reads /media/fat/linux/hybrid.d, not platform/hybrid.d.
grep -q "MiSTer_hybrid registry: <binary dir>/hybrid.d" "$HOOK_BIN" 2>/dev/null \
	|| { echo "$HOOK_BIN missing or not the platform v0.4 hooked build (run $PLAT/device/main-hook/build-hps.sh)"; exit 1; }

# Glue from the checked-out source, in a clean output dir.
rm -rf "$ROOT/build/rel"
docker run --rm -v "$ROOT":/p -w /p "$IMAGE" make OUT=build/rel -j4 >/dev/null

OUT=$ROOT/build/release/DonutDodo-MiSTer-$TAG
G=$OUT/games/DonutDodo
rm -rf "$OUT"; mkdir -p "$OUT/_Other" "$G/libs" "$G/gamedata"
python3 "$PLAT/tools/mister_platform.py" render "$ROOT/mister-port.toml" --out "$OUT" --hook-binary "$HOOK_BIN"
cp "$ROOT/dist/README.md" "$G/README.md"
cp "$ROOT/LICENSE" "$ROOT/LICENSING.md" "$G/"
docker run --rm -v "$(dirname "$ENGINE")":/i:ro -v "$G":/o "$IMAGE" \
	arm-linux-gnueabihf-strip -o /o/frt_3.5.2 "/i/$(basename "$ENGINE")"
cp "$ROOT/build/rel/libmisterglue.so" "$G/libs/"
# Debug info only; the code is the tested build's.
docker run --rm -v "$(dirname "$SDL2")":/i:ro -v "$G/libs":/o "$IMAGE" \
	arm-linux-gnueabihf-objcopy --strip-debug "/i/$(basename "$SDL2")" /o/libSDL2-2.0.so.0
echo "Put your DonutDodo.pck here (see ../README.md)." > "$G/gamedata/PUT_DonutDodo.pck_HERE.txt"
cp "$RBF_SRC" "$OUT/_Other/DonutDodo_$(basename "$RBF_SRC" .rbf | grep -oE '[0-9]{8}$').rbf"
chmod +x "$G/frt_3.5.2"
# Checksums live inside the game folder: extracting over /media/fat must not
# drop files into the SD root.
( cd "$OUT" && find . -type f ! -name sha256sums.txt | sort | xargs shasum -a 256 > games/DonutDodo/sha256sums.txt )
( cd "$OUT" && rm -f "../DonutDodo-MiSTer-$TAG.zip" && zip -qr "../DonutDodo-MiSTer-$TAG.zip" . )
# Install paths: only games/DonutDodo/, Scripts/ and _Other/ (a MiSTer database
# cannot install anything else), and the hook with its registry entry must be there.
ZIP=$ROOT/build/release/DonutDodo-MiSTer-$TAG.zip
BAD=$(unzip -Z1 "$ZIP" | grep -vE '^(games/(DonutDodo/.*)?|Scripts/.*|_Other/.*)$' || true)
[ -z "$BAD" ] || { echo "zip has paths outside games/DonutDodo/, Scripts/, _Other/:"; echo "$BAD"; exit 1; }
for m in games/DonutDodo/platform/MiSTer_hybrid games/DonutDodo/platform/hybrid.d/DonutDodo.conf; do
	unzip -Z1 "$ZIP" | grep -qx "$m" || { echo "zip is missing $m"; exit 1; }
done
ls -la "$ROOT/build/release/DonutDodo-MiSTer-$TAG.zip"
shasum -a 256 "$ROOT/build/release/DonutDodo-MiSTer-$TAG.zip"
