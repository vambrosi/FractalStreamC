/* gfx_demo.c - windowed smoke test of the renderer (src/gfx.c).
 *
 * Opens a window, renders a Mandelbrot viewer into a target every frame and draws
 * that target plus translucent triangles and a clipped rectangle through a draw
 * list. Halfway through it resizes the window to exercise swapchain recreation.
 *
 * Two uses:
 *   - tests/test_gfx.c includes this file and runs gfx_demo_run() as the test
 *     gfx.window_smoke (skipped without a display: `xvfb-run -a ./build.sh test gfx`);
 *   - tools/gfx_demo.sh builds it as a program (-DGFX_DEMO_MAIN) and takes a
 *     screenshot under Xvfb:  build/gfx_demo [seconds]
 * It uses GLFW directly (not platform.c) so it only depends on gfx.
 */
#ifdef GFX_DEMO_MAIN
/* Standalone: only the modules the renderer needs (same preamble as src/all.c). */
#include "../src/base.c"
#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "../src/gfx.c"
#endif
#include "test_gfx_shaders.h"

typedef struct DemoGlobals { float center[2]; float radius; float time; } DemoGlobals; /* demo_mandel.frag */

typedef struct DemoList {      /* a small fixed-capacity draw list */
    GfxVertex verts[64];
    u32 indices[96];
    GfxCmd cmds[8];
    GfxDrawList dl;
} DemoList;

static void demo_vertex(DemoList *L, float x, float y, float u, float v, u32 rgba) {
    GfxVertex *p = &L->verts[L->dl.nverts++];
    p->x = x; p->y = y; p->u = u; p->v = v; p->rgba = rgba;
}

/* Starts a command using `texture` clipped to the given rectangle. */
static void demo_cmd(DemoList *L, int texture, float x0, float y0, float x1, float y1) {
    GfxCmd *c = &L->cmds[L->dl.ncmds++];
    c->first_index = (u32)L->dl.nindices;
    c->index_count = 0;
    c->texture = texture;
    c->clip_x0 = x0; c->clip_y0 = y0; c->clip_x1 = x1; c->clip_y1 = y1;
}

static void demo_index(DemoList *L, u32 i) {
    L->indices[L->dl.nindices++] = i;
    L->cmds[L->dl.ncmds - 1].index_count++;
}

/* Axis-aligned quad with texture coordinates (u0,v0)-(u1,v1). */
static void demo_quad(DemoList *L, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, u32 rgba) {
    u32 b = (u32)L->dl.nverts;
    demo_vertex(L, x0, y0, u0, v0, rgba);
    demo_vertex(L, x1, y0, u1, v0, rgba);
    demo_vertex(L, x1, y1, u1, v1, rgba);
    demo_vertex(L, x0, y1, u0, v1, rgba);
    demo_index(L, b); demo_index(L, b + 1); demo_index(L, b + 2);
    demo_index(L, b); demo_index(L, b + 2); demo_index(L, b + 3);
}

static void demo_triangle(DemoList *L, float cx, float cy, float r, float angle, u32 rgba) {
    u32 b = (u32)L->dl.nverts;
    for (int k = 0; k < 3; k++) {
        float a = angle + (float)k * 2.0943951f;
        demo_vertex(L, cx + r * cosf(a), cy + r * sinf(a), 0.0f, 0.0f, rgba);   /* uv 0: white atlas texel */
        demo_index(L, b + (u32)k);
    }
}

/* Runs the demo for `frames` frames, or for `seconds` when > 0. Returns the number
   of frames drawn, -2 when no window can be opened (no display), -1 on errors. */
