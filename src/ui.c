/* ui.c - fonts, draw-list builder and immediate-mode widgets.  See ui.h for the contract.
 *
 * Layout of this file:
 *   1. context and small helpers
 *   2. fonts: baking the glyph atlas (text, plus Material Icons glyphs requested on demand), measuring text
 *   3. draw lists: layers, clip stack, primitives (anti-aliased by a 1 px feathered fringe), text, images, icons
 *   4. frame begin/end, ids, hit-testing, generic interaction
 *   5. widgets
 * Private helpers are prefixed uip_ because the whole program is one translation unit. */
#include "ui.h"
#include "stb_truetype.h"
#include "../third_party/fonts/fonts_embedded.c"
#include "../third_party/fonts/material_icons_codepoints.c"

/* ======================================================================================================
 * 1. context
 * ==================================================================================================== */

#define UIP_MAX_OCC 1024
#define UIP_MAX_IDS 32
#define UIP_MAX_CLIPS 16
#define UIP_TOOLTIP_DELAY 0.5
#define UIP_BLINK_HALF_PERIOD 0.53

typedef struct UipGlyph {
    u32 cp;
    float u0, v0, u1, v1; /* atlas rectangle, normalized */
    float x0, y0, x1, y1; /* quad relative to the pen (x) and the baseline (y), pixels, y down */
    float adv;            /* advance in pixels */
} UipGlyph;

typedef struct UipFont {
    stbtt_fontinfo info;
    float logical_px;     /* requested em size, unscaled */
    float px;             /* em size in physical pixels */
    float scale;          /* stb_truetype scale for px */
    float ascent, line_h; /* whole pixels */
    bool mono;
    UipGlyph *glyphs;     /* sorted by codepoint */
    int nglyphs;
    short ascii[256];     /* glyph index for codepoints < 256, or -1 */
    int fffd;             /* glyph index of U+FFFD, or -1 */
} UipFont;

/* One Material Icons glyph at one whole-pixel size, baked on demand.  The icon font draws every icon inside its em
 * box (1 em wide and tall, the 24 dp grid of the design), so icons are placed by centring that box, not the ink.
 * g.x0..y1 are the bitmap corners relative to the TOP-LEFT of the box (not the baseline); g.adv is the box width. */
typedef struct UipIcon {
    UipGlyph g;           /* g.cp is the Material Icons codepoint */
    int px;               /* em size in whole pixels */
    float box_h;          /* height of the em box in pixels */
    bool ready;           /* baked into the current atlas (else it was requested after the last bake) */
    u64 last_used;        /* ui.icon_clock at the last draw, to evict the least recently used when there are too many */
} UipIcon;

typedef struct UipLayer {
    VEC(u32) idx;
    VEC(GfxCmd) cmds;
    float clips[UIP_MAX_CLIPS][4]; /* x0, y0, x1, y1; [0] is the whole framebuffer */
    int nclips;
} UipLayer;

typedef struct UipOcc { float x0, y0, x1, y1; int layer; } UipOcc;

/* Single-line text editing state; only one text widget has the focus at a time, so one is enough. */
typedef struct UipTextEdit {
    u32 id;               /* widget being edited (0 = none) */
    int cursor, anchor;   /* byte offsets */
    float scroll;         /* horizontal scroll, pixels */
    double epoch;         /* caret blink phase starts here */
    char *orig;           /* text when the field got the focus (restored by Escape) */
    bool dragging;
} UipTextEdit;

typedef struct UiCtx {
    UiInput in;
    bool key_used[ARRAY_COUNT(((UiInput *)0)->keys)];
    float fb_w, fb_h;
    float prev_mx, prev_my;

    /* fonts */
    UipFont fonts[UI_FONT_COUNT];
    float scale;
    u8 *atlas;
    int atlas_w, atlas_h;
    bool atlas_fresh;     /* rebuilt since ui_take_atlas_update */
    bool bake_needed;
    float white_u, white_v;

    /* icons: the Material Icons font and the icon/size pairs requested so far (see ui_icon) */
    stbtt_fontinfo icon_info;
    bool icon_font_tried, icon_font_ok;
    VEC(UipIcon) icons;
    u64 icon_clock;                   /* counts icon draws, orders them by recency */

    /* draw buffers */
    VEC(GfxVertex) verts;
    UipLayer layers[UI_LAYER_COUNT];
    int layer;
    VEC(u32) out_idx;
    VEC(GfxCmd) out_cmds;
    GfxDrawList out;

    /* ids and interaction */
    u32 id_stack[UIP_MAX_IDS];
    int nid;
    u32 active, focus;
    bool focus_claimed;               /* a widget took the focus during this frame's mouse press */
    bool active_seen, focus_seen;     /* the active / focused widget was run this frame (else it vanished) */
    u32 hover_id;
    int hover_layer;
    bool last_hovered;
    u32 hover_track_id;               /* tooltip timer */
    double hover_track_start;
    bool hover_seen;                  /* some widget was hovered this frame */
    float press_x, press_y;
    double press_time;
    bool mouse_captured;              /* the application owns the mouse this frame (e.g. panning a viewer) */
    double drag_val;                  /* unclamped value of the drag widget being dragged */
    UipOcc occ[UIP_MAX_OCC], occ_prev[UIP_MAX_OCC];
    int nocc, nocc_prev;
    UiCursor cursor;
    bool redraw_now;
    double wake_in;                   /* seconds until a timed redraw is needed */

    /* text editing */
    UipTextEdit te;
    u32 te_lost;                      /* a text field whose editing ended because another one took the focus: its next
                                         call reports UI_TEXT_BLURRED */
    u32 num_edit_id;                  /* drag widget currently in "type a number" mode */
    char num_buf[64];
    int num_flags;
} UiCtx;

static UiCtx ui;

u32 ui_alpha(u32 c, float a) {
    float k = FS_CLAMP(a, 0.0f, 1.0f);
    u32 al = (u32)((float)(c >> 24) * k + 0.5f);
    return (c & 0x00FFFFFFu) | (al << 24);
}

