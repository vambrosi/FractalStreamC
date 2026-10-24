/* test_ui.c - tests for platform.c (input layer) and ui.c (fonts, draw lists, widgets).
 *
 * Widgets are driven with synthetic UiInput frames (no window needed).  The CPU rasterizer in raster.c turns
 * draw lists into pixels so tests can check what is actually drawn, and "ui_preview" / "ui_icons_preview" write
 * PNGs (build/ui_preview*.png, build/ui_icons_preview*.png) for looking at the result by eye. */
#include "raster.c"

#define FS_TEST_UI_HARNESS 1 /* test_editor.c reuses the harness below (T, tu_*) */

/* ---- synthetic input harness ------------------------------------------------------------------------------ */

static UiInput T; /* the input the next frame will see */

static void tu_reset(float scale) {
    ui_reset_interaction();
    memset(&T, 0, sizeof T);
    T.fb_w = 800;
    T.fb_h = 600;
    T.content_scale = scale;
    T.mouse_in_window = true;
    T.focused = true;
    T.time = 10.0;
    T.dt = 1.0f / 60.0f;
    T.mouse_x = T.mouse_y = -100;
}

static void tu_begin(void) { ui_begin_frame(&T); }

static const GfxDrawList *tu_end(void) {
    const GfxDrawList *dl = ui_end_frame();
    /* end-of-frame bookkeeping of the platform layer: edges and events are per frame */
    memset(T.mouse_pressed, 0, sizeof T.mouse_pressed);
    memset(T.mouse_released, 0, sizeof T.mouse_released);
    T.nkeys = T.ntext = 0;
    T.scroll_x = T.scroll_y = 0;
    T.time += T.dt;
    return dl;
}

static void tu_move(float x, float y) { T.mouse_x = x; T.mouse_y = y; }
static void tu_press(int b, int clicks) { T.mouse_down[b] = true; T.mouse_pressed[b] = true; T.click_count = clicks; }
static void tu_release(int b) { T.mouse_down[b] = false; T.mouse_released[b] = true; }

static void tu_key(UiKey k, int mods) {
    UiKeyEvent e = { k, mods, false, 0 };
    T.keys[T.nkeys++] = e;
    T.mods = mods;
}

static void tu_type(const char *s) {
    int len = (int)strlen(s);
    for (int i = 0; i < len;) {
        u32 cp;
        i += utf8_decode(s + i, len - i, &cp);
        UiKeyEvent e = { UI_KEY_NONE, 0, false, cp };
        T.keys[T.nkeys++] = e;
        T.text[T.ntext++] = cp;
    }
}

/* Run an empty frame (time advances, edges clear). */
static void tu_idle(void) { tu_begin(); tu_end(); }

static int tu_atlas_updates; /* how many times the atlas was rebuilt and picked up through tu_atlas() */

/* What the app does right after ui_begin_frame: pick up a rebuilt atlas (cached for rendering the draw list). */
static RasterTexture tu_atlas(void) {
    RasterTexture t = { 0, 0, NULL };
    t.rgba = ui_take_atlas_update(&t.w, &t.h);
    static RasterTexture last;
    if (t.rgba) { last = t; tu_atlas_updates++; }
    return last;
}

/* Render a draw list over a background color. */
static RasterImage tu_render(const GfxDrawList *dl, u32 bg) {
    RasterTexture atlas = tu_atlas();
    RasterImage im = raster_new(T.fb_w, T.fb_h, bg);
    raster_draw(&im, dl, &atlas, NULL, 0);
    return im;
}

/* Count pixels of `im` inside the rectangle that differ from `bg`. */
static int tu_count_diff(const RasterImage *im, int x0, int y0, int x1, int y1, u32 bg) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (raster_pixel(im, x, y) != bg) n++;
    return n;
}

#define BGC 0xFF101010u

/* ---- fonts --------------------------------------------------------------------------------------------------- */

TEST(ui_atlas) {
    tu_reset(1.0f);
    int w = 0, h = 0;
    const u8 *px = ui_fonts_init(1.0f, &w, &h);
    CHECK(px != NULL);
    CHECK(w >= 128 && h >= 16 && w <= 4096 && h <= 4096);
    CHECK((w & (w - 1)) == 0 && (h & (h - 1)) == 0);
    for (int i = 0; i < 4; i++) CHECK_INT(px[(i * w + i) * 4 + 3], 255); /* white texel at (0,0) and around */
    CHECK_INT(px[0], 255);
    CHECK_INT(px[1], 255);
    CHECK_INT(px[2], 255);
    /* the atlas has glyph coverage somewhere outside the white block */
    int lit = 0;
    for (int i = 0; i < w * h; i++) if (px[i * 4 + 3] > 0) lit++;
    CHECK(lit > 5000);
    int w2 = 0, h2 = 0;
    ui_fonts_init(2.0f, &w2, &h2);
    CHECK(w2 * h2 >= 2 * w * h);
    CHECK(ui_take_atlas_update(&w2, &h2) != NULL);
    CHECK(ui_take_atlas_update(&w2, &h2) == NULL); /* only reported once */
    ui_fonts_init(1.0f, NULL, NULL);
}

TEST(ui_font_metrics) {
    tu_reset(1.0f);
    ui_fonts_init(1.0f, NULL, NULL);
    float lh = ui_font_line_height(UI_FONT_SANS), asc = ui_font_ascent(UI_FONT_SANS);
    CHECK(lh >= 15 && lh <= 24);
    CHECK(asc > 0 && asc < lh);
    CHECK_NEAR(lh, floorf(lh), 0); /* whole pixels */
    /* monospace: every ASCII glyph has the same whole-pixel advance */
    float m = ui_font_advance(UI_FONT_MONO, 'i');
    CHECK_NEAR(m, floorf(m), 0);
    CHECK_NEAR(ui_font_advance(UI_FONT_MONO, 'W'), m, 0);
    CHECK_NEAR(ui_font_advance(UI_FONT_MONO, ' '), m, 0);
    CHECK_NEAR(ui_font_advance(UI_FONT_MONO, 0x3C0 /* pi */), m, 0);
    CHECK_NEAR(ui_font_advance(UI_FONT_MONO, 0x2102 /* C */), m, 0);
    /* proportional */
    CHECK(ui_font_advance(UI_FONT_SANS, 'i') < ui_font_advance(UI_FONT_SANS, 'W'));
    /* additive widths, UTF-8 */
    CHECK_NEAR(ui_text_width(UI_FONT_SANS, "ab", -1), ui_font_advance(UI_FONT_SANS, 'a') + ui_font_advance(UI_FONT_SANS, 'b'), 1e-4);
    CHECK_NEAR(ui_text_width(UI_FONT_SANS, "a\xCE\xB8", -1), ui_font_advance(UI_FONT_SANS, 'a') + ui_font_advance(UI_FONT_SANS, 0x3B8), 1e-4);
    CHECK_NEAR(ui_text_width(UI_FONT_SANS, "abc", 2), ui_text_width(UI_FONT_SANS, "ab", -1), 1e-4);
    /* unknown codepoints draw as U+FFFD */
    CHECK_NEAR(ui_font_advance(UI_FONT_SANS, 0x4E2D), ui_font_advance(UI_FONT_SANS, 0xFFFD), 0);
    CHECK(ui_font_advance(UI_FONT_SANS, 0xFFFD) > 0);
    /* the hamburger exists in the sans font only; the mono font borrows it */
    CHECK(ui_font_advance(UI_FONT_MONO, 0x2630) > 0);
    /* scaling */
    ui_fonts_init(2.0f, NULL, NULL);
    CHECK(ui_font_line_height(UI_FONT_SANS) >= 2 * lh - 2 && ui_font_line_height(UI_FONT_SANS) <= 2 * lh + 2);
    CHECK_NEAR(ui_px(10), 20, 1e-5);
    ui_set_font_size(UI_FONT_MONO, 20);
    tu_reset(2.0f);
    tu_begin(); /* applies the new size */
    tu_end();
    CHECK(ui_font_advance(UI_FONT_MONO, 'a') >= 23); /* 20 px * 2 * 0.602 */
    ui_set_font_size(UI_FONT_MONO, 15);
    tu_reset(1.0f);
    tu_begin();
    tu_end();
    CHECK(ui_font_advance(UI_FONT_MONO, 'a') <= 10);
}

/* ---- draw lists ---------------------------------------------------------------------------------------------- */

TEST(ui_draw_text_counts) {
    tu_reset(1.0f);
    tu_begin();
    ui_text(UI_FONT_SANS, 10, 10, "Hi there", -1, 0xFFFFFFFFu);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 7 * 4); /* the space has no bitmap */
    CHECK_INT(dl->nindices, 7 * 6);
    CHECK_INT(dl->ncmds, 1);
    CHECK_INT(dl->cmds[0].texture, 0);
    CHECK_INT(dl->cmds[0].index_count, 7 * 6);
    for (int i = 0; i < dl->nindices; i++) CHECK(dl->indices[i] < (u32)dl->nverts);
    /* glyph quads sit on whole pixels */
    for (int i = 0; i < dl->nverts; i++) {
        CHECK_NEAR(dl->verts[i].x, floorf(dl->verts[i].x), 0);
        CHECK_NEAR(dl->verts[i].y, floorf(dl->verts[i].y), 0);
    }
}

TEST(ui_draw_rect_and_cmds) {
    tu_reset(1.0f);
    tu_begin();
    ui_rect(10.4f, 10.6f, 20, 20, 0xFF0000FFu);
    ui_rect(40, 10, 20, 20, 0xFF00FF00u);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 8);
    CHECK_INT(dl->nindices, 12);
    CHECK_INT(dl->ncmds, 1); /* same texture and clip: one draw call */
    CHECK_NEAR(dl->verts[0].x, 10, 0);
    CHECK_NEAR(dl->verts[0].y, 11, 0);
    CHECK_NEAR(dl->verts[2].x, 30, 0);
    CHECK_NEAR(dl->verts[2].y, 31, 0);
    /* solid shapes sample the white texel */
    int aw, ah;
    ui_fonts_init(1.0f, &aw, &ah);
    CHECK_NEAR(dl->verts[0].u, 0.5f / (float)aw, 1e-7);
    CHECK_NEAR(dl->verts[0].v, 0.5f / (float)ah, 1e-7);
}

TEST(ui_clip_and_layers) {
    tu_reset(1.0f);
    tu_begin();
    ui_layer(UI_LAYER_TOOLTIP);
    ui_rect(0, 0, 10, 10, 0xFF0000FFu); /* first call, but on top */
    ui_layer(UI_LAYER_BASE);
    ui_push_clip(100, 100, 50, 50);
    ui_rect(90, 90, 100, 100, 0xFF00FF00u);
    ui_push_clip(120, 120, 100, 100); /* nested: intersection */
    ui_rect(0, 0, 800, 600, 0xFFFF0000u);
    ui_pop_clip();
    ui_pop_clip();
    ui_rect(300, 300, 10, 10, 0xFFFFFFFFu);
    ui_push_clip(0, 0, 10, 10);
    ui_rect(500, 500, 10, 10, 0xFFFFFFFFu); /* completely clipped: not emitted at all */
    ui_pop_clip();
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 4 * 4);
    CHECK_INT(dl->ncmds, 3); /* the unclipped base rect and the tooltip layer's rect share a draw call */
    /* base layer first, tooltip layer last */
    CHECK_NEAR(dl->cmds[0].clip_x0, 100, 0);
    CHECK_NEAR(dl->cmds[0].clip_x1, 150, 0);
    CHECK_NEAR(dl->cmds[1].clip_x0, 120, 0);
    CHECK_NEAR(dl->cmds[1].clip_x1, 150, 0);
    CHECK_NEAR(dl->cmds[1].clip_y1, 150, 0);
    CHECK_NEAR(dl->cmds[2].clip_x1, 800, 0);
    CHECK_INT(dl->cmds[2].index_count, 12);
    CHECK(dl->verts[dl->indices[dl->cmds[2].first_index]].rgba == 0xFFFFFFFFu);
    CHECK(dl->verts[dl->indices[dl->cmds[2].first_index + 6]].rgba == 0xFF0000FFu); /* tooltip rect drawn last */
    /* the tooltip layer is not clipped by the base layer's clip stack */
    tu_begin();
    ui_push_clip(10, 10, 5, 5);
    int prev = ui_layer(UI_LAYER_TOOLTIP);
    ui_rect(0, 0, 100, 100, 0xFFFFFFFFu);
    ui_layer(prev);
    ui_pop_clip();
    dl = tu_end();
    CHECK_INT(dl->ncmds, 1);
    CHECK_NEAR(dl->cmds[0].clip_x1, 800, 0);
}

