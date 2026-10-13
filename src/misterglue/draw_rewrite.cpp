/* Draw rewrites that cut fabric time. See draw_rewrite.h. */
#include "draw_rewrite.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace dr {
namespace {

/* Below this many source texels a rotated sprite is cheap enough as it is. */
constexpr int kMinTexels = 32 * 32;
/* Rotated draws handled per frame; any beyond take the normal path. */
constexpr int kSlots = 8;
/* Largest scratch side: a quad bigger than this on screen takes the normal path. */
constexpr int kMaxSide = 512;

struct Scratch {
    uint8_t *rgba = nullptr;
    int cap = 0;   /* texels */
};
Scratch g_scratch[2][kSlots];
int g_parity = 0;
int g_used = 0;

bool near(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* Every texel the same colour; returns it. Large textures are not checked. */
bool uniform_texture(const RTexture &t, uint8_t out[4]) {
    if (!t.valid || !t.rgba || t.format != RTEX_RGBA8888) return false;
    const int n = t.w * t.h;
    if (n <= 0 || n > 64 * 64) return false;
    for (int i = 1; i < n; i++)
        if (memcmp(t.rgba + i * 4, t.rgba, 4) != 0) return false;
    memcpy(out, t.rgba, 4);
    return true;
}

/* The two triangles are one axis-aligned rectangle: every vertex on one of the
   bounding box's corners (0..3 going round), each triangle using three distinct
   corners, and the two leaving out opposite corners, i.e. split on a diagonal. */
bool axis_rect(const BVtx *v, float *px0, float *py0, float *px1, float *py1) {
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < 6; i++) {
        x0 = fminf(x0, v[i].x); x1 = fmaxf(x1, v[i].x);
        y0 = fminf(y0, v[i].y); y1 = fmaxf(y1, v[i].y);
    }
    if (x1 - x0 <= 0.0f || y1 - y0 <= 0.0f) return false;
    int omitted[2];
    for (int t = 0; t < 2; t++) {
        int mask = 0;
        for (int i = 3 * t; i < 3 * t + 3; i++) {
            const bool l = near(v[i].x, x0), r = near(v[i].x, x1);
            const bool tp = near(v[i].y, y0), bt = near(v[i].y, y1);
            if (!(l || r) || !(tp || bt)) return false;
            mask |= 1 << (tp ? (l ? 0 : 1) : (r ? 2 : 3));
        }
        if (__builtin_popcount(mask) != 3) return false;
        omitted[t] = __builtin_ctz(~mask & 0xF);
    }
    if (omitted[1] != (omitted[0] + 2) % 4) return false;
    *px0 = x0; *py0 = y0; *px1 = x1; *py1 = y1;
    return true;
}

} /* namespace */

bool identity_present(const gs::DecodedDraw &d, int w, int h) {
    if (!d.src_is_surface || !d.to_default_fb || d.tris != 2 || !d.verts) return false;
    if (d.blend != RB_NONE || d.tex.w != w || d.tex.h != h) return false;
    const BVtx *v = d.verts;
    float x0, y0, x1, y1;
    if (!axis_rect(v, &x0, &y0, &x1, &y1)) return false;
    if (!near(x0, 0.0f) || !near(y0, 0.0f) || !near(x1, (float)w) || !near(y1, (float)h)) return false;
    /* The backend samples the surface at (u*w, (vmin+vmax-v)*h): GL's
       bottom-origin texture rows, flipped about the quad's own V extent. */
    float vmin = v[0].v, vmax = v[0].v;
    for (int i = 1; i < 6; i++) { vmin = fminf(vmin, v[i].v); vmax = fmaxf(vmax, v[i].v); }
    for (int i = 0; i < 6; i++) {
        if (v[i].r < 0.999f || v[i].g < 0.999f || v[i].b < 0.999f || v[i].a < 0.999f) return false;
        if (fabsf(v[i].u * w - v[i].x) > 0.01f || fabsf((vmin + vmax - v[i].v) * h - v[i].y) > 0.01f)
            return false;
    }
    return true;
}

bool solid_cover(const gs::DecodedDraw &d, int w, int h, uint8_t rgba[4]) {
    if (d.tris != 2 || d.src_is_surface || !d.verts) return false;
    if (d.blend != RB_NONE && d.blend != RB_ALPHA) return false;
    uint8_t tx[4];
    if (!uniform_texture(d.tex, tx)) return false;

    const BVtx *v = d.verts;
    for (int i = 1; i < 6; i++)
        if (!near(v[i].r, v[0].r) || !near(v[i].g, v[0].g) ||
            !near(v[i].b, v[0].b) || !near(v[i].a, v[0].a)) return false;
    float x0, y0, x1, y1;
    if (!axis_rect(v, &x0, &y0, &x1, &y1)) return false;
    if (x0 > 0.0f || y0 > 0.0f || x1 < (float)w || y1 < (float)h) return false;

    /* frag = vertex colour * texel; it must be opaque for a FILL to equal it. */
    const float a = v[0].a * tx[3] / 255.0f;
    if (d.blend == RB_ALPHA && a < 0.999f) return false;
    const float c[3] = { v[0].r, v[0].g, v[0].b };
    for (int k = 0; k < 3; k++) {
        float f = c[k] * tx[k];
        rgba[k] = (uint8_t)(f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f + 0.5f);
    }
    rgba[3] = 255;
    return true;
}

