/* test_lang.c - language front-end and interpreter tests. */

static Program *load_src(const char *src) { return program_load(src, "test"); }

static void print_diags(Program *p) {
    StrBuf sb = {0};
    program_format_diags(p, &sb);
    printf("%s", sb_cstr(&sb));
    sb_free(&sb);
}

static Program *load_ok_(const char *src, int line) {
    Program *p = load_src(src);
    if (p->nerrors) {
        printf("    unexpected errors (test line %d):\n", line);
        print_diags(p);
        test_failures_++;
    }
    return p;
}
#define LOAD_OK(src) load_ok_(src, __LINE__)

static void expect_error_(const char *src, const char *needle, int line) {
    Program *p = load_src(src);
    bool found = false;
    for (int i = 0; i < p->diags.len; i++)
        if (p->diags.data[i].severity == DIAG_ERROR && strstr(p->diags.data[i].msg, needle)) found = true;
    if (!found) {
        test_failures_++;
        printf("    expected an error containing \"%s\" (test line %d), got:\n", needle, line);
        print_diags(p);
        if (!p->diags.len) printf("    (no diagnostics)\n");
    }
    program_free(p);
}
#define EXPECT_ERROR(src, needle) expect_error_(src, needle, __LINE__)

static Sym *find_global(Program *p, const char *name) {
    for (int i = 0; i < p->globals.len; i++)
        if (strcmp(p->globals.data[i]->name, name) == 0) return p->globals.data[i];
    return NULL;
}

static double gnum(Program *p, const char *name, int comp) {
    Sym *s = find_global(p, name);
    if (!s || !p->initial_state) {
        printf("    no global '%s'\n", name);
        test_failures_++;
        return NAN;
    }
    u32 w = p->initial_state[s->slot + comp];
    if (s->type == t_int || s->type == t_bool || (s->type->kind == T_VEC && s->type->elem == t_int)) return (double)(i32)w;
    if (s->type->kind == T_LIST && comp == 0) return (double)(i32)w;
    return (double)wf(w);
}

static void viewer_rgb(Program *p, int v, float re, float im, float rgb[3]) {
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    rgb[0] = rgb[1] = rgb[2] = -1;
    if (!interp_eval_viewer(&I, p->viewers[v], re, im, rgb)) printf("    viewer failed: %s\n", I.error);
    interp_free(&I);
    free(state);
}

TEST(default_viewer) {
    Program *p = LOAD_OK("color = .red\n");
    CHECK_INT(p->nviewers, 1);
    CHECK_STR(p->viewers[0]->name, "plane");
    float c[3];
    viewer_rgb(p, 0, 0.3f, 0.1f, c);
    CHECK_NEAR(c[0], 1, 1e-6);
    CHECK_NEAR(c[1], 0, 1e-6);
    program_free(p);
}

TEST(mandelbrot) {
    Program *p = LOAD_OK(
        "# Mandelbrot set\n"
        "w := 0\n"
        "while w not escapes do\n"
        "    w = w^2 + z\n"
        "end\n"
        "if .stuck then\n"
        "    color = .black\n"
        "else\n"
        "    color = .white\n"
        "end\n");
    float c[3];
    viewer_rgb(p, 0, -0.1f, 0.1f, c);
    CHECK_NEAR(c[0], 0, 1e-6);
    viewer_rgb(p, 0, 1.0f, 1.0f, c);
    CHECK_NEAR(c[0], 1, 1e-6);
    program_free(p);
}

TEST(language_sketch) {
    /* the first sketch of the language, made valid */
    Program *p = LOAD_OK(
        "@global\n"
        "j : Z = 10\n"
        "z0 := 10\n"
        "r : R = 2.0\n"
        "w := j + 20i\n"
        "w = cos w + 2(1 - w)\n"
        "define f(x, y) = (u, v) where\n"
        "    u = x + y\n"
        "    v = 0\n"
        "    for k in 1..10 do\n"
        "        u += i\n"
        "        if k == 2 then\n"
        "            break\n"
        "        else if k == 3 then\n"
        "            return\n"
        "        end\n"
        "    end\n"
        "    v = 20\n"
        "end\n"
        "(a, b) := f(1, 2)\n"
        "do\n"
        "    r -= 1\n"
        "while r not vanishes or |w| > 1e30 end\n"
        "@viewer plane z \"z-plane\" \"Ctrl + 1\"\n"
        "color = .red\n");
    double cw = cos(10.0) * cosh(20.0), sw = -sin(10.0) * sinh(20.0); /* cos(10+20i) */
    CHECK_NEAR(gnum(p, "w", 0) / (cw + 2 * (1 - 10)), 1, 1e-4);
    CHECK_NEAR(gnum(p, "w", 1) / (sw - 40), 1, 1e-4);
    CHECK_NEAR(gnum(p, "a", 0), 3, 1e-6);
    CHECK_NEAR(gnum(p, "a", 1), 2, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 20, 1e-6);
    CHECK_NEAR(gnum(p, "r", 0), 0, 1e-6);
    program_free(p);
}

TEST(types_and_conversions) {
    Program *p = LOAD_OK(
        "@global\n"
        "n : Z = 7\n"
        "x : R = n / 2\n"
        "c := 7\n"          /* untyped: the type of the value (7 is Z, 2.5 is R, 2i is C) */
        "d := n\n"
        "t := n / 2 + pi\n" /* R */
        "u := 2 pi + 1\n"   /* R */
        "w := 2 + 0i\n"     /* C */
        "q : H = 1 + 2i\n"
        "v : R^3 = [1, 2, 3]\n"
        "k : Z = 2^10\n"
        "h : R = 2^-1\n"
        "m : Z = -7 mod 3\n"
        "f : R = 7.5 mod 2\n"
        "b := 3 > 2 and not false\n");
    CHECK_NEAR(gnum(p, "x", 0), 3.5, 1e-6);
    CHECK(find_global(p, "c") && find_global(p, "c")->type == t_int);
    CHECK(find_global(p, "d") && find_global(p, "d")->type == t_int);
    CHECK(find_global(p, "t") && find_global(p, "t")->type == t_real);
    CHECK(find_global(p, "u") && find_global(p, "u")->type == t_real);
    CHECK(find_global(p, "w") && find_global(p, "w")->type == t_complex);
    CHECK_NEAR(gnum(p, "q", 1), 2, 1e-6);
    CHECK_NEAR(gnum(p, "v", 2), 3, 1e-6);
    CHECK_NEAR(gnum(p, "k", 0), 1024, 0);
    CHECK_NEAR(gnum(p, "h", 0), 0.5, 1e-7);
    CHECK_NEAR(gnum(p, "m", 0), 2, 0);
    CHECK_NEAR(gnum(p, "f", 0), 1.5, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 1, 0);
    program_free(p);
    EXPECT_ERROR("@global\nr : R = 2i\n", "must be R");
    EXPECT_ERROR("@global\nk : Z = 2.5\n", "floor, ceil or round");
    EXPECT_ERROR("@global\nn := 3.5\nfor k in 1..n do\nend\n", "integers");
    EXPECT_ERROR("@global\nx : Integers = 2\n", "write Integer (or Z)");
    EXPECT_ERROR("@global\nx : Reals = 2\n", "write Real (or R)");
    EXPECT_ERROR("@global\nb := 1i < 2\n", "can't be ordered");
}

TEST(implicit_multiplication) {
    Program *p = LOAD_OK(
        "@global\n"
        "w := 0.5 + 0.25i\n"
        "a := cos w + 2(1 - w)\n"
        "b := cos(w) + 2 * (1 - w)\n"
        "t : R = 0.1\n"
        "s1 := sin 2 pi t\n"
        "s2 := sin(2 * pi * t)\n"
        "p1 := sin t cos t\n"
        "p2 := sin(t) * cos(t)\n"
        "y := 3|w|\n"
        "e1 := exp -w^2\n"
        "e2 := exp(-(w^2))\n"
        "d := 2/4 w\n");
    CHECK_NEAR(gnum(p, "a", 0), gnum(p, "b", 0), 1e-6);
    CHECK_NEAR(gnum(p, "a", 1), gnum(p, "b", 1), 1e-6);
    CHECK_NEAR(gnum(p, "s1", 0), gnum(p, "s2", 0), 1e-6);
    CHECK_NEAR(gnum(p, "p1", 0), gnum(p, "p2", 0), 1e-6);
    CHECK_NEAR(gnum(p, "y", 0), 3 * sqrt(0.25 + 0.0625), 1e-5);
    CHECK_NEAR(gnum(p, "e1", 0), gnum(p, "e2", 0), 1e-6);
    CHECK_NEAR(gnum(p, "e1", 1), gnum(p, "e2", 1), 1e-6);
    CHECK_NEAR(gnum(p, "d", 0), 0.25, 1e-6);
    program_free(p);
}

TEST(complex_functions) {
    Program *p = LOAD_OK(
        "@global\n"
        "a := exp(i pi)\n"
        "b := sqrt(-4 + 0i)\n"
        "c := log(-1 + 0i)\n"
        "d := (1 + i)^2\n"
        "e2 := (1 + i)^-1\n"
        "f := (2 + 0i)^0.5\n"
        "g := i^i\n"
        "h := conj(3 + 4i) / |3 + 4i|\n"
        "s := sin(1 + 2i)\n"
        "ar : R = arg(-1 - 0.0001i)\n");
    CHECK_NEAR(gnum(p, "a", 0), -1, 1e-6);
    CHECK_NEAR(gnum(p, "a", 1), 0, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 0, 1e-6);
    CHECK_NEAR(gnum(p, "b", 1), 2, 1e-6);
    CHECK_NEAR(gnum(p, "c", 1), M_PI, 1e-6);
    CHECK_NEAR(gnum(p, "d", 0), 0, 1e-6);
    CHECK_NEAR(gnum(p, "d", 1), 2, 1e-6);
    CHECK_NEAR(gnum(p, "e2", 0), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "e2", 1), -0.5, 1e-6);
    CHECK_NEAR(gnum(p, "f", 0), sqrt(2.0), 1e-6);
    CHECK_NEAR(gnum(p, "g", 0), exp(-M_PI / 2), 1e-6);
    CHECK_NEAR(gnum(p, "h", 0), 0.6, 1e-6);
    CHECK_NEAR(gnum(p, "h", 1), -0.8, 1e-6);
    CHECK_NEAR(gnum(p, "s", 0), sin(1.0) * cosh(2.0), 1e-5);
    CHECK_NEAR(gnum(p, "s", 1), cos(1.0) * sinh(2.0), 1e-5);
    CHECK_NEAR(gnum(p, "ar", 0), -M_PI, 1e-3);
    program_free(p);
}

