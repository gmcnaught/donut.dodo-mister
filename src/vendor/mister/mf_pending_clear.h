#ifndef MF_PENDING_CLEAR_H
#define MF_PENDING_CLEAR_H
// Phase 4 Stage B — the deferred full-screen clear.
//
// The game's glClear reaches mf_clear() and is emitted as a ring BLT_OP_FILL
// over the whole 288x216 target: 62,208 pixels at ~1 px/cyc = 0.632 ms of every
// frame, and it lands in the fabric's `ovhd` term (a ring FILL dispatches
// through comp_pipeline, not through the S_TRI_* states `tri` counts).
//
// On the captured streams the opaque draws already repaint the whole screen
// (COPY covers 125,568 px over 62,208 unique), so that fill is redundant. But
// mf_clear() cannot know it: the draws have not arrived yet. So it DEFERS --
// records the fill here -- and the decision is made at the first subsequent
// draw, in mf_emit_group, which is both the single point every surviving draw
// passes and the only place the fabric blend mode is resolved.
//
// THE PROOF IS EXACT, NOT A BOUNDING BOX. Two thin slivers at opposite corners
// have a full-screen bbox and cover almost nothing. mf_pc_is_cover instead
// requires each triangle to be three DISTINCT corners of the target rect, with
// the two triangles together hitting all four: two such triangles are each
// exactly half the rect and are the two complementary halves, so their union is
// the whole rect.
//
// Any condition not provably met emits the fill. The permitted failure is a
// lost 0.632 ms, never a stale pixel.
//
// A pending clear on a target is discharged by EITHER of two draws: one that
// TARGETS it (mf_emit_group's destination-side check, keyed on g_cur_target)
// or one that SAMPLES it (the app-surface composite's BLT_F_SRC_SURFACE read,
// discharged source-side before that same draw's blt_trilist) -- and, failing
// both, by mf_pc_flush_all at frame end. A composite that reads a surface with
// a clear still pending on it is exactly the stale-pixel case this module
// exists to rule out, so the sampling side had to be covered too, not just the
// targeting side.
//
// Pure by construction: no I/O, no globals, no device headers, no engine types,
// so the whole decision is unit-testable on the host.
#include <stdint.h>
#include <string.h>

#define MF_PC_TARGETS 2      /* WORK, APPSURF — mirrors MF_TARGET_* */

typedef struct {
    int      pending;
    int      w, h;
    uint16_t color;
} mf_pc_slot_t;

typedef struct {
    mf_pc_slot_t slot[MF_PC_TARGETS];
    uint32_t dropped;        /* fills proven redundant and never emitted */
    uint32_t emitted;        /* fills that were emitted after all         */
} mf_pc_t;

static inline void mf_pc_reset(mf_pc_t *p) { memset(p, 0, sizeof *p); }

static inline int mf_pc_valid_target(int target) {
    return target >= 0 && target < MF_PC_TARGETS;
}

/* Record a deferred full-extent fill. A second record with one already pending
   means two clears arrived with no draw between them: the second provably
   overwrites the first, so the first is a real saving and is counted as
   dropped rather than silently lost. */
static inline void mf_pc_record(mf_pc_t *p, int target, int w, int h, uint16_t color) {
    if (!mf_pc_valid_target(target)) return;
    mf_pc_slot_t *s = &p->slot[target];
    if (s->pending) p->dropped++;
    s->pending = 1;
    s->w = w; s->h = h; s->color = color;
}

static inline int mf_pc_pending(const mf_pc_t *p, int target) {
    if (!mf_pc_valid_target(target)) return 0;
    return p->slot[target].pending;
}

/* Take the pending fill so the caller can emit it. Returns 0 when there is
   nothing pending, so the call site needs no separate guard. */
