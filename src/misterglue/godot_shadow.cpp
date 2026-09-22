/*
  GLES2 state shadow + Godot canvas draw decode. See godot_shadow.h.

  The transform chain below is transcribed from the vertex shader Godot 3.5
  generates for canvas_item (drivers/gles2/shaders/canvas.glsl), captured off the
  device from this game's own shader compiles. Keeping it in lockstep with that
  source is the whole correctness argument for this file.
*/
#include "godot_shadow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gs {
namespace {

/* ---- GL enums we need, without pulling in a GL header --------------------- */
enum {
    GL_POINTS = 0x0000, GL_LINES = 0x0001, GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005, GL_TRIANGLE_FAN = 0x0006,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_FLOAT = 0x1406,
    GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909,
    GL_LUMINANCE_ALPHA = 0x190A,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11,
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0
};

/* Godot's canvas attribute slots (canvas.glsl "// attrib:N" comments). */
enum { ATTR_VERTEX = 0, ATTR_LIGHT_ANGLE = 2, ATTR_COLOR = 3, ATTR_UV = 4,
       ATTR_MODULATE = 5, ATTR_TRANSLATE = 6, ATTR_BASIS = 7, MAX_ATTRIBS = 16 };

/* Uniform semantics we track. */
enum UniformSem {
    U_NONE = 0, U_PROJECTION, U_MODELVIEW, U_EXTRA, U_FINAL_MODULATE,
    U_DST_RECT, U_SRC_RECT, U_COUNT
};

struct Texture {
    uint32_t id = 0;
    uint8_t *rgba = nullptr;
    int w = 0, h = 0;
    bool nearest = true;
    bool valid = false;
    bool opaque = false;
    uint32_t generation = 0;   /* bumped on re-upload: invalidates the fabric cache */
    uint8_t *white_rgba = nullptr;  /* Player.shader flash copy, built on first use */
    uint32_t white_gen = 0;         /* `generation` white_rgba was built from */
    bool     white_built = false;
};

struct Fbo {
    uint32_t id = 0;
    uint32_t color_tex = 0;
};

struct Buffer {
    uint32_t id = 0;
    uint8_t *data = nullptr;
    size_t size = 0;
};

struct AttribState {
    bool enabled = false;
    int size = 4;
    uint32_t type = GL_FLOAT;
    bool normalized = false;
    int stride = 0;
    const void *ptr = nullptr;
    uint32_t buffer = 0;
    float constant[4] = { 0, 0, 0, 1 };  /* glVertexAttrib4f value when disabled */
};

struct UniformSlot { int loc = -1; };

struct Program {
    uint32_t id = 0;
    ProgramInfo info;
    int loc[U_COUNT] = { -1, -1, -1, -1, -1, -1, -1 };
    float projection[16] = {};
    float modelview[16]  = {};
    float extra[16]      = {};
    float final_modulate[4] = { 1, 1, 1, 1 };
    float dst_rect[4] = {};
    float src_rect[4] = {};
    int  loc_white = -1;       /* Player.shader's `uniform bool white` (m_white) */
    int  white = 0;
};

/* ---- fixed-capacity tables (Godot's working set here is tiny) ------------- */
template <typename T, int N>
struct Table {
    T slots[N];
    int used = 0;
    T *find(uint32_t id) {
        for (int i = 0; i < used; i++)
            if (slots[i].id == id) return &slots[i];
        return nullptr;
    }
    T *get_or_add(uint32_t id) {
        if (T *t = find(id)) return t;
        if (used >= N) return nullptr;
        T *t = &slots[used++];
        *t = T();
        t->id = id;
        return t;
    }
};

struct State {
    Table<Texture, 512> textures;
    Table<Buffer, 256>  buffers;
    Table<Program, 64>  programs;
    Table<Fbo, 64>      fbos;

    uint32_t appsurf_fbo = 0;   /* Godot's viewport render target, once detected */
    uint32_t appsurf_tex = 0;

    uint32_t bound_tex[8] = {};
    uint32_t active_unit = 0;
    uint32_t array_buffer = 0, element_buffer = 0;
    AttribState attribs[MAX_ATTRIBS];
    uint32_t cur_program = 0;
    uint32_t bound_fbo = 0;

    bool blend_enabled = false;
    uint32_t blend_src = GL_ONE, blend_dst = GL_ZERO;

    int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    float clear_color[4] = { 0, 0, 0, 1 };

    int scanout_w = 320, scanout_h = 240;

    /* shader id -> parsed defines, folded into the program at attach time */
    struct ShaderInfo { uint32_t id; ProgramInfo info; };
    Table<ShaderInfo, 128> shaders;

    BVtx *vbuf = nullptr;
    int vbuf_cap = 0;