u32 ui_mix(u32 a, u32 b, float t) {
    u32 out = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        out |= (u32)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

int ui_utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

int ui_utf8_next(const char *s, int i) {
    if (!s[i]) return i;
    i++;
    while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static float uip_roundf(float v) { return floorf(v + 0.5f); }

/* ======================================================================================================
 * 2. fonts
 * ==================================================================================================== */

/* Codepoint ranges baked into the atlas, ascending.  Glyphs a font does not have are taken from the other one. */
static const u32 uip_ranges[][2] = {
    { 0x0020, 0x00FF }, /* Latin-1 */
    { 0x0370, 0x03FF }, /* Greek */
    { 0x2010, 0x2027 }, /* dashes, quotes, bullet, ellipsis */
    { 0x2070, 0x209F }, /* super- and subscripts */
    { 0x2100, 0x214F }, /* letterlike: complex, real, integers, quaternions */
    { 0x2190, 0x22FF }, /* arrows, mathematical operators */
    { 0x25A0, 0x25FF }, /* geometric shapes */
    { 0x2630, 0x2630 }, /* trigram for heaven: the hamburger icon */
    { 0xFFFD, 0xFFFD }, /* replacement character */
};

#define UIP_COVERAGE_GAMMA 0.80f /* <1 thickens light-on-dark text a bit; blending is done in sRGB space */

typedef struct UipBakeJob {
    UipGlyph *dst;
    const stbtt_fontinfo *info;
    float scale;
    int glyph;
    int w, h;   /* bitmap size */
    int ax, ay; /* top-left of the padded cell in the atlas */
} UipBakeJob;

static int uip_job_cmp(const void *a, const void *b) {
    const UipBakeJob *x = *(UipBakeJob *const *)a, *y = *(UipBakeJob *const *)b;
    if (x->h != y->h) return y->h - x->h;
    return y->w - x->w;
}

/* Shelf packing; cells are the bitmap plus 1 px of padding on every side.  The 4x4 block at the origin is
 * reserved for the white texel.  Returns the height used. */
static int uip_shelf_pack(UipBakeJob **order, int n, int W) {
    int x = 4, y = 0, shelf_h = 4;
    for (int i = 0; i < n; i++) {
        UipBakeJob *j = order[i];
        int cw = j->w + 2, ch = j->h + 2;
        if (x + cw > W) { y += shelf_h; x = 0; shelf_h = 0; }
        j->ax = x;
        j->ay = y;
        x += cw;
        if (ch > shelf_h) shelf_h = ch;
    }
    return y + shelf_h;
}

static int uip_glyph_cmp(const void *a, const void *b) {
    u32 x = ((const UipGlyph *)a)->cp, y = ((const UipGlyph *)b)->cp;
    return x < y ? -1 : x > y;
}

static const UipGlyph *uip_find_glyph(const UipFont *f, u32 cp) {
    if (cp < 256 && f->ascii[cp] >= 0) return &f->glyphs[f->ascii[cp]];
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 c = f->glyphs[mid].cp;
        if (c == cp) return &f->glyphs[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return f->fffd >= 0 ? &f->glyphs[f->fffd] : NULL;
}

/* ---- icon glyph bookkeeping (the drawing side is in "icons" further down) ---- */

#define UIP_ICON_MIN_PX 4
#define UIP_ICON_MAX_PX 256
#define UIP_ICON_MAX_COUNT 512      /* icon/size pairs kept in the atlas ... */
#define UIP_ICON_MAX_AREA (1 << 20) /* ... and the atlas pixels (padded cells) they may use */

static bool uip_icon_font(void) {
    if (!ui.icon_font_tried) {
        ui.icon_font_tried = true;
        ui.icon_font_ok = stbtt_InitFont(&ui.icon_info, font_icons_ttf, stbtt_GetFontOffsetForIndex(font_icons_ttf, 0)) != 0;
    }
    return ui.icon_font_ok;
}

/* Every icon/size pair ever requested stays in the atlas, so a rebuild never makes an icon disappear.  The only
 * exception is a pathological program (icons animated through hundreds of sizes): past a budget the pairs that
 * were drawn least recently are dropped, and drawn again (one more rebuild) if they are still wanted. */
static void uip_icons_trim(void) {
    long area = 0;
    for (int i = 0; i < ui.icons.len; i++) area += (long)(ui.icons.data[i].px + 2) * (long)(ui.icons.data[i].px + 2);
    while (ui.icons.len > UIP_ICON_MAX_COUNT || area > UIP_ICON_MAX_AREA) {
        int victim = 0;
        for (int i = 1; i < ui.icons.len; i++)
            if (ui.icons.data[i].last_used < ui.icons.data[victim].last_used) victim = i;
        area -= (long)(ui.icons.data[victim].px + 2) * (long)(ui.icons.data[victim].px + 2);
        ui.icons.data[victim] = ui.icons.data[--ui.icons.len];
    }
}

const u8 *ui_fonts_init(float scale, int *out_w, int *out_h) {
    if (scale < 0.5f) scale = 0.5f;
    static const unsigned char *ttfs[UI_FONT_COUNT] = { font_sans_ttf, font_mono_ttf };
    ui.scale = scale;
    for (int fi = 0; fi < UI_FONT_COUNT; fi++) {
        UipFont *f = &ui.fonts[fi];
        if (f->logical_px <= 0) f->logical_px = 15.0f;
        if (!f->info.data) stbtt_InitFont(&f->info, ttfs[fi], stbtt_GetFontOffsetForIndex(ttfs[fi], 0));
        f->mono = fi == UI_FONT_MONO;
        f->px = f->logical_px * scale;
        f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, f->px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
        f->ascent = uip_roundf((float)asc * f->scale);
        f->line_h = f->ascent + uip_roundf((float)-desc * f->scale) + uip_roundf((float)gap * f->scale);
        free(f->glyphs);
        f->glyphs = NULL;
        f->nglyphs = 0;
    }

    /* 1. collect every glyph of both fonts, then every icon/size pair requested so far */
    int cap = 0;
    for (size_t r = 0; r < ARRAY_COUNT(uip_ranges); r++) cap += (int)(uip_ranges[r][1] - uip_ranges[r][0] + 1);
    uip_icons_trim();
    UipBakeJob *jobs = xcalloc((size_t)cap * UI_FONT_COUNT + (size_t)ui.icons.len, sizeof(UipBakeJob));
    int njobs = 0;
    for (int fi = 0; fi < UI_FONT_COUNT; fi++) {
        UipFont *f = &ui.fonts[fi];
        UipFont *other = &ui.fonts[1 - fi];
        f->glyphs = xcalloc((size_t)cap, sizeof(UipGlyph));
        for (size_t r = 0; r < ARRAY_COUNT(uip_ranges); r++) {
            for (u32 cp = uip_ranges[r][0]; cp <= uip_ranges[r][1]; cp++) {
                const stbtt_fontinfo *src = &f->info;
                float s = f->scale;
                int gi = stbtt_FindGlyphIndex(src, (int)cp);
                if (!gi) { /* fall back to the other font at the same em size */
                    src = &other->info;
                    s = stbtt_ScaleForMappingEmToPixels(src, f->px);
                    gi = stbtt_FindGlyphIndex(src, (int)cp);
                    if (!gi) continue;
                }
                int adv, lsb, x0, y0, x1, y1;
                stbtt_GetGlyphHMetrics(src, gi, &adv, &lsb);
                stbtt_GetGlyphBitmapBox(src, gi, s, s, &x0, &y0, &x1, &y1);
                UipGlyph *g = &f->glyphs[f->nglyphs++];
                g->cp = cp;
                g->adv = (float)adv * s;
                if (f->mono && src == &f->info) g->adv = uip_roundf(g->adv); /* exact character grid */
                g->x0 = (float)x0;
                g->y0 = (float)y0;
                g->x1 = (float)x1;
                g->y1 = (float)y1;
                UipBakeJob *j = &jobs[njobs++];
                j->dst = g;
                j->info = src;
                j->scale = s;
                j->glyph = gi;
                j->w = x1 - x0;
                j->h = y1 - y0;
            }
        }
    }
    for (int i = 0; i < ui.icons.len && uip_icon_font(); i++) {
        UipIcon *ic = &ui.icons.data[i];
        const stbtt_fontinfo *info = &ui.icon_info;
        float s = stbtt_ScaleForMappingEmToPixels(info, (float)ic->px);
        int gi = stbtt_FindGlyphIndex(info, (int)ic->g.cp);
        int asc, desc, gap, adv = 0, lsb, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
        if (gi) { /* (a codepoint the font lacks stays an empty glyph) */
            stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
            stbtt_GetGlyphBitmapBox(info, gi, s, s, &x0, &y0, &x1, &y1);
        }
        float top = uip_roundf((float)asc * s); /* from the top of the em box down to the baseline (whole pixels) */
        ic->g.adv = gi ? (float)adv * s : (float)ic->px;
        ic->box_h = (float)(asc - desc) * s;
        ic->g.x0 = (float)x0;
        ic->g.y0 = (float)y0 + top;
        ic->g.x1 = (float)x1;
        ic->g.y1 = (float)y1 + top;
        UipBakeJob *j = &jobs[njobs++];
        j->dst = &ic->g;
        j->info = info;
        j->scale = s;
        j->glyph = gi;
        j->w = x1 - x0;
        j->h = y1 - y0;
    }

    /* 2. pack: the narrowest power-of-two width whose height is not larger than the width */
    UipBakeJob **order = xmalloc(sizeof(UipBakeJob *) * (size_t)(njobs + 1));
    int norder = 0;
    for (int i = 0; i < njobs; i++)
        if (jobs[i].w > 0 && jobs[i].h > 0) order[norder++] = &jobs[i];
    qsort(order, (size_t)norder, sizeof(order[0]), uip_job_cmp);
    int W = 128, H = 0;
    for (;; W *= 2) {
        H = uip_shelf_pack(order, norder, W);
        if (H <= W || W >= 4096) break;
    }
    FS_ASSERT(H <= 4096);
    int Hp = 16;
    while (Hp < H) Hp *= 2;
    H = Hp;

    /* 3. rasterize into an 8-bit coverage image, then expand to RGBA */
    u8 *cov = xcalloc((size_t)W * (size_t)H, 1);
    for (int i = 0; i < norder; i++) {
        UipBakeJob *j = order[i];
        stbtt_MakeGlyphBitmap(j->info, cov + (size_t)(j->ay + 1) * (size_t)W + (size_t)(j->ax + 1), j->w, j->h, W,
                              j->scale, j->scale, j->glyph);
    }
    u8 lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (u8)(255.0f * powf((float)i / 255.0f, UIP_COVERAGE_GAMMA) + 0.5f);
    free(ui.atlas);
    ui.atlas = xmalloc((size_t)W * (size_t)H * 4);
    for (size_t p = 0, n = (size_t)W * (size_t)H; p < n; p++) {
        ui.atlas[p * 4 + 0] = ui.atlas[p * 4 + 1] = ui.atlas[p * 4 + 2] = 255;
        ui.atlas[p * 4 + 3] = lut[cov[p]];
    }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) ui.atlas[((size_t)y * (size_t)W + (size_t)x) * 4 + 3] = 255; /* white texel */
    free(cov);
    ui.atlas_w = W;
    ui.atlas_h = H;
    ui.white_u = 0.5f / (float)W;
    ui.white_v = 0.5f / (float)H;

    /* 4. fill in texture coordinates and the lookup tables */
    for (int i = 0; i < njobs; i++) {
        UipBakeJob *j = &jobs[i];
        if (j->w <= 0 || j->h <= 0) continue;
        j->dst->u0 = (float)(j->ax + 1) / (float)W;
        j->dst->v0 = (float)(j->ay + 1) / (float)H;
        j->dst->u1 = (float)(j->ax + 1 + j->w) / (float)W;
        j->dst->v1 = (float)(j->ay + 1 + j->h) / (float)H;
    }
    for (int fi = 0; fi < UI_FONT_COUNT; fi++) {
        UipFont *f = &ui.fonts[fi];
        qsort(f->glyphs, (size_t)f->nglyphs, sizeof(UipGlyph), uip_glyph_cmp); /* already sorted; cheap insurance */
        for (int c = 0; c < 256; c++) f->ascii[c] = -1;
        f->fffd = -1;
        for (int g = 0; g < f->nglyphs; g++) {
            if (f->glyphs[g].cp < 256) f->ascii[f->glyphs[g].cp] = (short)g;
            if (f->glyphs[g].cp == 0xFFFD) f->fffd = g;
        }
    }
    free(order);
    free(jobs);
    for (int i = 0; i < ui.icons.len; i++) ui.icons.data[i].ready = true;
    ui.atlas_fresh = true;
    ui.bake_needed = false;
    if (out_w) *out_w = W;
    if (out_h) *out_h = H;
    return ui.atlas;
}

const u8 *ui_take_atlas_update(int *w, int *h) {
    if (!ui.atlas_fresh) return NULL;
    ui.atlas_fresh = false;
    if (w) *w = ui.atlas_w;
    if (h) *h = ui.atlas_h;
    return ui.atlas;
}

static void uip_ensure_baked(void) {
    if (!ui.atlas) ui_fonts_init(ui.scale > 0 ? ui.scale : 1.0f, NULL, NULL);
}

void ui_set_font_size(UiFont f, float logical_px) {
    if (f < 0 || f >= UI_FONT_COUNT || logical_px < 6) return;
    if (ui.fonts[f].logical_px == logical_px) return;
    ui.fonts[f].logical_px = logical_px;
    ui.bake_needed = true;
}

float ui_font_size(UiFont f) {
    float v = ui.fonts[f].logical_px;
    return v > 0 ? v : 15.0f;
}

float ui_scale(void) { return ui.scale > 0 ? ui.scale : 1.0f; }
float ui_px(float logical) { return logical * ui_scale(); }
float ui_font_line_height(UiFont f) { uip_ensure_baked(); return ui.fonts[f].line_h; }
float ui_font_ascent(UiFont f) { uip_ensure_baked(); return ui.fonts[f].ascent; }

float ui_font_advance(UiFont f, u32 cp) {
    uip_ensure_baked();
    const UipGlyph *g = uip_find_glyph(&ui.fonts[f], cp);
    return g ? g->adv : ui.fonts[f].px * 0.5f;
}

float ui_text_width(UiFont f, const char *s, int len) {
    uip_ensure_baked();
    if (len < 0) len = (int)strlen(s);
    float w = 0;
    for (int i = 0; i < len;) {
        u32 cp;
        i += utf8_decode(s + i, len - i, &cp);
        const UipGlyph *g = uip_find_glyph(&ui.fonts[f], cp);
        w += g ? g->adv : ui.fonts[f].px * 0.5f;
    }
    return w;
}

/* ======================================================================================================
 * 3. draw lists
 * ==================================================================================================== */

static UipLayer *uip_layer(void) { return &ui.layers[ui.layer]; }
static float *uip_clip(void) { UipLayer *L = uip_layer(); return L->clips[L->nclips - 1]; }

int ui_layer(int layer) {
    int prev = ui.layer;
    ui.layer = FS_CLAMP(layer, 0, UI_LAYER_COUNT - 1);
    return prev;
}

void ui_push_clip(float x, float y, float w, float h) {
    UipLayer *L = uip_layer();
    if (L->nclips >= UIP_MAX_CLIPS) return;
    float *c = uip_clip(), *n = L->clips[L->nclips++];
    n[0] = FS_MAX(c[0], uip_roundf(x));
    n[1] = FS_MAX(c[1], uip_roundf(y));
    n[2] = FS_MIN(c[2], uip_roundf(x + w));
    n[3] = FS_MIN(c[3], uip_roundf(y + h));
    if (n[2] < n[0]) n[2] = n[0];
    if (n[3] < n[1]) n[3] = n[1];
}

void ui_pop_clip(void) {
    UipLayer *L = uip_layer();
    if (L->nclips > 1) L->nclips--;
}

/* True if the bounding box is entirely outside the current clip (nothing to draw). */
static bool uip_culled(float x0, float y0, float x1, float y1) {
    float *c = uip_clip();
    return x1 < c[0] || x0 > c[2] || y1 < c[1] || y0 > c[3] || c[2] <= c[0] || c[3] <= c[1];
}

/* The command that new indices append to: same texture and clip as the last one, else a fresh one. */
static GfxCmd *uip_cmd_for(int tex) {
    UipLayer *L = uip_layer();
    float *c = uip_clip();
    if (L->cmds.len > 0) {
        GfxCmd *last = &L->cmds.data[L->cmds.len - 1];
        if (last->texture == tex && last->clip_x0 == c[0] && last->clip_y0 == c[1] && last->clip_x1 == c[2] &&
            last->clip_y1 == c[3])
            return last;
    }
    GfxCmd cmd = { (u32)L->idx.len, 0, tex, c[0], c[1], c[2], c[3] };
    vec_push(L->cmds, cmd);
    return &L->cmds.data[L->cmds.len - 1];
}

static u32 uip_vtx(float x, float y, float u, float v, u32 col) {
    GfxVertex vt = { x, y, u, v, col };
    vec_push(ui.verts, vt);
    return (u32)(ui.verts.len - 1);
}

static u32 uip_vtx_solid(float x, float y, u32 col) { return uip_vtx(x, y, ui.white_u, ui.white_v, col); }

static void uip_tri(GfxCmd *cmd, u32 a, u32 b, u32 c) {
    UipLayer *L = uip_layer();
    vec_push(L->idx, a);
    vec_push(L->idx, b);
    vec_push(L->idx, c);
    cmd->index_count += 3;
}

static void uip_quad(GfxCmd *cmd, u32 a, u32 b, u32 c, u32 d) { /* corners in order around the quad */
    uip_tri(cmd, a, b, c);
    uip_tri(cmd, a, c, d);
}

static void uip_quad_uv(int tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 col) {
    GfxCmd *cmd = uip_cmd_for(tex);
    u32 a = uip_vtx(x0, y0, u0, v0, col), b = uip_vtx(x1, y0, u1, v0, col);
    u32 c = uip_vtx(x1, y1, u1, v1, col), d = uip_vtx(x0, y1, u0, v1, col);
    uip_quad(cmd, a, b, c, d);
}

void ui_rect(float x, float y, float w, float h, u32 col) {
    float x0 = uip_roundf(x), y0 = uip_roundf(y), x1 = uip_roundf(x + w), y1 = uip_roundf(y + h);
    if (x1 <= x0 || y1 <= y0 || (col >> 24) == 0 || uip_culled(x0, y0, x1, y1)) return;
    uip_quad_uv(0, x0, y0, x1, y1, ui.white_u, ui.white_v, ui.white_u, ui.white_v, col);
}

void ui_rect_gradient(float x, float y, float w, float h, u32 top, u32 bottom) {
    float x0 = uip_roundf(x), y0 = uip_roundf(y), x1 = uip_roundf(x + w), y1 = uip_roundf(y + h);
    if (x1 <= x0 || y1 <= y0 || uip_culled(x0, y0, x1, y1)) return;
    GfxCmd *cmd = uip_cmd_for(0);
    u32 a = uip_vtx_solid(x0, y0, top), b = uip_vtx_solid(x1, y0, top);
    u32 c = uip_vtx_solid(x1, y1, bottom), d = uip_vtx_solid(x0, y1, bottom);
    uip_quad(cmd, a, b, c, d);
}

void ui_rect_outline(float x, float y, float w, float h, float thickness, u32 col) {
    float t = FS_MAX(1.0f, uip_roundf(thickness));
    float x0 = uip_roundf(x), y0 = uip_roundf(y), x1 = uip_roundf(x + w), y1 = uip_roundf(y + h);
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    if (x1 - x0 <= 2 * t || y1 - y0 <= 2 * t) { ui_rect(x0, y0, x1 - x0, y1 - y0, col); return; }
    ui_rect(x0, y0, x1 - x0, t, col);                        /* top */
    ui_rect(x0, y1 - t, x1 - x0, t, col);                    /* bottom */
    ui_rect(x0, y0 + t, t, y1 - y0 - 2 * t, col);            /* left */
    ui_rect(x1 - t, y0 + t, t, y1 - y0 - 2 * t, col);        /* right */
}

/* ---- anti-aliased shapes ---------------------------------------------------------------------------- */

#define UIP_MAX_PATH 600 /* points */

/* Fill a convex polygon.  The inner polygon is inset by half a pixel and a one pixel fringe ramps the alpha to
 * zero across the true edge, which gives exact coverage for pixel-aligned edges and a good one elsewhere. */
static void uip_fill_convex(const float *p, int n, u32 col) {
    if (n < 3 || (col >> 24) == 0) return;
    float area = 0, minx = p[0], maxx = p[0], miny = p[1], maxy = p[1];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1];
        minx = FS_MIN(minx, p[2 * i]); maxx = FS_MAX(maxx, p[2 * i]);
        miny = FS_MIN(miny, p[2 * i + 1]); maxy = FS_MAX(maxy, p[2 * i + 1]);
    }
    if (uip_culled(minx - 1, miny - 1, maxx + 1, maxy + 1) || area == 0) return;
    float sgn = area > 0 ? 1.0f : -1.0f;
    float nx[UIP_MAX_PATH], ny[UIP_MAX_PATH]; /* outward edge normals */
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dx = p[2 * j] - p[2 * i], dy = p[2 * j + 1] - p[2 * i + 1];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f) { nx[i] = ny[i] = 0; continue; }
        nx[i] = dy / len * sgn;
        ny[i] = -dx / len * sgn;
    }
    GfxCmd *cmd = uip_cmd_for(0);
    u32 clear = col & 0x00FFFFFFu;
    u32 base = (u32)ui.verts.len;
    for (int i = 0; i < n; i++) {
        int h = (i + n - 1) % n;
        float mx = (nx[h] + nx[i]) * 0.5f, my = (ny[h] + ny[i]) * 0.5f;
        float d2 = mx * mx + my * my;
        if (d2 > 1e-6f) {
            float k = FS_MIN(1.0f / d2, 10.0f); /* miter scale, limited so sharp tips do not spike */
            mx *= k;
            my *= k;
        }
        uip_vtx_solid(p[2 * i] - mx * 0.5f, p[2 * i + 1] - my * 0.5f, col);   /* base + 2i     */
        uip_vtx_solid(p[2 * i] + mx * 0.5f, p[2 * i + 1] + my * 0.5f, clear); /* base + 2i + 1 */
    }
    for (int i = 1; i + 1 < n; i++) uip_tri(cmd, base, base + 2 * (u32)i, base + 2 * (u32)(i + 1));
    for (int i = 0; i < n; i++) {
        u32 j = (u32)((i + 1) % n);
        u32 a = base + 2 * (u32)i, b = base + 2 * j;
        uip_quad(cmd, a, b, b + 1, a + 1);
    }
}

