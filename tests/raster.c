/* raster.c - a tiny CPU rasterizer for GfxDrawList, used by the UI tests to look at (and check) what the UI
 * produces without a GPU.  It mimics the real renderer: triangles with the top-left fill rule, per-vertex
 * colors times a bilinearly sampled texture (clamp to edge), straight-alpha blending src*a + dst*(1-a), and the
 * per-command scissor rectangle.  Included by test_ui.c / test_editor.c (the unity build makes it available). */
#ifndef FS_TEST_RASTER_C
#define FS_TEST_RASTER_C

typedef struct RasterImage {
    int w, h;
    u32 *px; /* RGBA, red in the low byte */
} RasterImage;

typedef struct RasterTexture {
    int w, h;
    const u8 *rgba;
} RasterTexture;

static RasterImage raster_new(int w, int h, u32 clear) {
    RasterImage im = { w, h, xmalloc((size_t)w * (size_t)h * 4) };
    for (int i = 0; i < w * h; i++) im.px[i] = clear;
    return im;
}

static void raster_free(RasterImage *im) {
    free(im->px);
    im->px = NULL;
}

static void raster_sample(const RasterTexture *t, float u, float v, float out[4]) {
    float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    for (int c = 0; c < 4; c++) out[c] = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int x = FS_CLAMP(x0 + i, 0, t->w - 1), y = FS_CLAMP(y0 + j, 0, t->h - 1);
            float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const u8 *p = t->rgba + ((size_t)y * (size_t)t->w + (size_t)x) * 4;
            for (int c = 0; c < 4; c++) out[c] += wgt * (float)p[c] / 255.0f;
        }
    }
}

static float raster_edge(const GfxVertex *a, const GfxVertex *b, float px, float py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Interior is on the positive side for clockwise-on-screen triangles; top edges run right, left edges run up. */
static bool raster_top_left(const GfxVertex *a, const GfxVertex *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    return (dy == 0 && dx > 0) || dy < 0;
}

static void raster_tri(RasterImage *im, const GfxCmd *cmd, const RasterTexture *tex, GfxVertex a, GfxVertex b, GfxVertex c) {
    float area = raster_edge(&a, &b, c.x, c.y);
    if (area == 0) return;
    if (area < 0) { GfxVertex t = b; b = c; c = t; area = -area; }
    int x0 = (int)floorf(FS_MIN(a.x, FS_MIN(b.x, c.x))), x1 = (int)ceilf(FS_MAX(a.x, FS_MAX(b.x, c.x)));
    int y0 = (int)floorf(FS_MIN(a.y, FS_MIN(b.y, c.y))), y1 = (int)ceilf(FS_MAX(a.y, FS_MAX(b.y, c.y)));
    x0 = FS_MAX(x0, FS_MAX(0, (int)floorf(cmd->clip_x0)));
    y0 = FS_MAX(y0, FS_MAX(0, (int)floorf(cmd->clip_y0)));
    x1 = FS_MIN(x1, FS_MIN(im->w, (int)ceilf(cmd->clip_x1)));
    y1 = FS_MIN(y1, FS_MIN(im->h, (int)ceilf(cmd->clip_y1)));
    bool tl0 = raster_top_left(&b, &c), tl1 = raster_top_left(&c, &a), tl2 = raster_top_left(&a, &b);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = raster_edge(&b, &c, px, py), w1 = raster_edge(&c, &a, px, py), w2 = raster_edge(&a, &b, px, py);
            if (!(w0 > 0 || (w0 == 0 && tl0)) || !(w1 > 0 || (w1 == 0 && tl1)) || !(w2 > 0 || (w2 == 0 && tl2))) continue;
            float l0 = w0 / area, l1 = w1 / area, l2 = w2 / area;
            float src[4];
            for (int ch = 0; ch < 4; ch++) {
                int s = ch * 8;
                src[ch] = (l0 * (float)((a.rgba >> s) & 255) + l1 * (float)((b.rgba >> s) & 255) + l2 * (float)((c.rgba >> s) & 255)) / 255.0f;
            }
            if (tex) {
                float t[4];
                raster_sample(tex, l0 * a.u + l1 * b.u + l2 * c.u, l0 * a.v + l1 * b.v + l2 * c.v, t);
                for (int ch = 0; ch < 4; ch++) src[ch] *= t[ch];
            }
            u32 *dst = &im->px[y * im->w + x];
            u32 out = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                float d = (float)((*dst >> (ch * 8)) & 255) / 255.0f;
                float v = src[ch] * src[3] + d * (1.0f - src[3]);
                out |= (u32)(FS_CLAMP(v, 0.0f, 1.0f) * 255.0f + 0.5f) << (ch * 8);
            }
            *dst = out;
        }
    }
}

/* Draw `dl`.  Texture 0 is the font atlas; ids 1..ntargets index `targets`. */
static void raster_draw(RasterImage *im, const GfxDrawList *dl, const RasterTexture *atlas, const RasterTexture *targets, int ntargets) {
    for (int c = 0; c < dl->ncmds; c++) {
        const GfxCmd *cmd = &dl->cmds[c];
        const RasterTexture *tex = cmd->texture == 0 ? atlas : (cmd->texture <= ntargets ? &targets[cmd->texture - 1] : NULL);
        for (u32 i = 0; i + 2 < cmd->index_count; i += 3) {
            const u32 *ix = &dl->indices[cmd->first_index + i];
            raster_tri(im, cmd, tex, dl->verts[ix[0]], dl->verts[ix[1]], dl->verts[ix[2]]);
        }
    }
}

/* Save (a zoomed crop of) the image as PNG; zoom is an integer nearest-neighbor factor. */
static bool raster_save(const RasterImage *im, int cx, int cy, int cw, int ch, int zoom, const char *path) {
    cx = FS_CLAMP(cx, 0, im->w - 1);
    cy = FS_CLAMP(cy, 0, im->h - 1);
    cw = FS_MIN(cw, im->w - cx);
    ch = FS_MIN(ch, im->h - cy);
    int ow = cw * zoom, oh = ch * zoom;
    u32 *out = xmalloc((size_t)ow * (size_t)oh * 4);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) out[y * ow + x] = im->px[(cy + y / zoom) * im->w + cx + x / zoom];
    int ok = stbi_write_png(path, ow, oh, 4, out, ow * 4);
    free(out);
    return ok != 0;
}

static u32 raster_pixel(const RasterImage *im, int x, int y) { return im->px[y * im->w + x]; }

/* Absolute difference of the largest channel, for tolerant color checks. */
static int raster_color_dist(u32 a, u32 b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        m = FS_MAX(m, d < 0 ? -d : d);
    }
    return m;
}

#endif
