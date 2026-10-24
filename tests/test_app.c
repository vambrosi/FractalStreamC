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
    CHECK(e != NULL && n >= 6);
    bool templates = false;   /* the numbered folders in order, then "templates" */
    for (int i = 0; e && i < n; i++) {
        if (!strcmp(e[i].name, "templates")) templates = e[i].is_dir;
        else CHECK(e[i].is_dir && atoi(e[i].name) == i);
        CHECK(e[i].name[0] != '.');
    }
    CHECK(templates);
    if (e) fs_free_dir(e, n);
    /* folders first; case aside; numbers by their value */
    FsDirEntry t[] = {{"b", false}, {"10 - x", false}, {"A", false}, {"9 - y", false}, {"z", true}, {"09 - w", false}};
    qsort(t, ARRAY_COUNT(t), sizeof *t, fs_dir_entry_cmp);
    const char *want[] = {"z", "09 - w", "9 - y", "10 - x", "A", "b"};
    for (int i = 0; i < ARRAY_COUNT(t); i++) CHECK_STR(t[i].name, want[i]);
    e = fs_list_dir("no/such/folder", &n);
    CHECK(e == NULL && n == 0);
    CHECK(fs_is_dir("examples") && !fs_is_dir("examples/0 - Basics/0 - Mandelbrot set.fstream") && !fs_is_dir("no/such"));
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

/* ---- the examples are the tutorial: they must all work, and between them use every name of the language, so
   that searching them for a name always finds an example of it ---- */

/* the scripts under dir: their texts appended to all; with `check`, each is loaded (no errors, no warnings) */
static int examples_check_dir(const char *dir, StrBuf *all, bool check) {
    int n = 0, count = 0;
    FsDirEntry *e = fs_list_dir(dir, &n);
    CHECK(e != NULL);
    for (int i = 0; e && i < n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e[i].name);
        if (e[i].is_dir) {
            count += examples_check_dir(path, all, check);
            continue;
        }
        if (!has_ext(e[i].name, ".fstream")) continue;
        size_t len;
        char *src = fs_read_file(path, &len);
        CHECK(src != NULL);
        if (!src) continue;
        sb_printf(all, "%s\n", src);
        count++;
        if (!check) {
            free(src);
            continue;
        }
        Program *p = program_load(src, path);
        if (p->diags.len) {
            printf("    %s:\n", path);
            print_diags(p);
            test_failures_++;
        }
        /* every view compiles, and draws more than one colour (a small picture on the CPU; not deep zooms, which
           need the double precision of the GPU shaders) */
        bool deep = p->s_double && p->initial_state && p->initial_state[p->s_double->slot];
        for (int v = 0; v < p->nviewers && !p->nerrors; v++) {
            Section *sec = p->viewers[v];
            u32 nw;
            char err[256];
            u32 *words = codegen_viewer(p, sec, &nw, err, sizeof err);
            if (!words) printf("    %s, viewer %s: %s\n", path, sec->name, err);
            CHECK(words != NULL);
            free(words);
            if (deep) continue;
            enum { EW = 24, EH = 18 };
            static u8 rgba[EW * EH * 4];
            if (sec->flood) flood_render_cpu(p, p->initial_state, sec, EW, EH, rgba, NULL);
            else render_cpu(p, p->initial_state, sec, EW, EH, rgba);
            bool varied = false;
            for (int k = 1; k < EW * EH && !varied; k++) varied = memcmp(rgba + 4 * k, rgba, 4) != 0;
            if (!varied) printf("    %s, viewer %s: the picture is one colour\n", path, sec->name);
            CHECK(varied);
        }
        program_free(p);
        free(src);
    }
    if (e) fs_free_dir(e, n);
    return count;
}

TEST(app_examples_load) {
    StrBuf all = {0};
    CHECK(examples_check_dir("examples", &all, true) >= 25);
    sb_free(&all);
}

/* the word w, not inside a longer name (quoted: "w", as palette names are written) */
static bool examples_have(const char *text, const char *w, bool quoted) {
    size_t n = strlen(w);
    for (const char *s = strstr(text, w); s; s = strstr(s + 1, w)) {
        char before = s > text ? s[-1] : ' ', after = s[n];
        if (quoted ? before == '"' && after == '"' : !is_ident_char(before) && !is_ident_char(after)) return true;
    }
    return false;
}

