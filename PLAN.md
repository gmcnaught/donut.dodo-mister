# Donut Dodo on MiSTer — port plan

Status: design only, nothing built. Written 2026-08-22.
Sibling repos referenced: `../gmloader-next`, `../cursed.castilla-mister`,
`../solarus-mister`, `../mister-fpga-blitter`.

---

## 1. Verified findings

**Game / runtime (Observed)**

| Fact | Evidence |
|---|---|
| Game is **Godot 3.5.2**, pck format v1, 32.8 MB | `GDPC` header of `~/Downloads/DonutDodo/gamedata/DonutDodo.pck` → `3.5.2` |
| Base viewport **320×240**, `stretch/mode=viewport`, `aspect=keep`, fullscreen | `project.binary` decoded from the pck |
| Renderer **GLES2**; `rendering/threads/thread_model=2` (multithreaded); 2D batching on (`max_join_item_commands=4`, `single_rect_fallback=true`) | same |
| Audio driver **ALSA**, mix rate 48000, output latency 64 ms | same |
| Assets: 482 files — 69 `.stex`, 83 `.tscn`, 80 `.gdc`, 28 `.sample`, 13 `.oggstr`; largest single asset is `DefaultTheme.tres` (6.95 MB); ETC (not ETC2/S3TC) VRAM compression enabled on import | pck index dump |
| PortMaster launcher runs `frt_3.5.2 --main-pack gamedata/DonutDodo.pck` under gptokeyb, with `FRT_NO_EXIT_SHORTCUTS` | `ports/donut.dodo/Donut Dodo.sh` |

**The runner (Observed — this settles the extract-vs-compile question)**

- `frt_3.5.2.squashfs` (12.7 MB, from PortMaster-Runtime `runtimes` release) contains
  **exactly one file**: `frt_3.5.2`, a **34 MB ELF 64-bit aarch64** executable
  (`interpreter /lib/ld-linux-aarch64.so.1`, stripped, built 2023-04-21).
- **There is no armhf binary in the runtime.** MiSTer's HPS is `armv7l`. Extraction is dead;
  we compile.
- That binary's `DT_NEEDED`: `libSDL2-2.0.so.0`, `libstdc++`, `libz`, `libpthread`, `libdl`,
  `libm`, `libgcc_s`, `libc`. So PortMaster's FRT is the **SDL2-backed** FRT, GLES2 renderer.
  EGL/GLES are pulled in at runtime by SDL, not linked.

**FRT upstream (Observed)**

- `efornara/frt` master is a thin Godot-3 platform over SDL2 — the whole backend is
  `sdl2_adapter.h` + `sdl2_godot_map.h` (plus `frt_godot.cc`, `frt_exe.cc`).
  That single file is our interception point.
- Official build: `scripts/Dockerfile` (fedora:39) using **Godot's own buildroot SDKs**
  (`arm-godot-linux-gnueabihf_sdk-buildroot`, godot-2023.08.x-4) plus a **self-built SDL2
  2.32.10** per arch, driven by `frt-pull` / `frt-compile` against `efornara/godot3`.
  Old glibc target → runs on MiSTer's 2.31.
