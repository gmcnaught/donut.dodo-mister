/*
  libmisterglue.so — the module our patched SDL2 loads.

  Every GL entry point Godot resolves goes through resolve(): we hand back a
  wrapper for the calls we shadow and NULL for everything else. Wrappers keep the
  shadow current and then call Mesa, so the rendered picture is unchanged until
  the fabric path is switched on with MISTER_GLUE_FABRIC=1 — at which point a
  decoded draw is emitted to the FPGA and the Mesa call is skipped, while
  anything we cannot decode still falls through to Mesa, per draw.
*/
#include "mister_glue_abi.h"
#include "godot_shadow.h"

#include <stdarg.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "raster_backend.h"
/* Declared in raster_backend_mfgpu.cpp but absent from the seam header; the
   app-surface registration is how the backend learns which FBO/texture pair is
   the engine's render target. */
void RasterBackend_MFGPU_SetAppSurface(uint32_t fbo, uint32_t tex);
}

namespace {

typedef void (*PFN)(void);

struct RealGL {
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*AttachShader)(unsigned, unsigned);
    void (*UseProgram)(unsigned);
    int  (*GetUniformLocation)(unsigned, const char *);
    void (*UniformMatrix4fv)(int, int, unsigned char, const float *);
    void (*Uniform4fv)(int, int, const float *);
    void (*Uniform4f)(int, float, float, float, float);
    void (*Uniform1i)(int, int);
    void (*BindTexture)(unsigned, unsigned);
    void (*ActiveTexture)(unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*CopyTexImage2D)(unsigned, int, unsigned, int, int, int, int, int);
    void (*CopyTexSubImage2D)(unsigned, int, int, int, int, int, int, int);
    void (*DeleteTextures)(int, const unsigned *);
    void (*BindBuffer)(unsigned, unsigned);
    void (*BufferData)(unsigned, intptr_t, const void *, unsigned);
    void (*BufferSubData)(unsigned, intptr_t, intptr_t, const void *);
    void (*DeleteBuffers)(int, const unsigned *);
    void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
    void (*EnableVertexAttribArray)(unsigned);
    void (*DisableVertexAttribArray)(unsigned);
    void (*VertexAttrib4f)(unsigned, float, float, float, float);
    void (*VertexAttrib4fv)(unsigned, const float *);
    void (*Enable)(unsigned);
    void (*Disable)(unsigned);
    void (*BlendFunc)(unsigned, unsigned);
    void (*BlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    void (*GenFramebuffers)(int, unsigned *);
    void (*Viewport)(int, int, int, int);
    void (*Scissor)(int, int, int, int);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    void (*DrawArrays)(unsigned, int, int);
    void (*DrawElements)(unsigned, int, unsigned, const void *);
};

RealGL gl;
MisterGlueHost host;
bool g_inited      = false;
bool g_fabric      = false;   /* MISTER_GLUE_FABRIC=1: let the fabric own drawing */
bool g_log_stats   = false;
unsigned g_frames  = 0;

const RasterBackend *g_backend = nullptr;

void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Surfaces the software back-end draws into. The fabric owns its own pixels, so
   these exist only for backend_sw — which we keep wired as an oracle: the same
   decoded draw stream rendered on the CPU, to tell a decode bug apart from an
   emit/staging bug. */
struct GlueSurfaces {
    RSurface def{};
    RSurface app{};
    bool     ready = false;
} g_surf;

unsigned g_dump_frame = 0;   /* MISTER_GLUE_DUMP=N: dump the default surface at frame N */
bool     g_sw_surfaces = false;

/* Frame pacing. This core snapshots the composited WORK framebuffer out to the
   DDR scanout buffers on vblank rising (blitter_top's S_SNAP_WAIT). Submitting
   faster than the 59.92 Hz scanout therefore does not show more frames — it
   starves the snapshot, and the display freezes on the last frame that caught a
   vblank while C_SUBMIT/C_DONE keep climbing. Pace to the scanout instead.
   MISTER_GLUE_FPS=0 disables pacing (for throughput measurements). */
double   g_pace_hz = 59.92;
uint64_t g_next_frame_ns = 0;

/* Video control word (0x3A000000) — OFF by default, and it should stay that way
   on the fabric path. That word belongs to the SOFTWARE video producer: gmloader
   writes it from NativeVideoWriter only when the CPU rasterizer owns the frame.
   On the fabric path gmloader's main loop deliberately does nothing at present
   time ("the core composites into on-chip BRAM and scans itself out"), so
   publishing it here injects a second producer into a path that has one.
   Retained behind MISTER_GLUE_PUBLISH=1 purely as a diagnostic. */
volatile uint8_t *g_vctrl = nullptr;
int      g_vctrl_fd = -1;
uint32_t g_vframe = 0;
uint32_t g_vbank  = 0;

bool vctrl_init(void) {
    g_vctrl_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_vctrl_fd < 0) return false;
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, g_vctrl_fd, 0x3A000000u);
    if (m == MAP_FAILED) { close(g_vctrl_fd); g_vctrl_fd = -1; return false; }
    g_vctrl = (volatile uint8_t *)m;
    return true;
}