TEST(loops_and_stuck) {
    Program *p = LOAD_OK(
        "@config\n"
        ".maxIterations = 50\n"
        "@global\n"
        "s : Z = 0\n"
        "for k in 1..10 do\n"
        "    s += k\n"
        "end\n"
        "it1 : Z = .iterations\n"
        "st1 := .stuck\n"
        "t : Z = 0\n"
        "for k in 10..1 by -3 do\n"
        "    t = t * 100 + k\n"
        "end\n"
        "x := 0\n"
        "while true do\n"
        "    x += 1\n"
        "end\n"
        "st2 := .stuck\n"
        "it2 : Z = .iterations\n"
        "u : Z = 0\n"
        "for k in 1..10 do\n"
        "    if k mod 2 == 0 then\n"
        "        continue\n"
        "    end\n"
        "    if k > 7 then\n"
        "        break\n"
        "    end\n"
        "    u += k\n"
        "end\n"
        "st3 := .stuck\n"
        "dw : Z = 0\n"
        "do\n"
        "    dw += 1\n"
        "while dw < 5 end\n"
        "nested : Z = 0\n"
        "for a in 1..3 do\n"
        "    for b in a..3 do\n"
        "        nested += 1\n"
        "    end\n"
        "end\n");
    CHECK_NEAR(gnum(p, "s", 0), 55, 0);
    CHECK_NEAR(gnum(p, "it1", 0), 10, 0);
    CHECK_NEAR(gnum(p, "st1", 0), 0, 0);
    CHECK_NEAR(gnum(p, "t", 0), 10070401, 0);
    CHECK_NEAR(gnum(p, "x", 0), 50, 0);
    CHECK_NEAR(gnum(p, "st2", 0), 1, 0);
    CHECK_NEAR(gnum(p, "it2", 0), 50, 0);
    CHECK_NEAR(gnum(p, "u", 0), 1 + 3 + 5 + 7, 0);
    CHECK_NEAR(gnum(p, "st3", 0), 0, 0);
    CHECK_NEAR(gnum(p, "dw", 0), 5, 0);
    CHECK_NEAR(gnum(p, "nested", 0), 6, 0);
    program_free(p);
}

TEST(epsilon_comparisons) {
    Program *p = LOAD_OK(
        "@global\n"
        "a := 0.1 + 0.2 == 0.3\n"
        "b := 1 == 1.001\n"
        "c := (1e-7 + 0i) vanishes\n"
        "d := (3 + 4i) escapes\n"
        "e2 := 1.5 escapes\n");
    CHECK_NEAR(gnum(p, "a", 0), 1, 0);
    CHECK_NEAR(gnum(p, "b", 0), 0, 0);
    CHECK_NEAR(gnum(p, "c", 0), 1, 0);
    CHECK_NEAR(gnum(p, "d", 0), 1, 0);
    CHECK_NEAR(gnum(p, "e2", 0), 0, 0);
    program_free(p);
}

TEST(vectors_colors_lists) {
    Program *p = LOAD_OK(
        "@global\n"
        "v : R^3 = [1, 2, 3]\n"
        "v[2] = 5\n"
        "w := 2 v + [1, 1, 1]\n"
        "n : R = |[3, 4]|\n"
        "col : Color = mix(.red, .blue, 0.25)\n"
        "hs : Color = hsv(1/3, 1, 1)\n"
        "L : List of C = []\n"
        "append(L, 1 + i)\n"
        "append(L, 2)\n"
        "sum := 0\n"
        "for x in L do\n"
        "    sum += x\n"
        "end\n"
        "len : Z = |L|\n"
        "second := L[2]\n"
        "zc : C = 3 + 4i\n"
        "zr : R = zc[1] + zc[2]\n");
    CHECK_NEAR(gnum(p, "v", 1), 5, 0);
    CHECK_NEAR(gnum(p, "w", 0), 3, 1e-6);
    CHECK_NEAR(gnum(p, "w", 1), 11, 1e-6);
    CHECK_NEAR(gnum(p, "n", 0), 5, 1e-6);
    CHECK_NEAR(gnum(p, "col", 0), 0.75, 1e-6);
    CHECK_NEAR(gnum(p, "col", 2), 0.25, 1e-6);
    CHECK_NEAR(gnum(p, "hs", 0), 0, 1e-5);
    CHECK_NEAR(gnum(p, "hs", 1), 1, 1e-5);
    CHECK_NEAR(gnum(p, "sum", 0), 3, 1e-6);
    CHECK_NEAR(gnum(p, "sum", 1), 1, 1e-6);
    CHECK_NEAR(gnum(p, "len", 0), 2, 0);
    CHECK_NEAR(gnum(p, "second", 0), 2, 1e-6);
    CHECK_NEAR(gnum(p, "zr", 0), 7, 1e-6);
    program_free(p);
}

TEST(functions_and_overloads) {
    Program *p = LOAD_OK(
        "@global\n"
        "c := -0.5 + 0.5i\n"
        "define f(z) = z^2 + c\n"
        "define g(x : R) = x * x\n"
        "define g(z : C) = z * z * z\n"
        "define h(n : Z) = s : Z where\n"
        "    s = 0\n"
        "    for k in 1..n do\n"
        "        s += k\n"
        "    end\n"
        "end\n"
        "a := f(1)\n"
        "b : R = g(3)\n"
        "d := g(1i)\n"
        "e2 : Z = h(4)\n"
        "define setc(v) = r where\n"
        "    c = v\n"
        "    r = 1\n"
        "end\n"
        "dummy := setc(2)\n");
    CHECK_NEAR(gnum(p, "a", 0), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "a", 1), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 9, 1e-6);
    CHECK_NEAR(gnum(p, "d", 1), -1, 1e-6);
    CHECK_NEAR(gnum(p, "e2", 0), 10, 0);
    CHECK_NEAR(gnum(p, "c", 0), 2, 1e-6); /* functions can change globals */
    program_free(p);
    EXPECT_ERROR("@global\ndefine f(z) = f(z) + 1\nx := f(1)\n", "recursive");
    EXPECT_ERROR("@global\nx := sin(true)\n", "can't be applied");
}

TEST(sections_and_config) {
    Program *p = LOAD_OK(
        "@config\n"
        ".title = \"Julia\"\n"
        ".width = 640\n"
        "iters : Z = 100 in 1..1000\n"
        "mandel.center = -0.5\n"
        "mandel.radius = 1.5\n"
        "@viewer mandel c \"Parameter plane\" \"Ctrl + 1\"\n"
        "z := 0\n"
        "color = .white\n"
        "@viewer julia z \"Dynamical plane\" \"Ctrl + 2\"\n"
        "w := z\n"
        "cc := .c\n"
        "color = .black\n"
        "@tool \"Orbit\" \"O\"\n"
        ".panning = true\n"
        "w := .lastClick\n"
        "for k in 1..3 do\n"
        "    w2 := w^2 + .c\n"
        "    draw line between w and w2\n"
        "    w = w2\n"
        "end\n"
        "draw circle at .lastClick with radius 3 px and color .red\n");
    CHECK_STR(p->title, "Julia");
    CHECK_NEAR(gnum(p, "width", 0), 640, 0);
    CHECK_NEAR(gnum(p, "mandel.center", 0), -0.5, 1e-6);
    CHECK_NEAR(gnum(p, "mandel.radius", 0), 1.5, 1e-6);
    Sym *it = find_global(p, "iters");
    CHECK(it && it->is_config && it->has_range && it->range_hi == 1000);
    CHECK_INT(p->nviewers, 2);
    CHECK_INT(p->ntools, 1);
    if (p->ntools == 1) {
        CHECK(p->tools[0]->panning);
        u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
        memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
        Sym *lc = p->s_last_click;
        state[lc->slot] = fw(0.5f);
        Interp I;
        interp_init(&I, p, state);
        DrawItemVec draws = {0};
        I.draws = &draws;
        CHECK(interp_run_section(&I, p->tools[0]));
        CHECK_INT(draws.len, 4);
        if (draws.len == 4) {
            CHECK_NEAR(draws.data[0].p1[0], 0.25, 1e-6);
            CHECK(draws.data[3].shape == DRAW_CIRCLE && draws.data[3].radius_px && draws.data[3].color[0] == 1);
        }
        vec_free(draws);
        interp_free(&I);
        free(state);
    }
    program_free(p);
}

TEST(errors) {
    EXPECT_ERROR("x = 1 +\n", "expected a value");
    EXPECT_ERROR("while true do\n x = 1\n", "missing 'end'");
    EXPECT_ERROR("color = foo\n", "unknown name 'foo'");
    EXPECT_ERROR("color = .nothing\n", "unknown internal");
    EXPECT_ERROR("color = 1 + true\n", "can't use '+'");
    EXPECT_ERROR("draw point at 0\n", "only be used in tools");
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\"\n.time = 2\n", "can't be changed");
    EXPECT_ERROR("break\n", "inside a loop");
    EXPECT_ERROR("x := 1\nx\n", "does nothing");
    EXPECT_ERROR("@viewer a z\ncolor = .red\n@viewer a w\ncolor = .red\n", "already a viewer");
    EXPECT_ERROR("color = 2 3\n", "explicit *");
    EXPECT_ERROR("@global\nx := 5i\n@viewer v z\ncolor = gray(x)\n", "must be R");
    EXPECT_ERROR("@global\ndefine re(z : C) = z\n", "built-in");
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\"\nw := z\n", "only defined inside the viewer");
}

TEST(runaway_tool_is_stopped) {
    Program *p = LOAD_OK(
        "@config\n.maxIterations = 100000\n"
        "@tool \"Slow\"\n"
        "x := 0\n"
        "for a in 1..100000 do\n"
        "  for b in 1..100000 do\n"
        "    x += 1\n"
        "  end\n"
        "end\n");
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    I.step_limit = 1e6;
    CHECK(!interp_run_section(&I, p->tools[0]));
    CHECK(strstr(I.error, "too long") != NULL);
    interp_free(&I);
    free(state);
    program_free(p);
}

