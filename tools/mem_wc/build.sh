#!/bin/sh
# Build mem_wc (write-combining /dev/mem for the fabric window) for the device's
# running MiSTer kernel. Needs Docker and ssh access to the device.
#   tools/mem_wc/build.sh [device]      (default root@192.168.20.81)
# Output: tools/mem_wc/prebuilt/mem_wc-<uname -r>.ko
# The kernel tree is cloned inside a Docker volume (kbuild618): Linux sources
# contain case-only-different file names that a macOS checkout mangles.
set -e
DEV=${1:-root@192.168.20.81}
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(ssh "$DEV" uname -r)                     # e.g. 6.18.38-MiSTer
VER=${REL%%-*}; LV=-${REL#*-}                   # 6.18.38 / -MiSTer
BR=MiSTer-v$(echo "$VER" | cut -d. -f1,2)       # MiSTer-v6.18
ssh "$DEV" 'zcat /proc/config.gz' > "$HERE/device.config"
docker build -q -t kbuild:bullseye -f "$HERE/Dockerfile" "$HERE" >/dev/null
docker volume create kbuild618 >/dev/null
docker run --rm -v kbuild618:/k -v "$HERE":/m kbuild:bullseye bash -c "
set -e
[ -d /k/linux/.git ] || git clone -q --depth 1 --branch $BR https://github.com/MiSTer-devel/Linux-Kernel_MiSTer.git /k/linux
cd /k/linux
grep -q '^SUBLEVEL = ${VER##*.}\$' Makefile || { echo 'kernel tree is not $VER'; exit 1; }
cp /m/device.config .config
K='ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION=$LV'
make -s \$K olddefconfig
make -s -j8 \$K modules_prepare
[ \"\$(make -s \$K kernelrelease)\" = '$REL' ] || { echo 'kernelrelease mismatch'; exit 1; }
mkdir -p /tmp/b && cp /m/mem_wc.c /m/Kbuild /tmp/b/
# MODVERSIONS is off on MiSTer kernels: symbols resolve by name at load time,
# so the missing Module.symvers is expected (KBUILD_MODPOST_WARN).
make -s -C /k/linux M=/tmp/b \$K KBUILD_MODPOST_WARN=1 modules 2>/dev/null
arm-linux-gnueabihf-strip --strip-debug /tmp/b/mem_wc.ko -o /m/prebuilt/mem_wc-$REL.ko
modinfo /m/prebuilt/mem_wc-$REL.ko | grep vermagic
"