TEST(ui_image_command) {
    tu_reset(1.0f);
    tu_begin();
    ui_rect(0, 0, 5, 5, 0xFFFFFFFFu);
    ui_image(3, 10, 10, 100, 80);
    ui_rect(0, 0, 5, 5, 0xFFFFFFFFu);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->ncmds, 3);
    CHECK_INT(dl->cmds[1].texture, 3);
    CHECK_INT(dl->cmds[1].index_count, 6);
    CHECK_NEAR(dl->verts[dl->indices[dl->cmds[1].first_index]].u, 0, 0);
    CHECK_NEAR(dl->verts[dl->indices[dl->cmds[1].first_index + 1]].u, 1, 0);
}

/* ---- rasterized checks ------------------------------------------------------------------------------------------ */

TEST(ui_raster_rect_exact) {
    tu_reset(1.0f);
    tu_begin();
    ui_rect(10, 10, 20, 20, UI_RGBA(255, 0, 0, 128)); /* translucent: a seam would show as a double-blended diagonal */
    const GfxDrawList *dl = tu_end();
    RasterImage im = tu_render(dl, BGC);
    u32 inside = raster_pixel(&im, 15, 15);
    CHECK(inside != BGC);
    for (int y = 10; y < 30; y++)
        for (int x = 10; x < 30; x++) CHECK_INT(raster_pixel(&im, x, y), inside);
    CHECK_INT(tu_count_diff(&im, 0, 0, 800, 600, BGC), 400);
    raster_free(&im);
}

TEST(ui_raster_shapes) {
    tu_reset(1.0f);
    tu_begin();
    ui_disk(100, 100, 20, 0xFFFFFFFFu);
    ui_rrect(200, 100, 60, 40, 10, 0xFFFFFFFFu);
    ui_line(300, 100.0f, 360, 100.0f, 2, 0xFFFFFFFFu);  /* straddles rows 99 and 100 */
    ui_line(300, 120.5f, 360, 120.5f, 1, 0xFFFFFFFFu);  /* exactly row 120 */
    ui_circle(450, 100, 20, 2, 0xFFFFFFFFu);
    ui_triangle(500, 100, 560, 100, 500, 160, 0xFFFFFFFFu);
    const GfxDrawList *dl = tu_end();
    RasterImage im = tu_render(dl, 0xFF000000u);
    /* disk: area close to pi r^2, center solid, outside empty */
    double cover = 0;
    for (int y = 70; y < 130; y++)
        for (int x = 70; x < 130; x++) cover += (double)(raster_pixel(&im, x, y) & 255) / 255.0;
    CHECK_NEAR(cover, 3.14159265 * 400, 12);
    CHECK_INT(raster_pixel(&im, 100, 100) & 255, 255);
    CHECK_INT(raster_pixel(&im, 100, 70) & 255, 0);
    /* rounded rectangle: sharp interior, cut corner, crisp straight edges */
    CHECK_INT(raster_pixel(&im, 230, 120) & 255, 255);
    CHECK_INT(raster_pixel(&im, 200, 100) & 255, 0);   /* the very corner is outside the arc */
    CHECK_INT(raster_pixel(&im, 230, 100) & 255, 255); /* top edge row is fully covered */
    CHECK_INT(raster_pixel(&im, 230, 99) & 255, 0);
    /* lines */
    CHECK_NEAR(raster_pixel(&im, 330, 99) & 255, 255, 3);
    CHECK_NEAR(raster_pixel(&im, 330, 100) & 255, 255, 3);
    CHECK_INT(raster_pixel(&im, 330, 98) & 255, 0);
    CHECK_INT(raster_pixel(&im, 330, 101) & 255, 0);
    CHECK_INT(raster_pixel(&im, 330, 120) & 255, 255);
    CHECK_INT(raster_pixel(&im, 330, 119) & 255, 0);
    CHECK_INT(raster_pixel(&im, 330, 121) & 255, 0);
    /* circle outline: ring pixel on, center off */
    CHECK(raster_pixel(&im, 450 + 19, 100) & 255);
    CHECK_INT(raster_pixel(&im, 450, 100) & 255, 0);
    /* triangle */
    CHECK_INT(raster_pixel(&im, 510, 110) & 255, 255);
    CHECK_INT(raster_pixel(&im, 555, 150) & 255, 0);
    raster_free(&im);
}

TEST(ui_raster_text_and_clip) {
    tu_reset(1.0f);
    tu_begin();
    ui_text(UI_FONT_SANS, 20, 20, "Hello", -1, 0xFFFFFFFFu);
    ui_push_clip(20, 60, 30, 30);
    ui_text(UI_FONT_SANS, 20, 60, "Clipped text goes on and on", -1, 0xFFFFFFFFu);
    ui_pop_clip();
    const GfxDrawList *dl = tu_end();
    RasterImage im = tu_render(dl, 0xFF000000u);
    int lh = (int)ui_font_line_height(UI_FONT_SANS);
    CHECK(tu_count_diff(&im, 20, 20, 20 + 40, 20 + lh, 0xFF000000u) > 40); /* glyph pixels */
    CHECK_INT(tu_count_diff(&im, 0, 0, 800, 19, 0xFF000000u), 0);
    CHECK(tu_count_diff(&im, 20, 60, 50, 90, 0xFF000000u) > 10);
    CHECK_INT(tu_count_diff(&im, 50, 55, 400, 100, 0xFF000000u), 0); /* nothing outside the scissor */
    raster_free(&im);
}

/* ---- widgets ------------------------------------------------------------------------------------------------------ */

TEST(ui_button_click) {
    tu_reset(1.0f);
    bool clicked = false, any = false;
    /* hover */
    tu_move(60, 40);
    tu_begin(); clicked = ui_button("Run", 20, 20, 100, 30); bool hov = ui_last_hovered(); bool wm = ui_wants_mouse(); tu_end();
    CHECK(!clicked && hov && wm);
    /* press */
    tu_press(0, 1);
    tu_begin(); clicked = ui_button("Run", 20, 20, 100, 30); CHECK(ui_active_id() != 0); tu_end();
    CHECK(!clicked);
    tu_begin(); clicked = ui_button("Run", 20, 20, 100, 30); tu_end(); /* still held */
    CHECK(!clicked);
    /* release inside: click */
    tu_release(0);
    tu_begin(); clicked = ui_button("Run", 20, 20, 100, 30); tu_end();
    CHECK(clicked);
    CHECK_INT(ui_active_id(), 0);
    /* press, drag out, release: no click */
    tu_press(0, 1);
    tu_begin(); ui_button("Run", 20, 20, 100, 30); tu_end();
    tu_move(300, 300);
    tu_begin(); any |= ui_button("Run", 20, 20, 100, 30); CHECK(ui_wants_mouse()); /* still dragging it */ tu_end();
    tu_release(0);
    tu_begin(); any |= ui_button("Run", 20, 20, 100, 30); tu_end();
    CHECK(!any);
    /* press and release in the same frame still clicks */
    tu_move(60, 40);
    tu_press(0, 1);
    tu_release(0);
    tu_begin(); clicked = ui_button("Run", 20, 20, 100, 30); tu_end();
    CHECK(clicked);
    /* disabled buttons do not click but still block the mouse */
    tu_idle();
    tu_press(0, 1); tu_release(0);
    tu_begin(); clicked = ui_button_ex("Off", 20, 20, 100, 30, UI_DISABLED); CHECK(ui_wants_mouse()); tu_end();
    CHECK(!clicked);
    /* mouse elsewhere: the UI does not want it */
    tu_move(400, 400);
    tu_begin(); ui_button("Run", 20, 20, 100, 30); CHECK(!ui_wants_mouse()); tu_end();
}

TEST(ui_layers_get_mouse_first) {
    tu_reset(1.0f);
    tu_move(60, 40);
    bool low = false, high = false;
    for (int frame = 0; frame < 3; frame++) {
        if (frame == 2) { tu_press(0, 1); tu_release(0); }
        tu_begin();
        low = ui_button("Low", 20, 20, 100, 30);          /* base layer, processed first */
        int prev = ui_layer(UI_LAYER_MENU);
        high = ui_button("High", 40, 20, 100, 30);        /* overlaps, on top */
        ui_layer(prev);
        tu_end();
    }
    CHECK(high);
    CHECK(!low);
    /* a panel blocks widgets beneath even where it has no widget */
    tu_reset(1.0f);
    tu_move(200, 200);
    bool b = false;
    for (int frame = 0; frame < 3; frame++) {
        if (frame == 2) { tu_press(0, 1); tu_release(0); }
        tu_begin();
        b = ui_button("Under", 150, 150, 100, 100);
        ui_layer(UI_LAYER_POPUP);
        ui_panel(100, 100, 300, 300);
        ui_layer(UI_LAYER_BASE);
        tu_end();
    }
    CHECK(!b);
}

TEST(ui_toggle_widget) {
    tu_reset(1.0f);
    bool v = false;
    tu_move(30, 30);
    for (int i = 0; i < 2; i++) { tu_begin(); ui_toggle("Show grid", 20, 20, 200, 24, &v); tu_end(); }
    tu_press(0, 1); tu_begin(); ui_toggle("Show grid", 20, 20, 200, 24, &v); tu_end();
    tu_release(0);
    tu_begin(); bool c = ui_toggle("Show grid", 20, 20, 200, 24, &v); tu_end();
    CHECK(c && v);
    tu_press(0, 1); tu_release(0);
    tu_begin(); c = ui_toggle("Show grid", 20, 20, 200, 24, &v); tu_end();
    CHECK(c && !v);
}

TEST(ui_slider_values) {
    tu_reset(1.0f);
    float f = 0;
    int n = 0;
    double lg = 1;
    tu_move(120, 30); /* 3/4 of the way along x = 20..220 -> 170 */
    tu_move(170, 30);
    tu_press(0, 1);
    tu_begin();
    bool c1 = ui_slider_float("A", 20, 20, 200, 20, &f, 0, 10, "%.2f");
    bool c2 = ui_slider_int("B", 20, 50, 200, 20, &n, 0, 8);
    tu_end();
    CHECK(c1);
    CHECK_NEAR(f, 7.5, 1e-4);
    CHECK(!c2);
    tu_release(0);
    tu_begin(); ui_slider_float("A", 20, 20, 200, 20, &f, 0, 10, "%.2f"); tu_end();
    /* drag beyond the right end clamps; value follows while the mouse is outside the widget */
    tu_press(0, 1);
    tu_begin(); ui_slider_float("A", 20, 20, 200, 20, &f, 0, 10, "%.2f"); tu_end();
    tu_move(700, 500);
    tu_begin(); ui_slider_float("A", 20, 20, 200, 20, &f, 0, 10, "%.2f"); tu_end();
    CHECK_NEAR(f, 10, 1e-6);
    tu_move(-50, 0);
    tu_begin(); ui_slider_float("A", 20, 20, 200, 20, &f, 0, 10, "%.2f"); tu_end();
    CHECK_NEAR(f, 0, 1e-6);
    tu_release(0);
    tu_idle();
    /* integers snap */
    tu_move(20 + 200 * 0.51f, 60);
    tu_press(0, 1);
    tu_begin(); ui_slider_int("B", 20, 50, 200, 20, &n, 0, 8); tu_end();
    CHECK_INT(n, 4);
    tu_release(0);
    tu_idle();
    /* logarithmic: the middle of the track is the geometric mean */
    tu_move(120, 90);
    tu_press(0, 1);
    tu_begin(); ui_slider_double("L", 20, 80, 200, 20, &lg, 1, 10000, "%.3g", UI_SLIDER_LOG); tu_end();
    CHECK_NEAR(lg, 100, 1e-6);
    tu_release(0);
    tu_idle();
}

