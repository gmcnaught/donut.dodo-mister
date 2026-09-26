#!/bin/bash
#
# Donut Dodo on MiSTer — engine launcher. Started by MiSTer_DonutDodo when the
# DonutDodo core loads (MiSTer.ini [DonutDodo] main=, turned on by
# Scripts/DonutDodo_CoresMenu.sh), or by Scripts/DonutDodo.sh.
#
# The engine is stock Godot 3.5.2 + FRT 2.1.0 (ARMv7). Everything MiSTer-specific
# lives below it: our libSDL2 provides the video/present path, and libmisterglue
# turns Godot's GLES2 canvas draws into FPGA fabric commands. GL is a null
# implementation inside our libSDL2 (patch 0007): no Mesa is loaded.
#
# What this script adds around the engine (after cash.cow.dx-mister's launch.sh):
#   - one engine at a time (lock + reap of any fabric engine), FPGA-ready wait
#   - mem_wc: write-combining DDR mapping, loaded if absent, NEVER unloaded
#   - fabric gate: if the blitter stops retiring work after start, reload the
#     core and retry (up to 4 times) — PLAN.md §1j item 3
#   - watchdog: stop the engine if another core is loaded from the OSD
set -u

GAMEDIR=/media/fat/games/DonutDodo
LOGDIR=/media/fat/logs/DonutDodo
LOG="$LOGDIR/donutdodo.log"
CORENAME=DonutDodo
RBF_GLOB="/media/fat/_Other/DonutDodo_*.rbf"
# Kept as frt_3.5.2: the other fabric ports' launchers reap this name.
ENGINE=frt_3.5.2
PCK=gamedata/DonutDodo.pck
FABRIC_CTRL=0x3B000000      # C_SUBMIT
FABRIC_DONE=0x3B000028      # C_DONE
RETRY_MARK=/tmp/donutdodo_fabric_retry
LOCKDIR=/tmp/donutdodo-launch.lock
MAX_RETRIES=4

# Interruptible sleep, so a SIGTERM runs the cleanup trap without waiting for a
# foreground sleep.
nap() { sleep "$1" & wait $!; }

mkdir -p "$LOGDIR" "$GAMEDIR/conf"
# Measurement hook: extra env for the engine, e.g. MISTER_GLUE_STATS=1.
# In /tmp, so it never survives a reboot.
[ -f /tmp/donutdodo_test.env ] && . /tmp/donutdodo_test.env
cd "$GAMEDIR" || exit 1

# Only on our core (e.g. a Scripts run racing a core change).
if [ "$(cat /tmp/CORENAME 2>/dev/null)" != "$CORENAME" ]; then
	echo "$(date) launch.sh: core is '$(cat /tmp/CORENAME 2>/dev/null)', not $CORENAME — not starting" >> "$LOGDIR/launch.log"
	exit 0
fi

# --- one launcher / one engine -------------------------------------------------
if ! mkdir "$LOCKDIR" 2>/dev/null; then
	owner=$(cat "$LOCKDIR/pid" 2>/dev/null)
	if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then
		echo "launch.sh: another launcher (pid $owner) is running — standing down"
		exit 0
	fi
fi
echo $$ > "$LOCKDIR/pid"
# Two engines on one fabric control block corrupt it: each reads the other's
# sequence out of C_DONE (PLAN.md §1j item 1). Every fabric port drives the same
# BLTCTRL, so reap the whole family. pidof matches the executable name only.
for name in $ENGINE gmloader cashcowdx; do
	for pid in $(pidof "$name" 2>/dev/null); do
		echo "launch.sh: stopping a running fabric engine ($name pid $pid)"
		kill "$pid" 2>/dev/null
		sleep 2
		kill -9 "$pid" 2>/dev/null
	done
done

mv -f "$LOG" "$LOGDIR/donutdodo.prev.log" 2>/dev/null
exec >> "$LOG" 2>&1
echo "=== $(date) launch.sh (pid $$) CORENAME='$(cat /tmp/CORENAME 2>/dev/null)' kernel=$(uname -r)"

[ -f "$PCK" ] || { echo "missing $GAMEDIR/$PCK — see README.md"; rm -rf "$LOCKDIR"; exit 1; }

# --- FPGA ready (bit 31 of the HPS GPI is low once the core is configured) ----
waited=0
while v=$(busybox devmem 0xFF706014 32 2>/dev/null) && [ -n "$v" ] && [ $((v & 0x80000000)) -ne 0 ]; do
	[ "$waited" -ge 20 ] && { echo "FPGA still not ready after 20s — starting anyway"; break; }
	nap 1; waited=$((waited + 1))