void vctrl_publish(void) {
    if (!g_vctrl) return;
    g_vframe++;
    *(volatile uint32_t *)g_vctrl = (g_vframe << 2) | (g_vbank & 1u);
    g_vbank ^= 1u;
}

void vctrl_shutdown(void) {
    if (g_vctrl) { munmap((void *)g_vctrl, 0x1000); g_vctrl = nullptr; }
    if (g_vctrl_fd >= 0) { close(g_vctrl_fd); g_vctrl_fd = -1; }
}

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pace_frame(void) {
    if (g_pace_hz <= 0.0) return;
    const uint64_t interval = (uint64_t)(1e9 / g_pace_hz);
    const uint64_t now = now_ns();
    if (g_next_frame_ns == 0 || now > g_next_frame_ns + interval * 4) {
        g_next_frame_ns = now + interval;   /* first frame, or we fell far behind */
        return;
    }
    if (g_next_frame_ns > now) {
        struct timespec ts;
        uint64_t d = g_next_frame_ns - now;
        ts.tv_sec  = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        nanosleep(&ts, NULL);
    }
    g_next_frame_ns += interval;
}

bool surfaces_init(int w, int h) {
    if (g_surf.ready) return true;
    g_surf.def.rgba = (uint8_t *)calloc((size_t)w * h, 4);
    g_surf.app.rgba = (uint8_t *)calloc((size_t)w * h, 4);
    if (!g_surf.def.rgba || !g_surf.app.rgba) return false;
    g_surf.def.w = g_surf.app.w = w;
    g_surf.def.h = g_surf.app.h = h;
    g_surf.ready = true;
    return true;
}

void dump_surface(const RSurface *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s->rgba, 1, (size_t)s->w * s->h * 4, f);
    fclose(f);
    logf_("misterglue: dumped %dx%d RGBA to %s", s->w, s->h, path);
}