TEST(ui_drag_and_type) {
    tu_reset(1.0f);
    double v = 1.0;
    int iv = 5;
    tu_move(100, 30);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    tu_press(0, 1);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK_NEAR(v, 1.0, 0);
    tu_move(110, 30); /* +10 px at 0.5 per px */
    tu_begin(); bool c = ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK(c);
    CHECK_NEAR(v, 6.0, 1e-9);
    T.mods = UI_MOD_SHIFT; /* fine control */
    tu_move(120, 30);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK_NEAR(v, 6.5, 1e-9);
    T.mods = 0;
    tu_release(0);
    tu_idle();
    /* clamped integer drag */
    tu_move(100, 60);
    tu_press(0, 1);
    tu_begin(); ui_drag_int("N", 20, 50, 200, 24, &iv, 0.1f, 0, 7); tu_end();
    tu_move(160, 60); /* +60 px * 0.1 = +6, clamped to 7 */
    tu_begin(); ui_drag_int("N", 20, 50, 200, 24, &iv, 0.1f, 0, 7); tu_end();
    CHECK_INT(iv, 7);
    tu_release(0);
    tu_idle();
    /* double click: type a number */
    tu_move(100, 30);
    tu_press(0, 2);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    tu_release(0);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); CHECK(ui_wants_keyboard()); tu_end();
    tu_type("0.25");
    tu_begin(); c = ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK(!c); /* not applied until Enter */
    tu_key(UI_KEY_ENTER, 0);
    tu_begin(); c = ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK(c);
    CHECK_NEAR(v, 0.25, 0);
    CHECK(!ui_wants_keyboard());
    /* Escape cancels, garbage is ignored */
    tu_press(0, 2); tu_release(0);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    tu_type("9");
    tu_key(UI_KEY_ESCAPE, 0);
    tu_begin(); c = ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK(!c);
    CHECK_NEAR(v, 0.25, 0);
    tu_press(0, 2); tu_release(0);
    tu_begin(); ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    tu_type("abc");
    tu_key(UI_KEY_ENTER, 0);
    tu_begin(); c = ui_drag_double("Radius", 20, 20, 200, 24, &v, 0.5, 0, 0, "%.3f"); tu_end();
    CHECK(!c);
    CHECK_NEAR(v, 0.25, 0);
}

TEST(ui_drag_log_scale) {
    tu_reset(1.0f);
    double r = 1e-9;
    tu_move(100, 30);
    tu_begin(); ui_drag_double_log("Radius", 20, 20, 200, 24, &r, 0.01, 1e-30, 1e30, "%.4g"); tu_end();
    tu_press(0, 1);
    tu_begin(); ui_drag_double_log("Radius", 20, 20, 200, 24, &r, 0.01, 1e-30, 1e30, "%.4g"); tu_end();
    tu_move(169.3147f, 30); /* + 69.3 px: times exp(0.693) = 2 */
    tu_begin(); bool c = ui_drag_double_log("Radius", 20, 20, 200, 24, &r, 0.01, 1e-30, 1e30, "%.4g"); tu_end();
    CHECK(c);
    CHECK_NEAR(r / 2e-9, 1, 1e-4);
    tu_move(-1000, 30); /* far to the left: tiny, but positive */
    tu_begin(); ui_drag_double_log("Radius", 20, 20, 200, 24, &r, 0.01, 1e-30, 1e30, "%.4g"); tu_end();
    CHECK(r > 0);
    CHECK_NEAR(r / (1e-9 * exp(-0.01 * 1100)), 1, 1e-3);
    tu_move(100, 30); /* back where the drag started */
    tu_begin(); ui_drag_double_log("Radius", 20, 20, 200, 24, &r, 0.01, 1e-30, 1e30, "%.4g"); tu_end();
    CHECK_NEAR(r / 1e-9, 1, 1e-6);
    tu_release(0);
    tu_idle();
}

/* A text field driven for one frame; returns the UI_TEXT_* bits. */
static int tu_field(char *buf, int cap) {
    tu_begin();
    int ev = ui_text_field_ex("field", 20, 20, 200, 26, buf, cap, 0, NULL);
    tu_end();
    return ev;
}

static void tu_click_field(float x) {
    tu_move(x, 32);
    tu_idle();
    tu_press(0, 1);
    tu_release(0);
}

TEST(ui_text_field_editing) {
    tu_reset(1.0f);
    char buf[32] = "";
    tu_click_field(100);
    tu_field(buf, sizeof buf);
    CHECK(ui_wants_keyboard());
    tu_type("abc");
    CHECK(tu_field(buf, sizeof buf) & UI_TEXT_CHANGED);
    CHECK_STR(buf, "abc");
    tu_key(UI_KEY_BACKSPACE, 0);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "ab");
    tu_key(UI_KEY_HOME, 0);
    tu_type("X");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "Xab");
    tu_key(UI_KEY_RIGHT, 0); tu_key(UI_KEY_DELETE, 0);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "Xa");
    /* selection with shift, typing replaces it */
    tu_key(UI_KEY_END, 0);
    tu_key(UI_KEY_LEFT, UI_MOD_SHIFT); tu_key(UI_KEY_LEFT, UI_MOD_SHIFT);
    tu_type("Q");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "Q");
    /* order of events inside one frame is preserved */
    tu_type("12"); tu_key(UI_KEY_BACKSPACE, 0); tu_type("3");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "Q13");
    /* clipboard */
    tu_key(UI_KEY_A, UI_MOD_MOD);
    tu_key(UI_KEY_C, UI_MOD_MOD);
    tu_field(buf, sizeof buf);
    CHECK_STR(platform_get_clipboard(), "Q13");
    tu_key(UI_KEY_END, 0);
    tu_key(UI_KEY_V, UI_MOD_MOD);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "Q13Q13");
    tu_key(UI_KEY_A, UI_MOD_MOD);
    tu_key(UI_KEY_X, UI_MOD_MOD);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "");
    CHECK_STR(platform_get_clipboard(), "Q13Q13");
    /* UTF-8: backspace removes a whole character; word movement */
    tu_type("h\xC3\xA9llo \xCE\xB8\xCE\xB1");
    tu_field(buf, sizeof buf);
    tu_key(UI_KEY_BACKSPACE, 0);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "h\xC3\xA9llo \xCE\xB8");
    tu_key(UI_KEY_LEFT, UI_MOD_WORD);
    tu_type("_");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "h\xC3\xA9llo _\xCE\xB8");
    /* capacity is respected */
    char small[4] = "";
    tu_key(UI_KEY_A, UI_MOD_MOD);
    tu_field(buf, sizeof buf);
    tu_key(UI_KEY_DELETE, 0);
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "");
    tu_click_field(100);
    tu_field(small, sizeof small);
    tu_type("abcdef");
    tu_field(small, sizeof small);
    CHECK_STR(small, "abc");
}

TEST(ui_text_field_submit_cancel_focus) {
    tu_reset(1.0f);
    char buf[32] = "start";
    tu_click_field(100);
    tu_field(buf, sizeof buf);
    tu_type("!");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "start!");
    tu_key(UI_KEY_ESCAPE, 0);
    int ev = tu_field(buf, sizeof buf);
    CHECK(ev & UI_TEXT_CANCEL);
    CHECK_STR(buf, "start");
    CHECK(!ui_wants_keyboard());
    /* Enter submits and releases the focus */
    tu_click_field(100);
    tu_field(buf, sizeof buf);
    tu_type("2");
    tu_key(UI_KEY_ENTER, 0);
    ev = tu_field(buf, sizeof buf);
    CHECK((ev & UI_TEXT_SUBMIT) && (ev & UI_TEXT_CHANGED));
    CHECK_STR(buf, "start2");
    CHECK(!ui_wants_keyboard());
    /* clicking elsewhere removes the focus and reports it */
    tu_click_field(100);
    tu_field(buf, sizeof buf);
    CHECK(ui_wants_keyboard());
    tu_move(500, 500);
    tu_press(0, 1);
    tu_field(buf, sizeof buf);
    tu_release(0);
    ev = tu_field(buf, sizeof buf);
    CHECK(ev & UI_TEXT_BLURRED);
    CHECK(!ui_wants_keyboard());
    /* typing with no focus does nothing */
    tu_type("zzz");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "start2");
    /* double click selects a word */
    snprintf(buf, sizeof buf, "foo bar");
    float bx = 20 + 8 + ui_text_width(UI_FONT_SANS, "foo ba", -1);
    tu_move(bx, 32);
    tu_idle();
    tu_press(0, 2);
    tu_field(buf, sizeof buf);
    tu_release(0);
    tu_type("X");
    tu_field(buf, sizeof buf);
    CHECK_STR(buf, "foo X");
}

/* A field that selects everything on focus: the click that focuses it keeps the selection (typing replaces the
   value), the next click places the caret. */
TEST(ui_text_field_select_all_on_click) {
    tu_reset(1.0f);
    char buf[32] = "1.5";
    int flags = UI_TF_SELECT_ALL;
    tu_click_field(30); /* near the left end: a plain field would put the caret before "1" */
    tu_begin(); ui_text_field_ex("field", 20, 20, 200, 26, buf, sizeof buf, flags, NULL); tu_end();
    tu_type("2i");
    tu_begin(); ui_text_field_ex("field", 20, 20, 200, 26, buf, sizeof buf, flags, NULL); tu_end();
    CHECK_STR(buf, "2i");
    tu_move(150, 32); /* already focused: the caret goes to the click (after the text) */
    tu_begin(); ui_text_field_ex("field", 20, 20, 200, 26, buf, sizeof buf, flags, NULL); tu_end();
    tu_press(0, 1);
    tu_release(0);
    tu_begin(); ui_text_field_ex("field", 20, 20, 200, 26, buf, sizeof buf, flags, NULL); tu_end();
    tu_type("+1");
    tu_key(UI_KEY_ENTER, 0);
    int ev = 0;
    tu_begin(); ev = ui_text_field_ex("field", 20, 20, 200, 26, buf, sizeof buf, flags, NULL); tu_end();
    CHECK_STR(buf, "2i+1");
    CHECK(ev & UI_TEXT_SUBMIT);
    char plain[32] = "abc"; /* without the flag a click places the caret */
    tu_click_field(150);
    tu_field(plain, sizeof plain);
    tu_type("d");
    tu_field(plain, sizeof plain);
    CHECK_STR(plain, "abcd");
}

TEST(ui_shortcuts_and_key_order) {
    tu_reset(1.0f);
    tu_key(UI_KEY_R, UI_MOD_MOD);
    tu_key(UI_KEY_S, UI_MOD_MOD | UI_MOD_SHIFT);
    tu_begin();
    CHECK(!ui_shortcut(UI_KEY_R, 0));
    CHECK(!ui_shortcut(UI_KEY_S, UI_MOD_MOD));
    CHECK(ui_shortcut(UI_KEY_R, UI_MOD_MOD));
    CHECK(!ui_shortcut(UI_KEY_R, UI_MOD_MOD)); /* consumed */
    CHECK(ui_shortcut(UI_KEY_S, UI_MOD_MOD | UI_MOD_SHIFT));
    tu_end();
    UiKeyEvent rep = { UI_KEY_Z, UI_MOD_MOD, true, 0 };
    T.keys[T.nkeys++] = rep;
    tu_begin();
    CHECK(!ui_shortcut(UI_KEY_Z, UI_MOD_MOD));
    CHECK(ui_shortcut_repeat(UI_KEY_Z, UI_MOD_MOD));
    tu_end();
    /* a focused text field consumes what it handles but not Ctrl+R */
    char buf[16] = "";
    tu_click_field(100);
    tu_field(buf, sizeof buf);
    tu_key(UI_KEY_R, UI_MOD_MOD);
    tu_type("a");
    tu_begin();
    ui_text_field("field", 20, 20, 200, 26, buf, sizeof buf);
    CHECK(ui_shortcut(UI_KEY_R, UI_MOD_MOD));
    int n;
    ui_key_events(&n);
    CHECK_INT(n, 2);
    CHECK(ui_key_consumed(1)); /* the typed 'a' */
    tu_end();
}

TEST(ui_tooltip_delay) {
    tu_reset(1.0f);
    tu_move(60, 30);
    int counts[3];
    double t_first = 0;
    for (int i = 0; i < 3; i++) {
        tu_begin();
        ui_button("Hover me", 20, 20, 100, 24);
        ui_tooltip("A helpful hint");
        if (i == 0) t_first = T.time;
        counts[i] = ui_end_frame()->nverts;
        T.time += (i == 0 ? 0.2 : 0.4);
    }
    CHECK(counts[1] == counts[0]);
    CHECK(counts[2] > counts[0]); /* shown after 0.5 s of hovering */
    (void)t_first;
    /* before the delay expires the UI asks to be woken up */
    tu_reset(1.0f);
    tu_move(60, 30);
    tu_begin(); ui_button("Hover me", 20, 20, 100, 24); ui_tooltip("hint"); CHECK(ui_redraw_in() > 0 && ui_redraw_in() <= 0.5); tu_end();
    /* moving away resets */
    tu_move(500, 500);
    tu_begin(); ui_button("Hover me", 20, 20, 100, 24); ui_tooltip("hint"); CHECK(ui_redraw_in() > 1e6); tu_end();
}

TEST(ui_ids_and_labels) {
    tu_reset(1.0f);
    tu_begin();
    u32 a = ui_id("x");
    ui_push_id("panel1");
    u32 b = ui_id("x");
    ui_push_id_int(7);
    u32 c = ui_id("x");
    ui_pop_id();
    CHECK_INT(ui_id("x"), b);
    ui_pop_id();
    CHECK_INT(ui_id("x"), a);
    CHECK(a != b && b != c && a != c);
    CHECK(ui_id("Run##a") != ui_id("Run##b"));
    CHECK(a != 0);
    /* "Name##hidden" draws only "Name" */
    ui_label(0, 0, 200, 20, "Run##a", UI_ALIGN_LEFT, 0xFFFFFFFFu);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 3 * 4);
}

