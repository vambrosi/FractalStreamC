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
     "g : R = 0\n"
     "if st and n == 37 and it1 == 16 and d == 10 and t == 30 and acc == 6 then\n"
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

static const TestCase tests_codegen[] = {
    TESTCASE(spirv_validates),
    TESTCASE(msl_function_names),
    TESTCASE(gpu_matches_cpu),
};
