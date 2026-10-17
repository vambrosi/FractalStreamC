/* test_gfx.c - tests of the Vulkan renderer (src/gfx.c).
 *
 * GPU tests are skipped when no Vulkan device is available. The window test needs
 * a display:  xvfb-run -a ./build.sh test gfx
 * Everything runs with the validation layer when it is installed, and each test
 * checks that the layer reported no errors.
 *
 * The test viewer (tests/shaders/test_viewer.frag.glsl) outputs
 *   r = (x + offset.x) / size.x,  g = (y + offset.y) / size.y,  b = k,
 *   a = data[x] if x < n else 1      (x, y = pixel centers)
 * with globals { float k; uint n; float data[]; }.
 */
#include "test_gfx_shaders.h"
#include "gfx_demo.c"

typedef struct GtGlobals { float k; u32 n; float data[256]; } GtGlobals;

static int gt_ready;            /* 0 = not tried, 1 = initialized, -1 = no device */
static char gt_why[300];

static bool gt_init(void) {
    if (gt_ready == 0) {
        GfxInitDesc d;
        memset(&d, 0, sizeof d);
        d.validation = true;
        char err[256] = "";
        if (gfx_init(&d, err, sizeof err)) gt_ready = 1;
        else { gt_ready = -1; snprintf(gt_why, sizeof gt_why, "no Vulkan device: %s", err); }
    }
    return gt_ready == 1;
}

static void gt_shutdown(void) {
    if (gt_ready == 1) gfx_shutdown();
    gt_ready = 0;
}

#define GT_REQUIRE() do { if (!gt_init()) SKIP(gt_why); } while (0)
#define GT_NO_VALIDATION_ERRORS() \
    CHECK_MSG(gfx_validation_errors() <= 0, "%d validation errors (see stderr)", gfx_validation_errors())

static int gt_viewer(void) {
    char err[256] = "";
    int v = gfx_viewer_create(gfx_test_spv_test_viewer_frag, ARRAY_COUNT(gfx_test_spv_test_viewer_frag), err, sizeof err);
    CHECK_MSG(v > 0, "gfx_viewer_create: %s", err);
    return v;
}

static void gt_set_k(float k) {
    GtGlobals g;
    memset(&g, 0, sizeof g);
    g.k = k;
    gfx_set_globals(&g, sizeof g);
}

static int gt_unorm(float f) { return (int)lrintf(FS_CLAMP(f, 0.0f, 1.0f) * 255.0f); }

static GfxViewerParams gt_params(float w, float h) {
    GfxViewerParams p;
    memset(&p, 0, sizeof p);
    p.width = w;
    p.height = h;
    return p;
}

/* Compares pixel (x,y) of img (width w) with want (tolerance 1 per channel);
   counts mismatches in *bad and prints the first one. */
static void gt_pixel(const u8 *img, int w, int x, int y, const int want[4], int *bad) {
    const u8 *p = img + ((size_t)y * w + x) * 4;
    for (int c = 0; c < 4; c++)
        if (abs((int)p[c] - want[c]) > 1) {
            if (*bad == 0)
                printf("    pixel (%d,%d) = (%d,%d,%d,%d), expected (%d,%d,%d,%d)\n",
                       x, y, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
            (*bad)++;
            return;
        }
}

/* Mismatching pixels in [x0,x1)x[y0,y1) against a constant color. */
static int gt_bad_color(const u8 *img, int w, int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
    int bad = 0, want[4] = { r, g, b, a };
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) gt_pixel(img, w, x, y, want, &bad);
    return bad;
}

/* Same against the test viewer's output for push constants (sw, sh, ox, oy) and k. */
static int gt_bad_viewer(const u8 *img, int w, int x0, int y0, int x1, int y1,
                         float sw, float sh, float ox, float oy, float k) {
    int bad = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int want[4] = { gt_unorm((x + 0.5f + ox) / sw), gt_unorm((y + 0.5f + oy) / sh), gt_unorm(k), 255 };
            gt_pixel(img, w, x, y, want, &bad);
        }
    return bad;
}