TEST(ui_word_helpers) {
    const char *s = "foo  bar+baz\n\xCE\xB8x = 1";
    CHECK_INT(ui_word_right(s, 0), 3);
    CHECK_INT(ui_word_right(s, 3), 8);   /* skips blanks, then the word */
    CHECK_INT(ui_word_right(s, 8), 9);   /* '+' is its own run */
    CHECK_INT(ui_word_right(s, 12), 13); /* at the newline: step over it */
    CHECK_INT(ui_word_right(s, 13), 16); /* theta x is one word (2 + 1 bytes) */
    CHECK_INT(ui_word_left(s, 8), 5);
    CHECK_INT(ui_word_left(s, 5), 0);
    CHECK_INT(ui_word_left(s, 13), 12);
    CHECK_INT(ui_word_left(s, 16), 13);
    int a, b;
    ui_word_at(s, 6, &a, &b);
    CHECK(a == 5 && b == 8);
    ui_word_at(s, 3, &a, &b); /* inside the blanks */
    CHECK(a == 3 && b == 5);
    ui_word_at(s, 12, &a, &b); /* on the newline: the word before it */
    CHECK(a == 9 && b == 12);
    CHECK_INT(ui_utf8_next("\xCE\xB8z", 0), 2);
    CHECK_INT(ui_utf8_prev("\xCE\xB8z", 2), 0);
    CHECK_INT(ui_utf8_prev("abc", 0), 0);
}

TEST(ui_scroll_area_wheel) {
    tu_reset(1.0f);
    tu_move(50, 50);
    T.scroll_y = -2; /* wheel towards the user = scroll down */
    tu_begin();
    float d = ui_scroll_area(10, 10, 100, 100);
    float e = ui_scroll_area(300, 300, 100, 100);
    tu_end();
    CHECK(d > 0);
    CHECK_NEAR(e, 0, 0);
}

/* ---- platform input layer (no window needed) ------------------------------------------------------------------------- */

static void pi_reset(void) {
    sb_free(&plat.fake_clipboard);
    memset(&plat, 0, sizeof plat);
    plat.in.content_scale = 1;
}

static UiInput pi_frame(void) { /* finish the frame like platform_poll does */
    UiInput in;
    plat_end_frame(&in, 100.0);
    return in;
}

TEST(platform_key_mapping) {
    CHECK_INT(plat_map_key(GLFW_KEY_A), UI_KEY_A);
    CHECK_INT(plat_map_key(GLFW_KEY_Z), 'Z');
    CHECK_INT(plat_map_key(GLFW_KEY_0), UI_KEY_0);
    CHECK_INT(plat_map_key(GLFW_KEY_9), '9');
    CHECK_INT(plat_map_key(GLFW_KEY_SLASH), UI_KEY_SLASH);
    CHECK_INT(plat_map_key(GLFW_KEY_SPACE), UI_KEY_SPACE);
    CHECK_INT(plat_map_key(GLFW_KEY_LEFT), UI_KEY_LEFT);
    CHECK_INT(plat_map_key(GLFW_KEY_PAGE_DOWN), UI_KEY_PAGE_DOWN);
    CHECK_INT(plat_map_key(GLFW_KEY_ENTER), UI_KEY_ENTER);
    CHECK_INT(plat_map_key(GLFW_KEY_KP_ENTER), UI_KEY_ENTER);
    CHECK_INT(plat_map_key(GLFW_KEY_KP_3), UI_KEY_3);
    CHECK_INT(plat_map_key(GLFW_KEY_F1), UI_KEY_F1);
    CHECK_INT(plat_map_key(GLFW_KEY_F12), UI_KEY_F12);
    CHECK_INT(UI_KEY_F12 - UI_KEY_F1, 11);
    CHECK_INT(plat_map_key(GLFW_KEY_LEFT_SHIFT), UI_KEY_NONE);
    CHECK_INT(plat_map_key(GLFW_KEY_F20), UI_KEY_NONE);
    CHECK(UI_KEY_COUNT < 200);
    CHECK_INT(plat_map_mods(GLFW_MOD_SHIFT | GLFW_MOD_CONTROL), UI_MOD_SHIFT | UI_MOD_CTRL);
    CHECK_INT(plat_map_mods(GLFW_MOD_ALT | GLFW_MOD_SUPER | GLFW_MOD_CAPS_LOCK), UI_MOD_ALT | UI_MOD_SUPER);
}

TEST(platform_input_accumulation) {
    pi_reset();
    plat_begin_frame();
    /* "type a, Backspace, type b" keeps its order in one stream; modifier-only presses just update mods */
    plat_on_key(GLFW_KEY_LEFT_SHIFT, GLFW_PRESS, GLFW_MOD_SHIFT);
    plat_on_key(GLFW_KEY_A, GLFW_PRESS, GLFW_MOD_SHIFT);
    plat_on_char('A');
    plat_on_key(GLFW_KEY_A, GLFW_RELEASE, GLFW_MOD_SHIFT);
    plat_on_key(GLFW_KEY_BACKSPACE, GLFW_PRESS, GLFW_MOD_SHIFT);
    plat_on_key(GLFW_KEY_BACKSPACE, GLFW_REPEAT, GLFW_MOD_SHIFT);
    plat_on_char(0x3B8);
    UiInput in = pi_frame();
    CHECK_INT(in.nkeys, 5);
    CHECK(in.keys[0].key == UI_KEY_A && in.keys[0].text == 0 && (in.keys[0].mods & UI_MOD_SHIFT));
    CHECK(in.keys[1].key == UI_KEY_NONE && in.keys[1].text == 'A');
    CHECK(in.keys[2].key == UI_KEY_BACKSPACE && !in.keys[2].repeat);
    CHECK(in.keys[3].key == UI_KEY_BACKSPACE && in.keys[3].repeat);
    CHECK(in.keys[4].text == 0x3B8);
    CHECK_INT(in.ntext, 2);
    CHECK(in.text[0] == 'A' && in.text[1] == 0x3B8);
    CHECK(in.key_down[UI_KEY_BACKSPACE] && in.key_down[UI_KEY_A]); /* A was tapped inside this poll: down for one frame */
    CHECK_INT(in.mods, UI_MOD_SHIFT);
    /* next frame: events are gone, held keys and modifiers stay */
    plat_begin_frame();
    in = pi_frame();
    CHECK_INT(in.nkeys, 0);
    CHECK_INT(in.ntext, 0);
    CHECK(in.key_down[UI_KEY_BACKSPACE] && !in.key_down[UI_KEY_A]);
    CHECK_INT(in.mods, UI_MOD_SHIFT);
    /* Ctrl+letter is a shortcut, not text; AltGr (Ctrl+Alt) is text; Cmd+letter is a shortcut */
    plat_begin_frame();
    plat_on_key(GLFW_KEY_1, GLFW_PRESS, GLFW_MOD_CONTROL);
    plat_on_char('1');
    plat_on_key(GLFW_KEY_Q, GLFW_PRESS, GLFW_MOD_CONTROL | GLFW_MOD_ALT);
    plat_on_char('@');
    plat_on_key(GLFW_KEY_C, GLFW_PRESS, GLFW_MOD_SUPER);
    plat_on_char('c');
    plat_on_key(GLFW_KEY_ESCAPE, GLFW_PRESS, 0);
    plat_on_char(3); /* control characters never are text */
    in = pi_frame();
    CHECK_INT(in.ntext, 1);
    CHECK(in.text[0] == '@');
    CHECK_INT(in.nkeys, 5); /* 4 key presses + the AltGr character */
    /* overflow is dropped, not corrupted */
    plat_begin_frame();
    for (int i = 0; i < 100; i++) plat_on_key(GLFW_KEY_A, GLFW_PRESS, 0);
    in = pi_frame();
    CHECK_INT(in.nkeys, 64);
}

/* One poll with the given mouse events (cursor moves are applied first), like platform_poll would collect them. */
#define PI_PRESS(t) plat_on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0, (t))
#define PI_RELEASE(t) plat_on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0, (t))

TEST(platform_mouse_and_clicks) {
    pi_reset();
    plat_begin_frame();
    plat_on_cursor(100, 50);
    PI_PRESS(1.00);
    UiInput in = pi_frame();
    CHECK(in.mouse_down[0] && in.mouse_pressed[0] && !in.mouse_released[0]);
    CHECK_INT(in.click_count, 1);
    CHECK_NEAR(in.mouse_x, 100, 0);
    plat_begin_frame();
    PI_RELEASE(1.05);
    in = pi_frame();
    CHECK(!in.mouse_down[0] && !in.mouse_pressed[0] && in.mouse_released[0]);
    CHECK_INT(in.click_count, 1); /* the count belongs to the last press */
    /* quick second press nearby: double, then triple, then back to single (one press or release per frame here) */
    plat_begin_frame();
    plat_on_cursor(102, 51);
    PI_PRESS(1.20);
    in = pi_frame();
    CHECK_INT(in.click_count, 2);
    plat_begin_frame();
    PI_RELEASE(1.22);
    pi_frame();
    plat_begin_frame();
    PI_PRESS(1.40);
    in = pi_frame();
    CHECK_INT(in.click_count, 3);
    plat_begin_frame();
    PI_RELEASE(1.41);
    pi_frame();
    plat_begin_frame();
    PI_PRESS(1.60);
    in = pi_frame();
    CHECK_INT(in.click_count, 1);
    /* too slow, or too far: a single click again */
    plat_begin_frame();
    PI_RELEASE(1.61);
    pi_frame();
    plat_begin_frame();
    PI_PRESS(3.0);
    in = pi_frame();
    CHECK_INT(in.click_count, 1);
    plat_begin_frame();
    PI_RELEASE(3.01);
    pi_frame();
    plat_begin_frame();
    plat_on_cursor(300, 300);
    PI_PRESS(3.1);
    in = pi_frame();
    CHECK_INT(in.click_count, 1);
    /* other buttons, scroll accumulation, focus loss */
    plat_begin_frame();
    plat_on_mouse_button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0, 4.0);
    plat_on_scroll(0, 1);
    plat_on_scroll(0.5f, 2);
    in = pi_frame();
    CHECK(in.mouse_down[1] && in.mouse_pressed[1]);
    CHECK_NEAR(in.scroll_y, 3, 0);
    CHECK_NEAR(in.scroll_x, 0.5f, 0);
    plat_begin_frame();
    plat_on_focus(false);
    in = pi_frame();
    CHECK(!in.mouse_down[0] && !in.mouse_down[1] && !in.focused);
    CHECK(in.mouse_released[0] && in.mouse_released[1]); /* the app still sees the releases */
    /* the side buttons (back, forward): one frame each, nothing else changes */
    plat_begin_frame();
    plat_on_mouse_button(GLFW_MOUSE_BUTTON_4, GLFW_PRESS, 0, 5.0);
    in = pi_frame();
    CHECK(in.mouse_back && !in.mouse_forward && !in.mouse_pressed[0] && !in.mouse_pressed[1] && !in.mouse_pressed[2]);
    plat_begin_frame();
    plat_on_mouse_button(GLFW_MOUSE_BUTTON_4, GLFW_RELEASE, 0, 5.1);
    plat_on_mouse_button(GLFW_MOUSE_BUTTON_5, GLFW_PRESS, 0, 5.2);
    in = pi_frame();
    CHECK(!in.mouse_back && in.mouse_forward && !in.mouse_released[0]);
    plat_begin_frame();
    in = pi_frame();
    CHECK(!in.mouse_back && !in.mouse_forward);
}

/* A click that arrives completely inside one poll (xdotool, a quick tap while the app sleeps in glfwWaitEvents) must
 * not lose its press or its release: they are delivered in two consecutive frames, and the second poll must not block. */