    Stats st{};
};

State g;

/* ---- helpers ------------------------------------------------------------- */
void mat4_mul_vec4(const float *m, const float *v, float *out) {
    /* column-major, as GL uploads it */
    for (int i = 0; i < 4; i++)
        out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

bool has_define(const char *src, const char *name) {
    const char *p = src;
    size_t n = strlen(name);
    while ((p = strstr(p, "#define ")) != nullptr) {
        p += 8;
        if (strncmp(p, name, n) == 0 && (p[n] == '\n' || p[n] == ' ' || p[n] == '\r'))
            return true;
    }
    return false;
}

BVtx *vbuf_reserve(int nverts) {
    if (nverts <= g.vbuf_cap) return g.vbuf;
    int cap = nverts < 1024 ? 1024 : nverts;
    BVtx *nb = (BVtx *)realloc(g.vbuf, (size_t)cap * sizeof(BVtx));
    if (!nb) return nullptr;
    g.vbuf = nb;
    g.vbuf_cap = cap;
    return g.vbuf;
}

const uint8_t *attrib_base(const AttribState &a, size_t *out_avail) {
    if (a.buffer) {
        Buffer *b = g.buffers.find(a.buffer);
        if (!b || !b->data) return nullptr;
        size_t off = (size_t)(uintptr_t)a.ptr;
        if (off > b->size) return nullptr;
        *out_avail = b->size - off;
        return b->data + off;
    }
    /* client array: Godot's canvas always uses VBOs, but honour it anyway */
    if (!a.ptr) return nullptr;
    *out_avail = (size_t)-1;
    return (const uint8_t *)a.ptr;
}

bool read_attrib(const AttribState &a, int index, float *out4) {
    out4[0] = a.constant[0]; out4[1] = a.constant[1];
    out4[2] = a.constant[2]; out4[3] = a.constant[3];
    if (!a.enabled) return true;

    size_t avail = 0;
    const uint8_t *base = attrib_base(a, &avail);
    if (!base) return false;

    int elem = a.size;
    int esz = (a.type == GL_FLOAT) ? 4 : (a.type == GL_UNSIGNED_BYTE ? 1 : 2);
    int stride = a.stride ? a.stride : elem * esz;
    size_t off = (size_t)index * (size_t)stride;
    if (avail != (size_t)-1 && off + (size_t)elem * esz > avail) return false;

    const uint8_t *p = base + off;
    for (int i = 0; i < 4; i++) {
        if (i >= elem) { out4[i] = (i == 3) ? 1.0f : 0.0f; continue; }
        switch (a.type) {
            case GL_FLOAT: {
                float f; memcpy(&f, p + i * 4, 4); out4[i] = f; break;
            }
            case GL_UNSIGNED_BYTE:
                out4[i] = a.normalized ? p[i] / 255.0f : (float)p[i];
                break;
            case GL_UNSIGNED_SHORT: {
                uint16_t s; memcpy(&s, p + i * 2, 2);
                out4[i] = a.normalized ? s / 65535.0f : (float)s;
                break;
            }
            default: return false;
        }
    }
    return true;
}

/* Convert an uploaded texture to the RGBA8888 the seam expects. */
bool store_texture(Texture *t, int w, int h, uint32_t fmt, uint32_t type, const void *px) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    uint8_t *dst = (uint8_t *)realloc(t->rgba, (size_t)w * h * 4);
    if (!dst) return false;
    t->rgba = dst;
    t->w = w; t->h = h;
    t->generation++;

    if (!px) {                       /* allocation only (FBO target): no pixels yet */
        memset(dst, 0, (size_t)w * h * 4);
        t->valid = false;
        t->opaque = false;
        return true;
    }

    const uint8_t *s8 = (const uint8_t *)px;
    const size_t n = (size_t)w * h;
    bool opaque = true;

    if (type == GL_UNSIGNED_BYTE && fmt == GL_RGBA) {
        memcpy(dst, s8, n * 4);
        for (size_t i = 0; i < n; i++) if (dst[i * 4 + 3] != 255) { opaque = false; break; }
    } else if (type == GL_UNSIGNED_BYTE && fmt == GL_RGB) {
        for (size_t i = 0; i < n; i++) {
            dst[i * 4 + 0] = s8[i * 3 + 0]; dst[i * 4 + 1] = s8[i * 3 + 1];
            dst[i * 4 + 2] = s8[i * 3 + 2]; dst[i * 4 + 3] = 255;
        }
    } else if (type == GL_UNSIGNED_BYTE && (fmt == GL_LUMINANCE || fmt == GL_ALPHA)) {
        for (size_t i = 0; i < n; i++) {
            uint8_t v = s8[i];
            if (fmt == GL_ALPHA) {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
                dst[i * 4 + 3] = v;
                if (v != 255) opaque = false;
            } else {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = v;
                dst[i * 4 + 3] = 255;
            }
        }
    } else if (type == GL_UNSIGNED_BYTE && fmt == GL_LUMINANCE_ALPHA) {
        for (size_t i = 0; i < n; i++) {
            dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = s8[i * 2];
            dst[i * 4 + 3] = s8[i * 2 + 1];
            if (s8[i * 2 + 1] != 255) opaque = false;
        }
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4 && fmt == GL_RGBA) {
        const uint16_t *s16 = (const uint16_t *)px;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = s16[i];
            uint8_t r = (v >> 12) & 0xF, gg = (v >> 8) & 0xF, b = (v >> 4) & 0xF, a = v & 0xF;
            dst[i * 4 + 0] = (uint8_t)(r * 17); dst[i * 4 + 1] = (uint8_t)(gg * 17);
            dst[i * 4 + 2] = (uint8_t)(b * 17); dst[i * 4 + 3] = (uint8_t)(a * 17);
            if (a != 0xF) opaque = false;
        }
    } else if (type == GL_UNSIGNED_SHORT_5_6_5 && fmt == GL_RGB) {
        const uint16_t *s16 = (const uint16_t *)px;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = s16[i];
            dst[i * 4 + 0] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
            dst[i * 4 + 1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
            dst[i * 4 + 2] = (uint8_t)((v & 0x1F) * 255 / 31);
            dst[i * 4 + 3] = 255;
        }
    } else {
        t->valid = false;
        g.st.uploads_rejected++;
        g.st.last_rej_fmt = fmt;
        g.st.last_rej_type = type;
        return false;    /* unknown upload format: draws using it fall back */
    }