/* Stroke a polyline with thickness `thick` (pixels).  Core half-width thick/2 - 0.5 plus a 1 px fringe.
 * Thinner than one pixel is drawn one pixel wide at reduced alpha. */
static void uip_stroke(const float *p, int n, bool closed, float thick, u32 col) {
    if (n < 2 || (col >> 24) == 0) return;
    if (thick < 1.0f) { col = ui_alpha(col, thick); thick = 1.0f; }
    float minx = p[0], maxx = p[0], miny = p[1], maxy = p[1];
    for (int i = 1; i < n; i++) {
        minx = FS_MIN(minx, p[2 * i]); maxx = FS_MAX(maxx, p[2 * i]);
        miny = FS_MIN(miny, p[2 * i + 1]); maxy = FS_MAX(maxy, p[2 * i + 1]);
    }
    float pad = thick + 2;
    if (uip_culled(minx - pad, miny - pad, maxx + pad, maxy + pad)) return;
    float core = (thick - 1.0f) * 0.5f, edge = core + 1.0f;
    int nseg = closed ? n : n - 1;
    float sx[UIP_MAX_PATH], sy[UIP_MAX_PATH]; /* unit normal of segment i -> i+1 */
    for (int i = 0; i < nseg; i++) {
        int j = (i + 1) % n;
        float dx = p[2 * j] - p[2 * i], dy = p[2 * j + 1] - p[2 * i + 1];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f) { sx[i] = sy[i] = 0; continue; }
        sx[i] = -dy / len;
        sy[i] = dx / len;
    }
    GfxCmd *cmd = uip_cmd_for(0);
    u32 clear = col & 0x00FFFFFFu;
    u32 base = (u32)ui.verts.len;
    for (int i = 0; i < n; i++) {
        bool has_prev = closed || i > 0, has_next = closed || i < n - 1;
        int ip = (i + n - 1) % n;
        float mx = 0, my = 0;
        if (has_prev) { mx += sx[ip]; my += sy[ip]; }
        if (has_next) { mx += sx[i]; my += sy[i]; }
        if (has_prev && has_next) { mx *= 0.5f; my *= 0.5f; }
        float d2 = mx * mx + my * my;
        if (d2 < 1e-6f) { mx = has_next ? sx[i] : sx[ip]; my = has_next ? sy[i] : sy[ip]; d2 = mx * mx + my * my; }
        if (d2 > 1e-6f && has_prev && has_next) {
            float k = FS_MIN(1.0f / d2, 10.0f);
            mx *= k;
            my *= k;
        }
        float x = p[2 * i], y = p[2 * i + 1];
        uip_vtx_solid(x + mx * edge, y + my * edge, clear);
        uip_vtx_solid(x + mx * core, y + my * core, col);
        uip_vtx_solid(x - mx * core, y - my * core, col);
        uip_vtx_solid(x - mx * edge, y - my * edge, clear);
    }
    for (int i = 0; i < nseg; i++) {
        u32 a = base + 4 * (u32)i, b = base + 4 * (u32)((i + 1) % n);
        for (u32 k = 0; k < 3; k++) uip_quad(cmd, a + k, b + k, b + k + 1, a + k + 1);
    }
}