void logf_(const char *fmt, ...) {
    if (!host.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host.log(buf);
}

/* ---- wrappers ------------------------------------------------------------ */
void w_ShaderSource(unsigned s, int count, const char *const *str, const int *len) {
    /* Godot passes the whole generated source in one or more chunks; join enough
       of it to see the #define prologue and the uniform names. */
    if (count > 0 && str) {
        size_t total = 0;
        for (int i = 0; i < count; i++)
            total += (len && len[i] > 0) ? (size_t)len[i] : (str[i] ? strlen(str[i]) : 0);
        char *joined = (char *)malloc(total + 1);
        if (joined) {
            size_t w = 0;
            for (int i = 0; i < count; i++) {
                if (!str[i]) continue;
                size_t n = (len && len[i] > 0) ? (size_t)len[i] : strlen(str[i]);
                memcpy(joined + w, str[i], n);
                w += n;
            }
            joined[w] = 0;
            gs::on_shader_source(s, joined);
            free(joined);
        }
    }
    gl.ShaderSource(s, count, str, len);
}

void w_AttachShader(unsigned p, unsigned s) { gs::on_attach_shader(p, s); gl.AttachShader(p, s); }
void w_UseProgram(unsigned p) { gs::on_use_program(p); gl.UseProgram(p); }

int w_GetUniformLocation(unsigned p, const char *name) {
    int loc = gl.GetUniformLocation(p, name);
    gs::on_get_uniform_location(p, name, loc);
    return loc;
}

void w_UniformMatrix4fv(int l, int c, unsigned char t, const float *v) {
    gs::on_uniform_matrix4fv(l, c, v);
    gl.UniformMatrix4fv(l, c, t, v);
}
void w_Uniform4fv(int l, int c, const float *v) { gs::on_uniform4fv(l, c, v); gl.Uniform4fv(l, c, v); }
void w_Uniform4f(int l, float x, float y, float z, float w) {
    gs::on_uniform4f(l, x, y, z, w); gl.Uniform4f(l, x, y, z, w);
}
void w_Uniform1i(int l, int v) { gs::on_uniform1i(l, v); gl.Uniform1i(l, v); }

void w_BindTexture(unsigned t, unsigned tex) { gs::on_bind_texture(t, tex); gl.BindTexture(t, tex); }
void w_ActiveTexture(unsigned u) { gs::on_active_texture(u); gl.ActiveTexture(u); }

void w_TexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int b,
                  unsigned fmt, unsigned type, const void *px) {
    gs::on_tex_image2d(tgt, lvl, ifmt, w, h, b, fmt, type, px);
    if (g_fabric) RasterBackend_MFGPU_InvalidateTex(gs::bound_texture());
    gl.TexImage2D(tgt, lvl, ifmt, w, h, b, fmt, type, px);
}
void w_TexSubImage2D(unsigned tgt, int lvl, int x, int y, int w, int h,
                     unsigned fmt, unsigned type, const void *px) {
    gs::on_tex_subimage2d(tgt, lvl, x, y, w, h, fmt, type, px);
    if (g_fabric) RasterBackend_MFGPU_InvalidateTex(gs::bound_texture());
    gl.TexSubImage2D(tgt, lvl, x, y, w, h, fmt, type, px);
}
void w_CopyTexImage2D(unsigned tgt, int lvl, unsigned ifmt, int x, int y, int w, int h, int b) {
    gs::on_copy_tex_image(tgt);
    gl.CopyTexImage2D(tgt, lvl, ifmt, x, y, w, h, b);
}
void w_CopyTexSubImage2D(unsigned tgt, int lvl, int xo, int yo, int x, int y, int w, int h) {
    gs::on_copy_tex_image(tgt);
    gl.CopyTexSubImage2D(tgt, lvl, xo, yo, x, y, w, h);
}

void w_TexParameteri(unsigned t, unsigned p, int v) { gs::on_tex_parameteri(t, p, v); gl.TexParameteri(t, p, v); }
void w_DeleteTextures(int n, const unsigned *ids) {
    if (g_fabric)
        for (int i = 0; i < n; i++) RasterBackend_MFGPU_InvalidateTex(ids[i]);
    gs::on_delete_textures(n, ids);
    gl.DeleteTextures(n, ids);
}

void w_BindBuffer(unsigned t, unsigned b) { gs::on_bind_buffer(t, b); gl.BindBuffer(t, b); }
void w_BufferData(unsigned t, intptr_t sz, const void *d, unsigned u) {
    gs::on_buffer_data(t, sz, d, u); gl.BufferData(t, sz, d, u);
}
void w_BufferSubData(unsigned t, intptr_t o, intptr_t sz, const void *d) {
    gs::on_buffer_sub_data(t, o, sz, d); gl.BufferSubData(t, o, sz, d);
}
void w_DeleteBuffers(int n, const unsigned *ids) { gs::on_delete_buffers(n, ids); gl.DeleteBuffers(n, ids); }