done
[ "$waited" -gt 0 ] && nap 1

# --- mem_wc (optional; the fabric backend falls back to /dev/mem) -------------
# Load only if nothing has: never rmmod a mem_wc — a process can keep a live
# mapping after closing the fd, and unloading under it once hung a device.
KO="$GAMEDIR/mem_wc-$(uname -r).ko"
if [ ! -e /dev/mem_wc ]; then
	if [ -f "$KO" ]; then
		insmod "$KO" phys_base=0x3B000000 phys_size=0x01000000 2>/dev/null \
			&& echo "mem_wc: loaded ($KO)" || echo "mem_wc: insmod failed — strongly-ordered DDR mapping"
	else
		echo "mem_wc: no module for kernel $(uname -r) — strongly-ordered DDR mapping"
	fi
else
	echo "mem_wc: already present"
fi

engine_pid=""
cleanup() {
	# Background: a SIGKILL of this script must not skip the engine kill.
	[ -n "$engine_pid" ] && { kill "$engine_pid" 2>/dev/null; ( sleep 2; kill -9 "$engine_pid" 2>/dev/null ) & }
	rm -rf "$LOCKDIR"
	wait
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# --- video: the fabric owns the frame; GL is a null implementation -------------
export LD_LIBRARY_PATH="$GAMEDIR/libs"
export SDL_VIDEODRIVER=offscreen          # no DRM/KMS device on MiSTer
# A/B only (test.env): SDL_MISTER_NULL_GL=0 MESA_DIR=<llvmpipe runtime> loads Mesa
# on a surfaceless EGL context again. It is not shipped: its output never reaches
# the scanout, and llvmpipe's unflushed scene grew ~1.4 MB/min (PLAN.md §1l).
if [ "${SDL_MISTER_NULL_GL:-1}" = 0 ] && [ -n "${MESA_DIR:-}" ]; then
	export SDL_MISTER_NULL_GL=0
	export LD_LIBRARY_PATH="$GAMEDIR/libs:$MESA_DIR"
	export SDL_VIDEO_EGL_DEFAULT_DISPLAY=1    # eglQueryDevicesEXT finds nothing here
	export EGL_PLATFORM=surfaceless
	export LIBGL_DRIVERS_PATH="$MESA_DIR"
	export GALLIUM_DRIVER=llvmpipe
fi

# Scanout geometry is the core's wire contract, and must match the RBF's
# FB_W/FB_H and the host's BLT_FB_WIDTH/HEIGHT.
export SDL_MISTER_SCANOUT=320x240
export SDL_MISTER_DDR=0                   # the fabric publishes frames, not our readback

# --- audio: the FPGA drains a DDR ring; there is no usable ALSA path under a core.
# The core's gm_audio SRC_RATE must equal the rate our driver opens (48 kHz).
export SDL_AUDIODRIVER=mister

# --- input: read the core's published joystick words rather than /dev/input, so
# the button assignments the user made in the MiSTer OSD are what the game sees.
export SDL_MISTER_JOY=1
export SDL_MISTER_JOY_PLAYERS=1
# Where this core publishes its video/joystick words (Maldita.sv FB_QW_BASE).
# Reading the wrong base fails silently — the words just never change.
export SDL_MISTER_JOY_BASE=0x3BF40000

# --- fabric -------------------------------------------------------------------
export SDL_MISTER_GLUE=1
export MISTER_GLUE_FABRIC=1
export GMLOADER_RASTER=mfgpu              # selects the fabric back-end over the CPU one

# --- engine -------------------------------------------------------------------
export GODOT_SILENCE_ROOT_WARNING=1
export XDG_CONFIG_HOME="$GAMEDIR/conf"
export XDG_DATA_HOME="$GAMEDIR/conf"

start_engine() {
	# Engine output goes through a pipe to a logger process: /media/fat is
	# mounted sync, so a print written straight to the log would block the
	# engine's main thread on an SD write (cash.cow.dx-mister PLAN §6.25).
	./$ENGINE --main-pack "$PCK" --video-driver GLES2 > >(exec cat) 2>&1 &
	engine_pid=$!
	echo "engine: started pid $engine_pid"
}

# Blitter still retiring work? (done advances, or nothing is outstanding)
fabric_ok() {
	local d0 d1 s1
	d0=$(busybox devmem $FABRIC_DONE 32 2>/dev/null); nap 8
	d1=$(busybox devmem $FABRIC_DONE 32 2>/dev/null); s1=$(busybox devmem $FABRIC_CTRL 32 2>/dev/null)
	echo "fabric gate: done $d0 -> $d1 (submit $s1)"
	[ "$d1" != "$d0" ] || [ "$d1" = "$s1" ]
}

# Reload the core via the menu core, from a detached helper. After the reload
# MiSTer_DonutDodo starts a new launch.sh when main= is on; otherwise the helper
# starts it.
reload_core() {
	local rbf
	rbf=$(ls -t $RBF_GLOB 2>/dev/null | head -1)
	[ -n "$rbf" ] && [ -p /dev/MiSTer_cmd ] || return 1
	setsid sh -c '
		echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd
		w=0; while [ "$(cat /tmp/CORENAME 2>/dev/null)" != MENU ] && [ $w -lt 20 ]; do sleep 1; w=$((w+1)); done
		echo "load_core $1" > /dev/MiSTer_cmd
		w=0; while [ "$(cat /tmp/CORENAME 2>/dev/null)" != "$2" ] && [ $w -lt 30 ]; do sleep 1; w=$((w+1)); done
		sleep 2
		[ -x "$4" ] && grep -q "^main=$4" /media/fat/MiSTer.ini 2>/dev/null || exec "$3"
	' reload "$rbf" "$CORENAME" "$0" "$GAMEDIR/MiSTer_DonutDodo" < /dev/null >> "$LOG" 2>&1 &
	# Hold until the core is gone, so this launcher exits before the reloaded core starts the next one.
	local waited=0
	while [ "$(cat /tmp/CORENAME 2>/dev/null)" = "$CORENAME" ] && [ $waited -lt 20 ]; do nap 1; waited=$((waited+1)); done
}

attempt=$(cat "$RETRY_MARK" 2>/dev/null); case "$attempt" in ''|*[!0-9]*) attempt=0 ;; esac
start_engine
waited=0
while [ $waited -lt 60 ] && ! grep -q "fabric bring-up" "$LOG" 2>/dev/null; do
	kill -0 "$engine_pid" 2>/dev/null || { echo "engine exited during start-up"; exit 1; }
	nap 1; waited=$((waited + 1))