static inline int mf_pc_take(mf_pc_t *p, int target, int *w, int *h, uint16_t *color) {
    if (!mf_pc_valid_target(target)) return 0;
    mf_pc_slot_t *s = &p->slot[target];
    if (!s->pending) return 0;
    if (w)     *w     = s->w;
    if (h)     *h     = s->h;
    if (color) *color = s->color;
    s->pending = 0;
    p->emitted++;
    return 1;
}

/* Discard the pending fill as proven redundant. Call ONLY after the covering
   draw has actually been accepted into the ring. */
static inline void mf_pc_drop(mf_pc_t *p, int target) {
    if (!mf_pc_valid_target(target)) return;
    mf_pc_slot_t *s = &p->slot[target];
    if (!s->pending) return;
    s->pending = 0;
    p->dropped++;
}

/* Which corner of the w x h rect is (x,y)? -1 if it is not a corner.
   Comparisons are >= / <= against the exact edges, never a tolerance: a quad
   that falls short must be REJECTED, and one that overhangs still covers. */
static inline int mf_pc_corner_of(float x, float y, int w, int h) {
    int left  = (x <= 0.0f);
    int right = (x >= (float)w);
    int top   = (y <= 0.0f);
    int bot   = (y >= (float)h);
    if (left && top)   return 0;
    if (right && top)  return 1;
    if (left && bot)   return 2;
    if (right && bot)  return 3;
    return -1;
}

static inline int mf_pc_popcount4(int m) {
    int n = 0;
    for (int i = 0; i < 4; i++) if (m & (1 << i)) n++;
    return n;
}

/* Does this draw provably write EVERY pixel of the w x h target?
     blend_copy : the RESOLVED fabric blend is BLT_BLEND_COPY (out = src on
                  every covered pixel — no dst read, no colorkey, no per-texel
                  alpha). Any other mode can leave a pixel untouched.
     nt         : triangle count; only a 2-triangle quad is in scope.
     xs, ys     : nt*3 screen-space vertex coordinates, in triangle order. */
static inline int mf_pc_is_cover(int blend_copy, int nt,
                                 const float *xs, const float *ys,
                                 int w, int h) {
    if (!blend_copy) return 0;
    if (nt != 2) return 0;
    if (!xs || !ys) return 0;
    int mask[2] = { 0, 0 };
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 3; i++) {
            int c = mf_pc_corner_of(xs[t * 3 + i], ys[t * 3 + i], w, h);
            if (c < 0) return 0;              /* not a corner -> not provable */
            mask[t] |= (1 << c);
        }
        if (mf_pc_popcount4(mask[t]) != 3) return 0;   /* degenerate half */
    }
    if ((mask[0] | mask[1]) != 0xF) return 0;

    /* Corner-union alone is NOT sufficient: the two halves of a rect meet
       along a DIAGONAL, not an edge. Two triangles can each hold three
       distinct corners and union to all four while still sharing an EDGE
       pair, leaving a wedge unpainted. Concrete counterexample (W x H rect):
         A = (0,0),(W,0),(0,H)  -> corners TL,TR,BL -> mask 0b0111
         B = (0,0),(W,0),(W,H)  -> corners TL,TR,BR -> mask 0b1011
         union = 0b1111 (all four corners hit) but both A and B contain the
         TOP EDGE, and the point (0.5*W, 0.9*H) lies in neither: for A the
         interior test is x/W + y/H <= 1 -> 0.5 + 0.9 = 1.4 > 1 (outside);
         for B it is y/H <= x/W -> 0.9 <= 0.5, false (outside). A real pixel
         goes unpainted, so a clear discharged on union alone would strand a
         stale pixel on screen -- exactly the failure this module exists to
         prevent.
       The two valid tilings share a DIAGONAL pair: TL|BR = 0x9 or TR|BL =
       0x6 (corner numbering: 0=TL,1=TR,2=BL,3=BR). Do not simplify this
       back down to the union check alone -- it is not equivalent. */
    int shared = mask[0] & mask[1];
    return (shared == 0x9 || shared == 0x6);
}

#endif /* MF_PENDING_CLEAR_H */
