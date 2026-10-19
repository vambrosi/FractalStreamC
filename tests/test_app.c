/* test_app.c - parts of the application that work without a window: the views' history (Back / Forward),
 * Copy / Paste location, and a deep-zoom centre written in the script. */

static const char *app_test_script =
    "@config\n"
    "m.center = -0.743643887037151 + 0.131825904205330i\n"
    "m.radius = 1e-9\n"
    "@viewer m c\n"
    "color = .black\n"
    "@viewer s z \"Sphere\" projective\n"
    "color = .white\n";

/* run `body` with the application's state set up for program `p` (no window, no GPU) */
static App app_test_saved;
static void app_test_begin(Program *p) {
    app_test_saved = A;
    memset(&A, 0, sizeof A);
    A.prog = p;
    A.state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(A.state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    A.vrt = (ViewerRT *)xcalloc((size_t)p->nviewers, sizeof(ViewerRT));
}
static void app_test_end(void) {
    for (int v = 0; v < A.prog->nviewers; v++) free(A.vrt[v].hist);
    free(A.vrt);
    free(A.state);
    program_free(A.prog);
    A = app_test_saved;
}

TEST(app_script_center_keeps_its_digits) {
    Program *p = program_load(app_test_script, "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    Section *m = p->viewers[0];
    CHECK(m->center_lo != NULL);
    double cx = (double)wf(p->initial_state[m->center->slot]) + wf(p->initial_state[m->center_lo->slot]);
    double cy = (double)wf(p->initial_state[m->center->slot + 1]) + wf(p->initial_state[m->center_lo->slot + 1]);
    CHECK_NEAR(cx, -0.743643887037151, 1e-15);
    CHECK_NEAR(cy, 0.131825904205330, 1e-15);
    program_free(p);
    /* a centre computed by code (not a constant) or a float keeps no low part */
    p = program_load("@config\nm.center = -0.5 + 0.25i\n@viewer m c\ncolor = .black\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (!p->nerrors) {
        CHECK_INT(p->initial_state[p->viewers[0]->center_lo->slot], 0);
        CHECK_INT(p->initial_state[p->viewers[0]->center_lo->slot + 1], 0);
    }
    program_free(p);
    p = program_load("@config\nm.center = -1/3 - 2i/3\n@viewer m c\ncolor = .black\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (!p->nerrors) {
        Section *v = p->viewers[0];
        double x = (double)wf(p->initial_state[v->center->slot]) + wf(p->initial_state[v->center_lo->slot]);
        double y = (double)wf(p->initial_state[v->center->slot + 1]) + wf(p->initial_state[v->center_lo->slot + 1]);
        CHECK_NEAR(x, -1.0 / 3, 1e-14);
        CHECK_NEAR(y, -2.0 / 3, 1e-14);
    }
    program_free(p);
}

TEST(app_view_history) {
    Program *p = program_load(app_test_script, "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    ViewPlace start = view_place(0);
    CHECK(!nav_go(0, false)); /* nothing yet */
    CHECK_INT(nav_count(0, false), 0);
    /* a wheel gesture: many steps, one place */
    for (int k = 0; k < 5; k++) {
        nav_begin(0, NAV_WHEEL);
        zoom_at(0, (Rect){0, 0, 400, 400}, 100, 300, 0.85f);
    }
    ViewPlace zoomed = view_place(0);
    CHECK(!same_place(&zoomed, &start));
    CHECK_INT(nav_count(0, false), 1);
    /* a drag: another gesture */
    A.drag_gesture = nav_new_gesture();
    for (int k = 0; k < 3; k++) {
        nav_begin(0, A.drag_gesture);
        pan_by(0, (Rect){0, 0, 400, 400}, 10, -5);
    }
    ViewPlace panned = view_place(0);
    CHECK_INT(nav_count(0, false), 2);
    /* back, back, forward, forward */
    CHECK(nav_go(0, false));
    ViewPlace pl = view_place(0);
    CHECK(same_place(&pl, &zoomed));
    CHECK(nav_go(0, false));
    pl = view_place(0);
    CHECK(same_place(&pl, &start));
    CHECK(!nav_go(0, false));
    CHECK_INT(nav_count(0, true), 2);
    CHECK(nav_go(0, true));
    pl = view_place(0);
    CHECK(same_place(&pl, &zoomed));
    CHECK(nav_go(0, true));
    pl = view_place(0);
    CHECK(same_place(&pl, &panned));
    CHECK(!nav_go(0, true));
    /* going somewhere new forgets the forward places */
    CHECK(nav_go(0, false));
    reset_view(0);
    CHECK_INT(nav_count(0, true), 0);
    pl = view_place(0);
    CHECK(same_place(&pl, &start)); /* (Home: the script's view, with all its digits) */
    CHECK(nav_go(0, false));
    pl = view_place(0);
    CHECK(same_place(&pl, &zoomed));
    /* the sphere's Möbius map goes back too */
    ViewPlace s0 = view_place(1);
    nav_begin(1, NAV_WHEEL);
    zoom_at(1, (Rect){0, 0, 400, 400}, 250, 120, 0.5f);
    ViewPlace s1 = view_place(1);
    CHECK(!same_place(&s0, &s1));
    CHECK(nav_go(1, false));
    pl = view_place(1);
    CHECK(same_place(&pl, &s0));
    /* the history holds the last VIEW_HISTORY places */
    for (int k = 0; k < VIEW_HISTORY + 10; k++) {
        nav_begin(0, nav_new_gesture());
        pan_by(0, (Rect){0, 0, 400, 400}, 1, 0);
    }
    CHECK_INT(nav_count(0, false), VIEW_HISTORY);
    app_test_end();
}

TEST(app_copy_paste_location) {
    Program *p = program_load(app_test_script, "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    char text[256];
    location_text(0, text, sizeof text);
    CHECK_STR(text, "m.center = -0.74364388703715 + 0.13182590420533i\nm.radius = 1e-09\n"); /* (14 digits at 1e-9) */
    /* what Copy location writes, Paste location reads, digits included */
    ViewPlace before = view_place(0);
    reset_view(0);
    pan_by(0, (Rect){0, 0, 400, 400}, 123, 45);
    platform_set_clipboard(text);
    CHECK(paste_location(0));
    ViewPlace after = view_place(0);
    CHECK_NEAR(after.cx, before.cx, 1e-14); /* (a hundred-thousandth of the radius) */
    CHECK_NEAR(after.cy, before.cy, 1e-14);
    CHECK(after.radius == before.radius);
    CHECK(nav_go(0, false)); /* (pasting is a step of the history) */
    /* a point alone moves the centre; a radius alone zooms */
    platform_set_clipboard("  -0.75+0.1i \n");
    CHECK(paste_location(0));
    after = view_place(0);
    CHECK_NEAR(after.cx, -0.75, 0);
    CHECK_NEAR(after.cy, 0.1, 1e-15);
    CHECK(after.radius == before.radius);
    platform_set_clipboard("whatever.radius = 0.5");
    CHECK(paste_location(0));
    after = view_place(0);
    CHECK_NEAR(after.cx, -0.75, 0);
    CHECK(after.radius == 0.5f);
    /* not a location: nothing moves */
    platform_set_clipboard("hello");
    CHECK(!paste_location(0));
    ViewPlace still = view_place(0);
    CHECK(same_place(&still, &after));
    platform_set_clipboard("m.radius = -2");
    CHECK(!paste_location(0));
    /* a shallow view: a float's shortest digits */
    platform_set_clipboard("m.center = -0.5 + 0.25i\nm.radius = 1.5");
    CHECK(paste_location(0));
    location_text(0, text, sizeof text);
    CHECK_STR(text, "m.center = -0.5 + 0.25i\nm.radius = 1.5\n");
    platform_set_clipboard("m.center = 0.1 - 0.3i\nm.radius = 2");
    CHECK(paste_location(0));
    location_text(0, text, sizeof text);
    CHECK_STR(text, "m.center = 0.1 - 0.3i\nm.radius = 2\n");
    app_test_end();
}

/* the values changed in the app go into the script's text: values given as numbers are replaced, formulas are left
   alone, the views get lines at the top of @config; the script compiled again starts as the app is now */
TEST(app_write_values_into_script) {
    const char *src =
        "# test\n"
        "@config\n"
        "a : C = 0.3 \"a =\"\n"
        "n : Z = 2 in 0..10\n"
        "flag : Bool = false\n"
        "seeds : List of C = [0.1, 0.2]\n"
        "col : Color = .red\n"
        "@global\n"
        "b := 2 * a + n  # a formula\n"
        "@prepare\n"
        "q := 0\n"
        "@viewer m c\n"
        "color = col\n";
    Program *p = program_load(src, "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    A.ed = editor_create();
    editor_set_text(A.ed, src, (int)strlen(src));
    A.compiled_version = editor_version(A.ed);
    CHECK_INT(count_changed(p, true), 0);
    Sym *sa = NULL, *sn = NULL, *sf = NULL, *ss = NULL, *sb = NULL, *sc = NULL;
    for (int i = 0; i < p->globals.len; i++) {
        Sym *g = p->globals.data[i];
        if (!strcmp(g->name, "a")) sa = g;
        if (!strcmp(g->name, "n")) sn = g;
        if (!strcmp(g->name, "flag")) sf = g;
        if (!strcmp(g->name, "seeds")) ss = g;
        if (!strcmp(g->name, "b")) sb = g;
        if (!strcmp(g->name, "col")) sc = g;
    }
    CHECK(sa && sn && sf && ss && sb && sc);
    if (!(sa && sn && sf && ss && sb && sc)) { editor_destroy(A.ed); app_test_end(); return; }
    A.state[sa->slot] = fw(0.35f);
    A.state[sa->slot + 1] = fw(-0.1f);
    A.state[sn->slot] = 5;
    A.state[sf->slot] = 1;
    A.state[ss->slot] = 1; /* [0.5] */
    A.state[ss->slot + ss->type->data_off] = fw(0.5f);
    A.state[ss->slot + ss->type->data_off + 1] = 0;
    A.state[sb->slot] = fw(7.0f);
    A.state[sc->slot] = fw(0.25f);
    A.state[sc->slot + 1] = fw(0.5f);
    A.state[sc->slot + 2] = fw(1.0f);
    set_view_center_d(0, -0.743643887037151, 0.131825904205330);
    A.state[p->viewers[0]->radius->slot] = fw(1e-9f);
    CHECK_INT(count_changed(p, false), 6);
    CHECK_INT(count_changed(p, true), 7);
    write_values();
    int len;
    const char *text = editor_get_text(A.ed, &len);
    CHECK_STR(text,
              "# test\n"
              "@config\n"
              "m.center = -0.74364388703715 + 0.13182590420533i\n"
              "m.radius = 1e-09\n"
              "a : C = 0.35 - 0.1i \"a =\"\n"
              "n : Z = 5 in 0..10\n"
              "flag : Bool = true\n"
              "seeds : List of C = [0.5]\n"
              "col : Color = rgb(0.25, 0.5, 1)\n"
              "@global\n"
              "b := 2 * a + n  # a formula\n"
              "@prepare\n"
              "q := 0\n"
              "@viewer m c\n"
              "color = col\n");
    CHECK(strstr(A.status, "Not written") && strstr(A.status, "b"));
    /* compiled again, the script starts where the app is (but for the formula) */
    Program *q = program_load(text, "t");
    CHECK_INT(q->nerrors, 0);
    if (!q->nerrors) {
        for (int i = 0; i < q->globals.len; i++) {
            Sym *g = q->globals.data[i];
            Sym *old = NULL;
            for (int k = 0; k < p->globals.len; k++)
                if (!strcmp(p->globals.data[k]->name, g->name)) old = p->globals.data[k];
            if (!old || app_managed(q, g) || view_geometry(q, g) || !strcmp(g->name, "b")) continue;
            CHECK_MSG(same_value(g->type, q->initial_state + g->slot, A.state + old->slot), "%s", g->name);
        }
        Section *m = q->viewers[0];
        double cx = (double)wf(q->initial_state[m->center->slot]) + wf(q->initial_state[m->center_lo->slot]);
        double cy = (double)wf(q->initial_state[m->center->slot + 1]) + wf(q->initial_state[m->center_lo->slot + 1]);
        ViewPlace now = view_place(0);
        CHECK_NEAR(cx, now.cx, 1e-14);
        CHECK_NEAR(cy, now.cy, 1e-14);
        CHECK(wf(q->initial_state[m->radius->slot]) == now.radius);
    }
    program_free(q);
    /* undo is one step */
    CHECK(editor_undo(A.ed));
    text = editor_get_text(A.ed, &len);
    CHECK_STR(text, src);
    /* Reset values: as in the script, the view stays */
    ViewPlace pl = view_place(0);
    reset_values();
    CHECK_INT(count_changed(p, false), 0);
    CHECK_INT(count_changed(p, true), 1);
    ViewPlace pl2 = view_place(0);
    CHECK(same_place(&pl, &pl2));
    editor_destroy(A.ed);
    app_test_end();
}

/* the open / save dialogs: folders listed, paths split */
TEST(app_folder_listing) {
    int n = 0;
    FsDirEntry *e = fs_list_dir("examples", &n);
    CHECK(e != NULL && n > 10);
    bool templates = false, mandel = false, sorted = true;
    for (int i = 0; e && i < n; i++) {
        if (!strcmp(e[i].name, "templates")) templates = e[i].is_dir && i == 0; /* folders first */
        if (!strcmp(e[i].name, "mandelbrot.fstream")) mandel = !e[i].is_dir;
        if (i > 1 && strcasecmp(e[i - 1].name, e[i].name) > 0) sorted = false;
        CHECK(e[i].name[0] != '.');
    }
    CHECK(templates && mandel && sorted);
    if (e) fs_free_dir(e, n);
    e = fs_list_dir("no/such/folder", &n);
    CHECK(e == NULL && n == 0);
    CHECK(fs_is_dir("examples") && !fs_is_dir("examples/mandelbrot.fstream") && !fs_is_dir("no/such"));
    CHECK(fs_exe_dir()[0] != 0);
    char dir[256];
    CHECK_STR(split_path("a/b/c.fstream", dir, sizeof dir), "c.fstream");
    CHECK_STR(dir, "a/b");
    CHECK_STR(split_path("a/b/", dir, sizeof dir), "");
    CHECK_STR(dir, "a/b");
    CHECK_STR(split_path("c", dir, sizeof dir), "c");
    CHECK_STR(dir, "");
    CHECK_STR(split_path("/c", dir, sizeof dir), "c");
    CHECK_STR(dir, "/");
    CHECK(contains_nocase("SaddleDrop.fstream", "ledr") && !contains_nocase("julia", "mandel") && contains_nocase("x", ""));
    CHECK(has_ext("a.FStream", ".fstream") && !has_ext(".fstream", ".fstream") && !has_ext("a.png", ".fstream"));
    char abs[1024];
    CHECK(fs_absolute("examples/../examples", abs, sizeof abs) && abs[0] != 0 && !strstr(abs, ".."));
    /* the examples inside the macOS app are saved elsewhere */
    CHECK(inside_app_bundle("/Applications/FractalStream.app/Contents/Resources/examples/julia.fstream"));
    CHECK(!inside_app_bundle("examples/julia.fstream"));
}

/* the recent files: latest first, no duplicates, at most 12, kept in the settings folder */
TEST(app_recent_files) {
#if FS_OS_WINDOWS
    SKIP("uses setenv");
#else
    const char *old = getenv("FS_CONFIG_DIR");
    char *saved_env = old ? xstrdup(old) : NULL;
    setenv("FS_CONFIG_DIR", "build/test-settings/deeper", 1);
    remove("build/test-settings/deeper/recent.txt");
    App saved = A;
    A.nrecent = 0;
    note_recent("examples/julia.fstream");
    note_recent("examples/mandelbrot.fstream");
    note_recent("examples/julia.fstream");
    CHECK_INT(A.nrecent, 2);
    CHECK(strstr(A.recent[0], "examples/julia.fstream") && A.recent[0][0] == '/');
    CHECK(strstr(A.recent[1], "examples/mandelbrot.fstream"));
    for (int k = 0; k < 20; k++) {
        char name[64];
        snprintf(name, sizeof name, "no-such-%d.fstream", k);
        note_recent(name);
    }
    CHECK_INT(A.nrecent, 12);
    CHECK(strstr(A.recent[0], "no-such-19.fstream") && strstr(A.recent[11], "no-such-8.fstream"));
    A.nrecent = 0;
    load_recent(); /* (read back) */
    CHECK_INT(A.nrecent, 12);
    CHECK(strstr(A.recent[0], "no-such-19.fstream") != NULL);
    A = saved;
    if (saved_env) setenv("FS_CONFIG_DIR", saved_env, 1);
    else unsetenv("FS_CONFIG_DIR");
    free(saved_env);
#endif
}

/* which values are settings: @config variables (even when @prepare adjusts them), not what @prepare computes */
TEST(app_settings_and_prepare) {
    Program *p = program_load("@config\n"
                              "a : C = 0.3\n"
                              "@global\n"
                              "w0 := 1\n"
                              "k : Z = 2\n"
                              "q := 0\n"
                              "@prepare\n"
                              "t := a\n"
                              "a = 0\n"
                              "a = t\n"
                              "q = 2 k\n"
                              "if k > 0 then\n"
                              "    w0 = w0 + 1\n"
                              "end\n"
                              "@viewer m c\n"
                              "color = .black\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    Sym *a = NULL, *w0 = NULL, *k = NULL, *q = NULL;
    for (int i = 0; i < p->globals.len; i++) {
        Sym *g = p->globals.data[i];
        if (!strcmp(g->name, "a")) a = g;
        if (!strcmp(g->name, "w0")) w0 = g;
        if (!strcmp(g->name, "k")) k = g;
        if (!strcmp(g->name, "q")) q = g;
    }
    CHECK(a && w0 && k && q);
    if (a && w0 && k && q) {
        CHECK(script_setting(p, a));  /* (in @config) */
        CHECK(script_setting(p, w0)); /* (only adjusted, inside an if of @prepare) */
        CHECK(!script_setting(p, q)); /* (computed by @prepare every time) */
        CHECK(script_setting(p, k));
        CHECK(!script_setting(p, p->s_mouse) && !script_setting(p, p->viewers[0]->center));
    }
    program_free(p);
}

/* the language reference shown in the app (F2): docs/language.md parsed into blocks */
TEST(app_reference_parse) {
    size_t len;
    char *md = fs_read_file("docs/language.md", &len);
    CHECK(md != NULL);
    if (!md) return;
    RdBlockVec blocks = {0};
    rd_parse(md, &blocks);
    int fences = 0;
    for (const char *p = md; (p = strstr(p, "```")) != NULL; p += 3) fences++;
    int code = 0, tables = 0, h1 = 0, h2 = 0, bullets = 0;
    for (int i = 0; i < blocks.len; i++) {
        RdBlock *b = &blocks.data[i];
        code += b->kind == RD_CODE;
        h1 += b->kind == RD_H1;
        h2 += b->kind == RD_H2;
        bullets += b->kind == RD_BULLET;
        if (b->kind == RD_TABLE) {
            tables++;
            CHECK(b->rows >= 2 && b->cols >= 2);
            for (int k = 0; k < b->rows * b->cols; k++) CHECK(b->cells[k] != NULL);
        }
    }
    CHECK_INT(code, fences / 2);
    CHECK(tables >= 3 && h1 == 1 && h2 >= 8 && bullets >= 20);
    CHECK(blocks.data[0].kind == RD_H1 && !strcmp(blocks.data[0].text, "The FractalStream language"));
    rd_free_blocks(&blocks);
    free(md);
    /* the bits of Markdown: a paragraph joined over lines, nested bullets, a table, an unclosed code block */
    rd_parse("Some text\ngoes on.\n\n- one\n  - two\n    more\n| a | b |\n|---|---|\n| 1 | 2 |\n```\ncode", &blocks);
    CHECK_INT(blocks.len, 5);
    if (blocks.len == 5) {
        CHECK_STR(blocks.data[0].text, "Some text goes on.");
        CHECK(blocks.data[1].kind == RD_BULLET && blocks.data[1].indent == 0);
        CHECK(blocks.data[2].kind == RD_BULLET && blocks.data[2].indent == 1);
        CHECK_STR(blocks.data[2].text, "two more");
        CHECK(blocks.data[3].kind == RD_TABLE && blocks.data[3].rows == 2 && blocks.data[3].cols == 2);
        CHECK_STR(blocks.data[3].cells[3], "2");
        CHECK(blocks.data[4].kind == RD_CODE);
        CHECK_STR(blocks.data[4].text, "code");
    }
    rd_free_blocks(&blocks);
}

/* what the editor says about a word under the mouse */
TEST(app_hover_info) {
    Program *p = program_load("@config\n"
                              "a : R = 0.3 in 0..1 \"The a\"\n"
                              "define f(z, n : Z) = z^n\n"
                              "@viewer mandel c \"Parameter plane\"\n"
                              "color = hsv(re(f(c, 2)), 1, 1)\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    const char *h = hover_info(NULL, ".maxIterations");
    CHECK(h && strstr(h, "iteration limit"));
    h = hover_info(NULL, "a");
    CHECK(h && strstr(h, "a : R in 0..1") && strstr(h, "The a") && strstr(h, "config"));
    h = hover_info(NULL, "f");
    CHECK(h && strstr(h, "f(z : C, n : Z) : C"));
    h = hover_info(NULL, "hsv");
    CHECK_MSG(h && strstr(h, "hsv("), "%s", h ? h : "(null)");
    h = hover_info(NULL, "re");
    CHECK(h && strstr(h, "re(C) : R"));
    h = hover_info(NULL, "mandel.center");
    CHECK(h && strstr(h, "the centre"));
    h = hover_info(NULL, "mandel");
    CHECK(h && strstr(h, "Parameter plane"));
    h = hover_info(NULL, ".c");
    CHECK(h && strstr(h, ".c : C"));
    CHECK(hover_info(NULL, "nothing_here") == NULL);
    h = hover_info(NULL, "c"); /* (the viewer's variable: a local of the section) */
    CHECK_MSG(h && strstr(h, "c : C"), "%s", h ? h : "(null)");
    app_test_end();
}

/* ---- the bars at the window's edges ---------------------------------------------------------------------------- */
/* one frame of the bars with the test input (tests/test_ui.c), 800 x 600, no window */
static void app_test_bars_frame(void) {
    A.in = T;
    tu_begin();
    layout_panes(800, 600);
    A.nvis = 0;
    layout_compute(A.root, A.area);
    for (int c = 0; c < MAX_FLOATS; c++)
        if (A.floats[c] >= 0) {
            VisTile t = {A.floats[c], float_rect(c), -1, c};
            A.vis[A.nvis++] = t;
        }
    bars();
    tu_end();
}

/* the centre of a bar's item (kind, viewer, what), or false if the bar doesn't have it */
static bool app_test_bar_item(bool left, int kind, int viewer, int what, float *x, float *y) {
    BarItem items[BAR_ITEMS];
    float total;
    int n = left ? left_bar_items(items, &total) : right_bar_items(items, &total);
    Rect r = left ? A.lbar_r : A.rbar_r;
    for (int i = 0; i < n; i++)
        if (items[i].kind == kind && items[i].viewer == viewer && items[i].what == what) {
            *x = r.x + r.w / 2;
            *y = r.y + items[i].y + bar_button() / 2;
            return true;
        }
    return false;
}

static bool app_test_bar_click(bool left, int kind, int viewer, int what) {
    float x, y;
    if (!app_test_bar_item(left, kind, viewer, what, &x, &y)) return false;
    tu_move(x, y);
    app_test_bars_frame();
    tu_press(0, 1);
    app_test_bars_frame();
    tu_release(0);
    app_test_bars_frame();
    return true;
}

TEST(app_bars) {
    Program *p = program_load("@viewer mandel c \"Parameter plane\"\n"
                              "color = .black\n"
                              "@viewer julia z \"Julia set\"\n"
                              "color = .white\n"
                              "@julia.tool \"Orbit\" \"O\" \"timeline\"\n"
                              "draw point at z\n"
                              "@tool \"Mark\" \"M\"\n"
                              "draw point at .lastClick\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    for (int v = 0; v < p->nviewers; v++) A.vrt[v].tool = TOOL_PAN;
    A.hover_viewer = A.hover_tile = -1;
    layout_reset(0);
    tu_reset(1.0f);
    app_test_bars_frame();
    /* left: the script, then view 1 with its tools (Pan, Pick point, Mark), then view 2 alone (hidden) */
    BarItem it[BAR_ITEMS];
    float total;
    int n = left_bar_items(it, &total);
    static const int kinds[] = {BI_SCRIPT, BI_SEP, BI_VIEW, BI_TOOL, BI_TOOL, BI_TOOL, BI_SEP, BI_VIEW};
    CHECK_INT(n, ARRAY_COUNT(kinds));
    for (int i = 0; i < n && i < ARRAY_COUNT(kinds); i++) CHECK_INT(it[i].kind, kinds[i]);
    if (n == ARRAY_COUNT(kinds)) {
        CHECK(it[3].what == TOOL_PAN && it[4].what == TOOL_PICK && it[5].what == 1); /* (Mark is tool 1: for every view) */
        CHECK_INT(it[7].viewer, 1);
    }
    /* the script's button opens and closes it; the bar stays where it is */
    float x0 = A.lbar_r.x;
    CHECK(app_test_bar_click(true, BI_SCRIPT, -1, 0));
    CHECK(A.editor_open);
    CHECK(A.lbar_r.x == x0 && A.editor_r.x == A.lbar_r.x + A.lbar_r.w && A.area.x == A.editor_r.x + A.editor_r.w);
    CHECK(app_test_bar_click(true, BI_SCRIPT, -1, 0));
    CHECK(!A.editor_open);
    /* a tool */
    CHECK(app_test_bar_click(true, BI_TOOL, 0, 1));
    CHECK_INT(A.vrt[0].tool, 1);
    /* a hidden view: a click shows it in place of the focused one */
    CHECK(app_test_bar_click(true, BI_VIEW, 1, 0));
    CHECK(viewer_visible(1) && !viewer_visible(0));
    CHECK_INT(A.focus_viewer, 1);
    float x, y;
    CHECK(!app_test_bar_item(true, BI_TOOL, 0, TOOL_PAN, &x, &y)); /* (the hidden view's tools are gone) */
    CHECK(app_test_bar_click(true, BI_TOOL, 1, 0));                 /* Orbit, in the Julia set */
    CHECK_INT(A.vrt[1].tool, 0);
    /* the right bar: the config; nothing to hide while one view is shown */
    CHECK(app_test_bar_click(false, BI_CONFIG, -1, 0));
    CHECK(A.config_open);
    CHECK(A.rbar_r.x + A.rbar_r.w == 800 && A.config_r.x + A.config_r.w == A.rbar_r.x);
    CHECK(!app_test_bar_item(false, BI_ACTION, 1, ACT_HIDE, &x, &y));
    CHECK(app_test_bar_item(false, BI_FOCUS, 1, 0, &x, &y));
    /* dragging a view's number starts placing it; dropped on the right half of the view, it splits it */
    CHECK(app_test_bar_item(true, BI_VIEW, 0, 0, &x, &y));
    tu_move(x, y);
    app_test_bars_frame();
    tu_press(0, 1);
    app_test_bars_frame();
    tu_move(x + 30, y + 5);
    app_test_bars_frame();
    CHECK(A.drag == DRAG_VIEWER && A.drag_viewer == 0);
    tu_release(0);
    apply_drop(A.drag_viewer, A.area.x + A.area.w * 0.9f, A.area.y + A.area.h * 0.5f);
    A.drag = DRAG_NONE;
    app_test_bars_frame();
    CHECK(viewer_visible(0) && viewer_visible(1));
    CHECK_INT(count_leaves(A.root), 2);
    /* the focused view can be hidden now; the focus goes to the other one */
    A.focus_viewer = 0;
    app_test_bars_frame();
    CHECK(app_test_bar_click(false, BI_ACTION, 0, ACT_HIDE));
    CHECK(!viewer_visible(0) && viewer_visible(1));
    CHECK_INT(A.focus_viewer, 1);
    /* the axes and grid of the focused view */
    CHECK(app_test_bar_click(false, BI_ACTION, 1, ACT_GRID));
    CHECK(A.vrt[1].grid);
    app_test_end();
}

static const TestCase tests_app[] = {
    TESTCASE(app_script_center_keeps_its_digits),
    TESTCASE(app_view_history),
    TESTCASE(app_copy_paste_location),
    TESTCASE(app_write_values_into_script),
    TESTCASE(app_folder_listing),
    TESTCASE(app_recent_files),
    TESTCASE(app_settings_and_prepare),
    TESTCASE(app_reference_parse),
    TESTCASE(app_hover_info),
    TESTCASE(app_bars),
};