static int gfx_demo_run(int frames, double seconds, bool validation, char *err, int errcap) {
    gfx_preinit_loader();
    if (!glfwInit()) { snprintf(err, (size_t)errcap, "glfwInit failed (no display?)"); return -2; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *win = glfwCreateWindow(640, 400, "FractalStreamC gfx demo", NULL, NULL);
    if (!win) { glfwTerminate(); snprintf(err, (size_t)errcap, "glfwCreateWindow failed"); return -2; }

    GfxInitDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.glfw_window = win;
    desc.vsync = true;
    desc.validation = validation;
    int drawn = -1, viewer = 0, target = 0, tw = 0, th = 0, check_viewer = 0, check = 0;
    if (!gfx_init(&desc, err, errcap)) goto done;
    viewer = gfx_viewer_create(gfx_test_spv_demo_mandel_frag, ARRAY_COUNT(gfx_test_spv_demo_mandel_frag), err, errcap);
    check_viewer = gfx_viewer_create(gfx_test_spv_test_viewer_frag, ARRAY_COUNT(gfx_test_spv_test_viewer_frag), err, errcap);
    check = gfx_target_create(8, 4);
    if (!viewer || !check_viewer || !check) goto done;

    drawn = 0;
    bool resized = false;
    double t0 = glfwGetTime();
    for (int i = 0; seconds > 0 ? glfwGetTime() - t0 < seconds : i < frames; i++) {
        glfwPollEvents();
        if (glfwWindowShouldClose(win)) break;
        if (!resized && (seconds > 0 ? glfwGetTime() - t0 > seconds * 0.5 : i >= frames / 2)) {
            glfwSetWindowSize(win, 800, 500);
            resized = true;
        }
        if (!gfx_frame_begin()) continue;
        int fw, fh;
        gfx_framebuffer_size(&fw, &fh);
        if (fw != tw || fh != th) {   /* one target covering the window */
            gfx_target_destroy(target);
            target = gfx_target_create(fw, fh);
            tw = fw;
            th = fh;
        }
        /* Frame-path check (read back after the loop): two globals versions in one
           frame, the second one rendering only the right half of `check`. */
        struct { float k; u32 n; } g1 = { 0.25f, 0 }, g2 = { 0.75f, 0 };   /* test_viewer.frag */
        GfxViewerParams cp;
        memset(&cp, 0, sizeof cp);
        cp.width = 8;
        cp.height = 4;
        gfx_set_globals(&g1, sizeof g1);
        gfx_frame_render_viewer(check, check_viewer, &cp);
        gfx_set_globals(&g2, sizeof g2);
        cp.x0 = 4; cp.y0 = 0; cp.x1 = 8; cp.y1 = 4;
        gfx_frame_render_viewer(check, check_viewer, &cp);

        float t = (float)(glfwGetTime() - t0);
        DemoGlobals g = { { -0.6f, 0.0f }, 1.3f, t };
        gfx_set_globals(&g, sizeof g);
        GfxViewerParams p;
        memset(&p, 0, sizeof p);
        p.width = (float)fw;
        p.height = (float)fh;
        gfx_frame_render_viewer(target, viewer, &p);

        static DemoList L;
        memset(&L, 0, sizeof L);
        L.dl.verts = L.verts; L.dl.indices = L.indices; L.dl.cmds = L.cmds;
        demo_cmd(&L, target, 0, 0, (float)fw, (float)fh);             /* the viewer image */
        demo_quad(&L, 0, 0, (float)fw, (float)fh, 0, 0, 1, 1, 0xFFFFFFFFu);
        demo_cmd(&L, 0, 0, 0, (float)fw, (float)fh);                   /* translucent triangles */
        demo_triangle(&L, 120, 120, 90, t, 0xA00000FFu);                 /* red */
        demo_triangle(&L, 180, 150, 90, -t, 0xA000FF00u);                /* green */
        demo_triangle(&L, 150, 200, 90, 0.5f * t, 0xA0FF0000u);          /* blue */
        demo_cmd(&L, 0, (float)fw - 200, 20, (float)fw - 20, 60);        /* clipped: only the box shows */
        demo_quad(&L, (float)fw - 400, 0, (float)fw, 200, 0, 0, 0, 0, 0xC0FFFFFFu);
        static const float clear[3] = { 0.1f, 0.1f, 0.12f };
        gfx_frame_draw(&L.dl, clear);
        gfx_frame_end();
        drawn++;
    }
    if (drawn > 0) {   /* left half rendered with k = 0.25, right half with k = 0.75 */
        u8 px[8 * 4 * 4];
        if (!gfx_target_read(check, px, err, errcap)) drawn = -1;
        for (int i = 0; drawn > 0 && i < 8 * 4; i++) {
            int x = i % 8, b = px[i * 4 + 2], want = x < 4 ? 64 : 191;
            if (abs(b - want) > 1) {
                snprintf(err, (size_t)errcap, "frame render check: pixel (%d,%d) blue = %d, expected %d", x, i / 8, b, want);
                drawn = -1;
            }
        }
    }
done:
    gfx_shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return drawn;
}

#ifdef GFX_DEMO_MAIN
int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 3.0;
    char err[512] = "";
    int n = gfx_demo_run(0, seconds, true, err, sizeof err);
    if (n < 0) { fprintf(stderr, "gfx_demo: %s\n", err); return 1; }
    printf("gfx_demo: %d frames, validation errors: %d\n", n, gfx_validation_errors());
    return gfx_validation_errors() > 0;
}
#endif
