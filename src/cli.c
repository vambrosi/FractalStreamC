/* cli.c - command line interface. */

static const char *cli_usage =
    "FractalStreamC " FS_VERSION " - interactive plots of the complex plane\n"
    "\n"
    "usage:\n"
    "  fractalstreamc                      open the editor with an example script\n"
    "  fractalstreamc SCRIPT               open SCRIPT\n"
    "  fractalstreamc --check SCRIPT       check SCRIPT for errors (exit code 1 if any)\n"
    "  fractalstreamc --spirv DIR SCRIPT   write the SPIR-V of every viewer to DIR/<viewer>.spv\n"
    "  fractalstreamc --state SCRIPT       print the initial value of every global\n"
    "  fractalstreamc --render OUT.png [options] SCRIPT\n"
    "                                      render a viewer to a PNG without opening a window\n"
    "      --viewer NAME    viewer to render (default: the first one)\n"
    "      --size WxH       image size in pixels (default 512x512)\n"
    "      --cpu            use the CPU interpreter instead of the GPU (slow, for testing)\n"
    "      --set NAME=VALUE set a global first, e.g. --set c=-0.8+0.156i or --set plane.radius=0.5\n"
    "                       (--set .doublePrecision=true: deep zooms in double precision;\n"
    "                       --set .time=T: the picture at time T)\n"
    "  fractalstreamc --video OUT.mp4 [options] SCRIPT\n"
    "                                      a video of a viewer (H.264, MP4) over time: frame k shows .time = FROM + k / FPS\n"
    "      --seconds S      length (default 10)        --from T   time of the first frame (default 0)\n"
    "      --fps N          frames per second (default 30)\n"
    "      --samples N      samples per pixel, 1 to 16 (default: the script's .antialiasing)\n"
    "      --viewer, --size (default 1280x720, even numbers), --set: as for --render\n"
    "  fractalstreamc --tool NAME --at X+Yi [--event EVENT] [--viewer NAME] SCRIPT\n"
    "                                      run a tool once at a point (a click, or one event of a tool with\n"
    "                                      'on' blocks); print what it prints, its draw calls and the globals\n"
    "  fractalstreamc --builtins           print how the built-in functions are defined\n"
    "  fractalstreamc --version | --help\n";

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

/* "1.5", "-2", "0.3i", "-0.8+0.156i", "1-2i" (parse_complex_literal_d is in program.c) */
static bool parse_complex_literal(const char *s, float *re, float *im) {
    double r, i;
    bool ok = parse_complex_literal_d(s, &r, &i);
    *re = (float)r;
    *im = (float)i;
    return ok;
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
    if (s && s->type->kind == T_BOOL && (!strcmp(eq + 1, "true") || !strcmp(eq + 1, "false"))) {
        state[s->slot] = eq[1] == 't';
        return true;
    }
    if (s && s->type->kind == T_PROJ) { /* a complex number or inf */
        if (parse_proj_literal(eq + 1, state + s->slot)) return true;
        fprintf(stderr, "error: can't set '%s' (a point of the sphere: a complex number or inf)\n", assign);
        return false;
    }
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
    for (int v = 0; v < p->nviewers; v++) { /* a view's center keeps its digits (deep zooms) */
        Section *vs = p->viewers[v];
        if (vs->center != s || !vs->center_lo) continue;
        double dre, dim;
        parse_complex_literal_d(eq + 1, &dre, &dim);
        state[vs->center_lo->slot] = fw((float)(dre - (double)re));
        state[vs->center_lo->slot + 1] = fw((float)(dim - (double)im));
    }
    return true;
}

/* each line of `text` after `prefix` */
static void cli_print_lines(const char *prefix, const char *text) {
    for (const char *q = text; *q;) {
        const char *e = strchr(q, '\n');
        int n = e ? (int)(e - q) : (int)strlen(q);
        printf("%s%.*s\n", prefix, n, q);
        if (!e) break;
        q = e + 1;
    }
}

