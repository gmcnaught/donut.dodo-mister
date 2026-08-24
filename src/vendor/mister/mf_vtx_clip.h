// Guard-band screen-space clipping for the fabric vertex path.
//
// WHY THIS EXISTS — the fabric wire vertex is int16 12.4 fixed point
// (blt_vtx_t.x/y, refmodel/blitter_ref.h), so a screen coordinate is
// representable only over [-2048.0, +2047.9375] px. bvtx_to_blt() packs it with
// an unchecked narrowing cast:
//
//     o.x = (int16_t)lroundf(v->x * 16.0f);
//
// and nothing upstream bounds the input: blitter.cpp's GL-shadow applies a bare
// viewport transform (bv.y = g_vpY + (ndcy*0.5f+0.5f)*g_vpH) with no clip and no
// cull, because the SW rasterizer it was written against clips for itself — it
// bboxes in float and clamps to the surface (blitter_raster.cpp), so an
// off-screen triangle simply draws nothing.
//
// The fabric path has no such backstop. Past the end of the int16 range the
// coordinate WRAPS modulo 4096 px, with two device-visible failure modes:
//
//   * STRADDLE — one triangle with vertices on both sides of the wrap becomes
//     ~4000 px tall. Its bbox clamps to the whole framebuffer and it rasterizes
//     as a full-height column: replaying the golden's own coverage + attribute
//     math over a 24x24 quad at y=2040 lights all 216 rows at a near-constant
//     12-13 px width, with the interpolated v FROZEN at ~11 of 24 while u sweeps
//     0..23. That is one horizontal texel row of the sprite extruded down every
//     scanline, at the sprite's true x and true width — the red vertical bars
//     photographed on the level-1 bridge, whose gap is the sprite's own
//     colorkeyed transparency inside the smeared row. Any primitive longer than
//     2048 px straddles unconditionally, whatever its position.
//
//   * TELEPORT — a primitive wholly past the boundary wraps coherently and is
//     drawn, undistorted, at a bogus on-screen position (y=4200 renders at
//     y=104). A phantom sprite rather than a smear.
//
// WHAT THIS DOES — a guard band, not a viewport clip. Triangles whose vertices
// all sit inside MF_VTX_GUARD_PX are passed through byte-identical, so the
// overwhelming majority of draws take a compare-only fast path and no existing
// pixel changes. Only a triangle that reaches outside the guard is clipped, and
// it is clipped against a rect that CONTAINS the viewport with MF_VTX_CLIP_PAD
// px of slack on every side. Two consequences that matter:
//
//   1. Coverage inside the viewport is unchanged. Clipping a triangle to a
//      superset of the viewport cannot alter which viewport pixels it covers.
//   2. The output is in range by construction: every clipped coordinate lands in
//      [-PAD, FB + PAD], i.e. |x| <= 1312 and |y| <= 1240 px, well inside the
//      +/-2047.94 the wire can hold.
//
// Attribute interpolation at the new vertices is exact in real arithmetic: this
// is a 2D rasterizer with no perspective divide (see blt_tri.c — barycentric
// weights over a constant area, no w), so u/v/rgba are affine in screen space
// and a linear lerp along a clipped edge is the same function the rasterizer
// would have evaluated there. Only the golden's integer `divr` rounding differs,
// by at most 1 LSB — inside the project's stated +/-1 LSB RGB565 tolerance, and
// reachable only by triangles that today render garbage.
//
// Pure and dependency-light (no globals, no device state) so mf_vtx_clip_test.cpp
// can exercise the geometry directly.
#pragma once

#include "blitter_raster.h"   // BVtx

extern "C" {
#include "blitter_ref.h"      // BLT_FB_WIDTH / BLT_FB_HEIGHT
}

// Vertices strictly inside this (in px, both axes) are passed through untouched.
// Set below the +/-2047.9375 wire limit with room to spare: the check is the only
// thing standing between a coordinate and the unchecked cast, so it must fire
// before rounding can push a value over. 2000 leaves ~48 px of margin and still
// admits every coordinate any real draw produces on a 288x216 target.
#define MF_VTX_GUARD_PX   2000.0f

// Slack added around the framebuffer on all four sides when a triangle does have
// to be clipped. Large enough that the clip rect is nowhere near the viewport
// (so no ordinary partly-offscreen geometry is ever re-triangulated), small
// enough that FB + PAD stays far inside the wire range.
#define MF_VTX_CLIP_PAD   1024.0f

// Max vertices in the clipped polygon: a triangle against 4 half-planes gains at
// most one vertex per plane (3 -> 7). The +3 is slack so a bounds slip is caught
// by the cap rather than by a stack smash.
#define MF_VTX_CLIP_POLY_MAX 10

static inline float mf_vc_lerpf(float a, float b, float t) { return a + (b - a) * t; }

