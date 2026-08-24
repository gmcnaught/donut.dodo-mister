# Donut Dodo on MiSTer

Donut Dodo (Godot 3.5.2) running on the DE10-Nano under a fabric-accelerated
MiSTer core: the engine's GLES2 canvas draws are decoded into FPGA blitter
commands, and the FPGA owns the frame, the audio ring and the joystick words.

    FRT 2.1.0 / Godot 3.5.2 (GLES2)
      └─ patched SDL2 2.32.10
           ├─ video   : offscreen EGL (Mesa llvmpipe) -> libmisterglue -> fabric
           ├─ audio   : "mister" driver -> DDR audio ring (gm_audio)
           └─ input   : "mister" driver -> the core's DDR joystick words
    FPGA: blitter fabric + scanout, BLTCTRL at 0x3B000000, frame/joystick words at 0x3BF40000

## Layout

| Path | What |
|---|---|
| `PLAN.md` | The working record: findings, measurements, tiers and gates. Start here. |
| `src/misterglue/` | The GL glue module (`libmisterglue.so`) — Godot GLES2 state shadow + draw decode |
| `src/vendor/` | The fabric stack shared with gmloader-next / maldita.castilla-mister — see `src/vendor/VENDOR.md` |
| `patches/` | Our diffs against upstream SDL2 and FRT, with the pinned versions — see `patches/README.md` |
| `games/Donut Dodo/launch.sh` | The on-device launcher: addresses, driver selection, one-engine guard |
| `scripts/deploy_and_verify.sh` | Push an RBF, reconfigure, screenshot |
| `Makefile` | Cross-builds `libmisterglue.so` (armhf, Cortex-A9) |
| `Dockerfile.frt-build` | The armhf cross image used for SDL2 and the engine |

Not committed: `work/` (upstream trees + build output), `_Other/*.rbf` and the
Quartus reports. The RBF is a release artifact; `patches/` plus the pinned
upstream versions are the source of truth for everything built from source.

## Status

The fabric renders the game at 60 fps locked to scanout, audio drains on rate,
and the pad enumerates through the DDR driver. Open items are tracked at the end
of `PLAN.md` §1j — most importantly, this launcher has no fabric-wedge recovery
gate, which `maldita.castilla-mister` does carry.

The game itself is not here and is not redistributable: buy it at
https://zapposh.itch.io/donut-dodo-retropie-edition and put `DonutDodo.pck` in
`gamedata/` on the device.