bool unrotate(gs::DecodedDraw &d, int w, int h, void (*invalidate)(uint32_t key)) {
    const RTexture &t = d.tex;
    if (d.tris != 2 || d.src_is_surface || !d.verts) return false;
    if (!t.valid || !t.rgba || t.format != RTEX_RGBA8888 || !t.nearest) return false;
    if (t.w * t.h < kMinTexels) return false;
    if (g_used >= kSlots) return false;

    /* The screen->texel affine map from the first triangle. */
    BVtx *v = d.verts;
    const float ex1 = v[1].x - v[0].x, ey1 = v[1].y - v[0].y;
    const float ex2 = v[2].x - v[0].x, ey2 = v[2].y - v[0].y;
    const float det = ex1 * ey2 - ex2 * ey1;
    if (fabsf(det) < 1e-6f) return false;
    const float tw = (float)t.w, th = (float)t.h;
    const float du1 = (v[1].u - v[0].u) * tw, du2 = (v[2].u - v[0].u) * tw;
    const float dv1 = (v[1].v - v[0].v) * th, dv2 = (v[2].v - v[0].v) * th;
    const float dudx = (du1 * ey2 - du2 * ey1) / det, dudy = (du2 * ex1 - du1 * ex2) / det;
    const float dvdx = (dv1 * ey2 - dv2 * ey1) / det, dvdy = (dv2 * ex1 - dv1 * ex2) / det;

    /* Rotated (or sheared): texture rows are not screen rows. */
    if (fabsf(dudy) < 1e-3f && fabsf(dvdx) < 1e-3f) return false;

    /* The second triangle must lie on the same map: one quad, one sprite. */
    for (int i = 3; i < 6; i++) {
        const float px = v[i].x - v[0].x, py = v[i].y - v[0].y;
        if (fabsf(v[0].u * tw + dudx * px + dudy * py - v[i].u * tw) > 0.01f ||
            fabsf(v[0].v * th + dvdx * px + dvdy * py - v[i].v * th) > 0.01f) return false;
    }

    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 6; i++) {
        minx = fminf(minx, v[i].x); maxx = fmaxf(maxx, v[i].x);
        miny = fminf(miny, v[i].y); maxy = fmaxf(maxy, v[i].y);
    }
    /* The whole quad, not just its on-screen part: the backend clamps UVs to
       [0,1], so every vertex must land inside the scratch texture. */
    if (maxx <= 0.0f || maxy <= 0.0f || minx >= (float)w || miny >= (float)h) return false;
    const int bx0 = (int)floorf(minx), by0 = (int)floorf(miny);
    const int W = (int)ceilf(maxx) - bx0, H = (int)ceilf(maxy) - by0;
    if (W <= 0 || H <= 0 || W > kMaxSide || H > kMaxSide) return false;

    Scratch &s = g_scratch[g_parity][g_used];
    if (s.cap < W * H) {
        uint8_t *nb = (uint8_t *)realloc(s.rgba, (size_t)W * H * 4);
        if (!nb) return false;
        s.rgba = nb;
        s.cap = W * H;
    }

    /* Nearest sample at each pixel centre, as the fabric (and GL) would; the
       pixels outside the quad are never drawn, so clamping them is harmless. */
    const float u_org = v[0].u * tw - dudx * v[0].x - dudy * v[0].y;
    const float v_org = v[0].v * th - dvdx * v[0].x - dvdy * v[0].y;
    const uint32_t *src = (const uint32_t *)t.rgba;
    uint32_t *dst = (uint32_t *)s.rgba;
    for (int py = 0; py < H; py++) {
        const float Y = (float)(by0 + py) + 0.5f, X = (float)bx0 + 0.5f;
        float u = u_org + dudx * X + dudy * Y;
        float vv = v_org + dvdx * X + dvdy * Y;
        for (int px = 0; px < W; px++, u += dudx, vv += dvdx) {
            int tx = (int)floorf(u), ty = (int)floorf(vv);
            tx = tx < 0 ? 0 : tx >= t.w ? t.w - 1 : tx;
            ty = ty < 0 ? 0 : ty >= t.h ? t.h - 1 : ty;
            *dst++ = src[ty * t.w + tx];
        }
    }

    const uint32_t key = kScratchKeyBase | ((uint32_t)g_parity << 8) | (uint32_t)g_used;
    g_used++;
    if (invalidate) invalidate(key);

    for (int i = 0; i < 6; i++) {
        v[i].u = (v[i].x - (float)bx0) / (float)W;
        v[i].v = (v[i].y - (float)by0) / (float)H;
    }
    d.tex.rgba    = s.rgba;
    d.tex.w       = W;
    d.tex.h       = H;
    d.tex.nearest = 1;
    d.tex.valid   = 1;
    d.tex.format  = RTEX_RGBA8888;
    /* opaque stays as the source's: every drawn pixel is a source texel */
    d.tex_key     = key;
    return true;
}

void frame_end() {
    g_parity ^= 1;
    g_used = 0;
}

} /* namespace dr */
