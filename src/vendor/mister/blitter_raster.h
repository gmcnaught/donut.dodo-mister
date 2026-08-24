//
//  MiSTer 2D software blitter — P2 step 2: the software triangle rasterizer.
//  See BLITTER_DESIGN.md. This is the inner compositor: it takes already-decoded
//  screen-space textured triangles and composites them into a CPU RGBA8888
//  surface. It is deliberately GL/SDL-INDEPENDENT and host-buildable so it can be
//  unit-tested with plain g++/clang++ on the dev machine.
//
//  The blitter.cpp decode step (which IS GL-dependent) adapts its SoftSurface /
//  ShadowTexture / BlendMode to the GL-free RSurface / RTexture / RBlend below
//  and calls Blitter_RasterTri per triangle.
//
//  Scalar + correct first; NEON inner loop is a later phase.
//
#pragma once

#include <stdint.h>

// ---- Render target ----------------------------------------------------------
// Destination surface, RGBA8888, top-left origin, tightly packed (row pitch = w*4).
struct RSurface {
    uint8_t *rgba;
    int      w, h;
    // [app-surface render target, step 1] The GL FBO id this surface is bound
    // to (0 = default framebuffer / WORK), set by blitter.cpp's
    // get_render_target(). Lets a backend (backend_mfgpu) tell "the
    // application-surface FBO" apart from "some other FBO" apart from "the
    // default framebuffer" without depending on GL types here -- plain
    // uint32_t, not GLuint, to keep this header GL-independent (it's
    // deliberately host-buildable with no GL headers; see the file comment).
    uint32_t fbo = 0;
};

// ---- Source texture ---------------------------------------------------------
// CPU copy of a texture's pixels. When valid==0 (or tex==nullptr) the sampler
// returns opaque white, so an untextured draw is just the vertex colour.
//   format RTEX_RGBA8888: `rgba` is 4 bytes/texel R,G,B,A.
//   format RTEX_RGBA4444: `rgba` is 2 bytes/texel, one uint16 per texel packed
//                         (R<<12)|(G<<8)|(B<<4)|A; each nibble is expanded back to
//                         8-bit by replication when sampled. Halves the per-texel
//                         gather bandwidth / cache footprint (see Blitter footprint
//                         notes). Nearest-only.
enum {
    RTEX_RGBA8888 = 0,
    RTEX_RGBA4444 = 1,
};
struct RTexture {
    const uint8_t *rgba;
    int            w, h;
    int            nearest;   // 1 = nearest filter (only mode implemented; linear TODO)
    int            valid;     // pixels present and usable
    int            format;    // RTEX_RGBA8888 | RTEX_RGBA4444
    int            opaque;    // 1 = every texel alpha == 255 (enables blend fast-path)
};

// ---- Blend modes ------------------------------------------------------------
// Mirrors BlendMode in blitter.h (GL-free). src = frag after the alpha test,
// dst = existing pixel, everything normalized to [0,1] before compositing:
//   RB_NONE    : out = src                                        (opaque copy)
//   RB_ALPHA   : out.rgb = src.rgb*src.a + dst.rgb*(1-src.a)
//                out.a   = src.a + dst.a*(1-src.a)
//   RB_PREMULT : out.rgb = src.rgb + dst.rgb*(1-src.a)
//                out.a   = src.a + dst.a*(1-src.a)
//   RB_ADD     : out.rgb = src.rgb*src.a + dst.rgb  (clamp 1)
//                out.a   = max(src.a, dst.a)
enum RBlend {
    RB_NONE = 0,
    RB_ALPHA,
    RB_PREMULT,
    RB_ADD
};

// ---- Vertex -----------------------------------------------------------------
// One decoded vertex: screen-space pixel position in the dest (top-left origin),
// texcoords in [0,1] (wrap-repeat outside), and a per-vertex colour in [0,1].
struct BVtx {
    float x, y;
    float u, v;
    float r, g, b, a;
};

// ---- API --------------------------------------------------------------------
// Rasterize one triangle into dst. Per covered pixel the pipeline is:
//   texel    = tex valid ? nearest-sample(u,v) wrap-repeat : white(1,1,1,1)
//   frag.rgb = vtx.rgb * texel.rgb ;  frag.a = vtx.a * texel.a   (all in [0,1])
//   alpha test: if frag.a <= alphaRef -> discard the pixel
//   composite frag into dst[x,y] per the blend mode above
// u,v and r,g,b,a are interpolated barycentrically. Writes are clipped to
// [0,dst->w) x [0,dst->h) — never out of bounds. Degenerate / zero-area /
// non-finite triangles are skipped.
void Blitter_RasterTri(RSurface *dst, const BVtx v[3], const RTexture *tex,
                       RBlend blend, float alphaRef);

// Rasterize a list of triangles (3 BVtx each: count*3 verts) into dst, splitting
// the work across `threads` cores by horizontal scanline bands. Pixel-identical
// to calling Blitter_RasterTri per triangle. threads<=1 => single-threaded.
void Blitter_RasterDraw(RSurface *dst, const BVtx *verts, int triCount,
                        const RTexture *tex, RBlend blend, float alphaRef,
                        int threads);

// Fill the whole surface with a colour (used for clears).
void Blitter_ClearSurface(RSurface *s, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
