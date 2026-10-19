#include <ctype.h>
/* test_codegen.c - SPIR-V generation: validation with spirv-val (when installed)
 * and GPU-vs-CPU agreement (when a Vulkan device is available, e.g. lavapipe). */

/* Feature scripts.  Outputs are smooth functions where possible so that
 * float rounding differences between CPU and GPU stay invisible. */
static const struct { const char *name; const char *src; double max_bad; } cg_scripts[] = {
    {"arith",
     "@viewer v z\n"
     "w := z * (2 - 3i) / (1 + z * conj(z)) - z^3 + 0.5 z^-2\n"
     "x : R = re(w) * 0.1 + im(z)\n"
     "n : Z = 7\n"
     "m : Z = (n * 3 - 4) mod 5 + abs(-2) + max(1, 3) + 2^5\n"
     "color = [0.5 + 0.4 sin(x), 0.5 + 0.4 cos(im(w)), m / 50]\n", 0.002},
    {"complex_functions",
     "@viewer v z\n"
     "w := exp(z) + sin(z) * cos(z) - sqrt(z) / (1 + z^2) + log(z + 3) + z^0.5 + (1 + i)^z + tanh(z) + atan(z / 3)\n"
     "color = [0.5 + 0.4 sin(re(w)), 0.5 + 0.4 sin(im(w)), 0.5 + 0.4 cos(|w|)]\n", 0.01},
    {"real_functions",
     "@viewer v z\n"
     "x : R = re(z)\n"
     "y : R = im(z)\n"
     "a : R = atan2(y, x) + sinh(x) - cosh(y) / 4 + tanh(x) + exp(-x * x) + log(2 + x) + sqrt(|y|) + fract(3.3) + sign(x)\n"
     "b : R = clamp(x, -1, 1) + mix(x, y, 0.25) + smoothstep(-1, 1, y) + min(x, y) + max(x, y) + x^2.5\n"
     "color = [0.5 + 0.4 sin(a), 0.5 + 0.4 cos(b), 0.5]\n", 0.01},
    {"loops",
     "@config\n.maxIterations = 37\n"
     "@viewer v z\n"
     "s : R = 0\n"
     "for k in 1..20 do\n"
     "    if k mod 3 == 0 then\n"
     "        continue\n"
     "    end\n"
     "    if k > 15 then\n"
     "        break\n"
     "    end\n"
     "    s += k * re(z)\n"
     "end\n"
     "it1 : Z = .iterations\n"
     "n : Z = 0\n"
     "while true do\n"
     "    n += 1\n"
     "end\n"
     "st := .stuck\n"
     "d : Z = 0\n"
     "do\n"
     "    d += 2\n"
     "while d < 9 end\n"
     "t : Z = 0\n"
     "for k in 10..1 by -2 do\n"
     "    for j in 1..k do\n"
     "        t += 1\n"
     "    end\n"
     "end\n"
     "v : R^3 = [1, 2, 3]\n"
     "acc : R = 0\n"
     "for e in v do\n"
     "    acc += e\n"
     "end\n"
     "f : Z = 0\n"
     "for k in 1..100 do\n"   /* for loops run their whole range, beyond .maxIterations */
     "    f += 1\n"
     "end\n"
     "fs := .stuck\n"
     "g : R = 0\n"
     "if st and n == 37 and it1 == 16 and d == 10 and t == 30 and acc == 6 and f == 100 and not fs then\n"
     "    g = 1\n"
     "end\n"
     "color = [0.5 + 0.4 sin(s), g, 0.5]\n", 0.002},
    {"functions",
     "@global\n"
     "define f(x, y) = (u, w) where\n"
     "    u = x + y\n"
     "    w = 0\n"
     "    for k in 1..10 do\n"
     "        u += i\n"
     "        if k == 3 then\n"
     "            return\n"
     "        end\n"
     "    end\n"
     "    w = 20\n"
     "end\n"
     "define g(x : R) = x * 2\n"
     "define g(z : C) = z * 3\n"
     "define h(n : Z) = r : R where\n"
     "    r = 0\n"
     "    while n > 0 do\n"
     "        r += n\n"
     "        n -= 1\n"
     "    end\n"
     "end\n"
     "@viewer v z\n"
     "(a, b) := f(z, 1)\n"
     "c := g(z) + g(1.5) + h(4)\n"
     "color = [0.5 + 0.4 sin(re(a) + im(b)), 0.5 + 0.4 cos(re(c)), 0.5 + 0.4 sin(im(a))]\n", 0.002},
    {"lists_vectors",
     "@global\n"
     "pts : List of C = [1, i, -1]\n"
     "@viewer v z\n"
     "L : List of C = []\n"
     "for k in 1..5 do\n"
     "    append(L, z * k)\n"
     "end\n"
     "s := 0\n"
     "for x in L do\n"
     "    s += x\n"
     "end\n"
     "s += L[2] + L[9] + pts[2]\n"
     "L[1] = 7\n"
     "n : Z = |L| + length(pts)\n"
     "vv : R^4 = [re(z), im(z), 1, 2]\n"
     "vv[3] = vv[1] * 2\n"
     "k : Z = 2\n"
     "dd : R = dot(vv, vv) + vv[k] + |vv|\n"
     "clear(L)\n"
     "e := L[1]\n"
     "color = [0.5 + 0.4 sin(re(s)), 0.5 + 0.4 cos(dd + n), 0.5 + 0.4 sin(re(e) + re(L[1]) + im(s))]\n", 0.002},
    {"projective",
     "@global\n"
     "a : C = -0.4 + 0.6i\n"
     "define f(z : P) = (z^2 + a)/(z^2 - 1)\n"
     "@viewer sphere z : P \"Sphere\" projective\n"
     "w := z\n"
     "for n in 1..12 do\n"
     "    w = f(w)\n"
     "end\n"
     "d := derivative t -> f(t) at w\n"
     "c := critical t -> f(t) with seed z\n"
     "e := [re(w[1]) : w[2] + 1] * 2 - 1/w\n"
     "color = [chordal(c, 0), |d| / (1 + |d|), chordal(w, ∞) + 0.2 chordal(e, 1)]\n", 0.002},
    {"quaternions_bools",
     "@global\n"
     "count : Z = 0\n"
     "define bump() = r : Bool where\n"
     "    count += 1\n"
     "    r = true\n"
     "end\n"
     "@viewer v z\n"
     "q : H = quat(re(z), im(z), 0.5, -0.25)\n"
     "p := q * q - q / (1 + q) + conj(q) * .j\n"
     "a : R = |p| + p[2] + p[4]\n"
     "ok := re(z) > 0 or bump()\n"
     "ok2 := re(z) > 0 and bump()\n"
     "b : R = count\n"
     "near := z == z + 1e-9 and not (z == z + 1)\n"
     "e := (z * 10) escapes\n"
     "va := (z * 1e-8) vanishes\n"
     "x : R = 0\n"
     "if near and va and ok then\n"
     "    x = 0.3\n"
     "end\n"
     "if e then\n"
     "    x += 0.4\n"
     "end\n"
     "color = [0.5 + 0.4 sin(a), b / 4 + x, 0.5]\n", 0.002},
    {"variable_powers",
     "@config\nd : Z = 3 in 2..8\n"
     "@viewer v z\n"
     "n : Z = d\n"
     "w := z^n + z^(-n) * 0.01 + (re(z) * 0.5 + 1)^n\n"
     "it : Z = 0\n"
     "for k in 1..3 do\n"
     "    it += 1\n"
     "end\n"
     "q := z^n\n"
     "color = [0.5 + 0.4 sin(re(w)), 0.5 + 0.4 cos(im(w)), .iterations / 4]\n", 0.01},
    {"autodiff",
     "@viewer v z\n"
     "w := critical t -> t^3 - 3 z t with seed 1 + 0.5i\n"
     "r := root t -> t^2 - z with seed 1\n"
     "d := derivative t -> exp(t) * sin(z t) at z\n"
     "color = [0.5 + 0.4 sin(re(w) + im(r)), 0.5 + 0.4 cos(re(d) * 0.1), 0.5]\n", 0.03},
    {"negative_indices",
     "@viewer v z\n"
     "L : List of R up to 100 = []\n"
     "for k in 1..70 do\n"
     "    append(L, k / 70)\n"
     "end\n"
     "u : R^3 = [0.2, 0.5, re(z)]\n"
     "color = [L[-1] * 0.5, u[-2], 0.5 + 0.4 sin(u[-1] + L[-3])]\n", 0.002},
    {"if_expressions",
     "@global\n"
     "define sq(z) = z^2\n"
     "@viewer v z\n"
     "w := if re(z) > 0 then sq(z) else z^3 + 1\n"
     "k : Z = if im(z) > 0 then 3 else 5\n"
     "col : Color = if |z| < 1 then [0.2, 0.4, 0.6] else if |z| < 1.5 then .orange else gray(0.3)\n"
     "d := derivative t -> (if re(t) > 0 then t^2 else sin(t)) at z\n"
     "color = mix(col, [0.5 + 0.4 sin(re(w)), k / 6, 0.5 + 0.4 cos(re(d))], 0.5)\n", 0.01},
    {"autodiff_loops",
     "@config\n.maxIterations = 40\n.escapeRadius = 100\n"
     "@global\n"
     "define esc(t, c) = x where\n"
     "    x = t\n"
     "    y := t\n"
     "    while x not escapes do\n"
     "        (x, y) = (x^2 + c - 0.3 y, x)\n"
     "    end\n"
     "end\n"
     "@viewer v z\n"
     "d := derivative t -> esc(t, z) at 0.5\n"
     "w := critical t -> log(esc(t, z)) with seed 0.5\n"
     "color = [0.5 + 0.4 sin(re(d) * 0.01), 0.5 + 0.4 cos(im(w)), 0.5 + 0.2 sin(.iterations)]\n", 0.05},
    {"projective",
     "@config\nsphere.radius = 2\nsphere.center = 0.5i\n"
     "@viewer sphere z \"Riemann sphere\" \"Ctrl + 1\" projective\n"
     "w := (z^2 - 1) / (z^2 + 1)\n"
     "color = [0.5 + 0.4 sin(re(w) * 3), 0.5 + 0.4 cos(im(w) * 3), 0.5 + 0.4 sin(|z|)]\n", 0.01},
    {"mandelbrot",
     "@config\n.maxIterations = 200\nplane.center = -0.5\nplane.radius = 1.5\n"
     "@viewer plane c\n"
     "z := 0\n"
     "while z not escapes do\n"
     "    z = z^2 + c\n"
     "end\n"
     "if .stuck then\n"
     "    color = .black\n"
     "else\n"
     "    color = hsv(.iterations / 30, 0.7, 1)\n"
     "end\n", 0.03},
    {"newton",
     "@config\n.maxIterations = 50\n"
     "@viewer plane z\n"
     "w := z\n"
     "for k in 1..50 do\n"
     "    d := (w^3 - 1) / (3 w^2)\n"
     "    w -= d\n"
     "    if d vanishes then\n"
     "        break\n"
     "    end\n"
     "end\n"
     "color = mix(rgb(re(w) * 0.5 + 0.5, im(w) * 0.5 + 0.5, 0.5), .white, 0.1)\n"
     "if |w - 1| < 0.1 then\n"
     "    color = .red\n"
     "    return\n"
     "end\n"
     "color = color * 0.9\n", 0.03},
};

