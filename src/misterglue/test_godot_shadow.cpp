/* Host test for godot_shadow.cpp's object tables: make test

   Godot frees a level's textures, buffers and shader programs when the level
   unloads, and null GL never reuses a name, so over a session the shadow sees
   thousands of ids while only a handful are live. Each loop below creates,
   draws with and deletes far more objects than a table holds; every draw must
   still decode, and the tables must end where they started. */
#include "godot_shadow.h"

#include <stdio.h>
#include <string.h>

namespace {

enum {
    GL_FLOAT = 0x1406, GL_UNSIGNED_BYTE = 0x1401, GL_RGBA = 0x1908,
    GL_TEXTURE_2D = 0x0DE1, GL_ARRAY_BUFFER = 0x8892, GL_TRIANGLE_STRIP = 0x0005,
    GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_TEXTURE0 = 0x84C0
};

const char *kCanvasVS =
    "uniform highp mat4 projection_matrix;\n"
    "uniform highp mat4 modelview_matrix;\n"
    "uniform highp mat4 extra_matrix;\n"
    "uniform mediump vec4 final_modulate;\n";

int g_fail = 0;
uint32_t g_name = 1;   /* null GL style: never reused */
int g_loc = 1;

#define EXPECT(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* A canvas program with identity transforms, made current. */
uint32_t make_program() {
    uint32_t vs = g_name++, prog = g_name++;
    gs::on_shader_source(vs, kCanvasVS);
    gs::on_attach_shader(prog, vs);
    gs::on_delete_shader(vs);   /* Godot deletes shaders once linked */
    gs::on_use_program(prog);
    const char *names[] = { "projection_matrix", "modelview_matrix", "extra_matrix", "final_modulate" };
    int locs[4];
    for (int i = 0; i < 4; i++) { locs[i] = g_loc++; gs::on_get_uniform_location(prog, names[i], locs[i]); }
    for (int i = 0; i < 3; i++) gs::on_uniform_matrix4fv(locs[i], 1, kIdentity);
    gs::on_uniform4f(locs[3], 1, 1, 1, 1);
    return prog;
}

/* A unit quad in a fresh VBO: position (attrib 0) and uv (attrib 4). */
uint32_t make_quad_buffer() {
    static const float q[] = { -1, -1, 0, 0,   1, -1, 1, 0,   -1, 1, 0, 1,   1, 1, 1, 1 };
    uint32_t buf = g_name++;
    gs::on_bind_buffer(GL_ARRAY_BUFFER, buf);
    gs::on_buffer_data(GL_ARRAY_BUFFER, sizeof q, q, 0);
    gs::on_vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 16, (const void *)0);
    gs::on_enable_vertex_attrib(0, true);
    gs::on_vertex_attrib_pointer(4, 2, GL_FLOAT, 0, 16, (const void *)8);
    gs::on_enable_vertex_attrib(4, true);
    return buf;
}

uint32_t make_texture() {
    static const uint8_t px[2 * 2 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    uint32_t tex = g_name++;
    gs::on_active_texture(GL_TEXTURE0);
    gs::on_bind_texture(GL_TEXTURE_2D, tex);
    gs::on_tex_image2d(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return tex;
}

bool draw_ok() {
    gs::DecodedDraw d;
    return gs::decode_draw_arrays(GL_TRIANGLE_STRIP, 0, 4, &d) && d.tris == 2;
}

void test_texture_churn() {
    make_program();
    make_quad_buffer();
    gs::TableUse before = gs::table_use();
    for (int i = 0; i < 4000; i++) {
        uint32_t tex = make_texture();
        if (!draw_ok()) { EXPECT(false, "texture %d (id %u): draw did not decode", i, tex); return; }
        gs::on_delete_textures(1, &tex);
    }
    EXPECT(gs::table_use().textures == before.textures, "texture slots %d, want %d",
           gs::table_use().textures, before.textures);
}

void test_buffer_churn() {
    uint32_t tex = make_texture();
    gs::TableUse before = gs::table_use();
    for (int i = 0; i < 4000; i++) {
        uint32_t buf = make_quad_buffer();
        if (!draw_ok()) { EXPECT(false, "buffer %d (id %u): draw did not decode", i, buf); return; }
        gs::on_delete_buffers(1, &buf);
    }
    EXPECT(gs::table_use().buffers == before.buffers, "buffer slots %d, want %d",
           gs::table_use().buffers, before.buffers);
    gs::on_delete_textures(1, &tex);
}

void test_program_churn() {
    uint32_t tex = make_texture();
    uint32_t buf = make_quad_buffer();
    uint32_t base = make_program();
    gs::TableUse before = gs::table_use();
    for (int i = 0; i < 1000; i++) {
        uint32_t prog = make_program();
        if (!draw_ok()) { EXPECT(false, "program %d (id %u): draw did not decode", i, prog); return; }
        /* Deleted while current: GL keeps it usable until another one is bound. */
        gs::on_delete_program(prog);
        EXPECT(draw_ok(), "program %d: deleted-while-current program stopped decoding", i);
        gs::on_use_program(base);
    }
    gs::TableUse after = gs::table_use();
    EXPECT(after.programs == before.programs, "program slots %d, want %d", after.programs, before.programs);
    EXPECT(after.shaders == before.shaders, "shader slots %d, want %d", after.shaders, before.shaders);
    gs::on_delete_buffers(1, &buf);
    gs::on_delete_textures(1, &tex);
}

void test_fbo_churn() {
    gs::TableUse before = gs::table_use();
    for (int i = 0; i < 1000; i++) {
        uint32_t fbo = g_name++;
        gs::on_gen_framebuffers(1, &fbo);
        gs::on_bind_framebuffer(GL_FRAMEBUFFER, fbo);
        uint32_t tex = make_texture();
        gs::on_framebuffer_texture2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        gs::on_delete_framebuffers(1, &fbo);
        gs::on_delete_textures(1, &tex);
        if (gs::bound_fbo() != 0) {
            EXPECT(false, "fbo %d: deleting the bound FBO must revert to the default", i);
            gs::on_bind_framebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
    }
    EXPECT(gs::table_use().fbos == before.fbos, "fbo slots %d, want %d", gs::table_use().fbos, before.fbos);
}

}  // namespace

int main() {
    gs::init(320, 240);
    test_texture_churn();
    test_buffer_churn();
    test_program_churn();
    test_fbo_churn();
    gs::shutdown();
    if (g_fail) { printf("test_godot_shadow: %d failure(s)\n", g_fail); return 1; }
    printf("test_godot_shadow: ok\n");
    return 0;
}
