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

TEST(brief_example) {
    /* the illustration from AGENTS.md, made valid */
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
        "c := n\n"          /* untyped -> C */
        "q : H = 1 + 2i\n"
        "v : R^3 = [1, 2, 3]\n"
        "k : Z = 2^10\n"
        "h : R = 2^-1\n"
        "m : Z = -7 mod 3\n"
        "f : R = 7.5 mod 2\n"
        "b := 3 > 2 and not false\n");
    CHECK_NEAR(gnum(p, "x", 0), 3.5, 1e-6);
    CHECK(find_global(p, "c") && find_global(p, "c")->type == t_complex);
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
    EXPECT_ERROR("@global\nn := 3\nfor k in 1..n do\nend\n", "integers");
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
    EXPECT_ERROR("@global\nx := 5\n@viewer v z\ncolor = gray(x)\n", "must be R");
    EXPECT_ERROR("@global\ndefine sin(x : R) = x\n", "built-in");
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

static const TestCase tests_lang[] = {
    TESTCASE(automatic_differentiation),
    TESTCASE(variable_integer_powers),
    TESTCASE(default_viewer), TESTCASE(mandelbrot), TESTCASE(brief_example), TESTCASE(types_and_conversions),
    TESTCASE(implicit_multiplication), TESTCASE(complex_functions), TESTCASE(loops_and_stuck),
    TESTCASE(epsilon_comparisons), TESTCASE(vectors_colors_lists), TESTCASE(functions_and_overloads),
    TESTCASE(sections_and_config), TESTCASE(errors), TESTCASE(runaway_tool_is_stopped),
};