    t->valid = true;
    t->opaque = opaque;
    return true;
}

Program *cur_prog() { return g.cur_program ? g.programs.find(g.cur_program) : nullptr; }

/* The FBO this texture is the colour attachment of, or 0. */
uint32_t fbo_owning_texture(uint32_t tex) {
    if (!tex) return 0;
    for (int i = 0; i < g.fbos.used; i++)
        if (g.fbos.slots[i].color_tex == tex) return g.fbos.slots[i].id;
    return 0;
}

int sem_of_loc(const Program *p, int loc) {
    if (loc < 0) return U_NONE;
    for (int i = 1; i < U_COUNT; i++)
        if (p->loc[i] == loc) return i;
    return U_NONE;
}

RBlend decode_blend(bool *ok) {
    *ok = true;
    if (!g.blend_enabled) return RB_NONE;
    if (g.blend_src == GL_SRC_ALPHA && g.blend_dst == GL_ONE_MINUS_SRC_ALPHA) return RB_ALPHA;
    if (g.blend_src == GL_ONE && g.blend_dst == GL_ONE_MINUS_SRC_ALPHA) return RB_PREMULT;
    if (g.blend_src == GL_ONE && g.blend_dst == GL_ONE) return RB_ADD;
    if (g.blend_src == GL_SRC_ALPHA && g.blend_dst == GL_ONE) return RB_ADD;
    *ok = false;
    return RB_NONE;
}

} /* anonymous namespace */

/* ---- lifecycle ----------------------------------------------------------- */
void init(int scanout_w, int scanout_h) {
    g.scanout_w = scanout_w;
    g.scanout_h = scanout_h;
    g.vp_w = scanout_w;
    g.vp_h = scanout_h;
}

void shutdown() {
    for (int i = 0; i < g.textures.used; i++) free(g.textures.slots[i].rgba);
    for (int i = 0; i < g.buffers.used; i++) free(g.buffers.slots[i].data);
    free(g.vbuf);
    g = State();
}

/* ---- state shadow -------------------------------------------------------- */
void on_shader_source(uint32_t shader, const char *src) {
    State::ShaderInfo *si = g.shaders.get_or_add(shader);
    if (!si || !src) return;
    ProgramInfo pi;
    /* The canvas shader is identified by its own uniforms, not by a define. */
    pi.is_canvas       = strstr(src, "extra_matrix") != nullptr ||
                         strstr(src, "final_modulate") != nullptr;
    pi.texture_rect    = has_define(src, "USE_TEXTURE_RECT");
    pi.attrib_large    = has_define(src, "USE_ATTRIB_LARGE_VERTEX");
    pi.attrib_modulate = has_define(src, "USE_ATTRIB_MODULATE");
    pi.skip_transform  = has_define(src, "SKIP_TRANSFORM_USED");
    pi.pixel_snap      = has_define(src, "USE_PIXEL_SNAP");
    pi.instancing      = has_define(src, "USE_INSTANCING");
    pi.skeleton        = has_define(src, "USE_SKELETON");
    pi.lighting        = has_define(src, "USE_LIGHTING");
    /* Game-supplied shader code: the fabric is fixed-function, so those draws
       must stay on Mesa. Godot's shader compiler emits every *user* uniform with
       an "m_" prefix, and emits none for its own stock shaders — verified against
       every shader this game compiles — so that prefix is the discriminator.
       (Testing for SCREEN_TEXTURE instead is wrong: the stock canvas fragment
       shader declares that sampler whether or not the material samples it.) */
    pi.custom_fragment = strstr(src, "uniform bool m_") != nullptr ||
                         strstr(src, "uniform float m_") != nullptr ||
                         strstr(src, "uniform int m_") != nullptr ||
                         strstr(src, "uniform vec2 m_") != nullptr ||
                         strstr(src, "uniform vec3 m_") != nullptr ||
                         strstr(src, "uniform vec4 m_") != nullptr ||
                         strstr(src, "uniform sampler2D m_") != nullptr ||
                         /* this game's shaders by name, in case the prefix rule
                            ever misses a form we have not seen */
                         strstr(src, "aperture_grille") != nullptr ||
                         strstr(src, "white_texture_rgb") != nullptr;
    /* Player.shader is the one game shader the fabric can reproduce: its
       fragment is the texel (vertex colour ignored), optionally mixed 75% toward
       white. Only the fragment half carries the body, so this is ORed in at
       attach time and overrides custom_fragment, which the m_white uniform in
       both halves sets. */
    pi.white_flash     = strstr(src, "white_texture_rgb") != nullptr;
    si->info = pi;
}

void on_attach_shader(uint32_t program, uint32_t shader) {
    Program *p = g.programs.get_or_add(program);
    State::ShaderInfo *si = g.shaders.find(shader);
    if (!p || !si) return;
    ProgramInfo &d = p->info;
    const ProgramInfo &s = si->info;
    d.is_canvas       |= s.is_canvas;
    d.texture_rect    |= s.texture_rect;
    d.attrib_large    |= s.attrib_large;
    d.attrib_modulate |= s.attrib_modulate;
    d.skip_transform  |= s.skip_transform;
    d.pixel_snap      |= s.pixel_snap;
    d.instancing      |= s.instancing;
    d.skeleton        |= s.skeleton;
    d.lighting        |= s.lighting;
    d.custom_fragment |= s.custom_fragment;
    d.white_flash     |= s.white_flash;
}

void on_use_program(uint32_t program) { g.cur_program = program; }

void on_bind_attrib_location(uint32_t, uint32_t, const char *) { /* Godot uses fixed slots */ }