TEST(init) {
    GT_REQUIRE();
    printf("    device: %s, validation layer: %s\n", gfx_device_name(),
           gfx_validation_errors() < 0 ? "not installed" : "on");
    CHECK(gfx_device_name()[0] != 0);
    CHECK(gfx_max_image_size() >= 4096);
    CHECK(gfx_fatal_error() == NULL);
    int w = -1, h = -1;
    gfx_framebuffer_size(&w, &h);
    CHECK(w == 0 && h == 0);
    GfxInitDesc d;
    memset(&d, 0, sizeof d);
    char err[128] = "";
    CHECK(!gfx_init(&d, err, sizeof err));       /* already initialized */
    CHECK(strstr(err, "already") != NULL);
    GT_NO_VALIDATION_ERRORS();
}

TEST(render_to_memory) {
    GT_REQUIRE();
    int v = gt_viewer();
    if (!v) return;
    gt_set_k(0.25f);
    enum { W = 64, H = 48 };
    u8 *img = (u8 *)xmalloc(W * H * 4);
    char err[256] = "";
    CHECK_MSG(gfx_render_to_memory(v, W, H, 0, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, 0.25f), 0);
    CHECK(!gfx_render_to_memory(v, 0, H, 0, img, err, sizeof err));
    CHECK(!gfx_render_to_memory(v + 100, W, H, 0, img, err, sizeof err));
    CHECK(strstr(err, "viewer") != NULL);
    free(img);
    gfx_viewer_destroy(v);
    GT_NO_VALIDATION_ERRORS();
}

TEST(render_banded) {
    GT_REQUIRE();
    int v = gt_viewer();
    if (!v) return;
    gt_set_k(0.75f);
    enum { W = 61, H = 37 };
    static const int bands[] = { 0, 1, 5, 36, 37, 1000 };
    u8 *ref = (u8 *)xmalloc(W * H * 4), *img = (u8 *)xmalloc(W * H * 4);
    char err[256] = "";
    for (int i = 0; i < ARRAY_COUNT(bands); i++) {
        memset(img, 0x55, W * H * 4);
        CHECK_MSG(gfx_render_to_memory(v, W, H, bands[i], img, err, sizeof err), "%s", err);
        if (i == 0) {
            memcpy(ref, img, W * H * 4);
            CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, 0.75f), 0);
        } else {
            CHECK_MSG(memcmp(ref, img, W * H * 4) == 0, "band_rows=%d differs from a single band", bands[i]);
        }
    }
    free(ref);
    free(img);
    gfx_viewer_destroy(v);
    GT_NO_VALIDATION_ERRORS();
}

TEST(target_subregion) {
    GT_REQUIRE();
    int v = gt_viewer();
    if (!v) return;
    enum { W = 40, H = 30 };
    int t = gfx_target_create(W, H);
    CHECK(t > 0);
    u8 *img = (u8 *)xmalloc(W * H * 4);
    char err[256] = "";
    GfxViewerParams p = gt_params(W, H);
    gt_set_k(0.0f);
    CHECK_MSG(gfx_target_render(t, v, &p, err, sizeof err), "%s", err);
    /* Re-render a sub-rectangle with other globals and an offset. */
    gt_set_k(1.0f);
    p.offset_x = 3.0f;
    p.offset_y = -2.0f;
    p.x0 = 8; p.y0 = 5; p.x1 = 20; p.y1 = 17;
    CHECK_MSG(gfx_target_render(t, v, &p, err, sizeof err), "%s", err);
    CHECK_MSG(gfx_target_read(t, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 8, 5, 20, 17, W, H, 3, -2, 1.0f), 0);   /* inside */
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, 5, W, H, 0, 0, 0.0f), 0);      /* above */
    CHECK_INT(gt_bad_viewer(img, W, 0, 17, W, H, W, H, 0, 0, 0.0f), 0);     /* below */
    CHECK_INT(gt_bad_viewer(img, W, 0, 5, 8, 17, W, H, 0, 0, 0.0f), 0);     /* left */
    CHECK_INT(gt_bad_viewer(img, W, 20, 5, W, 17, W, H, 0, 0, 0.0f), 0);    /* right */
    /* A region sticking out of the target is clipped; an empty one renders nothing. */
    gt_set_k(0.5f);
    p = gt_params(W, H);
    p.x0 = 30; p.y0 = -10; p.x1 = 1000; p.y1 = 3;
    CHECK_MSG(gfx_target_render(t, v, &p, err, sizeof err), "%s", err);
    p.x0 = 0; p.y0 = 10; p.x1 = 10; p.y1 = 10;
    CHECK_MSG(gfx_target_render(t, v, &p, err, sizeof err), "%s", err);
    CHECK_MSG(gfx_target_read(t, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 30, 0, W, 3, W, H, 0, 0, 0.5f), 0);
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, 30, 3, W, H, 0, 0, 0.0f), 0);
    CHECK_INT(gt_bad_viewer(img, W, 0, 17, W, H, W, H, 0, 0, 0.0f), 0);
    free(img);
    gfx_target_destroy(t);
    gfx_viewer_destroy(v);
    GT_NO_VALIDATION_ERRORS();
}