void w_VertexAttribPointer(unsigned i, int s, unsigned t, unsigned char n, int st, const void *p) {
    gs::on_vertex_attrib_pointer(i, s, t, n, st, p);
    gl.VertexAttribPointer(i, s, t, n, st, p);
}
void w_EnableVertexAttribArray(unsigned i) { gs::on_enable_vertex_attrib(i, true); gl.EnableVertexAttribArray(i); }
void w_DisableVertexAttribArray(unsigned i) { gs::on_enable_vertex_attrib(i, false); gl.DisableVertexAttribArray(i); }
void w_VertexAttrib4f(unsigned i, float x, float y, float z, float w) {
    gs::on_vertex_attrib4f(i, x, y, z, w); gl.VertexAttrib4f(i, x, y, z, w);
}

void w_VertexAttrib4fv(unsigned i, const float *v) {
    if (v) gs::on_vertex_attrib4f(i, v[0], v[1], v[2], v[3]);
    gl.VertexAttrib4fv(i, v);
}

void w_Enable(unsigned cap) { gs::on_enable_cap(cap, true); gl.Enable(cap); }
void w_Disable(unsigned cap) { gs::on_enable_cap(cap, false); gl.Disable(cap); }
void w_BlendFunc(unsigned s, unsigned d) { gs::on_blend_func(s, d); gl.BlendFunc(s, d); }
void w_BlendFuncSeparate(unsigned sr, unsigned dr, unsigned sa, unsigned da) {
    gs::on_blend_func_separate(sr, dr, sa, da); gl.BlendFuncSeparate(sr, dr, sa, da);
}

void w_BindFramebuffer(unsigned t, unsigned f) { gs::on_bind_framebuffer(t, f); gl.BindFramebuffer(t, f); }
void w_GenFramebuffers(int n, unsigned *ids) {
    gl.GenFramebuffers(n, ids);
    gs::on_gen_framebuffers(n, ids);
}

void w_FramebufferTexture2D(unsigned t, unsigned a, unsigned tt, unsigned tex, int l) {
    gs::on_framebuffer_texture2d(t, a, tt, tex, l);
    gl.FramebufferTexture2D(t, a, tt, tex, l);
}
void w_Viewport(int x, int y, int w, int h) { gs::on_viewport(x, y, w, h); gl.Viewport(x, y, w, h); }
void w_Scissor(int x, int y, int w, int h) { gs::on_scissor(x, y, w, h); gl.Scissor(x, y, w, h); }
void w_ClearColor(float r, float g_, float b, float a) { gs::on_clear_color(r, g_, b, a); gl.ClearColor(r, g_, b, a); }

void w_Clear(unsigned mask) {
    if (g_fabric && g_backend) {
        uint32_t as_fbo = 0;
        bool is_def = gs::target_is_default();
        bool is_app = gs::app_surface(&as_fbo, nullptr) && !is_def && gs::bound_fbo() == as_fbo;
        if (is_def || is_app) {
            uint8_t r, gg, b, a;
            gs::get_clear_color(&r, &gg, &b, &a);
            RSurface dst{};
            dst.w = host.scanout_w;
            dst.h = host.scanout_h;
            dst.fbo = is_def ? 0 : as_fbo;
            if (g_sw_surfaces) {
                RSurface *s = is_def ? &g_surf.def : &g_surf.app;
                g_backend->clear(s, r, gg, b, a);
            } else {
                g_backend->clear(&dst, r, gg, b, a);
            }
        }
    }
    gl.Clear(mask);
}

/* The fabric can own a draw that targets the default framebuffer or the
   application surface; any other FBO is still Mesa's. */
bool fabric_owns(const gs::DecodedDraw &d) {
    uint32_t as_fbo = 0;
    if (d.to_default_fb) return true;
    return gs::app_surface(&as_fbo, nullptr) && d.dst_fbo == as_fbo;
}

