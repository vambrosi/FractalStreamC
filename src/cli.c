/* cli.c - command line interface. */

static const char *cli_usage =
    "FractalStream " FS_VERSION " - interactive plots of the complex plane\n"
    "\n"
    "usage:\n"
    "  fractalstream                       open the editor with an example script\n"
    "  fractalstream SCRIPT                open SCRIPT\n"
    "  fractalstream --check SCRIPT        check SCRIPT for errors (exit code 1 if any)\n"
    "  fractalstream --spirv DIR SCRIPT    write the SPIR-V of every viewer to DIR/<viewer>.spv\n"
    "  fractalstream --state SCRIPT        print the initial value of every global\n"
    "  fractalstream --render OUT.png [options] SCRIPT\n"
    "                                      render a viewer to a PNG without opening a window\n"
    "      --viewer NAME    viewer to render (default: the first one)\n"
    "      --size WxH       image size in pixels (default 512x512)\n"
    "      --cpu            use the CPU interpreter instead of the GPU (slow, for testing)\n"
    "      --set NAME=VALUE set a global first, e.g. --set c=-0.8+0.156i or --set plane.radius=0.5\n"
    "  fractalstream --tool NAME --at X+Yi [--viewer NAME] SCRIPT\n"
    "                                      run a tool once at a point; print its draw calls and the globals\n"
    "  fractalstream --version | --help\n";

static int cli_load(const char *path, Program **out) {
    size_t len;
    char *src = fs_read_file(path, &len);
    *out = NULL;
    if (!src) {
        fprintf(stderr, "error: can't read %s\n", path);
        return 2;
    }
    Program *p = program_load(src, path);
    free(src);
    StrBuf sb = {0};
    program_format_diags(p, &sb);
    if (sb.len) fprintf(stderr, "%s", sb_cstr(&sb));
    sb_free(&sb);
    *out = p;
    return p->nerrors ? 1 : 0;
}

/* "1.5", "-2", "0.3i", "-0.8+0.156i", "1-2i" */
static bool parse_complex_literal(const char *s, float *re, float *im) {
    char *end;
    *re = *im = 0;
    double a = strtod(s, &end);
    if (end == s) {
        if (*s == 'i') { *im = 1; return s[1] == 0; }
        if (*s == '-' && s[1] == 'i' && !s[2]) { *im = -1; return true; }
        return false;
    }
    if (*end == 'i' && end[1] == 0) { *im = (float)a; return true; }
    if (*end == 0) { *re = (float)a; return true; }
    if (*end != '+' && *end != '-') return false;
    const char *t = end;
    double b = strtod(t, &end);
    if (end == t + 1 && *end == 'i') b = *t == '-' ? -1 : 1; /* "1+i" */
    if (*end != 'i' || end[1] != 0) return false;
    *re = (float)a;
    *im = (float)b;
    return true;
}

static Sym *cli_find_global(Program *p, const char *name) {
    const char *n = name[0] == '.' ? name + 1 : name;
    for (int i = 0; i < p->globals.len; i++)
        if (strcmp(p->globals.data[i]->name, n) == 0) return p->globals.data[i];
    return NULL;
}

static bool cli_set_global(Program *p, u32 *state, const char *assign) {
    const char *eq = strchr(assign, '=');
    if (!eq) return false;
    char name[128];
    snprintf(name, sizeof name, "%.*s", (int)(eq - assign), assign);
    Sym *s = cli_find_global(p, name);
    float re, im;
    if (!s || !parse_complex_literal(eq + 1, &re, &im)) {
        fprintf(stderr, "error: can't set '%s' (unknown global or bad value)\n", assign);
        return false;
    }
    u32 *dst = state + s->slot;
    switch (s->type->kind) {
    case T_INT: dst[0] = (u32)(i32)re; break;
    case T_BOOL: dst[0] = re != 0; break;
    case T_REAL: dst[0] = fw(re); break;
    case T_COMPLEX: dst[0] = fw(re); dst[1] = fw(im); break;
    case T_QUAT: dst[0] = fw(re); dst[1] = fw(im); dst[2] = dst[3] = 0; break;
    default: fprintf(stderr, "error: '%s' can't be set from the command line\n", name); return false;
    }
    return true;
}

