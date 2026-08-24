#!/bin/bash
#
# Donut Dodo on MiSTer — launcher.
#
# The engine is stock Godot 3.5.2 + FRT 2.1.0 (ARMv7). Everything MiSTer-specific
# lives below it: our libSDL2 provides the video/present path, and libmisterglue
# turns Godot's GLES2 canvas draws into FPGA fabric commands. Mesa stays loaded
# and takes any draw the glue declines, so an unsupported effect degrades to
# software rather than failing.
#
set -u

GAMEDIR=/media/fat/games/donutdodo
LOGDIR=/media/fat/logs/DonutDodo
mkdir -p "$LOGDIR" "$GAMEDIR/conf"

cd "$GAMEDIR" || exit 1

# --- one engine at a time -----------------------------------------------------
# Two engines on one fabric control block corrupts it (C_DONE runs backwards),
# so refuse to start over a live one rather than racing it.
#
# "Live one" is not just another copy of THIS engine. Every fabric port on this
# box drives the same BLTCTRL at 0x3B000000, so a Maldita Castilla gmloader left
# running from an earlier core is just as fatal, and it is the case that actually
# happened (2026-08-23/24: both logs full of mutual submit timeouts, each engine
# reading the other's sequence out of C_DONE, every frame dropped on both sides,
# black screen here). Match on the whole family, not on our own binary name.
# pidof, not `ps w | grep`: pidof matches the EXECUTABLE name, so it cannot be
# fooled by an unrelated command line that merely mentions one (a shell running
# `killall -9 frt_3.5.2` matched the grep form and refused a legitimate launch).
for name in frt_3.5.2 gmloader; do
    pid=$(pidof "$name" 2>/dev/null | awk '{print $1}')
    [ -n "$pid" ] || continue
    echo "launch.sh: a fabric engine is already running ($name pid $pid), refusing to start a second" >> "$LOGDIR/donutdodo.log"
    exit 0
done

# --- video: Mesa on a surfaceless EGL context; the fabric owns the frame -------
export LD_LIBRARY_PATH="$GAMEDIR/libs:$GAMEDIR/mesa"
export SDL_VIDEODRIVER=offscreen          # no DRM/KMS device on MiSTer
export SDL_VIDEO_EGL_DEFAULT_DISPLAY=1    # our fallback: eglQueryDevicesEXT finds nothing here
export EGL_PLATFORM=surfaceless
export LIBGL_DRIVERS_PATH="$GAMEDIR/mesa"
export GALLIUM_DRIVER=llvmpipe

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
# Where THIS core publishes its video/joystick words (Maldita.sv FB_QW_BASE).
# The OpenBOR core uses 0x3A000000; this fabric core moved to 0x3BF40000 to stay
# clear of gmloader's NativeVideoWriter region. Reading the wrong base fails
# silently — the words just never change — so it is stated, not assumed.
export SDL_MISTER_JOY_BASE=0x3BF40000

# --- fabric -------------------------------------------------------------------
export SDL_MISTER_GLUE=1
export MISTER_GLUE_FABRIC=1
export GMLOADER_RASTER=mfgpu              # selects the fabric back-end over the CPU one

# --- engine -------------------------------------------------------------------
export GODOT_SILENCE_ROOT_WARNING=1
export XDG_CONFIG_HOME="$GAMEDIR/conf"
export XDG_DATA_HOME="$GAMEDIR/conf"

exec ./frt_3.5.2 --main-pack gamedata/DonutDodo.pck --video-driver GLES2 \
    >> "$LOGDIR/donutdodo.log" 2>&1