TEST(platform_click_split_over_frames) {
    pi_reset();
    plat_begin_frame();
    plat_on_cursor(40, 30);
    PI_PRESS(1.0);
    PI_RELEASE(1.01);
    UiInput a = pi_frame();
    CHECK(a.mouse_pressed[0] && a.mouse_down[0] && !a.mouse_released[0]);
    CHECK_INT(a.click_count, 1);
    CHECK(plat_begin_frame()); /* something is held back: the poll must return at once */
    UiInput b = pi_frame();
    CHECK(!b.mouse_pressed[0] && b.mouse_released[0] && !b.mouse_down[0]);
    CHECK(!plat_begin_frame()); /* nothing left */
    UiInput c = pi_frame();
    CHECK(!c.mouse_pressed[0] && !c.mouse_released[0] && !c.mouse_down[0]);

    /* a double click inside one poll: press, release, press, release over four frames, counted as a double click */
    plat_begin_frame();
    PI_PRESS(2.00);
    PI_RELEASE(2.02);
    PI_PRESS(2.10);
    PI_RELEASE(2.12);
    UiInput f[4];
    f[0] = pi_frame();
    for (int i = 1; i < 4; i++) {
        CHECK(plat_begin_frame());
        f[i] = pi_frame();
    }
    CHECK(!plat_begin_frame());
    CHECK(f[0].mouse_pressed[0] && !f[0].mouse_released[0] && f[0].mouse_down[0] && f[0].click_count == 1);
    CHECK(!f[1].mouse_pressed[0] && f[1].mouse_released[0] && !f[1].mouse_down[0]);
    CHECK(f[2].mouse_pressed[0] && !f[2].mouse_released[0] && f[2].mouse_down[0] && f[2].click_count == 2);
    CHECK(!f[3].mouse_pressed[0] && f[3].mouse_released[0] && !f[3].mouse_down[0]);

    /* different buttons do not wait for each other; order is kept */
    pi_reset();
    plat_begin_frame();
    PI_PRESS(1.0);
    PI_RELEASE(1.01);
    plat_on_mouse_button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0, 1.02);
    a = pi_frame();
    CHECK(a.mouse_pressed[0] && !a.mouse_pressed[1]); /* the right press queued behind the left release */
    CHECK(plat_begin_frame());
    b = pi_frame();
    CHECK(b.mouse_released[0] && b.mouse_pressed[1] && b.mouse_down[1] && !b.mouse_down[0]);

    /* focus lost right after a press in the same poll: the release still comes one frame later */
    pi_reset();
    plat_begin_frame();
    PI_PRESS(1.0);
    plat_on_focus(false);
    a = pi_frame();
    CHECK(a.mouse_pressed[0] && !a.mouse_released[0]);
    CHECK(plat_begin_frame());
    b = pi_frame();
    CHECK(b.mouse_released[0] && !b.mouse_down[0]);
}

/* A key tapped inside one poll is seen down for one frame (its press event is in the stream either way). */
TEST(platform_key_tap_split) {
    pi_reset();
    plat_begin_frame();
    plat_on_key(GLFW_KEY_K, GLFW_PRESS, 0);
    plat_on_key(GLFW_KEY_K, GLFW_RELEASE, 0);
    UiInput a = pi_frame();
    CHECK(a.key_down[UI_KEY_K] && a.nkeys == 1 && a.keys[0].key == UI_KEY_K);
    CHECK(plat_begin_frame());
    UiInput b = pi_frame();
    CHECK(!b.key_down[UI_KEY_K] && b.nkeys == 0);
    CHECK(!plat_begin_frame());
    /* a tapped key that is pressed again in the same poll stays down until its real release */
    plat_begin_frame();
    plat_on_key(GLFW_KEY_K, GLFW_PRESS, 0);
    plat_on_key(GLFW_KEY_K, GLFW_RELEASE, 0);
    plat_on_key(GLFW_KEY_K, GLFW_PRESS, 0);
    a = pi_frame();
    CHECK(a.key_down[UI_KEY_K] && a.nkeys == 2);
    plat_begin_frame();
    b = pi_frame();
    CHECK(b.key_down[UI_KEY_K]);
    plat_begin_frame();
    plat_on_key(GLFW_KEY_K, GLFW_RELEASE, 0); /* held across frames: released at once */
    b = pi_frame();
    CHECK(!b.key_down[UI_KEY_K]);
    CHECK(!plat_begin_frame());
}

/* Random sequences of events cut into random polls: no frame ever shows a press and a release of one button, every
 * event is delivered exactly once and in order, and after enough empty polls the state is the logical one. */
TEST(platform_event_stress) {
    u32 rng = 12345;
#define RND(n) (rng = rng * 1664525u + 1013904223u, (int)((rng >> 8) % (u32)(n)))
    for (int round = 0; round < 200; round++) {
        pi_reset();
        bool down[3] = { false, false, false }; /* logical state after all events so far */
        int presses[3] = { 0, 0, 0 }, releases[3] = { 0, 0, 0 }, sent_p[3] = { 0, 0, 0 }, sent_r[3] = { 0, 0, 0 };
        bool prev_down[3] = { false, false, false };
        double t = 1.0;
        int polls = 1 + RND(8);
        for (int poll = 0; poll < polls || plat.nqueue > 0; poll++) { /* then empty polls flush the backlog */
            plat_begin_frame();
            int nev = poll < polls ? RND(6) : 0;
            for (int e = 0; e < nev; e++) {
                int b = RND(3);
                t += 0.01;
                down[b] = !down[b];
                if (down[b]) sent_p[b]++; else sent_r[b]++;
                plat_on_mouse_button(b, down[b] ? GLFW_PRESS : GLFW_RELEASE, 0, t);
            }
            UiInput in = pi_frame();
            for (int b = 0; b < 3; b++) {
                CHECK(!(in.mouse_pressed[b] && in.mouse_released[b]));
                presses[b] += in.mouse_pressed[b];
                releases[b] += in.mouse_released[b];
                /* the held state follows the edges */
                bool want = in.mouse_pressed[b] ? true : in.mouse_released[b] ? false : prev_down[b];
                CHECK(in.mouse_down[b] == want);
                prev_down[b] = in.mouse_down[b];
            }
        }
        for (int b = 0; b < 3; b++) {
            CHECK_INT(presses[b], sent_p[b]);
            CHECK_INT(releases[b], sent_r[b]);
            CHECK(prev_down[b] == down[b]);
        }
        CHECK_INT(plat.nqueue, 0);
    }
#undef RND
}

TEST(platform_dropped_files) {
    pi_reset();
    plat_begin_frame();
    const char *paths[2] = { "/tmp/a.fs", "/tmp/b c.fs" };
    plat_on_drop(2, paths);
    UiInput in = pi_frame();
    CHECK_INT(in.ndropped, 2);
    CHECK_STR(in.dropped_files[1], "/tmp/b c.fs");
    plat_begin_frame();
    in = pi_frame();
    CHECK_INT(in.ndropped, 0);
}

TEST(platform_clipboard_headless) {
    pi_reset();
    platform_set_clipboard("h\xC3\xA9llo\nworld");
    CHECK_STR(platform_get_clipboard(), "h\xC3\xA9llo\nworld");
    platform_set_clipboard("");
    CHECK_STR(platform_get_clipboard(), "");
    platform_shutdown(); /* frees the fake clipboard; safe without a window */
}

TEST(platform_window) {
    static char err[256]; /* SKIP keeps a pointer to the reason */
    err[0] = 0;
    if (!platform_init(640, 400, "FractalStreamC test", err, sizeof err)) {
        platform_shutdown();
        SKIP(err[0] ? err : "no display");
    }
    UiInput in;
    platform_poll(&in, false);
    CHECK(in.fb_w > 0 && in.fb_h > 0);
    CHECK(in.content_scale >= 1.0f);
    CHECK(platform_glfw_window() != NULL);
    CHECK(!platform_should_close());
    platform_set_title("renamed");
    platform_set_size(500, 300);
    platform_set_clipboard("clip \xCE\xB8");
    platform_poll_timeout(&in, 0.01);
    const char *c = platform_get_clipboard();
    CHECK(c != NULL);
    for (int i = 0; i < UI_CURSOR_COUNT; i++) platform_set_cursor((UiCursor)i);
    platform_set_cursor(UI_CURSOR_ARROW);
    int w, h;
    platform_get_size(&w, &h);
    CHECK(w > 0 && h > 0);
    platform_set_should_close(true);
    CHECK(platform_should_close());
    platform_shutdown();
    CHECK(platform_glfw_window() == NULL);
}

/* ---- the preview picture -------------------------------------------------------------------------------------------------- */

/* End to end on X11: a click synthesized with xdotool (press and release back to back, arriving while the app sleeps in
 * glfwWaitEventsTimeout) must reach the app as a press in one poll and the release in a later one. */
TEST(platform_window_click) {
#ifdef FS_OS_LINUX
    static char err[256];
    err[0] = 0;
    if (!platform_init(300, 200, "FractalStreamC click test", err, sizeof err)) {
        platform_shutdown();
        SKIP(err[0] ? err : "no display");
    }
    if (system("xdotool version >/dev/null 2>&1") != 0) {
        platform_shutdown();
        SKIP("xdotool is not installed");
    }
    UiInput in;
    for (int i = 0; i < 3; i++) platform_poll_timeout(&in, 0.1); /* let the window appear */
    int wx = 0, wy = 0;
    glfwGetWindowPos(platform_glfw_window(), &wx, &wy);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "(sleep 0.4; xdotool mousemove %d %d click 1) >/dev/null 2>&1 &", wx + 100, wy + 80);
    if (system(cmd) != 0) {
        platform_shutdown();
        SKIP("cannot run xdotool");
    }
    int pressed_frame = -1, released_frame = -1, sleeps = 0;
    double t0 = fs_time_seconds();
    for (int frame = 0; frame < 40 && fs_time_seconds() - t0 < 4.0 && released_frame < 0; frame++) {
        double before = fs_time_seconds();
        platform_poll_timeout(&in, 1.0); /* sleeps until the click arrives */
        sleeps += fs_time_seconds() - before > 0.2;
        CHECK(!(in.mouse_pressed[0] && in.mouse_released[0]));
        if (in.mouse_pressed[0] && pressed_frame < 0) pressed_frame = frame;
        if (in.mouse_released[0] && released_frame < 0) released_frame = frame;
    }
    platform_shutdown();
    if (pressed_frame < 0) SKIP("the synthesized click did not reach the window");
    CHECK(released_frame > pressed_frame);
    CHECK(sleeps <= 2); /* the release came from the held-back queue, not after another timeout */
#else
    SKIP("X11 only");
#endif
}

static void preview_hamburger(float x, float y, float s, u32 col) {
    for (int i = 0; i < 3; i++) ui_rect(x + s * 0.2f, y + s * (0.3f + 0.2f * (float)i), s * 0.6f, FS_MAX(2.0f, s * 0.08f), col);
}

/* A representative screen, scripted like a user: click the Title field, select all, hover the Run button until its
 * tooltip shows.  `scale` is the content scale; the last frame is saved as a PNG. */
