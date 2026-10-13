/* Host test for draw_rewrite.cpp: make test

   Each rewrite must leave the picture unchanged. The software rasterizer the
   glue keeps as an oracle (blitter_raster.cpp) draws the original draw and the
   rewritten one into separate surfaces, and the two are compared pixel for
   pixel. */
#include "draw_rewrite.h"
#include "blitter_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int g_fail = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

constexpr int W = 320, H = 240;

struct Surf {
    uint8_t px[W * H * 4];
    RSurface s;
    Surf() { s.rgba = px; s.w = W; s.h = H; reset(); }
    void reset() { memset(px, 0x40, sizeof px); }
};

/* A 128x128 sprite with an alpha cut-out and a texel pattern that makes any
   off-by-one sample show. */
uint8_t g_sprite[128 * 128 * 4];
RTexture sprite_tex() {
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            uint8_t *p = g_sprite + (y * 128 + x) * 4;
            p[0] = (uint8_t)(x * 2); p[1] = (uint8_t)(y * 2); p[2] = (uint8_t)((x ^ y) * 4);
            const int dx = x - 64, dy = y - 64;
            p[3] = (dx * dx + dy * dy < 60 * 60) ? 255 : 0;
        }
    return RTexture{ g_sprite, 128, 128, 1, 1, RTEX_RGBA8888, 0 };
}

/* Two triangles of a size x size quad centred on (cx, cy), turned by angle. */
void quad(BVtx *v, float cx, float cy, float size, float angle, float alpha = 1.0f) {
    const float c = cosf(angle), s = sinf(angle), h = size * 0.5f;
    const float cu[4] = { 0, 1, 1, 0 }, cv[4] = { 0, 0, 1, 1 };
    const float lx[4] = { -h, h, h, -h }, ly[4] = { -h, -h, h, h };
    const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        const int k = order[i];
        v[i] = BVtx{};
        v[i].x = cx + lx[k] * c - ly[k] * s;
        v[i].y = cy + lx[k] * s + ly[k] * c;
        v[i].u = cu[k]; v[i].v = cv[k];
        v[i].r = v[i].g = v[i].b = 1.0f; v[i].a = alpha;
    }
}

uint32_t g_invalidated = 0;
void note_invalidate(uint32_t key) { g_invalidated = key; }

void test_unrotate_matches(float cx, float cy, float angle) {
    static Surf ref, got;
    ref.reset(); got.reset();
    RTexture tex = sprite_tex();
    BVtx v[6], w[6];
    quad(v, cx, cy, 128, angle);
    memcpy(w, v, sizeof v);
    Blitter_RasterDraw(&ref.s, v, 2, &tex, RB_ALPHA, 0.0f, 1);

    gs::DecodedDraw d;
    d.verts = w; d.tris = 2; d.tex = tex; d.blend = RB_ALPHA; d.tex_key = 7;
    const bool rewritten = dr::unrotate(d, W, H, note_invalidate);
    EXPECT(rewritten, "angle %.3f at (%.0f,%.0f): rotated quad not rewritten", angle, cx, cy);
    if (!rewritten) return;
    EXPECT(d.tex_key == g_invalidated && (d.tex_key & dr::kScratchKeyBase),
           "scratch key %#x must be invalidated before use", d.tex_key);
    Blitter_RasterDraw(&got.s, d.verts, d.tris, &d.tex, d.blend, 0.0f, 1);
    /* Exact, except where a pixel centre sits on a texel edge to within the
       float rounding of the two different interpolations: there either texel
       is a correct nearest sample. Check each differing pixel is such a case. */
    const double c = cos(angle), s = sin(angle);
    int bad = 0, edge = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (memcmp(ref.px + (y * W + x) * 4, got.px + (y * W + x) * 4, 4) == 0) continue;
            /* the exact texel coordinate of this pixel centre (inverse of quad()) */
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double tu = (dx * c + dy * s) + 64.0, tv = (-dx * s + dy * c) + 64.0;
            const double fu = fabs(tu - floor(tu + 0.5)), fv = fabs(tv - floor(tv + 0.5));
            if (fu < 1e-3 || fv < 1e-3) edge++;
            else bad++;
        }
    EXPECT(bad == 0, "angle %.3f at (%.0f,%.0f): %d pixels differ off a texel edge (%d on one)",
           angle, cx, cy, bad, edge);
    dr::frame_end();
}