TEST(variable_integer_powers) {
    Program *p = LOAD_OK(
        "@global\n"
        "n : Z = 5\n"
        "m : Z = -3\n"
        "a := (1 + i)^n\n"
        "b := (1 + i)^m\n"
        "x : R = (-2)^n\n"
        "y : R = 2^m\n"
        "w := 0\n"
        "while w not escapes do\n"
        "    w = w^n + 0.1\n"
        "end\n"
        "it : Z = .iterations\n"
        "st := .stuck\n");
    CHECK_NEAR(gnum(p, "a", 0), -4, 1e-5);
    CHECK_NEAR(gnum(p, "a", 1), -4, 1e-5);
    CHECK_NEAR(gnum(p, "b", 0), -0.25, 1e-6);
    CHECK_NEAR(gnum(p, "b", 1), -0.25, 1e-6);
    CHECK_NEAR(gnum(p, "x", 0), -32, 1e-5);
    CHECK_NEAR(gnum(p, "y", 0), 0.125, 1e-7);
    CHECK_NEAR(gnum(p, "it", 0), 1000, 0); /* the library loop inside ^ must not change .iterations */
    CHECK_NEAR(gnum(p, "st", 0), 1, 0);
    program_free(p);
}

TEST(automatic_differentiation) {
    Program *p = LOAD_OK(
        "@global\n"
        "c := -0.75 + 0.1i\n"
        "cp := critical z -> z^2 + c with seed 1\n"
        "st1 := .stuck\n"
        "r := root z -> z^3 - 1 with seed 0.8 + 0.1i\n"
        "dv := derivative z -> z^3 + sin(z) at 2\n"
        "define g(z) = z^3 - 2z + c\n"
        "cg := critical g with seed 1\n"
        "e2 := derivative w -> exp(2w) / w at 1\n"
        "pw := derivative w -> w^2.5 at 4\n"
        "nc := root z -> z^2 + 1 with seed 1\n"
        "st2 := .stuck\n"
        "@viewer v z\n"
        "w := critical t -> t^3 - 3 z t with seed 1\n"
        "color = gray(|w^2 - z|)\n");
    CHECK_NEAR(gnum(p, "cp", 0), 0, 1e-5);
    CHECK_NEAR(gnum(p, "cp", 1), 0, 1e-5);
    CHECK_NEAR(gnum(p, "st1", 0), 0, 0);
    CHECK_NEAR(gnum(p, "r", 0), 1, 1e-5);
    CHECK_NEAR(gnum(p, "r", 1), 0, 1e-5);
    CHECK_NEAR(gnum(p, "dv", 0), 12 + cos(2.0), 1e-4);
    CHECK_NEAR(gnum(p, "cg", 0), sqrt(2.0 / 3.0), 1e-5);
    CHECK_NEAR(gnum(p, "e2", 0), exp(2.0), 1e-4); /* (2w - 1) e^(2w) / w^2 at 1 */
    CHECK_NEAR(gnum(p, "pw", 0), 2.5 * 8, 1e-3);
    CHECK_NEAR(gnum(p, "st2", 0), 1, 0); /* z^2 + 1 from a real seed never converges */
    float c[3];
    viewer_rgb(p, 0, 0.7f, 0.4f, c);
    CHECK_NEAR(c[0], 0, 1e-4);
    program_free(p);
    EXPECT_ERROR("@global\nx := root z -> conj(z) - 1 with seed 0\n", "can't differentiate");
    EXPECT_ERROR("@global\nx := critical z -> 3z + 1 with seed 0\n", "constant");
}

TEST(projective_mapping) {
    Program *p = LOAD_OK(
        "@config\nsphere.radius = 2\nsphere.center = 1 + i\n"
        "@viewer sphere z \"Riemann sphere\" \"Ctrl + 1\" projective\n"
        "color = gray(|z|)\n");
    Section *v = p->viewers[0];
    CHECK(v->mobius != NULL);
    float re, im;
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 100, 50, &re, &im);
    CHECK_NEAR(re, 1, 1e-5);
    CHECK_NEAR(im, 1, 1e-5);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 150, 50, &re, &im); /* edge of the smaller side */
    CHECK_NEAR(re, 3, 1e-4);
    CHECK_NEAR(im, 1, 1e-4);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 100, 0, &re, &im);
    CHECK_NEAR(re, 1, 1e-4);
    CHECK_NEAR(im, 3, 1e-4);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 125, 50, &re, &im); /* (seen from the centre: not halfway) */
    CHECK(re > 2.1f);
    program_free(p);
    /* conformal: seen from a point of the sphere, the view is the Möbius map of the pixel's position (the same points
       at the centre and at the edges) */
    p = LOAD_OK("@config\nsphere.radius = 2\nsphere.center = 1 + i\nsphere.conformal = true\n"
                "@viewer sphere z \"Riemann sphere\" \"Ctrl + 1\" projective\n"
                "color = gray(|z|)\n");
    v = p->viewers[0];
    CHECK(viewer_conformal(v, p->initial_state));
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 100, 50, &re, &im);
    CHECK_NEAR(re, 1, 1e-5);
    CHECK_NEAR(im, 1, 1e-5);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 150, 50, &re, &im);
    CHECK_NEAR(re, 3, 1e-4);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 125, 50, &re, &im); /* halfway */
    CHECK_NEAR(re, 2, 1e-4);
    viewer_pixel_to_plane(p, p->initial_state, v, 200, 100, 125, 25, &re, &im);
    CHECK_NEAR(re, 2, 1e-4);
    CHECK_NEAR(im, 2, 1e-4);
    program_free(p);
}

/* a point of P as a complex number (inf: a huge one), for checking */
static void pnum(Program *p, const char *name, double *re, double *im) {
    Sym *s = find_global(p, name);
    if (!s || s->type != t_proj || !p->initial_state) {
        printf("    no point '%s'\n", name);
        test_failures_++;
        *re = *im = NAN;
        return;
    }
    const u32 *v = p->initial_state + s->slot;
    double ar = wf(v[0]), ai = wf(v[1]), br = wf(v[2]), bi = wf(v[3]), d = br * br + bi * bi;
    if (d == 0) {
        *re = ar == 0 && ai == 0 ? NAN : INFINITY;
        *im = 0;
        return;
    }
    *re = (ar * br + ai * bi) / d;
    *im = (ai * br - ar * bi) / d;
}
#define CHECK_POINT(p, name, xre, xim) do { double r_, i_; pnum(p, name, &r_, &i_); \
    if (isinf((double)(xre))) CHECK_MSG(isinf(r_), "%s should be infinity, is %g%+gi", name, r_, i_); \
    else { CHECK_NEAR(r_, xre, 1e-5); CHECK_NEAR(i_, xim, 1e-5); } } while (0)

