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
#include "draw_rewrite.h"
#include "safe_area.h"

#include <math.h>
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
    void (*DeleteProgram)(unsigned);
    void (*DeleteShader)(unsigned);
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
    void (*DeleteFramebuffers)(int, const unsigned *);
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
/* draw_rewrite.h, as a bitmask for A/B runs: MISTER_GLUE_REWRITE 1 fill,
   2 unrotate, 4 direct present (default 7, all; 0 none). */
enum { RW_FILL = 1, RW_UNROTATE = 2, RW_DIRECT = 4 };
int  g_rewrite     = RW_FILL | RW_UNROTATE | RW_DIRECT;
unsigned g_rw_fills = 0, g_rw_unrotated = 0;   /* per stats window */
constexpr int kMinFillPx = 64 * 64;   /* solid rects below this stay triangles */

/* Direct present (draw_rewrite.h identity_present). While the last frame that
   presented copied the app surface 1:1 (g_present_1to1), the next frame draws
   the scene straight into the screen: app-surface draws and clears target the
   default framebuffer, and the copy is dropped. Until that copy arrives, the
   screen's own draws and clears are dropped too -- the opaque copy would have
   covered them. Any other use of the surface ends it from the next frame.
   A safe area that only moves the picture by whole pixels (Picture Up,
   v_shift) keeps it: the scene is drawn at its place, its clear fills only the
   picture, and where the copy was the border is filled black, as the copy
   would have left it. A scaled safe area keeps the copy. */
bool     g_present_1to1 = false;
bool     g_direct       = false;   /* this frame */
bool     g_presented    = false;   /* this frame's surface copy has been seen */
unsigned g_frame_presents = 0;     /* surface copies this frame */
bool     g_frame_1to1   = true;    /* ...all of them 1:1, and nothing else touched the surface */
unsigned g_pd_frames = 0, g_pd_dropped = 0, g_pd_mismatch = 0;   /* per stats window */
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

/* Safe area (safe_area.h): the rect the finished picture is scaled into, so it
   fits the lines the core scans out and the display shows. Applied to draws
   into the default framebuffer only -- on Godot that is the one composite of
   the viewport render target, so the game itself still renders 320x240.
   The border is the default framebuffer's clear; when the engine has not
   cleared it this frame we clear it to black ourselves before the first
   default-framebuffer draw (the backend emits that fill, since the shrunk
   composite no longer proves it redundant). */
bool  g_safe = false;
float g_sa_sx = 1.0f, g_sa_sy = 1.0f, g_sa_ox = 0.0f, g_sa_oy = 0.0f;
bool  g_def_cleared = false;   /* the default framebuffer was cleared this frame */

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

/* Read key= from the display.cfg file, if it exists and has one. */
bool read_cfg(const char *path, const char *key, char *val, size_t val_sz) {
    if (!path || !*path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char text[4096];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return safe_area::cfg_value(text, key, val, val_sz);
}

/* Safe-area configuration, resolved once at init; the rect itself is recomputed
   whenever the core's Vertical Lines mode changes (safe_area_poll). */
struct SafeAreaCfg {
    int  fb_w = 0, fb_h = 0;
    safe_area::Rect window{0, 0, 0, 0};   /* MISTER_GLUE_VISIBLE: the core's short window */
    safe_area::Ratio dar{0, 0};           /* MISTER_GLUE_DAR: how the core shows the window */
    char spec[64] = "off";
    const char *src = "default";
    int  v_shift = 0;                     /* display.cfg v_shift= / MISTER_GLUE_V_SHIFT */
    int  osd_up = 0;                      /* OSD Picture Up rows (0: v_shift applies) */
    int  osd_hud = -1;                    /* OSD HUD Down rows, as last published */
    int  full = -1;                       /* the mode the rect was last resolved for */
} g_sa;

/* The core's OSD Vertical Lines mode: C_STATUS (0x3B000030) low32 bit2, written by
   the fabric every frame (blitter_top S_WR_STATUS). Read-only mapping of the
   control block's page; cores that predate the option leave the bit 0, which is
   the short window -- the right answer for them. MISTER_GLUE_OSD_LINES=1 enables. */
const uint32_t kBltCtrlBase = 0x3B000000u, kCStatusOff = 0x30u, kCStatusVFull = 1u << 2;
/* Bits 7:3 of the same word carry the OSD HUD Down / Picture Up steps
   (safe_area::osd_hud_down / osd_picture_up). */
volatile const uint8_t *g_bltctrl = nullptr;
int g_bltctrl_fd = -1;

bool bltctrl_init(void) {
    const char *dev = getenv("MISTER_GLUE_MEMDEV");   /* test hook: a sparse file stands in */
    g_bltctrl_fd = open(dev && *dev ? dev : "/dev/mem", O_RDONLY | O_SYNC);
    if (g_bltctrl_fd < 0) return false;
    void *m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, g_bltctrl_fd, kBltCtrlBase);
    if (m == MAP_FAILED) { close(g_bltctrl_fd); g_bltctrl_fd = -1; return false; }
    g_bltctrl = (volatile const uint8_t *)m;
    return true;
}