void on_get_uniform_location(uint32_t program, const char *name, int loc) {
    Program *p = g.programs.get_or_add(program);
    if (!p || !name || loc < 0) return;
    if      (!strcmp(name, "projection_matrix")) p->loc[U_PROJECTION] = loc;
    else if (!strcmp(name, "modelview_matrix"))  p->loc[U_MODELVIEW] = loc;
    else if (!strcmp(name, "extra_matrix"))      p->loc[U_EXTRA] = loc;
    else if (!strcmp(name, "final_modulate"))    p->loc[U_FINAL_MODULATE] = loc;
    else if (!strcmp(name, "dst_rect"))          p->loc[U_DST_RECT] = loc;
    else if (!strcmp(name, "src_rect"))          p->loc[U_SRC_RECT] = loc;
    else if (!strcmp(name, "m_white"))           p->loc_white = loc;
}

void on_uniform_matrix4fv(int loc, int count, const float *v) {
    Program *p = cur_prog();
    if (!p || !v || count < 1) return;
    switch (sem_of_loc(p, loc)) {
        case U_PROJECTION: memcpy(p->projection, v, 16 * sizeof(float)); break;
        case U_MODELVIEW:  memcpy(p->modelview,  v, 16 * sizeof(float)); break;
        case U_EXTRA:      memcpy(p->extra,      v, 16 * sizeof(float)); break;
        default: break;
    }
}

void on_uniform4fv(int loc, int count, const float *v) {
    Program *p = cur_prog();
    if (!p || !v || count < 1) return;
    switch (sem_of_loc(p, loc)) {
        case U_FINAL_MODULATE: memcpy(p->final_modulate, v, 4 * sizeof(float)); break;
        case U_DST_RECT:       memcpy(p->dst_rect, v, 4 * sizeof(float)); break;
        case U_SRC_RECT:       memcpy(p->src_rect, v, 4 * sizeof(float)); break;
        default: break;
    }
}

void on_uniform4f(int loc, float x, float y, float z, float w) {
    const float v[4] = { x, y, z, w };
    on_uniform4fv(loc, 1, v);
}

void on_uniform1i(int loc, int v) {
    /* Sampler bindings are ignored (unit 0 is all we decode); the one value we
       need is Player.shader's bool, which Godot uploads with glUniform1i. */
    Program *p = cur_prog();
    if (p && loc >= 0 && loc == p->loc_white) p->white = v;
}

void on_active_texture(uint32_t unit) {
    uint32_t idx = unit - 0x84C0 /* GL_TEXTURE0 */;
    g.active_unit = (idx < 8) ? idx : 0;
}

void on_bind_texture(uint32_t target, uint32_t tex) {
    if (target != GL_TEXTURE_2D) return;
    g.bound_tex[g.active_unit] = tex;
}

void on_tex_image2d(uint32_t target, int level, int, int w, int h, int,
                    uint32_t fmt, uint32_t type, const void *px) {
    if (target != GL_TEXTURE_2D || level != 0) return;
    uint32_t id = g.bound_tex[g.active_unit];
    if (!id) return;
    Texture *t = g.textures.get_or_add(id);
    if (t) store_texture(t, w, h, fmt, type, px);
}

void on_tex_subimage2d(uint32_t target, int level, int x, int y, int w, int h,
                       uint32_t fmt, uint32_t type, const void *px) {
    if (target != GL_TEXTURE_2D || level != 0) return;
    uint32_t id = g.bound_tex[g.active_unit];
    Texture *t = id ? g.textures.find(id) : nullptr;
    if (!t || !t->rgba || !px) return;
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
    /* Glyph atlases arrive here one glyph at a time, in whichever single-channel
       or packed format the font backend chose, so this has to cover the same
       set as the full upload path or a whole atlas silently goes unshadowed. */
    int src_bpp;
    if (type == GL_UNSIGNED_BYTE &&
        (fmt == GL_RGBA || fmt == GL_ALPHA || fmt == GL_LUMINANCE ||
         fmt == GL_RGB || fmt == GL_LUMINANCE_ALPHA)) {
        src_bpp = (fmt == GL_RGBA) ? 4 : (fmt == GL_RGB ? 3 : (fmt == GL_LUMINANCE_ALPHA ? 2 : 1));
    } else if ((type == GL_UNSIGNED_SHORT_4_4_4_4 && fmt == GL_RGBA) ||
               (type == GL_UNSIGNED_SHORT_5_6_5 && fmt == GL_RGB)) {
        src_bpp = 2;
    } else {
        t->valid = false;   /* sub-upload we cannot mirror: force fallback */
        g.st.subuploads_rejected++;
        g.st.last_rej_fmt = fmt;
        g.st.last_rej_type = type;
        return;
    }

    const uint8_t *s = (const uint8_t *)px;
    for (int row = 0; row < h; row++) {
        uint8_t *d = t->rgba + ((size_t)(y + row) * t->w + x) * 4;
        const uint8_t *sr = s + (size_t)row * w * src_bpp;
        for (int col = 0; col < w; col++) {
            const uint8_t *sp = sr + (size_t)col * src_bpp;
            uint8_t *dp = d + col * 4;
            if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                uint16_t vv; memcpy(&vv, sp, 2);
                dp[0] = (uint8_t)(((vv >> 12) & 0xF) * 17); dp[1] = (uint8_t)(((vv >> 8) & 0xF) * 17);
                dp[2] = (uint8_t)(((vv >> 4) & 0xF) * 17);  dp[3] = (uint8_t)((vv & 0xF) * 17);
            } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
                uint16_t vv; memcpy(&vv, sp, 2);
                dp[0] = (uint8_t)(((vv >> 11) & 0x1F) * 255 / 31);
                dp[1] = (uint8_t)(((vv >> 5) & 0x3F) * 255 / 63);
                dp[2] = (uint8_t)((vv & 0x1F) * 255 / 31);
                dp[3] = 255;
            } else if (fmt == GL_RGBA) {
                memcpy(dp, sp, 4);
            } else if (fmt == GL_RGB) {
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            } else if (fmt == GL_LUMINANCE_ALPHA) {
                dp[0] = dp[1] = dp[2] = sp[0]; dp[3] = sp[1];
            } else if (fmt == GL_ALPHA) {
                dp[0] = dp[1] = dp[2] = 255; dp[3] = sp[0];
            } else { /* GL_LUMINANCE */
                dp[0] = dp[1] = dp[2] = sp[0]; dp[3] = 255;
            }
        }
    }
    t->generation++;
    t->opaque = false;
    t->valid = true;   /* allocated empty then filled by sub-uploads (font atlases) */
}