TEST(projective_points) {
    Program *p = LOAD_OK(
        "@global\n"
        "define f(z : P) = (z^2 + 1)/(z^2 - 1)\n"
        "define g(z : P) = z^2 + 0.5i\n"
        "define h(z : P) = (1/z + 1)/(1/z - 1)\n"
        "inf : P = [1 : 0]\n"
        "two : P = 2\n"
        "a := f(inf)\n"          /* 1: exact at infinity (no [0 : 0]) */
        "b := f(two)\n"          /* 5/3 */
        "c := g(.infinity)\n"    /* infinity */
        "d := h(0)\n"            /* 1: exact at 0 too */
        "e := 1 / inf\n"         /* 0 */
        "half := [2 : 4]\n"
        "same := inf == ∞ and half == 0.5 and half != 2\n"
        "w := 0\n"
        "w = two + 1i\n"         /* w becomes a point */
        "q3 := two^3\n"
        "qm := two^-2\n"
        "k : Z = 3\n"
        "qk := two^k\n"
        "m := |two| + re(half[1] / half[2])\n"          /* 2 + 0.5 */
        "esc := two escapes\n"       /* |2| > 2: false */
        "van := [1e-9 : 1] vanishes\n"
        "cd := chordal(0, ∞)\n"
        "zc := complex(half)\n"
        "L : List of P = []\n"
        "append(L, ∞)\n"
        "append(L, two)\n"
        "n := |L|\n"
        "count := 0\n"
        "define once(z : P) = r : P where\n" /* (pieces with calls are computed once) */
        "    count = count + 1\n"
        "    r = z\n"
        "end\n"
        "y := once(two) * once(two) + 1\n");
    CHECK_POINT(p, "a", 1, 0);
    CHECK_POINT(p, "b", 5.0 / 3, 0);
    CHECK_POINT(p, "c", INFINITY, 0);
    CHECK_POINT(p, "d", 1, 0);
    CHECK_POINT(p, "e", 0, 0);
    CHECK_POINT(p, "half", 0.5, 0);
    CHECK_NEAR(gnum(p, "same", 0), 1, 0);
    CHECK_POINT(p, "w", 2, 1);
    CHECK_POINT(p, "q3", 8, 0);
    CHECK_POINT(p, "qm", 0.25, 0);
    CHECK_POINT(p, "qk", 8, 0);
    CHECK_NEAR(gnum(p, "m", 0), 2.5, 1e-6);
    CHECK_NEAR(gnum(p, "esc", 0), 0, 0);
    CHECK_NEAR(gnum(p, "van", 0), 1, 0);
    CHECK_NEAR(gnum(p, "cd", 0), 1, 1e-6);
    CHECK_NEAR(gnum(p, "zc", 0), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "n", 0), 2, 0);
    CHECK_POINT(p, "y", 5, 0);
    CHECK_NEAR(gnum(p, "count", 0), 2, 0);
    program_free(p);
    EXPECT_ERROR("@global\np : P = 2\nx := p[3]\n", "p[1] is a and p[2] is b");
    EXPECT_ERROR("@global\np : P = 2\np[1] = 3\n", "can't be set one by one");
    EXPECT_ERROR("@global\np : P = 2\nb := p < p\n", "can't be ordered");
    EXPECT_ERROR("@global\np : P = 2\nq := p^0.5\n", "integer powers");
    EXPECT_ERROR("@global\nq := [1 : true]\n", "coordinate in [a : b]");
    EXPECT_ERROR("@global\nz : C = [1 : 2]\n", "must be C");
    /* derivatives in the natural charts (z near points with |z| <= 1, 1/z near the others) */
    p = LOAD_OK(
        "@global\n"
        "define f(z : P) = (z^2 + 1)/(z^2 - 1)\n"
        "define q(z : P) = z^2 - 1\n"
        "d1 := derivative t -> f(t) at [0.5 : 1]\n"   /* f'(z) = -4z/(z^2 - 1)^2 = -2/0.5625; f(0.5) = -5/3: 1/f */
        "d2 := derivative t -> f(t) at ∞\n"           /* 0 */
        "d3 := derivative t -> 2 t at ∞\n"            /* s -> s/2 */
        "d4 := derivative t -> t^2 + 0.25 at [0.5 : 1]\n" /* the parabolic fixed point: 1 */
        "r1 := root t -> q(t) with seed [3 : 1]\n"
        "c1 := critical t -> f(t) with seed [0.3 : 1]\n"
        "c2 := critical t -> f(t) with seed [1 : 0.2]\n"
        "c3 := critical q with seed [1 : 0.1]\n");
    /* d(1/f)/dz = -f'/f^2 at 0.5: f' = -32/9, f = -5/3, so 32/25 */
    CHECK_NEAR(gnum(p, "d1", 0), 32.0 / 25, 1e-4);
    CHECK_NEAR(gnum(p, "d2", 0), 0, 1e-6);
    CHECK_NEAR(gnum(p, "d3", 0), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "d4", 0), 1, 1e-6);
    CHECK_POINT(p, "r1", 1, 0);
    CHECK_POINT(p, "c1", 0, 0);
    CHECK_POINT(p, "c2", INFINITY, 0);
    CHECK_POINT(p, "c3", INFINITY, 0);
    program_free(p);
    /* the multiplier of a 2-cycle, a product of derivatives: z^2 - 1 has the cycle 0, -1 (superattracting) and
       z^2 - 1.5 the cycle (-1 +- sqrt(3) i... ) with multiplier 4(1 + c) = -2 */
    p = LOAD_OK(
        "@global\n"
        "define f(z : P) = z^2 - 1.5\n"
        "x1 := root t -> f(f(t)) - t with seed [0.4 : 1]\n" /* (-1 + sqrt 3)/2 */
        "mu := (derivative f at x1) * (derivative f at f(x1))\n");
    CHECK_POINT(p, "x1", (sqrt(3.0) - 1) / 2, 0);
    CHECK_NEAR(gnum(p, "mu", 0), -2, 1e-3);
    program_free(p);
    /* the same with the point written as a complex number: still natural charts on both sides (2/z swaps 2 and 1:
       multiplier 1); a map to the sphere makes `critical` give a point */
    p = LOAD_OK(
        "@global\n"
        "define f(z : P) = 2 / z\n"
        "xc := 2\n"
        "mc := (derivative f at xc) * (derivative f at f(xc))\n"
        "define g(z : P) = (z^2 + 0.3)/(z^2 - 1)\n"
        "cg := critical t -> g(t) with seed 0.1\n");
    CHECK_NEAR(gnum(p, "mc", 0), 1, 1e-5);
    CHECK_NEAR(gnum(p, "mc", 1), 0, 1e-5);
    CHECK_POINT(p, "cg", 0, 0);
    program_free(p);
    /* pieces of a formula with calls: computed in order (a global read before the call that changes it), the same
       variable once (so its powers still cancel at 0 and ∞), any number of them in a derivative */
    p = LOAD_OK(
        "@global\n"
        "g : P = 2\n"
        "define bump(z : P) = r : P where\n"
        "    g = 10\n"
        "    r = z\n"
        "end\n"
        "a := g * bump(1)\n"
        "define id(z : P) = z\n"
        "define t1(w : P) = w * w / w + id(0)\n"
        "define t2(w : P) = (w^2 + 1)/(w^2 - 1) * id(1)\n"
        "q1 := t1(∞)\n"
        "q2 := t2(∞)\n"
        "define sq(w : P) = w^2\n"
        "d := derivative t -> sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) +"
        " sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) +"
        " sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) + sq(t) at [0.1 : 1]\n");
    CHECK_POINT(p, "a", 2, 0);
    CHECK_POINT(p, "q1", INFINITY, 0);
    CHECK_POINT(p, "q2", 1, 0);
    CHECK_NEAR(gnum(p, "d", 0), 34 * 0.2, 1e-4);
    program_free(p);
    /* Newton's method moves away from a pole (its steps grow): not a root */
    p = LOAD_OK(
        "@global\n"
        "define fc(z : C) = (z^2 + 0.25)/(z^2 - 1)\n"
        "q := root fc with seed 0.999\n"
        "sq := .stuck\n"
        "define f(z : P) = (z^2 + 0.25)/(z^2 - 1)\n"
        "r := root f with seed [0.999 : 1]\n"
        "sr := .stuck\n"
        "r2 := root f with seed [0.6i : 1]\n"   /* (a root: 0.5i) */
        "sr2 := .stuck\n");
    CHECK_NEAR(gnum(p, "sq", 0), 1, 0);
    CHECK_NEAR(gnum(p, "sr", 0), 1, 0);
    CHECK_NEAR(gnum(p, "sr2", 0), 0, 0);
    CHECK_POINT(p, "r2", 0, 0.5);
    program_free(p);
}

TEST(autodiff_through_loops) {
    Program *p = LOAD_OK(
        "@global\n"
        "define iter(z, c) = w where\n"
        "    w = z\n"
        "    for k in 1..3 do\n"
        "        w = w^2 + c\n"
        "    end\n"
        "end\n"
        "d3 := derivative t -> iter(t, 0.1) at 0.5\n"
        "define f(z) = s where\n"
        "    s = 0\n"
        "    for k in 1..3 do\n"
        "        s += (z - k)^2\n"
        "    end\n"
        "end\n"
        "cf := critical t -> f(t) with seed 7 + i\n"
        "st := .stuck\n"
        "define esc(z, c) = w where\n"
        "    w = z\n"
        "    while w not escapes do\n"
        "        w = w^2 + c\n"
        "    end\n"
        "end\n"
        "z0 := 0.3 + 0.2i\n"
        "c0 := 0.4 + 0.1i\n"
        "de := derivative t -> esc(t, c0) at z0\n"
        "fd := 1\n"   /* the same derivative, propagated by hand */
        "x := z0\n"
        "while x not escapes do\n"
        "    fd = 2 x fd\n"
        "    x = x^2 + c0\n"
        "end\n"
        "define pq(z) = (u, v) where\n"
        "    u = z^2\n"
        "    v = u * z\n"
        "end\n"
        "define hh(z) = r where\n"
        "    (u, v) := pq(z)\n"
        "    r = u + v\n"
        "end\n"
        "dh := derivative t -> hh(t) at 2\n"
        "define cnt(z) = w where\n"
        "    w = z\n"
        "    n : Z = 0\n"
        "    while |w| < 10 do\n"
        "        w = w * 2\n"
        "        n += 1\n"
        "    end\n"
        "    w = w / n\n"
        "end\n"
        "dc := derivative t -> cnt(t) at 1\n"
        "rl := root t -> log(t) + t - 1 with seed 3\n");
    CHECK_NEAR(gnum(p, "d3", 0), 0.3115, 1e-5);
    CHECK_NEAR(gnum(p, "cf", 0), 2, 1e-5);
    CHECK_NEAR(gnum(p, "cf", 1), 0, 1e-5);
    CHECK_NEAR(gnum(p, "st", 0), 0, 0);
    CHECK_NEAR(gnum(p, "de", 0) / gnum(p, "fd", 0), 1, 1e-5);
    CHECK_NEAR(gnum(p, "de", 1) / gnum(p, "fd", 1), 1, 1e-5);
    CHECK_NEAR(gnum(p, "dh", 0), 2 * 2 + 3 * 4, 1e-4);
    CHECK_NEAR(gnum(p, "dc", 0), 16.0 / 4.0, 1e-5); /* 1 -> 16 in 4 doublings: w = 16 t / 4 */
    CHECK_NEAR(gnum(p, "rl", 0), 1, 1e-5);
    program_free(p);
    EXPECT_ERROR("@global\ndefine bad(z) = w where\n    w = z\n    w = w * |w|\nend\nx := root t -> bad(t) with seed 1\n",
                 "can't differentiate");
    EXPECT_ERROR("@global\ndefine tt(z) = w where\n    r : R = |z|\n    w = z * r\nend\nx := root t -> tt(t) with seed 1\n",
                 "can't differentiate");
}

TEST(continuing_checks) {
    Program *p = LOAD_OK(
        "@viewer v c\n"
        "define sq(t, c) = t^2 - c\n"
        "w := root t -> sq(t, c) with seed 1 continuing from c = 1\n"
        "color = gray(re(w))\n");
    CHECK(p->viewers[0]->flood != NULL);
    float rgb[3];
    viewer_rgb(p, 0, 0.25f, 0, rgb);
    CHECK_NEAR(rgb[0], 0.5, 1e-4);
    program_free(p);
    EXPECT_ERROR("@viewer v c\ncolor = .red\n@global\nw := root t -> t^2 - 1 with seed 1 continuing from 1\n", "only be used in a viewer");
    EXPECT_ERROR("@viewer v c\nw := root t -> t^2 - c with seed 1 continuing from 1\nu := root t -> t^2 - c with seed 1 continuing from 1\ncolor = .red\n",
                 "only one");
    EXPECT_ERROR("@viewer v c\nd := 2 c\nw := root t -> t^2 - d with seed 1 continuing from c = 1\ncolor = .red\n", "computed from c");
    EXPECT_ERROR("@viewer v c\nw := root t -> t^2 - c with seed 1 continuing from z = 1\ncolor = .red\n", "viewer's variable");
}