static bool cg_have_spirv_val(void) {
    static int have = -1;
    if (have < 0) have = system("spirv-val --version > /dev/null 2>&1") == 0;
    return have == 1;
}

TEST(spirv_validates) {
    if (!cg_have_spirv_val()) SKIP("spirv-val not installed");
    for (int i = 0; i < ARRAY_COUNT(cg_scripts); i++) {
        Program *p = program_load(cg_scripts[i].src, cg_scripts[i].name);
        if (p->nerrors) {
            printf("    %s: unexpected errors\n", cg_scripts[i].name);
            print_diags(p);
            test_failures_++;
            program_free(p);
            continue;
        }
        for (int v = 0; v < p->nviewers; v++) {
            u32 n;
            char err[256];
            u32 *words = codegen_viewer(p, p->viewers[v], &n, err, sizeof err);
            CHECK_MSG(words != NULL, "%s: %s", cg_scripts[i].name, err);
            if (!words) continue;
            const char *path = "build/test_tmp.spv";
            fs_write_file(path, words, n * 4);
            free(words);
            int rc = system("spirv-val --target-env vulkan1.0 build/test_tmp.spv > build/test_tmp.txt 2>&1");
            if (rc != 0) {
                size_t len;
                char *out = fs_read_file("build/test_tmp.txt", &len);
                CHECK_MSG(rc == 0, "%s: spirv-val failed:\n%s", cg_scripts[i].name, out ? out : "");
                free(out);
            }
        }
        program_free(p);
    }
}