TEST(app_examples_cover_the_language) {
    StrBuf all = {0}, missing = {0};
    examples_check_dir("examples", &all, false);
    const char *text = sb_cstr(&all);
#define NEED(w, quoted) do { if (!examples_have(text, (w), (quoted))) sb_printf(&missing, " %s%s%s", (quoted) ? "\"" : "", (w), (quoted) ? "\"" : ""); } while (0)
    for (int k = 0; k < ARRAY_COUNT(keywords); k++) NEED(keywords[k].word, false);
    for (int k = 0; k < ARRAY_COUNT(completion_words); k++) NEED(completion_words[k], false);
    for (int k = 0; k < ARRAY_COUNT(completion_internals); k++) NEED(completion_internals[k], false);
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++)
        if (intrinsics[k].name[0] != '_') NEED(intrinsics[k].name, false);
    static const char *more[] = {"@config", "@global", "@prepare", "@viewer", "@tool", "projective", "button", "palette",
                                 "conformal", "mobius", "center", "pi", "e", "tau", ".j", ".k", "Z", "R", "C", "H", "P"};
    for (int k = 0; k < ARRAY_COUNT(more); k++) NEED(more[k], false);
    if (!strstr(text, ".tool \"")) sb_printf(&missing, " @VIEW.tool");
    Program *p = program_load("", "test");   /* the built-in functions written in the language, and the palettes */
    for (int i = 0; i < p->funcs.len; i++) {
        const Func *f = p->funcs.data[i];
        if (!f->prelude || f->tmpl) continue;
        if (!strncmp(f->name, "__palette_", 10)) NEED(f->name + 10, true);
        else if (f->name[0] != '_') NEED(f->name, false);
    }
    program_free(p);
#undef NEED
    if (missing.len) printf("    no example uses:%s\n", sb_cstr(&missing));
    CHECK(missing.len == 0);
    sb_free(&missing);
    sb_free(&all);
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
    note_recent("examples/0 - Basics/3 - Two views.fstream");
    note_recent("examples/0 - Basics/0 - Mandelbrot set.fstream");
    note_recent("examples/0 - Basics/3 - Two views.fstream");
    CHECK_INT(A.nrecent, 2);
    CHECK(strstr(A.recent[0], "examples/0 - Basics/3 - Two views.fstream") && A.recent[0][0] == '/');
    CHECK(strstr(A.recent[1], "examples/0 - Basics/0 - Mandelbrot set.fstream"));
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

/* points of the sphere in the config panel: shown as a complex number or ∞, typed; Enter or leaving the field sets
   the value, Escape doesn't. The fields of p1 and p2 are at y = 20 and y = 60 (one frame each call). */
static Sym *app_test_p1, *app_test_p2;
static void app_test_point_fields(void) {
    A.in = T;
    tu_begin();
    config_number(app_test_p1, 20, 20, 200);
    if (app_test_p2) config_number(app_test_p2, 20, 60, 200);
    tu_end();
}
static void app_test_point_click(float x, float y) {
    tu_move(x, y);
    app_test_point_fields();
    tu_press(0, 1);
    tu_release(0);
    app_test_point_fields();
}
static bool app_test_point_is(Sym *s, double re, double im) {
    const u32 *v = A.state + s->slot;
    double ar = wf(v[0]), ai = wf(v[1]), br = wf(v[2]), bi = wf(v[3]), d = br * br + bi * bi;
    if (!(d > 0)) {
        printf("    %s is not a finite point\n", s->name);
        return false;
    }
    double xr = (ar * br + ai * bi) / d, xi = (ai * br - ar * bi) / d;
    if (fabs(xr - re) > 1e-6 || fabs(xi - im) > 1e-6) {
        printf("    %s = %g%+gi, expected %g%+gi\n", s->name, xr, xi, re, im);
        return false;
    }
    return true;
}

