#ifndef MF_SEAM_STAT_H
#define MF_SEAM_STAT_H
// Phase 4 Stage A — the submit-seam decomposition accumulator.
//
//   period = host + block + pub
//     host  = doorbell N            -> mf_publish_barrier entry
//     block = barrier entry         -> barrier returns true  (the real wait on the fabric)
//     pub   = barrier return        -> doorbell N+1
//
// Three CONSECUTIVE intervals, so no term is a residual. That shape is the point:
// the 2026-07-30 exposed-cost audit sec 6.2 records BLITPROF's `logic` residual
// silently absorbing a 16.9 ms deferred await, because a residual absorbs whatever
// nobody named.
//
// Pure by construction: no I/O, no globals, no device headers, so all of the
// arithmetic is unit-testable on the host. The device side contributes timestamps
// and nothing else.
#include <stdint.h>
#include <string.h>

#define MF_SEAM_WINDOW   30      /* frames per report; matches MFSUBMIT's window */
#define MF_SEAM_TOL_MS   0.05    /* identity slack for the timestamp reads themselves */
#define MF_SEAM_BUCKETS  8

/* Upper edges in ms, lower-inclusive. Fine below 1 ms because that is where
   `pub` must land for a 16.6882 ms period against a 16.20 ms fabric; the last
   edge IS the scanout period, so the top bucket means "this frame could not have
   locked". */
static const double MF_SEAM_EDGES[MF_SEAM_BUCKETS - 1] = {
    0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.6882
};

static inline int mf_seam_bucket(double ms) {
    for (int i = 0; i < MF_SEAM_BUCKETS - 1; i++)
        if (ms < MF_SEAM_EDGES[i]) return i;
    return MF_SEAM_BUCKETS - 1;
}

typedef struct {
    unsigned n;
    unsigned suspect;            /* frames whose parts did not sum to their period */
    unsigned blocked;            /* frames where the host actually waited on the fabric */
    double   host_sum, block_sum, pub_sum, period_sum;
    double   notice_sum;         /* summed over blocked frames only */
    uint32_t host_hist[MF_SEAM_BUCKETS];
    uint32_t pub_hist[MF_SEAM_BUCKETS];
} mf_seam_acc_t;

typedef struct {
    double   host_ms, block_ms, pub_ms, period_ms;
    double   notice_ms;          /* mean over BLOCKED frames; 0 when none blocked */
    double   blocked_frac;       /* read notice_ms together with this, never alone */
    unsigned suspect;
} mf_seam_out_t;

static inline void mf_seam_reset(mf_seam_acc_t *a) { memset(a, 0, sizeof *a); }

/* frame_ms is the fabric's own compute counter (C_DONE.hi) for the batch this
   frame's `block` waited on. `blocked` is the CALLER's answer to "did the host
   actually wait?" — it cannot be derived from block_ms, because a frame that
   never waited still spends the few microseconds of one uncached C_DONE read
   inside the await, so block_ms is never exactly zero on device. */
static inline void mf_seam_add(mf_seam_acc_t *a, double host_ms, double block_ms,
                               double pub_ms, double period_ms, double frame_ms,
                               int blocked) {
    a->n++;
    a->host_sum   += host_ms;
    a->block_sum  += block_ms;
    a->pub_sum    += pub_ms;
    a->period_sum += period_ms;

    double d = (host_ms + block_ms + pub_ms) - period_ms;
    if (d < 0.0) d = -d;
    if (d >= MF_SEAM_TOL_MS) a->suspect++;

    /* Only a frame that actually blocked measures the fabric's doorbell->done
       latency; on a frame that arrived late, host+block is the host's own
       lateness and would overstate `notice` by all of it. */
    if (blocked) {
        a->blocked++;
        a->notice_sum += (host_ms + block_ms) - frame_ms;
    }

    a->host_hist[mf_seam_bucket(host_ms)]++;
    a->pub_hist[mf_seam_bucket(pub_ms)]++;
}

static inline int mf_seam_ready(const mf_seam_acc_t *a) {
    return a->n >= MF_SEAM_WINDOW;
}

static inline void mf_seam_derive(const mf_seam_acc_t *a, mf_seam_out_t *o) {
    const double n = a->n ? (double)a->n : 1.0;   /* an empty window reports zeros */
    o->host_ms   = a->host_sum   / n;
    o->block_ms  = a->block_sum  / n;
    o->pub_ms    = a->pub_sum    / n;
    o->period_ms = a->period_sum / n;
    o->suspect   = a->suspect;
    o->notice_ms    = a->blocked ? a->notice_sum / (double)a->blocked : 0.0;
    o->blocked_frac = a->blocked / n;
}

#endif /* MF_SEAM_STAT_H */
