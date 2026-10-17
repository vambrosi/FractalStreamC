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

/* ---- compute (storage buffers, compute pipelines, batches) ----------------
 * Shaders: tests/shaders/compute_*.comp.glsl (see their headers). */

static int gt_compute(const u32 *spv, u32 nwords, int nbuffers, u32 push_bytes) {
    GfxComputeDesc d;
    memset(&d, 0, sizeof d);
    d.spirv = spv;
    d.nwords = nwords;
    d.nbuffers = nbuffers;
    d.push_bytes = push_bytes;
    char err[256] = "";
    int p = gfx_compute_create(&d, err, sizeof err);
    CHECK_MSG(p > 0, "gfx_compute_create: %s", err);
    return p;
}
#define GT_COMPUTE(name, nbuffers, push_bytes) \
    gt_compute(gfx_test_spv_##name##_comp, (u32)ARRAY_COUNT(gfx_test_spv_##name##_comp), nbuffers, push_bytes)

/* Downloads n u32 words at word `first` of buf into a new array (NULL on failure). */
static u32 *gt_download(int buf, u32 first, u32 n) {
    u32 *out = (u32 *)xcalloc(n ? n : 1, 4);
    char err[256] = "";
    bool ok = gfx_buffer_download(buf, (u64)first * 4, out, (u64)n * 4, err, sizeof err);
    CHECK_MSG(ok, "gfx_buffer_download: %s", err);
    if (!ok) { free(out); return NULL; }
    return out;
}

/* Mismatches of v[0..n) against want(i) = mul * (i + first) + add; prints the first. */
static int gt_bad_words(const u32 *v, u32 n, u32 first, u32 mul, u32 add) {
    int bad = 0;
    for (u32 i = 0; i < n; i++) {
        u32 want = mul * (i + first) + add;
        if (v[i] != want && bad++ == 0) printf("    word %u = %u, expected %u\n", i + first, v[i], want);
    }
    return bad;
}

/* The ring test's table, computed on the CPU (see compute_chain.comp.glsl). */
static void gt_chain_cpu(u32 *v, u32 w, u32 rows) {
    for (u32 k = 1; k < rows; k++)
        for (u32 i = 0; i < w; i++)
            v[k * w + i] = v[(k - 1) * w + i] * 1664525u + v[(k - 1) * w + (i + 1) % w] + 1013904223u;
}

TEST(compute_dispatch) {
    GT_REQUIRE();
    printf("    max storage buffer: %llu bytes\n", (unsigned long long)gfx_max_buffer_size());
    CHECK(gfx_max_buffer_size() >= (u64)64 << 20);
    int pipe = GT_COMPUTE(compute_index, 1, 8);
    enum { N = 1000, CAP = 1100 };
    int buf = gfx_buffer_create(CAP * 4 - 2);   /* rounded up to a multiple of 4 */
    CHECK(buf > 0);
    CHECK_INT(gfx_buffer_size(buf), CAP * 4);
    if (!pipe || !buf) return;
    u32 *v = gt_download(buf, 0, CAP);         /* zero-filled */
    CHECK(v && gt_bad_words(v, CAP, 0, 0, 0) == 0);
    free(v);

    u32 push[2] = { 7, N };
    char err[256] = "";
    CHECK(gfx_batch_begin());
    gfx_frame_dispatch(pipe, &buf, push, (N + 63) / 64, 1, 1);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    v = gt_download(buf, 0, CAP);
    CHECK(v && gt_bad_words(v, N, 0, 3, 7) == 0);           /* written */
    CHECK(v && gt_bad_words(v + N, CAP - N, N, 0, 0) == 0); /* beyond n: untouched */
    free(v);
    v = gt_download(buf, 100, 10);                          /* a piece at an offset */
    CHECK(v && gt_bad_words(v, 10, 100, 3, 7) == 0);
    free(v);

    /* Upload at an unaligned byte offset, then read back around it. */
    u8 bytes[7] = { 1, 2, 3, 4, 5, 6, 7 };
    CHECK_MSG(gfx_buffer_upload(buf, 4 * N + 1, bytes, sizeof bytes, err, sizeof err), "%s", err);
    u8 back[12];
    CHECK_MSG(gfx_buffer_download(buf, 4 * N, back, sizeof back, err, sizeof err), "%s", err);
    CHECK(back[0] == 0 && memcmp(back + 1, bytes, 7) == 0 && back[8] == 0);
    CHECK(gfx_buffer_upload(buf, 0, NULL, 0, err, sizeof err));     /* nothing to do */

    /* Uploads and downloads larger than one staging chunk. */
    u32 big_n = (u32)(GFX_TRANSFER_CHUNK / 4) * 2 + 5;
    int big = gfx_buffer_create((u64)big_n * 4);
    u32 *src = (u32 *)xmalloc((size_t)big_n * 4);
    for (u32 i = 0; i < big_n; i++) src[i] = i * 2654435761u;
    CHECK_MSG(big && gfx_buffer_upload(big, 0, src, (u64)big_n * 4, err, sizeof err), "%s", err);
    v = big ? gt_download(big, 0, big_n) : NULL;
    CHECK(v && gt_bad_words(v, big_n, 0, 2654435761u, 0) == 0);
    free(v);
    free(src);
    gfx_buffer_destroy(big);
    gfx_buffer_destroy(buf);
    gfx_compute_destroy(pipe);
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_chain) {
    GT_REQUIRE();
    /* Ring-style dependencies: ROWS-1 dispatches in one batch, each reading the
       row the previous one wrote. Correct only if every dispatch sees the last. */
    enum { W = 100, ROWS = 1500 };
    int pipe = GT_COMPUTE(compute_chain, 2, 8);
    int buf = gfx_buffer_create((u64)W * ROWS * 4);
    int bufs[2] = { buf, buf };
    if (!pipe || !buf) return;
    u32 *want = (u32 *)xcalloc((size_t)W * ROWS, 4);
    for (u32 i = 0; i < W; i++) want[i] = i * 7u + 1u;
    char err[256] = "";
    CHECK_MSG(gfx_buffer_upload(buf, 0, want, W * 4, err, sizeof err), "%s", err);
    for (u32 i = 0; i < W / 2; i++) want[i] = 0xFFFF0000u;   /* the fill below */
    gt_chain_cpu(want, W, ROWS);

    double t0 = fs_time_seconds();
    CHECK(gfx_batch_begin());
    gfx_frame_fill_buffer(buf, W * 4, (u64)W * (ROWS - 1) * 4, 0xDEADBEEFu);   /* garbage to overwrite */
    gfx_frame_fill_buffer(buf, 0, (W / 2) * 4, 0xFFFF0000u);
    for (u32 k = 1; k < ROWS; k++) {
        u32 push[2] = { k, W };
        gfx_frame_dispatch(pipe, bufs, push, (W + 31) / 32, 1, 1);
    }
    double t1 = fs_time_seconds();
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    printf("    %d dispatches: recorded in %.2f ms, ran in %.1f ms\n", ROWS - 1, (t1 - t0) * 1e3,
           (fs_time_seconds() - t1) * 1e3);
    /* One descriptor set for all those dispatches (white box: the batch's cache). */
    CHECK_INT(gx.batch.csets.len, 1);
    u32 *v = gt_download(buf, 0, W * ROWS);
    int bad = 0;
    for (u32 i = 0; v && i < W * ROWS; i++)
        if (v[i] != want[i] && bad++ == 0) printf("    row %u col %u = %u, expected %u\n", i / W, i % W, v[i], want[i]);
    CHECK_INT(bad, 0);
    free(v);
    free(want);
    gfx_buffer_destroy(buf);
    gfx_compute_destroy(pipe);
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_buffers) {
    GT_REQUIRE();
    /* Two buffers, ping-pong: a -> b, b -> a, ... with the globals changing size
       every time, so each dispatch needs a new descriptor set (globals range) and
       the batch's descriptor pools and globals buffer grow on the way. */
    enum { N = 300, STEPS = 150 };
    int pipe = GT_COMPUTE(compute_add, 2, 8);
    int a = gfx_buffer_create(N * 4), b = gfx_buffer_create(N * 4);
    if (!pipe || !a || !b) return;
    u32 init[N];
    for (u32 i = 0; i < N; i++) init[i] = i * 10u;
    char err[256] = "";
    CHECK(gfx_buffer_upload(a, 0, init, sizeof init, err, sizeof err));
    u8 *globals = (u8 *)xcalloc(1, 64 * STEPS);
    CHECK(gfx_batch_begin());
    for (u32 s = 0; s < STEPS; s++) {
        gfx_set_globals(globals, 64 * (s + 1));
        int ab[2] = { a, b }, ba[2] = { b, a };
        u32 push[2] = { s + 1, N };
        gfx_frame_dispatch(pipe, s % 2 ? ba : ab, push, (N + 63) / 64, 1, 1);
    }
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK(gx.batch.cpools.len >= 2);   /* more sets than one pool holds */
    /* a ends with init + 1 + 2 + ... + STEPS (STEPS even: the last step wrote a). */
    u32 *v = gt_download(a, 0, N);
    CHECK(v && gt_bad_words(v, N, 0, 10, STEPS * (STEPS + 1) / 2) == 0);
    free(v);
    v = gt_download(b, 0, N);
    CHECK(v && gt_bad_words(v, N, 0, 10, (STEPS - 1) * STEPS / 2) == 0);
    free(v);

    /* The same buffer twice (in-place add) and the next batch reusing the pools. */
    CHECK(gfx_batch_begin());
    int aa[2] = { a, a };
    u32 push[2] = { 1000, N };
    for (int i = 0; i < 3; i++) gfx_frame_dispatch(pipe, aa, push, (N + 63) / 64, 1, 1);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK_INT(gx.batch.csets.len, 1);
    v = gt_download(a, 0, N);
    CHECK(v && gt_bad_words(v, N, 0, 10, STEPS * (STEPS + 1) / 2 + 3000) == 0);
    free(v);
    free(globals);
    gt_set_k(0.0f);
    gfx_buffer_destroy(a);
    gfx_buffer_destroy(b);
    gfx_compute_destroy(pipe);
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_target_copies) {
    GT_REQUIRE();
    enum { W = 13, H = 7, OFF = 64 };
    int t = gfx_target_create(W, H), t2 = gfx_target_create(W, H);
    int p = gfx_buffer_create(OFF + W * H * 4), q = gfx_buffer_create(16 + W * H * 4);
    int v = gt_viewer();
    if (!t || !t2 || !p || !q || !v) return;
    char err[256] = "";
    /* buffer -> target: a pattern uploaded at an offset. */
    u8 pat[W * H * 4], img[W * H * 4], img2[W * H * 4];
    for (int i = 0; i < W * H * 4; i++) pat[i] = (u8)(i * 37 + 11);
    CHECK(gfx_buffer_upload(p, OFF, pat, sizeof pat, err, sizeof err));
    CHECK(gfx_batch_begin());
    gfx_frame_buffer_to_target(p, OFF, t);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK_MSG(gfx_target_read(t, img, err, sizeof err), "%s", err);
    CHECK(memcmp(img, pat, sizeof pat) == 0);

    /* target -> buffer: a viewer picture. */
    gt_set_k(0.5f);
    GfxViewerParams vp = gt_params(W, H);
    CHECK_MSG(gfx_target_render(t2, v, &vp, err, sizeof err), "%s", err);
    CHECK(gfx_target_read(t2, img2, err, sizeof err));
    CHECK_INT(gt_bad_viewer(img2, W, 0, 0, W, H, W, H, 0, 0, 0.5f), 0);
    CHECK(gfx_batch_begin());
    gfx_frame_target_to_buffer(t2, q, 16);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK(gfx_buffer_download(q, 16, img, sizeof img, err, sizeof err));
    CHECK(memcmp(img, img2, sizeof img) == 0);

    /* Round trip in one batch, then a viewer render on top of the copied picture
       (render pass after a copy) and a draw of the target (sampling after a copy). */
    CHECK(gfx_batch_begin());
    gfx_frame_target_to_buffer(t2, p, 0);
    gfx_frame_fill_buffer(p, 0, 4, 0xFF0000FFu);   /* pixel (0,0): opaque red */
    gfx_frame_buffer_to_target(p, 0, t);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK(gfx_target_read(t, img, err, sizeof err));
    CHECK_INT(gt_bad_color(img, W, 0, 0, 1, 1, 255, 0, 0, 255), 0);
    CHECK(memcmp(img + 4, img2 + 4, sizeof img - 4) == 0);
    vp.x0 = 0; vp.y0 = 0; vp.x1 = 4; vp.y1 = H;   /* left 4 columns: k = 1 */
    gt_set_k(1.0f);
    CHECK(gfx_target_render(t, v, &vp, err, sizeof err));
    CHECK(gfx_target_read(t, img, err, sizeof err));
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, 4, H, W, H, 0, 0, 1.0f), 0);
    CHECK_INT(gt_bad_viewer(img, W, 4, 0, W, H, W, H, 0, 0, 0.5f), 0);

    /* A compute-made picture: compute_color -> buffer -> target. */
    int pipe = GT_COMPUTE(compute_color, 1, 12);
    gt_set_k(0.25f);
    u32 push[3] = { W, H, OFF / 4 };
    CHECK(gfx_batch_begin());
    gfx_frame_dispatch(pipe, &p, push, (W + 7) / 8, (H + 7) / 8, 1);
    gfx_frame_buffer_to_target(p, OFF, t);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK(gfx_target_read(t, img, err, sizeof err));
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, 0.25f), 0);
    gfx_compute_destroy(pipe);
    gfx_viewer_destroy(v);
    gfx_target_destroy(t);
    gfx_target_destroy(t2);
    gfx_buffer_destroy(p);
    gfx_buffer_destroy(q);
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_globals) {
    GT_REQUIRE();
    /* Two globals versions in one batch: each dispatch sees the globals current at its call. */
    enum { W = 40, H = 3 };
    int pipe = GT_COMPUTE(compute_color, 1, 12);
    int p = gfx_buffer_create(2 * W * H * 4), t = gfx_target_create(W, H);
    if (!pipe || !p || !t) return;
    GtGlobals g;
    memset(&g, 0, sizeof g);
    char err[256] = "";
    CHECK(gfx_batch_begin());
    g.k = 0.25f;
    gfx_set_globals(&g, 8);
    u32 push[3] = { W, H, 0 };
    gfx_frame_dispatch(pipe, &p, push, (W + 7) / 8, 1, 1);
    g.k = 0.75f;
    g.n = W;
    for (int x = 0; x < W; x++) g.data[x] = (float)(x * 5) / 255.0f;
    gfx_set_globals(&g, sizeof g);
    push[2] = W * H;
    gfx_frame_dispatch(pipe, &p, push, (W + 7) / 8, 1, 1);
    gfx_frame_buffer_to_target(p, W * H * 4, t);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    u8 img[2 * W * H * 4];
    CHECK(gfx_buffer_download(p, 0, img, sizeof img, err, sizeof err));
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, 0.25f), 0);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int want[4] = { gt_unorm((x + 0.5f) / W), gt_unorm((y + 0.5f) / H), gt_unorm(0.75f), x * 5 };
            gt_pixel(img + W * H * 4, W, x, y, want, &bad);
        }
    CHECK_INT(bad, 0);
    u8 timg[W * H * 4];
    CHECK(gfx_target_read(t, timg, err, sizeof err));
    CHECK(memcmp(timg, img + W * H * 4, sizeof timg) == 0);
    gt_set_k(0.0f);
    gfx_compute_destroy(pipe);
    gfx_buffer_destroy(p);
    gfx_target_destroy(t);
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_lifetime) {
    GT_REQUIRE();
    /* Destroy a buffer and a pipeline right after recording a dispatch that uses
       them (deferred until the batch ran), and create new ones that get the same
       handles within the same batch: the new ones must be used. */
    enum { N = 64 };
    int pipe = GT_COMPUTE(compute_index, 1, 8);
    int x = gfx_buffer_create(N * 4);
    if (!pipe || !x) return;
    char err[256] = "";
    u32 push[2] = { 1, N };
    CHECK(gfx_batch_begin());
    gfx_frame_dispatch(pipe, &x, push, 1, 1, 1);
    gfx_buffer_destroy(x);
    gfx_compute_destroy(pipe);
    int y = gfx_buffer_create(N * 4);
    int pipe2 = GT_COMPUTE(compute_index, 1, 8);
    CHECK_INT(y, x);           /* handle reused */
    CHECK_INT(pipe2, pipe);
    push[0] = 5;
    gfx_frame_dispatch(pipe2, &y, push, 1, 1, 1);
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK_INT(gx.batch.csets.len, 2);
    u32 *v = gt_download(y, 0, N);
    CHECK(v && gt_bad_words(v, N, 0, 3, 5) == 0);
    free(v);
    gfx_buffer_destroy(y);
    gfx_compute_destroy(pipe2);
    CHECK_INT(gx.garbage.len, 0);   /* nothing in flight: all destroyed */
    GT_NO_VALIDATION_ERRORS();
}