void bltctrl_shutdown(void) {
    if (g_bltctrl) { munmap((void *)g_bltctrl, 0x1000); g_bltctrl = nullptr; }
    if (g_bltctrl_fd >= 0) { close(g_bltctrl_fd); g_bltctrl_fd = -1; }
}

/* Resolve the safe area against the window the core scans out in this mode: the
   whole frame with Vertical Lines = 240, MISTER_GLUE_VISIBLE otherwise. A bad
   spec is logged and leaves the picture unscaled. */
void safe_area_set_mode(bool full) {
    const int fb_w = g_sa.fb_w, fb_h = g_sa.fb_h;
    const safe_area::Rect vis = full ? safe_area::Rect{0, 0, fb_w, fb_h} : g_sa.window;
    safe_area::Rect r;
    const char *err = nullptr;
    if (!safe_area::resolve(g_sa.spec, fb_w, fb_h, vis, g_sa.dar, &r, &err)) {
        logf_("misterglue: safe area \"%s\" (%s) ignored: %s", g_sa.spec, g_sa.src, err);
        r = safe_area::Rect{0, 0, fb_w, fb_h};
    }
    const int shift = g_sa.osd_up ? g_sa.osd_up : g_sa.v_shift;   /* a non-zero OSD step wins */
    g_safe  = !(r.x == 0 && r.y == 0 && r.w == fb_w && r.h == fb_h) || shift != 0;
    g_sa_sx = (float)r.w / (float)fb_w;
    g_sa_sy = (float)r.h / (float)fb_h;
    g_sa_ox = (float)r.x;
    g_sa_oy = (float)(r.y - shift);
    g_sa.full = full ? 1 : 0;
    logf_("misterglue: safe area %dx%d+%d+%d v_shift %d%s (\"%s\" from %s; visible %dx%d+%d+%d at %d:%d%s)",
          r.w, r.h, r.x, r.y, shift, g_sa.osd_up ? " (OSD Picture Up)" : "", g_sa.spec, g_sa.src, vis.w, vis.h, vis.x, vis.y,
          g_sa.dar.num, g_sa.dar.den,
          g_bltctrl ? (full ? ", core 240 lines" : ", core short window") : "");
}

/* Once per frame, between frames: follow the OSD Vertical Lines option live. */
void safe_area_poll(void) {
    if (!g_bltctrl) return;
    const uint32_t st = *(volatile const uint32_t *)(g_bltctrl + kCStatusOff);
    const int full = (st & kCStatusVFull) ? 1 : 0;
    const int up = safe_area::osd_picture_up(st);
    if (full != g_sa.full || up != g_sa.osd_up) {
        g_sa.osd_up = up;
        safe_area_set_mode(full);
    }
    /* HUD Down is applied by the port's GDScript (src/godot/mister_layout.gd),
       which runs on this same main thread and reads it with OS.get_environment. */
    const int hud = safe_area::osd_hud_down(st);
    if (hud != g_sa.osd_hud) {
        char v[8];
        snprintf(v, sizeof v, "%d", hud);
        setenv("MISTER_HUD_OFFSET_OSD", v, 1);
        g_sa.osd_hud = hud;
        logf_("misterglue: OSD HUD Down %d", hud);
    }
}

/* MISTER_GLUE_VISIBLE: the window the core scans out (default: the whole frame).
   MISTER_GLUE_DAR: the display aspect the core shows that window at, in either
   Vertical Lines mode (the RBF's video_freak ARX:ARY; default: fb_w:fb_h).
   Safe area: display.cfg's safe_area= (MISTER_GLUE_SAFE_AREA_FILE) wins over
   MISTER_GLUE_SAFE_AREA, so the user's file overrides the port's default. */