/* MoltenVK translates SPIR-V to Metal with SPIRV-Cross; function names that are
 * also Metal library functions (clamp, atan2, ...) used to break compilation on
 * macOS.  Check that every function in the translated code has our prefix. */
TEST(msl_function_names) {
    static int have = -1;
    if (have < 0) have = system("spirv-cross --help > /dev/null 2>&1") == 0 || system("which spirv-cross > /dev/null 2>&1") == 0;
    if (!have) SKIP("spirv-cross not installed");
    for (int i = 0; i < ARRAY_COUNT(cg_scripts); i++) {
        Program *p = program_load(cg_scripts[i].src, cg_scripts[i].name);
        if (p->nerrors) { program_free(p); continue; }
        u32 n;
        char err[256];
        u32 *words = codegen_viewer(p, p->viewers[0], &n, err, sizeof err);
        program_free(p);
        if (!words) continue;
        fs_write_file("build/test_msl.spv", words, n * 4);
        free(words);
        if (system("spirv-cross --msl build/test_msl.spv --output build/test_msl.metal > build/test_msl.txt 2>&1") != 0) {
            CHECK_MSG(false, "%s: spirv-cross failed", cg_scripts[i].name);
            continue;
        }
        size_t len;
        char *msl = fs_read_file("build/test_msl.metal", &len);
        if (!msl) continue;
        /* function definitions look like "float fs_name(float _12, ...)" at the start of a line */
        for (char *line = msl; line && *line;) {
            char *next = strchr(line, '\n');
            if (next) *next = 0;
            /* a definition: starts in column 0, has "(", ends with ")" and no ";" or "=" */
            size_t ll = strlen(line);
            char *paren = strchr(line, '(');
            if (paren && ll > 0 && line[ll - 1] == ')' && line[0] != ' ' && line[0] != '#' && !strchr(line, ';') &&
                !strchr(line, '=') && strncmp(line, "struct", 6) != 0 && strncmp(line, "template", 8) != 0 &&
                strncmp(line, "fragment", 8) != 0 && strncmp(line, "kernel", 6) != 0 && strncmp(line, "static", 6) != 0) {
                char *e = paren, *b;
                while (e > line && e[-1] == ' ') e--;
                b = e;
                while (b > line && (isalnum((unsigned char)b[-1]) || b[-1] == '_')) b--;
                char name[128];
                snprintf(name, sizeof name, "%.*s", (int)(e - b), b);
                CHECK_MSG(strncmp(name, "fs_", 3) == 0 || strcmp(name, "main0") == 0 || strncmp(name, "spv", 3) == 0,
                          "%s: Metal function '%s' (line: %s)", cg_scripts[i].name, name, line);
            }
            line = next ? next + 1 : NULL;
        }
        free(msl);
    }
}