TEST(app_config_point) {
    Program *p = program_load("@config\np1 : P = ∞\np2 : P = 1\n@viewer v z\ncolor = .black\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    tu_reset(1.0f);
    app_test_begin(p);
    app_test_p1 = app_test_p2 = NULL;
    for (int i = 0; i < p->globals.len; i++) {
        if (!strcmp(p->globals.data[i]->name, "p1")) app_test_p1 = p->globals.data[i];
        if (!strcmp(p->globals.data[i]->name, "p2")) app_test_p2 = p->globals.data[i];
    }
    CHECK(app_test_p1 && app_test_p1->type == t_proj && app_test_p2);
    if (app_test_p1 && app_test_p2) {
        Sym *s1 = app_test_p1, *s2 = app_test_p2;
        StrBuf sb = {0};
        CHECK(value_literal(&sb, t_proj, A.state + s1->slot) && !strcmp(sb_cstr(&sb), "∞"));
        sb_free(&sb);
        app_test_point_fields();
        app_test_point_click(60, 30); /* the click selects everything: typing replaces it; Enter sets it */
        tu_type("0.5-2i");
        app_test_point_fields();
        CHECK_STR(A.text_buf, "0.5-2i");
        tu_key(UI_KEY_ENTER, 0);
        app_test_point_fields();
        app_test_point_fields();
        CHECK(app_test_point_is(s1, 0.5, -2));
        app_test_point_click(60, 30); /* leaving the field (a click elsewhere) sets it too */
        tu_type("7");
        app_test_point_fields();
        app_test_point_click(400, 400);
        app_test_point_fields();
        CHECK(app_test_point_is(s1, 7, 0));
        app_test_point_click(60, 70); /* from one point to the other: the first is set, its text isn't lost */
        tu_type("9i");
        app_test_point_fields();
        app_test_point_click(60, 30);
        app_test_point_fields();
        CHECK(app_test_point_is(s2, 0, 9));
        tu_type("3");
        app_test_point_fields();
        tu_key(UI_KEY_ESCAPE, 0); /* Escape: as it was */
        app_test_point_fields();
        app_test_point_fields();
        CHECK(app_test_point_is(s1, 7, 0));
        CHECK(!A.text_name[0]);
        u32 inf[4];
        CHECK(parse_proj_literal("inf", inf) && wf(inf[0]) == 1 && wf(inf[2]) == 0);
        CHECK(!parse_proj_literal("not a number", inf));
    }
    app_test_end();
}

/* ---- the bars at the window's edges ---------------------------------------------------------------------------- */
/* one frame of the bars with the test input (tests/test_ui.c), 800 x 600, no window */
static void app_test_bars_frame(void) {
    A.in = T;
    tu_begin();
    layout_tiles(800, 600);
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
    /* left: the script, then view 1 with its tools (Pan, Pick point, Mark, Inspect), then view 2 alone (hidden) */
    BarItem it[BAR_ITEMS];
    float total;
    int n = left_bar_items(it, &total);
    static const int kinds[] = {BI_SCRIPT, BI_SEP, BI_VIEW, BI_TOOL, BI_TOOL, BI_TOOL, BI_TOOL, BI_SEP, BI_VIEW};
    CHECK_INT(n, ARRAY_COUNT(kinds));
    for (int i = 0; i < n && i < ARRAY_COUNT(kinds); i++) CHECK_INT(it[i].kind, kinds[i]);
    if (n == ARRAY_COUNT(kinds)) {
        CHECK(it[3].what == TOOL_PAN && it[4].what == TOOL_PICK && it[5].what == 1); /* (Mark is tool 1: for every view) */
        CHECK(it[6].what == TOOL_INSPECT);
        CHECK_INT(it[8].viewer, 1);
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

/* what tools print goes to the Info pane like their drawings (replaced at each run, built up with .keep); Inspect
   shows a view's values at a point */
TEST(app_info_and_inspect) {
    Program *p = program_load("@config\n"
                              "c := 0.25\n"
                              "@global\n"
                              "print \"hello\"\n"
                              "@viewer v z\n"
                              "w := z\n"
                              "k : Z = 0\n"
                              "while w not escapes do\n"
                              "    w = w^2 + c\n"
                              "    k += 1\n"
                              "end\n"
                              "print \"k is \" + k\n"
                              "color = gray(k / 50)\n"
                              "@v.tool \"Once\"\n"
                              "print \"at \" + z\n"
                              "@v.tool \"Log\"\n"
                              ".keep = true\n"
                              "print \"at \" + z\n"
                              "@tool \"Clear\" button\n"
                              "erase \"Log\"\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    interp_init(&A.interp, p, A.state);
    A.draws = (DrawItemVec *)xcalloc((size_t)p->ntools + 1, sizeof(DrawItemVec));
    outs_init();
    layout_reset(0);
    CHECK_STR(sb_cstr(&A.outs[out_script()]), "hello\n");
    run_tool(0, 0, TOOL_EV_PRESS, 1, 0);
    run_tool(0, 0, TOOL_EV_PRESS, 2, 0);
    CHECK_STR(sb_cstr(&A.outs[0]), "at 2\n");
    CHECK(A.info_open); /* (a click that printed opened the pane) */
    run_tool(1, 0, TOOL_EV_PRESS, 1, 0);
    run_tool(1, 0, TOOL_EV_PRESS, 2, 0);
    CHECK_STR(sb_cstr(&A.outs[1]), "at 1\nat 2\n");
    run_tool(2, 0, TOOL_EV_CLICK, 0, 0); /* erase "Log" */
    CHECK_STR(sb_cstr(&A.outs[1]), "");
    CHECK_STR(sb_cstr(&A.outs[0]), "at 2\n");
    inspect_at(0, 3, 0); /* escapes at once: w = 3, k = 0 */
    const char *s = sb_cstr(&A.outs[out_inspect()]);
    CHECK_MSG(strstr(s, "k is 0\n") && strstr(s, "k : Z = 0\n") && strstr(s, "w : C = 3\n") && strstr(s, "z : C = 3\n"),
              "%s", s);
    inspect_at(0, 0, 0); /* stays bounded (c = 1/4): the loop is stuck */
    s = sb_cstr(&A.outs[out_inspect()]);
    CHECK_MSG(strstr(s, ".stuck = true") && strstr(s, "k : Z = 1000"), "%s", s);
    CHECK(wf(A.state[find_global(p, "c")->slot]) == 0.25f); /* (the viewer ran on a copy of the state) */
    interp_free(&A.interp);
    outs_free();
    for (int t = 0; t < p->ntools; t++) vec_free(A.draws[t]);
    free(A.draws);
    app_test_end();
    /* a view of points of the sphere: Inspect looks at the pixel's point, exact at infinity (here the centre of the
       view, turned by the Möbius map z = 1/w) */
    p = program_load("@viewer sphere z : P \"sphere\" projective\n"
                     "color = if z == ∞ then .red else .blue\n", "t");
    CHECK_INT(p->nerrors, 0);
    if (p->nerrors) { program_free(p); return; }
    app_test_begin(p);
    interp_init(&A.interp, p, A.state);
    A.draws = (DrawItemVec *)xcalloc((size_t)p->ntools + 1, sizeof(DrawItemVec));
    outs_init();
    layout_reset(0);
    float m8[8] = {0, 0, 1, 0, 1, 0, 0, 0};
    for (int k = 0; k < 8; k++) A.state[p->viewers[0]->mobius->slot + k] = fw(m8[k]);
    A.nvis = 1;
    A.vis[0].viewer = 0;
    A.vis[0].r = (Rect){0, 0, 65, 65};
    A.in.mouse_x = A.in.mouse_y = 32.5f;
    inspect_at(0, NAN, NAN);
    s = sb_cstr(&A.outs[out_inspect()]);
    CHECK_MSG(strstr(s, "at z = ∞") && strstr(s, "color = [1, 0, 0]"), "%s", s);
    interp_free(&A.interp);
    outs_free();
    free(A.draws);
    app_test_end();
}

/* ---- the layout as the script changes (no GPU: the viewers have no pipelines) ----------------------------------- */
#define APP_TEST_VIEW(name) "@viewer " name " z\ncolor = .black\n"

/* load `src` as the app does (LOAD_OPEN: another script, LOAD_RUN: the same script edited); false if it has errors
   (then the running program stays, and the views whose sections are gone close) */
static bool app_test_load(const char *src, int mode) {
    Program *p = program_load(src, "t");
    if (p->nerrors) {
        if (mode == LOAD_RUN && A.prog) mark_gone(p);
        program_free(p);
        return false;
    }
    adopt_program(p, (ViewerRT *)xcalloc((size_t)p->nviewers + 1, sizeof(ViewerRT)), mode);
    return true;
}

static App app_test_saved_app;
static void app_test_start(void) {
    app_test_saved_app = A;
    memset(&A, 0, sizeof A);
    A.ed = editor_create();
    A.editor_frac = 0.4f;
    A.config_frac = 0.27f;
    A.info_frac = 0.4f;
    A.hover_viewer = A.hover_tile = -1;
    closed_layout.n = 0;
    tu_reset(1.0f);
}
static void app_test_stop(void) {
    if (A.prog) {
        free_viewers();
        interp_free(&A.interp);
        outs_free();
        program_free(A.prog);
    }
    free(A.state);
    free(A.uploaded);
    free(A.prepare_snap);
    editor_destroy(A.ed);
    closed_layout.n = 0;
    A = app_test_saved_app;
}

static void app_test_layout_node(int n, StrBuf *sb) {
    if (n < 0) {
        sb_puts(sb, "-");
        return;
    }
    LNode *nd = &A.nodes[n];
    if (nd->kind == N_LEAF) {
        int v = nd->viewer;
        sb_printf(sb, "%s%s", leaf_gone(n) ? "~" : "", v >= 0 && v < A.prog->nviewers ? A.prog->viewers[v]->name : "?");
        return;
    }
    sb_putc(sb, '(');
    app_test_layout_node(nd->a, sb);
    sb_putc(sb, nd->kind == N_HSPLIT ? '|' : '/');
    app_test_layout_node(nd->b, sb);
    sb_putc(sb, ')');
}

/* the layout as text: "(va|(vb/vc))" (| side by side, / one over the other, ~ closed: its section is gone), then the
   floating views ("vc@1": in corner 1) and the focused view ("*va") */
static const char *app_test_layout(void) {
    static char out[512];
    StrBuf sb = {0};
    app_test_layout_node(A.root, &sb);
    for (int c = 0; c < MAX_FLOATS; c++)
        if (A.floats[c] >= 0) sb_printf(&sb, " %s@%d", A.prog->viewers[A.floats[c]]->name, c);
    sb_printf(&sb, " *%s", A.focus_viewer >= 0 ? A.prog->viewers[A.focus_viewer]->name : "-");
    snprintf(out, sizeof out, "%s", sb_cstr(&sb));
    sb_free(&sb);
    return out;
}

/* one frame of the whole UI (as frame() builds it, without the GPU), 800 x 600 */
static void app_test_ui_frame(void) {
    A.in = T;
    tu_begin();
    layout_tiles(800, 600);
    frame_ui(800, 600);
    tu_end();
}

/* a view whose section is deleted closes; the others keep their places; a renamed view keeps its place; an undo brings
   a closed view back where it was */
TEST(app_layout_follows_the_script) {
    app_test_start();
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb") APP_TEST_VIEW("vc"), LOAD_OPEN));
    CHECK_STR(app_test_layout(), "(va|vb) *va"); /* (a script just opened: the first two side by side) */
    split_leaf(find_leaf(A.root, 1), DROP_BOTTOM, 2);
    A.focus_viewer = 1;
    A.nodes[A.root].ratio = 0.3f;
    CHECK_STR(app_test_layout(), "(va|(vb/vc)) *vb");
    /* vb's section deleted: its tile closes, vc takes its room, va stays as it was */
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|vc) *va");
    CHECK(A.nodes[A.root].ratio == 0.3f);
    app_test_ui_frame();
    CHECK_INT(A.nvis, 2);
    /* undo: vb is back where it was */
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|(vb/vc)) *vb");
    /* ... but not once the layout changed in between */
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|vc) *va");
    A.nodes[A.root].kind = N_VSPLIT;
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va/vc) *va");
    A.nodes[A.root].kind = N_HSPLIT;
    split_leaf(find_leaf(A.root, 2), DROP_TOP, 1);
    A.focus_viewer = 1;
    CHECK_STR(app_test_layout(), "(va|(vb/vc)) *vb");
    /* a renamed view keeps its place (and the focus) */
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("bee") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|(bee/vc)) *bee");
    /* a floating view whose section is gone goes away */
    detach_viewer(2);
    A.floats[1] = 2;
    CHECK_STR(app_test_layout(), "(va|bee) vc@1 *bee");
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("bee"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|bee) *bee");
    /* every view on screen gone: new views take their places (the focus follows its tile) */
    CHECK(app_test_load(APP_TEST_VIEW("x1") APP_TEST_VIEW("x2") APP_TEST_VIEW("x3"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(x1|x2) *x2");
    /* x1 renamed y1 takes its place; x2 gone, and x3 isn't new: x2's tile closes */
    CHECK(app_test_load(APP_TEST_VIEW("y1") APP_TEST_VIEW("x3"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "y1 *y1");
    /* every view on screen gone and none new: the first views of the script, as when it is opened */
    CHECK(app_test_load(APP_TEST_VIEW("x3"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "x3 *x3");
    CHECK(app_test_load(APP_TEST_VIEW("x3") APP_TEST_VIEW("q1") APP_TEST_VIEW("q2"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "x3 *x3");
    CHECK(app_test_load(APP_TEST_VIEW("q1") APP_TEST_VIEW("q2"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(q1|q2) *q1");
    hide_view(0);
    CHECK_STR(app_test_layout(), "q2 *q2");
    CHECK(app_test_load(APP_TEST_VIEW("y1") APP_TEST_VIEW("x3"), LOAD_RUN)); /* (q2 gone: y1, new, takes its place) */
    CHECK_STR(app_test_layout(), "y1 *y1");
    /* the last tile gone while a view floats: the floating view takes the window */
    split_leaf(A.root, DROP_RIGHT, 1);
    detach_viewer(1);
    A.floats[3] = 1;
    CHECK_STR(app_test_layout(), "y1 x3@3 *x3");
    CHECK(app_test_load(APP_TEST_VIEW("x3"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "x3 *x3");
    app_test_stop();
}

/* while the script has errors the program keeps running, but a view whose section is gone closes at once (and comes
   back if the section does) */
TEST(app_layout_gone_while_errors) {
    app_test_start();
    const char *abc = APP_TEST_VIEW("va") APP_TEST_VIEW("vb") APP_TEST_VIEW("vc");
    CHECK(app_test_load(abc, LOAD_OPEN));
    split_leaf(find_leaf(A.root, 1), DROP_BOTTOM, 2);
    A.focus_viewer = 1;
    /* vb's section deleted, and a mistake elsewhere: vb's tile closes, the others share its room */
    CHECK(!app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vc") "w = = 2\n", LOAD_RUN));
    CHECK_INT(A.prog->nviewers, 3); /* (still the program that ran) */
    CHECK_STR(app_test_layout(), "(va|(~vb/vc)) *va");
    CHECK(!viewer_visible(1) && viewer_visible(0) && viewer_visible(2));
    app_test_ui_frame();
    CHECK_INT(A.nvis, 2);
    for (int i = 0; i < A.nvis; i++) CHECK(A.vis[i].viewer != 1);
    if (A.nvis == 2) CHECK(A.vis[1].r.y == 0 && A.vis[1].r.h == 600); /* (vc has the whole height) */
    CHECK(!view_hideable(1));
    show_view(1); /* (its button does nothing) */
    CHECK(!viewer_visible(1));
    /* the section back (an undo), still with the mistake: vb shows again in its place */
    CHECK(!app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb") APP_TEST_VIEW("vc") "w = = 2\n", LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|(vb/vc)) *va");
    /* gone again, and the mistake fixed: closed for good */
    CHECK(!app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vc") "w = = 2\n", LOAD_RUN));
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|vc) *va");
    CHECK(!A.vrt[0].gone && !A.vrt[1].gone);
    /* the usual case: the @config line about a deleted view makes an error; its tile closes all the same */
    CHECK(app_test_load("@config\nvc.radius = 3\n" APP_TEST_VIEW("va") APP_TEST_VIEW("vc"), LOAD_RUN));
    CHECK(!app_test_load("@config\nvc.radius = 3\n" APP_TEST_VIEW("va"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "(va|~vc) *va");
    app_test_ui_frame();
    CHECK_INT(A.nvis, 1);
    if (A.nvis == 1) CHECK(A.vis[0].r.w == A.area.w);
    CHECK(app_test_load(APP_TEST_VIEW("va"), LOAD_RUN));
    CHECK_STR(app_test_layout(), "va *va");
    app_test_stop();
}

/* an empty script (or one with only comments) has no view: no tiles, the script's pane open */
TEST(app_layout_empty_script) {
    app_test_start();
    Program *p = program_load("# nothing yet\n\n", "t");
    CHECK_INT(p->nerrors, 0);
    CHECK_INT(p->nviewers, 0);
    program_free(p);
    p = program_load("w := z\ncolor = .white\n", "t"); /* (code without sections: one view) */
    CHECK_INT(p->nviewers, 1);
    program_free(p);
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb"), LOAD_OPEN));
    CHECK(app_test_load("", LOAD_RUN));
    CHECK_STR(app_test_layout(), "- *-");
    CHECK(A.editor_open);
    app_test_ui_frame();
    CHECK_INT(A.nvis, 0);
    A.overlay_panes = true;
    app_test_ui_frame(); /* (over a quiet background) */
    CHECK_INT(A.nvis, 0);
    CHECK_INT(bar_view(), -1);
    float x, y;
    CHECK(!app_test_bar_item(false, BI_FOCUS, 0, 0, &x, &y)); /* (no view to act on) */
    /* code typed: its view shows */
    CHECK(app_test_load("w := z\ncolor = .white\n", LOAD_RUN));
    CHECK_STR(app_test_layout(), "plane *plane");
    app_test_ui_frame();
    CHECK_INT(A.nvis, 1);
    /* and a script opened without views */
    CHECK(app_test_load("@config\nk : Z = 2 in 1..5\n", LOAD_OPEN));
    CHECK_STR(app_test_layout(), "- *-");
    app_test_ui_frame();
    CHECK_INT(A.nvis, 0);
    app_test_stop();
}

/* a view dropped beside the view next to it moves there (it used to vanish) */
TEST(app_split_beside_neighbour) {
    app_test_start();
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb"), LOAD_OPEN));
    CHECK_STR(app_test_layout(), "(va|vb) *va");
    split_leaf(find_leaf(A.root, 0), DROP_LEFT, 1);
    CHECK_STR(app_test_layout(), "(vb|va) *vb");
    split_leaf(find_leaf(A.root, 1), DROP_BOTTOM, 0);
    CHECK_STR(app_test_layout(), "(vb/va) *va");
    app_test_ui_frame();
    CHECK_INT(A.nvis, 2);
    app_test_stop();
}

/* overlay panes: the views use the whole space between the bars, the panes float over them and take the mouse where
   they are drawn; the views keep it elsewhere */
TEST(app_overlay_panes) {
    app_test_start();
    CHECK(app_test_load(APP_TEST_VIEW("va") APP_TEST_VIEW("vb"), LOAD_OPEN));
    A.overlay_panes = true;
    A.editor_open = A.config_open = true;
    A.nodes[A.root].ratio = -1; /* (the split as a new one: in the middle of what shows) */
    app_test_ui_frame();
    float bar = bar_width();
    CHECK(A.area.x == bar && A.area.x + A.area.w == 800 - bar);
    CHECK(A.free.x == A.editor_r.x + A.editor_r.w && A.free.x + A.free.w == A.config_r.x);
    CHECK(A.free.w < A.area.w);
    CHECK_INT(A.nvis, 2);
    if (A.nvis == 2) {
        VisTile *t0 = &A.vis[0], *t1 = &A.vis[1];
        CHECK(t0->r.x == A.area.x && t1->r.x + t1->r.w == A.area.x + A.area.w); /* (rendered whole, under the panes) */
        CHECK_NEAR(t0->r.x + t0->r.w, A.free.x + A.free.w / 2, 2);
        CHECK(t0->vr.x == A.free.x && t0->vr.w < t0->r.w && t1->vr.x + t1->vr.w == A.free.x + A.free.w);
        int tw, th;
        export_size(0, 1, &tw, &th);
        CHECK_INT(tw, (int)t0->r.w);
    }
    Sym *rad = A.prog->viewers[0]->radius;
    float r0 = wf(A.state[rad->slot]);
    /* the wheel over the script pane scrolls the script, not the view under it */
    tu_move(A.editor_r.x + A.editor_r.w / 2, 300);
    app_test_ui_frame();
    T.scroll_y = 1;
    app_test_ui_frame();
    CHECK_INT(A.hover_viewer, -1);
    CHECK(wf(A.state[rad->slot]) == r0);
    /* a click there is the pane's too: the view doesn't pan */
    ViewPlace pl0 = view_place(0);
    tu_press(0, 1);
    app_test_ui_frame();
    tu_move(A.editor_r.x + A.editor_r.w / 2 + 30, 340);
    app_test_ui_frame();
    tu_release(0);
    app_test_ui_frame();
    ViewPlace pl1 = view_place(0);
    CHECK(same_place(&pl0, &pl1) && A.drag == DRAG_NONE);
    /* where no pane is, the view has the wheel */
    tu_move(A.free.x + 20, 300);
    app_test_ui_frame();
    T.scroll_y = 1;
    app_test_ui_frame();
    CHECK_INT(A.hover_viewer, 0);
    CHECK(wf(A.state[rad->slot]) < r0);
    /* the panes are flush with the bars: right at its edge, the mouse is the pane's */
    tu_move(A.editor_r.x + 2, 300);
    app_test_ui_frame();
    app_test_ui_frame();
    CHECK_INT(A.hover_viewer, -1);
    /* the config pane takes the mouse from the view under it */
    tu_move(A.config_r.x + A.config_r.w / 2, 300);
    app_test_ui_frame();
    app_test_ui_frame();
    CHECK_INT(A.hover_viewer, -1);
    /* a view dropped on the free part of the first view splits it there; dropped on a pane, nothing happens */
    int leaf;
    Rect hl;
    CHECK(drop_zone(A.editor_r.x + A.editor_r.w / 2, 300, &leaf, &hl) == DROP_NONE);
    if (A.nvis == 2) {
        Rect vr = A.vis[0].vr;
        CHECK(drop_zone(vr.x + vr.w * 0.9f, vr.y + vr.h / 2, &leaf, &hl) == DROP_RIGHT);
        CHECK(hl.x >= A.free.x); /* (what will show of the new tile) */
    }
    /* beside the views: the views are between the panes again */
    A.overlay_panes = false;
    app_test_ui_frame();
    CHECK(A.area.x == A.free.x && A.area.w == A.free.w);
    CHECK(A.nvis == 2 && A.vis[0].r.x == A.free.x && A.vis[0].vr.w == A.vis[0].r.w);
    app_test_stop();
}

/* a pane's dragged edge follows the mouse, also when both panes are open and share what the views leave them */
TEST(app_pane_edges_follow_the_mouse) {
    app_test_start();
    CHECK(app_test_load(APP_TEST_VIEW("va"), LOAD_OPEN));
    for (int mode = 0; mode < 2; mode++) {
        A.overlay_panes = mode == 1;
        A.editor_open = A.config_open = true;
        A.editor_frac = 0.45f;
        A.config_frac = 0.27f;
        app_test_ui_frame();
        CHECK(A.editor_r.w < floorf(800 * 0.45f)); /* (both shrunk: the views keep a third of the window) */
        float edge = A.editor_r.x + A.editor_r.w, to = edge + 40;
        tu_move(edge, 300);
        app_test_ui_frame();
        tu_press(0, 1);
        app_test_ui_frame();
        tu_move(to, 300);
        app_test_ui_frame();
        app_test_ui_frame();
        tu_release(0);
        app_test_ui_frame();
        CHECK_NEAR(A.editor_r.x + A.editor_r.w, to, 2);
        edge = A.config_r.x;
        to = edge - 30;
        tu_move(edge, 300);
        app_test_ui_frame();
        tu_press(0, 1);
        app_test_ui_frame();
        tu_move(to, 300);
        app_test_ui_frame();
        app_test_ui_frame();
        tu_release(0);
        app_test_ui_frame();
        CHECK_NEAR(A.config_r.x, to, 2);
        tu_move(-100, -100);
        app_test_ui_frame();
    }
    app_test_stop();
}

/* the panes over the views or beside them: a button in the right bar, kept in settings.txt with the interface's size */
TEST(app_overlay_setting) {
#if FS_OS_WINDOWS
    SKIP("uses setenv");
#else
    const char *old = getenv("FS_CONFIG_DIR");
    char *saved_env = old ? xstrdup(old) : NULL;
    setenv("FS_CONFIG_DIR", "build/test-settings/overlay", 1);
    remove("build/test-settings/overlay/settings.txt");
    App saved = A;
    memset(&A, 0, sizeof A);
    load_settings();
    CHECK(A.overlay_panes); /* (on unless the settings say otherwise) */
    CHECK(A.ui_zoom == 1);
    tu_reset(1.0f);
    A.hover_viewer = A.hover_tile = -1;
    layout_reset(-1);
    app_test_bars_frame();
    CHECK(app_test_bar_click(false, BI_OVERLAY, -1, 0));
    CHECK(!A.overlay_panes);
    A.ui_zoom = 1.5f;
    save_settings();
    A.overlay_panes = true;
    A.ui_zoom = 1;
    load_settings();
    CHECK(!A.overlay_panes);
    CHECK_NEAR(A.ui_zoom, 1.5, 1e-6);
    CHECK(app_test_bar_click(false, BI_OVERLAY, -1, 0));
    CHECK(A.overlay_panes);
    A.overlay_panes = false;
    load_settings();
    CHECK(A.overlay_panes);
    CHECK_NEAR(A.ui_zoom, 1.5, 1e-6);
    A = saved;
    if (saved_env) setenv("FS_CONFIG_DIR", saved_env, 1);
    else unsetenv("FS_CONFIG_DIR");
    free(saved_env);
#endif
}

static const TestCase tests_app[] = {
    TESTCASE(app_script_center_keeps_its_digits),
    TESTCASE(app_view_history),
    TESTCASE(app_copy_paste_location),
    TESTCASE(app_write_values_into_script),
    TESTCASE(app_folder_listing),
    TESTCASE(app_examples_load),
    TESTCASE(app_examples_cover_the_language),
    TESTCASE(app_recent_files),
    TESTCASE(app_settings_and_prepare),
    TESTCASE(app_reference_parse),
    TESTCASE(app_hover_info),
    TESTCASE(app_config_point),
    TESTCASE(app_bars),
    TESTCASE(app_info_and_inspect),
    TESTCASE(app_layout_follows_the_script),
    TESTCASE(app_layout_gone_while_errors),
    TESTCASE(app_layout_empty_script),
    TESTCASE(app_split_beside_neighbour),
    TESTCASE(app_overlay_panes),
    TESTCASE(app_pane_edges_follow_the_mouse),
    TESTCASE(app_overlay_setting),
};