void safe_area_init(int fb_w, int fb_h) {
    g_sa.fb_w = fb_w;
    g_sa.fb_h = fb_h;
    g_sa.window = safe_area::Rect{0, 0, fb_w, fb_h};
    const char *ve = getenv("MISTER_GLUE_VISIBLE");
    bool has_pos = false;
    if (ve && *ve) {
        safe_area::Rect v;
        if (safe_area::parse_geometry(ve, &v, &has_pos) && has_pos && safe_area::rect_in(v, fb_w, fb_h))
            g_sa.window = v;
        else
            logf_("misterglue: MISTER_GLUE_VISIBLE=\"%s\" ignored (want WxH+X+Y inside %dx%d)", ve, fb_w, fb_h);
    }
    g_sa.dar = safe_area::Ratio{fb_w, fb_h};
    const char *de = getenv("MISTER_GLUE_DAR");
    if (de && *de && !safe_area::parse_ratio(de, &g_sa.dar)) {
        g_sa.dar = safe_area::Ratio{fb_w, fb_h};
        logf_("misterglue: MISTER_GLUE_DAR=\"%s\" ignored (want W:H)", de);
    }

    char cfgval[64];
    const char *cfg = getenv("MISTER_GLUE_SAFE_AREA_FILE");
    const char *spec = getenv("MISTER_GLUE_SAFE_AREA");
    g_sa.src = "MISTER_GLUE_SAFE_AREA";
    if (read_cfg(cfg, "safe_area", cfgval, sizeof cfgval)) { spec = cfgval; g_sa.src = cfg; }
    if (!spec) { spec = "off"; g_sa.src = "default"; }
    snprintf(g_sa.spec, sizeof g_sa.spec, "%s", spec);

    /* v_shift: display.cfg wins over MISTER_GLUE_V_SHIFT, like safe_area. */
    char shval[64];
    const char *sh = getenv("MISTER_GLUE_V_SHIFT");
    if (read_cfg(cfg, "v_shift", shval, sizeof shval)) sh = shval;
    g_sa.v_shift = 0;
    if (sh && *sh && !safe_area::parse_shift(sh, &g_sa.v_shift)) {
        g_sa.v_shift = 0;
        logf_("misterglue: v_shift \"%s\" ignored (want -%d..%d)", sh, (int)safe_area::kMaxShift,
              (int)safe_area::kMaxShift);
    }

    const char *ol = getenv("MISTER_GLUE_OSD_LINES");
    if (ol && *ol == '1' && !bltctrl_init())
        logf_("misterglue: core Vertical Lines unreadable (need /dev/mem); using MISTER_GLUE_VISIBLE");
    g_sa.full = -1;
    if (g_bltctrl) safe_area_poll();
    else safe_area_set_mode(false);
}

/* Map a default-framebuffer draw from frame coordinates into the safe area. */
void safe_area_apply(gs::DecodedDraw &d) {
    for (int i = 0; i < d.tris * 3; i++) {
        d.verts[i].x = g_sa_ox + d.verts[i].x * g_sa_sx;
        d.verts[i].y = g_sa_oy + d.verts[i].y * g_sa_sy;
    }
}

/* The safe area moves the picture by whole pixels at most, so direct present
   can draw the scene where the copy would have put it. */
bool safe_area_is_move() {
    return !g_safe || (g_sa_sx == 1.0f && g_sa_sy == 1.0f &&
                       g_sa_ox == floorf(g_sa_ox) && g_sa_oy == floorf(g_sa_oy));
}

/* Direct present with a safe area: fill the picture rect (the scene's clear)
   or the black border around it (where the copy was). */
void direct_fill_picture(uint8_t r, uint8_t g_, uint8_t b) {
    RSurface def{};
    def.w = host.scanout_w;
    def.h = host.scanout_h;
    RasterBackend_MFGPU_FillRect(&def, (int)g_sa_ox, (int)g_sa_oy, host.scanout_w, host.scanout_h,
                                 r, g_, b);
}