TEST(gpu_matches_cpu) {
#ifdef FS_HAVE_GFX
    char err[512];
    if (!gfx_preinit_loader()) SKIP("no Vulkan loader");
    GfxInitDesc d = {0};
    if (!gfx_init(&d, err, sizeof err)) SKIP("no Vulkan device");
    const int W = 48, H = 40;
    u8 *a = (u8 *)xmalloc(W * H * 4), *b = (u8 *)xmalloc(W * H * 4);
    for (int i = 0; i < ARRAY_COUNT(cg_scripts); i++) {
        Program *p = program_load(cg_scripts[i].src, cg_scripts[i].name);
        if (p->nerrors) { program_free(p); continue; } /* reported by spirv_validates */
        Section *v = p->viewers[0];
        u32 n;
        u32 *words = codegen_viewer(p, v, &n, err, sizeof err);
        gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
        int vid = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
        free(words);
        CHECK_MSG(vid > 0, "%s: %s", cg_scripts[i].name, err);
        if (vid > 0) {
            CHECK_MSG(gfx_render_to_memory(vid, W, H, 0, a, err, sizeof err), "%s", err);
            gfx_viewer_destroy(vid);
            render_cpu(p, p->initial_state, v, W, H, b);
            int bad = 0, worst = 0, wx = 0, wy = 0;
            for (int k = 0; k < W * H; k++) {
                int dmax = 0;
                for (int c = 0; c < 3; c++) dmax = FS_MAX(dmax, abs((int)a[4 * k + c] - (int)b[4 * k + c]));
                if (dmax > 3) bad++;
                if (dmax > worst) { worst = dmax; wx = k % W; wy = k / W; }
            }
            double frac = (double)bad / (W * H);
            CHECK_MSG(frac <= cg_scripts[i].max_bad, "%s: %d of %d pixels differ (worst %d at %d,%d: gpu %d,%d,%d cpu %d,%d,%d)",
                      cg_scripts[i].name, bad, W * H, worst, wx, wy, a[4 * (wy * W + wx)], a[4 * (wy * W + wx) + 1],
                      a[4 * (wy * W + wx) + 2], b[4 * (wy * W + wx)], b[4 * (wy * W + wx) + 1], b[4 * (wy * W + wx) + 2]);
        }
        program_free(p);
    }
    free(a);
    free(b);
    gfx_shutdown();
#else
    SKIP("built without gfx");
#endif
}

TEST(flood_spirv_validates) {
    if (!cg_have_spirv_val()) SKIP("spirv-val not installed");
    const char *src =
        "@global\n"
        "define esc(t, c) = x where\n"
        "    x = t\n"
        "    y := t\n"
        "    while x not escapes do\n"
        "        (x, y) = (x^2 + c - 0.3 y, x)\n"
        "    end\n"
        "end\n"
        "@viewer v c\n"
        "w := critical t -> log(esc(t, c)) with seed 0.5 continuing from c = .lastClick\n"
        "if .stuck then\n"
        "    return\n"
        "end\n"
        "color = gray(fract(.iterations / 10))\n";
    Program *p = program_load(src, "flood_spirv");
    CHECK(p->nerrors == 0);
    if (p->nerrors) { print_diags(p); program_free(p); return; }
    u32 n;
    char err[256];
    u32 *words = codegen_viewer_flood(p, p->viewers[0], &n, err, sizeof err);
    CHECK_MSG(words != NULL, "%s", err);
    if (words) {
        fs_write_file("build/test_flood.spv", words, n * 4);
        free(words);
        int rc = system("spirv-val --target-env vulkan1.0 build/test_flood.spv > build/test_tmp.txt 2>&1");
        if (rc != 0) {
            size_t len;
            char *out = fs_read_file("build/test_tmp.txt", &len);
            CHECK_MSG(rc == 0, "spirv-val failed:\n%s", out ? out : "");
            free(out);
        }
        /* and SPIRV-Cross (what MoltenVK uses) turns it into a Metal compute kernel */
        static int have_cross = -1;
        if (have_cross < 0) have_cross = system("which spirv-cross > /dev/null 2>&1") == 0;
        if (have_cross) {
            int rc2 = system("spirv-cross --msl build/test_flood.spv --output build/test_flood.metal > build/test_tmp.txt 2>&1");
            CHECK_MSG(rc2 == 0, "spirv-cross --msl failed on the flood shader");
            size_t len;
            char *msl = rc2 == 0 ? fs_read_file("build/test_flood.metal", &len) : NULL;
            CHECK(msl && strstr(msl, "kernel void main0("));
            free(msl);
        }
    }
    program_free(p);
}

