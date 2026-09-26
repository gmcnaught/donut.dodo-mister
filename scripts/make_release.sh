#!/bin/sh
# Assemble the SD-card release bundle (after cash.cow.dx-mister's make_release.sh).
#   RBF_SRC=<DonutDodo_YYYYMMDD.rbf> scripts/make_release.sh <tag>
# Output: build/release/DonutDodo-MiSTer-<tag>.zip (+ games/DonutDodo/sha256sums.txt inside)
#
# Inputs, all built from this repo's sources and pins (README.md "Building"):
#   ENGINE   FRT 2.1.0 / Godot 3.5.2 armhf template, pruned (scripts/build_engine.sh; patches 0002, 0006)
#   SDL2     SDL2 2.32.10 + patches 0001, 0003-0005, 0007 (null GL: no Mesa ships)
#   GLUE     libmisterglue.so, rebuilt here from src/ (make, armhf image)
#   WRAPPER  MiSTer_DonutDodo (tools/mister-wrapper/build-hps.sh)
#   RBF_SRC  DonutDodo core, maldita.castilla-mister branch donutdodo/fb-320x240, default core_variant
#   tools/mem_wc/prebuilt/*.ko
# The game is NOT bundled: DonutDodo.pck is the user's own copy.
set -e
TAG=$1
[ -n "$TAG" ] || { echo "usage: RBF_SRC=<rbf> $0 <tag>"; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ENGINE=${ENGINE:-$ROOT/work/godot-3.5.2-prune/bin/godot.frt.opt.arm32v7}
SDL2=${SDL2:-$ROOT/work/build-sdl2/build/.libs/libSDL2-2.0.so.0.3200.10}
WRAPPER=${WRAPPER:-$ROOT/build/mister-wrapper/MiSTer_DonutDodo}
IMAGE=${IMAGE:-gmloader-armhf-build:bullseye}
RBF_SRC=${RBF_SRC:?set RBF_SRC to the DonutDodo RBF (DonutDodo_*_YYYYMMDD.rbf)}
for f in "$ENGINE" "$SDL2" "$RBF_SRC"; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
done
# An SDL2 without patch 0007 would need Mesa, which does not ship.
grep -q "MiSTer null GL" "$SDL2" || { echo "$SDL2 has no null GL (patch 0007)"; exit 1; }
# A stock Main_MiSTer under this name loads the core and never starts the game.
grep -q /media/fat/games/DonutDodo/launch.sh "$WRAPPER" 2>/dev/null \
	|| { echo "$WRAPPER missing or not the hooked build (run tools/mister-wrapper/build-hps.sh)"; exit 1; }

# Glue from the checked-out source, in a clean output dir.
rm -rf "$ROOT/build/rel"
docker run --rm -v "$ROOT":/p -w /p "$IMAGE" make OUT=build/rel -j4 >/dev/null

OUT=$ROOT/build/release/DonutDodo-MiSTer-$TAG
G=$OUT/games/DonutDodo
rm -rf "$OUT"; mkdir -p "$OUT/_Other" "$OUT/Scripts" "$G/libs" "$G/gamedata"
cp "$ROOT/dist/Scripts/DonutDodo.sh" "$ROOT/dist/Scripts/DonutDodo_CoresMenu.sh" "$OUT/Scripts/"
cp "$ROOT/dist/games/DonutDodo/launch.sh" "$G/"
cp "$ROOT/dist/README.md" "$G/README.md"
cp "$ROOT/LICENSE" "$ROOT/LICENSING.md" "$G/"
cp "$WRAPPER" "$G/MiSTer_DonutDodo"
docker run --rm -v "$(dirname "$ENGINE")":/i:ro -v "$G":/o "$IMAGE" \
	arm-linux-gnueabihf-strip -o /o/frt_3.5.2 "/i/$(basename "$ENGINE")"
cp "$ROOT/build/rel/libmisterglue.so" "$G/libs/"
# Debug info only; the code is the tested build's.
docker run --rm -v "$(dirname "$SDL2")":/i:ro -v "$G/libs":/o "$IMAGE" \
	arm-linux-gnueabihf-objcopy --strip-debug "/i/$(basename "$SDL2")" /o/libSDL2-2.0.so.0
cp "$ROOT"/tools/mem_wc/prebuilt/*.ko "$G/"
echo "Put your DonutDodo.pck here (see ../README.md)." > "$G/gamedata/PUT_DonutDodo.pck_HERE.txt"
cp "$RBF_SRC" "$OUT/_Other/DonutDodo_$(basename "$RBF_SRC" .rbf | grep -oE '[0-9]{8}$').rbf"
chmod +x "$OUT"/Scripts/*.sh "$G/launch.sh" "$G/MiSTer_DonutDodo" "$G/frt_3.5.2"
# Checksums live inside the game folder: extracting over /media/fat must not
# drop files into the SD root.
( cd "$OUT" && find . -type f ! -name sha256sums.txt | sort | xargs shasum -a 256 > games/DonutDodo/sha256sums.txt )
( cd "$OUT" && rm -f "../DonutDodo-MiSTer-$TAG.zip" && zip -qr "../DonutDodo-MiSTer-$TAG.zip" . )
ls -la "$ROOT/build/release/DonutDodo-MiSTer-$TAG.zip"
shasum -a 256 "$ROOT/build/release/DonutDodo-MiSTer-$TAG.zip"