void dispatch_decoded(const gs::DecodedDraw &d) {
    /* Keep the backend's view of the application surface current: it decides
       SET_TARGET(APPSURF) and BLT_F_SRC_SURFACE off these two ids. */
    static uint32_t reported_fbo, reported_tex;
    uint32_t as_fbo = 0, as_tex = 0;
    if (gs::app_surface(&as_fbo, &as_tex) &&
        (as_fbo != reported_fbo || as_tex != reported_tex)) {
        RasterBackend_MFGPU_SetAppSurface(as_fbo, as_tex);
        reported_fbo = as_fbo;
        reported_tex = as_tex;
        logf_("misterglue: app surface = fbo %u tex %u", as_fbo, as_tex);
    }

    RSurface dst{};
    dst.w = host.scanout_w;
    dst.h = host.scanout_h;
    dst.fbo = d.dst_fbo;    /* real GL id: the backend compares it to the app surface */

    if (g_sw_surfaces) {
        /* CPU oracle: pick the real destination pixels, and give a render-target
           sample the app surface's pixels (the fabric reads its own surface, the
           software rasterizer needs the buffer). */
        RSurface *s = (d.dst_fbo == 0) ? &g_surf.def : &g_surf.app;
        RTexture tex = d.tex;
        if (d.src_is_surface) {
            tex.rgba   = g_surf.app.rgba;
            tex.w      = g_surf.app.w;
            tex.h      = g_surf.app.h;
            tex.format = RTEX_RGBA8888;
            tex.valid  = 1;
            tex.opaque = 0;
        }
        g_backend->draw(s, d.verts, d.tris, &tex, d.blend, 0.0f, d.tex_key);
        return;
    }

    g_backend->draw(&dst, d.verts, d.tris, &d.tex, d.blend, 0.0f, d.tex_key);
}

void w_DrawArrays(unsigned mode, int first, int count) {
    gs::DecodedDraw d;
    bool decoded = gs::decode_draw_arrays(mode, first, count, &d);
    if (g_fabric && decoded && g_backend && fabric_owns(d)) {
        dispatch_decoded(d);
        return;                       /* fabric owns this draw */
    }
    gl.DrawArrays(mode, first, count);
}

void w_DrawElements(unsigned mode, int count, unsigned type, const void *indices) {
    gs::DecodedDraw d;
    bool decoded = gs::decode_draw_elements(mode, count, type, indices, &d);
    if (g_fabric && decoded && g_backend && fabric_owns(d)) {
        dispatch_decoded(d);
        return;
    }
    gl.DrawElements(mode, count, type, indices);
}

/* ---- module entry points -------------------------------------------------- */
struct Entry { const char *name; void *fn; };

const Entry kEntries[] = {
    { "glShaderSource", (void *)w_ShaderSource },
    { "glAttachShader", (void *)w_AttachShader },
    { "glUseProgram", (void *)w_UseProgram },
    { "glGetUniformLocation", (void *)w_GetUniformLocation },
    { "glUniformMatrix4fv", (void *)w_UniformMatrix4fv },
    { "glUniform4fv", (void *)w_Uniform4fv },
    { "glUniform4f", (void *)w_Uniform4f },
    { "glUniform1i", (void *)w_Uniform1i },
    { "glBindTexture", (void *)w_BindTexture },
    { "glActiveTexture", (void *)w_ActiveTexture },
    { "glTexImage2D", (void *)w_TexImage2D },
    { "glTexSubImage2D", (void *)w_TexSubImage2D },
    { "glTexParameteri", (void *)w_TexParameteri },
    { "glCopyTexImage2D", (void *)w_CopyTexImage2D },
    { "glCopyTexSubImage2D", (void *)w_CopyTexSubImage2D },
    { "glDeleteTextures", (void *)w_DeleteTextures },
    { "glBindBuffer", (void *)w_BindBuffer },
    { "glBufferData", (void *)w_BufferData },
    { "glBufferSubData", (void *)w_BufferSubData },
    { "glDeleteBuffers", (void *)w_DeleteBuffers },
    { "glVertexAttribPointer", (void *)w_VertexAttribPointer },
    { "glEnableVertexAttribArray", (void *)w_EnableVertexAttribArray },
    { "glDisableVertexAttribArray", (void *)w_DisableVertexAttribArray },
    { "glVertexAttrib4f", (void *)w_VertexAttrib4f },
    { "glVertexAttrib4fv", (void *)w_VertexAttrib4fv },
    { "glEnable", (void *)w_Enable },
    { "glDisable", (void *)w_Disable },
    { "glBlendFunc", (void *)w_BlendFunc },
    { "glBlendFuncSeparate", (void *)w_BlendFuncSeparate },
    { "glBindFramebuffer", (void *)w_BindFramebuffer },
    { "glFramebufferTexture2D", (void *)w_FramebufferTexture2D },
    { "glGenFramebuffers", (void *)w_GenFramebuffers },
    { "glViewport", (void *)w_Viewport },
    { "glScissor", (void *)w_Scissor },
    { "glClearColor", (void *)w_ClearColor },
    { "glClear", (void *)w_Clear },
    { "glDrawArrays", (void *)w_DrawArrays },
    { "glDrawElements", (void *)w_DrawElements },
};

