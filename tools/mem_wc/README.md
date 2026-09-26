# mem_wc for the MiSTer kernel on this device

Write-combining `/dev/mem_wc`, restricted to one physical window. Driver source vendored unchanged (via
`../cash.cow.dx-mister/tools/mem_wc/`) from `../maldita.castilla-mister/tools/mister-mem-wc/mem_wc.c` (originally skmp/minicast, GPL-2.0); see that
directory's README for the analysis (strongly-ordered `/dev/mem` stores 80 MB/s vs 814 MB/s write-combined).

- Build for the device's running kernel: `tools/mem_wc/build.sh` -> `prebuilt/mem_wc-$(uname -r).ko`.
- Load (fabric window only): `insmod mem_wc-6.18.38-MiSTer.ko phys_base=0x3B000000 phys_size=0x01000000`
  — only when `/dev/mem_wc` does not already exist.
- **Never `rmmod` it**: a process can keep a live mapping after closing the fd, and an unload under it hung
  the device once (see Maldita's `mem_wc_load.sh`). It stays until reboot.
- The fabric backend (`src/vendor/mister/raster_backend_mfgpu.cpp`) maps it automatically when present;
  `GMLOADER_NO_WC=1` forces the strongly-ordered mapping for A/B.