TEST(pixel_size_and_px) {
    Program *p = LOAD_OK(
        "@config\n"
        "plane.radius = 2\n"
        "@viewer plane z\n"
        "a : R = 8 px\n"
        "color = rgb(.pixel * 10, a / .pixel / 10, 0)\n"
        "@plane.tool \"T\"\n"
        "b : R = 3 px\n"
        "draw circle at z with radius 4 px and width 2 px\n"
        "draw circle at z with radius b\n");
    Section *v = p->viewers[0];
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    I.pixel = viewer_pixel_size(p, state, v, 200, 100, 10, 10); /* 2 / 50 */
    CHECK_NEAR(I.pixel, 0.04, 1e-7);
    float rgb[3];
    CHECK(interp_eval_viewer(&I, v, 0, 0, rgb));
    CHECK_NEAR(rgb[0], 0.4, 1e-6);
    CHECK_NEAR(rgb[1], 0.8, 1e-6);
    DrawItemVec draws = {0};
    I.draws = &draws;
    CHECK(interp_run_tool(&I, p->tools[0], 1, 0));
    CHECK_INT(draws.len, 2);
    if (draws.len == 2) {
        CHECK(draws.data[0].radius_px); /* radius 4 px stays a screen size */
        CHECK_NEAR(draws.data[0].radius, 4, 1e-6);
        CHECK_NEAR(draws.data[0].width, 2, 1e-6);
        CHECK(!draws.data[1].radius_px); /* 3 px computed in plane units */
        CHECK_NEAR(draws.data[1].radius, 0.12, 1e-6);
    }
    vec_free(draws);
    interp_free(&I);
    free(state);
    program_free(p);
    EXPECT_ERROR("@viewer v z\n.pixel = 2\ncolor = .red\n", "can't be changed");
}

TEST(solve_fails_when_its_expression_gets_stuck) {
    Program *p = LOAD_OK(
        "@global\n"
        "define esc(z) = w where\n"
        "    w = z\n"
        "    while w not escapes do\n"
        "        w = w^2\n"
        "    end\n"
        "end\n"
        "r1 := root t -> esc(t) - 1e6 with seed 1.5\n"  /* escapes: a root (1.5^(2^k) = 1e6 for the k of the seed) */
        "s1 := .stuck\n"
        "r2 := root t -> esc(t) - 1e6 with seed 0.5\n"  /* the orbit of 0.5 never escapes */
        "s2 := .stuck\n");
    CHECK_NEAR(gnum(p, "s1", 0), 0, 0);
    CHECK_NEAR(gnum(p, "s2", 0), 1, 0);
    program_free(p);
}

TEST(variables_take_the_widest_type_assigned) {
    Program *p = LOAD_OK(
        "@global\n"
        "x := 1\n"
        "x = x / 2\n"            /* R */
        "n := 0\n"
        "for k in 1..4 do\n"
        "    n = n + k\n"        /* stays Z */
        "end\n"
        "z := 0\n"
        "z = z + 1i\n"           /* C */
        "v := [1, 2]\n"
        "v[2] = 0.5i\n"          /* C^2 */
        "(a, b) := (1, 2)\n"
        "(a, b) = (b, a + 0.5i)\n" /* b is C, so a is too */
        "define count(t) = m where\n"
        "    m = 0\n"
        "    while m < t do\n"
        "        m = m + 1\n"
        "    end\n"
        "end\n"
        "q := count(2.5)\n"
        "define sum3(u) = s where\n" /* an output without a type: what is assigned to it */
        "    s = u\n"
        "    s = s + u + u\n"
        "end\n"
        "r := sum3(0.5)\n"
        "@viewer plane c\n"
        "w := 0\n"
        "while w not escapes do\n"
        "    w = w^2 + c\n"
        "end\n"
        "color = .black\n");
    CHECK(find_global(p, "x") && find_global(p, "x")->type == t_real);
    CHECK_NEAR(gnum(p, "x", 0), 0.5, 1e-7);
    CHECK(find_global(p, "n") && find_global(p, "n")->type == t_int);
    CHECK_NEAR(gnum(p, "n", 0), 10, 0);
    CHECK(find_global(p, "z") && find_global(p, "z")->type == t_complex);
    CHECK(find_global(p, "v") && find_global(p, "v")->type == type_vec(t_complex, 2));
    CHECK_NEAR(gnum(p, "v", 3), 0.5, 1e-7);
    CHECK(find_global(p, "a") && find_global(p, "a")->type == t_complex);
    CHECK(find_global(p, "b") && find_global(p, "b")->type == t_complex);
    CHECK_NEAR(gnum(p, "a", 0), 2, 1e-7);
    CHECK_NEAR(gnum(p, "b", 1), 0.5, 1e-7);
    CHECK(find_global(p, "q") && find_global(p, "q")->type == t_int);
    CHECK_NEAR(gnum(p, "q", 0), 3, 0);
    CHECK(find_global(p, "r") && find_global(p, "r")->type == t_real);
    CHECK_NEAR(gnum(p, "r", 0), 1.5, 1e-7);
    Sym *w = NULL;
    for (int i = 0; i < p->viewers[0]->locals.len; i++)
        if (!strcmp(p->viewers[0]->locals.data[i]->name, "w")) w = p->viewers[0]->locals.data[i];
    CHECK(w && w->type == t_complex);
    program_free(p);
    /* declared again (in two branches, say): the wider type */
    p = LOAD_OK("@global\nc := 1\nif c > 0 then\n    t := 0\nelse\n    t := 0.5\nend\n"
                "if c > 0 then\n    u := 0.5\nelse\n    u := 2\nend\n");
    CHECK(find_global(p, "t") && find_global(p, "t")->type == t_real);
    CHECK(find_global(p, "u") && find_global(p, "u")->type == t_real);
    CHECK_NEAR(gnum(p, "u", 0), 0.5, 1e-7);
    program_free(p);
    /* a value that can't fit: the error is about the assignment */
    EXPECT_ERROR("@global\nk := 1\nk = true\n", "must be Z, but this is Bool");
    EXPECT_ERROR("@global\nn : Z = 1\nn = 0.5\n", "floor, ceil or round");
    /* the results of a function, unpacked, widen too; so does declaring them again */
    p = LOAD_OK("@global\ndefine rot(x, y, t) = (x cos(t) - y sin(t), x sin(t) + y cos(t))\n"
                "define h(x) = (x / 2, x * 3)\n"
                "x := 1\ny := 0\n(x, y) = rot(x, y, 0.5)\n"
                "a := 0\n(a, b) := h(1)\n");
    CHECK(find_global(p, "x") && find_global(p, "x")->type == t_real);
    CHECK_NEAR(gnum(p, "x", 0), cos(0.5), 1e-6);
    CHECK(find_global(p, "a") && find_global(p, "a")->type == t_real);
    CHECK_NEAR(gnum(p, "a", 0), 0.5, 1e-7);
    CHECK(find_global(p, "b") && find_global(p, "b")->type == t_int);
    program_free(p);
    /* a long chain of variables assigned to each other (one more run each) */
    StrBuf sb = {0};
    sb_puts(&sb, "@global\n");
    for (int k = 0; k < 12; k++) sb_printf(&sb, "v%d := 0\n", k);
    for (int k = 0; k < 11; k++) sb_printf(&sb, "v%d = v%d\n", k, k + 1);
    sb_puts(&sb, "v11 = 0.5\n");
    p = LOAD_OK(sb_cstr(&sb));
    CHECK(find_global(p, "v0") && find_global(p, "v0")->type == t_real);
    program_free(p);
    sb_clear(&sb); /* (far too long: an error that says what to do) */
    sb_puts(&sb, "@global\n");
    for (int k = 0; k < 40; k++) sb_printf(&sb, "v%d := 0\n", k);
    for (int k = 0; k < 39; k++) sb_printf(&sb, "v%d = v%d\n", k, k + 1);
    sb_puts(&sb, "v39 = 0.5\n");
    EXPECT_ERROR(sb_cstr(&sb), "could not be worked out in time: declare it with its type");
    sb_free(&sb);
    /* an output's first value fixes its type: a later value that doesn't fit is the error */
    p = load_src("@global\ndefine f(x) = r where\n    r = x\n    r = [1, 2]\nend\na := f(1)\n");
    CHECK_INT(p->nerrors, 1);
    for (int i = 0; i < p->diags.len; i++)
        if (p->diags.data[i].severity == DIAG_ERROR) CHECK_INT(p->diags.data[i].line, 4);
    program_free(p);
    /* a slider range written with real numbers makes a real variable */
    p = LOAD_OK("@config\nspeed := 1 in 0.1..10\nn := 2 in 1..5\n");
    CHECK(find_global(p, "speed") && find_global(p, "speed")->type == t_real && find_global(p, "speed")->has_range);
    CHECK(find_global(p, "n") && find_global(p, "n")->type == t_int);
    program_free(p);
    EXPECT_ERROR("@config\nn : Z = 3 in 0.5..10\n", "its slider range must be integers");
}

/* A generic function nothing calls is checked, but changes no types (here: it can't take C or R, Z works) */
TEST(unused_generic_function_changes_nothing) {
    Program *p = LOAD_OK("@config\ndepth := 50 in 10..500\n@global\ndefine setDepth(d) where\n    depth = d\nend\n"
                         "@viewer plane z\nw := 0\nfor k in 1..depth do\n    w = w^2 + z\nend\ncolor = gray(|w| / 4)\n");
    CHECK(find_global(p, "depth") && find_global(p, "depth")->type == t_int);
    program_free(p);
}

TEST(assigning_needs_a_declaration) {
    EXPECT_ERROR("@viewer v z\nw := z\nwx = w^2\ncolor = .red\n", "'wx' isn't declared: did you mean 'w'?");
    EXPECT_ERROR("@viewer v z\nn = 1\ncolor = .red\n", "declare it with n := ... (or with its type: n : Z = ...)");
    EXPECT_ERROR("@viewer v z\n(a, b) = (1, 2)\ncolor = .red\n", "'a' isn't declared");
    EXPECT_ERROR("@global\ncount := 0\n@tool \"T\"\ncuont = 1\n", "did you mean 'count'?");
    EXPECT_ERROR("@global\nCount := 0\n@tool \"T\"\ncount = 1\n", "did you mean 'Count'?");
    /* one error for the name, not more for its uses */
    Program *p = load_src("@viewer v z\nq = 1\nq = q + 1\ncolor = gray(q)\n");
    CHECK_INT(p->nerrors, 1);
    program_free(p);
    p = load_src("@global\nundefinedvec[1] = 3\n");
    CHECK_INT(p->nerrors, 1);
    program_free(p);
    /* constants can't change (and pi is not a typo of p) */
    EXPECT_ERROR("@global\np := 1\npi = 3.14\n", "'pi' is a constant and can't be changed");
    /* the errors of a function with types are its own, even when an instance of a generic one checks it first */
    p = load_src("@global\ndefine helper(x : C) = x < 1\ndefine f(x) = helper(x)\na := f(2)\n");
    CHECK(p->nerrors >= 1);
    for (int i = 0; i < p->diags.len; i++) CHECK(!strstr(p->diags.data[i].msg, "(in f("));
    program_free(p);
}