#define REAL(field, name)                                                    \
    do {                                                                     \
        *(void **)&gl.field = host.get_real_proc(name);                      \
        if (!gl.field) { logf_("misterglue: missing %s", name); return -1; }  \
    } while (0)

int glue_init(const MisterGlueHost *h) {
    if (!h || h->abi_version != MISTER_GLUE_ABI_VERSION || !h->get_real_proc) return -1;
    host = *h;

    REAL(ShaderSource, "glShaderSource");
    REAL(AttachShader, "glAttachShader");
    REAL(UseProgram, "glUseProgram");
    REAL(GetUniformLocation, "glGetUniformLocation");
    REAL(UniformMatrix4fv, "glUniformMatrix4fv");
    REAL(Uniform4fv, "glUniform4fv");
    REAL(Uniform4f, "glUniform4f");
    REAL(Uniform1i, "glUniform1i");
    REAL(BindTexture, "glBindTexture");
    REAL(ActiveTexture, "glActiveTexture");
    REAL(TexImage2D, "glTexImage2D");
    REAL(TexSubImage2D, "glTexSubImage2D");
    REAL(TexParameteri, "glTexParameteri");
    REAL(CopyTexImage2D, "glCopyTexImage2D");
    REAL(CopyTexSubImage2D, "glCopyTexSubImage2D");
    REAL(DeleteTextures, "glDeleteTextures");
    REAL(BindBuffer, "glBindBuffer");
    REAL(BufferData, "glBufferData");
    REAL(BufferSubData, "glBufferSubData");
    REAL(DeleteBuffers, "glDeleteBuffers");
    REAL(VertexAttribPointer, "glVertexAttribPointer");
    REAL(EnableVertexAttribArray, "glEnableVertexAttribArray");
    REAL(DisableVertexAttribArray, "glDisableVertexAttribArray");
    REAL(VertexAttrib4f, "glVertexAttrib4f");
    REAL(VertexAttrib4fv, "glVertexAttrib4fv");
    REAL(Enable, "glEnable");
    REAL(Disable, "glDisable");
    REAL(BlendFunc, "glBlendFunc");
    REAL(BlendFuncSeparate, "glBlendFuncSeparate");
    REAL(BindFramebuffer, "glBindFramebuffer");
    REAL(FramebufferTexture2D, "glFramebufferTexture2D");
    REAL(GenFramebuffers, "glGenFramebuffers");
    REAL(Viewport, "glViewport");
    REAL(Scissor, "glScissor");
    REAL(ClearColor, "glClearColor");
    REAL(Clear, "glClear");
    REAL(DrawArrays, "glDrawArrays");
    REAL(DrawElements, "glDrawElements");

    gs::init(host.scanout_w, host.scanout_h);

    const char *fab = getenv("MISTER_GLUE_FABRIC");
    g_fabric = fab && *fab == '1';
    const char *st = getenv("MISTER_GLUE_STATS");
    g_log_stats = st && *st == '1';

    if (g_fabric) {
        g_backend = RasterBackend_Select();
        g_sw_surfaces = g_backend && strcmp(g_backend->name, "sw") == 0;
        if (g_sw_surfaces && !surfaces_init(host.scanout_w, host.scanout_h)) {
            logf_("misterglue: surface alloc failed");
            return -1;
        }
        {
            const char *dp = getenv("MISTER_GLUE_DUMP");
            g_dump_frame = dp ? (unsigned)atoi(dp) : 0;
            const char *fp = getenv("MISTER_GLUE_FPS");
            if (fp) g_pace_hz = atof(fp);
            const char *pub = getenv("MISTER_GLUE_PUBLISH");
            if (pub && *pub == '1') {
                if (!vctrl_init())
                    logf_("misterglue: video control word unavailable (need /dev/mem)");
            }
            logf_("misterglue: frame pacing %.2f Hz%s", g_pace_hz,
                  g_pace_hz > 0.0 ? "" : " (off)");
        }
        logf_("misterglue: fabric ON, backend=%s, scanout %dx%d",
              g_backend ? g_backend->name : "none", host.scanout_w, host.scanout_h);
        if (!g_backend) { g_fabric = false; return -1; }
    } else {
        logf_("misterglue: shadow-only (set MISTER_GLUE_FABRIC=1 to drive the fabric)");
    }

    g_inited = true;
    return 0;
}