TEST(compute_errors) {
    GT_REQUIRE();
    char err[256] = "";
    /* Buffers. */
    CHECK(gfx_buffer_create(0) == 0);
    CHECK(gfx_buffer_create(gfx_max_buffer_size() + 1) == 0);
    CHECK(strstr(gx.err, "maxStorageBufferRange") != NULL);
    CHECK(gfx_buffer_size(0) == 0 && gfx_buffer_size(-3) == 0 && gfx_buffer_size(999) == 0);
    gfx_buffer_destroy(0);
    gfx_buffer_destroy(-1);
    gfx_buffer_destroy(999);
    int buf = gfx_buffer_create(64);
    u8 data[64] = { 0 };
    CHECK(!gfx_buffer_upload(buf, 60, data, 8, err, sizeof err));
    CHECK(strstr(err, "outside") != NULL);
    CHECK(!gfx_buffer_download(buf, 65, data, 0, err, sizeof err));
    CHECK(!gfx_buffer_download(buf, 0, NULL, 4, err, sizeof err));
    CHECK(!gfx_buffer_download(999, 0, data, 4, err, sizeof err));
    CHECK(strstr(err, "invalid storage buffer") != NULL);

    /* Pipelines. */
    CHECK(gfx_compute_create(NULL, err, sizeof err) == 0);
    GfxComputeDesc d;
    memset(&d, 0, sizeof d);
    d.spirv = gfx_test_spv_test_viewer_frag;       /* a fragment shader */
    d.nwords = (u32)ARRAY_COUNT(gfx_test_spv_test_viewer_frag);
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    CHECK(strstr(err, "compute") != NULL);
    static const u32 junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    d.spirv = junk;
    d.nwords = 8;
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    CHECK(strstr(err, "SPIR-V") != NULL);
    d.spirv = gfx_test_spv_compute_index_comp;
    d.nwords = (u32)ARRAY_COUNT(gfx_test_spv_compute_index_comp);
    d.nbuffers = 9;
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    CHECK(strstr(err, "nbuffers") != NULL);
    d.nbuffers = 1;
    d.push_bytes = 132;
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    CHECK(strstr(err, "push") != NULL);
    d.push_bytes = 6;
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    /* No workgroup size: the module without its OpExecutionMode (LocalSize) and
       its OpDecorate ... BuiltIn WorkgroupSize (glslang emits both). */
    const u32 *in = gfx_test_spv_compute_index_comp;
    u32 nw = (u32)ARRAY_COUNT(gfx_test_spv_compute_index_comp), out = 5;
    u32 *mod = (u32 *)xmalloc(nw * 4);
    memcpy(mod, in, 5 * 4);
    for (u32 i = 5, len; i < nw; i += len) {
        u32 op = in[i] & 0xFFFF;
        len = in[i] >> 16;
        if (op == 16 || (op == 71 && in[i + 2] == 11 && in[i + 3] == 25)) continue;
        memcpy(mod + out, in + i, len * 4);
        out += len;
    }
    CHECK(out < nw - 4);
    d.spirv = mod;
    d.nwords = out;
    d.push_bytes = 8;
    CHECK(gfx_compute_create(&d, err, sizeof err) == 0);
    CHECK(strstr(err, "LocalSize") != NULL);
    free(mod);
    gfx_compute_destroy(0);
    gfx_compute_destroy(-1);
    gfx_compute_destroy(999);

    /* Recording outside a frame or batch: ignored with a message. */
    int pipe = GT_COMPUTE(compute_index, 1, 8);
    gfx_frame_dispatch(pipe, &buf, NULL, 1, 1, 1);
    gfx_frame_fill_buffer(buf, 0, 4, 1);
    gfx_frame_buffer_to_target(buf, 0, 1);
    CHECK(!gfx_batch_end(err, sizeof err));          /* no batch */

    /* Errors inside a batch: the valid commands run, gfx_batch_end reports the first error. */
    int t = gfx_target_create(8, 8);                 /* needs 256 bytes, buf has 64 */
    CHECK(gfx_batch_begin());
    CHECK(!gfx_batch_begin());                       /* already open */
    gfx_frame_dispatch(12345, &buf, NULL, 1, 1, 1);
    gfx_frame_fill_buffer(buf, 2, 4, 1);             /* unaligned */
    gfx_frame_buffer_to_target(buf, 0, t);           /* too small */
    gfx_frame_target_to_buffer(t, 777, 0);           /* invalid buffer */
    u32 push[2] = { 9, 16 };
    gfx_frame_dispatch(pipe, &buf, push, 1, 0, 1);   /* no workgroups: nothing */
    gfx_frame_dispatch(pipe, &buf, push, 1, 1, 0xFFFFFFFFu);  /* over the limit */
    gfx_frame_dispatch(pipe, NULL, push, 1, 1, 1);   /* no buffers */
    gfx_frame_dispatch(pipe, &buf, push, 1, 1, 1);   /* valid */
    CHECK(!gfx_batch_end(err, sizeof err));
    CHECK(strstr(err, "12345") != NULL && strstr(err, "5 more") != NULL);
    u32 *v = gt_download(buf, 0, 16);
    CHECK(v && gt_bad_words(v, 16, 0, 3, 9) == 0);
    free(v);
    CHECK(gfx_batch_begin());                        /* an empty batch is fine */
    CHECK_MSG(gfx_batch_end(err, sizeof err), "%s", err);
    CHECK(!gfx_frame_begin());                       /* headless */
    gfx_target_destroy(t);
    gfx_buffer_destroy(buf);
    gfx_compute_destroy(pipe);
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

TEST(compute_window) {
    /* Compute in windowed frames: every frame recomputes the ring table and the
       compute picture, copies it into a target and draws that; a buffer and a
       pipeline are destroyed mid-frame (and recreated with the same handles), and
       another buffer between frames, while frames are in flight. The test viewer,
       rendered in the same frames, draws the same picture as compute_color.
       Needs a display. */
    gt_shutdown();
    gfx_preinit_loader();
    if (!glfwInit()) SKIP("no display (run under xvfb-run -a)");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *win = glfwCreateWindow(320, 200, "gfx compute test", NULL, NULL);
    if (!win) { glfwTerminate(); SKIP("no display (run under xvfb-run -a)"); }
    GfxInitDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.glfw_window = win;
    desc.vsync = true;
    desc.validation = true;
    char err[256] = "";
    enum { W = 37, H = 21, CW = 50, ROWS = 40, FRAMES = 24 };
    u32 *want = (u32 *)xcalloc(CW * ROWS, 4);
    int drawn = 0;
    if (!gfx_init(&desc, err, sizeof err)) { CHECK_MSG(false, "gfx_init: %s", err); goto done; }
    int color = GT_COMPUTE(compute_color, 1, 12), chain = GT_COMPUTE(compute_chain, 2, 8);
    int t = gfx_target_create(W, H), px = gfx_buffer_create(W * H * 4), tab = gfx_buffer_create(CW * ROWS * 4);
    int viewer = gt_viewer(), vt = gfx_target_create(W, H);
    GfxViewerParams vp = gt_params(W, H);
    if (!color || !chain || !t || !px || !tab || !viewer || !vt) goto done;
    for (u32 i = 0; i < CW; i++) want[i] = i * 3u + 2u;
    CHECK(gfx_buffer_upload(tab, 0, want, CW * 4, err, sizeof err));
    gt_chain_cpu(want, CW, ROWS);
    float last_k = -1.0f;
    for (int i = 0; i < FRAMES; i++) {
        glfwPollEvents();
        if (!gfx_frame_begin()) continue;
        float k = (float)(i % 5) / 4.0f;
        gt_set_k(k);
        gfx_frame_fill_buffer(tab, CW * 4, CW * (ROWS - 1) * 4, 0);
        for (u32 r = 1; r < ROWS; r++) {
            u32 push[2] = { r, CW };
            int tabs[2] = { tab, tab };
            gfx_frame_dispatch(chain, tabs, push, (CW + 31) / 32, 1, 1);
        }
        u32 push[3] = { W, H, 0 };
        gfx_frame_dispatch(color, &px, push, (W + 7) / 8, (H + 7) / 8, 1);
        gfx_frame_render_viewer(vt, viewer, &vp);
        gfx_frame_buffer_to_target(px, 0, t);
        if (i == 8) {
            int old_px = px, old_color = color;
            gfx_buffer_destroy(px);
            gfx_compute_destroy(color);
            px = gfx_buffer_create(W * H * 4);
            color = GT_COMPUTE(compute_color, 1, 12);
            CHECK(px == old_px && color == old_color);
            gfx_frame_dispatch(color, &px, push, (W + 7) / 8, (H + 7) / 8, 1);
            gfx_frame_buffer_to_target(px, 0, t);
        }
        int fw, fh;
        gfx_framebuffer_size(&fw, &fh);
        static DemoList L;
        memset(&L, 0, sizeof L);
        L.dl.verts = L.verts; L.dl.indices = L.indices; L.dl.cmds = L.cmds;
        demo_cmd(&L, t, 0, 0, (float)fw, (float)fh);
        demo_quad(&L, 0, 0, (float)fw, (float)fh, 0, 0, 1, 1, 0xFFFFFFFFu);
        static const float clear[3] = { 0.1f, 0.1f, 0.12f };
        gfx_frame_draw(&L.dl, clear);
        gfx_frame_end();
        last_k = k;
        drawn++;
        if (i == 16) {   /* between frames, with frames in flight */
            gfx_buffer_destroy(tab);
            tab = gfx_buffer_create(CW * ROWS * 4);
            u32 row0[CW];
            for (u32 x = 0; x < CW; x++) row0[x] = x * 3u + 2u;
            CHECK(tab && gfx_buffer_upload(tab, 0, row0, sizeof row0, err, sizeof err));
        }
    }
    CHECK_MSG(drawn >= FRAMES - 4, "only %d of %d frames drawn", drawn, FRAMES);
    u8 img[W * H * 4];
    CHECK_MSG(gfx_target_read(t, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, last_k), 0);
    CHECK_MSG(gfx_target_read(vt, img, err, sizeof err), "%s", err);
    CHECK_INT(gt_bad_viewer(img, W, 0, 0, W, H, W, H, 0, 0, last_k), 0);
    u32 *v = gt_download(tab, 0, CW * ROWS);
    CHECK(v && memcmp(v, want, CW * ROWS * 4) == 0);
    free(v);
done:
    free(want);
    gfx_shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    gt_set_k(0.0f);
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
    TESTCASE(compute_dispatch),
    TESTCASE(compute_chain),
    TESTCASE(compute_buffers),
    TESTCASE(compute_target_copies),
    TESTCASE(compute_globals),
    TESTCASE(compute_lifetime),
    TESTCASE(compute_errors),
    TESTCASE(window_smoke),
    TESTCASE(compute_window),
    TESTCASE(shutdown_reinit),
};