void on_copy_tex_image(uint32_t /*target*/) {
    /* Contents produced on the GPU: our shadow has no pixels for it, and the
       fabric cannot read GL memory, so any draw sampling it must fall back. */
    uint32_t id = g.bound_tex[g.active_unit];
    Texture *t = id ? g.textures.get_or_add(id) : nullptr;
    g.st.copytex_calls++;
    if (t) t->valid = false;
}

void on_tex_parameteri(uint32_t target, uint32_t pname, int param) {
    if (target != GL_TEXTURE_2D) return;
    uint32_t id = g.bound_tex[g.active_unit];
    Texture *t = id ? g.textures.get_or_add(id) : nullptr;
    if (!t) return;
    if (pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_FILTER)
        t->nearest = (param == GL_NEAREST);
}

void on_delete_textures(int n, const uint32_t *ids) {
    for (int i = 0; i < n; i++) {
        Texture *t = g.textures.find(ids[i]);
        if (!t) continue;
        free(t->rgba);
        t->rgba = nullptr;
        free(t->white_rgba);
        t->white_rgba = nullptr;
        t->white_built = false;
        t->valid = false;
        t->generation++;
    }
}

void on_bind_buffer(uint32_t target, uint32_t buf) {
    if (target == GL_ARRAY_BUFFER) g.array_buffer = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g.element_buffer = buf;
}

void on_buffer_data(uint32_t target, intptr_t size, const void *data, uint32_t) {
    uint32_t id = (target == GL_ARRAY_BUFFER) ? g.array_buffer : g.element_buffer;
    if (!id || size <= 0) return;
    Buffer *b = g.buffers.get_or_add(id);
    if (!b) return;
    uint8_t *nb = (uint8_t *)realloc(b->data, (size_t)size);
    if (!nb) return;
    b->data = nb;
    b->size = (size_t)size;
    if (data) memcpy(b->data, data, (size_t)size);
    else memset(b->data, 0, (size_t)size);
}

void on_buffer_sub_data(uint32_t target, intptr_t off, intptr_t size, const void *data) {
    uint32_t id = (target == GL_ARRAY_BUFFER) ? g.array_buffer : g.element_buffer;
    Buffer *b = id ? g.buffers.find(id) : nullptr;
    if (!b || !b->data || !data || off < 0 || size <= 0) return;
    if ((size_t)off + (size_t)size > b->size) return;
    memcpy(b->data + off, data, (size_t)size);
}

void on_delete_buffers(int n, const uint32_t *ids) {
    for (int i = 0; i < n; i++) {
        Buffer *b = g.buffers.find(ids[i]);
        if (!b) continue;
        free(b->data);
        b->data = nullptr;
        b->size = 0;
    }
}

void on_vertex_attrib_pointer(uint32_t index, int size, uint32_t type,
                              uint8_t normalized, int stride, const void *ptr) {
    if (index >= MAX_ATTRIBS) return;
    AttribState &a = g.attribs[index];
    a.size = size; a.type = type; a.normalized = normalized != 0;
    a.stride = stride; a.ptr = ptr; a.buffer = g.array_buffer;
}

void on_enable_vertex_attrib(uint32_t index, bool enabled) {
    if (index < MAX_ATTRIBS) g.attribs[index].enabled = enabled;
}

void on_vertex_attrib4f(uint32_t index, float x, float y, float z, float w) {
    if (index >= MAX_ATTRIBS) return;
    g.attribs[index].constant[0] = x; g.attribs[index].constant[1] = y;
    g.attribs[index].constant[2] = z; g.attribs[index].constant[3] = w;
}

void on_enable_cap(uint32_t cap, bool enabled) {
    if (cap == GL_BLEND) g.blend_enabled = enabled;
}

void on_blend_func(uint32_t src, uint32_t dst) { g.blend_src = src; g.blend_dst = dst; }

void on_blend_func_separate(uint32_t srcRGB, uint32_t dstRGB, uint32_t, uint32_t) {
    g.blend_src = srcRGB; g.blend_dst = dstRGB;
}

void on_bind_framebuffer(uint32_t target, uint32_t fbo) {
    if (target == GL_FRAMEBUFFER) g.bound_fbo = fbo;
}

void on_framebuffer_texture2d(uint32_t target, uint32_t attach, uint32_t /*textarget*/,
                              uint32_t tex, int /*level*/) {
    if (target != GL_FRAMEBUFFER || attach != GL_COLOR_ATTACHMENT0) return;
    if (!g.bound_fbo) return;
    Fbo *f = g.fbos.get_or_add(g.bound_fbo);
    if (f) f->color_tex = tex;
}