static void preview_scene(float scale, const char *png_path, bool crop) {
    tu_reset(scale);
    T.fb_w = (int)(960 * scale);
    T.fb_h = (int)(640 * scale);
    float s = scale;
    char name[64] = "Mandelbrot \xCE\xB8";
    float radius = 2.5f, hue = 0.35f;
    int iters = 200;
    double zoom = 1.0;
    bool grid = true, smooth = false;
    char txt[64] = "z^2 + c";
    float title_pos[2] = { 0, 0 }, run_pos[2] = { 0, 0 };
    RasterImage im = { 0 };
    for (int frame = 0; frame < 5; frame++) {
        switch (frame) {
        case 1: tu_move(title_pos[0], title_pos[1]); tu_press(0, 1); tu_release(0); break;
        case 2: tu_key(UI_KEY_A, UI_MOD_MOD); break;
        case 3: tu_move(run_pos[0], run_pos[1]); break;
        case 4: T.time += 0.7; break;
        }
        tu_begin();
        ui_rect(0, 0, (float)T.fb_w, (float)T.fb_h, UI_COL_BG);
        /* a fake viewer: gradient with rings and crosshair */
        ui_rect_gradient(10 * s, 10 * s, 610 * s, 620 * s, UI_HEX(0x1c2b5a), UI_HEX(0x6a2c70));
        for (int i = 0; i < 12; i++) ui_circle(300 * s, 320 * s, (30.0f + 22.0f * (float)i) * s, 1.5f * s, ui_alpha(0xFFFFFFFFu, 0.15f));
        ui_line(300 * s, 10 * s, 300 * s, 630 * s, s, ui_alpha(0xFFFFFFFFu, 0.4f));
        ui_line(10 * s, 320.5f * s, 620 * s, 320.5f * s, s, ui_alpha(0xFFFFFFFFu, 0.4f));
        ui_disk(380 * s, 250 * s, 5 * s, UI_COL_ACCENT);
        ui_circle(380 * s, 250 * s, 11 * s, 1.5f * s, UI_COL_ACCENT);
        /* hamburger */
        ui_rrect(18 * s, 18 * s, 36 * s, 36 * s, 6 * s, ui_alpha(UI_COL_PANEL, 0.85f));
        preview_hamburger(18 * s, 18 * s, 36 * s, UI_COL_TEXT);
        /* side panel */
        float px = 630 * s, pw = 320 * s;
        ui_panel(px, 10 * s, pw, 620 * s);
        float x = px + 14 * s, w = pw - 28 * s, y = 22 * s, h = 28 * s, gap = 8 * s;
        ui_label(x, y, w, h, "Settings \xE2\x80\x94 z \xE2\x86\xA6 z\xC2\xB2 + c   \xE2\x84\x82 \xE2\x84\x9D \xE2\x84\xA4 \xCF\x80 \xE2\x89\xA4 \xE2\x88\x9E", UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        y += h + gap;
        float bw = (w - 2 * gap) / 3;
        ui_button_ex("Run", x, y, bw, h, UI_ACCENT);
        ui_tooltip("Compile the script and show the result (Ctrl+R)");
        run_pos[0] = x + bw * 0.5f;
        run_pos[1] = y + h * 0.5f;
        ui_button("Reset", x + bw + gap, y, bw, h);
        ui_button_ex("Disabled", x + 2 * (bw + gap), y, bw, h, UI_DISABLED);
        y += h + gap;
        ui_button_ex("Selected", x, y, bw, h, UI_SELECTED);
        ui_button_ex("Flat", x + bw + gap, y, bw, h, UI_FLAT);
        ui_button("Cancel", x + 2 * (bw + gap), y, bw, h);
        y += h + gap;
        ui_separator(x, y, w);
        y += gap + 2;
        ui_toggle("Show grid", x, y, w, h, &grid);
        y += h + 2;
        ui_toggle("Smooth coloring", x, y, w, h, &smooth);
        y += h + gap;
        ui_slider_float("Hue", x, y, w, h, &hue, 0, 1, "%.2f");
        y += h + gap;
        ui_slider_int("Iterations", x, y, w, h, &iters, 1, 1000);
        y += h + gap;
        ui_slider_float("Radius", x, y, w, h, &radius, 0.1f, 4, "%.3f");
        y += h + gap;
        ui_drag_double("Zoom", x, y, w, h, &zoom, 0.01, 0, 0, "%.4f");
        y += h + gap;
        title_pos[0] = x + w * 0.5f;
        title_pos[1] = y + h * 0.5f;
        ui_text_field("Title", x, y, w, h, name, sizeof name);
        y += h + gap;
        ui_text_field_ex("Formula", x, y, w, h, txt, sizeof txt, 0, "type a formula");
        y += h + gap;
        char empty[4] = "";
        ui_text_field_ex("Empty", x, y, w, h, empty, sizeof empty, 0, "placeholder");
        y += h + gap + 6 * s;
        /* fonts */
        ui_text(UI_FONT_SANS, x, y, "Sans: The quick brown fox jumps over the lazy dog 0123456789", -1, UI_COL_TEXT);
        y += ui_font_line_height(UI_FONT_SANS) + 2;
        ui_text(UI_FONT_MONO, x, y, "Mono: z := z^2 + c  # \xCE\xB8 \xCF\x80 \xE2\x89\xA4 \xE2\x88\x9E", -1, UI_COL_TEXT);
        y += ui_font_line_height(UI_FONT_MONO) + 6 * s;
        /* primitives */
        for (int i = 0; i < 4; i++) ui_line(x + (float)i * 40 * s, y, x + (float)i * 40 * s + 30 * s, y + 24 * s + (float)i * 3 * s, (0.5f + (float)i) * s, UI_COL_ACCENT);
        ui_triangle(x + 180 * s, y + 24 * s, x + 200 * s, y, x + 220 * s, y + 24 * s, UI_COL_WARNING);
        ui_disk(x + 250 * s, y + 12 * s, 11 * s, UI_COL_ERROR);
        ui_circle(x + 285 * s, y + 12 * s, 11 * s, s, UI_COL_OK);
        y += 36 * s;
        ui_rrect(x, y, 90 * s, 28 * s, 14 * s, UI_COL_ACCENT_DIM);
        ui_rrect_outline(x + 100 * s, y, 90 * s, 28 * s, 6 * s, 2 * s, UI_COL_ACCENT);
        ui_rect_outline(x + 200 * s, y, 90 * s, 28 * s, s, UI_COL_BORDER);
        const GfxDrawList *dl = tu_end();
        if (frame == 4) im = tu_render(dl, UI_COL_BG);
    }
    CHECK(im.px != NULL);
    if (im.px) {
        if (crop) raster_save(&im, 630 * (int)s, 0, im.w - 630 * (int)s, im.h, 1, png_path);
        else raster_save(&im, 0, 0, im.w, im.h, 1, png_path);
        /* the panel is the panel color just inside its left edge */
        CHECK(raster_color_dist(raster_pixel(&im, (int)(634 * s), (int)(300 * s)), UI_COL_PANEL) < 4);
        raster_free(&im);
    }
}

TEST(ui_preview) {
    preview_scene(1.0f, "build/ui_preview.png", false);
    preview_scene(2.0f, "build/ui_preview_2x.png", true);
}

/* ---- icons ----------------------------------------------------------------------------------------------------
 * Icon glyphs are baked on demand and stay in ui.c's table for the whole process.  Tests that look at the first
 * request of an icon forget the table first (the unity build lets a test reach into ui.c). */

static void tu_forget_icons(void) {
    ui.icons.len = 0;
    ui.bake_needed = true;
}

/* Run frames until the atlas holds everything requested so far, like the app's main loop does by itself. */
static void tu_settle_atlas(void) {
    for (int i = 0; i < 2; i++) { tu_begin(); tu_atlas(); tu_end(); }
}

/* Bounding box of the pixels in [rx0,rx1) x [ry0,ry1) that differ from bg by more than `min_dist`. */
static bool tu_ink_bbox(const RasterImage *im, int rx0, int ry0, int rx1, int ry1, u32 bg, int min_dist, int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = 1 << 30;
    *x1 = *y1 = -1;
    for (int y = ry0; y < ry1; y++)
        for (int x = rx0; x < rx1; x++)
            if (raster_color_dist(raster_pixel(im, x, y), bg) > min_dist) {
                *x0 = FS_MIN(*x0, x);
                *y0 = FS_MIN(*y0, y);
                *x1 = FS_MAX(*x1, x + 1);
                *y1 = FS_MAX(*y1, y + 1);
            }
    return *x1 > 0;
}

/* Is this icon/size pair baked into the atlas? */
static bool tu_icon_baked(u32 cp, int px) {
    for (int i = 0; i < ui.icons.len; i++)
        if (ui.icons.data[i].g.cp == cp && ui.icons.data[i].px == px) return ui.icons.data[i].ready;
    return false;
}

TEST(ui_icon_lookup) {
    static const struct { const char *name; u32 cp; } known[] = {
        { "menu", 0xe5d2 }, { "pan_tool", 0xe925 }, { "my_location", 0xe55c }, { "close", 0xe5cd }, { "tune", 0xe429 },
        { "code", 0xe86f }, { "chevron_left", 0xe5cb }, { "chevron_right", 0xe5cc }, { "play_arrow", 0xe037 },
        { "save", 0xe161 }, { "folder_open", 0xe2c8 }, { "help_outline", 0xe8fd }, { "vertical_split", 0xe949 },
        { "picture_in_picture_alt", 0xe911 }, { "fullscreen", 0xe5d0 }, { "refresh", 0xe5d5 }, { "timeline", 0xe922 },
        { "10k", 0xe951 }, { "3d_rotation", 0xe84d },   /* the first names of the table start with digits */
        { "zoom_out_map", 0xe56b },                     /* the last one */
        { "fluorescent", 0xec31 }, { "flourescent", 0xec31 }, /* upstream lists a misspelled alias too */
    };
    for (int i = 0; i < ARRAY_COUNT(known); i++)
        CHECK_MSG(ui_icon_lookup(known[i].name) == known[i].cp, "%s -> %x", known[i].name, ui_icon_lookup(known[i].name));
    /* unknown: not a name, wrong case (names are exact), near misses, prefixes and extensions of real names */
    static const char *unknown[] = { NULL, "", " ", "no_such_icon", "Menu", "MENU", "menu ", " menu", "men", "menu_", "menus",
                                     "pan-tool", "pan tool", "e5d2", "0", "~", "zzzzzz", "\xE2\x98\xB0" };
    for (int i = 0; i < ARRAY_COUNT(unknown); i++)
        CHECK_MSG(ui_icon_lookup(unknown[i]) == 0, "'%s'", unknown[i] ? unknown[i] : "(null)");
}

TEST(ui_icon_table_matches_font) {
    stbtt_fontinfo info;
    CHECK(stbtt_InitFont(&info, font_icons_ttf, stbtt_GetFontOffsetForIndex(font_icons_ttf, 0)) != 0);
    int n = ARRAY_COUNT(material_icon_names), unsorted = 0, lost = 0, no_glyph = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && strcmp(material_icon_names[i - 1].name, material_icon_names[i].name) >= 0) unsorted++;
        if (ui_icon_lookup(material_icon_names[i].name) != material_icon_names[i].cp) lost++; /* binary search finds all */
        if (!stbtt_FindGlyphIndex(&info, (int)material_icon_names[i].cp)) no_glyph++;         /* codepoints match the font */
    }
    CHECK(n > 2000);
    CHECK_INT(unsorted, 0);
    CHECK_INT(lost, 0);
    CHECK_INT(no_glyph, 0);
    /* the placement code centers the em box: a 512-unit square from the baseline up, one em wide */
    int asc, desc, gap, adv, lsb;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    CHECK_INT(asc, 512);
    CHECK_INT(desc, 0);
    CHECK_NEAR(stbtt_ScaleForMappingEmToPixels(&info, 512), 1.0, 1e-6);
    stbtt_GetGlyphHMetrics(&info, stbtt_FindGlyphIndex(&info, (int)ui_icon_lookup("menu")), &adv, &lsb);
    CHECK_INT(adv, 512);
}

TEST(ui_icon_bake_on_demand) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    int updates = tu_atlas_updates;
    const u32 white = 0xFFFFFFFFu;

    /* frame 1: the first draw of an icon at a size only schedules it.  Nothing is drawn (not the fallback letter
     * either), the atlas is not touched in the middle of the frame, and the UI wants another frame right away. */
    tu_begin();
    tu_atlas();
    ui_icon("menu", "M", 100, 100, 24, white);
    CHECK(ui_redraw_in() == 0.0);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 0);
    CHECK_INT(tu_atlas_updates, updates);

    /* frame 2: the atlas was rebuilt before the frame was built, and the icon is one textured quad */
    tu_begin();
    RasterTexture atlas = tu_atlas();
    CHECK_INT(tu_atlas_updates, updates + 1);
    ui_icon("menu", "M", 100, 100, 24, white);
    CHECK(ui_redraw_in() > 0.0);
    dl = tu_end();
    CHECK_INT(dl->nverts, 4);
    CHECK_INT(dl->nindices, 6);
    CHECK_INT(dl->ncmds, 1);
    CHECK_INT(dl->cmds[0].texture, 0);
    const GfxVertex *q = dl->verts; /* top-left, top-right, bottom-right, bottom-left */
    float qw = q[2].x - q[0].x, qh = q[2].y - q[0].y;
    CHECK(q[0].x == floorf(q[0].x) && q[0].y == floorf(q[0].y)); /* on whole pixels */
    CHECK(q[0].x >= 100 && q[2].x <= 124 && q[0].y >= 100 && q[2].y <= 124);
    CHECK_MSG(qw >= 16 && qw <= 20 && qh >= 10 && qh <= 14, "%g x %g", qw, qh); /* three bars, 18 x 12 dp */
    CHECK_INT(q[0].rgba, white);

    /* the atlas texels under the quad: about half ink, and a clear 1 px border around them (no bleeding) */
    int ax0 = (int)floorf(q[0].u * (float)atlas.w + 0.5f), ay0 = (int)floorf(q[0].v * (float)atlas.h + 0.5f);
    int ax1 = (int)floorf(q[2].u * (float)atlas.w + 0.5f), ay1 = (int)floorf(q[2].v * (float)atlas.h + 0.5f);
    CHECK_INT(ax1 - ax0, (int)qw);
    CHECK_INT(ay1 - ay0, (int)qh);
    int lit = 0, border = 0;
    for (int y = ay0 - 1; y <= ay1; y++) {
        for (int x = ax0 - 1; x <= ax1; x++) {
            int a = atlas.rgba[((size_t)y * (size_t)atlas.w + (size_t)x) * 4 + 3];
            bool inside = x >= ax0 && x < ax1 && y >= ay0 && y < ay1;
            if (inside && a >= 128) lit++;
            if (!inside && a != 0) border++;
        }
    }
    int area = (ax1 - ax0) * (ay1 - ay0);
    CHECK_MSG(lit > area * 4 / 10 && lit < area * 8 / 10, "lit %d of %d", lit, area);
    CHECK_INT(border, 0);

    /* on screen it is centered in the square it was given (the glyph is symmetric) */
    RasterImage im = tu_render(dl, BGC);
    int x0, y0, x1, y1;
    CHECK(tu_ink_bbox(&im, 90, 90, 140, 140, BGC, 40, &x0, &y0, &x1, &y1));
    CHECK_MSG(abs(x0 + x1 - 224) <= 2 && abs(y0 + y1 - 224) <= 2, "ink (%d,%d)-(%d,%d)", x0, y0, x1, y1);
    raster_free(&im);

    /* frame 3: nothing new, so no rebuild.  Sizes are rounded to whole pixels: 24.3 is the same glyph... */
    tu_begin();
    tu_atlas();
    ui_icon("menu", "M", 100, 100, 24.3f, white);
    CHECK(ui_redraw_in() > 0.0);
    dl = tu_end();
    CHECK_INT(dl->nverts, 4);
    CHECK_INT(tu_atlas_updates, updates + 1);
    /* ... and 24.6 is the next one: a new request */
    tu_begin();
    tu_atlas();
    ui_icon("menu", "M", 100, 100, 24.6f, white);
    CHECK(ui_redraw_in() == 0.0);
    dl = tu_end();
    CHECK_INT(dl->nverts, 0);
}

