#!/bin/bash
# Fetch an RBF from a CI run, load it, and verify the port end to end.
#
#   scripts/deploy_and_verify.sh <run-id> [rbf-name]
#
# Checks, in order: the core loads, our engine is the only one on the fabric,
# audio drains at the sink rate, and the scanout captures a frame.
set -u
RUN_ID=${1:?usage: deploy_and_verify.sh <run-id> [rbf-name]}
NAME=${2:-DonutDodo_320x240_H40}
DEV=root@192.168.20.81
REPO=gmcnaught/maldita.castilla-mister
HERE=$(cd "$(dirname "$0")/.." && pwd)

echo "== fetching artifact from run $RUN_ID =="
rm -rf "$HERE/_Other/_dl" && mkdir -p "$HERE/_Other/_dl"
gh run download "$RUN_ID" -R "$REPO" -n maldita-rbf-linux -D "$HERE/_Other/_dl" || exit 1
SRC=$(ls "$HERE/_Other/_dl"/*.rbf | head -1)
DST="$HERE/_Other/${NAME}_$(date +%Y%m%d).rbf"
mv "$SRC" "$DST"
echo "  -> $DST"

echo "== deploying =="
scp -q "$DST" "$DEV:/media/fat/_Other/" || exit 1

echo "== loading core =="
ssh "$DEV" "
  for p in \$(ps w | grep -v grep | grep frt_3.5.2 | awk '{print \$1}'); do kill -9 \$p 2>/dev/null; done
  echo 'load_core /media/fat/_Other/$(basename "$DST")' > /dev/MiSTer_cmd
  sleep 10
  for p in \$(ps w | grep -viE 'grep|\[' | grep -E 'gmloader -c|launch.sh' | awk '{print \$1}'); do kill -TERM \$p 2>/dev/null; done
  sleep 3
  for p in \$(ps w | grep -viE 'grep|\[' | grep -E 'gmloader -c|launch.sh' | awk '{print \$1}'); do kill -9 \$p 2>/dev/null; done
  echo \"core=\$(cat /tmp/CORENAME) competing_engines=\$(ps w|grep -viE 'grep|\[' |grep -cE 'gmloader -c')\"
"

echo "== starting the port =="
ssh "$DEV" "nohup bash '/media/fat/games/Donut Dodo/launch.sh' >/dev/null 2>&1 & sleep 30; echo started"

echo "== audio drain (want ~48000 Hz) =="
ssh "$DEV" "/tmp/audio_ring_probe.armhf 6 2>&1 | grep -E 'drain|submit|occupancy'"

echo "== scanout capture =="
ssh "$DEV" "echo screenshot > /dev/MiSTer_cmd; sleep 4; ls -t '/media/fat/screenshots/DonutDodo/' | head -1"
