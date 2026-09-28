# Donut Dodo for MiSTer

Donut Dodo (Godot 3.5.2) running on the MiSTer DE10-Nano: the game logic runs on the ARM cores, and every frame is
drawn by an FPGA blitter core. Audio and the joystick go through the core as well.

**The game itself is not included.** You need your own copy of **Donut Dodo — RetroPie Edition** from
https://zapposh.itch.io/donut-dodo-retropie-edition.

## Install

1. Extract this zip over the root of your MiSTer SD card (`/media/fat/`). It adds:
   - `_Other/DonutDodo_<date>.rbf` — the FPGA core (the shared Godot/GameMaker blitter core, branded DonutDodo)
   - `Scripts/DonutDodo.sh` — loads the core and starts the game
   - `Scripts/DonutDodo_CoresMenu.sh` — turns on starting the game from the core list (step 3)
   - `_Other/DonutDodo.mgl` — a core-list entry that always loads the newest `DonutDodo_*.rbf`
   - `games/DonutDodo/` — the engine, its runtime, the launcher, this README and `sha256sums.txt`;
     `games/DonutDodo/platform/MiSTer_hybrid` and `platform/hybrid.d/DonutDodo.conf` start the game on core load (step 3)
     (verify the copy: FAT filesystems can silently truncate files on an interrupted copy)
2. Copy **`DonutDodo.pck`** from the RetroPie Edition download (`DonutDodo/gamedata/DonutDodo.pck`) to
   `/media/fat/games/DonutDodo/gamedata/DonutDodo.pck`.
   - The file this port was tested with has SHA-256 `b5b9cc58ef3d767d70cc3290924b5b34ef58caa8ef033b617db6e4530ee2b78d`.
3. Run **Scripts → DonutDodo_CoresMenu** once. It adds a `[DonutDodo]` section to `/media/fat/MiSTer.ini`
   (backed up first to `MiSTer.ini.bak.<time>`) with `main=/media/fat/games/DonutDodo/platform/MiSTer_hybrid`.
   From then on, loading **DonutDodo** from the core list (`_Other`) starts the game. Run it again to turn this off.
   - `main=` is MiSTer's per-core setting for which MiSTer program runs while that core is loaded.
     `MiSTer_hybrid` is the standard MiSTer program plus one addition that starts the game once the core is up
     (it reads `hybrid.d/` next to itself, so it only knows about DonutDodo);
     every other core keeps using your normal, updated `/media/fat/MiSTer`. No background service is installed.
4. **Scripts → DonutDodo** also starts the game, with or without step 3.

Upgrading from an earlier release: extract the new zip, then run **Scripts → DonutDodo** once (or run
**Scripts → DonutDodo_CoresMenu** again). It moves an existing `[DonutDodo] main=` line to
`/media/fat/games/DonutDodo/platform/MiSTer_hybrid` (backing up `MiSTer.ini`):
- from 20260926/20260926b (`main=/media/fat/linux/MiSTer_hybrid`): it also removes `linux/hybrid.d/DonutDodo.conf`,
  and deletes `linux/MiSTer_hybrid` once no other `MiSTer.ini` section uses it.
  Until you do this, the old `linux/MiSTer_hybrid` keeps starting the game.
- from earlier releases (`games/DonutDodo/MiSTer_DonutDodo`): it also deletes `MiSTer_DonutDodo` and the old
  `games/DonutDodo/mem_wc-*.ko`.

To quit, load another core from the OSD; the launcher stops the game when the core changes.

## Controls

The game reads the joystick through the core, so the MiSTer OSD button mapping for this core applies
(**OSD → Define joystick buttons**). Buttons in order: **Jump/OK, Back, Unused, Options, Start, Select/Coin,
Unused L, Unused R**. Default map on a pad with no saved DonutDodo mapping: bottom face = Jump/OK, right face = Back,
top face = Options. The D-pad moves.

A mapping you saved for this core earlier (`config/inputs/DonutDodo_input_*.map`) takes precedence over the default.

## Notes

- The core shows the 224 lines a CRT displays (rows 7–230 of the game's 240).
- Saves and settings go to `games/DonutDodo/conf/`.
- Logs: `/media/fat/logs/DonutDodo/` (`launch.log`, `donutdodo.log`, and `donutdodo.prev.log` from the previous run).
- `platform/mem_wc/mem_wc-<kernel>.ko` gives the blitter a faster (write-combining) DDR mapping. It is loaded only if it matches the
  running kernel (`uname -r`) and nothing else has loaded one; otherwise the game runs with the slower mapping.
  It stays loaded until reboot by design.
- If the blitter stops responding (at start-up or during play), the launcher reloads the core and restarts the game.
- Only one blitter-core game can run at a time; starting this one stops any other.

## Credits and licences

- Donut Dodo © Zapposh; not distributed here.
- This port: GPL-3.0, source at https://github.com/gmcnaught/donut.dodo-mister (`LICENSING.md` lists the exceptions).
- `MiSTer_hybrid`: MiSTer-devel/Main_MiSTer (GPL-3.0) plus the mister-hybrid-platform hook
  (https://github.com/gmcnaught/mister-hybrid-platform).
- Godot Engine 3.5.2 and FRT 2.1.0 (MIT); SDL2 2.32.10 (zlib) with MiSTer video, audio and joystick drivers and a
  null GL (no Mesa: the FPGA draws every frame).
- `mem_wc` driver: GPL-2.0, from skmp/minicast (source in mister-hybrid-platform under `device/mem_wc/`).