TEST(generic_functions) {
    Program *p = LOAD_OK(
        "@global\n"
        "define sq(x) = x^2\n"
        "define twice(x) = y where\n"
        "    y = x + x\n"
        "end\n"
        "define acc(z, n : Z) = w where\n"
        "    w = 0\n"
        "    for k in 1..n do\n"
        "        w = w + z\n"
        "    end\n"
        "end\n"
        "define g(x : R) = 1\n"
        "define g(x) = 2\n"
        "define unused(t) = t < 1\n"   /* (checked anyway: it works for real numbers) */
        "define rainbow(t) = [t, t, t]\n"
        "a := sq(3)\n"
        "b := sq(1.5)\n"
        "c := sq(1 + 2i)\n"
        "d := twice(2)\n"
        "e := twice([1, 2])\n"
        "f := acc(2, 3)\n"
        "h := acc(0.5i, 4)\n"
        "k := g(1.5)\n"
        "m := g(true)\n"
        "col := rainbow(0.25)\n"
        "@viewer plane z\n"
        "color = gray(|sq(z)|)\n");
    CHECK(find_global(p, "a") && find_global(p, "a")->type == t_int);
    CHECK_NEAR(gnum(p, "a", 0), 9, 0);
    CHECK(find_global(p, "b") && find_global(p, "b")->type == t_real);
    CHECK_NEAR(gnum(p, "b", 0), 2.25, 1e-6);
    CHECK(find_global(p, "c") && find_global(p, "c")->type == t_complex);
    CHECK_NEAR(gnum(p, "c", 0), -3, 1e-6);
    CHECK_NEAR(gnum(p, "c", 1), 4, 1e-6);
    CHECK(find_global(p, "d") && find_global(p, "d")->type == t_int);
    CHECK(find_global(p, "e") && find_global(p, "e")->type->kind == T_VEC);
    CHECK_NEAR(gnum(p, "e", 1), 4, 1e-6);
    CHECK(find_global(p, "f") && find_global(p, "f")->type == t_int);
    CHECK_NEAR(gnum(p, "f", 0), 6, 0);
    CHECK(find_global(p, "h") && find_global(p, "h")->type == t_complex);
    CHECK_NEAR(gnum(p, "h", 1), 2, 1e-6);
    CHECK_NEAR(gnum(p, "k", 0), 1, 0); /* the version with types wins */
    CHECK_NEAR(gnum(p, "m", 0), 2, 0);
    CHECK_NEAR(gnum(p, "col", 1), 0.25, 1e-6); /* the script's rainbow */
    program_free(p);
    EXPECT_ERROR("@global\ndefine f(x) = x < 1\nv := f(1i)\n", "(in f(x : C), called on line 3)");
    EXPECT_ERROR("@global\ndefine bad(t) = foo + t\n", "since nothing calls it yet");
    EXPECT_ERROR("@global\ndefine f(x) = 1\ndefine f(y) = 2\n", "already defined");
    EXPECT_ERROR("@global\ndefine re(x) = x\n", "built-in");
    EXPECT_ERROR("@global\ndefine f(z) = f(z + 1)\nx := f(1)\n", "recursive");
    /* the script's own version of a built-in function (written in the language) replaces it */
    p = LOAD_OK("@global\ndefine sin(x) = 2 x\ndefine step(z, c) = z^2 + c\na := sin(1.5)\nb := step(1i, 1)\n");
    CHECK_NEAR(gnum(p, "a", 0), 3, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 0, 1e-6);
    program_free(p);
    /* the same error in two instances is reported once */
    p = load_src("@global\ndefine f(x) = foo(x)\na := f(1)\nb := f(1i)\n");
    int n = 0;
    for (int i = 0; i < p->diags.len; i++)
        if (strstr(p->diags.data[i].msg, "unknown name 'foo'")) n++;
    CHECK_INT(n, 1);
    program_free(p);
    /* derivatives through a generic function */
    p = LOAD_OK("@global\ndefine f(z) = z^3 - 1\nd := derivative f at 2\nr := root f with seed 1 + 0.5i\n");
    CHECK_NEAR(gnum(p, "d", 0), 12, 1e-5);
    CHECK_NEAR(gnum(p, "r", 0), 1, 1e-5);
    program_free(p);
}

TEST(declarations_take_the_type_of_their_value) {
    Program *p = LOAD_OK(
        "@global\n"
        "define two(z) = (n : Z, ok : Bool) where\n"
        "    n = 2\n"
        "    ok = true\n"
        "end\n"
        "(n, ok) := two(1)\n"
        "(x, y) := (0, n)\n"
        "r := |3 + 4i|\n"
        "q := fract(2.5)\n");
    CHECK(find_global(p, "n") && find_global(p, "n")->type == t_int);
    CHECK(find_global(p, "ok") && find_global(p, "ok")->type == t_bool);
    CHECK(find_global(p, "x") && find_global(p, "x")->type == t_int);
    CHECK(find_global(p, "y") && find_global(p, "y")->type == t_int);
    CHECK(find_global(p, "r") && find_global(p, "r")->type == t_real);
    CHECK(find_global(p, "q") && find_global(p, "q")->type == t_real);
    CHECK_NEAR(gnum(p, "r", 0), 5, 1e-6);
    CHECK_NEAR(gnum(p, "y", 0), 2, 0);
    program_free(p);
}

TEST(for_loops_run_their_range) {
    Program *p = LOAD_OK(
        "@config\n"
        ".maxIterations = 10\n"
        "@global\n"
        "f : Z = 0\n"
        "for k in 1..100 do\n"
        "    f += 1\n"
        "end\n"
        "fs := .stuck\n"
        "fi := .iterations\n"
        "n : Z = 0\n"
        "while n < 100 do\n"
        "    n += 1\n"
        "end\n"
        "ws := .stuck\n");
    CHECK_NEAR(gnum(p, "f", 0), 100, 0);
    CHECK_NEAR(gnum(p, "fs", 0), 0, 0);
    CHECK_NEAR(gnum(p, "fi", 0), 100, 0);
    CHECK_NEAR(gnum(p, "n", 0), 10, 0);
    CHECK_NEAR(gnum(p, "ws", 0), 1, 0);
    program_free(p);
}

TEST(config_groups_notes_labels_choices) {
    Program *p = LOAD_OK(
        "@config \"Iteration\"\n"
        "\"A note about the settings.\"\n"
        ".maxIterations = 300 in 10..5000 \"Iterations\"\n"
        "@config \"Colors\"\n"
        "scheme : Z = 2 in [\"Rainbow\", \"Grays\", \"Fire\"] \"Palette\"\n"
        "speed : R = 1 in 0.1..10\n"
        "plain := 3\n"
        "@viewer v z\n"
        "color = gray(scheme / 3)\n");
    CHECK_INT(p->config.len, 7);
    if (p->config.len == 7) {
        CHECK(p->config.data[0].kind == CFG_GROUP && !strcmp(p->config.data[0].text, "Iteration"));
        CHECK(p->config.data[1].kind == CFG_NOTE && !strcmp(p->config.data[1].text, "A note about the settings."));
        CHECK(p->config.data[2].kind == CFG_VAR && p->config.data[2].sym == p->s_max_iter);
        CHECK(p->config.data[3].kind == CFG_GROUP && !strcmp(p->config.data[3].text, "Colors"));
        CHECK(p->config.data[4].kind == CFG_VAR && !strcmp(p->config.data[4].sym->name, "scheme"));
        CHECK(p->config.data[5].kind == CFG_VAR && !strcmp(p->config.data[5].sym->name, "speed"));
        CHECK(p->config.data[6].kind == CFG_VAR && !strcmp(p->config.data[6].sym->name, "plain"));
    }
    CHECK(p->s_max_iter->has_range && p->s_max_iter->range_lo == 10 && p->s_max_iter->range_hi == 5000);
    CHECK_STR(p->s_max_iter->label, "Iterations");
    Sym *sc = find_global(p, "scheme");
    CHECK(sc && sc->nchoices == 3 && !strcmp(sc->choices[2], "Fire") && sc->has_range && sc->range_hi == 3);
    CHECK(sc && sc->label && !strcmp(sc->label, "Palette"));
    CHECK_NEAR(gnum(p, "scheme", 0), 2, 0);
    CHECK_NEAR(gnum(p, "maxIterations", 0), 300, 0);
    program_free(p);
    p = LOAD_OK("@config\ndefine f(z, c) = z^2 +  c\n@viewer v z\ncolor = gray(re(f(z, 1)))\n");
    bool found = false;
    for (int i = 0; i < p->config.len; i++)
        if (p->config.data[i].kind == CFG_FORMULA) {
            Func *f = p->config.data[i].fn;
            found = f && !strcmp(f->name, "f") && f->src_end - f->src_start == 8 &&
                    !strncmp(p->source + f->src_start, "z^2 +  c", 8);
        }
    CHECK(found);
    program_free(p);
    EXPECT_ERROR("@config\nx : R = 1 in [\"a\", \"b\"]\n", "integer variable");
    EXPECT_ERROR("@global\nx : R = 1 in 0..2\n", "belong in @config");
    EXPECT_ERROR("@viewer v z\n\"hello\"\ncolor = .red\n", "only means something in @config");
}

static void expect_warning_(const char *src, const char *needle, int line) {
    Program *p = load_src(src);
    bool found = false;
    for (int i = 0; i < p->diags.len; i++)
        if (p->diags.data[i].severity == DIAG_WARNING && strstr(p->diags.data[i].msg, needle)) found = true;
    if (!found) {
        test_failures_++;
        printf("    expected a warning containing \"%s\" (test line %d), got:\n", needle, line);
        print_diags(p);
    }
    program_free(p);
}
#define EXPECT_WARNING(src, needle) expect_warning_(src, needle, __LINE__)

TEST(friendly_messages) {
    EXPECT_ERROR("@tool \"T\"\ndraw line between 0 and 1 with color .black width 2 px\n", "write 'and' or 'with' before 'width'");
    EXPECT_ERROR("@global\nx := 2 3\n", "explicit *");
    EXPECT_WARNING("@viewer v z\ncolor = .red\n@v.tool \"A\" \"R\"\ndraw point at z\n@v.tool \"B\" \"r\"\ndraw point at z\n",
                   "already uses the key");
    EXPECT_WARNING("@viewer v z\ncolor = .red\n@v.tool \"A\" \"Shift + P\"\ndraw point at z\n@v.tool \"B\" \"P\"\ndraw point at z\n",
                   "key of Pick point");
}