static void cli_print_value(Type *t, const u32 *v) {
    switch (t->kind) {
    case T_BOOL: printf("%s", v[0] ? "true" : "false"); break;
    case T_INT: printf("%d", (i32)v[0]); break;
    case T_REAL: printf("%.9g", wf(v[0])); break;
    case T_COMPLEX: printf("%.9g%+.9gi", wf(v[0]), wf(v[1])); break;
    case T_QUAT: printf("quat(%.9g, %.9g, %.9g, %.9g)", wf(v[0]), wf(v[1]), wf(v[2]), wf(v[3])); break;
    case T_VEC:
        printf("[");
        for (int i = 0; i < t->n; i++) {
            if (i) printf(", ");
            cli_print_value(t->elem, v + i * t->stride);
        }
        printf("]");
        break;
    case T_LIST: {
        i32 n = (i32)v[0];
        printf("[");
        for (int i = 0; i < n && i < t->n; i++) {
            if (i) printf(", ");
            cli_print_value(t->elem, v + t->data_off + i * t->stride);
        }
        printf("]");
        break;
    }
    default: printf("?"); break;
    }
}

static void cli_print_state(Program *p, const u32 *state) {
    for (int i = 0; i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        printf("%s%s : %s = ", s->is_internal ? "." : "", s->name, type_str(s->type));
        cli_print_value(s->type, state + s->slot);
        printf("\n");
    }
}

static Section *cli_find_viewer(Program *p, const char *name) {
    if (!name) return p->nviewers ? p->viewers[0] : NULL;
    for (int v = 0; v < p->nviewers; v++)
        if (p->viewers[v]->name && strcmp(p->viewers[v]->name, name) == 0) return p->viewers[v];
    return NULL;
}

/* CPU reference rendering, rows top to bottom, RGBA8 */
void render_cpu(Program *p, const u32 *state, Section *v, int w, int h, u8 *rgba) {
    u32 *st = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    Interp I;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            memcpy(st, state, sizeof(u32) * (size_t)p->state_words);
            interp_init(&I, p, st);
            float re, im, c[3];
            viewer_pixel_to_plane(p, st, v, (float)w, (float)h, x + 0.5f, y + 0.5f, &re, &im);
            interp_eval_viewer(&I, v, re, im, c);
            interp_free(&I);
            u8 *px = rgba + 4 * ((size_t)y * w + x);
            for (int k = 0; k < 3; k++) px[k] = (u8)(c[k] * 255.0f + 0.5f);
            px[3] = 255;
        }
    }
    free(st);
}

#define FS_HAVE_GFX 1

static bool render_gpu(Program *p, const u32 *state, Section *v, int w, int h, u8 *rgba, char *err, int errcap) {
#ifdef FS_HAVE_GFX
    u32 nwords;
    u32 *spv = codegen_viewer(p, v, &nwords, err, errcap);
    if (!spv) return false;
    GfxInitDesc d = {0};
    d.validation = getenv("FS_VALIDATION") != NULL;
    if (!gfx_preinit_loader()) {
        snprintf(err, (size_t)errcap, "could not load the Vulkan library (is a Vulkan driver installed?)");
        free(spv);
        return false;
    }
    if (!gfx_init(&d, err, errcap)) {
        free(spv);
        return false;
    }
    gfx_set_globals(state, (u32)p->state_words * 4);
    int vid = gfx_viewer_create(spv, nwords, err, errcap);
    free(spv);
    bool ok = vid > 0 && gfx_render_to_memory(vid, w, h, 256, rgba, err, errcap);
    if (vid > 0) gfx_viewer_destroy(vid);
    gfx_shutdown();
    return ok;
#else
    snprintf(err, (size_t)errcap, "this build has no GPU renderer");
    (void)p; (void)state; (void)v; (void)w; (void)h; (void)rgba;
    return false;
#endif
}

#include "app.h"