- Prebuilt releases exist for **3.6.1-1 / 3.6.2-1 arm32** only; 3.5.x lives on SourceForge.
- Known upstream issue: arm32 templates "Illegal instruction" on some older boards
  (godotengine/godot#112189). Cortex-A9 is a plausible victim — we control `-march`/`-mfpu`
  when we build, so this is a build-flag risk, not a blocker.

**Device (Observed, `192.168.20.81`)**

- `armv7l`, kernel 5.15.1-MiSTer, **glibc 2.31** (matches the existing Mesa constraint).
- `/usr/lib` already has `libSDL2-2.0.so.0.14.0`, `libasound.so.2`, `libpng16`, `libvorbis*`.
- `/media/fat/games/gmloader/mesa/` already holds the armhf **llvmpipe** stack:
  `libEGL.so.1`, `libGLESv2.so.2`, `libglapi.so.0`, `swrast_dri.so`, `libdrm.so.2`,
  `libtinfo.so.6`.
- No `/dev/dri`, no X11 (per gmloader-next notes) → **surfaceless EGL is the only GL path**.

---

## 1b. Tier-0 results (2026-08-22, measured on device)

Two findings changed the design; both are settled by hardware evidence.

**Prebuilt binaries are unusable — we compile everything.** Upstream's
`frt_3.6.2-1_arm32_release` SIGILLs instantly on MiSTer (exit 132, no output).
`readelf -A` explains it: `Tag_CPU_arch: v8`, `Tag_FP_arch: FP for ARMv8`,
`NEON for ARMv8`, MPext + Virtualization — an **ARMv8-A** build. The HPS is a
Cortex-A9 (ARMv7-A). This is godotengine/godot#112189 seen from the other side.
FRT's own `frt_arch=arm32v7` preset is *also* wrong for this chip: it selects
`-mfpu=neon-vfpv4`, and VFPv4 (FMA) is Cortex-A7/A15+. We build with
`-march=armv7-a -mfpu=neon -mtune=cortex-a9` (patch `0002`).

**All MiSTer specifics fit inside SDL2 — the engine stays stock.** FRT 2.x
dynamically links SDL2 (`pkg-config sdl2`), so a custom `libSDL2-2.0.so.0`
changes the platform without touching Godot or FRT source. Two patches ship there:

- `0001` — SDL's offscreen driver initialises EGL through `eglQueryDevicesEXT`
  (`EGL_PLATFORM_DEVICE_EXT`), which reports **zero devices** on MiSTer (no
  `/dev/dri`). Falls back to `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, which Mesa
  resolves via `EGL_PLATFORM=surfaceless`.
- `src/video/offscreen/SDL_mister_ddr.c` — `SDL_GL_SwapWindow` copies the frame
  into the FPGA's DDR3 double-buffer (RGB565, 320x240, control word after the
  pixels), with optional frame pacing. Inert unless `SDL_MISTER_DDR=1`.

**Verified on hardware** (OpenBOR_7533 core loaded, engines stopped):

| Check | Result |
|---|---|
| SDL video driver | `offscreen`, EGL surfaceless fallback taken |
| GL stack | `llvmpipe (LLVM 11.0.0, 128 bits)`, `OpenGL ES 3.2 Mesa 21.3.9` |
| clear + readback, 320x240 x60 | 373 ms uncontended (160.9 fps); 484 ms with RGBA8888->RGB565 convert + DDR write (124.0 fps) |
| FPGA consumed the frames | control word `0x3A000000` climbing 0x2C1 -> 0x4A4 -> 0x77C at the paced 59.92 Hz |
| Scanout | core screenshot shows the GL clear colour — full path GL -> DDR -> FPGA -> HDMI works |

**Host core: OpenBOR_7533, no FPGA work needed.** `../MiSTer_OpenBOR_7533`
already scans 320x240 RGB565 out of `0x3A000040`/`0x3A040040` at 59.92 Hz
(Genesis H40 timing), publishes joystick words at `+0x008`/`+0x018`, and drains a
48 kHz stereo audio ring at `0x3A0D0000` — matching Donut Dodo's 320x240 / 48 kHz
exactly. Its `openbor_video_reader.sv` is the same reader gmloader targets.

Open item: the core's screenshot is 320x224, so the visible window may crop 8
lines top/bottom of our 240-line frame. Check against real game content.

Deferred to their own steps, both implemented inside our SDL2 rather than the engine:
input (SDL joystick driver reading the DDR joy words, so MiSTer's OSD mapping is
inherited) and audio (SDL audio driver writing the DDR ring instead of ALSA).

## 1c. Tier-1 results — it runs, and where the time goes

**The game boots and renders on hardware.** Godot 3.5.2 + FRT 2.1.0, cross-built
for ARMv7-A, running the retail `DonutDodo.pck` under the OpenBOR_7533 core; the
hall-of-fame screen came back off the FPGA scanout pixel-correct (no flip, no
crop artefacts). Engine binary: 25.6 MB, `Tag_CPU_arch v7` / VFPv3 / NEONv1,
max `GLIBC_2.29`, and its only non-libc dependency is our `libSDL2-2.0.so.0`.

**Frame rate: ~13.7 fps steady state** (120-frame windows, 320x240). Startup to
first frame is ~25 s. Profile, measured through wrappers installed at SDL's
`GL_GetProcAddress` (Godot resolves every GLES2 entry point there, so this hook
sees all of them):

| Component | Cost/frame |
|---|---|
| `glDrawArrays` (6-7 calls) | 20-23 ms |
| `glDrawElements` (12-18 calls) | 4.0-4.8 ms |
| `glReadPixels` in present (forces llvmpipe's deferred raster) | 38 ms |
| RGBA8888 -> RGB565 convert + DDR copy | 2.7 ms |
| everything else (state, buffers, FBO switches) | ~0.8 ms |

**The engine is not the problem — the software GL is.** No-op'ing just the draw
calls (`SDL_MISTER_NULLGL=1`) leaves Godot doing all of its own per-frame work
and yields **350-480 fps**, i.e. Godot itself costs ~2.5 ms/frame. Mesa/llvmpipe
accounts for ~98% of the frame. Corroborating measurements:

- `GALLIUM_NUM_THREADS=1` vs `2`: identical fps -> not raster-thread-bound.
- Process consumes ~1.0 of 2 cores; thread ticks split main 2497 / llvmpipe-0 716
  / llvmpipe-1 700 -> the main thread (Mesa front-end) dominates, raster is secondary.
- `--render-thread unsafe` (single-threaded): no change.
- RGB565 default framebuffer (`SDL_MISTER_565=1`): no change — Godot renders into
  its own RGBA8 viewport FBO, so the default framebuffer's format barely matters.
- Dropping our whole present path: 13.7 -> 15.2 fps, so readback+copy costs ~7 ms
  of real work; the other 31 ms attributed to `glReadPixels` is deferred raster.

**Budget for 60 fps:** 16.7 ms/frame, of which the engine needs ~2.5 ms. Rendering
must therefore fit in ~14 ms — it currently takes ~67 ms. Software GL cannot close
that gap; a ~5x rendering speedup is needed, which is what removing Mesa entirely
buys. Tier 2 is therefore required, and its ceiling is high (~400 fps engine-side).

**Tier 2 gets a better hook than gmloader had.** gmloader intercepts GLES2 by
thunking an Android binary. Here, Godot resolves GL through *our* SDL's
`GL_GetProcAddress` — already wrapped and profiling — so the blit-command backend
(`../mister-fpga-blitter/host/blt_emitter.c`) can be driven from inside libSDL2
with no engine patch at all. An intermediate option worth costing first: a
hand-written NEON 2D blitter for the same intercepted subset, which needs no RTL
work; the game draws only ~25 textured quads per frame.

## 1d. Tier-2 (mfgpu) progress — 2026-08-22

Going straight to the fabric, per decision. State: **decode proven correct, fabric
executing our commands, blocked on framebuffer geometry.**

**Built and working**

- `src/vendor/` — gmloader-next's `RasterBackend` seam, `raster_backend_mfgpu.cpp`
  (~4.1k lines) and the mfgpu protocol (`blt_emitter`/`blt_wire`/`blitter_ref`),
  vendored and cross-compiling clean for ARMv7. Pinned to gmloader-next's copy,
  which is `TRILIST=10`/`SET_TARGET=11` — the standalone `mister-fpga-blitter`
  repo has renumbered these to 12/13 and does NOT match the deployed RBF.
- `src/misterglue/` — `libmisterglue.so`: a GLES2 state shadow plus a decode of
  Godot 3.5's canvas vertex path (transcribed from the generated `canvas.glsl`
  this game compiles), behind a small ABI our SDL2 dlopens. SDL asks it to
  resolve each GL entry point; unhandled calls stay on Mesa, so fallback is per
  draw, not per frame.
- **Decode coverage: 100%** — 3240/3240 draws per 120-frame window, zero
  fallbacks, on the title and hall-of-fame screens. Getting there needed: the
  `m_`-prefixed-uniform test for user shaders (testing `SCREEN_TEXTURE` is wrong —
  the stock canvas fragment shader declares that sampler regardless), and
  mirroring glyph-atlas sub-uploads in every format the font backend emits.
- **The decode is correct**, proven by rendering the same decoded stream through
  the software back-end: the title screen comes out pixel-correct
  (`work/sw_appsurface.png`).
- **The fabric executes our commands**: bring-up succeeds against the Maldita
  RBF ("cleared ring=511KiB heap=15104KiB"), C_SUBMIT climbs, frames advance,
  and the engine runs unthrottled at ~66 fps with the fabric owning drawing.

**Blocker: the fabric is 288x216, Godot's viewport is 320x240.**
`FB_W/FB_H` is a wire contract shared by the RTL, the refmodel and the host. The
TRILIST-capable cores (Maldita, Cursed) are 288x216; Solarus is 320x240 but has
no TRILIST. Godot's render target is fixed at the project's 320x240 base size, so
scene draws overflow the fabric's app surface and the composite samples past its
edge — which is exactly the streaked output in `work/fb_fabric2.png`.
`override.cfg` cannot fix it: Godot does not read one when the project is loaded
via `--main-pack`.

**Next step: a 320x240 TRILIST core.** Take the Maldita/Cursed fpga tree, set
`FB_W/FB_H` to 320/240 and the refmodel's `BLT_FB_WIDTH/HEIGHT` to match (the
sim's contract-check enforces the pairing), rebuild the RBF (Quartus 17.0 Lite,
self-hosted Windows runner). BRAM cost is roughly +30 KB on the WORK framebuffer
over the 288x216 build, against Cursed's measured 61% utilisation. Everything
above the RBF is already proven and needs no change for it.

Two engine-side items still open, both deferred behind the geometry fix: the
composite V-flip (the vendored backend applies one for GameMaker's bottom-origin
FBO convention — correct for us too, but verify on real output), and audio/input.

## 1e. Tier-2 complete — the fabric renders the game (2026-08-23)

**Donut Dodo runs on the FPGA fabric at 320x240, locked to the scanout.**

| Measure | llvmpipe (Tier 1) | mfgpu fabric (Tier 2) |
|---|---|---|
| Frame rate | 13.7 fps | **60.1 fps**, 1:1 with scanout (601 submits / 601 scanout frames per 10 s) |
| Engine CPU | ~1.0 core (of 2) | **0.33 core** — 16.5% of the A9 |
| Rasterization | Mesa/llvmpipe on the A9 | FPGA; the A9 never touches a pixel |

Verified from the real scanout via MiSTer screenshots (320x240, confirming the
new core's timing), showing attract mode advancing between captures.

**The core came from Linux CI.** `build-linux` (ubuntu-latest +
`raetro/quartus:17.0`) built it in 23 min with the production gates passing — no
Windows runner involved. Branch `donutdodo/fb-320x240` of
`maldita.castilla-mister` carries the two-file geometry change; artifact saved as
`_Other/DonutDodo_320x240_20260823.rbf`. Resources: ALM 43%, RAM blocks 78%,
memory bits 54%, worst slack -0.159 ns (this core's fabric clock closes slightly
negative by habit and runs correctly on hardware).

**Two host bugs found and fixed during bring-up**

1. `present()` already ends the frame (`mf_present` -> `mf_frame_end`); calling
   `frame_end()` after it submitted a second, empty batch every frame — visible
   as a submit rate at exactly twice the frame rate.
2. The engine free-runs at ~167 fps against a 60 Hz scanout, so the glue now
   paces to 59.92 Hz.

**A measurement trap worth recording.** For several hours the display looked
frozen: `fbdump` read byte-identical framebuffers while `C_SUBMIT`/`C_DONE` and
the scanout counter all advanced. The dump was reading the wrong memory. The
`0x3A000040` buffers belong to the *software* video producer; on the fabric path
gmloader's main loop deliberately presents nothing ("the core composites into
on-chip BRAM and scans itself out"), so those buffers hold whatever the last
software-rendered run left behind and never change. The fabric's output only
appears on the scanout. Read it with a MiSTer screenshot, not `/dev/mem` — and
note screenshots stop being written if the `main=` wrapper's children have been
killed around, which a reboot fixes.

Still open: audio, input, and the H40-exact timing/PLL change (the core currently
holds H_TOTAL at 380 rather than Genesis' 420, so a CRT shows a slightly wide
image).

## 1f. Input, audio, and H40 timing (2026-08-23)

**Input — an SDL joystick driver reading the core's DDR words.**
`src/joystick/SDL_sysjoystick_mister.c` in our SDL2. On MiSTer the pad belongs to
the framework: the HPS reads it, applies the mapping the user set in the core's
OSD, and the core publishes the normalised word to DDR (`+0x008` P1, `+0x018` P2;
bit0 right, bit1 left, bit2 down, bit3 up, bit4.. the CONF_STR J1 buttons).
Reading that — rather than `/dev/input` — is what makes the OSD's assignments
apply to the game. The driver reports a built-in gamepad mapping, so it comes up
as a game controller with no `SDL_GAMECONTROLLERCONFIG` needed. Verified on
device: 1 pad, 8 buttons, 1 hat, `gamecontroller=yes`. Button *presses* are
unverified — the DDR word only moves when a human touches the pad.

**Audio — an SDL audio driver writing the core's DDR ring.**
`src/audio/mister/SDL_misteraudio.c`. There is no usable Linux sound path under a
core; `gm_audio` drains a 64 KiB ring of stereo S16 (`wr_ptr` +0x030 ours,
`rd_ptr` +0x038 the FPGA's, ring at 0x3A0D0000) and mixes it into the MiSTer
output. Verified on device: ring opens at 48 kHz stereo and the FPGA's read
pointer advances, so frames are being consumed.

**`gm_audio`'s SRC_RATE is the other half of that contract.** It is a
compile-time parameter driving the resampler to the 48 kHz output, and it carried
GameMaker's 22050. Godot mixes at 48 kHz, so the shipping core sets SRC_RATE to
48000 — a rate mismatch here detunes every sample rather than failing loudly.

**Timing — Genesis H40, without touching the PLL.** The first 320x240 build held
`H_TOTAL` at 380 by shrinking blanking, which preserved the line rate but
stretched active time past H40. The core now runs the exact H40 raster: 320
active + 100 blanking = 420 pixel clocks, issued in the Genesis' 3420 MCLK by a
mixed `/8,/9,/10` CE_PIXEL schedule (320@/8 + 28@/10 + 4@/9 + 68@/8), taken from
MiSTer_OpenBOR_7533 which runs this geometry. Active time is the CRT-correct
47.68 us; line rate and refresh are unchanged. No PLL edit was needed —
`CLK_VIDEO` was already the 53.693 MHz Genesis MCLK and only the CE divider moved.

**An SDL detail worth remembering.** `SDL_sysaudio.h` `#undef`s `_THIS` after
declaring the driver interface, so every audio backend re-defines it in its own
header. Omitting that produces `type of '_THIS' defaults to 'int'` — which reads
like an include-path problem and is not one.

## 1g. Audio rate: 22050 -> 48000, and why it looked like a ceiling

**Corrected.** An earlier version of this section claimed 22050 was a measured
hardware ceiling of `gm_audio`. That was wrong. The module now runs at **48000,
unity ratio, no resampling anywhere** — Godot mixes at 48 kHz, our driver opens
the ring at 48 kHz, the core consumes at 48 kHz, the DAC emits at 48 kHz.

Four separate defects stacked up to make 22050 look like a limit. The pointer
that broke it open was that OpenBOR and MAMESTer already do 48/44.1 kHz on this
same hardware — a sibling core doing the thing you have concluded is impossible
is the cheapest possible disproof, and it should have been checked first.

1. **A 32-bit overflow in the increment.**
   `INC_NOM = (65536 * SRC_RATE + OUT_RATE/2) / OUT_RATE` in Verilog `int`
   arithmetic: `65536 * 44100 = 2,890,137,600` exceeds 2^31, so the increment
   came out NEGATIVE (measured `nom -29266`). 22050 fits, which is exactly why
   that one rate worked. The testbench carried a copy of the same expression, so
   both sides agreed on the wrong answer. Computed in 64 bits now, with the
   `INC_NOM` assertion turned into a guard against precisely this.

2. **Single-beat fetch.** `ram_burstcount = 1` into 4 qwords of staging, ~8.7k
   qwords/s measured — enough for 22050 (11k/s), not for more. Now bursts up to
   8 qwords into 32, wrap-safe, the way OpenBOR/MAMESTer do it on this bus.

3. **A one-bit integer part in the phase accumulator.** A tick could advance the
   window by at most one source frame, so at unity ratio the rate-matching loop
   could slow the consumer but never hurry it, and any host running fractionally
   fast grew the ring without bound. Two bits now: a tick pulls 0, 1 or 2 frames,
   and since a qword holds two frames a double pull retires exactly one qword
   whichever half it started on. Only viable once staging was deep enough to
   pull twice from.

4. **Constants that were times written as counts.** `SLEW_STEP` is an absolute
   increment delta, so the loop's authority as a fraction of `INC_NOM` halved
   when the rate doubled; `TARGET_QW`/`BAND_QW` are buffer times expressed in
   qwords, so they meant half as long. Both derived from the rate now.

`tb_gm_audio` passes at 22050/32000/44100/48000. Two of its gates asserted that
the output is *interpolated*, which at unity ratio asserts a defect — a
full-slope delta is the correct result of a 1:1 copy. Those are taken only while
resampling now; unity gets a no-delta-exceeds-source-slope gate instead, and the
mean-slope gate (the real ratio check) applies to both.

Measurement note that still stands: use `audio_ring_probe`, not `devmem`
sampling. The 64 KiB ring wraps every 341 ms, so any interval past ~150 ms
cannot tell a wrap from a stall. The probe also hardcodes a 48000 nominal in its
banner, so at other rates read it against `SRC_RATE`, not its own warning line.

## 1h. Final verification on hardware (2026-08-23)

Core `DonutDodo_320x240_H40_22k_20260823.rbf` — CI run 32635250465, Linux
(`ubuntu-latest` + `raetro/quartus:17.0`), seed 3.

| Subsystem | Result |
|---|---|
| Video | Fabric-rendered 320x240, **60.1 fps locked 1:1 to scanout**, engine CPU 0.33 of 2 cores |
| Timing | Genesis H40: 420 pixel clocks in 3420 MCLK, 47.68 us active, no PLL change |
| Audio | **drain 22038.9 Hz vs SRC_RATE 22050 (-0.05%)**, submit matched, 74.7 ms latency |
| Input | Enumerates as a game controller: 1 pad, 8 buttons, 1 hat, built-in mapping |
| Decode | 100% of draws, zero fallbacks |

Caveat on the probe's own output: `audio_ring_probe` hardcodes a 48000 nominal
and therefore prints "-54% vs nominal" and a starvation warning even when the
ring is running exactly on rate. Read it against `SRC_RATE`, not its banner.

Unverified: button *presses*. The DDR joystick word only changes when a human
touches the pad, so enumeration is proven and the press path is not.

Still open, in rough priority order: the core still carries Maldita's CONF_STR
button names ("Sword, Action, Item 1, Item 2, Pause"), so the OSD labels are
wrong for this game; full-rate 48 kHz audio needs `gm_audio`'s fetch bursting;
and the port is not yet packaged (`main=` wrapper, Scripts entry, release bundle)
the way cursed.castilla-mister ships.

## 1i. The address trap that cost the most time (2026-08-23)

**This core relocates its video and joystick region.** `Maldita.sv` sets
`FB_QW_BASE = 0x3BF40000`, deliberately "disjoint from gmloader's 0x3A
NativeVideoWriter region". So for the Maldita/Donut Dodo fabric core:

| What | Address |
|---|---|
| control word (frame counter, active bank) | `0x3BF40000` |
| joystick P1 / P2 | `0x3BF40008` / `0x3BF40018` |
| scanout buffers BUF0 / BUF1 | `0x3BF40040` / `0x3BF80040` |
| audio ring + pointers (gm_audio, ABSOLUTE — did not move) | `0x3A0D0000`, `0x3A000030/38` |

The OpenBOR core keeps all of it at `0x3A000000`. Nothing enforces the pairing,
and getting it wrong is **silent**: reads return zeros forever, which is
indistinguishable from "no buttons pressed" and from "the framebuffer is frozen".
Both of those wrong conclusions were reached in this session before the base was
found — including a confident but incorrect explanation that the fabric path
simply presents nothing.

Distinguishing test, which costs seconds: poke a sentinel and see if the FPGA
overwrites it.

    busybox devmem 0x3BF40008 32 0xDEADBEEF   # overwritten within a frame => live
    busybox devmem 0x3A000008 32 0xDEADBEEF   # survives => dead region

Both the joystick driver and the DDR present path now take the base from the
environment (`SDL_MISTER_JOY_BASE`, `SDL_MISTER_DDR_BASE`, defaulting to this
core's `0x3BF40000`), and `launch.sh` states it explicitly rather than relying on
a default that is only right for one core.

Audio was unaffected throughout, because `gm_audio`'s addresses are absolute and
in the `0x3A` region — which is exactly why audio worked while video dumps and
input did not, a divergence that should have pointed here sooner.

## 1j. Two black screens and a dead pad (2026-08-24)

Three separate faults, found from one "it is only a black screen" report.

**1. Two engines on one control block — this was the black screen.** A Maldita
Castilla `gmloader` left running from 2026-08-23 19:55 was still alive when Donut
Dodo launched. Both drive BLTCTRL at `0x3B000000`, so each read the OTHER's
sequence out of C_DONE and neither ever saw its own ack. The two logs are exact
mirrors, same minute:

    donutdodo.log:  pending=2085267 emitter=2085268 done=2285327
    maldita.log:    pending=2285021 emitter=2285022 done=2085037

1,164,841 frames dropped here, 1,097,641 there. Both guards existed and both
missed it: this launcher refused only on a live `frt_3.5.2`, Maldita's reaps only
`gmloader -c`. Neither excludes the other. `launch.sh` now refuses on either, via
`pidof` rather than `ps w | grep` — the grep form matches any command line that
merely NAMES the binary, and a shell running `killall -9 frt_3.5.2` tripped it.
Maldita's launcher still has the same one-sided guard; fixing that side is open.

**2. The pad enumerated twice and the DDR device lost.** `SDL_MISTER_JOY=1` was
supposed to hide kernel joystick devices, because MiSTer holds an exclusive
EVDEV grab and they can never deliver an event. It did not: the skip lived only
in `SDL_JoystickInit()`, and `SDL_joystick.c` walks the driver array in four
other places. `SDL_JoystickDetect()` calls `Detect()` on every driver whether or
not its `Init` ran, so `LINUX_JoystickDetect` scanned `/dev/input` anyway and
added the 8BitDo. Measured: `frt` had `/dev/input/event2` open, the joystick
words at `0x3BF40008` changed under a press (`0x0A`, `0x10`, `0x20`, `0x100`,
`0x200`), and the engine logged ZERO button events.

Two devices then collide in FRT: `SDL_JOYDEVICEADDED` stores by device index
(`js_[id] = SDL_JoystickOpen(id)`), so whichever arrives second overwrites the
first handle and `get_js_id()` drops every event from the loser. Which one loses
is an enumeration race, which is why input worked on 08-23 and not on 08-24 — the
old log shows the 8BitDo landing on `id=1` that time and on `id=0` this time.

Fixed in `SDL_joystick.c` with one predicate, `SDL_JoystickDriverSuppressed()`,
applied at all five walks (Init, `SDL_GetDriverAndJoystickIndex`,
`SDL_NumJoysticks`, Quit, Detect) so counts, index->driver mapping, Detect and
Quit all agree on one driver. Verified on device: a single `MiSTer Joystick 1` at
`id=0`, and NO `/dev/input/*` or `/dev/hidraw*` fd open at all. `SDL_JOYSTICK_HIDAPI=0`
was tried first as a launcher workaround and is NOT the fix — HIDAPI was not the
source; the Linux driver was. Button presses themselves are still unverified.

The FRT side is still wrong on its own terms: indexing `js_[]` by device index
rather than allocating a free slot keyed on instance id. With one driver there is
nothing to collide with, so this is latent, not live, and it costs a Godot
rebuild to fix.

**3. The fabric wedge is not gone.** After a clean start the game rendered for
about two minutes (verified by dumping the scanout: hi-score screen at frame
4710), then C_DONE froze at `0x1FFC25` with C_SUBMIT climbing and 3,421 frames
dropped. Probe words `0x3BFB0010=0x40004012` / `0x3BFB0014=0x25` frozen, scanout
frame counter frozen. A menu->core reconfigure cleared it. This is the wedge
Maldita's launcher carries a recovery gate for; this launcher has none, so a
wedge here is a dead picture with no way out but a manual reload. Porting that
gate is the open item. Note the arbiter fix is claimed as landed in v0.3.2 —
whether `DonutDodo_48k_20260823.rbf` carries it has NOT been checked.

## 2. Architecture

Same shape as the gmloader-next/cursed.castilla stack, with the engine swapped:

```
FRT 3.5.2 (Godot 3.5.2, GLES2)      <- we build this, armhf
  └─ sdl2_adapter.h                  <- replaced/extended: "mister" adapter
       ├─ video   : EGL surfaceless (Mesa llvmpipe) -> present to DDR
       ├─ present : native_video_writer.c   (from ../gmloader-next/gmloader/mister/)
       ├─ audio   : native_audio_writer.c / mister_native_audio.cpp (fabric ring)
       └─ input   : joy_ddr_reader / joy_shm_reader  (MiSTer joystick -> engine)
FPGA: existing scanout (openbor_video_reader + ddr3_scan_adapter) reads the DDR
      double-buffer at 0x3A000040 / 0x3A040040 and drives video out.
```

Three tiers, each with a measurable gate. Do not start a tier before its predecessor's gate.

### Tier 0 — prove the platform stack (days, no new code)

Goal: answer "can *any* Godot binary get a GL context and draw on MiSTer?"

1. Fetch upstream `frt_3.6.2-1_arm32_release`, scp to `/media/fat/games/donutdodo/`.
2. Run it against the 3.5.2 pck with the existing llvmpipe Mesa:
   `SDL_VIDEODRIVER=offscreen LD_LIBRARY_PATH=…/gmloader/mesa GALLIUM_DRIVER=llvmpipe
    EGL_PLATFORM=surfaceless ./frt --main-pack DonutDodo.pck --video-driver GLES2`
3. Gate A: process starts, no SIGILL (checks the armv7/A9 codegen risk), and a GL context
   is created. Capture with `frame_capture.cpp`-style glReadPixels or just check
   Godot's log for `OpenGL ES 2.0 Mesa 21.3.9`.

Unknown to resolve here: whether the SDL2 in play exposes the **`offscreen`** video driver
(SDL ≥ 2.0.22; device's system SDL is 2.0.14, which likely does **not**). If not, Tier 0 must
ship its own SDL2 (built like FRT's Dockerfile does) — that work is needed in Tier 1 anyway.
A 3.6 binary loading a 3.5.2 pck may also fail on API drift; that only invalidates Tier 0 as a
*game* test, not as a *platform* test.

### Tier 1 — our own FRT 3.5.2 armhf + native MiSTer present/audio/input

Goal: the real port, software-rendered.

1. **Build**: clone `efornara/godot3` (3.5.2 branch) + `efornara/frt`, build in a Docker image
   modelled on FRT's `scripts/Dockerfile` but pinned to the game's version, using
   `arm-godot-linux-gnueabihf_sdk-buildroot`, `-march=armv7-a -mfpu=neon -mfloat-abi=hard`,
   `target=release tools=no`.
2. **Video**: add a MiSTer path to `sdl2_adapter.h` (or a parallel `mister_adapter.h` selected
   by an env var) that does what `../gmloader-next/gmloader/main.cpp` already does — surfaceless
   EGL + llvmpipe — and presents each frame through
   `../gmloader-next/gmloader/mister/native_video_writer.c` into the DDR double-buffer the
   FPGA scanout already reads. Because the project's stretch mode is `viewport` at 320×240,
   set the window to 320×240 so the final blit is 1:1 and nothing is upscaled on the A9.
3. **Audio**: MiSTer has `libasound`, but the shipping cores drive audio through the fabric
   ring (`mister_native_audio.cpp`, validated by `cursed.castilla-mister/tools/audio_ring_probe`).
   Implement a Godot `AudioDriver` on that ring rather than trusting ALSA.
4. **Input**: reuse `joy_ddr_reader` / `joy_shm_reader` from gmloader-next; no gptokeyb,
   no `/dev/uinput` games.
5. **Threading**: force `rendering/threads/thread_model=0` (safe single-thread) on a dual-core
   A9 first; re-enable 2 only if measurements justify it.
6. Gate B: game boots to the title screen and is measurably >= 30 fps at 320×240
   (`GMLOADER_DRAW_TRACE`-equivalent per-frame budget log ported into the adapter).

**Why Tier 1 may actually be enough here** (Inferred, needs measurement): the Maldita Castilla
4.2 fps llvmpipe result was dominated by clearing a ~2048² GameMaker `application_surface` three
times a frame plus deferred-raster readback. Donut Dodo renders a 320×240 viewport (76.8 kpx) with
Godot's 2D batcher. The fill-rate arithmetic is ~40× smaller. Treat "software is enough" as the
working hypothesis and Tier 2 as contingency — do not pre-build the fabric path.

### Tier 2 — fabric acceleration (only if Gate B fails)

Two candidate interception points, in order of preference:

- **Engine-level (preferred)**: we compile Godot from source, so replace
  `RasterizerCanvasGLES2` with a `RasterizerCanvasMister` that emits `blt_emitter` commands
  (`../mister-fpga-blitter/host/blt_emitter.c`) — the same move `solarus-mister` made by
  subclassing `SDLRenderer` into `MisterBlitterRenderer`. Cleaner than GL interception because
  the item/command stream is typed (rects, ninepatches, polygons) instead of reverse-engineered
  triangles.
- **GL-shadow (fallback)**: port `../gmloader-next/gmloader/mister/blitter.cpp`'s approach —
  intercept `glDrawArrays`/`glDrawElements`/`glClear` and re-emit to `raster_backend_mfgpu`.
  Necessary only if Godot's canvas rasterizer proves too entangled to subclass.

Prerequisite either way: the fabric protocol work is already done and bit-exact-verified
(`blt_wire.h`, `blitter_ref.c`); Donut Dodo would ride it, not extend it — but any protocol
change must land reference-model-first, per `cursed.castilla-mister/CLAUDE.md`.

---

## 3. Reuse map

| Need | Take from | File |
|---|---|---|
| Surfaceless EGL bring-up, llvmpipe env, crash handler | gmloader-next | `gmloader/main.cpp` |
| Frame → DDR double-buffer present | gmloader-next | `gmloader/mister/native_video_writer.{c,h}` |
| Audio ring to fabric | gmloader-next | `gmloader/mister/{native_audio_writer.c,mister_native_audio.cpp}` |
| Joystick from MiSTer | gmloader-next | `gmloader/mister/joy_{ddr,shm}_reader.cpp` |
| Per-frame budget profiling | gmloader-next | `gmloader/mister/draw_trace.{h,cpp}` |
| armhf Mesa (llvmpipe + softpipe) | `~/mesa-build/` | `build_inner_cached.sh`, `Dockerfile.armhf-mesa` |
| Blit command emitter / protocol | mister-fpga-blitter | `host/blt_emitter.c`, `host/blt_wire.h` |
| Scanout, `main=` wrapper, Scripts entry, release bundle | cursed.castilla-mister | `deploy.py`, `vendor/Main_MiSTer/*_hook.cpp`, `.github/workflows/release.yml` |
| Engine-level renderer subclass precedent | solarus-mister | `patches/mister/`, `games/Solarus/solarus_run.sh` |

Non-goal for now: a new FPGA core. Tier 0/1 run on an existing core's scanout; only Tier 2
needs fabric time, and even then it reuses the shipped `blitter_top` protocol.

---

## 4. Packaging

Follow the cursed.castilla layout exactly:

```
/media/fat/games/donutdodo/
  frt_donutdodo            # our armhf build
  gamedata/DonutDodo.pck   # user-supplied (bought from itch.io)
  mesa/                    # llvmpipe stack (or a symlink to the gmloader one)
  conf/                    # XDG_CONFIG_HOME / XDG_DATA_HOME target, as PortMaster does
  launch.sh
```

Game data stays user-supplied — same legal model as the other ports (Godot and FRT are MIT;
the pck is not ours to ship). The user's copy is already at
`~/Downloads/DonutDodo/gamedata/DonutDodo.pck` (v1.39-era, 2023-05-30).

---

## 5. Risks / open questions

1. **SDL2 offscreen driver availability** — device SDL is 2.0.14; `offscreen` (EGL) landed in
   2.0.22. Likely we must ship our own SDL2 (FRT's Dockerfile already builds 2.32.10 per arch).
   *Cheap check:* run `SDL_VIDEODRIVER=offscreen` against the system lib and read the error.
2. **Illegal instruction on Cortex-A9** — upstream arm32 templates reportedly fault on older
   boards. Mitigation: build with explicit `-march=armv7-a -mfpu=neon -mfloat-abi=hard`
   and verify with a trivial pck before wiring anything MiSTer-specific.
3. **ETC1 textures under llvmpipe** — imports are ETC (not ETC2). Mesa decodes ETC1 in software,
   but confirm the `.stex` files actually carry ETC and not just an uncompressed fallback;
   a decode-per-upload would be a load-time cost, not a per-frame one.
4. **Multithreaded rendering (`thread_model=2`)** on two A9 cores with a DDR present path —
   force single-thread first.
5. **Audio**: whether MiSTer's ALSA is usable at all under a core; the fabric ring is the
   known-good path but needs a Godot AudioDriver written against it.
6. **Godot 3.5.2 source availability** — `efornara/godot3` must still carry a 3.5.2-compatible
   branch; if not, build stock Godot 3.5.2 + the FRT platform dir, or accept 3.6.x and test the
   pck for API drift.

---

## 6. Next actions (ordered)

1. Copy `DonutDodo.pck` to a work dir and stand up the Tier-0 device test with the upstream
   `frt_3.6.2-1_arm32_release` binary + existing gmloader Mesa. → answers Gate A, the SDL
   offscreen question, and the SIGILL risk in one run.
2. Build the FRT docker image (FRT `scripts/Dockerfile`, pinned to 3.5.2) and produce a stock
   armhf `frt_3.5.2`. Confirm it runs the pck on device before any MiSTer-specific code.
3. Fork `sdl2_adapter.h` into a MiSTer adapter; wire `native_video_writer` present. Measure fps.
4. Decide Tier 2 on the measurement, not in advance.