TEST(ui_icon_edge_cases) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    const float nan = nanf("");
    tu_begin();
    tu_atlas();
    ui_icon("menu", "M", 10, 10, 24, 0x00FFFFFFu);    /* invisible */
    ui_icon("menu", "M", 10, 10, 0, 0xFFFFFFFFu);     /* no size */
    ui_icon("menu", "M", 10, 10, -5, 0xFFFFFFFFu);
    ui_icon("menu", "M", 10, 10, nan, 0xFFFFFFFFu);
    ui_push_clip(0, 0, 50, 50);
    ui_icon("menu", "M", 100, 100, 24, 0xFFFFFFFFu);  /* outside the clip: nobody sees it, so it is not even baked */
    ui_icon(NULL, "M", 100, 100, 24, 0xFFFFFFFFu);
    ui_pop_clip();
    CHECK(ui_redraw_in() > 0.0);
    const GfxDrawList *dl = tu_end();
    CHECK_INT(dl->nverts, 0);
    CHECK_INT(ui.icons.len, 0);
    /* absurd sizes are clamped to the 4..256 px the atlas supports, centered on the square they were given */
    for (int frame = 0; frame < 2; frame++) {
        tu_begin();
        tu_atlas();
        ui_icon("menu", NULL, 400.0f - 50000.0f, 300.0f - 50000.0f, 100000.0f, 0xFFFFFFFFu);
        ui_icon("menu", NULL, 10, 10, 1.5f, 0xFFFFFFFFu);
        dl = tu_end();
    }
    CHECK_INT(dl->nverts, 8);
    float big = dl->verts[2].x - dl->verts[0].x, small = dl->verts[6].x - dl->verts[4].x;
    CHECK(big > 100 && big <= 256);
    CHECK(small >= 1 && small <= 4);
    CHECK_NEAR((dl->verts[0].x + dl->verts[2].x) * 0.5f, 400, 2);
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icon_fallback_text) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    int updates = tu_atlas_updates;
    /* NULL, empty, unknown or wrongly cased names draw the fallback right away (nothing to bake), centered */
    static const char *bad[] = { NULL, "", "no_such_icon", "Menu" };
    for (int i = 0; i < ARRAY_COUNT(bad); i++) {
        tu_begin();
        tu_atlas();
        ui_icon(bad[i], "H", 100, 100, 40, UI_COL_TEXT);
        CHECK(ui_redraw_in() > 0.0);
        const GfxDrawList *dl = tu_end();
        CHECK_INT(dl->nverts, 4);
        RasterImage im = tu_render(dl, BGC);
        int x0, y0, x1, y1;
        CHECK(tu_ink_bbox(&im, 90, 90, 150, 150, BGC, 40, &x0, &y0, &x1, &y1));
        CHECK_MSG(abs(x0 + x1 - 240) <= 2 && abs(y0 + y1 - 240) <= 2, "%d: ink (%d,%d)-(%d,%d)", i, x0, y0, x1, y1);
        raster_free(&im);
    }
    CHECK_INT(tu_atlas_updates, updates);

    /* the same baseline for letters of one row: an 'H' and an 'O' sit on the same line (O only overshoots a little) */
    tu_begin();
    tu_atlas();
    ui_icon(NULL, "H", 100, 100, 40, UI_COL_TEXT);
    ui_icon(NULL, "O", 200, 100, 40, UI_COL_TEXT);
    RasterImage im = tu_render(tu_end(), BGC);
    int hx0, hy0, hx1, hy1, ox0, oy0, ox1, oy1;
    CHECK(tu_ink_bbox(&im, 90, 90, 150, 150, BGC, 40, &hx0, &hy0, &hx1, &hy1));
    CHECK(tu_ink_bbox(&im, 190, 90, 250, 150, BGC, 40, &ox0, &oy0, &ox1, &oy1));
    CHECK(abs(hy1 - oy1) <= 1);
    raster_free(&im);

    /* no usable fallback: nothing is drawn */
    tu_begin();
    tu_atlas();
    ui_icon("no_such_icon", NULL, 10, 10, 24, UI_COL_TEXT);
    ui_icon(NULL, "", 40, 10, 24, UI_COL_TEXT);
    ui_icon(NULL, NULL, 70, 10, 24, UI_COL_TEXT);
    CHECK_INT(tu_end()->nverts, 0);

    /* any UTF-8 text will do, and a real icon wins over its fallback: while it is pending nothing is shown */
    tu_begin();
    tu_atlas();
    ui_icon(NULL, "\xCE\xA9", 10, 10, 24, UI_COL_TEXT);
    ui_icon("close", "X", 40, 10, 24, UI_COL_TEXT);
    CHECK_INT(tu_end()->nverts, 4);
    tu_begin();
    tu_atlas();
    ui_icon("close", "X", 40, 10, 24, UI_COL_TEXT);
    CHECK_INT(tu_end()->nverts, 4); /* the icon only, not the icon and an X */
    tu_forget_icons();
    tu_settle_atlas();
}

/* The text pass of the "text pixels" test: both fonts and a widget. */
static RasterImage tu_text_image(void) {
    tu_begin();
    tu_atlas();
    ui_text(UI_FONT_SANS, 10, 10, "The quick brown fox \xCE\xB8 \xE2\x89\xA4 \xE2\x88\x9E jumps", -1, UI_COL_TEXT);
    ui_text(UI_FONT_MONO, 10, 40, "z := z^2 + c  # mono \xCF\x80", -1, UI_COL_TEXT_DIM);
    ui_button("Run", 10, 70, 80, 28);
    const GfxDrawList *dl = tu_end();
    return tu_render(dl, BGC);
}

