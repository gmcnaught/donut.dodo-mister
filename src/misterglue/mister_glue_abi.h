/*
  ABI between our patched SDL2 and libmisterglue.so.

  SDL owns the GL context (Mesa/llvmpipe, surfaceless EGL) and the window; the
  glue owns the FPGA fabric. SDL asks the glue to resolve each GL entry point
  Godot looks up: the glue returns its own wrapper for calls it shadows, or NULL
  to hand back Mesa's function unchanged. Every wrapper can still call into Mesa
  through host->get_real_proc, so an unsupported draw falls back per draw rather
  than per frame.

  Kept deliberately small: SDL should not need rebuilding when the glue changes.
*/
#ifndef MISTER_GLUE_ABI_H
#define MISTER_GLUE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MISTER_GLUE_ABI_VERSION 1
#define MISTER_GLUE_ENTRY       "MisterGlue_Get"

typedef struct MisterGlueHost {
    uint32_t abi_version;
    /* Resolve the real (Mesa) entry point; the glue keeps these for fallback. */
    void *(*get_real_proc)(const char *proc);
    /* Scanout geometry the FPGA core expects — the fabric's wire contract. */
    int scanout_w;
    int scanout_h;
    /* Optional logger; may be NULL. */
    void (*log)(const char *msg);
} MisterGlueHost;

typedef struct MisterGlue {
    uint32_t abi_version;

    /* Returns 0 on success. On failure SDL keeps using Mesa for everything. */
    int (*init)(const MisterGlueHost *host);
    void (*shutdown)(void);

    /* NULL => caller should use the real GL function for this name. */
    void *(*resolve)(const char *proc);

    /* Called from SDL_GL_SwapWindow. Returns 1 if the glue presented the frame
       (fabric owns scanout, SDL must not read pixels back), 0 if SDL should
       present the GL framebuffer itself. */
    int (*present)(void);
} MisterGlue;

/* The module's single exported symbol. */
const MisterGlue *MisterGlue_Get(void);

#ifdef __cplusplus
}
#endif
#endif /* MISTER_GLUE_ABI_H */