TEST(if_expressions) {
    Program *p = LOAD_OK(
        "@global\n"
        "define f(z) = if re(z) > 0 then z^2 else z^3\n"
        "a := if 1 > 2 then 5 else 7\n"
        "b : R = if true then 1 else 2.5\n"
        "c := if false then .red else if true then .green else .blue\n"
        "d1 := derivative t -> f(t) at 2\n"
        "d2 := derivative t -> (if re(t) > 0 then t^2 else t^3) at -2\n"
        "n : Z = 0\n"
        "define bump() = r where\n"
        "    n += 1\n"
        "    r = 1\n"
        "end\n"
        "e := if true then 1 else bump()\n");   /* only the chosen value is computed */
    CHECK_NEAR(gnum(p, "a", 0), 7, 0);
    CHECK_NEAR(gnum(p, "b", 0), 1, 0);
    CHECK_NEAR(gnum(p, "c", 1), 1, 0);
    CHECK_NEAR(gnum(p, "d1", 0), 4, 1e-5);
    CHECK_NEAR(gnum(p, "d2", 0), 12, 1e-4);
    CHECK_NEAR(gnum(p, "n", 0), 0, 0);
    program_free(p);
    EXPECT_ERROR("@global\nx := if true then 1 end\n", "needs an 'else'");
    EXPECT_ERROR("@global\nx := if true then 1 else .red\n", "don't mix");
    EXPECT_ERROR("@global\nx := if 1 then 1 else 2\n", "condition");
}

TEST(colour_helpers_and_palettes) {
    Program *p = LOAD_OK(
        "@global\n"
        "a := rainbow(0.3)\n"
        "b := palette(\"fire\", 0)\n"
        "c := cycle(0.25, .red, .blue)\n"
        "d := dark(.white)\n"
        "e := light(.black)\n"
        "f := invert(.violet)\n"
        "g := blend(0.25, .red, .blue)\n"
        "h := palette(\"rainbow\", 1.3)\n");
    CHECK_NEAR(gnum(p, "a", 0), 0.6, 1e-5);
    CHECK_NEAR(gnum(p, "a", 1), 1 - 0.4 * (1 - 180.0 / 255), 1e-4);
    CHECK_NEAR(gnum(p, "a", 2), 0, 1e-6);
    CHECK_NEAR(gnum(p, "b", 1), 0x3d / 255.0, 1e-3);
    CHECK_NEAR(gnum(p, "c", 0), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "c", 2), 0.5, 1e-6);
    CHECK_NEAR(gnum(p, "d", 1), 2.0 / 3, 1e-5);
    CHECK_NEAR(gnum(p, "e", 2), 1.0 / 3, 1e-5);
    CHECK_NEAR(gnum(p, "f", 2), 1 - 160.0 / 255, 1e-5);
    CHECK_NEAR(gnum(p, "g", 0), 0.25, 1e-6);
    CHECK_NEAR(gnum(p, "g", 2), 0.75, 1e-6);
    CHECK_NEAR(gnum(p, "h", 1), gnum(p, "a", 1), 1e-5);
    program_free(p);
    /* a script may define its own rainbow; the built-in palettes keep using the built-in one */
    p = LOAD_OK(
        "@global\n"
        "define rainbow(t : R) = gray(t)\n"
        "a := rainbow(0.3)\n"
        "b := palette(\"rainbow\", 0.3)\n");
    CHECK_NEAR(gnum(p, "a", 1), 0.3, 1e-6);
    CHECK_NEAR(gnum(p, "b", 0), 0.6, 1e-5);
    program_free(p);
    EXPECT_ERROR("@global\nx := palette(\"nope\", 0)\n", "no palette");
    p = LOAD_OK("@global\ndefine hsv(h : R, s : R, v : R) = gray(h)\nc := hsv(0.3, 1, 1)\n"); /* (any built-in) */
    CHECK_NEAR(gnum(p, "c", 2), 0.3, 1e-6);
    program_free(p);
}

TEST(tool_events) {
    Program *p = LOAD_OK(
        "@global\n"
        "n : Z = 0\n"
        "@viewer v z\n"
        "color = .red\n"
        "@v.tool \"T\" \"T\" \"brush\"\n"
        ".keep = true\n"
        "k : Z = 10\n"                    /* runs before every event */
        "on press do\n"
        "    n += k\n"
        "    draw point at z\n"
        "end\n"
        "on drag do\n"
        "    n += 1\n"
        "    draw line between .dragStart and z\n"
        "end\n"
        "on hide do\n"
        "    erase\n"
        "end\n"
        "@tool \"Clear\" button\n"
        "erase all\n");
    Section *t = p->tools[0];
    CHECK(t->has_events && t->keep && t->on[TOOL_EV_PRESS] && t->on[TOOL_EV_DRAG] && t->on[TOOL_EV_HIDE]);
    CHECK(!t->on[TOOL_EV_CLICK] && !t->on[TOOL_EV_SHOW]);
    CHECK(p->tools[1]->button && !p->tools[1]->has_events);
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    DrawItemVec draws = {0};
    I.draws = &draws;
    CHECK(interp_run_tool_event(&I, t, TOOL_EV_PRESS, 1, 2));
    CHECK(interp_run_tool_event(&I, t, TOOL_EV_DRAG, 2, 2));
    CHECK(interp_run_tool_event(&I, t, TOOL_EV_CLICK, 2, 2)); /* no handler: only the shared part runs */
    Sym *n = find_global(p, "n");
    CHECK(n && (i32)state[n->slot] == 11);
    CHECK_INT(draws.len, 2);
    I.erased = I.erased_all = false;
    CHECK(interp_run_tool_event(&I, t, TOOL_EV_HIDE, 0, 0));
    CHECK(I.erased && !I.erased_all && draws.len == 0);
    CHECK(interp_run_tool(&I, p->tools[1], 0, 0));
    CHECK(I.erased_all);
    CHECK(tool_runs_on(t, TOOL_EV_PRESS) && tool_runs_on(t, TOOL_EV_DRAG) && !tool_runs_on(t, TOOL_EV_CLICK));
    vec_free(draws);
    interp_free(&I);
    free(state);
    program_free(p);
    /* which events run a tool: mouse blocks decide; show/hide blocks alone keep the mouse defaults */
    Program *q = LOAD_OK(
        "@viewer v z\ncolor = .red\n"
        "@v.tool \"Mark\"\ndraw point at z\non hide do\n    erase\nend\n"
        "@v.tool \"Pan-ish\"\n.panning = true\n.hover = true\ndraw point at z\non show do\nend\n"
        "@v.tool \"Plain\"\ndraw point at z\n"
        "@v.tool \"Shows\"\n.panning = true\non show do\n    draw point at 0\nend\n");
    CHECK(tool_runs_on(q->tools[0], TOOL_EV_PRESS) && tool_runs_on(q->tools[0], TOOL_EV_DRAG));
    CHECK(tool_runs_on(q->tools[0], TOOL_EV_HIDE) && !tool_runs_on(q->tools[0], TOOL_EV_SHOW));
    CHECK(!tool_runs_on(q->tools[0], TOOL_EV_CLICK) && !tool_runs_on(q->tools[0], TOOL_EV_RELEASE));
    CHECK(tool_runs_on(q->tools[1], TOOL_EV_CLICK) && tool_runs_on(q->tools[1], TOOL_EV_HOVER));
    CHECK(!tool_runs_on(q->tools[1], TOOL_EV_PRESS) && tool_runs_on(q->tools[1], TOOL_EV_SHOW));
    CHECK(tool_runs_on(q->tools[2], TOOL_EV_PRESS) && !tool_runs_on(q->tools[2], TOOL_EV_HIDE));
    CHECK(!tool_runs_on(q->tools[3], TOOL_EV_CLICK) && !tool_runs_on(q->tools[3], TOOL_EV_PRESS) && tool_runs_on(q->tools[3], TOOL_EV_SHOW));
    program_free(q);
    /* erase "Name": a button clears another tool's drawings */
    q = LOAD_OK(
        "@viewer v z\ncolor = .red\n"
        "@v.tool \"Mark\"\n.keep = true\ndraw point at z\n"
        "@w.tool \"Mark\"\ndraw point at z\n"
        "@viewer w z\ncolor = .blue\n"
        "@tool \"Clear\" button\nerase \"Mark\"\n");
    {
        u32 *st2 = (u32 *)xmalloc(sizeof(u32) * (size_t)q->state_words);
        memcpy(st2, q->initial_state, sizeof(u32) * (size_t)q->state_words);
        Interp J;
        interp_init(&J, q, st2);
        DrawItemVec d2 = {0};
        J.draws = &d2;
        J.erase_tools = 0;
        CHECK(interp_run_tool(&J, q->tools[2], 0, 0));
        CHECK(J.erase_tools == 3 && !J.erased && !J.erased_all); /* both tools named "Mark" */
        vec_free(d2);
        interp_free(&J);
        free(st2);
    }
    program_free(q);
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\" button\nerase \"Nope\"\n", "there is no tool named \"Nope\"");
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\"\non press do\nend\non press do\nend\n", "already has an 'on' block");
    EXPECT_ERROR("@viewer v z\non press do\nend\ncolor = .red\n", "belongs in a tool section");
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\"\non squeeze do\nend\n", "not an event");
    EXPECT_ERROR("@viewer v z\nerase\ncolor = .red\n", "only be used in tools");
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@v.tool \"T\"\non press do\n    q := 1\nend\non drag do\n    draw point at q\nend\n",
                 "unknown name 'q'");
}