TEST(target_lifecycle) {
    GT_REQUIRE();
    char err[256] = "";
    CHECK(gfx_target_create(0, 10) == 0);
    CHECK(gfx_target_create(gfx_max_image_size() + 1, 4) == 0);
    /* New targets are opaque black. */
    int t = gfx_target_create(5, 3);
    CHECK(t > 0);
    u8 img[5 * 3 * 4];
    memset(img, 0x77, sizeof img);
    CHECK_MSG(gfx_target_read(t, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_color(img, 5, 0, 0, 5, 3, 0, 0, 0, 255), 0);
    int w = 0, h = 0;
    CHECK(gfx_target_size(t, &w, &h) && w == 5 && h == 3);
    gfx_target_destroy(t);
    gfx_target_destroy(t);          /* twice, and invalid handles: no effect */
    gfx_target_destroy(0);
    gfx_target_destroy(-3);
    gfx_target_destroy(100000);
    CHECK(!gfx_target_size(t, &w, &h) && w == 0);
    CHECK(!gfx_target_read(t, img, err, sizeof err));
    /* Fill every slot, twice over; handles are distinct and > 0. */
    int ids[64], n = 0;
    for (int round = 0; round < 2; round++) {
        for (n = 0; n < 64; n++) {
            ids[n] = gfx_target_create(8 + n, 8);
            if (ids[n] <= 0) break;
        }
        CHECK_INT(n, 64);
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) CHECK(ids[i] != ids[j]);
        if (round == 0) CHECK(gfx_target_create(8, 8) == 0);   /* full (prints a message) */
        for (int i = 0; i < n; i++) gfx_target_destroy(ids[i]);
    }
    GT_NO_VALIDATION_ERRORS();
}