void on_gen_framebuffers(int n, const uint32_t *ids) {
    for (int i = 0; i < n; i++) g.fbos.get_or_add(ids[i]);
}

bool app_surface(uint32_t *fbo, uint32_t *tex) {
    if (!g.appsurf_fbo || !g.appsurf_tex) return false;
    if (fbo) *fbo = g.appsurf_fbo;
    if (tex) *tex = g.appsurf_tex;
    return true;
}

void on_viewport(int x, int y, int w, int h) { g.vp_x = x; g.vp_y = y; g.vp_w = w; g.vp_h = h; }
void on_scissor(int, int, int, int) {}
void on_clear_color(float r, float gg, float b, float a) {
    g.clear_color[0] = r; g.clear_color[1] = gg; g.clear_color[2] = b; g.clear_color[3] = a;
}

void get_clear_color(uint8_t *r, uint8_t *gg, uint8_t *b, uint8_t *a) {
    const float *c = g.clear_color;
    *r  = (uint8_t)(c[0] * 255.0f + 0.5f);
    *gg = (uint8_t)(c[1] * 255.0f + 0.5f);
    *b  = (uint8_t)(c[2] * 255.0f + 0.5f);
    *a  = (uint8_t)(c[3] * 255.0f + 0.5f);
}

bool target_is_default() { return g.bound_fbo == 0; }

uint32_t bound_texture() { return g.bound_tex[g.active_unit]; }

uint32_t bound_fbo() { return g.bound_fbo; }

const Stats &stats() { return g.st; }
void stats_reset() { memset(&g.st, 0, sizeof(g.st)); }

