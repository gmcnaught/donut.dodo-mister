/*
  Draw rewrites that cut fabric time without changing a pixel. Both act on a
  decoded draw before it reaches the RasterBackend.

  solid_rect: an opaque one-colour rectangle -- Godot's black ColorRect behind
  every level, the HUD bar, the "GET READY!" card over a level's first seconds
  -- costs ~6 fabric cycles per pixel as a textured TRILIST (~4.7 ms for the
  whole 320x240 screen) but 1 cycle as a FILL. The caller issues it as the
  backend's clear() when it covers the target, else as a rect FILL.

  unrotate: the fabric's texel cache is two 256-byte SDRAM lines plus a 2 KB
  direct-mapped qword cache, so a sprite sampled at an angle walks onto a new
  texture row every ~1.4 pixels and nearly every pixel waits on SDRAM (Ferris
  Wheel Frenzy: texwait 5-24 ms per frame for one 128x128 wheel). For a large
  rotated quad we nearest-sample the texture on the CPU into a scratch texture
  laid out in screen space, then draw the same triangles reading it row by row.
  The scratch keys alternate by frame parity, so the texture a still-executing
  frame reads is never the one being replaced.

  identity_present: Godot draws the scene into its viewport render target (the
  app surface) and then copies that to the screen, a 320x240 TRILIST at ~6
  fabric cycles per pixel (~4.7 ms a frame). When the copy is 1:1, the glue can
  draw the scene straight into the screen and drop the copy (glue.cpp).
*/
#ifndef DRAW_REWRITE_H
#define DRAW_REWRITE_H

#include <stdint.h>
#include "godot_shadow.h"

namespace dr {

/* Cache keys of scratch textures: above any GL name, below gs::kWhiteTexKeyBit. */
constexpr uint32_t kScratchKeyBase = 0x40000000u;

/* True when d is an opaque single-colour axis-aligned quad whose edges inside
   the w x h target lie on pixel boundaries; rgba receives the colour and rect
   the pixels it covers (x, y, w, h), clipped to the target. */
bool solid_rect(const gs::DecodedDraw &d, int w, int h, uint8_t rgba[4], int rect[4]);

/* solid_rect covering the whole target. */
bool solid_cover(const gs::DecodedDraw &d, int w, int h, uint8_t rgba[4]);

/* Rewrite a large rotated textured quad to sample an axis-aligned scratch copy.
   Returns true when d now points at the scratch texture; invalidate(key) is
   called first so the backend restages it. */
bool unrotate(gs::DecodedDraw &d, int w, int h, void (*invalidate)(uint32_t key));

/* True when d copies the whole app surface onto a w x h default framebuffer
   pixel for pixel: an opaque, untinted, axis-aligned w x h quad whose texel
   (as the backend samples it) is its own pixel. */
bool identity_present(const gs::DecodedDraw &d, int w, int h);

/* Once per presented frame: flips the scratch key parity. */
void frame_end();

} /* namespace dr */
#endif /* DRAW_REWRITE_H */