TEST(ui_icon_keeps_text_pixels) {
    /* Baking icons into the atlas must not change what existing text looks like (here at two content scales). */
    for (int pass = 0; pass < 2; pass++) {
        tu_reset(pass ? 2.0f : 1.0f);
        tu_forget_icons();
        tu_settle_atlas();
        RasterImage before = tu_text_image();
        tu_begin();
        tu_atlas();
        static const char *names[] = { "menu", "close", "code", "tune", "save", "refresh", "fullscreen", "timeline" };
        for (int i = 0; i < ARRAY_COUNT(names); i++)
            for (int k = 0; k < 3; k++) ui_icon(names[i], NULL, 10, 10, 18.0f + 6.0f * (float)k, 0xFFFFFFFFu);
        tu_end();
        RasterImage after = tu_text_image();
        CHECK_INT(ui.icons.len, 24);
        CHECK(tu_icon_baked(ui_icon_lookup("save"), 30));
        CHECK(tu_count_diff(&before, 0, 0, T.fb_w, T.fb_h, BGC) > 200); /* the text is really there */
        int differing = 0; /* the glyphs sit elsewhere in the atlas, so bilinear weights may differ in the last bit */
        for (int i = 0; i < before.w * before.h; i++) differing += raster_color_dist(before.px[i], after.px[i]) > 1;
        CHECK_INT(differing, 0);
        raster_free(&before);
        raster_free(&after);
    }
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icon_rebuild_once_and_kept) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    static const char *names[] = { "menu", "close", "code", "tune", "save", "refresh", "fullscreen", "timeline", "pan_tool", "my_location" };
    const int n = ARRAY_COUNT(names);
    const u32 white = 0xFFFFFFFFu;
    int updates = tu_atlas_updates;
    /* ten icons at two sizes (20 requests) show up in one frame: a single rebuild, before the next frame is built */
    for (int frame = 0; frame < 3; frame++) {
        tu_begin();
        tu_atlas();
        for (int i = 0; i < n; i++) {
            ui_icon(names[i], NULL, 10.0f + 40.0f * (float)i, 10, 24, white);
            ui_icon(names[i], NULL, 10.0f + 40.0f * (float)i, 60, 32, white);
        }
        CHECK_INT(tu_end()->nverts, frame == 0 ? 0 : 8 * n);
        CHECK_INT(tu_atlas_updates, updates + (frame > 0));
    }
    /* a new size of one icon: only that one is missing for a frame, the others stay; one more rebuild */
    for (int frame = 0; frame < 3; frame++) {
        tu_begin();
        tu_atlas();
        for (int i = 0; i < n; i++) {
            ui_icon(names[i], NULL, 10.0f + 40.0f * (float)i, 10, 24, white);
            ui_icon(names[i], NULL, 10.0f + 40.0f * (float)i, 60, 32, white);
        }
        ui_icon("menu", NULL, 10, 120, 40, white);
        CHECK_INT(tu_end()->nverts, 4 * (2 * n + (frame > 0)));
        CHECK_INT(tu_atlas_updates, updates + 1 + (frame > 0));
    }
    /* a content-scale change rebakes the text for the new scale; the icons asked for earlier are drawn at once */
    T.content_scale = 2.0f;
    tu_begin();
    tu_atlas();
    CHECK_INT(tu_atlas_updates, updates + 3);
    for (int i = 0; i < n; i++) ui_icon(names[i], NULL, 10.0f + 40.0f * (float)i, 10, 24, white);
    ui_icon("menu", NULL, 10, 120, 40, white);
    CHECK_INT(tu_end()->nverts, 4 * (n + 1));
    /* what the app would ask for at 2x is new, and arrives one frame later */
    tu_begin();
    tu_atlas();
    ui_icon("menu", NULL, 10, 10, 48, white);
    ui_icon("menu", NULL, 10, 70, 24, white);
    CHECK_INT(tu_end()->nverts, 4);
    tu_begin();
    tu_atlas();
    CHECK_INT(tu_atlas_updates, updates + 4);
    ui_icon("menu", NULL, 10, 10, 48, white);
    ui_icon("menu", NULL, 10, 70, 24, white);
    CHECK_INT(tu_end()->nverts, 8);
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icon_budget) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    const u32 white = 0xFFFFFFFFu;
    /* more icon/size pairs than the table keeps: 75 icons at 8 sizes = 600 > 512 */
    u32 first_cp = 0, last_cp = 0;
    tu_begin();
    tu_atlas();
    for (int i = 0, count = 0; i < ARRAY_COUNT(material_icon_names) && count < 75; i += 20, count++) {
        if (!first_cp) first_cp = material_icon_names[i].cp;
        last_cp = material_icon_names[i].cp;
        for (int px = 6; px < 14; px++) ui_icon(material_icon_names[i].name, NULL, 10, 10, (float)px, white);
    }
    CHECK(ui.icons.len > 512);
    tu_end();
    tu_begin();
    tu_atlas();
    CHECK_INT(ui.icons.len, 512);
    CHECK(tu_icon_baked(last_cp, 13));  /* the newest requests survive ... */
    CHECK(!tu_icon_baked(first_cp, 6)); /* ... the oldest are dropped (and would be baked again if asked for) */
    tu_end();

    /* too many atlas pixels: three icons at 41 sizes up to 220 px would take about 2M of them */
    tu_forget_icons();
    tu_settle_atlas();
    tu_begin();
    tu_atlas();
    for (int px = 20; px <= 220; px += 5)
        for (int i = 0; i < 3; i++) ui_icon(i == 0 ? "menu" : i == 1 ? "close" : "code", NULL, 10, 10, (float)px, white);
    CHECK_INT(ui.icons.len, 123);
    tu_end();
    RasterTexture atlas;
    tu_begin();
    atlas = tu_atlas();
    long area = 0;
    for (int i = 0; i < ui.icons.len; i++) area += (long)(ui.icons.data[i].px + 2) * (long)(ui.icons.data[i].px + 2);
    CHECK(area <= UIP_ICON_MAX_AREA && ui.icons.len >= 20 && ui.icons.len < 123);
    CHECK(tu_icon_baked(ui_icon_lookup("code"), 220));
    CHECK(atlas.w <= 4096 && atlas.h <= 4096);
    tu_end();
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icon_button_click) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    bool clicked = false, any = false;
    /* hover (the icon is still being baked during these first frames: that must not matter) */
    tu_move(40, 40);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); bool hov = ui_last_hovered(); bool wm = ui_wants_mouse(); tu_end();
    CHECK(!clicked && hov && wm);
    /* press, hold, release inside: one click */
    tu_press(0, 1);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); CHECK(ui_active_id() != 0); tu_end();
    CHECK(!clicked);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); tu_end();
    CHECK(!clicked);
    tu_release(0);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); tu_end();
    CHECK(clicked);
    CHECK_INT(ui_active_id(), 0);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); tu_end();
    CHECK(!clicked);
    /* press, drag out, release: no click */
    tu_press(0, 1);
    tu_begin(); ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); tu_end();
    tu_move(300, 300);
    tu_begin(); any |= ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); CHECK(ui_wants_mouse()); tu_end();
    tu_release(0);
    tu_begin(); any |= ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); tu_end();
    CHECK(!any);
    /* press and release within one frame still clicks; "selected" is only a look, the app decides what it means */
    tu_move(40, 40);
    tu_press(0, 1);
    tu_release(0);
    tu_begin(); clicked = ui_icon_button("run", "play_arrow", "P", 20, 20, 40, true); tu_end();
    CHECK(clicked);
    /* the id is the identity: two buttons side by side, press on one and release on the other clicks neither */
    tu_move(40, 40);
    tu_press(0, 1);
    bool a, b;
    tu_begin(); ui_icon_button("a", "play_arrow", NULL, 20, 20, 40, false); ui_icon_button("b", "play_arrow", NULL, 70, 20, 40, false); tu_end();
    tu_move(90, 40);
    tu_release(0);
    tu_begin(); a = ui_icon_button("a", "play_arrow", NULL, 20, 20, 40, false); b = ui_icon_button("b", "play_arrow", NULL, 70, 20, 40, false); tu_end();
    CHECK(!a && !b);
    tu_press(0, 1);
    tu_release(0);
    tu_begin(); a = ui_icon_button("a", "play_arrow", NULL, 20, 20, 40, false); b = ui_icon_button("b", "play_arrow", NULL, 70, 20, 40, false); tu_end();
    CHECK(!a && b);
    /* disabled buttons do not click but still block the mouse; a NULL id and unknown icons do not crash */
    tu_press(0, 1);
    tu_release(0);
    tu_begin(); clicked = ui_icon_button_ex("off", "play_arrow", "P", 70, 20, 40, UI_DISABLED); CHECK(ui_wants_mouse()); tu_end();
    CHECK(!clicked);
    tu_begin(); ui_icon_button_ex(NULL, "nope", NULL, 70, 20, 40, UI_FLAT); tu_end();
    /* mouse elsewhere: the UI does not want it */
    tu_move(400, 400);
    tu_begin(); ui_icon_button("run", "play_arrow", "P", 20, 20, 40, false); CHECK(!ui_wants_mouse()); tu_end();
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icon_button_look) {
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
    /* five 40 px buttons showing "menu" (its middle bar passes through the center pixel): normal, hovered, selected,
     * accent, disabled.  A pixel inside the left edge shows the fill, the center pixel the icon color (the glyph's
     * outline is not exactly on the pixel grid, so its coverage there is a bit under 100%). */
    static const struct { int flags; u32 fill, ink; } look[5] = {
        { 0, UI_COL_WIDGET, UI_COL_TEXT },
        { 0, UI_COL_WIDGET_HOVER, UI_COL_TEXT },
        { UI_SELECTED, UI_COL_ACCENT_DIM, UI_COL_TEXT },
        { UI_ACCENT, UI_COL_ACCENT, UI_COL_ON_ACCENT },
        { UI_DISABLED, UI_COL_WIDGET, UI_COL_TEXT_OFF },
    };
    RasterImage im = { 0 };
    tu_move(20 + 60 + 20, 40); /* over the second button */
    for (int frame = 0; frame < 4; frame++) {
        tu_begin();
        tu_atlas();
        ui_rect(0, 0, (float)T.fb_w, (float)T.fb_h, UI_COL_BG);
        for (int i = 0; i < 5; i++) {
            ui_push_id_int(i);
            ui_icon_button_ex("b", "menu", "M", 20.0f + 60.0f * (float)i, 20, 40, look[i].flags);
            ui_pop_id();
        }
        const GfxDrawList *dl = tu_end();
        if (frame == 3) im = tu_render(dl, UI_COL_BG);
    }
    CHECK(im.px != NULL);
    if (im.px) {
        for (int i = 0; i < 5; i++) {
            int bx = 20 + 60 * i;
            CHECK_MSG(raster_color_dist(raster_pixel(&im, bx + 3, 40), look[i].fill) < 3, "button %d fill %08x", i, raster_pixel(&im, bx + 3, 40));
            CHECK_MSG(raster_color_dist(raster_pixel(&im, bx + 20, 40), look[i].ink) < 10, "button %d icon %08x", i, raster_pixel(&im, bx + 20, 40));
        }
        CHECK(raster_color_dist(raster_pixel(&im, 20 + 120, 40), UI_COL_ACCENT) < 6); /* the selected button has an accent border */
        CHECK(raster_color_dist(raster_pixel(&im, 20, 40), UI_COL_BORDER) < 6);       /* the others a plain one */
        raster_free(&im);
    }
    /* a fallback letter takes the same color as an icon would (here a disabled button) */
    tu_move(-100, -100);
    tu_begin();
    tu_atlas();
    ui_icon_button_ex("off", NULL, "H", 20, 20, 40, UI_DISABLED);
    im = tu_render(tu_end(), BGC);
    int x0, y0, x1, y1;
    CHECK(tu_ink_bbox(&im, 28, 28, 52, 52, UI_COL_WIDGET, 20, &x0, &y0, &x1, &y1));
    CHECK(abs(x0 + x1 - 80) <= 2 && abs(y0 + y1 - 80) <= 2);
    raster_free(&im);
    tu_forget_icons();
    tu_settle_atlas();
}

/* A toolbar like the app's: icon buttons in every state, the icons of the app's chrome, and several sizes. */
static void icons_preview_scene(float scale, const char *png_path, int zoom) {
    tu_reset(scale);
    tu_forget_icons();
    float s = scale;
    T.fb_w = (int)(600 * s);
    T.fb_h = (int)(232 * s);
    static const struct { const char *name, *fallback; int flags; } btn[] = {
        { "menu", "M", 0 }, { "pan_tool", "P", UI_SELECTED }, { "my_location", "L", 0 }, { "tune", "T", 0 },
        { "code", "C", UI_DISABLED }, { "chevron_left", "<", UI_FLAT }, { "chevron_right", ">", UI_FLAT },
        { "play_arrow", "R", UI_ACCENT }, { "no_such_icon", "O", 0 }, { NULL, "J", UI_SELECTED },
    };
    static const char *chrome[] = { "menu", "pan_tool", "my_location", "close", "tune", "code", "chevron_left", "chevron_right",
                                    "play_arrow", "save", "folder_open", "help_outline", "vertical_split",
                                    "picture_in_picture_alt", "fullscreen", "refresh", "timeline" };
    static const float sizes[] = { 16, 20, 24, 32, 48 };
    RasterImage im = { 0 };
    for (int frame = 0; frame < 4; frame++) {
        if (frame == 2) tu_move((16 + 2 * 54 + 20) * s, (36 + 20) * s); /* hover the third button */
        tu_begin();
        tu_atlas();
        ui_rect(0, 0, (float)T.fb_w, (float)T.fb_h, UI_COL_BG);
        ui_panel(6 * s, 6 * s, 588 * s, 220 * s);
        ui_label(16 * s, 10 * s, 570 * s, 22 * s, "normal, selected, hovered, normal, disabled, flat, flat, accent, 2 letters", UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        for (int i = 0; i < 10; i++) {
            ui_push_id_int(i);
            ui_icon_button_ex("btn", btn[i].name, btn[i].fallback, (16.0f + 54.0f * (float)i) * s, 36 * s, 40 * s, btn[i].flags);
            ui_pop_id();
        }
        ui_label(16 * s, 84 * s, 570 * s, 22 * s, "The icons of the app's chrome, 24 px", UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        for (int i = 0; i < ARRAY_COUNT(chrome); i++) ui_icon(chrome[i], NULL, (16.0f + 33.0f * (float)i) * s, 110 * s, 24 * s, UI_COL_TEXT);
        ui_label(16 * s, 144 * s, 570 * s, 22 * s, "Sizes 16, 20, 24, 32, 48 px, in text, dim and accent colors", UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        float x = 16 * s;
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 5; i++) {
                ui_icon("timeline", NULL, x, (172.0f + (48.0f - sizes[i]) * 0.5f) * s, sizes[i] * s, c == 0 ? UI_COL_TEXT : c == 1 ? UI_COL_TEXT_DIM : UI_COL_ACCENT);
                x += (sizes[i] + 6) * s;
            }
            x += 12 * s;
        }
        const GfxDrawList *dl = tu_end();
        if (frame == 3) im = tu_render(dl, UI_COL_BG);
    }
    CHECK(im.px != NULL);
    if (im.px) {
        raster_save(&im, 0, 0, im.w, im.h, zoom, png_path);
        for (int i = 0; i < ARRAY_COUNT(chrome); i++) /* every icon of the row left ink in its cell */
            CHECK_MSG(tu_count_diff(&im, (int)((16.0f + 33.0f * (float)i) * s), (int)(110 * s), (int)((16.0f + 33.0f * (float)i + 24) * s), (int)(134 * s), UI_COL_PANEL) > 20,
                      "icon %s", chrome[i]);
        raster_free(&im);
    }
    tu_reset(1.0f);
    tu_forget_icons();
    tu_settle_atlas();
}

TEST(ui_icons_preview) {
    icons_preview_scene(1.0f, "build/ui_icons_preview.png", 2);
    icons_preview_scene(2.0f, "build/ui_icons_preview_2x.png", 1);
}


static const TestCase tests_ui[] = {
    TESTCASE(ui_atlas),
    TESTCASE(ui_font_metrics),
    TESTCASE(ui_draw_text_counts),
    TESTCASE(ui_draw_rect_and_cmds),
    TESTCASE(ui_clip_and_layers),
    TESTCASE(ui_image_command),
    TESTCASE(ui_raster_rect_exact),
    TESTCASE(ui_raster_shapes),
    TESTCASE(ui_raster_text_and_clip),
    TESTCASE(ui_button_click),
    TESTCASE(ui_layers_get_mouse_first),
    TESTCASE(ui_toggle_widget),
    TESTCASE(ui_slider_values),
    TESTCASE(ui_drag_and_type),
    TESTCASE(ui_drag_log_scale),
    TESTCASE(ui_text_field_editing),
    TESTCASE(ui_text_field_submit_cancel_focus),
    TESTCASE(ui_text_field_select_all_on_click),
    TESTCASE(ui_shortcuts_and_key_order),
    TESTCASE(ui_tooltip_delay),
    TESTCASE(ui_ids_and_labels),
    TESTCASE(ui_word_helpers),
    TESTCASE(ui_scroll_area_wheel),
    TESTCASE(ui_icon_lookup),
    TESTCASE(ui_icon_table_matches_font),
    TESTCASE(ui_icon_bake_on_demand),
    TESTCASE(ui_icon_edge_cases),
    TESTCASE(ui_icon_fallback_text),
    TESTCASE(ui_icon_keeps_text_pixels),
    TESTCASE(ui_icon_rebuild_once_and_kept),
    TESTCASE(ui_icon_budget),
    TESTCASE(ui_icon_button_click),
    TESTCASE(ui_icon_button_look),
    TESTCASE(platform_key_mapping),
    TESTCASE(platform_input_accumulation),
    TESTCASE(platform_mouse_and_clicks),
    TESTCASE(platform_click_split_over_frames),
    TESTCASE(platform_key_tap_split),
    TESTCASE(platform_event_stress),
    TESTCASE(platform_dropped_files),
    TESTCASE(platform_clipboard_headless),
    TESTCASE(platform_window),
    TESTCASE(platform_window_click),
    TESTCASE(ui_preview),
    TESTCASE(ui_icons_preview),
};