/* ---- draw decode --------------------------------------------------------- */
namespace {

/* One vertex through Godot's canvas vertex shader, ending in top-left-origin
   screen pixels. Mirrors canvas.glsl's main(); see the header comment. */
bool transform_vertex(const Program *p, int index, BVtx *out) {
    const ProgramInfo &pi = p->info;
    float pos[4] = { 0, 0, 0, 1 };
    float uv[2]  = { 0, 0 };
    float col[4] = { 1, 1, 1, 1 };

    float vtx[4];
    if (!read_attrib(g.attribs[ATTR_VERTEX], index, vtx)) return false;
    if (!read_attrib(g.attribs[ATTR_COLOR], index, col)) return false;
    if (pi.white_flash) col[0] = col[1] = col[2] = col[3] = 1.0f;  /* COLOR = texel */

    if (pi.texture_rect) {
        /* uv = src_rect.xy + abs(src_rect.zw) * vertex   (transposed if dst_rect.z < 0)
           pos = dst_rect.xy + abs(dst_rect.zw) * select(vertex, 1-vertex, src_rect.zw<0) */
        const float *dr = p->dst_rect, *sr = p->src_rect;
        float vx = vtx[0], vy = vtx[1];
        if (dr[2] < 0.0f) { uv[0] = sr[0] + fabsf(sr[2]) * vy; uv[1] = sr[1] + fabsf(sr[3]) * vx; }
        else              { uv[0] = sr[0] + fabsf(sr[2]) * vx; uv[1] = sr[1] + fabsf(sr[3]) * vy; }
        float sx = (sr[2] < 0.0f) ? (1.0f - vx) : vx;
        float sy = (sr[3] < 0.0f) ? (1.0f - vy) : vy;
        pos[0] = dr[0] + fabsf(dr[2]) * sx;
        pos[1] = dr[1] + fabsf(dr[3]) * sy;
    } else {
        pos[0] = vtx[0];
        pos[1] = vtx[1];
        float t[4];
        if (!read_attrib(g.attribs[ATTR_UV], index, t)) return false;
        uv[0] = t[0]; uv[1] = t[1];
    }

    if (pi.attrib_large) {
        float basis[4], translate[4];
        if (!read_attrib(g.attribs[ATTR_BASIS], index, basis)) return false;
        if (!read_attrib(g.attribs[ATTR_TRANSLATE], index, translate)) return false;
        float x = pos[0] * basis[0] + pos[1] * basis[2];
        float y = pos[0] * basis[1] + pos[1] * basis[3];
        pos[0] = x + translate[0];
        pos[1] = y + translate[1];
    } else if (!pi.skip_transform) {
        float tmp[4];
        mat4_mul_vec4(p->extra, pos, tmp);
        mat4_mul_vec4(p->modelview, tmp, pos);
    }

    if (pi.pixel_snap) {
        pos[0] = floorf(pos[0] + 0.5f);
        pos[1] = floorf(pos[1] + 0.5f);
    }

    float clip[4];
    mat4_mul_vec4(p->projection, pos, clip);
    if (!(clip[3] > 1e-6f || clip[3] < -1e-6f)) return false;
    float ndc_x = clip[0] / clip[3], ndc_y = clip[1] / clip[3];
    if (!isfinite(ndc_x) || !isfinite(ndc_y)) return false;

    /* GL viewport transform, then flip into the top-left origin BVtx wants. */
    out->x = (float)g.vp_x + (ndc_x * 0.5f + 0.5f) * (float)g.vp_w;
    out->y = (float)g.vp_y + (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)g.vp_h;
    out->u = uv[0];
    out->v = uv[1];

    if (pi.attrib_modulate) {
        float m[4];
        if (!read_attrib(g.attribs[ATTR_MODULATE], index, m)) return false;
        col[0] *= m[0]; col[1] *= m[1]; col[2] *= m[2]; col[3] *= m[3];
    } else {
        col[0] *= p->final_modulate[0]; col[1] *= p->final_modulate[1];
        col[2] *= p->final_modulate[2]; col[3] *= p->final_modulate[3];
    }
    out->r = col[0]; out->g = col[1]; out->b = col[2]; out->a = col[3];
    return true;
}

bool common_prologue(const Program **out_p, DecodedDraw *out) {
    g.st.draws_seen++;
    const Program *p = cur_prog();
    if (!p || !p->info.is_canvas) { g.st.fallback_program++; return false; }
    const ProgramInfo &pi = p->info;
    if (pi.custom_fragment && !pi.white_flash) { g.st.fallback_custom_shader++; return false; }
    if (pi.instancing || pi.skeleton || pi.lighting) { g.st.fallback_program++; return false; }

    bool blend_ok = false;
    RBlend bl = decode_blend(&blend_ok);
    if (!blend_ok) { g.st.fallback_blend++; return false; }

    uint32_t texid = g.bound_tex[0];
    Texture *t = texid ? g.textures.find(texid) : nullptr;
    if (!texid) { g.st.tex_unbound++; g.st.fallback_texture++; return false; }
    if (!t)     { g.st.tex_missing++; g.st.last_bad_tex = texid;
                  g.st.fallback_texture++; return false; }

    /* Sampling a render target has no CPU-side pixels by definition: the fabric
       composites from its own surface instead. Godot's viewport indirection is
       exactly this, and it is what the app-surface path exists for. */
    const uint32_t owner_fbo = fbo_owning_texture(texid);
    const bool sampling_rt = (owner_fbo != 0) && (owner_fbo != g.bound_fbo);

    if (sampling_rt) {
        if (getenv("MISTER_GLUE_LOGCOMPOSITE") && g.st.src_surface_draws < 1) {
            fprintf(stderr, "COMPOSITE draw: tex=%u %dx%d fbo=%u->%u blend=%d "
                            "vp=%d,%d %dx%d attr_color_enabled=%d const=%.3f,%.3f,%.3f,%.3f\n",
                    texid, t->w, t->h, g.bound_fbo, owner_fbo, (int)bl,
                    g.vp_x, g.vp_y, g.vp_w, g.vp_h,
                    (int)g.attribs[ATTR_COLOR].enabled,
                    (double)g.attribs[ATTR_COLOR].constant[0], (double)g.attribs[ATTR_COLOR].constant[1],
                    (double)g.attribs[ATTR_COLOR].constant[2], (double)g.attribs[ATTR_COLOR].constant[3]);
        }
        /* Drawing a render target into the default framebuffer identifies it as
           the application surface — the same signal gmloader keys on. */
        if (g.bound_fbo == 0 && !g.appsurf_fbo) {
            g.appsurf_fbo = owner_fbo;
            g.appsurf_tex = texid;
        }
        out->src_is_surface = true;
        g.st.src_surface_draws++;
    } else if (!t->valid || !t->rgba) {
        g.st.tex_invalid++;
        if (!t->rgba) g.st.tex_never_uploaded++;
        g.st.last_bad_tex = texid; g.st.last_bad_w = t->w; g.st.last_bad_h = t->h;
        g.st.fallback_texture++;
        return false;
    }

    out->tex.rgba    = out->src_is_surface ? nullptr : t->rgba;
    out->tex.w       = t->w;
    out->tex.h       = t->h;
    out->tex.nearest = t->nearest ? 1 : 0;
    out->tex.valid   = out->src_is_surface ? 0 : 1;
    out->tex.format  = RTEX_RGBA8888;
    out->tex.opaque  = t->opaque ? 1 : 0;
    out->blend       = bl;
    out->to_default_fb = (g.bound_fbo == 0);
    out->dst_fbo = g.bound_fbo;
    /* The raw GL texture id: the backend matches tex_key against the registered
       app-surface texture, so it must not be hashed. Re-uploads are handled by
       invalidating the backend's cache entry, not by changing the key. */
    out->tex_key = texid;

    /* Player.shader with `white` set: rgb = mix(texel, 1, 0.75), alpha kept. The
       fabric only multiplies texel by vertex colour, so sample a pre-whitened
       copy of the texture under its own cache key instead. */
    if (pi.white_flash && p->white && !out->src_is_surface) {
        if (!t->white_built || t->white_gen != t->generation || !t->white_rgba) {
            const size_t n = (size_t)t->w * t->h;
            uint8_t *w = (uint8_t *)realloc(t->white_rgba, n * 4);
            if (!w) { g.st.fallback_texture++; return false; }
            for (size_t i = 0; i < n; i++) {
                for (int c = 0; c < 3; c++)
                    w[i * 4 + c] = (uint8_t)((t->rgba[i * 4 + c] + 3 * 255 + 2) / 4);
                w[i * 4 + 3] = t->rgba[i * 4 + 3];
            }
            t->white_rgba = w;
            t->white_gen = t->generation;
            t->white_built = true;
        }
        out->tex.rgba = t->white_rgba;
        out->tex_key  = texid | kWhiteTexKeyBit;
    }
    *out_p = p;
    return true;
}

/* Expand the primitive into triangles, transforming each referenced vertex. */
bool emit_tris(const Program *p, uint32_t mode, const int *idx, int count, DecodedDraw *out) {
    int tris;
    switch (mode) {
        case GL_TRIANGLES:      tris = count / 3; break;
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:   tris = count - 2; break;
        default: g.st.fallback_mode++; return false;
    }
    if (tris <= 0) { g.st.fallback_mode++; return false; }

    BVtx *vb = vbuf_reserve(tris * 3);
    if (!vb) return false;

    int w = 0;
    for (int t = 0; t < tris; t++) {
        int a, b, c;
        if (mode == GL_TRIANGLES)          { a = t * 3; b = t * 3 + 1; c = t * 3 + 2; }
        else if (mode == GL_TRIANGLE_FAN)  { a = 0;     b = t + 1;     c = t + 2; }
        else { /* strip: keep winding consistent */
            if (t & 1) { a = t + 1; b = t; c = t + 2; }
            else       { a = t;     b = t + 1; c = t + 2; }
        }
        const int src[3] = { idx ? idx[a] : a, idx ? idx[b] : b, idx ? idx[c] : c };
        for (int k = 0; k < 3; k++) {
            if (!transform_vertex(p, src[k], &vb[w])) { g.st.fallback_attrib++; return false; }
            w++;
        }
    }
    out->verts = vb;
    out->tris  = tris;
    return true;
}


/* One-shot dump of a decoded draw's vertices: the fastest way to tell a
   degenerate transform from an off-screen one. */
void log_decoded(const DecodedDraw *d, const Program *p, const char *tag) {
    static int shots;
    if (!getenv("MISTER_GLUE_LOGVERTS") || shots >= 4) return;
    shots++;
    const ProgramInfo &pi = p->info;
    fprintf(stderr, "%s: tris=%d rect=%d large=%d skip=%d snap=%d modul=%d "
                    "dst_rect=%.2f,%.2f,%.2f,%.2f src_rect=%.3f,%.3f,%.3f,%.3f\n",
            tag, d->tris, (int)pi.texture_rect, (int)pi.attrib_large, (int)pi.skip_transform,
            (int)pi.pixel_snap, (int)pi.attrib_modulate,
            (double)p->dst_rect[0], (double)p->dst_rect[1], (double)p->dst_rect[2], (double)p->dst_rect[3],
            (double)p->src_rect[0], (double)p->src_rect[1], (double)p->src_rect[2], (double)p->src_rect[3]);
    int n = d->tris * 3; if (n > 6) n = 6;
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  v%d xy=%.2f,%.2f uv=%.4f,%.4f rgba=%.2f,%.2f,%.2f,%.2f\n",
                i, (double)d->verts[i].x, (double)d->verts[i].y,
                (double)d->verts[i].u, (double)d->verts[i].v,
                (double)d->verts[i].r, (double)d->verts[i].g,
                (double)d->verts[i].b, (double)d->verts[i].a);
    /* the vertex attribute feeding the transform, before any of our maths */
    const AttribState &va = g.attribs[ATTR_VERTEX];
    fprintf(stderr, "  attr0 enabled=%d size=%d type=0x%x stride=%d buf=%u ptr=%p\n",
            (int)va.enabled, va.size, va.type, va.stride, va.buffer, va.ptr);
}

} /* anonymous namespace */

