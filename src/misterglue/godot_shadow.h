/*
  GLES2 state shadow + Godot canvas draw decode.

  Godot 3.5's GLES2 canvas renderer is the only client we care about. Its vertex
  path is fully described by the shader Godot generates (see the transform chain
  replicated in decode_draw), so every draw can be turned back into screen-space
  triangles on the CPU without asking GL anything.

  This mirrors gmloader-next's gmloader/mister/blitter.cpp, which does the same
  job for GameMaker's runner. The state-shadow half is engine-independent; the
  canvas-specific parts are the attribute/uniform names and the transform chain.
*/
#ifndef GODOT_SHADOW_H
#define GODOT_SHADOW_H

#include <stdint.h>
#include "blitter_raster.h"   /* BVtx / RTexture / RBlend / RSurface */

namespace gs {

/* Which of Godot's canvas vertex paths a program compiles to. Parsed from the
   #define prologue Godot prepends to the shader source, which we see in
   glShaderSource — that is the authoritative signal, not a guess. */
struct ProgramInfo {
    bool is_canvas       = false;  /* canvas shader (vs the 3D scene shader) */
    bool texture_rect    = false;  /* USE_TEXTURE_RECT: geometry from dst_rect/src_rect */
    bool attrib_large    = false;  /* USE_ATTRIB_LARGE_VERTEX: basis/translate attribs */
    bool attrib_modulate = false;  /* USE_ATTRIB_MODULATE */
    bool skip_transform  = false;  /* SKIP_TRANSFORM_USED */
    bool pixel_snap      = false;  /* USE_PIXEL_SNAP */
    bool instancing      = false;  /* USE_INSTANCING — not decoded, forces fallback */
    bool skeleton        = false;  /* USE_SKELETON  — not decoded, forces fallback */
    bool lighting        = false;  /* USE_LIGHTING  — not decoded, forces fallback */
    bool custom_fragment = false;  /* game-supplied shader code (CRT/White): fallback */
    bool white_flash     = false;  /* Player.shader: COLOR = texel, or texel mixed 75%
                                      toward white while uniform `white` is set */
};

/* tex_key for the pre-whitened copy of a texture (Player.shader's flash). GL
   texture ids are small, so this bit never collides with a real one; the glue
   invalidates it alongside the source id on every re-upload/delete. */
constexpr uint32_t kWhiteTexKeyBit = 0x80000000u;

/* A decoded draw, ready for the RasterBackend seam. */
struct DecodedDraw {
    BVtx     *verts = nullptr;   /* 3 per triangle, screen space, y-down */
    int       tris  = 0;
    RTexture  tex{};
    RBlend    blend = RB_NONE;
    bool      to_default_fb = true;  /* false => render-to-texture */
    uint32_t  dst_fbo = 0;           /* real GL FBO id; the backend compares it
                                        against the detected app surface */
    uint32_t  tex_key = 0;           /* raw GL texture id — the backend matches
                                        this against the app-surface texture, so
                                        it must not be a derived hash */
    bool      src_is_surface = false; /* sampling a render target: no CPU pixels,
                                         the fabric reads its own surface */
};

void init(int scanout_w, int scanout_h);
void shutdown();

/* ---- state shadow ---- */
void on_shader_source(uint32_t shader, const char *src);
void on_attach_shader(uint32_t program, uint32_t shader);
void on_use_program(uint32_t program);
void on_bind_attrib_location(uint32_t program, uint32_t index, const char *name);
void on_get_uniform_location(uint32_t program, const char *name, int loc);
void on_uniform_matrix4fv(int loc, int count, const float *v);
void on_uniform4fv(int loc, int count, const float *v);
void on_uniform4f(int loc, float x, float y, float z, float w);
void on_uniform1i(int loc, int v);
void on_bind_texture(uint32_t target, uint32_t tex);
void on_active_texture(uint32_t unit);
void on_tex_image2d(uint32_t target, int level, int internalfmt, int w, int h,
                    int border, uint32_t fmt, uint32_t type, const void *px);
void on_tex_subimage2d(uint32_t target, int level, int x, int y, int w, int h,
                       uint32_t fmt, uint32_t type, const void *px);
void on_tex_parameteri(uint32_t target, uint32_t pname, int param);
void on_copy_tex_image(uint32_t target);
void on_delete_textures(int n, const uint32_t *ids);
void on_bind_buffer(uint32_t target, uint32_t buf);
void on_buffer_data(uint32_t target, intptr_t size, const void *data, uint32_t usage);
void on_buffer_sub_data(uint32_t target, intptr_t off, intptr_t size, const void *data);
void on_delete_buffers(int n, const uint32_t *ids);
void on_vertex_attrib_pointer(uint32_t index, int size, uint32_t type,
                              uint8_t normalized, int stride, const void *ptr);
void on_enable_vertex_attrib(uint32_t index, bool enabled);
void on_vertex_attrib4f(uint32_t index, float x, float y, float z, float w);
void on_enable_cap(uint32_t cap, bool enabled);
void on_blend_func(uint32_t src, uint32_t dst);
void on_blend_func_separate(uint32_t srcRGB, uint32_t dstRGB, uint32_t srcA, uint32_t dstA);
void on_bind_framebuffer(uint32_t target, uint32_t fbo);
void on_framebuffer_texture2d(uint32_t target, uint32_t attach, uint32_t textarget,
                              uint32_t tex, int level);
void on_gen_framebuffers(int n, const uint32_t *ids);

/* The FBO/texture pair Godot uses as its viewport render target, once a draw has
   revealed it by sampling that texture into the default framebuffer. Returns
   false until then. Mirrors gmloader's application-surface detection. */
bool app_surface(uint32_t *fbo, uint32_t *tex);
/* Texture bound to unit 0 — the identity the fabric caches staged texels under. */
uint32_t bound_texture();
uint32_t bound_fbo();
void on_viewport(int x, int y, int w, int h);
void on_scissor(int x, int y, int w, int h);
void on_clear_color(float r, float g, float b, float a);

/* ---- draw decode ----
   Returns true and fills `out` when the draw is fully understood; false means
   the caller must fall back to real GL for this draw. */
bool decode_draw_arrays(uint32_t mode, int first, int count, DecodedDraw *out);
bool decode_draw_elements(uint32_t mode, int count, uint32_t type,
                          const void *indices, DecodedDraw *out);

/* Current clear colour, for a fabric FILL. */
void get_clear_color(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);
/* True when the bound draw target is the default framebuffer. */
bool target_is_default();

/* Diagnostics */
struct Stats {
    uint32_t draws_seen, draws_decoded, draws_fallback;
    uint32_t fallback_custom_shader, fallback_program, fallback_mode,
             fallback_attrib, fallback_texture, fallback_blend;
    /* texture-fallback breakdown, to tell "no texture bound" from "we have the
       object but never saw usable pixels" (render targets, unknown formats) */
    uint32_t tex_unbound, tex_missing, tex_invalid, src_surface_draws;
    uint32_t last_bad_tex, last_bad_w, last_bad_h;
    /* uploads we could not mirror: which GL format/type defeated us */
    uint32_t uploads_rejected, subuploads_rejected, last_rej_fmt, last_rej_type;
    uint32_t copytex_calls;
    uint32_t tex_never_uploaded;
};
const Stats &stats();
void stats_reset();

} /* namespace gs */
#endif /* GODOT_SHADOW_H */