static inline BVtx mf_vc_lerp(const BVtx *a, const BVtx *b, float t) {
    BVtx o;
    o.x = mf_vc_lerpf(a->x, b->x, t);  o.y = mf_vc_lerpf(a->y, b->y, t);
    o.u = mf_vc_lerpf(a->u, b->u, t);  o.v = mf_vc_lerpf(a->v, b->v, t);
    o.r = mf_vc_lerpf(a->r, b->r, t);  o.g = mf_vc_lerpf(a->g, b->g, t);
    o.b = mf_vc_lerpf(a->b, b->b, t);  o.a = mf_vc_lerpf(a->a, b->a, t);
    return o;
}

// True if any vertex reaches the guard band — or is non-finite. Written as
// !(fabs < GUARD) rather than (fabs >= GUARD) so NaN answers TRUE and is routed
// into the clipper, which drops it. A NaN that reached lroundf would be an
// unspecified int16, i.e. the same wrap bug by another road.
static inline int mf_vtx_needs_clip(const BVtx *v, int nverts) {
    for (int i = 0; i < nverts; i++) {
        const float ax = v[i].x < 0.0f ? -v[i].x : v[i].x;
        const float ay = v[i].y < 0.0f ? -v[i].y : v[i].y;
        if (!(ax < MF_VTX_GUARD_PX) || !(ay < MF_VTX_GUARD_PX)) return 1;
    }
    return 0;
}

// One Sutherland-Hodgman pass against a single axis-aligned half-plane.
//   axis: 0 = x, 1 = y.   hi: 0 = keep coord >= lim, 1 = keep coord <= lim.
// Returns the new vertex count written to `out` (which must hold n+1).
static inline int mf_vc_clip_plane(const BVtx *in, int n, BVtx *out,
                                   int axis, int hi, float lim) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const BVtx *a = &in[i];
        const BVtx *b = &in[(i + 1) % n];
        const float ca = axis ? a->y : a->x;
        const float cb = axis ? b->y : b->x;
        // Signed distance, positive inside, so one formula drives both edges.
        const float da = hi ? (lim - ca) : (ca - lim);
        const float db = hi ? (lim - cb) : (cb - lim);
        const int ina = (da >= 0.0f), inb = (db >= 0.0f);
        if (ina) out[m++] = *a;
        if (ina != inb) {
            // da and db strictly straddle zero here (one >= 0, the other < 0), so
            // da - db > 0 — the divide cannot be by zero.
            out[m++] = mf_vc_lerp(a, b, da / (da - db));
        }
    }
    return m;
}

static inline int mf_vc_finite_tri(const BVtx *t) {
    for (int k = 0; k < 3; k++) {
        // Self-comparison rejects NaN; the magnitude test rejects the infinities
        // (and anything a clip lerp could not bring back into range).
        if (!(t[k].x == t[k].x) || !(t[k].y == t[k].y)) return 0;
        const float ax = t[k].x < 0.0f ? -t[k].x : t[k].x;
        const float ay = t[k].y < 0.0f ? -t[k].y : t[k].y;
        if (!(ax < 1e9f) || !(ay < 1e9f)) return 0;
    }
    return 1;
}

// Clip `nt` triangles (3 BVtx each) into `out`, fan-triangulating each surviving
// polygon. Returns the output TRIANGLE count (0 if everything was clipped away).
// `out` must hold max_out_verts vertices; the fan stops early rather than
// overrun, so a pathological input degrades to a partial draw, never a smash.
static inline int mf_vtx_clip_group(const BVtx *in, int nt, BVtx *out, int max_out_verts) {
    const float lox = -MF_VTX_CLIP_PAD, hix = (float)BLT_FB_WIDTH  + MF_VTX_CLIP_PAD;
    const float loy = -MF_VTX_CLIP_PAD, hiy = (float)BLT_FB_HEIGHT + MF_VTX_CLIP_PAD;
    int outn = 0;
    for (int t = 0; t < nt; t++) {
        const BVtx *tri = &in[t * 3];
        if (!mf_vc_finite_tri(tri)) continue;
        BVtx pa[MF_VTX_CLIP_POLY_MAX], pb[MF_VTX_CLIP_POLY_MAX];
        pa[0] = tri[0]; pa[1] = tri[1]; pa[2] = tri[2];
        int n = 3;
        n = mf_vc_clip_plane(pa, n, pb, /*axis=*/0, /*hi=*/0, lox); if (n < 3) continue;
        n = mf_vc_clip_plane(pb, n, pa, /*axis=*/0, /*hi=*/1, hix); if (n < 3) continue;
        n = mf_vc_clip_plane(pa, n, pb, /*axis=*/1, /*hi=*/0, loy); if (n < 3) continue;
        n = mf_vc_clip_plane(pb, n, pa, /*axis=*/1, /*hi=*/1, hiy); if (n < 3) continue;
        if (n > MF_VTX_CLIP_POLY_MAX) n = MF_VTX_CLIP_POLY_MAX;
        for (int k = 1; k + 1 < n; k++) {
            if (outn + 3 > max_out_verts) return outn / 3;
            out[outn++] = pa[0]; out[outn++] = pa[k]; out[outn++] = pa[k + 1];
        }
    }
    return outn / 3;
}