bool decode_draw_arrays(uint32_t mode, int first, int count, DecodedDraw *out) {
    const Program *p = nullptr;
    if (!common_prologue(&p, out)) { g.st.draws_fallback++; return false; }

    /* Fold `first` in by shifting indices; keeps emit_tris index-agnostic. */
    static int idxbuf[4096];
    if (count > (int)(sizeof(idxbuf) / sizeof(idxbuf[0]))) { g.st.draws_fallback++; return false; }
    for (int i = 0; i < count; i++) idxbuf[i] = first + i;

    if (!emit_tris(p, mode, idxbuf, count, out)) { g.st.draws_fallback++; return false; }
    if (out->src_is_surface) log_decoded(out, p, "COMPOSITE(arrays)");
    g.st.draws_decoded++;
    return true;
}

bool decode_draw_elements(uint32_t mode, int count, uint32_t type,
                          const void *indices, DecodedDraw *out) {
    const Program *p = nullptr;
    if (!common_prologue(&p, out)) { g.st.draws_fallback++; return false; }

    static int idxbuf[4096];
    if (count <= 0 || count > (int)(sizeof(idxbuf) / sizeof(idxbuf[0]))) {
        g.st.draws_fallback++; return false;
    }

    const uint8_t *base = nullptr;
    if (g.element_buffer) {
        Buffer *b = g.buffers.find(g.element_buffer);
        if (!b || !b->data) { g.st.fallback_attrib++; g.st.draws_fallback++; return false; }
        size_t off = (size_t)(uintptr_t)indices;
        size_t need = (size_t)count * (type == GL_UNSIGNED_BYTE ? 1 : 2);
        if (off + need > b->size) { g.st.fallback_attrib++; g.st.draws_fallback++; return false; }
        base = b->data + off;
    } else {
        base = (const uint8_t *)indices;
        if (!base) { g.st.fallback_attrib++; g.st.draws_fallback++; return false; }
    }

    for (int i = 0; i < count; i++) {
        if (type == GL_UNSIGNED_BYTE) idxbuf[i] = base[i];
        else if (type == GL_UNSIGNED_SHORT) { uint16_t v; memcpy(&v, base + i * 2, 2); idxbuf[i] = v; }
        else { g.st.fallback_mode++; g.st.draws_fallback++; return false; }
    }

    if (!emit_tris(p, mode, idxbuf, count, out)) { g.st.draws_fallback++; return false; }
    if (out->src_is_surface) log_decoded(out, p, "COMPOSITE(elements)");
    g.st.draws_decoded++;
    return true;
}

} /* namespace gs */