done
if ! fabric_ok; then
	if [ "$attempt" -lt "$MAX_RETRIES" ]; then
		echo $((attempt + 1)) > "$RETRY_MARK"
		echo "fabric gate: WEDGED — reloading the core, attempt $((attempt + 1))/$MAX_RETRIES"
		kill "$engine_pid" 2>/dev/null; nap 2; kill -9 "$engine_pid" 2>/dev/null; engine_pid=""
		rm -rf "$LOCKDIR"
		reload_core
		exit 1
	fi
	echo "fabric gate: still wedged after $attempt attempts — leaving the engine running"
fi
rm -f "$RETRY_MARK"

# --- watchdog --------------------------------------------------------------------
# Another core loaded from the OSD -> stop the engine.
# Fabric wedge mid-game -> reload the core. The wedge PLAN.md §1j saw came ~2 min
# into play, after a clean start: C_DONE frozen with C_SUBMIT ahead of it. While
# the fabric is healthy C_DONE moves every frame, or equals C_SUBMIT when idle.
STALL_S=${DONUTDODO_STALL_S:-6}
stall=0; last_done=""
while kill -0 "$engine_pid" 2>/dev/null; do
	cur=""; read -r cur < /tmp/CORENAME 2>/dev/null
	if [ "$cur" != "$CORENAME" ]; then
		echo "watchdog: core changed to '$cur' — stopping the engine"
		kill "$engine_pid" 2>/dev/null; nap 2; kill -9 "$engine_pid" 2>/dev/null
		break
	fi
	d=$(busybox devmem $FABRIC_DONE 32 2>/dev/null); s=$(busybox devmem $FABRIC_CTRL 32 2>/dev/null)
	if [ -n "$d" ] && [ "$d" = "$last_done" ] && [ "$d" != "$s" ]; then
		stall=$((stall + 1))
	else
		stall=0
	fi
	last_done=$d
	if [ "$stall" -ge "$STALL_S" ]; then
		echo "watchdog: fabric WEDGED (done $d, submit $s for ${stall}s) — reloading the core"
		kill "$engine_pid" 2>/dev/null; nap 2; kill -9 "$engine_pid" 2>/dev/null; engine_pid=""
		echo 1 > "$RETRY_MARK"
		rm -rf "$LOCKDIR"
		reload_core
		exit 1
	fi
	nap 1
done
wait "$engine_pid" 2>/dev/null
echo "engine: exited ($?)"
