// RasterBackend seam (Task 3). Behind this vtable sits the code that actually
// owns pixels: clear a surface, rasterize a decoded triangle list into it, and
// present the finished frame. blitter.cpp's GL-free decode (RSurface/BVtx/
// RTexture/RBlend, produced by state-shadow + draw decode) is unchanged by
// this seam — only the callee for clear/draw/present moves behind it.
//
// Task 3 ships exactly one implementation, `backend_sw`, which is a thin
// wrapper around today's software rasterizer (blitter_raster.cpp) with zero
// added logic and zero pixel change. A later task adds an FPGA-fabric
// back-end (`backend_mfgpu`) behind the same seam and makes the selector
// choose between them; RasterBackend_Select() always returns backend_sw here.
#ifndef RASTER_BACKEND_H
#define RASTER_BACKEND_H
#include "blitter_raster.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RasterBackend {
    const char *name;
    void (*frame_begin)(void);
    void (*clear)(RSurface *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void (*draw)(RSurface *dst, const BVtx *verts, int triCount,
                 const RTexture *tex, RBlend blend, float alphaRef,
                 uint32_t tex_key);
    void (*present)(const RSurface *defSurf);
    void (*frame_end)(void);
} RasterBackend;

/* Returns the active back-end. Task 3 always returns backend_sw;
 * Task 6 makes this env-selectable. */
const RasterBackend *RasterBackend_Select(void);

/* SW back-end only: mirror blitter.cpp's g_threads (GMLOADER_BLITTER_THREADS)
 * so backend_sw's rasterizer uses the same worker-thread count the direct
 * Blitter_RasterDraw call used before this refactor. Call once at init. */
void RasterBackend_SW_SetThreads(int n);

/* mfgpu back-end only: drop the cached staging of GL texture `id` (on GL
 * re-upload/delete). No-op when backend_sw is selected or nothing is cached. */
void RasterBackend_MFGPU_InvalidateTex(uint32_t id);

/* mfgpu back-end only [Phase 3 Stage B]: read the fabric's scanout instrument
 * (openbor_video_reader.sv, byte 0x3BFB0018) out of the back-end's existing DDR
 * mapping. *frame_cnt = monotonic scanout frame count (+1 per displayed frame
 * boundary, wraps at 2^32); *period_cyc = clk_sys (98.4375 MHz) cycles between
 * the last two boundaries. Either pointer may be NULL.
 * Returns 1 when the instrument is readable, 0 otherwise (host build, no
 * /dev/mem, or the mapping not yet established) — 0 leaves the outputs
 * untouched and obliges the caller to fall back rather than wait on it. */
int RasterBackend_MFGPU_ScanoutRead(uint32_t *frame_cnt, uint32_t *period_cyc);

/* mfgpu back-end only: quiesce the fabric and leave its DDR window in a state the
 * NEXT engine can start from. Waits (bounded) for the in-flight batch to be acked,
 * zeroes the command rings, and parks the control block idle without ever writing
 * the fabric-owned C_DONE.
 *
 * The window at 0x3B000000 outlives the process — load_core does not clear it and
 * neither does the kernel — so an engine that dies without running this leaves a
 * stale ring and a live doorbell behind, which is the frame-1 wedge the next engine
 * used to inherit. The back-end already runs this from atexit() and from its own
 * SIGTERM/SIGINT/SIGHUP/SIGQUIT handler; main.cpp calls it from the crash handler,
 * which owns SIGSEGV/SIGABRT/SIGBUS and must not have those hooked twice.
 *
 * Async-signal-safe, idempotent (runs at most once per process), and a no-op on a
 * host build, when backend_sw is selected, or when the DDR window was never mapped. */
void RasterBackend_MFGPU_Shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
