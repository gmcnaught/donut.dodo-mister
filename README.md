# Donut Dodo on MiSTer

Donut Dodo (Godot 3.5.2) running on the DE10-Nano under a fabric-accelerated
MiSTer core: the engine's GLES2 canvas draws are decoded into FPGA blitter
commands, and the FPGA owns the frame, the audio ring and the joystick words.

    FRT 2.1.0 / Godot 3.5.2 (GLES2)
      └─ patched SDL2 2.32.10
           ├─ video   : null GL (no Mesa) -> libmisterglue -> fabric
           ├─ audio   : "mister" driver -> DDR audio ring (gm_audio)
           └─ input   : "mister" driver -> the core's DDR joystick words
    FPGA: blitter fabric + scanout, BLTCTRL at 0x3B000000, frame/joystick words at 0x3BF40000

**The game is not included.** Buy it at https://zapposh.itch.io/donut-dodo-retropie-edition;
this repository and its releases contain only the engine, the launcher, the FPGA core and the glue.

| | |
|---|---|
| Gameplay | 60 fps, paced on the core's scanout |
| Audio | 48 kHz stereo through the core's DDR ring |
| Memory | ~96 MB engine RSS, flat (no Mesa) |
| Start-up (core load → 60 frames shown) | 5.4–5.7 s warm, 7.9–8.2 s cold |
| Recovery | a stalled blitter (at start or mid-game) reloads the core and restarts the game |

## Install

Download the latest `DonutDodo-MiSTer-<tag>.zip` from [Releases](https://github.com/gmcnaught/donut.dodo-mister/releases), then:

1. Extract it over the root of the SD card (`/media/fat/`).
2. Copy `DonutDodo.pck` from your download to `/media/fat/games/DonutDodo/gamedata/DonutDodo.pck`.
3. Run **Scripts → DonutDodo_CoresMenu** once. After that, loading **DonutDodo** from the core list (`_Other`) starts
   the game. **Scripts → DonutDodo** also starts it.

The user guide (controls, logs, notes) is [`dist/README.md`](dist/README.md), which ships in the zip as
`games/DonutDodo/README.md`.

## Layout

| Path | What |
|---|---|
| `PLAN.md` | The working record: findings, measurements, tiers and gates. Start here. |
| `src/misterglue/` | The GL glue module (`libmisterglue.so`) — Godot GLES2 state shadow + draw decode |
| `src/vendor/` | The fabric stack shared with gmloader-next / maldita.castilla-mister — see `src/vendor/VENDOR.md` |
| `patches/` | Our diffs against upstream SDL2 and FRT, with the pinned versions — see `patches/README.md` |
| `dist/` | What ships: `games/DonutDodo/launch.sh`, the two `Scripts/` entries, the user README |
| `tools/mister-wrapper/` | `MiSTer_DonutDodo`: upstream Main_MiSTer plus one hook that starts `launch.sh` on core load (`main=`) |
| `tools/mem_wc/` | Write-combining `/dev/mem_wc` driver (GPL-2.0) and its prebuilt module for 6.18.38-MiSTer |
| `scripts/make_release.sh` | Assembles the release zip |
| `scripts/deploy_and_verify.sh` | Push an RBF, reconfigure, screenshot |
| `Makefile` | Cross-builds `libmisterglue.so` (armhf, Cortex-A9) |
| `Dockerfile.frt-build` | The armhf cross image used for SDL2 and the engine |

Not committed: `work/` (upstream trees + build output), `build/`, `_Other/*.rbf` and the Quartus reports.

## Building a release

Requirements: Docker with the `gmloader-armhf-build:bullseye` image (from gmloader-next), and the upstream trees in
`work/` built per `patches/README.md`.

| Input | Source | Default path |
|---|---|---|
| Engine | Godot 3.5.2-stable + FRT `33f739d` + patches 0002, 0006, built by `scripts/build_engine.sh` (no 3D, 5 modules) | `work/godot-3.5.2-prune/bin/godot.frt.opt.arm32v7` |
| SDL2 | SDL2 2.32.10 + patches 0001, 0003–0005, 0007 | `work/build-sdl2/build/.libs/libSDL2-2.0.so.0.3200.10` |
| Glue | `src/`, rebuilt by the release script | `build/rel/libmisterglue.so` |
| Wrapper | `tools/mister-wrapper/build-hps.sh` | `build/mister-wrapper/MiSTer_DonutDodo` |
| Core | maldita.castilla-mister branch `donutdodo/fb-320x240`, `build-rbf.yml`, default `core_variant` | `RBF_SRC=` |

```sh
scripts/build_engine.sh                     # Docker VM needs >= 8 GiB at -j6
tools/mister-wrapper/build-hps.sh
RBF_SRC=_Other/DonutDodo_48k_v224_20260922.rbf scripts/make_release.sh <tag>
# -> build/release/DonutDodo-MiSTer-<tag>.zip, checksums in games/DonutDodo/sha256sums.txt
```


## Measuring on the device

`launch.sh` sources `/tmp/donutdodo_test.env` if it exists (gone after a reboot). For example
`echo 'export MISTER_GLUE_STATS=1' > /tmp/donutdodo_test.env` logs draw/decode/fallback counts every 120 frames.

## Licence

GPL-3.0 with per-path exceptions — see [`LICENSING.md`](LICENSING.md).