/* flood fill (continuation): the GPU compute shaders must find the same solutions as the CPU */
TEST(flood_gpu_matches_cpu) {
#ifdef FS_HAVE_GFX
    char err[512];
    if (!gfx_preinit_loader()) SKIP("no Vulkan loader");
    GfxInitDesc d = {0};
    if (!gfx_init(&d, err, sizeof err)) SKIP("no Vulkan device");
    const char *src =
        "@config\nv.center = 0.5\nv.radius = 1.5\n"
        "@global\n"
        "start := 1 + 0.5i\n"
        "define f(t, c) = t^3 - 2t + c\n"
        "@viewer v c\n"
        "w := root t -> f(t, c) with seed 1.2 continuing from c = start\n"
        "if .stuck then\n"
        "    color = .white\n"
        "else\n"
        "    color = [0.5 + 0.4 sin(3 re(w)), 0.5 + 0.4 sin(3 im(w)), 0.2]\n"
        "end\n";
    Program *p = program_load(src, "flood");
    CHECK(p->nerrors == 0 && p->viewers[0]->flood);
    if (p->nerrors) { print_diags(p); program_free(p); gfx_shutdown(); return; }
    const int W = 37, H = 29;
    u8 *a = (u8 *)xmalloc(W * H * 4), *b = (u8 *)xmalloc(W * H * 4);
    FloodEntry *fa = (FloodEntry *)xmalloc(sizeof(FloodEntry) * W * H), *fb = (FloodEntry *)xmalloc(sizeof(FloodEntry) * W * H);
    gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
    CHECK_MSG(flood_render_gpu(p, p->initial_state, p->viewers[0], W, H, a, fa, err, sizeof err), "%s", err);
    flood_render_cpu(p, p->initial_state, p->viewers[0], W, H, b, fb);
    int bad = 0, done = 0;
    for (int k = 0; k < W * H; k++) {
        if (fb[k].status == FLOOD_DONE) done++;
        bool same = fa[k].status == fb[k].status &&
                    (fa[k].status != FLOOD_DONE || (fabsf(fa[k].t[0] - fb[k].t[0]) < 1e-3f && fabsf(fa[k].t[1] - fb[k].t[1]) < 1e-3f));
        if (!same) bad++;
    }
    CHECK_MSG(done > W * H * 9 / 10, "only %d of %d pixels converged", done, W * H);
    CHECK_MSG(bad <= 2, "%d of %d pixels differ", bad, W * H);
    /* the same in double-single precision (deep zooms) */
    CHECK_MSG(flood_render_gpu_ex(p, p->initial_state, p->viewers[0], true, W, H, a, fa, err, sizeof err), "%s", err);
    int bad_df = 0;
    for (int k = 0; k < W * H; k++)
        if (fa[k].status != fb[k].status ||
            (fa[k].status == FLOOD_DONE && (fabsf(fa[k].t[0] - fb[k].t[0]) >= 1e-3f || fabsf(fa[k].t[1] - fb[k].t[1]) >= 1e-3f)))
            bad_df++;
    CHECK_MSG(bad_df <= 2, "double precision: %d of %d pixels differ", bad_df, W * H);
    /* continuity: neighbours (where both converged) found nearby roots */
    int jumps = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x + 1 < W; x++) {
            FloodEntry *e0 = &fb[y * W + x], *e1 = &fb[y * W + x + 1];
            if (e0->status == FLOOD_DONE && e1->status == FLOOD_DONE &&
                fabsf(e0->t[0] - e1->t[0]) + fabsf(e0->t[1] - e1->t[1]) > 0.3f) jumps++;
        }
    CHECK_MSG(jumps < W, "%d jumps between neighbours", jumps);
    free(a); free(b); free(fa); free(fb);
    program_free(p);
    gfx_shutdown();
#else
    SKIP("built without gfx");
#endif
}

/* Mending: two nearby branch points of a square root (the continuation along the rows between them
 * comes back with the other root and would carry it to the edge) leave only a short cut. */
static int band_jumps(const FloodEntry *f, int W, int H, int x0) {
    int jumps = 0;
    for (int x = x0; x < W; x++)
        for (int y = 0; y + 1 < H; y++) {
            const FloodEntry *a = &f[y * W + x], *b = &f[(y + 1) * W + x];
            if (a->status == FLOOD_DONE && b->status == FLOOD_DONE && fabsf(a->t[0] - b->t[0]) + fabsf(a->t[1] - b->t[1]) > 0.3f)
                jumps++;
        }
    return jumps;
}