static void cli_print_value(Type *t, const u32 *v) {
    switch (t->kind) {
    case T_BOOL: printf("%s", v[0] ? "true" : "false"); break;
    case T_INT: printf("%d", (i32)v[0]); break;
    case T_REAL: printf("%.9g", wf(v[0])); break;
    case T_COMPLEX: printf("%.9g%+.9gi", wf(v[0]), wf(v[1])); break;
    case T_QUAT: printf("quat(%.9g, %.9g, %.9g, %.9g)", wf(v[0]), wf(v[1]), wf(v[2]), wf(v[3])); break;
    case T_PROJ: { /* the point's complex number (or inf) */
        double ar = wf(v[0]), ai = wf(v[1]), br = wf(v[2]), bi = wf(v[3]), d = br * br + bi * bi;
        if (isnan(ar) || isnan(ai) || isnan(d)) printf("nan");
        else if (d == 0) printf("%s", ar == 0 && ai == 0 ? "[0 : 0]" : "inf");
        else printf("%.9g%+.9gi", (ar * br + ai * bi) / d, (ai * br - ar * bi) / d);
        break;
    }
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
            float lift[4], c[3];
            viewer_pixel_to_lift(p, st, v, (float)w, (float)h, x + 0.5f, y + 0.5f, lift);
            I.pixel = viewer_pixel_size(p, st, v, (float)w, (float)h, x + 0.5f, y + 0.5f);
            interp_eval_viewer_lift(&I, v, lift, c);
            interp_free(&I);
            u8 *px = rgba + 4 * ((size_t)y * w + x);
            for (int k = 0; k < 3; k++) px[k] = (u8)(c[k] * 255.0f + 0.5f);
            px[3] = 255;
        }
    }
    free(st);
}

#define FS_HAVE_GFX 1

/* the GPU, without a window */
static bool cli_gfx_init(char *err, int errcap) {
    GfxInitDesc d = {0};
    d.validation = getenv("FS_VALIDATION") != NULL;
    if (!gfx_preinit_loader()) {
        const char *why = gfx_library_error();
        snprintf(err, (size_t)errcap, "could not load the Vulkan library (is a Vulkan driver installed?)%s%.400s",
                 why[0] ? ": " : "", why);
        return false;
    }
    return gfx_init(&d, err, errcap);
}