int cli_main(int argc, char **argv) {
    const char *mode = NULL, *arg = NULL, *script = NULL, *viewer = NULL, *tool = NULL, *at = NULL;
    int w = 512, h = 512;
    bool cpu = false;
    const char *sets[32];
    int nsets = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { printf("%s", cli_usage); return 0; }
        if (!strcmp(a, "--version")) { printf("FractalStream %s\n", FS_VERSION); return 0; }
        if (!strcmp(a, "--check") || !strcmp(a, "--state")) { mode = a; continue; }
        if ((!strcmp(a, "--spirv") || !strcmp(a, "--render")) && i + 1 < argc) { mode = a; arg = argv[++i]; continue; }
        if (!strcmp(a, "--viewer") && i + 1 < argc) { viewer = argv[++i]; continue; }
        if (!strcmp(a, "--tool") && i + 1 < argc) { mode = a; tool = argv[++i]; continue; }
        if (!strcmp(a, "--at") && i + 1 < argc) { at = argv[++i]; continue; }
        if (!strcmp(a, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 16384 || h > 16384) {
                fprintf(stderr, "error: --size expects WxH, like 800x600\n");
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--cpu")) { cpu = true; continue; }
        if (!strcmp(a, "--set") && i + 1 < argc) { if (nsets < 32) sets[nsets++] = argv[++i]; continue; }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "error: unknown option %s\n\n%s", a, cli_usage);
            return 2;
        }
        script = a;
    }
    if (!mode) return app_run(script);
    if (!script) {
        fprintf(stderr, "error: no script given\n\n%s", cli_usage);
        return 2;
    }
    Program *p = NULL;
    int rc = cli_load(script, &p);
    if (rc) {
        program_free(p);
        return rc;
    }
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    for (int i = 0; i < nsets; i++)
        if (!cli_set_global(p, state, sets[i])) { rc = 2; goto done; }

    if (!strcmp(mode, "--check")) {
        printf("%s: ok (%d viewer%s, %d tool%s)\n", script, p->nviewers, p->nviewers == 1 ? "" : "s", p->ntools,
               p->ntools == 1 ? "" : "s");
    } else if (!strcmp(mode, "--state")) {
        cli_print_state(p, state);
    } else if (!strcmp(mode, "--spirv")) {
        for (int v = 0; v < p->nviewers; v++) {
            u32 n;
            char err[256];
            u32 *words = codegen_viewer(p, p->viewers[v], &n, err, sizeof err);
            if (!words) { fprintf(stderr, "error: %s\n", err); rc = 1; continue; }
            char path[1024];
            snprintf(path, sizeof path, "%s/%s.spv", arg, p->viewers[v]->name);
            if (!fs_write_file(path, words, n * 4)) { fprintf(stderr, "error: can't write %s\n", path); rc = 1; }
            else printf("wrote %s (%u words)\n", path, n);
            free(words);
        }
    } else if (!strcmp(mode, "--render")) {
        Section *v = cli_find_viewer(p, viewer);
        if (!v) { fprintf(stderr, "error: no viewer named '%s'\n", viewer ? viewer : ""); rc = 2; goto done; }
        u8 *rgba = (u8 *)xmalloc((size_t)w * h * 4);
        char err[512] = "";
        bool ok = true;
        double t0 = fs_time_seconds();
        if (cpu) render_cpu(p, state, v, w, h, rgba);
        else ok = render_gpu(p, state, v, w, h, rgba, err, sizeof err);
        if (!ok) {
            fprintf(stderr, "error: %s\n", err);
            rc = 1;
        } else if (!stbi_write_png(arg, w, h, 4, rgba, w * 4)) {
            fprintf(stderr, "error: can't write %s\n", arg);
            rc = 1;
        } else {
            printf("wrote %s (%dx%d, %s, %.2fs)\n", arg, w, h, cpu ? "cpu" : "gpu", fs_time_seconds() - t0);
        }
        free(rgba);
    } else if (!strcmp(mode, "--tool")) {
        Section *t = NULL;
        for (int k = 0; k < p->ntools; k++)
            if (!strcmp(p->tools[k]->display, tool)) t = p->tools[k];
        Section *v = cli_find_viewer(p, viewer);
        float re = 0, im = 0;
        if (!t) { fprintf(stderr, "error: no tool named '%s'\n", tool); rc = 2; goto done; }
        if (at && !parse_complex_literal(at, &re, &im)) { fprintf(stderr, "error: bad point '%s'\n", at); rc = 2; goto done; }
        state[p->s_last_click->slot] = fw(re);
        state[p->s_last_click->slot + 1] = fw(im);
        state[p->s_mouse->slot] = fw(re);
        state[p->s_mouse->slot + 1] = fw(im);
        Interp I;
        interp_init(&I, p, state);
        DrawItemVec draws = {0};
        I.draws = &draws;
        if (!interp_run_tool(&I, t, re, im)) { fprintf(stderr, "error: %s\n", I.error); rc = 1; }
        static const char *shapes[] = {"point", "circle", "disk", "line"};
        for (int k = 0; k < draws.len; k++) {
            DrawItem *d = &draws.data[k];
            printf("draw %s at %g%+gi", shapes[d->shape], d->p0[0], d->p0[1]);
            if (d->shape == DRAW_LINE) printf(" to %g%+gi", d->p1[0], d->p1[1]);
            if (d->shape == DRAW_CIRCLE || d->shape == DRAW_DISK) printf(" radius %g%s", d->radius, d->radius_px ? " px" : "");
            printf(" color [%g, %g, %g]", d->color[0], d->color[1], d->color[2]);
            if (d->viewer >= 0) printf(" in %s", p->viewers[d->viewer]->name);
            printf("\n");
        }
        (void)v;
        vec_free(draws);
        interp_free(&I);
        cli_print_state(p, state);
    }
done:
    free(state);
    program_free(p);
    return rc;
}