void glue_shutdown() {
    if (!g_inited) return;
    vctrl_shutdown();
    gs::shutdown();
    g_inited = false;
}

void *glue_resolve(const char *proc) {
    if (!g_inited || !proc) return nullptr;
    for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); i++)
        if (!strcmp(proc, kEntries[i].name)) return kEntries[i].fn;
    return nullptr;
}

int glue_present() {
    g_frames++;
    if (g_fabric && g_backend) {
        RSurface def{};
        def.w = host.scanout_w;
        def.h = host.scanout_h;
        /* present() closes the frame itself (mf_present -> mf_frame_end); calling
           frame_end() again here submitted a second, empty batch per frame —
           visible as a submit rate at twice the presented frame rate. */
        g_backend->present(g_sw_surfaces ? &g_surf.def : &def);
        if (g_sw_surfaces && g_dump_frame && g_frames == g_dump_frame) {
            dump_surface(&g_surf.def, "/tmp/glue_def.raw");
            dump_surface(&g_surf.app, "/tmp/glue_app.raw");
        }
        g_backend->frame_begin();
        vctrl_publish();   /* tell the scanout reader a new frame is ready */
        pace_frame();
    }
    if (g_log_stats && (g_frames % 120) == 0) {
        const gs::Stats &s = gs::stats();
        logf_("misterglue: %u frames | draws %u decoded %u fallback %u "
              "(shader %u prog %u mode %u attrib %u tex %u blend %u) "
              "tex[unbound %u missing %u invalid %u never-uploaded %u last id=%u %ux%u] "
              "rej[up %u sub %u fmt=0x%x type=0x%x copytex %u] surfdraws %u",
              g_frames, s.draws_seen, s.draws_decoded, s.draws_fallback,
              s.fallback_custom_shader, s.fallback_program, s.fallback_mode,
              s.fallback_attrib, s.fallback_texture, s.fallback_blend,
              s.tex_unbound, s.tex_missing, s.tex_invalid, s.tex_never_uploaded,
              s.last_bad_tex, s.last_bad_w, s.last_bad_h,
              s.uploads_rejected, s.subuploads_rejected, s.last_rej_fmt,
              s.last_rej_type, s.copytex_calls, s.src_surface_draws);
        gs::stats_reset();
    }
    return g_fabric ? 1 : 0;
}

const MisterGlue kGlue = {
    MISTER_GLUE_ABI_VERSION,
    glue_init,
    glue_shutdown,
    glue_resolve,
    glue_present,
};

} /* anonymous namespace */

extern "C" const MisterGlue *MisterGlue_Get(void) { return &kGlue; }