void ui_polyline(const float *xy, int n, bool closed, float thickness, u32 col) {
    if (n > UIP_MAX_PATH) n = UIP_MAX_PATH;
    uip_stroke(xy, n, closed, thickness, col);
}

void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 col) {
    float p[4] = { x0, y0, x1, y1 };
    uip_stroke(p, 2, false, thickness, col);
}

void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 col) {
    float p[6] = { x0, y0, x1, y1, x2, y2 };
    uip_fill_convex(p, 3, col);
}

/* Points on a circle: enough segments that the polygon stays within ~0.04 px of the true circle. */
static int uip_circle_points(float r, float *p, float cx, float cy) {
    int n = 8;
    if (r > 0.3f) {
        float a = acosf(FS_MAX(0.0f, 1.0f - 0.04f / r));
        n = a > 0 ? (int)ceilf(3.14159265f / a) : UIP_MAX_PATH / 2;
    }
    n = FS_CLAMP(n, 12, UIP_MAX_PATH / 2);
    for (int i = 0; i < n; i++) {
        float t = 6.2831853f * (float)i / (float)n;
        p[2 * i] = cx + r * cosf(t);
        p[2 * i + 1] = cy + r * sinf(t);
    }
    return n;
}

void ui_circle(float cx, float cy, float r, float thickness, u32 col) {
    if (r <= 0) return;
    float p[UIP_MAX_PATH * 2];
    int n = uip_circle_points(r, p, cx, cy);
    uip_stroke(p, n, true, thickness, col);
}

void ui_disk(float cx, float cy, float r, u32 col) {
    if (r <= 0) return;
    if (r < 0.75f) { /* smaller than the feather: a pixel-sized dot whose alpha is the covered area */
        float a = FS_MIN(1.0f, 3.14159265f * r * r);
        uip_quad_uv(0, cx - 0.5f, cy - 0.5f, cx + 0.5f, cy + 0.5f, ui.white_u, ui.white_v, ui.white_u, ui.white_v,
                    ui_alpha(col, a));
        return;
    }
    float p[UIP_MAX_PATH * 2];
    int n = uip_circle_points(r, p, cx, cy);
    uip_fill_convex(p, n, col);
}

