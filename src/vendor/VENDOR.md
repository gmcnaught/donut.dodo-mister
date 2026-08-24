Vendored 2026-08-22
  gmloader-next/gmloader/mister/* (RasterBackend seam + mfgpu backend)
  gmloader-next/3rdparty/mfgpu/{host,refmodel} (TRILIST=10 protocol, matches deployed Maldita RBF)
Do not edit in place without recording the delta here.

## Local edits to vendored files

- `mfgpu/refmodel/blitter_ref.h`: `BLT_FB_WIDTH/HEIGHT` 288x216 -> 320x240.
  This is one half of a geometry contract; the other half is `FB_W/FB_H` in the
  core's `fpga/rtl/blitter_defs.vh`. Ours is built from
  `maldita.castilla-mister` branch `donutdodo/fb-320x240`. Re-vendoring from
  gmloader-next will silently reset this to 288x216 — which shows up not as a
  build error but as triangles clipped to the wrong rectangle on hardware.