TEST(flood_mends_bands) {
    const char *src =
        "@config\nv.center = 0\nv.radius = 1.5\n"
        "@global\nstart := -1.2\n"
        "@viewer v c\n"
        "w := root t -> t^2 - (c - 0.5 - 0.05i) (c - 0.5 + 0.05i) with seed -1.7 continuing from c = start\n"
        "color = if .stuck then .white else rainbow(arg(w) / tau)\n";
    Program *p = program_load(src, "band");
    CHECK(p->nerrors == 0 && p->viewers[0]->flood);
    if (p->nerrors) { print_diags(p); program_free(p); return; }
    const int W = 64, H = 48; /* the branch points are at x = 40, between rows 23 and 24 */
    u8 *a = (u8 *)xmalloc(W * H * 4), *b = (u8 *)xmalloc(W * H * 4);
    FloodEntry *fa = (FloodEntry *)xmalloc(sizeof(FloodEntry) * W * H), *fb = (FloodEntry *)xmalloc(sizeof(FloodEntry) * W * H);
    flood_render_cpu(p, p->initial_state, p->viewers[0], W, H, b, fb);
    int done = 0;
    for (int k = 0; k < W * H; k++) done += fb[k].status == FLOOD_DONE;
    CHECK_MSG(done == W * H, "only %d of %d pixels converged", done, W * H);
    /* right of the branch points the roots are continuous down every column (the cut is at most a few pixels long) */
    CHECK_MSG(band_jumps(fb, W, H, 46) == 0, "%d jumps right of the branch points", band_jumps(fb, W, H, 46));
    /* and the rows between them carry the same root as those above and below */
    CHECK(fb[23 * W + 60].t[0] * fb[10 * W + 60].t[0] > 0 && fb[24 * W + 60].t[0] * fb[40 * W + 60].t[0] > 0);
#ifdef FS_HAVE_GFX
    char err[512];
    GfxInitDesc d = {0};
    if (gfx_preinit_loader() && gfx_init(&d, err, sizeof err)) {
        gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
        CHECK_MSG(flood_render_gpu(p, p->initial_state, p->viewers[0], W, H, a, fa, err, sizeof err), "%s", err);
        int bad = 0;
        for (int k = 0; k < W * H; k++)
            if (fa[k].status != fb[k].status || fabsf(fa[k].t[0] - fb[k].t[0]) > 1e-3f || fabsf(fa[k].t[1] - fb[k].t[1]) > 1e-3f) bad++;
        CHECK_MSG(bad <= 2, "%d of %d pixels differ between the GPU and the CPU", bad, W * H);
        CHECK_MSG(band_jumps(fa, W, H, 46) == 0, "GPU: %d jumps right of the branch points", band_jumps(fa, W, H, 46));
        gfx_shutdown();
    }
#endif
    free(a); free(b); free(fa); free(fb);
    program_free(p);
}

/* ---- deep zooms: double-single precision ---------------------------------------------- */
/* every test script compiles in double-single precision, validates, converts to Metal and renders
 * like the CPU (at ordinary zooms the two agree up to rounding) */
TEST(df_viewers) {
#ifdef FS_HAVE_GFX
    char err[512];
    bool val = cg_have_spirv_val();
    static int have_cross = -1;
    if (have_cross < 0) have_cross = system("which spirv-cross > /dev/null 2>&1") == 0;
    bool gpu = gfx_preinit_loader();
    GfxInitDesc d = {0};
    if (gpu && !gfx_init(&d, err, sizeof err)) gpu = false;
    const int W = 48, H = 40;
    u8 *a = (u8 *)xmalloc(W * H * 4), *b = (u8 *)xmalloc(W * H * 4);
    for (int i = 0; i < ARRAY_COUNT(cg_scripts); i++) {
        Program *p = program_load(cg_scripts[i].src, cg_scripts[i].name);
        if (p->nerrors || p->viewers[0]->mobius) { program_free(p); continue; }
        Section *v = p->viewers[0];
        u32 n;
        u32 *words = codegen_viewer_ex(p, v, true, &n, err, sizeof err);
        CHECK_MSG(words != NULL, "%s: %s", cg_scripts[i].name, err);
        if (!words) { program_free(p); continue; }
        fs_write_file("build/test_df.spv", words, n * 4);
        if (val) {
            int rc = system("spirv-val --target-env vulkan1.0 build/test_df.spv > build/test_tmp.txt 2>&1");
            if (rc != 0) {
                size_t len;
                char *out = fs_read_file("build/test_tmp.txt", &len);
                CHECK_MSG(rc == 0, "%s (double precision): spirv-val failed:\n%s", cg_scripts[i].name, out ? out : "");
                free(out);
            }
        }
        if (have_cross)
            CHECK_MSG(system("spirv-cross --msl build/test_df.spv --output build/test_df.metal > build/test_tmp.txt 2>&1") == 0,
                      "%s (double precision): spirv-cross --msl failed", cg_scripts[i].name);
        if (gpu) {
            gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
            int vid = gfx_viewer_create(words, n, err, sizeof err);
            CHECK_MSG(vid > 0, "%s: %s", cg_scripts[i].name, err);
            if (vid > 0) {
                CHECK_MSG(gfx_render_to_memory(vid, W, H, 0, a, err, sizeof err), "%s", err);
                gfx_viewer_destroy(vid);
                render_cpu(p, p->initial_state, v, W, H, b);
                int bad = 0;
                for (int k = 0; k < W * H; k++) {
                    int dmax = 0;
                    for (int c = 0; c < 3; c++) dmax = FS_MAX(dmax, abs((int)a[4 * k + c] - (int)b[4 * k + c]));
                    if (dmax > 3) bad++;
                }
                CHECK_MSG((double)bad / (W * H) <= cg_scripts[i].max_bad + 0.02, "%s (double precision): %d of %d pixels differ",
                          cg_scripts[i].name, bad, W * H);
            }
        }
        free(words);
        program_free(p);
    }
    free(a);
    free(b);
    if (gpu) gfx_shutdown();
#else
    SKIP("built without gfx");
#endif
}