static bool render_gpu(Program *p, const u32 *state, Section *v, int w, int h, u8 *rgba, char *err, int errcap) {
#ifdef FS_HAVE_GFX
    u32 nwords;
    u32 *spv = codegen_viewer(p, v, &nwords, err, errcap);
    if (!spv) return false;
    if (!cli_gfx_init(err, errcap)) {
        free(spv);
        return false;
    }
    bool df = viewer_wants_df(p, state, v, w, h, false); /* deep zooms: double-single precision */
    if (df) app_init_exact_floats();
    gfx_set_globals(state, (u32)p->state_words * 4);
    bool ok;
    if (v->flood) {
        ok = flood_render_gpu_ex(p, state, v, df, w, h, rgba, NULL, err, errcap);
    } else {
        if (df) {
            free(spv);
            spv = codegen_viewer_ex(p, v, true, &nwords, err, errcap);
            if (!spv) {
                gfx_shutdown();
                return false;
            }
        }
        int vid = gfx_viewer_create(spv, nwords, err, errcap);
        ok = vid > 0 && gfx_render_to_memory(vid, w, h, 256, rgba, err, errcap);
        if (vid > 0) gfx_viewer_destroy(vid);
    }
    free(spv);
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
#ifdef __APPLE__
    /* MoltenVK: fast maths, except in the shaders that declare IEEE floats (deep zooms, see codegen.c) */
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "2", 0);
    /* and only its errors in the terminal (its release builds also print informational messages) */
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
    const char *mode = NULL, *arg = NULL, *script = NULL, *viewer = NULL, *tool = NULL, *at = NULL, *event = NULL;
    int w = 512, h = 512, fps = 30, samples = 0;
    bool cpu = false, sized = false;
    double seconds = 10, from = 0;
    const char *sets[32];
    int nsets = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { printf("%s", cli_usage); return 0; }
        if (!strcmp(a, "--version")) { printf("FractalStreamC %s\n", FS_VERSION); return 0; }
        if (!strcmp(a, "--builtins")) { printf("%s", prelude_source); return 0; }
        if (!strcmp(a, "--check") || !strcmp(a, "--state")) { mode = a; continue; }
        if ((!strcmp(a, "--spirv") || !strcmp(a, "--render") || !strcmp(a, "--video")) && i + 1 < argc) {
            mode = a;
            arg = argv[++i];
            continue;
        }
        if ((!strcmp(a, "--seconds") || !strcmp(a, "--from")) && i + 1 < argc) {
            char *end;
            double x = strtod(argv[++i], &end);
            if (*end || !(x == x) || (!strcmp(a, "--seconds") && !(x > 0 && x <= 36000))) {
                fprintf(stderr, "error: %s expects a number of seconds\n", a);
                return 2;
            }
            if (!strcmp(a, "--seconds")) seconds = x;
            else from = x;
            continue;
        }
        if ((!strcmp(a, "--fps") || !strcmp(a, "--samples")) && i + 1 < argc) {
            int n = atoi(argv[++i]);
            bool isfps = !strcmp(a, "--fps");
            if (isfps ? n < 1 || n > 240 : n < 1 || n > 16) {
                fprintf(stderr, "error: %s expects %s\n", a, isfps ? "1 to 240 frames per second" : "1 to 16 samples");
                return 2;
            }
            if (isfps) fps = n;
            else samples = n;
            continue;
        }
        if (!strcmp(a, "--viewer") && i + 1 < argc) { viewer = argv[++i]; continue; }
        if (!strcmp(a, "--tool") && i + 1 < argc) { mode = a; tool = argv[++i]; continue; }
        if (!strcmp(a, "--at") && i + 1 < argc) { at = argv[++i]; continue; }
        if (!strcmp(a, "--event") && i + 1 < argc) { event = argv[++i]; continue; }
        if (!strcmp(a, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 16384 || h > 16384) {
                fprintf(stderr, "error: --size expects WxH, like 800x600\n");
                return 2;
            }
            sized = true;
            continue;
        }
        if (!strcmp(a, "--cpu")) { cpu = true; continue; }
        if (!strcmp(a, "--set") && i + 1 < argc) { if (nsets < 32) sets[nsets++] = argv[++i]; continue; }
        if (!strncmp(a, "-psn_", 5)) continue; /* (macOS: the process serial number the Finder may pass to an app) */
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
    ViewsSaved sv; /* (a view of the sphere whose center or radius is set goes there) */
    views_save(p, state, &sv);
    for (int i = 0; i < nsets; i++)
        if (!cli_set_global(p, state, sets[i])) { rc = 2; goto done; }
    views_follow(p, &sv, state, true);
    if (nsets) { /* the state changed: @prepare runs again */
        char perr_[256];
        if (!program_prepare(p, state, perr_, sizeof perr_)) { fprintf(stderr, "error: in @prepare: %s\n", perr_); rc = 1; goto done; }
    }

    if (!strcmp(mode, "--check")) {
        printf("%s: ok (%d viewer%s, %d tool%s)\n", script, p->nviewers, p->nviewers == 1 ? "" : "s", p->ntools,
               p->ntools == 1 ? "" : "s");
    } else if (!strcmp(mode, "--state")) {
        if (p->load_output) cli_print_lines("print: ", p->load_output);
        if (p->nprepares) { /* (what @prepare prints: it runs again, on a copy) */
            u32 *st = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
            memcpy(st, state, sizeof(u32) * (size_t)p->state_words);
            StrBuf printed = {0};
            char perr_[256];
            program_prepare_ex(p, st, &printed, perr_, sizeof perr_);
            cli_print_lines("print: ", sb_cstr(&printed));
            sb_free(&printed);
            free(st);
        }
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
        if (!v) {
            if (viewer) fprintf(stderr, "error: no viewer named '%s'\n", viewer);
            else fprintf(stderr, "error: the script has no viewer\n");
            rc = 2;
            goto done;
        }
        u8 *rgba = (u8 *)xmalloc((size_t)w * h * 4);
        char err[512] = "";
        bool ok = true;
        double t0 = fs_time_seconds();
        if (cpu && v->flood) flood_render_cpu(p, state, v, w, h, rgba, NULL);
        else if (cpu) render_cpu(p, state, v, w, h, rgba);
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
    } else if (!strcmp(mode, "--video")) {
        Section *v = cli_find_viewer(p, viewer);
        if (!v) {
            if (viewer) fprintf(stderr, "error: no viewer named '%s'\n", viewer);
            else fprintf(stderr, "error: the script has no viewer\n");
            rc = 2;
            goto done;
        }
        if (!sized) {
            w = 1280;
            h = 720;
        }
        if ((w | h) & 1 || w > VIDEO_MAX_SIDE || h > VIDEO_MAX_SIDE) {
            fprintf(stderr, "error: a video is at most %dx%d, and its sides are even numbers\n", VIDEO_MAX_SIDE, VIDEO_MAX_SIDE);
            rc = 2;
            goto done;
        }
        if (!samples) samples = FS_CLAMP((i32)state[p->s_aa->slot], 1, 16);
        char err[512] = "";
        double t0 = fs_time_seconds(), shown = t0;
        VideoJob *j = cli_gfx_init(err, sizeof err)
                          ? video_begin(p, state, v, arg, w, h, fps, seconds, from, samples, err, sizeof err)
                          : NULL;
        int r = j ? 1 : -1;
        while (r == 1) {
            r = video_step(j);
            double now = fs_time_seconds();
            if (r == 1 && now - shown > 2) { /* (progress, every 2 seconds) */
                fprintf(stderr, "frame %d of %d\n", j->done, j->frames);
                shown = now;
            }
        }
        if (r < 0) {
            fprintf(stderr, "error: %s\n", j ? j->err : err);
            rc = 1;
        } else {
            long long bytes = 0;
            FILE *f = fopen(arg, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                bytes = ftell(f);
                fclose(f);
            }
            printf("wrote %s (%dx%d, %d frame%s at %d per second, %d sample%s per pixel, %.1f MB, %.1fs)\n", arg, w, h,
                   j->frames, j->frames == 1 ? "" : "s", fps, j->samples, j->samples == 1 ? "" : "s", bytes / 1e6,
                   fs_time_seconds() - t0);
        }
        video_end(j);
        gfx_shutdown();
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
        StrBuf printed = {0};
        I.draws = &draws;
        I.out = &printed;
        /* .pixel as if the tool's viewer were rendered at --size */
        Section *tv = t->viewer_of >= 0 ? p->viewers[t->viewer_of] : v;
        if (tv) I.pixel = viewer_pixel_size(p, state, tv, (float)w, (float)h, 0.5f * w, 0.5f * h);
        state[p->s_drag_start->slot] = fw(re);
        state[p->s_drag_start->slot + 1] = fw(im);
        views_save(p, state, &sv);
        if (!t->has_events) {
            if (event) fprintf(stderr, "warning: the tool has no 'on' blocks; --event is ignored\n");
            if (!interp_run_tool(&I, t, re, im)) { fprintf(stderr, "error: %s\n", I.error); rc = 1; }
        } else {
            /* one event, or a click: press, release, click */
            static const char *evn[TOOL_EV_COUNT] = {"press", "drag", "release", "click", "hover", "show", "hide"};
            int evs[3] = {TOOL_EV_PRESS, TOOL_EV_RELEASE, TOOL_EV_CLICK}, nev = 3;
            if (event) {
                nev = 0;
                for (int k = 0; k < TOOL_EV_COUNT; k++)
                    if (!strcmp(evn[k], event)) evs[nev++] = k;
                if (!nev) { fprintf(stderr, "error: unknown event '%s'\n", event); rc = 2; goto done; }
            }
            for (int k = 0; k < nev && !rc; k++) {
                if (!tool_runs_on(t, evs[k])) continue;
                if (!interp_run_tool_event(&I, t, evs[k], re, im)) { fprintf(stderr, "error: %s\n", I.error); rc = 1; }
            }
        }
        views_follow(p, &sv, state, false);
        static const char *shapes[] = {"point", "circle", "disk", "line", "text"};
        for (int k = 0; k < draws.len; k++) {
            DrawItem *d = &draws.data[k];
            printf("draw %s at %g%+gi", shapes[d->shape], d->p0[0], d->p0[1]);
            if (d->shape == DRAW_LINE) printf(" to %g%+gi", d->p1[0], d->p1[1]);
            if (d->shape == DRAW_TEXT) printf(" \"%s\"", d->text);
            if (d->shape == DRAW_CIRCLE || d->shape == DRAW_DISK) printf(" radius %g%s", d->radius, d->radius_px ? " px" : "");
            printf(" color [%g, %g, %g] width %g px", d->color[0], d->color[1], d->color[2], d->width);
            if (d->viewer >= 0) printf(" in %s", p->viewers[d->viewer]->name);
            printf("\n");
        }
        cli_print_lines("print: ", sb_cstr(&printed));
        sb_free(&printed);
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
