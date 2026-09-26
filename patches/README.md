# Patches

Local modifications to upstream sources. The upstream trees themselves are NOT
committed (they live under `work/`, which is ignored) — these patches plus the
pinned upstream versions below are the source of truth.

| # | Applies to | Root to run `patch -p1` from | What |
|---|---|---|---|
| 0001 | SDL2 2.32.10 | `SDL2-2.32.10/` | EGL offscreen: fall back to `eglGetDisplay(EGL_DEFAULT_DISPLAY)` because MiSTer has no `/dev/dri` and `EGL_EXT_device_enumeration` reports zero devices |
| 0002 | FRT 2.1.0 | `godot-3.5.2-stable/` | `arm32v7` build flags: `-mfpu=neon -mtune=cortex-a9` instead of `neon-vfpv4`, which SIGILLs on the DE10-Nano's Cortex-A9 |
| 0003 | SDL2 2.32.10 | `SDL2-2.32.10/` | Offscreen video: the `misterglue` hook (`SDL_mister_glue.*`, `mister_glue_abi.h`) and the DDR present path (`SDL_mister_ddr.*`) |
| 0004 | SDL2 2.32.10 | `SDL2-2.32.10/` | `mister` audio driver — writes the FPGA's DDR audio ring (`gm_audio`) — plus the `configure` line that compiles `src/audio/mister/*.c` |
| 0005 | SDL2 2.32.10 | `SDL2-2.32.10/` | `mister` joystick driver — reads the core's DDR joystick words — plus the driver-array exclusivity fix (see below) |
| 0006 | FRT 2.1.0 | `frt/` | `FRTJOY:` enumeration/button tracing in `sdl2_adapter.h` |
| 0007 | SDL2 2.32.10 | `SDL2-2.32.10/` | Null GL: with `SDL_MISTER_GLUE` set (and `SDL_MISTER_NULL_GL` not `0`) no EGL/GL library is loaded; GL entry points resolve to the glue over stubs that replay Mesa llvmpipe's GLES2 answers (PLAN.md §1l) |

Applied in order (0001, 0003, 0004, 0005, 0007) to a pristine `SDL2-2.32.10.tar.gz`, the SDL2
patches reproduce `work/SDL2-2.32.10/` exactly (checked 2026-09-26).

## Upstream versions

- SDL2 `release-2.32.10` (https://github.com/libsdl-org/SDL)
- FRT `33f739d` "frt 2.1.0 release notes" (https://github.com/efornara/frt)
- Godot `3.5.2-stable`

Configure line for SDL2, from `work/build-sdl2/config.status`:

    ../SDL2-2.32.10/configure --host=arm-linux-gnueabihf --prefix=/src/sdl2-armhf \
      --disable-static --enable-shared \
      CPPFLAGS='-DSDL_JOYSTICK_MISTER=1 -DSDL_AUDIO_DRIVER_MISTER=1' \
      --enable-video-offscreen --enable-video-dummy --enable-video-opengles \
      --enable-video-opengles2 --disable-video-x11 --disable-video-wayland \
      --disable-video-kmsdrm --disable-video-vulkan --disable-video-rpi \
      --disable-video-directfb --disable-pulseaudio --disable-jack --disable-sndio \
      --disable-nas --disable-arts --disable-esd

Built in the `gmloader-armhf-build:bullseye` container with the tree mounted at
`/src`.

## Note on 0005

The exclusivity rule — with `SDL_MISTER_JOY` set the DDR driver is the ONLY
joystick source — is enforced by `SDL_JoystickDriverSuppressed()` at all five
places `SDL_joystick.c` walks the driver array. Enforcing it in
`SDL_JoystickInit()` alone is not enough and was a live bug: `SDL_JoystickDetect()`
calls `Detect()` on every driver whether or not its `Init` ran, so
`LINUX_JoystickDetect` still enumerated the pad that MiSTer holds an exclusive
EVDEV grab on. See PLAN.md §1j.