/* the arithmetic keeps about twice the digits of a float, on the GPU */
TEST(df_arithmetic) {
#ifdef FS_HAVE_GFX
    char err[512];
    if (!gfx_preinit_loader()) SKIP("no Vulkan loader");
    GfxInitDesc d = {0};
    if (!gfx_init(&d, err, sizeof err)) SKIP("no Vulkan device");
    const char *src =
        "@viewer v c\n"                     /* (typed locals: not folded into constants by the compiler) */
        "a : R = 1\n"
        "b : R = 0.0000000001\n"
        "add := ((a + b) - a) * 1e10\n"                         /* 1 */
        "x : R = 1 + 1 / 1048576\n"
        "mul := (x * x - 1 - 2 / 1048576) * 1099511627776.0\n"   /* 1 */
        "three : R = 3\n"
        "q := a / three\n"
        "div := (q * three - a) * 1e13 + 0.5\n"                    /* about 0.5 (the error is ~1e-15) */
        "two : R = 2\n"
        "r := sqrt(two)\n"
        "sq := (r * r - two) * 1e12 + 0.5\n"                       /* about 0.5 */
        "tiny : R = 0.0000001\n"
        "z := complex(a, tiny)\n"
        "zz := z * z\n"                                            /* 1 - 1e-14 + 2e-7 i */
        "cm := (re(zz) - a) * -1e14\n"                             /* 1 */
        "color = [add, mul, if |div - 0.5| < 0.1 and |sq - 0.5| < 0.1 and |cm - 1| < 0.01 then 1 else 0]\n";
    Program *p = program_load(src, "df_arith");
    CHECK(p->nerrors == 0);
    if (p->nerrors) { print_diags(p); program_free(p); gfx_shutdown(); return; }
    gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
    /* (no check of single precision: shader compilers may simplify such expressions symbolically) */
    u32 n;
    u32 *words = codegen_viewer_ex(p, p->viewers[0], true, &n, err, sizeof err);
    int vid = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
    free(words);
    CHECK_MSG(vid > 0, "%s", err);
    if (vid > 0) {
        u8 px[4 * 4];
        CHECK_MSG(gfx_render_to_memory(vid, 2, 2, 0, px, err, sizeof err), "%s", err);
        gfx_viewer_destroy(vid);
        CHECK_MSG(px[0] == 255 && px[1] == 255 && px[2] == 255, "double precision: %d %d %d", px[0], px[1], px[2]);
    }
    program_free(p);
    gfx_shutdown();
#else
    SKIP("built without gfx");
#endif
}

/* Deep zooms in shaders that declare IEEE floats (float controls) instead of marking each operation: a valid
   module, and in Metal (through SPIRV-Cross) plain arithmetic, without the per-operation helper functions that
   make the marked version slow there.  (Whether a GPU keeps such shaders exact is checked when the app starts:
   app_init_exact_floats; Mesa doesn't, Metal without fast maths does.) */