/* Outline of a rounded rectangle as a closed path (clockwise on screen). */
static int uip_rrect_path(float *p, float x0, float y0, float x1, float y1, float r) {
    r = FS_MIN(r, FS_MIN(x1 - x0, y1 - y0) * 0.5f);
    if (r < 0.5f) {
        float q[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
        memcpy(p, q, sizeof q);
        return 4;
    }
    int seg = FS_CLAMP((int)ceilf(1.5707963f / acosf(FS_MAX(0.0f, 1.0f - 0.25f / r))), 2, 32);
    float cxs[4] = { x1 - r, x1 - r, x0 + r, x0 + r }, cys[4] = { y0 + r, y1 - r, y1 - r, y0 + r };
    int n = 0;
    for (int c = 0; c < 4; c++) { /* top-right, bottom-right, bottom-left, top-left */
        for (int i = 0; i <= seg; i++) {
            float t = ((float)c - 1.0f + (float)i / (float)seg) * 1.5707963f;
            p[2 * n] = cxs[c] + r * cosf(t);
            p[2 * n + 1] = cys[c] + r * sinf(t);
            n++;
        }
    }
    return n;
}

void ui_rrect(float x, float y, float w, float h, float radius, u32 col) {
    float x0 = uip_roundf(x), y0 = uip_roundf(y), x1 = uip_roundf(x + w), y1 = uip_roundf(y + h);
    if (x1 <= x0 || y1 <= y0) return;
    if (radius < 0.5f) { ui_rect(x0, y0, x1 - x0, y1 - y0, col); return; }
    float p[UIP_MAX_PATH * 2];
    int n = uip_rrect_path(p, x0, y0, x1, y1, radius);
    uip_fill_convex(p, n, col);
}

void ui_rrect_outline(float x, float y, float w, float h, float radius, float thickness, u32 col) {
    float t = FS_MAX(1.0f, uip_roundf(thickness));
    float x0 = uip_roundf(x), y0 = uip_roundf(y), x1 = uip_roundf(x + w), y1 = uip_roundf(y + h);
    if (x1 - x0 < 2 * t || y1 - y0 < 2 * t) return;
    if (radius < 0.5f) { ui_rect_outline(x0, y0, x1 - x0, y1 - y0, t, col); return; }
    float h2 = t * 0.5f; /* the stroke's center line runs half a thickness inside the edge */
    float p[UIP_MAX_PATH * 2];
    int n = uip_rrect_path(p, x0 + h2, y0 + h2, x1 - h2, y1 - h2, radius - h2);
    uip_stroke(p, n, true, t, col);
}

/* ---- text and images ------------------------------------------------------------------------------- */

void ui_text(UiFont f, float x, float y, const char *s, int len, u32 col) {
    uip_ensure_baked();
    if (!s || (col >> 24) == 0) return;
    if (len < 0) len = (int)strlen(s);
    const UipFont *fd = &ui.fonts[f];
    float base = uip_roundf(y) + fd->ascent;
    float *c = uip_clip();
    if (base - fd->ascent > c[3] || base + (fd->line_h - fd->ascent) < c[1]) return; /* line outside the clip */
    GfxCmd *cmd = NULL;
    float pen = x;
    for (int i = 0; i < len;) {
        u32 cp;
        i += utf8_decode(s + i, len - i, &cp);
        const UipGlyph *g = uip_find_glyph(fd, cp);
        if (!g) { pen += fd->px * 0.5f; continue; }
        if (g->x1 > g->x0) {
            float gx = floorf(pen + 0.5f);
            float x0 = gx + g->x0, x1 = gx + g->x1, y0 = base + g->y0, y1 = base + g->y1;
            if (x0 >= c[2]) break; /* past the right edge of the clip */
            if (x1 > c[0]) {
                if (!cmd) cmd = uip_cmd_for(0);
                u32 a = uip_vtx(x0, y0, g->u0, g->v0, col), b = uip_vtx(x1, y0, g->u1, g->v0, col);
                u32 d = uip_vtx(x0, y1, g->u0, g->v1, col), e = uip_vtx(x1, y1, g->u1, g->v1, col);
                uip_quad(cmd, a, b, e, d);
            }
        }
        pen += g->adv;
    }
}

float ui_text_wrapped(UiFont f, float x, float y, float max_w, const char *s, u32 col, bool draw) {
    float lh = ui_font_line_height(f), cy = y;
    int n = (int)strlen(s), ls = 0;
    while (ls <= n) {
        int i = ls, brk = -1, end = -1, next = -1;
        float w = 0;
        while (i < n && s[i] != '\n') {
            u32 cp;
            int l = utf8_decode(s + i, n - i, &cp);
            float a = ui_font_advance(f, cp);
            if (w + a > max_w && i > ls) { /* overflow: break at the last space, else inside the word */
                end = brk >= 0 ? brk : i;
                next = brk >= 0 ? brk + 1 : i;
                break;
            }
            w += a;
            if (cp == ' ') brk = i;
            i += l;
        }
        if (end < 0) { end = i; next = i < n ? i + 1 : n + 1; }
        if (draw && end > ls) ui_text(f, x, cy, s + ls, end - ls, col);
        cy += lh;
        ls = next;
    }
    return cy - y;
}

void ui_image_ex(int texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 tint) {
    if (uip_culled(x, y, x + w, y + h)) return;
    uip_quad_uv(texture, x, y, x + w, y + h, u0, v0, u1, v1, tint);
}

void ui_image(int texture, float x, float y, float w, float h) {
    ui_image_ex(texture, x, y, w, h, 0, 0, 1, 1, 0xFFFFFFFFu);
}

/* ---- icons ------------------------------------------------------------------------------------------------- */

u32 ui_icon_lookup(const char *name) {
    if (!name || !name[0]) return 0;
    int lo = 0, hi = ARRAY_COUNT(material_icon_names) - 1;
    while (lo <= hi) { /* the table is sorted in strcmp order (tools/gen_icon_codepoints.py) */
        int mid = (lo + hi) / 2;
        int c = strcmp(name, material_icon_names[mid].name);
        if (c == 0) return material_icon_names[mid].cp;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

/* The baked glyph for a codepoint at a whole-pixel size, or NULL if it is not in the atlas yet.  The first request
 * of a pair adds it to the table and asks for a rebuild: ui_begin_frame does it before the next frame is built, so
 * the atlas is rebuilt once per frame however many icons were new, and a frame is requested right away. */
static const UipIcon *uip_icon_get(u32 cp, int px) {
    for (int i = 0; i < ui.icons.len; i++) {
        UipIcon *ic = &ui.icons.data[i];
        if (ic->g.cp == cp && ic->px == px) {
            ic->last_used = ++ui.icon_clock;
            return ic->ready ? ic : NULL;
        }
    }
    UipIcon n;
    memset(&n, 0, sizeof n);
    n.g.cp = cp;
    n.px = px;
    n.last_used = ++ui.icon_clock;
    vec_push(ui.icons, n);
    ui.bake_needed = true;
    ui.redraw_now = true;
    return NULL;
}

/* Stand-in for an icon: text in the sans font (at its normal size), its advance centred horizontally and its cap
 * height centred vertically in the square, so single letters sit on a common baseline across a row of buttons. */
static void uip_icon_fallback(const char *text, float x, float y, float size, u32 rgba) {
    if (!text || !text[0]) return;
    const UipFont *f = &ui.fonts[UI_FONT_SANS];
    const UipGlyph *cap_h = uip_find_glyph(f, 'H');
    float cap = cap_h ? -cap_h->y0 : f->ascent * 0.7f;
    float tx = x + (size - ui_text_width(UI_FONT_SANS, text, -1)) * 0.5f;
    float baseline = uip_roundf(y + (size + cap) * 0.5f);
    ui_text(UI_FONT_SANS, tx, baseline - f->ascent, text, -1, rgba);
}

void ui_icon(const char *name, const char *fallback, float x, float y, float size, u32 rgba) {
    uip_ensure_baked();
    if ((rgba >> 24) == 0 || !(size >= 1.0f) || uip_culled(x, y, x + size, y + size)) return; /* (no baking for icons nobody sees) */
    u32 cp = uip_icon_font() ? ui_icon_lookup(name) : 0;
    if (!cp) { uip_icon_fallback(fallback, x, y, size, rgba); return; }
    int px = (int)FS_CLAMP(size + 0.5f, (float)UIP_ICON_MIN_PX, (float)UIP_ICON_MAX_PX); /* whole pixels keep the atlas small */
    const UipIcon *ic = uip_icon_get(cp, px);
    if (!ic || ic->g.x1 <= ic->g.x0) return; /* baked in time for the next frame, or blank */
    float bx = uip_roundf(x + (size - ic->g.adv) * 0.5f), by = uip_roundf(y + (size - ic->box_h) * 0.5f); /* em box */
    float x0 = bx + ic->g.x0, y0 = by + ic->g.y0, x1 = bx + ic->g.x1, y1 = by + ic->g.y1;
    if (uip_culled(x0, y0, x1, y1)) return;
    uip_quad_uv(0, x0, y0, x1, y1, ic->g.u0, ic->g.v0, ic->g.u1, ic->g.v1, rgba);
}

/* ======================================================================================================
 * 4. frame, ids, hit-testing, interaction
 * ==================================================================================================== */

void ui_begin_frame(const UiInput *in) {
    ui.in = *in;
    if (ui.in.content_scale <= 0) ui.in.content_scale = 1.0f;
    /* the one place where the atlas is rebuilt during normal operation: scale or font size changed, or icons were
     * requested last frame; so at most once per frame, before anything is drawn with the new texture coordinates */
    if (ui.bake_needed || !ui.atlas || fabsf(ui.in.content_scale - ui.scale) > 0.001f)
        ui_fonts_init(ui.in.content_scale, NULL, NULL);
    ui.fb_w = (float)ui.in.fb_w;
    ui.fb_h = (float)ui.in.fb_h;
    for (int i = 0; i < UI_LAYER_COUNT; i++) {
        UipLayer *L = &ui.layers[i];
        L->idx.len = 0;
        L->cmds.len = 0;
        L->nclips = 1;
        L->clips[0][0] = 0;
        L->clips[0][1] = 0;
        L->clips[0][2] = ui.fb_w;
        L->clips[0][3] = ui.fb_h;
    }
    ui.verts.len = 0;
    ui.layer = 0;
    ui.nid = 0;
    memcpy(ui.occ_prev, ui.occ, sizeof(UipOcc) * (size_t)ui.nocc);
    ui.nocc_prev = ui.nocc;
    ui.nocc = 0;
    memset(ui.key_used, 0, sizeof ui.key_used);
    ui.hover_id = 0;
    ui.hover_layer = -1;
    ui.last_hovered = false;
    ui.hover_seen = false;
    ui.focus_claimed = false;
    ui.active_seen = false;
    ui.focus_seen = false;
    ui.mouse_captured = false;
    /* a drag whose release we never saw (focus loss) must not stay active */
    if (ui.active && !ui.in.mouse_down[0] && !ui.in.mouse_released[0]) ui.active = 0;
    ui.cursor = UI_CURSOR_ARROW;
    ui.redraw_now = false;
    ui.wake_in = 1e9;
}

const GfxDrawList *ui_end_frame(void) {
    /* A click that no text widget claimed takes the keyboard focus away. */
    if (ui.in.mouse_pressed[0] && !ui.focus_claimed) ui.focus = 0;
    /* A widget that stopped being drawn (panel closed) cannot keep the mouse or the keyboard. */
    if (ui.active && !ui.active_seen) ui.active = 0;
    if (ui.focus && !ui.focus_seen && !ui.focus_claimed) ui.focus = 0;
    if (!ui.hover_seen) ui.hover_track_id = 0;
    ui.prev_mx = ui.in.mouse_x;
    ui.prev_my = ui.in.mouse_y;

    /* Concatenate the layers bottom to top, merging neighbouring commands that can share a draw call. */
    ui.out_idx.len = 0;
    ui.out_cmds.len = 0;
    for (int l = 0; l < UI_LAYER_COUNT; l++) {
        UipLayer *L = &ui.layers[l];
        u32 base = (u32)ui.out_idx.len;
        for (int i = 0; i < L->idx.len; i++) vec_push(ui.out_idx, L->idx.data[i]);
        for (int i = 0; i < L->cmds.len; i++) {
            GfxCmd c = L->cmds.data[i];
            if (c.index_count == 0) continue;
            c.first_index += base;
            if (ui.out_cmds.len > 0) {
                GfxCmd *p = &ui.out_cmds.data[ui.out_cmds.len - 1];
                if (p->texture == c.texture && p->clip_x0 == c.clip_x0 && p->clip_y0 == c.clip_y0 &&
                    p->clip_x1 == c.clip_x1 && p->clip_y1 == c.clip_y1 && p->first_index + p->index_count == c.first_index) {
                    p->index_count += c.index_count;
                    continue;
                }
            }
            vec_push(ui.out_cmds, c);
        }
    }
    ui.out.verts = ui.verts.data;
    ui.out.nverts = ui.verts.len;
    ui.out.indices = ui.out_idx.data;
    ui.out.nindices = ui.out_idx.len;
    ui.out.cmds = ui.out_cmds.data;
    ui.out.ncmds = ui.out_cmds.len;
    return &ui.out;
}

const UiInput *ui_input(void) { return &ui.in; }

/* ---- ids ---------------------------------------------------------------------------------------------- */

u32 ui_id(const char *s) {
    u32 h = ui.nid > 0 ? ui.id_stack[ui.nid - 1] : 2166136261u; /* FNV-1a seeded with the enclosing scope */
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1u;
}

void ui_push_id(const char *s) {
    u32 id = ui_id(s);
    FS_ASSERT(ui.nid < UIP_MAX_IDS);
    ui.id_stack[ui.nid++] = id;
}

void ui_push_id_int(int n) {
    char b[16];
    snprintf(b, sizeof b, "%d", n);
    ui_push_id(b);
}

void ui_push_id_ptr(const void *p) {
    char b[32];
    snprintf(b, sizeof b, "@%llx", (unsigned long long)(uintptr_t)p);
    ui_push_id(b);
}

void ui_pop_id(void) {
    if (ui.nid > 0) ui.nid--;
}

/* ---- covering and hit-testing -------------------------------------------------------------------------- */

static void uip_add_occ(float x, float y, float w, float h) {
    if (ui.nocc >= UIP_MAX_OCC) return;
    float *c = uip_clip();
    UipOcc o = { FS_MAX(x, c[0]), FS_MAX(y, c[1]), FS_MIN(x + w, c[2]), FS_MIN(y + h, c[3]), ui.layer };
    if (o.x1 <= o.x0 || o.y1 <= o.y0) return;
    ui.occ[ui.nocc++] = o;
}

void ui_occlude(float x, float y, float w, float h) { uip_add_occ(x, y, w, h); }

static bool uip_in_occ(const UipOcc *list, int n, float mx, float my, int above_layer) {
    for (int i = 0; i < n; i++) {
        const UipOcc *o = &list[i];
        if (o->layer > above_layer && mx >= o->x0 && mx < o->x1 && my >= o->y0 && my < o->y1) return true;
    }
    return false;
}

bool ui_mouse_over(float x, float y, float w, float h) {
    float mx = ui.in.mouse_x, my = ui.in.mouse_y;
    float *c = uip_clip();
    if (ui.mouse_captured) return false;
    if (mx < c[0] || mx >= c[2] || my < c[1] || my >= c[3]) return false;
    if (mx < x || mx >= x + w || my < y || my >= y + h) return false;
    return !uip_in_occ(ui.occ, ui.nocc, mx, my, ui.layer) && !uip_in_occ(ui.occ_prev, ui.nocc_prev, mx, my, ui.layer);
}

void ui_set_mouse_captured(bool captured) { ui.mouse_captured = captured; }

bool ui_wants_mouse(void) {
    if (ui.active) return true;
    float mx = ui.in.mouse_x, my = ui.in.mouse_y;
    return uip_in_occ(ui.occ, ui.nocc, mx, my, -1) || uip_in_occ(ui.occ_prev, ui.nocc_prev, mx, my, -1);
}

bool ui_wants_keyboard(void) { return ui.focus != 0; }
UiCursor ui_cursor(void) { return ui.cursor; }
void ui_set_cursor(UiCursor c) { ui.cursor = c; }
void ui_request_redraw(void) { ui.redraw_now = true; }
void ui_wake_in(double seconds) { ui.wake_in = FS_MIN(ui.wake_in, FS_MAX(seconds, 0.0)); }
double ui_redraw_in(void) { return ui.redraw_now ? 0.0 : ui.wake_in; }

u32 ui_active_id(void) { return ui.active; }
void ui_set_active(u32 id) {
    ui.active = id;
    ui.active_seen = true;
}
u32 ui_focus_id(void) { return ui.focus; }
bool ui_has_focus(u32 id) {
    if (id == 0 || ui.focus != id) return false;
    ui.focus_seen = true; /* the focused widget asks every frame; one that stops asking loses the focus */
    return true;
}
void ui_reset_interaction(void) {
    free(ui.te.orig);
    memset(&ui.te, 0, sizeof ui.te);
    ui.active = ui.focus = ui.num_edit_id = ui.te_lost = 0;
    ui.hover_track_id = 0;
    ui.nocc = ui.nocc_prev = 0;
}
void ui_set_focus(u32 id) {
    ui.focus = id;
    ui.focus_claimed = true;
}

bool ui_last_hovered(void) { return ui.last_hovered; }

UiInteract ui_interact_id(u32 id, float x, float y, float w, float h) {
    UiInteract r;
    memset(&r, 0, sizeof r);
    const UiInput *in = &ui.in;
    uip_add_occ(x, y, w, h);
    bool mine = ui.active == id;
    bool over = ui_mouse_over(x, y, w, h);
    if (over && (ui.active == 0 || mine) && (ui.hover_id == 0 || ui.layer > ui.hover_layer)) {
        ui.hover_id = id; /* the first widget on the topmost layer under the mouse wins */
        ui.hover_layer = ui.layer;
    }
    r.hovered = over && ui.hover_id == id && (ui.active == 0 || mine);
    ui.last_hovered = r.hovered;
    if (r.hovered) {
        ui.hover_seen = true;
        if (ui.hover_track_id != id) { ui.hover_track_id = id; ui.hover_track_start = in->time; }
    }
    if (r.hovered && in->mouse_pressed[0]) {
        ui.active = id;
        ui.press_x = in->mouse_x;
        ui.press_y = in->mouse_y;
        ui.press_time = in->time;
        r.pressed = true;
        r.double_clicked = in->click_count == 2;
    }
    if (ui.active == id) {
        ui.active_seen = true;
        r.press_x = ui.press_x;
        r.press_y = ui.press_y;
        r.dx = in->mouse_x - ui.prev_mx;
        r.dy = in->mouse_y - ui.prev_my;
        if (in->mouse_down[0]) {
            r.held = true;
            r.held_time = (float)(in->time - ui.press_time);
        }
        if (in->mouse_released[0]) {
            r.released = true;
            r.clicked = over;
            ui.active = 0;
        }
    }
    return r;
}

UiInteract ui_interact(const char *id, float x, float y, float w, float h) {
    return ui_interact_id(ui_id(id), x, y, w, h);
}

/* ---- keyboard ------------------------------------------------------------------------------------------ */

const UiKeyEvent *ui_key_events(int *n) {
    if (n) *n = ui.in.nkeys;
    return ui.in.keys;
}

bool ui_key_consumed(int i) { return i < 0 || i >= ui.in.nkeys || ui.key_used[i]; }

void ui_key_consume(int i) {
    if (i >= 0 && i < ui.in.nkeys) ui.key_used[i] = true;
}

static bool uip_shortcut(UiKey key, int mods, bool allow_repeat) {
    for (int i = 0; i < ui.in.nkeys; i++) {
        const UiKeyEvent *e = &ui.in.keys[i];
        if (ui.key_used[i] || e->key != key || e->text || (e->mods & 15) != mods) continue;
        if (e->repeat && !allow_repeat) continue;
        ui.key_used[i] = true;
        return true;
    }
    return false;
}

bool ui_shortcut(UiKey key, int mods) { return uip_shortcut(key, mods, false); }
bool ui_shortcut_repeat(UiKey key, int mods) { return uip_shortcut(key, mods, true); }

/* ---- words ---------------------------------------------------------------------------------------------- */

static u32 uip_cp_at(const char *s, int i, int *n) {
    int avail = 0;
    while (avail < 4 && s[i + avail]) avail++;
    u32 cp = 0;
    int l = avail > 0 ? utf8_decode(s + i, avail, &cp) : 0;
    if (n) *n = l > 0 ? l : 1;
    return cp;
}

/* 0 blank, 1 word character, 2 punctuation / symbol */
static int uip_cls(u32 cp) {
    if (cp == ' ' || cp == '\t') return 0;
    if (cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return 1;
    if (cp < 0x80) return 2;
    if ((cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) || (cp >= 0x370 && cp <= 0x3FF) ||
        (cp >= 0x2070 && cp <= 0x209F) || (cp >= 0x2100 && cp <= 0x214F))
        return 1; /* accented Latin, Greek, sub/superscripts, double-struck letters */
    return 2;
}

int ui_word_right(const char *s, int i) {
    if (!s[i]) return i;
    if (s[i] == '\n') return i + 1;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (!s[i] || s[i] == '\n') return i;
    int n, c = uip_cls(uip_cp_at(s, i, &n));
    while (s[i] && s[i] != '\n') {
        if (uip_cls(uip_cp_at(s, i, &n)) != c) break;
        i += n;
    }
    return i;
}

int ui_word_left(const char *s, int i) {
    if (i <= 0) return 0;
    if (s[i - 1] == '\n') return i - 1;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    if (i == 0 || s[i - 1] == '\n') return i;
    int n, p = ui_utf8_prev(s, i), c = uip_cls(uip_cp_at(s, p, &n));
    while (i > 0) {
        p = ui_utf8_prev(s, i);
        if (s[p] == '\n' || uip_cls(uip_cp_at(s, p, &n)) != c) break;
        i = p;
    }
    return i;
}

void ui_word_at(const char *s, int i, int *start, int *end) {
    int n, a = i, b = i;
    if (s[i] && s[i] != '\n') {
        int c = uip_cls(uip_cp_at(s, i, &n));
        while (a > 0) {
            int p = ui_utf8_prev(s, a);
            if (s[p] == '\n' || uip_cls(uip_cp_at(s, p, &n)) != c) break;
            a = p;
        }
        while (s[b] && s[b] != '\n') {
            if (uip_cls(uip_cp_at(s, b, &n)) != c) break;
            b += n;
        }
    } else if (i > 0 && s[i - 1] != '\n') { /* clicked past the end of the line: take the last word */
        a = ui_word_left(s, i);
    }
    *start = a;
    *end = b;
}

/* ======================================================================================================
 * 5. widgets
 * ==================================================================================================== */

#define UIP_RADIUS 4.0f /* logical px */

static int uip_label_len(const char *label) {
    const char *p = strstr(label, "##");
    return p ? (int)(p - label) : (int)strlen(label);
}

static float uip_text_y(UiFont f, float y, float h) {
    return y + floorf((h - ui_font_line_height(f)) * 0.5f);
}

void ui_label(float x, float y, float w, float h, const char *text, int align, u32 col) {
    if (!text || !text[0]) return;
    int len = uip_label_len(text);
    float tw = ui_text_width(UI_FONT_SANS, text, len);
    float tx = x;
    if (align == UI_ALIGN_CENTER) tx = x + (w - tw) * 0.5f;
    else if (align == UI_ALIGN_RIGHT) tx = x + w - tw;
    ui_push_clip(x, y, w, h);
    ui_text(UI_FONT_SANS, tx, uip_text_y(UI_FONT_SANS, y, h), text, len, col);
    ui_pop_clip();
}

void ui_panel_ex(float x, float y, float w, float h, u32 fill, float radius) {
    uip_add_occ(x, y, w, h);
    ui_rrect(x, y, w, h, radius, fill);
    ui_rrect_outline(x, y, w, h, radius, 1, UI_COL_BORDER);
}

void ui_panel(float x, float y, float w, float h) { ui_panel_ex(x, y, w, h, UI_COL_PANEL, ui_px(6)); }

void ui_separator(float x, float y, float w) { ui_rect(x, y, w, 1, UI_COL_BORDER); }

void ui_tooltip_now(const char *text) {
    if (!text || !text[0]) return;
    float pad = ui_px(6), max_w = ui_px(340), mx = ui.in.mouse_x, my = ui.in.mouse_y;
    float widest = 0;
    float th = ui_text_wrapped(UI_FONT_SANS, 0, 0, max_w, text, 0, false);
    for (const char *p = text; *p;) { /* width of the widest line, for a tight box */
        const char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        widest = FS_MAX(widest, ui_text_width(UI_FONT_SANS, p, l));
        p += l + (e ? 1 : 0);
        if (!e) break;
    }
    float tw = FS_MIN(widest, max_w) + 1;
    float w = tw + 2 * pad, h = th + 2 * pad;
    float x = mx + ui_px(14), y = my + ui_px(20);
    if (x + w > ui.fb_w) x = mx - w - ui_px(6);
    if (y + h > ui.fb_h) y = my - h - ui_px(8);
    x = FS_MAX(x, 0.0f);
    y = FS_MAX(y, 0.0f);
    int prev = ui_layer(UI_LAYER_TOOLTIP);
    ui_rrect(x, y, w, h, ui_px(4), UI_COL_PANEL_HI);
    ui_rrect_outline(x, y, w, h, ui_px(4), 1, UI_COL_BORDER);
    ui_text_wrapped(UI_FONT_SANS, x + pad, y + pad, max_w, text, UI_COL_TEXT, true);
    ui_layer(prev);
}

void ui_tooltip(const char *text) {
    if (!text || !ui.last_hovered || ui.active) return;
    double remaining = ui.hover_track_start + UIP_TOOLTIP_DELAY - ui.in.time;
    if (remaining > 0) { ui.wake_in = FS_MIN(ui.wake_in, remaining); return; }
    ui_tooltip_now(text);
}

float ui_scroll_area(float x, float y, float w, float h) {
    if (ui.in.scroll_y == 0 || !ui_mouse_over(x, y, w, h)) return 0;
    return -ui.in.scroll_y * ui_px(44);
}

/* ---- buttons ----------------------------------------------------------------------------------------------- */

/* Interaction and frame shared by text buttons and icon buttons: handles the mouse, draws the fill and border for the
 * state, and returns the color the content (label or icon) should be drawn in.  *clicked receives the result. */
static u32 uip_button_frame(u32 id, float x, float y, float w, float h, int flags, bool *clicked) {
    bool disabled = (flags & UI_DISABLED) != 0;
    UiInteract r;
    memset(&r, 0, sizeof r);
    if (disabled) uip_add_occ(x, y, w, h);
    else r = ui_interact_id(id, x, y, w, h);
    bool lit = r.hovered, down = r.held && r.hovered;

    u32 fill, border = UI_COL_BORDER, text = UI_COL_TEXT;
    if (flags & UI_ACCENT) {
        fill = down ? ui_mix(UI_COL_ACCENT, 0xFF000000u, 0.2f) : lit ? UI_COL_ACCENT_HOVER : UI_COL_ACCENT;
        border = fill;
        text = UI_COL_ON_ACCENT;
    } else if (flags & UI_SELECTED) {
        fill = down ? UI_COL_WIDGET_ACTIVE : lit ? ui_mix(UI_COL_ACCENT_DIM, UI_COL_ACCENT, 0.2f) : UI_COL_ACCENT_DIM;
        border = UI_COL_ACCENT;
    } else if (flags & UI_FLAT) {
        fill = down ? UI_COL_WIDGET_ACTIVE : lit ? UI_COL_WIDGET_HOVER : 0;
        border = 0;
    } else {
        fill = down ? UI_COL_WIDGET_ACTIVE : lit ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET;
    }
    if (disabled) { text = UI_COL_TEXT_OFF; if (flags & UI_ACCENT) fill = UI_COL_WIDGET; }
    float rad = ui_px(UIP_RADIUS);
    if (fill) ui_rrect(x, y, w, h, rad, fill);
    if (border) ui_rrect_outline(x, y, w, h, rad, 1, border);
    *clicked = r.clicked;
    return text;
}

bool ui_button_ex(const char *label, float x, float y, float w, float h, int flags) {
    bool clicked;
    u32 text = uip_button_frame(ui_id(label), x, y, w, h, flags, &clicked);
    ui_label(x + ui_px(4), y, w - ui_px(8), h, label, UI_ALIGN_CENTER, text);
    return clicked;
}

bool ui_button(const char *label, float x, float y, float w, float h) { return ui_button_ex(label, x, y, w, h, 0); }

bool ui_icon_button_ex(const char *id, const char *name, const char *fallback, float x, float y, float size, int flags) {
    bool clicked;
    u32 col = uip_button_frame(ui_id(id ? id : ""), x, y, size, size, flags, &clicked);
    /* The icon takes about 60% of the button (24 px in a 40 px button), and has the button's parity (even in even,
     * odd in odd) so that its em box is centred on whole pixels. */
    float ipx = uip_roundf(size * 0.6f);
    if (fmodf(ipx + uip_roundf(size), 2.0f) != 0.0f) ipx += 1.0f;
    ui_icon(name, fallback, x + (size - ipx) * 0.5f, y + (size - ipx) * 0.5f, ipx, col);
    return clicked;
}

bool ui_icon_button(const char *id, const char *name, const char *fallback, float x, float y, float size, bool selected) {
    return ui_icon_button_ex(id, name, fallback, x, y, size, selected ? UI_SELECTED : 0);
}

bool ui_toggle(const char *label, float x, float y, float w, float h, bool *value) {
    u32 id = ui_id(label);
    UiInteract r = ui_interact_id(id, x, y, w, h);
    float box = FS_MIN(h, ui_px(18));
    float bx = x, by = y + floorf((h - box) * 0.5f);
    float rad = ui_px(3);
    bool on = *value;
    if (on) {
        ui_rrect(bx, by, box, box, rad, r.hovered ? UI_COL_ACCENT_HOVER : UI_COL_ACCENT);
        float k = box / 18.0f; /* check mark drawn on an 18 px design grid */
        float chk[6] = { bx + 4.5f * k, by + 9.5f * k, bx + 7.5f * k, by + 12.5f * k, bx + 13.5f * k, by + 5.5f * k };
        ui_polyline(chk, 3, false, FS_MAX(1.6f, 1.8f * k), UI_COL_ON_ACCENT);
    } else {
        ui_rrect(bx, by, box, box, rad, r.hovered ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET);
        ui_rrect_outline(bx, by, box, box, rad, 1, UI_COL_BORDER);
    }
    ui_label(x + box + ui_px(8), y, w - box - ui_px(8), h, label, UI_ALIGN_LEFT, UI_COL_TEXT);
    if (r.clicked) *value = !*value;
    return r.clicked;
}

/* ---- text editing (single line) ------------------------------------------------------------------------------- */

/* Byte offset of the character boundary nearest to x (relative to the start of the text). */
static int uip_text_hit(const char *s, float x) {
    float acc = 0;
    int i = 0, n;
    while (s[i]) {
        u32 cp = uip_cp_at(s, i, &n);
        float a = ui_font_advance(UI_FONT_SANS, cp);
        if (x < acc + a * 0.5f) return i;
        acc += a;
        i += n;
    }
    return i;
}

/* Replace [lo, hi) of buf with `ins`; returns false (and does nothing) if it does not fit. */
static bool uip_te_replace(char *buf, int cap, int lo, int hi, const char *ins, int ins_len) {
    int len = (int)strlen(buf);
    if (len - (hi - lo) + ins_len >= cap) return false;
    memmove(buf + lo + ins_len, buf + hi, (size_t)(len - hi + 1));
    memcpy(buf + lo, ins, (size_t)ins_len);
    return true;
}

static void uip_te_end(UipTextEdit *te) {
    free(te->orig);
    te->orig = NULL;
    te->id = 0;
    te->dragging = false;
}

/* The shared engine of ui_text_field and the "type a number" mode of the drag widgets.  Returns UI_TEXT_* bits. */
static int uip_textedit(u32 id, float x, float y, float w, float h, char *buf, int cap, int flags, const char *placeholder) {
    UipTextEdit *te = &ui.te;
    const UiInput *in = &ui.in;
    int ev = 0;
    bool readonly = (flags & UI_TF_READONLY) != 0;
    double now = in->time;

    if (te->id == id && !ui_has_focus(id)) { ev |= UI_TEXT_BLURRED; uip_te_end(te); } /* lost the focus since last frame */
    else if (ui.te_lost == id && !ui_has_focus(id)) { ev |= UI_TEXT_BLURRED; ui.te_lost = 0; } /* (to another field) */

    UiInteract r = ui_interact_id(id, x, y, w, h);
    if (r.hovered) ui_set_cursor(UI_CURSOR_IBEAM);
    float pad = ui_px(8), tx = x + pad, inner_w = w - 2 * pad;

    if (r.pressed) {
        ui_set_focus(id);
        te->dragging = true;
    }
    bool select_all_now = false;
    if (ui_has_focus(id) && te->id != id) { /* gained the focus (click, or ui_set_focus from outside) */
        if (te->id) {
            ui.te_lost = te->id;
            uip_te_end(te);
        }
        te->id = id;
        te->orig = xstrdup(buf);
        te->cursor = (int)strlen(buf);
        te->anchor = (flags & UI_TF_SELECT_ALL) ? 0 : te->cursor;
        te->scroll = 0;
        te->epoch = now;
        /* a field that selects everything on focus keeps it selected after the click that focused it (typing
           replaces the value); the next click places the caret */
        if ((flags & UI_TF_SELECT_ALL) && r.pressed) { select_all_now = true; te->dragging = false; }
    }
    bool focused = te->id == id && ui_has_focus(id);

    if (focused) {
        int len = (int)strlen(buf);
        if (te->cursor > len) te->cursor = len;
        if (te->anchor > len) te->anchor = len;
        /* mouse: place the caret, extend while dragging, double click = word, triple = everything */
        if (r.pressed && !select_all_now) {
            int off = uip_text_hit(buf, in->mouse_x - tx + te->scroll);
            if (in->click_count >= 3) { te->anchor = 0; te->cursor = len; }
            else if (in->click_count == 2) ui_word_at(buf, off, &te->anchor, &te->cursor);
            else { te->cursor = off; if (!(in->mods & UI_MOD_SHIFT)) te->anchor = off; }
            te->epoch = now;
        } else if (r.held && te->dragging && (r.dx != 0 || r.dy != 0) && in->click_count < 2) {
            te->cursor = uip_text_hit(buf, in->mouse_x - tx + te->scroll);
            te->epoch = now;
        }
        if (!r.held) te->dragging = false;

        /* keyboard, in order */
        for (int i = 0; i < in->nkeys; i++) {
            if (ui.key_used[i]) continue;
            UiKeyEvent e = in->keys[i];
            bool shift = (e.mods & UI_MOD_SHIFT) != 0, mod = (e.mods & UI_MOD_MOD) != 0, word = (e.mods & UI_MOD_WORD) != 0;
            bool handled = true, edited = false;
            int lo = FS_MIN(te->cursor, te->anchor), hi = FS_MAX(te->cursor, te->anchor);
            len = (int)strlen(buf);
            if (e.text) {
                char u[4];
                int n = utf8_encode(e.text, u);
                if (!readonly && uip_te_replace(buf, cap, lo, hi, u, n)) { te->cursor = te->anchor = lo + n; edited = true; }
            } else {
                switch (e.key) {
                case UI_KEY_LEFT:
                    if (lo != hi && !shift && !word) te->cursor = lo;
                    else te->cursor = word ? ui_word_left(buf, te->cursor) : ui_utf8_prev(buf, te->cursor);
                    if (!shift) te->anchor = te->cursor;
                    break;
                case UI_KEY_RIGHT:
                    if (lo != hi && !shift && !word) te->cursor = hi;
                    else te->cursor = word ? ui_word_right(buf, te->cursor) : ui_utf8_next(buf, te->cursor);
                    if (!shift) te->anchor = te->cursor;
                    break;
                case UI_KEY_HOME: te->cursor = 0; if (!shift) te->anchor = 0; break;
                case UI_KEY_END: te->cursor = len; if (!shift) te->anchor = len; break;
                case UI_KEY_BACKSPACE:
                    if (readonly) break;
                    if (lo == hi) lo = word ? ui_word_left(buf, hi) : ui_utf8_prev(buf, hi);
                    if (hi > lo && uip_te_replace(buf, cap, lo, hi, "", 0)) { te->cursor = te->anchor = lo; edited = true; }
                    break;
                case UI_KEY_DELETE:
                    if (readonly) break;
                    if (lo == hi) hi = word ? ui_word_right(buf, lo) : ui_utf8_next(buf, lo);
                    if (hi > lo && uip_te_replace(buf, cap, lo, hi, "", 0)) { te->cursor = te->anchor = lo; edited = true; }
                    break;
                case UI_KEY_A:
                    if (mod) { te->anchor = 0; te->cursor = len; } else handled = false;
                    break;
                case UI_KEY_C:
                case UI_KEY_X:
                    if (!mod) { handled = false; break; }
                    if (hi > lo) {
                        char *sel = xstrndup(buf + lo, (size_t)(hi - lo));
                        platform_set_clipboard(sel);
                        free(sel);
                        if (e.key == UI_KEY_X && !readonly && uip_te_replace(buf, cap, lo, hi, "", 0)) {
                            te->cursor = te->anchor = lo;
                            edited = true;
                        }
                    }
                    break;
                case UI_KEY_V:
                    if (!mod) { handled = false; break; }
                    if (!readonly) {
                        const char *clip = platform_get_clipboard();
                        if (clip) {
                            char *t = xstrdup(clip);
                            for (char *p = t; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
                            int n = (int)strlen(t);
                            if (uip_te_replace(buf, cap, lo, hi, t, n)) { te->cursor = te->anchor = lo + n; edited = true; }
                            free(t);
                        }
                    }
                    break;
                case UI_KEY_ENTER:
                    ev |= UI_TEXT_SUBMIT;
                    break;
                case UI_KEY_ESCAPE:
                    if (!readonly && te->orig && strcmp(te->orig, buf) != 0) {
                        snprintf(buf, (size_t)cap, "%s", te->orig);
                        ev |= UI_TEXT_CHANGED;
                    }
                    ev |= UI_TEXT_CANCEL;
                    break;
                default: handled = false;
                }
            }
            if (handled) { ui_key_consume(i); te->epoch = now; }
            if (edited) ev |= UI_TEXT_CHANGED;
            if (ev & (UI_TEXT_CANCEL | UI_TEXT_SUBMIT)) break;
        }
        if (ev & UI_TEXT_CANCEL) { ui_set_focus(0); uip_te_end(te); focused = false; }
        else if ((ev & UI_TEXT_SUBMIT) && !(flags & UI_TF_KEEP_FOCUS)) { ui_set_focus(0); uip_te_end(te); focused = false; }
    }

    /* ---- drawing ---- */
    float rad = ui_px(UIP_RADIUS);
    ui_rrect(x, y, w, h, rad, focused ? UI_COL_PANEL : r.hovered ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET);
    ui_rrect_outline(x, y, w, h, rad, 1, focused ? UI_COL_ACCENT : UI_COL_BORDER);
    float lh = ui_font_line_height(UI_FONT_SANS), ty = uip_text_y(UI_FONT_SANS, y, h);
    ui_push_clip(x + 1, y, w - 2, h);
    if (focused) {
        int len = (int)strlen(buf);
        float cx = ui_text_width(UI_FONT_SANS, buf, te->cursor);
        float total = ui_text_width(UI_FONT_SANS, buf, len);
        if (cx - te->scroll > inner_w - 2) te->scroll = cx - inner_w + 2;
        if (cx - te->scroll < 0) te->scroll = cx;
        if (total - te->scroll < inner_w - 2) te->scroll = FS_MAX(0.0f, total - inner_w + 2);
        int lo = FS_MIN(te->cursor, te->anchor), hi = FS_MAX(te->cursor, te->anchor);
        if (hi > lo) {
            float sx0 = tx - te->scroll + ui_text_width(UI_FONT_SANS, buf, lo);
            float sx1 = tx - te->scroll + ui_text_width(UI_FONT_SANS, buf, hi);
            ui_rect(sx0, ty, sx1 - sx0, lh, UI_COL_SELECTION);
        }
        ui_text(UI_FONT_SANS, tx - te->scroll, ty, buf, len, UI_COL_TEXT);
        double ph = fmod(now - te->epoch, 2 * UIP_BLINK_HALF_PERIOD);
        if (ph < UIP_BLINK_HALF_PERIOD) ui_rect(tx - te->scroll + cx, ty, FS_MAX(1.0f, ceilf(ui_scale())), lh, UI_COL_TEXT);
        ui.wake_in = FS_MIN(ui.wake_in, (ph < UIP_BLINK_HALF_PERIOD ? UIP_BLINK_HALF_PERIOD : 2 * UIP_BLINK_HALF_PERIOD) - ph);
    } else if (buf[0]) {
        ui_text(UI_FONT_SANS, tx, ty, buf, -1, readonly ? UI_COL_TEXT_DIM : UI_COL_TEXT);
    } else if (placeholder) {
        ui_text(UI_FONT_SANS, tx, ty, placeholder, -1, UI_COL_TEXT_OFF);
    }
    ui_pop_clip();
    return ev;
}

int ui_text_field_ex(const char *label, float x, float y, float w, float h, char *buf, int cap, int flags, const char *placeholder) {
    return uip_textedit(ui_id(label), x, y, w, h, buf, cap, flags, placeholder);
}

bool ui_text_field(const char *label, float x, float y, float w, float h, char *buf, int cap) {
    return (ui_text_field_ex(label, x, y, w, h, buf, cap, 0, NULL) & UI_TEXT_CHANGED) != 0;
}

/* ---- number widgets (slider, drag) -------------------------------------------------------------------------------- */

static void uip_format_double(char *out, int cap, double v) { /* shortest text that reads back exactly */
    snprintf(out, (size_t)cap, "%.15g", v);
    if (strtod(out, NULL) != v) snprintf(out, (size_t)cap, "%.17g", v);
}

static void uip_format_value(char *out, int cap, double v, const char *fmt, bool is_int) {
    if (is_int) snprintf(out, (size_t)cap, "%d", (int)floor(v + 0.5));
    else snprintf(out, (size_t)cap, fmt ? fmt : "%.4g", v);
}

static double uip_clamp_range(double v, double lo, double hi, bool is_int) {
    if (is_int) v = floor(v + 0.5);
    if (lo < hi) v = v < lo ? lo : v > hi ? hi : v;
    return v;
}

/* While `id` is in "type a number" mode: run the text field, apply the result.  Returns true while editing. */
static bool uip_number_editing(u32 id, float x, float y, float w, float h, double *v, double lo, double hi, bool is_int, bool *changed) {
    if (ui.num_edit_id != id) return false;
    int ev = uip_textedit(id, x, y, w, h, ui.num_buf, (int)sizeof ui.num_buf, UI_TF_SELECT_ALL, NULL);
    if (ev & (UI_TEXT_SUBMIT | UI_TEXT_BLURRED)) {
        char *end = NULL;
        double nv = strtod(ui.num_buf, &end);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end != ui.num_buf && *end == 0 && nv == nv) {
            nv = uip_clamp_range(nv, lo, hi, is_int);
            if (nv != *v) { *v = nv; *changed = true; }
        }
    }
    if (ev & (UI_TEXT_SUBMIT | UI_TEXT_CANCEL | UI_TEXT_BLURRED)) ui.num_edit_id = 0;
    return true;
}

static void uip_begin_number_edit(u32 id, double v, bool is_int) {
    ui.num_edit_id = id;
    if (is_int) snprintf(ui.num_buf, sizeof ui.num_buf, "%d", (int)floor(v + 0.5));
    else uip_format_double(ui.num_buf, (int)sizeof ui.num_buf, v);
    ui.active = 0; /* the double click's drag is over */
    ui_set_focus(id);
}

static void uip_number_labels(const char *label, float x, float y, float w, float h, const char *value_text) {
    float pad = ui_px(8);
    float vw = ui_text_width(UI_FONT_SANS, value_text, -1);
    ui_label(x + pad, y, w - 2 * pad - vw - ui_px(6), h, label, UI_ALIGN_LEFT, UI_COL_TEXT);
    ui_label(x + pad, y, w - 2 * pad, h, value_text, UI_ALIGN_RIGHT, UI_COL_TEXT);
}

static bool uip_drag_core(const char *label, float x, float y, float w, float h, double *v, double speed, double lo, double hi,
                          const char *fmt, bool is_int, bool log_scale) {
    u32 id = ui_id(label);
    bool changed = false;
    if (uip_number_editing(id, x, y, w, h, v, lo, hi, is_int, &changed)) return changed;
    UiInteract r = ui_interact_id(id, x, y, w, h);
    if (r.hovered || r.held) ui_set_cursor(UI_CURSOR_HRESIZE);
    if (r.double_clicked) {
        uip_begin_number_edit(id, *v, is_int);
        return false;
    }
    if (r.pressed) ui.drag_val = log_scale ? log(FS_MAX(*v, 1e-300)) : *v; /* (log scale: drag the logarithm) */
    if (r.held && r.dx != 0) {
        double sp = speed / ui_scale();
        if (ui.in.mods & UI_MOD_SHIFT) sp *= 0.1;
        ui.drag_val += (double)r.dx * sp;
        double nv = uip_clamp_range(log_scale ? exp(ui.drag_val) : ui.drag_val, lo, hi, is_int);
        if (lo < hi) ui.drag_val = log_scale ? log(nv) : FS_CLAMP(ui.drag_val, lo, hi);
        if (nv != *v) { *v = nv; changed = true; }
    }
    char text[64];
    uip_format_value(text, sizeof text, *v, fmt, is_int);
    float rad = ui_px(UIP_RADIUS);
    ui_rrect(x, y, w, h, rad, r.held ? UI_COL_WIDGET_ACTIVE : r.hovered ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET);
    ui_rrect_outline(x, y, w, h, rad, 1, r.held ? UI_COL_ACCENT : UI_COL_BORDER);
    uip_number_labels(label, x, y, w, h, text);
    return changed;
}

bool ui_drag_double(const char *label, float x, float y, float w, float h, double *v, double speed, double lo, double hi, const char *fmt) {
    return uip_drag_core(label, x, y, w, h, v, speed, lo, hi, fmt, false, false);
}

bool ui_drag_double_log(const char *label, float x, float y, float w, float h, double *v, double speed, double lo, double hi,
                        const char *fmt) {
    return uip_drag_core(label, x, y, w, h, v, speed, FS_MAX(lo, 1e-300), hi, fmt, false, true);
}

bool ui_drag_float(const char *label, float x, float y, float w, float h, float *v, float speed, float lo, float hi, const char *fmt) {
    double d = *v;
    bool c = uip_drag_core(label, x, y, w, h, &d, speed, lo, hi, fmt, false, false);
    if (c) *v = (float)d;
    return c;
}

bool ui_drag_int(const char *label, float x, float y, float w, float h, int *v, float speed, int lo, int hi) {
    double d = *v;
    bool c = uip_drag_core(label, x, y, w, h, &d, speed, lo, hi, NULL, true, false);
    if (c) *v = (int)d;
    return c;
}

static double uip_slider_t(double v, double lo, double hi, bool log_scale) {
    if (hi == lo) return 0;
    double t = log_scale ? (log(v) - log(lo)) / (log(hi) - log(lo)) : (v - lo) / (hi - lo);
    return FS_CLAMP(t, 0.0, 1.0);
}

static bool uip_slider_core(const char *label, float x, float y, float w, float h, double *v, double lo, double hi, const char *fmt,
                            int flags, bool is_int) {
    u32 id = ui_id(label);
    bool log_scale = (flags & UI_SLIDER_LOG) && lo > 0 && hi > lo;
    bool changed = false;
    if (uip_number_editing(id, x, y, w, h, v, FS_MIN(lo, hi), FS_MAX(lo, hi), is_int, &changed)) return changed;
    UiInteract r = ui_interact_id(id, x, y, w, h);
    if (r.hovered || r.held) ui_set_cursor(UI_CURSOR_HRESIZE);
    if (r.double_clicked) {
        uip_begin_number_edit(id, *v, is_int);
        return false;
    }
    if (r.held) { /* the value follows the mouse */
        double t = ((double)ui.in.mouse_x - (double)x) / (double)w;
        t = FS_CLAMP(t, 0.0, 1.0);
        double nv = log_scale ? exp(log(lo) + t * (log(hi) - log(lo))) : lo + t * (hi - lo);
        nv = uip_clamp_range(nv, FS_MIN(lo, hi), FS_MAX(lo, hi), is_int);
        if (nv != *v) { *v = nv; changed = true; }
    }
    char text[64];
    uip_format_value(text, sizeof text, *v, fmt, is_int);
    float rad = ui_px(UIP_RADIUS);
    ui_rrect(x, y, w, h, rad, r.hovered || r.held ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET);
    float fw = (float)(uip_slider_t(*v, lo, hi, log_scale) * (double)w);
    if (fw >= 1) {
        ui_push_clip(x, y, fw, h);
        ui_rrect(x, y, w, h, rad, UI_COL_ACCENT_DIM);
        ui_pop_clip();
        ui_rect(x + fw - 1, y + 2, 2, h - 4, UI_COL_ACCENT);
    }
    ui_rrect_outline(x, y, w, h, rad, 1, r.held ? UI_COL_ACCENT : UI_COL_BORDER);
    uip_number_labels(label, x, y, w, h, text);
    return changed;
}

bool ui_slider_double(const char *label, float x, float y, float w, float h, double *v, double lo, double hi, const char *fmt, int flags) {
    return uip_slider_core(label, x, y, w, h, v, lo, hi, fmt, flags, false);
}

bool ui_slider_float(const char *label, float x, float y, float w, float h, float *v, float lo, float hi, const char *fmt) {
    double d = *v;
    bool c = uip_slider_core(label, x, y, w, h, &d, lo, hi, fmt, 0, false);
    if (c) *v = (float)d;
    return c;
}

bool ui_slider_int(const char *label, float x, float y, float w, float h, int *v, int lo, int hi) {
    double d = *v;
    bool c = uip_slider_core(label, x, y, w, h, &d, lo, hi, NULL, 0, true);
    if (c) *v = (int)d;
    return c;
}