/* define f(x) where ... end: a function without a result, run for its effects */
TEST(functions_without_results) {
    Program *p = LOAD_OK(
        "@global\n"
        "count : Z = 0\n"
        "define mark(z, r : R) where\n"
        "    draw circle at z with radius r px\n"
        "    count += 1\n"
        "    if r > 3 then\n"
        "        return\n"
        "    end\n"
        "    draw point at z\n"
        "end\n"
        "@viewer v z\ncolor = .white\n"
        "@v.tool \"M\"\nmark(z, 2)\nmark(z + 1, 5)\n");
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    DrawItemVec draws = {0};
    I.draws = &draws;
    CHECK(interp_run_tool(&I, p->tools[0], 0.5f, 0));
    CHECK_INT(draws.len, 3); /* circle, point; circle (then return) */
    Sym *count = find_global(p, "count");
    CHECK(count && (i32)state[count->slot] == 2);
    vec_free(draws);
    interp_free(&I);
    free(state);
    program_free(p);
    EXPECT_ERROR("@global\ndefine f(z) where\n    w := z\nend\nx := f(1)\n@viewer v z\ncolor = .red\n", "doesn't produce a value");
    /* in a viewer (nothing to do there, but it compiles) */
    Program *q = LOAD_OK("@global\ndefine nothing(z) where\n    w := z^2\nend\n@viewer v z\nnothing(z)\ncolor = .white\n");
    u32 n;
    char err[256];
    u32 *words = codegen_viewer(q, q->viewers[0], &n, err, sizeof err);
    CHECK_MSG(words != NULL, "%s", err);
    free(words);
    program_free(q);
}

TEST(draw_text) {
    Program *p = LOAD_OK(
        "@viewer v z\n"
        "color = .red\n"
        "@v.tool \"T\"\n"
        "n : Z = 3\n"
        "draw text \"n = \" + n + \", z = \" + z + \" \" + (n > 2) + \" \" + [1, 2.5] at z\n");
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, state);
    DrawItemVec draws = {0};
    I.draws = &draws;
    CHECK(interp_run_tool(&I, p->tools[0], 1, -2));
    CHECK_INT(draws.len, 1);
    if (draws.len == 1) {
        CHECK(draws.data[0].shape == DRAW_TEXT);
        CHECK_STR(draws.data[0].text, "n = 3, z = 1 - 2i true [1, 2.5]");
    }
    vec_free(draws);
    interp_free(&I);
    free(state);
    program_free(p);
    EXPECT_ERROR("@viewer v z\ncolor = .red\n@tool \"T\"\ndraw text \"a\" + .red + L at 0\n", "unknown name 'L'");
}

TEST(list_capacity_and_negative_indices) {
    Program *p = LOAD_OK(
        "@global\n"
        "L : List of C up to 200 = []\n"
        "for k in 1..150 do\n"
        "    append(L, k)\n"
        "end\n"
        "n : Z = |L|\n"
        "last := L[-1]\n"
        "second := L[-2]\n"
        "v : R^3 = [1, 2, 3]\n"
        "w : R = v[-1]\n"
        "S : List of Z = []\n"
        "for k in 1..100 do\n"
        "    append(S, k)\n"
        "end\n"
        "m : Z = |S|\n");
    CHECK_NEAR(gnum(p, "n", 0), 150, 0);
    CHECK_NEAR(gnum(p, "last", 0), 150, 0);
    CHECK_NEAR(gnum(p, "second", 0), 149, 0);
    CHECK_NEAR(gnum(p, "w", 0), 3, 0);
    CHECK_NEAR(gnum(p, "m", 0), 64, 0); /* the default capacity */
    program_free(p);
    EXPECT_ERROR("@global\nL : List of C up to 5000 = []\n", "1 to 4096");
}

TEST(prepare_sections) {
    Program *p = LOAD_OK(
        "@config\n"
        "a : R = 2 in 0..10\n"
        "@global\n"
        "b : R = 0\n"
        "runs : Z = 0\n"
        "@prepare\n"
        "t : R = a * a\n"          /* a local */
        "b = t + 1\n"
        "runs += 1\n"
        "@viewer v z\n"
        "color = gray(b / 100)\n");
    CHECK_NEAR(gnum(p, "b", 0), 5, 1e-6);
    CHECK_NEAR(gnum(p, "runs", 0), 1, 0);
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    Sym *a = find_global(p, "a"), *b = find_global(p, "b");
    state[a->slot] = fw(3);
    char err[128];
    CHECK(program_prepare(p, state, err, sizeof err));
    CHECK_NEAR(wf(state[b->slot]), 10, 1e-6);
    free(state);
    program_free(p);
    EXPECT_ERROR("@prepare\ndraw point at 0\n@viewer v z\ncolor = .red\n", "only be used in tools");
}

/* print: in @global / @config (Program.load_output), @prepare (program_prepare_ex), tools (Interp.out); a viewer's
   prints do nothing on the GPU */
TEST(print_statement) {
    Program *p = LOAD_OK(
        "@config\n"
        "a : R = 2 in 0..10\n"
        "@global\n"
        "L : List of C = [1, i, 2 - 0.5i]\n"
        "print \"a = \" + a + \", L = \" + L\n"
        "@prepare\n"
        "print \"prepare: \" + (a * a)\n"
        "@viewer v z\n"
        "print \"pixel \" + z\n"
        "color = .red\n"
        "@v.tool \"T\"\n"
        ".keep = true\n"
        "print \"clicked \" + z\n"
        "@tool \"E\"\n"
        "print \"before\"\n"
        "erase\n"
        "print \"after\"\n");
    CHECK_STR(p->load_output ? p->load_output : "", "a = 2, L = [1, i, 2 - 0.5i]\n"); /* (not @prepare's) */
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    StrBuf out = {0};
    char err[128];
    CHECK(program_prepare_ex(p, state, &out, err, sizeof err));
    CHECK_STR(sb_cstr(&out), "prepare: 4\n");
    sb_clear(&out);
    Interp I;
    interp_init(&I, p, state);
    I.out = &out;
    CHECK(interp_run_tool(&I, p->tools[0], 1, -1));
    CHECK_STR(sb_cstr(&out), "clicked 1 - i\n");
    sb_clear(&out);
    CHECK(interp_run_tool(&I, p->tools[1], 0, 0)); /* erase clears what the run printed so far */
    CHECK_STR(sb_cstr(&out), "after\n");
    sb_clear(&out);
    float rgb[3];
    CHECK(interp_eval_viewer(&I, p->viewers[0], 0.5f, 0, rgb));
    CHECK_STR(sb_cstr(&out), "pixel 0.5\n");
    u32 n;
    u32 *words = codegen_viewer(p, p->viewers[0], &n, err, sizeof err); /* (on the GPU it does nothing) */
    CHECK_MSG(words != NULL, "%s", err);
    free(words);
    sb_free(&out);
    interp_free(&I);
    free(state);
    program_free(p);
    /* `print` can still be a variable's name */
    p = LOAD_OK("@global\nprint := 2\nprint = print + 1\n@viewer v z\ncolor = .red\n");
    CHECK_NEAR(gnum(p, "print", 0), 3, 0);
    program_free(p);
    EXPECT_ERROR("@global\nprint\n@viewer v z\ncolor = .red\n", "what to print");
}

/* a statement goes on to the next line after an operator, a comma, "..." / "…", and inside ( ) and [ ] */
TEST(line_continuations) {
    Program *p = LOAD_OK(
        "@global\n"
        "a := 1 +\n"
        "     2\n"
        "b := 2 ...   # a comment\n"
        "     * 5\n"
        "c := 3 \xe2\x80\xa6\n"
        "     - 1\n"
        "d := max(1,\n"
        "         7)\n"
        "ok : Bool = a == 3 and\n"
        "            b == 10\n"
        "@viewer v z\n"
        "draw_me : Z = 0\n"
        "color = if ok then .red else ...\n"
        "        .blue\n");
    CHECK_NEAR(gnum(p, "a", 0), 3, 0);
    CHECK_NEAR(gnum(p, "b", 0), 10, 0);
    CHECK_NEAR(gnum(p, "c", 0), 2, 0);
    CHECK_NEAR(gnum(p, "d", 0), 7, 0);
    CHECK_NEAR(gnum(p, "ok", 0), 1, 0);
    program_free(p);
    /* "abc " + "def" is one text: a long note goes on after a + at the end of a line */
    p = LOAD_OK("@config\n"
                "\"A note that goes on \" +\n"
                "\"on the next line.\"\n"
                "x : R = 1 \"A label \" + ...\n"
                "    \"in two parts\"\n"
                "@viewer v z \"A view\" \"Ctrl + 1\"\n"
                "color = .red\n");
    bool note = false;
    for (int k = 0; k < p->config.len; k++)
        if (p->config.data[k].kind == CFG_NOTE && !strcmp(p->config.data[k].text, "A note that goes on on the next line."))
            note = true;
    CHECK(note);
    Sym *x = find_global(p, "x");
    CHECK(x && x->label && !strcmp(x->label, "A label in two parts"));
    CHECK_STR(p->viewers[0]->display, "A view"); /* (texts next to each other on a line stay apart) */
    program_free(p);
    /* the line numbers after a continuation are still right */
    p = load_src("@global\nx := 1 +\n  2\ny := nope\n@viewer v z\ncolor = .red\n");
    CHECK(p->diags.len > 0 && p->diags.data[0].line == 4);
    program_free(p);
}

static const TestCase tests_lang[] = {
    TESTCASE(line_continuations),
    TESTCASE(print_statement),
    TESTCASE(prepare_sections),
    TESTCASE(list_capacity_and_negative_indices),
    TESTCASE(functions_without_results),
    TESTCASE(draw_text),
    TESTCASE(tool_events),
    TESTCASE(colour_helpers_and_palettes),
    TESTCASE(if_expressions),
    TESTCASE(friendly_messages),
    TESTCASE(config_groups_notes_labels_choices),
    TESTCASE(for_loops_run_their_range),
    TESTCASE(pixel_size_and_px),
    TESTCASE(solve_fails_when_its_expression_gets_stuck),
    TESTCASE(declarations_take_the_type_of_their_value),
    TESTCASE(variables_take_the_widest_type_assigned),
    TESTCASE(unused_generic_function_changes_nothing),
    TESTCASE(assigning_needs_a_declaration),
    TESTCASE(generic_functions),
    TESTCASE(projective_points),
    TESTCASE(autodiff_through_loops),
    TESTCASE(continuing_checks),
    TESTCASE(projective_mapping),
    TESTCASE(automatic_differentiation),
    TESTCASE(variable_integer_powers),
    TESTCASE(default_viewer), TESTCASE(mandelbrot), TESTCASE(language_sketch), TESTCASE(types_and_conversions),
    TESTCASE(implicit_multiplication), TESTCASE(complex_functions), TESTCASE(loops_and_stuck),
    TESTCASE(epsilon_comparisons), TESTCASE(vectors_colors_lists), TESTCASE(functions_and_overloads),
    TESTCASE(sections_and_config), TESTCASE(errors), TESTCASE(runaway_tool_is_stopped),
};