TEST(df_float_controls_module) {
    Program *p = program_load("@viewer v c\nz := 0\nwhile z not escapes do\n    z = z^2 + c\nend\n"
                              "color = gray(.iterations / 100)\n", "fc");
    CHECK(p->nerrors == 0);
    char err[256];
    u32 n;
    codegen_set_float_controls(true);
    u32 *words = codegen_viewer_ex(p, p->viewers[0], true, &n, err, sizeof err);
    codegen_set_float_controls(false);
    program_free(p);
    CHECK_MSG(words != NULL, "%s", err);
    if (!words) return;
    fs_write_file("build/test_fc.spv", words, n * 4);
    free(words);
    static int have_val = -1, have_cross = -1;
    if (have_val < 0) have_val = system("spirv-val --version > /dev/null 2>&1") == 0;
    if (have_cross < 0) have_cross = system("spirv-cross --help > /dev/null 2>&1") == 0 || system("which spirv-cross > /dev/null 2>&1") == 0;
    if (have_val) CHECK_MSG(system("spirv-val build/test_fc.spv > build/test_fc.txt 2>&1") == 0, "spirv-val failed (build/test_fc.txt)");
    if (!have_cross) SKIP("spirv-cross not installed");
    CHECK(system("spirv-cross --msl build/test_fc.spv --output build/test_fc.metal > /dev/null 2>&1") == 0);
    size_t len;
    char *msl = fs_read_file("build/test_fc.metal", &len);
    CHECK(msl != NULL);
    if (msl) {
        CHECK_MSG(!strstr(msl, "optnone") && !strstr(msl, "spvFAdd"), "the Metal code still calls helpers per operation");
        CHECK(strstr(msl, "fma(") != NULL); /* (the exact products) */
        free(msl);
    }
}

/* a deep zoom of the Mandelbrot set (radius 1e-10) against a double-precision reference */
static int df_ref_iter(double cr, double ci, int maxit) {
    double zr = 0, zi = 0;
    for (int k = 0; k < maxit; k++) {
        if (zr * zr + zi * zi > 4.0) return k;
        double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
    }
    return -1;
}

TEST(df_deep_zoom) {
#ifdef FS_HAVE_GFX
    char err[512];
    if (!gfx_preinit_loader()) SKIP("no Vulkan loader");
    GfxInitDesc d = {0};
    if (!gfx_init(&d, err, sizeof err)) SKIP("no Vulkan device");
    const char *src =
        "@config\n.maxIterations = 1000\n.escapeRadius = 2\n"
        "@viewer v c\n"
        "z := 0\n"
        "while z not escapes do\n"
        "    z = z^2 + c\n"
        "end\n"
        "n := .iterations\n"
        "color = if .stuck then .black else [(n mod 256) / 255, floor(n / 256) / 255, 0.5]\n";
    Program *p = program_load(src, "df_deep");
    CHECK(p->nerrors == 0);
    if (p->nerrors) { print_diags(p); program_free(p); gfx_shutdown(); return; }
    Section *v = p->viewers[0];
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    double cx = -0.743643887037151, cy = 0.131825904205330, radius = 1e-10;
    float hx = (float)cx, hy = (float)cy;
    state[v->center->slot] = fw(hx);
    state[v->center->slot + 1] = fw(hy);
    state[v->center_lo->slot] = fw((float)(cx - hx));
    state[v->center_lo->slot + 1] = fw((float)(cy - hy));
    state[v->radius->slot] = fw((float)radius);
    const int W = 64, H = 64;
    CHECK(viewer_deep(p, state, v, W, H, false));
    CHECK(!viewer_wants_df(p, state, v, W, H, false)); /* (until .doublePrecision is on) */
    state[p->s_double->slot] = 1;
    CHECK(viewer_wants_df(p, state, v, W, H, false));
    gfx_set_globals(state, (u32)p->state_words * 4);
    u8 *rgba = (u8 *)xmalloc((size_t)W * H * 4);
    int exact[2] = {0, 0};
    for (int df = 0; df < 2; df++) {
        u32 n;
        u32 *words = codegen_viewer_ex(p, v, df != 0, &n, err, sizeof err);
        int vid = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
        free(words);
        CHECK_MSG(vid > 0, "%s", err);
        if (vid <= 0) continue;
        CHECK_MSG(gfx_render_to_memory(vid, W, H, 0, rgba, err, sizeof err), "%s", err);
        gfx_viewer_destroy(vid);
        double scale = radius / (0.5 * FS_MIN(W, H));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int r = df_ref_iter(cx + (x + 0.5 - W / 2.0) * scale, cy + (H / 2.0 - (y + 0.5)) * scale, 1000);
                u8 *q = rgba + 4 * (y * W + x);
                int g = q[0] == 0 && q[1] == 0 && q[2] == 0 ? -1 : q[0] + 256 * q[1];
                if (g == r) exact[df]++;
            }
    }
    CHECK_MSG(exact[1] >= W * H * 8 / 10, "double precision: %d of %d iteration counts exact", exact[1], W * H);
    CHECK_MSG(exact[0] <= W * H / 10, "single precision: %d of %d exact (the test isn't deep enough)", exact[0], W * H);
    free(rgba);
    free(state);
    program_free(p);
    gfx_shutdown();
#else
    SKIP("built without gfx");
#endif
}

static const TestCase tests_codegen[] = {
    TESTCASE(df_viewers),
    TESTCASE(df_arithmetic),
    TESTCASE(df_float_controls_module),
    TESTCASE(df_deep_zoom),
    TESTCASE(flood_spirv_validates),
    TESTCASE(flood_gpu_matches_cpu),
    TESTCASE(flood_mends_bands),
    TESTCASE(spirv_validates),
    TESTCASE(msl_function_names),
    TESTCASE(gpu_matches_cpu),
};
