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
}

static const TestCase tests_app[] = {
    TESTCASE(app_script_center_keeps_its_digits),
    TESTCASE(app_view_history),
    TESTCASE(app_copy_paste_location),
    TESTCASE(app_write_values_into_script),
    TESTCASE(app_folder_listing),
};