TEST(draw_list) {
    GT_REQUIRE();
    char err[256] = "";
    /* 2x2 atlas: white, red / green, transparent. */
    static const u8 atlas[16] = { 255, 255, 255, 255,  255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 0, 0 };
    CHECK(gfx_set_font_atlas(atlas, 2, 2));
    int v = gt_viewer();
    int a = gfx_target_create(64, 32), b = gfx_target_create(64, 64);
    CHECK(v > 0 && a > 0 && b > 0);
    if (!v || !a || !b) return;
    gt_set_k(0.5f);
    GfxViewerParams p = gt_params(64, 32);
    CHECK_MSG(gfx_target_render(a, v, &p, err, sizeof err), "%s", err);

    static DemoList L;
    memset(&L, 0, sizeof L);
    L.dl.verts = L.verts; L.dl.indices = L.indices; L.dl.cmds = L.cmds;
    demo_cmd(&L, 0, 0, 0, 64, 64);
    demo_quad(&L, 0, 0, 32, 32, 0.25f, 0.25f, 0.25f, 0.25f, 0x800000FFu);   /* half-transparent red */
    demo_quad(&L, 32, 0, 64, 32, 0.75f, 0.25f, 0.75f, 0.25f, 0xFFFFFFFFu);  /* atlas red texel */
    demo_cmd(&L, a, 0, 32, 32, 64);                                          /* target a, clipped */
    demo_quad(&L, 0, 32, 64, 64, 0, 0, 1, 1, 0xFFFFFFFFu);
    demo_cmd(&L, b, 0, 0, 64, 64);                                           /* samples itself: skipped */
    demo_quad(&L, 0, 0, 64, 64, 0, 0, 1, 1, 0xFFFFFFFFu);
    demo_cmd(&L, 0, 0, 0, 0, 0);                                             /* empty clip */
    demo_quad(&L, 0, 0, 64, 64, 0.25f, 0.25f, 0.25f, 0.25f, 0xFF00FF00u);
    static const float blue[3] = { 0, 0, 1 };
    CHECK_MSG(gfx_target_draw(b, &L.dl, blue, err, sizeof err), "%s", err);
    u8 *img = (u8 *)xmalloc(64 * 64 * 4), *src = (u8 *)xmalloc(64 * 32 * 4);
    CHECK_MSG(gfx_target_read(b, img, err, sizeof err), "%s", err);
    CHECK_MSG(gfx_target_read(a, src, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_color(img, 64, 0, 0, 32, 32, 128, 0, 127, 255), 0);    /* blended over blue */
    CHECK_INT(gt_bad_color(img, 64, 32, 0, 64, 32, 255, 0, 0, 255), 0);
    CHECK_INT(gt_bad_color(img, 64, 32, 32, 64, 64, 0, 0, 255, 255), 0);    /* clipped away */
    int bad = 0;   /* 1:1 texel mapping: the left half is an exact copy of a */
    for (int y = 32; y < 64; y++)
        for (int x = 0; x < 32; x++) {
            const u8 *s = src + ((y - 32) * 64 + x) * 4;
            int want[4] = { s[0], s[1], s[2], s[3] };
            gt_pixel(img, 64, x, y, want, &bad);
        }
    CHECK_INT(bad, 0);
    /* An index out of range rejects the list (prints a message); the clear still happens. */
    L.indices[0] = 1000;
    CHECK(gfx_target_draw(b, &L.dl, blue, err, sizeof err));
    CHECK(gfx_target_read(b, img, err, sizeof err));
    CHECK_INT(gt_bad_color(img, 64, 0, 0, 64, 64, 0, 0, 255, 255), 0);
    CHECK(gfx_target_draw(b, NULL, NULL, err, sizeof err));               /* clear to black */
    CHECK(gfx_target_read(b, img, err, sizeof err));
    CHECK_INT(gt_bad_color(img, 64, 0, 0, 64, 64, 0, 0, 0, 255), 0);
    static const u8 white[4] = { 255, 255, 255, 255 };
    CHECK(gfx_set_font_atlas(white, 1, 1));
    free(img);
    free(src);
    gfx_target_destroy(a);
    gfx_target_destroy(b);
    gfx_viewer_destroy(v);
    GT_NO_VALIDATION_ERRORS();
}

TEST(globals_large) {
    GT_REQUIRE();
    int v = gt_viewer();
    if (!v) return;
    /* 1 MiB of globals; the shader reads data[x] into alpha. */
    u32 n = 256 * 1024;
    size_t size = 8 + (size_t)n * 4;
    u8 *buf = (u8 *)xcalloc(1, size);
    float k = 0.0f;
    memcpy(buf, &k, 4);
    memcpy(buf + 4, &n, 4);
    for (u32 i = 0; i < n; i++) {
        float d = (float)(i % 256) / 255.0f;
        memcpy(buf + 8 + (size_t)i * 4, &d, 4);
    }
    gfx_set_globals(buf, (u32)size);
    enum { W = 256, H = 2 };
    u8 img[W * H * 4];
    char err[256] = "";
    CHECK_MSG(gfx_render_to_memory(v, W, H, 0, img, err, sizeof err), "%s", err);
    int bad = 0;
    for (int x = 0; x < W; x++) bad += abs(img[x * 4 + 3] - x) > 1;
    CHECK_INT(bad, 0);
    gt_set_k(0.0f);   /* small again: n = 0 -> alpha 1 */
    CHECK_MSG(gfx_render_to_memory(v, W, H, 0, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, 0.0f), 0);
    gfx_set_globals(NULL, 0);   /* no globals at all is valid too */
    CHECK_MSG(gfx_render_to_memory(v, W, H, 0, img, err, sizeof err), "%s", err);
    free(buf);
    gfx_viewer_destroy(v);
    GT_NO_VALIDATION_ERRORS();
}

TEST(viewer_errors) {
    GT_REQUIRE();
    char err[256] = "";
    static const u32 junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(gfx_viewer_create(junk, 8, err, sizeof err) == 0);
    CHECK(strstr(err, "SPIR-V") != NULL);
    err[0] = 0;
    CHECK(gfx_viewer_create(NULL, 0, err, sizeof err) == 0);
    CHECK(err[0] != 0);
    /* A vertex shader is not a viewer. */
    CHECK(gfx_viewer_create(gfx_spv_fullscreen_vert, ARRAY_COUNT(gfx_spv_fullscreen_vert), err, sizeof err) == 0);
    CHECK(strstr(err, "fragment") != NULL);
    /* Truncated in the middle of an instruction. */
    CHECK(gfx_viewer_create(gfx_test_spv_test_viewer_frag, 6, err, sizeof err) == 0);
    gfx_viewer_destroy(0);
    gfx_viewer_destroy(-1);
    gfx_viewer_destroy(12345);
    GfxViewerParams p = gt_params(4, 4);
    int t = gfx_target_create(4, 4);
    CHECK(!gfx_target_render(t, 999, &p, err, sizeof err));
    CHECK(strstr(err, "viewer") != NULL);
    CHECK(!gfx_target_render(999, 1, &p, err, sizeof err));
    gfx_target_destroy(t);
    GT_NO_VALIDATION_ERRORS();
}

TEST(window_smoke) {
    /* Needs its own windowed instance: close the headless one. */
    gt_shutdown();
    char err[256] = "";
    int n = gfx_demo_run(40, 0, true, err, sizeof err);
    if (n == -2) SKIP("no display (run under xvfb-run -a)");
    CHECK_MSG(n >= 30, "%d of 40 frames drawn: %s", n, err);
    GT_NO_VALIDATION_ERRORS();
}

TEST(shutdown_reinit) {
    GT_REQUIRE();
    gt_shutdown();
    CHECK(gfx_device_name()[0] == 0);
    char err[256] = "";
    u8 img[16];
    CHECK(!gfx_render_to_memory(1, 2, 2, 0, img, err, sizeof err));   /* harmless when not initialized */
    CHECK(strstr(err, "not initialized") != NULL);
    gfx_viewer_destroy(1);
    gfx_target_destroy(1);
    CHECK(gt_init());
    int v = gt_viewer();
    gt_set_k(1.0f);
    CHECK_MSG(v && gfx_render_to_memory(v, 2, 2, 0, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, 2, 0, 0, 2, 2, 2, 2, 0, 0, 1.0f), 0);
    gt_shutdown();   /* with a live viewer: shutdown destroys it */
    GT_NO_VALIDATION_ERRORS();
}

static const TestCase tests_gfx[] = {
    TESTCASE(init),
    TESTCASE(render_to_memory),
    TESTCASE(render_banded),
    TESTCASE(target_subregion),
    TESTCASE(target_lifecycle),
    TESTCASE(draw_list),
    TESTCASE(globals_large),
    TESTCASE(viewer_errors),
    TESTCASE(window_smoke),
    TESTCASE(shutdown_reinit),
};