void direct_fill_border() {
    RSurface def{};
    def.w = host.scanout_w;
    def.h = host.scanout_h;
    const int W = host.scanout_w, H = host.scanout_h;
    const int x0 = (int)g_sa_ox, y0 = (int)g_sa_oy, x1 = x0 + W, y1 = y0 + H;
    const int cy0 = y0 < 0 ? 0 : y0, cy1 = y1 > H ? H : y1;
    if (cy0 > 0) RasterBackend_MFGPU_FillRect(&def, 0, 0, W, cy0, 0, 0, 0);
    if (cy1 < H) RasterBackend_MFGPU_FillRect(&def, 0, cy1, W, H - cy1, 0, 0, 0);
    if (cy1 > cy0) {
        if (x0 > 0) RasterBackend_MFGPU_FillRect(&def, 0, cy0, x0, cy1 - cy0, 0, 0, 0);
        if (x1 < W) RasterBackend_MFGPU_FillRect(&def, x1, cy0, W - x1, cy1 - cy0, 0, 0, 0);
    }
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
void w_DeleteProgram(unsigned p) { gs::on_delete_program(p); gl.DeleteProgram(p); }
void w_DeleteShader(unsigned s) { gs::on_delete_shader(s); gl.DeleteShader(s); }

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

/* Drop the fabric's staged copy of a GL texture and of its pre-whitened twin
   (Player.shader's flash), which is derived from the same pixels. */
void invalidate_tex(uint32_t id) {
    RasterBackend_MFGPU_InvalidateTex(id);
    RasterBackend_MFGPU_InvalidateTex(id | gs::kWhiteTexKeyBit);
}

void w_TexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int b,
                  unsigned fmt, unsigned type, const void *px) {
    gs::on_tex_image2d(tgt, lvl, ifmt, w, h, b, fmt, type, px);
    if (g_fabric) invalidate_tex(gs::bound_texture());
    gl.TexImage2D(tgt, lvl, ifmt, w, h, b, fmt, type, px);
}
void w_TexSubImage2D(unsigned tgt, int lvl, int x, int y, int w, int h,
                     unsigned fmt, unsigned type, const void *px) {
    gs::on_tex_subimage2d(tgt, lvl, x, y, w, h, fmt, type, px);
    if (g_fabric) invalidate_tex(gs::bound_texture());
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
        for (int i = 0; i < n; i++) invalidate_tex(ids[i]);
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

void w_DeleteFramebuffers(int n, const unsigned *ids) {
    gs::on_delete_framebuffers(n, ids);
    gl.DeleteFramebuffers(n, ids);
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
        if (g_direct && is_def && !g_presented) {
            g_pd_dropped++;               /* under the copy we are not drawing */
            is_def = false;
        } else if (g_direct && is_app) {
            is_app = false;
            if (g_presented) {            /* for a copy that already happened */
                g_pd_mismatch++;
                g_frame_1to1 = false;
            } else if (g_safe) {
                uint8_t r, gg, b, a;
                gs::get_clear_color(&r, &gg, &b, &a);
                direct_fill_picture(r, gg, b);
            } else {
                is_def = true;            /* the scene's clear, onto the screen */
            }
        }
        if (is_def || is_app) {
            if (is_def) g_def_cleared = true;
            uint8_t r, gg, b, a;
            gs::get_clear_color(&r, &gg, &b, &a);
            /* With a safe area the screen clear is the visible border: keep it
               black whatever the engine's clear colour (Godot defaults to grey). */
            if (is_def && g_safe) r = gg = b = 0;
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

void dispatch_decoded(const gs::DecodedDraw &decoded) {
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

    gs::DecodedDraw d = decoded;
    if (d.src_is_surface) {
        g_frame_presents++;
        if (!d.to_default_fb || !dr::identity_present(d, host.scanout_w, host.scanout_h))
            g_frame_1to1 = false;
    }
    if (g_direct) {
        if (d.src_is_surface) {
            /* The scene is on the screen already; the surface holds an old frame,
               so drawing from it would only be worse than leaving it out. */
            if (g_presented || !g_frame_1to1) g_pd_mismatch++;
            else if (g_safe) direct_fill_border();
            g_presented = true;
            return;
        }
        if (!d.to_default_fb) {
            if (g_presented) {            /* for a copy that already happened */
                g_pd_mismatch++;
                g_frame_1to1 = false;
                return;
            }
            d.to_default_fb = true;
            d.dst_fbo = 0;
        } else if (!g_presented) {
            g_pd_dropped++;
            return;
        }
    }
    /* Safe area first: the rewrites below then see the remapped vertices, so a
       full-screen solid quad no longer covers the target and is not turned into
       a FILL over the black border. */
    if (g_safe && d.to_default_fb) {
        if (!g_def_cleared && !g_direct) {   /* direct: the border is filled at the copy */
            RSurface def{};
            def.w = host.scanout_w;
            def.h = host.scanout_h;
            g_backend->clear(g_sw_surfaces ? &g_surf.def : &def, 0, 0, 0, 255);
            g_def_cleared = true;
        }
        safe_area_apply(d);
    }

    /* Same pixels, fewer fabric cycles (draw_rewrite.h): a full-target solid
       quad becomes a FILL, a large rotated sprite samples an upright copy. */
    {
        uint8_t c[4];
        int r[4];
        if ((g_rewrite & RW_FILL) && dr::solid_rect(d, host.scanout_w, host.scanout_h, c, r)) {
            RSurface dst{};
            dst.w = host.scanout_w;
            dst.h = host.scanout_h;
            dst.fbo = d.dst_fbo;
            RSurface *s = g_sw_surfaces ? ((d.dst_fbo == 0) ? &g_surf.def : &g_surf.app) : &dst;
            if (r[2] == host.scanout_w && r[3] == host.scanout_h) {
                g_backend->clear(s, c[0], c[1], c[2], c[3]);
                g_rw_fills++;
                return;
            }
            /* Smaller rects stay triangles: a FILL ends the backend's batch. */
            if (!g_sw_surfaces && r[2] * r[3] >= kMinFillPx) {
                RasterBackend_MFGPU_FillRect(s, r[0], r[1], r[2], r[3], c[0], c[1], c[2]);
                g_rw_fills++;
                return;
            }
        }
        if ((g_rewrite & RW_UNROTATE) &&
            dr::unrotate(d, host.scanout_w, host.scanout_h, RasterBackend_MFGPU_InvalidateTex))
            g_rw_unrotated++;
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
    { "glDeleteProgram", (void *)w_DeleteProgram },
    { "glDeleteShader", (void *)w_DeleteShader },
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
    { "glDeleteFramebuffers", (void *)w_DeleteFramebuffers },
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
    REAL(DeleteProgram, "glDeleteProgram");
    REAL(DeleteShader, "glDeleteShader");
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
    REAL(DeleteFramebuffers, "glDeleteFramebuffers");
    REAL(Viewport, "glViewport");
    REAL(Scissor, "glScissor");
    REAL(ClearColor, "glClearColor");
    REAL(Clear, "glClear");
    REAL(DrawArrays, "glDrawArrays");
    REAL(DrawElements, "glDrawElements");

    gs::init(host.scanout_w, host.scanout_h);

    const char *fab = getenv("MISTER_GLUE_FABRIC");
    g_fabric = fab && *fab == '1';
    const char *rw = getenv("MISTER_GLUE_REWRITE");
    if (rw && *rw) g_rewrite = atoi(rw) & (RW_FILL | RW_UNROTATE | RW_DIRECT);
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
        safe_area_init(host.scanout_w, host.scanout_h);
    } else {
        logf_("misterglue: shadow-only (set MISTER_GLUE_FABRIC=1 to drive the fabric)");
    }

    g_inited = true;
    return 0;
}

void glue_shutdown() {
    if (!g_inited) return;
    vctrl_shutdown();
    bltctrl_shutdown();
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
        dr::frame_end();
        g_def_cleared = false;
        safe_area_poll();     /* between frames: the next frame uses the new rect whole */
        if (g_frame_presents)
            g_present_1to1 = g_frame_presents == 1 && g_frame_1to1;
        else if (!g_frame_1to1)
            g_present_1to1 = false;
        g_direct = (g_rewrite & RW_DIRECT) && g_present_1to1 && safe_area_is_move() && !g_sw_surfaces;
        if (g_direct) g_pd_frames++;
        g_presented = false;
        g_frame_presents = 0;
        g_frame_1to1 = true;
        vctrl_publish();   /* tell the scanout reader a new frame is ready */
        pace_frame();
    }
    if (g_log_stats && (g_frames % 120) == 0) {
        const gs::Stats &s = gs::stats();
        const gs::TableUse u = gs::table_use();
        logf_("misterglue: %u frames | draws %u decoded %u fallback %u "
              "(shader %u prog %u mode %u attrib %u tex %u blend %u) "
              "tex[unbound %u missing %u invalid %u never-uploaded %u last id=%u %ux%u] "
              "rej[up %u sub %u fmt=0x%x type=0x%x copytex %u] surfdraws %u "
              "slots[tex %d buf %d prog %d shader %d fbo %d] rewrite[fill %u unrotate %u] "
              "direct[frames %u dropped %u mismatch %u]",
              g_frames, s.draws_seen, s.draws_decoded, s.draws_fallback,
              s.fallback_custom_shader, s.fallback_program, s.fallback_mode,
              s.fallback_attrib, s.fallback_texture, s.fallback_blend,
              s.tex_unbound, s.tex_missing, s.tex_invalid, s.tex_never_uploaded,
              s.last_bad_tex, s.last_bad_w, s.last_bad_h,
              s.uploads_rejected, s.subuploads_rejected, s.last_rej_fmt,
              s.last_rej_type, s.copytex_calls, s.src_surface_draws,
              u.textures, u.buffers, u.programs, u.shaders, u.fbos, g_rw_fills, g_rw_unrotated,
              g_pd_frames, g_pd_dropped, g_pd_mismatch);
        g_rw_fills = g_rw_unrotated = 0;
        g_pd_frames = g_pd_dropped = g_pd_mismatch = 0;
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