void test_unrotate() {
    for (int i = 0; i < 64; i++) test_unrotate_matches(160, 112, 0.05f + i * 0.098f);
    test_unrotate_matches(20, 30, 0.7f);     /* partly off the top-left */
    test_unrotate_matches(300, 230, 2.2f);   /* partly off the bottom-right */

    RTexture tex = sprite_tex();
    BVtx v[6];
    gs::DecodedDraw d;
    d.verts = v; d.tris = 2; d.tex = tex; d.blend = RB_ALPHA;
    quad(v, 160, 112, 128, 0.0f);
    EXPECT(!dr::unrotate(d, W, H, nullptr), "an upright quad must take the normal path");
    quad(v, 160, 112, 128, (float)M_PI);
    EXPECT(!dr::unrotate(d, W, H, nullptr), "a quad turned 180 degrees reads rows already");
    RTexture small = tex; small.w = small.h = 16;
    d.tex = small;
    quad(v, 160, 112, 16, 0.7f);
    EXPECT(!dr::unrotate(d, W, H, nullptr), "small rotated sprites are left alone");
    d.tex = tex;
    quad(v, 400, 112, 128, 0.7f);
    v[0].x = v[3].x = 500;   /* wholly off screen */
    EXPECT(!dr::unrotate(d, W, H, nullptr), "off-screen quad must not be rewritten");

    /* Parity: the same slot in consecutive frames gets different keys. */
    quad(v, 160, 112, 128, 0.7f);
    BVtx w[6]; memcpy(w, v, sizeof v);
    d.verts = v; dr::unrotate(d, W, H, nullptr); const uint32_t k0 = d.tex_key;
    dr::frame_end();
    d.verts = w; d.tex = tex; dr::unrotate(d, W, H, nullptr); const uint32_t k1 = d.tex_key;
    EXPECT(k0 != k1, "consecutive frames reused scratch key %#x", k0);
    dr::frame_end();
}

void test_solid_cover() {
    static uint8_t white[8 * 8 * 4];
    memset(white, 255, sizeof white);
    RTexture tex{ white, 8, 8, 1, 1, RTEX_RGBA8888, 1 };
    BVtx v[6];
    const float xs[4] = { 0, 320, 320, 0 }, ys[4] = { -8, -8, 248, 248 };
    const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        v[i] = BVtx{};
        v[i].x = xs[order[i]]; v[i].y = ys[order[i]];
        v[i].u = v[i].v = 0.5f;
        v[i].r = 0.0f; v[i].g = 0.5f; v[i].b = 0.0f; v[i].a = 1.0f;
    }
    gs::DecodedDraw d;
    d.verts = v; d.tris = 2; d.tex = tex; d.blend = RB_ALPHA;
    uint8_t c[4];
    EXPECT(dr::solid_cover(d, W, H, c), "full-screen opaque quad must become a fill");

    static Surf ref, got;
    Blitter_RasterDraw(&ref.s, v, 2, &tex, RB_ALPHA, 0.0f, 1);
    Blitter_ClearSurface(&got.s, c[0], c[1], c[2], c[3]);
    int off = 0;   /* the rasterizer and the fill may round 0.5*255 differently */
    for (int i = 0; i < W * H * 4; i++) if (abs(ref.px[i] - got.px[i]) > 1) off++;
    EXPECT(off == 0, "fill colour %u,%u,%u differs from the quad by more than 1 LSB", c[0], c[1], c[2]);

    BVtx keep[6]; memcpy(keep, v, sizeof v);
    for (int i = 0; i < 6; i++) v[i].a = 0.5f;
    EXPECT(!dr::solid_cover(d, W, H, c), "translucent quad is not a fill");
    memcpy(v, keep, sizeof v);
    for (int i = 0; i < 6; i++) if (v[i].x > 300) v[i].x = 300;
    EXPECT(!dr::solid_cover(d, W, H, c), "quad short of the right edge is not a fill");
    memcpy(v, keep, sizeof v);
    v[3] = v[0]; v[4] = v[1]; v[5] = v[2];   /* same half twice */
    EXPECT(!dr::solid_cover(d, W, H, c), "two copies of one triangle do not cover the rectangle");
    memcpy(v, keep, sizeof v);
    white[5 * 4] = 0;
    EXPECT(!dr::solid_cover(d, W, H, c), "non-uniform texture is not a fill");
    white[5 * 4] = 255;
    d.blend = RB_ADD;
    EXPECT(!dr::solid_cover(d, W, H, c), "additive blend is not a fill");
}

}  // namespace

int main() {
    test_unrotate();
    test_solid_cover();
    if (g_fail) { printf("test_draw_rewrite: %d failure(s)\n", g_fail); return 1; }
    printf("test_draw_rewrite: ok\n");
    return 0;
}
