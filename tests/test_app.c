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

static const TestCase tests_app[] = {
    TESTCASE(app_script_center_keeps_its_digits),
    TESTCASE(app_view_history),
    TESTCASE(app_copy_paste_location),
};
