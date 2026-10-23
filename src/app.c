/* app.c - the GUI: viewers in tiles (each with its own tool strip), the
 * script editor and the config in collapsible side panes, menus, and
 * the drawings made by tools.
 *
 * Frame: poll input -> lay out panes and tiles -> pane splitters -> mouse
 * interaction with viewers (pan, zoom, tools) -> draw tiles and their
 * chrome, panes, overlays -> keyboard shortcuts -> GPU: upload the state,
 * (re)render the viewers whose inputs changed (in bands, or by flood fill
 * for viewers with a `continuing` solve), draw. */
#include "app.h"
#include <sys/stat.h>
#ifndef _MSC_VER
#include <strings.h>
#else
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
#if FS_OS_MACOS
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#define MAX_TILES 12
#define MAX_NODES (2 * MAX_TILES)
#define MAX_FLOATS 4

#if FS_OS_MACOS
#define MODKEY "Cmd" /* the name of UI_MOD_MOD in labels */
#define BACK_KEY "Cmd+[" /* Back / Forward, as the system's programs have them */
#define FORWARD_KEY "Cmd+]"
#else
#define MODKEY "Ctrl"
#define BACK_KEY "Alt+Left"
#define FORWARD_KEY "Alt+Right"
#endif

enum { TOOL_INSPECT = -3, TOOL_PAN = -2, TOOL_PICK = -1 };
enum { N_FREE, N_LEAF, N_HSPLIT, N_VSPLIT }; /* HSPLIT: a | b, VSPLIT: a over b */
enum { DRAG_NONE, DRAG_PAN, DRAG_TOOL, DRAG_PICK, DRAG_CLICK, DRAG_SPLIT, DRAG_VIEWER, DRAG_INSPECT };
enum { DROP_NONE, DROP_REPLACE, DROP_LEFT, DROP_RIGHT, DROP_TOP, DROP_BOTTOM, DROP_FLOAT };

typedef struct Rect { float x, y, w, h; } Rect;
/* a node of the layout tree: a leaf shows a viewer; a split shows a and b side by side (ratio: the share of a; < 0
   until the next layout, which splits the part of the node not under a pane in two) */
typedef struct LNode { int kind; int a, b; float ratio; int viewer; Rect r; } LNode;
/* a tile on screen: the viewer, its rectangle, its leaf (or float corner); vr is the part of r not under a pane,
   where its labels go */
typedef struct VisTile { int viewer; Rect r; int node; int fl; Rect vr; } VisTile;

/* where a view looks: Back / Forward go through the places a view has been */
typedef struct ViewPlace {
    double cx, cy;     /* the centre, with its low part */
    float radius;
    float mob[8];      /* projective views: the Möbius map */
} ViewPlace;

#define VIEW_HISTORY 64
typedef struct ViewHistory {
    ViewPlace back[VIEW_HISTORY], fwd[VIEW_HISTORY];
    int nback, nfwd;
    int gesture;       /* the navigation going on (NAV_*, or a number per drag): a new one remembers the place */
    double time;       /* when it last moved the view */
} ViewHistory;

typedef struct ViewerRT {
    int pipeline;      /* fragment shader (viewers without a `continuing` solve) */
    FloodGpu flood;    /* compute shaders (viewers with one) */
    bool is_flood;
    /* deep zooms: the same in double-single precision, made when the view gets that deep */
    bool df;           /* rendering in double-single precision */
    bool df_failed;    /* the GPU rejected that version: stay in single precision */
    int pipeline_df;
    FloodGpu flood_df;
    u32 center_bits[2]; /* the centre when its low part was set (a centre set elsewhere drops the low part) */
    int target, tw, th;
    u32 *snap;         /* state the image was rendered from */
    bool *used;        /* per global: does the viewer read it? */
    bool animated;
    bool rendered;
    int next_row;      /* progressive rendering: next row to render (cycles through the image) */
    int rows_left;     /* rows still to render with the latest state; 0 when the image is up to date */
    bool picked;       /* show the picked-point marker */
    int tool;          /* this viewer's selected tool: TOOL_PAN, TOOL_PICK or a script tool */
    u32 *show_snap;    /* the state when the selected tool last ran `on show` (NULL: run it) */
    bool flood_checked; /* flood viewers: the start pixel's result was looked at */
    ViewHistory *hist;  /* Back / Forward (made on the first move; kept when the script runs again) */
    bool grid;          /* show the axes and a grid with their values (flat views) */
    bool gone;          /* its section is gone from the script, which has errors (so this program still runs): its
                           tile is closed, and comes back if the section does (see mark_gone) */
} ViewerRT;

typedef struct App {
    Program *prog;
    u32 *state, *uploaded;
    Interp interp;
    ViewerRT *vrt;
    DrawItemVec *draws; /* per user tool */
    /* the Info pane: what `print` wrote, per source (the tools, then OUT_PREPARE, OUT_SCRIPT, OUT_INSPECT) */
    StrBuf *outs;
    int *out_serial;        /* bumped when a source's text changes */
    int out_seen;           /* the total of the serials when the pane was last open */
    bool info_open;
    bool info_auto;         /* the pane opened by itself (once per run of the script) */
    float info_frac, info_scroll;
    int inspect_viewer;     /* the point the Inspect tool looked at (-1 none) */
    float inspect_at[2];
    LNode nodes[MAX_NODES];
    int root;
    int floats[MAX_FLOATS]; /* viewer shown in each corner (-1 none): TL TR BL BR */
    int focus_viewer;
    VisTile vis[MAX_TILES + MAX_FLOATS];
    int nvis;
    Rect area;              /* where the tiles are: between the panes */
    Rect free;              /* the part of the area no pane covers (all of it: the panes are beside the views) */
    int band_rows;
    int flood_passes;       /* flood-fill passes per frame (adapts to the GPU) */
    /* the bars at the window's edges, and the panes next to them */
    bool editor_open, config_open;
    float editor_frac, config_frac;
    Rect lbar_r, rbar_r;    /* the bars (always there) */
    Rect editor_r, config_r; /* the panes (width 0 when closed) */
    float lbar_scroll, rbar_scroll; /* (a bar scrolls when the window is too short for it) */
    /* editor */
    Editor *ed;
    char path[1024];
    u32 compiled_version;
    int script_errors;      /* errors in the last compile */
    double edit_time;
    bool live;
    long long file_mtime;
    double next_file_check;
    u32 *prepare_snap;      /* the state after @prepare last ran (NULL: run it) */
    /* deferred to the start of the next frame (see process_requests) */
    bool req_run, req_reload, req_new;
    char req_new_path[1024]; /* a new script from this file ("": the welcome script) */
    char req_open[1024];
    char req_export[1024];  /* save a picture there */
    int export_viewer, export_scale, export_wait;
    /* overlays */
    bool ref_open;          /* the language reference (refdoc.c) */
    float ui_zoom;          /* the size of everything (Cmd+= / Cmd+- / Cmd+0; kept in the settings folder) */
    bool help_open;
    /* find / replace in the script */
    bool find_open, replace_open, find_focus, find_case, editor_grab;
    char find_buf[256], replace_buf[256];
    float config_scroll;
    const char *choice_open;  /* config choice whose list is open (points to choice_name), NULL none */
    char choice_name[128];
    /* a value edited as text in the config panel (a list, a point): its name and the text typed, from the moment its
       field gets the focus until Enter, or until the focus leaves it; then text_apply reads the text into the value */
    char text_name[128];
    char text_buf[1024];
    void (*text_apply)(Sym *s, const char *text);
    /* the formula being edited in the config pane: its text, and where it is in the script */
    char formula_buf[512];
    int formula_start, formula_end;
    int modal;              /* 0, 1 save as, 2 open, 3 save a picture, 4 new from a template, 5 unsaved changes */
    int pending;            /* what waits for the answer about unsaved changes (PENDING_*) */
    char pending_path[1024];
    bool discard_ok;        /* the changes may go: once, for the next open / new / quit */
    bool quit_now;          /* (asked: quit)*/
    bool modal_focus, modal_select_all;
    bool modal_confirm;     /* saving over a file: the next OK replaces it */
    char modal_buf[1024];
    char browse_dir[1024];  /* the folder listed in the dialog, and its entries */
    FsDirEntry *browse;
    int nbrowse;
    float browse_scroll;
    bool browse_recent;     /* the dialog lists the recent files instead */
    char recent[12][1024];  /* scripts opened or saved lately, the latest first (kept in the settings folder) */
    int nrecent;
    /* interaction */
    int drag;
    int drag_viewer, drag_node, drag_tool;
    float press_x, press_y, last_mx, last_my;
    bool moved;
    int nav_serial;         /* numbers navigation gestures (each drag, reset...) for the view history */
    int drag_gesture;       /* the number of the drag going on */
    int hover_viewer;       /* viewer under the mouse (nothing above it) */
    int hover_tile;         /* index in vis[] of the tile under the mouse, by geometry only (-1 none) */
    /* status line */
    char status[512];
    bool status_err;
    double status_until;
    double t0;
    UiInput in;
} App;

static App A;

static const char *welcome_script =
    "# Welcome to FractalStream!\n"
    "#\n"
    "# This is a script: edit it and the pictures follow as\n"
    "# soon as there are no errors. Ctrl+Enter runs it,\n"
    "# Ctrl+S saves it, F1 shows all the keys (Cmd instead\n"
    "# of Ctrl on a Mac).\n"
    "#\n"
    "# The bar on the left has a numbered button for each\n"
    "# view, with the view's tools below it (drag a number\n"
    "# onto the pictures to split or float that view); the\n"
    "# bar on the right has the config and what you can do\n"
    "# with the view you clicked last. Try \"Pick point\" in\n"
    "# the parameter plane: the Julia set follows the point.\n"
    "# The language is described in docs/language.md (F2\n"
    "# shows it here).\n"
    "\n"
    "@config\n"
    ".title = \"Mandelbrot and Julia sets\"\n"
    ".maxIterations = 200\n"
    "mandel.center = -0.5\n"
    "mandel.radius = 1.4\n"
    "julia.radius = 1.6\n"
    "\n"
    "@global\n"
    ".c = -0.8 + 0.156i\n"
    "define shade(n : Z) = hsv(n / 40 + 0.6, 0.7, 1)\n"
    "\n"
    "@viewer mandel c \"Parameter plane\" \"Ctrl + 1\"\n"
    "z := 0\n"
    "while z not escapes do\n"
    "    z = z^2 + c\n"
    "end\n"
    "if .stuck then\n"
    "    color = .black\n"
    "else\n"
    "    color = shade(.iterations)\n"
    "end\n"
    "\n"
    "@viewer julia z \"Julia set\" \"Ctrl + 2\"\n"
    "while z not escapes do\n"
    "    z = z^2 + .c\n"
    "end\n"
    "if .stuck then\n"
    "    color = .black\n"
    "else\n"
    "    color = shade(.iterations)\n"
    "end\n"
    "\n"
    "# a tool of the julia viewer: z is the point you click\n"
    "@julia.tool \"Orbit\" \"O\" \"timeline\"\n"
    "w := z\n"
    "for k in 1..100 do\n"
    "    w2 := w^2 + .c\n"
    "    draw line between w and w2 with color .yellow\n"
    "    w = w2\n"
    "end\n";

/* ---- exact floats for deep zooms ------------------------------------------------------ */
/* the double-single arithmetic deep zooms need, computed on the GPU: white if every operation is exact */
static const char *df_check_src =
    "@viewer v c\n" /* (typed locals: not folded into constants by the compiler) */
    "a : R = 1\n"
    "b : R = 0.0000000001\n"
    "add := ((a + b) - a) * 1e10\n"
    "x : R = 1 + 1 / 1048576\n"
    "mul := (x * x - 1 - 2 / 1048576) * 1099511627776.0\n"
    "three : R = 3\n"
    "q := a / three\n"
    "div := (q * three - a) * 1e13 + 0.5\n"
    "two : R = 2\n"
    "r := sqrt(two)\n"
    "sq := (r * r - two) * 1e12 + 0.5\n"
    "tiny : R = 0.0000001\n"
    "z := complex(a, tiny)\n"
    "zz := z * z\n"
    "cm := (re(zz) - a) * -1e14\n"
    "color = [add, mul, if |div - 0.5| < 0.1 and |sq - 0.5| < 0.1 and |cm - 1| < 0.01 then 1 else 0]\n";

static bool df_check_gpu(void) {
    char err[256] = "";
    Program *p = program_load(df_check_src, "df check");
    bool ok = false;
    if (p->nerrors == 0) {
        gfx_set_globals(p->initial_state, (u32)p->state_words * 4);
        u32 n;
        u32 *words = codegen_viewer_ex(p, p->viewers[0], true, &n, err, sizeof err);
        int vid = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
        free(words);
        if (vid > 0) {
            u8 px[16] = {0};
            bool rendered = gfx_render_to_memory(vid, 2, 2, 0, px, err, sizeof err);
            ok = rendered && px[0] == 255 && px[1] == 255 && px[2] == 255;
            if (getenv("FS_DEBUG")) {
                if (rendered) fprintf(stderr, "df check: %d %d %d\n", px[0], px[1], px[2]);
                else fprintf(stderr, "df check: %s\n", err);
            }
            gfx_viewer_destroy(vid);
        }
    }
    program_free(p);
    return ok;
}

bool app_init_exact_floats(void) {
    codegen_set_float_controls(false);
    if (!gfx_has_float_controls()) return false;
    codegen_set_float_controls(true);
    if (df_check_gpu()) return true;
    codegen_set_float_controls(false); /* (the GPU doesn't keep them exact: each operation is marked instead) */
    if (getenv("FS_DEBUG")) fprintf(stderr, "deep zooms: IEEE floats per shader don't check out; marking each operation\n");
    return false;
}

/* ---- small helpers ----------------------------------------------------------- */
static float px(float v) { return ui_px(v); }
static bool in_rect(Rect r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
/* the common part of two rectangles (width or height 0 if none) */
static Rect rect_and(Rect a, Rect b) {
    float x0 = FS_MAX(a.x, b.x), y0 = FS_MAX(a.y, b.y), x1 = FS_MIN(a.x + a.w, b.x + b.w), y1 = FS_MIN(a.y + a.h, b.y + b.h);
    Rect r = {x0, y0, FS_MAX(x1 - x0, 0.0f), FS_MAX(y1 - y0, 0.0f)};
    return r;
}

static void set_status(bool err, const char *fmt, ...) FS_PRINTF(2, 3);
static void set_status(bool err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.status, sizeof A.status, fmt, ap);
    va_end(ap);
    A.status_err = err;
    A.status_until = fs_time_seconds() + (err ? 8.0 : 3.0);
}

static long long file_mtime(const char *path) {
    struct stat st;
    if (!path[0] || stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
}

/* ---- Riemann sphere views (projective viewers) --------------------------------
 * A view is a Mobius map M (2x2 complex matrix, 8 floats a b c d) from the camera's
 * sphere coordinate w to the plane: z = (a w + b) / (c w + d).  See interp.c. */
typedef struct Cx { float re, im; } Cx;
static Cx cx(float re, float im) { Cx z = {re, im}; return z; }
static Cx cx_mul(Cx a, Cx b) { return cx(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static Cx cx_add(Cx a, Cx b) { return cx(a.re + b.re, a.im + b.im); }
static Cx cx_div(Cx a, Cx b) {
    float d = b.re * b.re + b.im * b.im;
    return cx((a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d);
}
typedef struct Mob { Cx a, b, c, d; } Mob;
static Mob mob_mul(Mob x, Mob y) {
    Mob r = {cx_add(cx_mul(x.a, y.a), cx_mul(x.b, y.c)), cx_add(cx_mul(x.a, y.b), cx_mul(x.b, y.d)),
             cx_add(cx_mul(x.c, y.a), cx_mul(x.d, y.c)), cx_add(cx_mul(x.c, y.b), cx_mul(x.d, y.d))};
    return r;
}
static Mob mob_inv(Mob x) {
    Mob r = {x.d, cx(-x.b.re, -x.b.im), cx(-x.c.re, -x.c.im), x.a};
    return r;
}
static Cx mob_apply(Mob m, Cx w) { return cx_div(cx_add(cx_mul(m.a, w), m.b), cx_add(cx_mul(m.c, w), m.d)); }
/* the sphere rotation taking p to 0 */
static Mob mob_to_zero(Cx p) {
    Mob r = {cx(1, 0), cx(-p.re, -p.im), cx(p.re, -p.im), cx(1, 0)};
    return r;
}
static Mob mob_get(Sym *s) {
    float m[8];
    for (int k = 0; k < 8; k++) m[k] = wf(A.state[s->slot + k]);
    Mob r = {cx(m[0], m[1]), cx(m[2], m[3]), cx(m[4], m[5]), cx(m[6], m[7])};
    return r;
}
static void mob_set(Sym *s, Mob x) {
    float n = 0;
    Cx e[4] = {x.a, x.b, x.c, x.d};
    for (int k = 0; k < 4; k++) n += e[k].re * e[k].re + e[k].im * e[k].im;
    n = 1.0f / sqrtf(n * 0.5f); /* keep the entries of moderate size (the map is unchanged by scaling) */
    for (int k = 0; k < 4; k++) {
        A.state[s->slot + 2 * k] = fw(e[k].re * n);
        A.state[s->slot + 2 * k + 1] = fw(e[k].im * n);
    }
}
#define APP_PROJ_T 1.19175359f
static Cx screen_to_w(Rect r, float x, float y) {
    float m = 0.5f * FS_MIN(r.w, r.h);
    float sx = (x - r.x - 0.5f * r.w) / m * APP_PROJ_T, sy = (0.5f * r.h - (y - r.y)) / m * APP_PROJ_T;
    float n = 1.0f / sqrtf(sx * sx + sy * sy + 1.0f);
    return cx(sx * n / (1 + n), sy * n / (1 + n));
}
static bool w_to_screen(Rect r, Cx w, float *x, float *y) {
    float s2 = w.re * w.re + w.im * w.im;
    if (!(s2 < 1.0f)) return false; /* behind the camera */
    float X = 2 * w.re / (1 + s2), Y = 2 * w.im / (1 + s2), Z = (s2 - 1) / (s2 + 1);
    float m = 0.5f * FS_MIN(r.w, r.h);
    *x = r.x + 0.5f * r.w + X / -Z / APP_PROJ_T * m;
    *y = r.y + 0.5f * r.h - Y / -Z / APP_PROJ_T * m;
    return true;
}

static void plane_of(int v, Rect r, float x, float y, float *re, float *im) {
    viewer_pixel_to_plane(A.prog, A.state, A.prog->viewers[v], r.w, r.h, x - r.x, y - r.y, re, im);
}

static void pixel_of(int v, Rect r, float re, float im, float *x, float *y) {
    Section *s = A.prog->viewers[v];
    if (s->mobius) {
        if (!w_to_screen(r, mob_apply(mob_inv(mob_get(s->mobius)), cx(re, im)), x, y)) *x = *y = 1e9f;
        return;
    }
    float cx = wf(A.state[s->center->slot]), cy = wf(A.state[s->center->slot + 1]);
    float scale = wf(A.state[s->radius->slot]) / (0.5f * FS_MIN(r.w, r.h));
    *x = r.x + 0.5f * r.w + (re - cx) / scale;
    *y = r.y + 0.5f * r.h - (im - cy) / scale;
}

static float view_scale(int v, Rect r) {
    Section *s = A.prog->viewers[v];
    return wf(A.state[s->radius->slot]) / (0.5f * FS_MIN(r.w, r.h));
}

static void set_c(Sym *s, float re, float im) {
    A.state[s->slot] = fw(re);
    A.state[s->slot + 1] = fw(im);
}

/* Deep zooms: the centre of a flat view, to about twice the precision of a float, is
 * NAME.center + NAME.center lo (the low part is kept by the app's panning and zooming). */
static void view_center_d(int v, double *cx, double *cy) {
    Section *s = A.prog->viewers[v];
    *cx = wf(A.state[s->center->slot]);
    *cy = wf(A.state[s->center->slot + 1]);
    if (s->center_lo) {
        *cx += wf(A.state[s->center_lo->slot]);
        *cy += wf(A.state[s->center_lo->slot + 1]);
    }
}

static void set_view_center_d(int v, double cx, double cy) {
    Section *s = A.prog->viewers[v];
    float hx = (float)cx, hy = (float)cy;
    set_c(s->center, hx, hy);
    if (!s->center_lo) return;
    set_c(s->center_lo, (float)(cx - hx), (float)(cy - hy));
    A.vrt[v].center_bits[0] = A.state[s->center->slot];
    A.vrt[v].center_bits[1] = A.state[s->center->slot + 1];
}

/* a centre set by something else (the config panel, a tool, @prepare) has no low part */
static void check_center_lo(int v) {
    Section *s = A.prog->viewers[v];
    if (!s->center_lo) return;
    u32 *lo = A.state + s->center_lo->slot;
    if ((lo[0] || lo[1]) && (A.state[s->center->slot] != A.vrt[v].center_bits[0] ||
                             A.state[s->center->slot + 1] != A.vrt[v].center_bits[1]))
        lo[0] = lo[1] = 0;
}

/* the plane point at screen position (x, y) of a flat view, in double precision */
static void plane_of_d(int v, Rect r, double x, double y, double *re, double *im) {
    double cx, cy;
    view_center_d(v, &cx, &cy);
    double sc = (double)wf(A.state[A.prog->viewers[v]->radius->slot]) / (0.5 * FS_MIN(r.w, r.h));
    *re = cx + (x - r.x - 0.5 * r.w) * sc;
    *im = cy + (0.5 * r.h - (y - r.y)) * sc;
}

/* ---- view history: Back / Forward ------------------------------------------------------ */
/* gestures: the wheel, the config panel and tools count as one gesture until they pause; each drag, reset or
   pasted location is a gesture of its own (NAV_SERIAL + a number) */
enum { NAV_WHEEL = 1, NAV_PANEL, NAV_TOOL, NAV_SERIAL = 16 };

static ViewPlace view_place(int v) {
    Section *s = A.prog->viewers[v];
    ViewPlace pl;
    memset(&pl, 0, sizeof pl);
    view_center_d(v, &pl.cx, &pl.cy);
    pl.radius = wf(A.state[s->radius->slot]);
    if (s->mobius)
        for (int k = 0; k < 8; k++) pl.mob[k] = wf(A.state[s->mobius->slot + k]);
    return pl;
}

static void set_view_place(int v, const ViewPlace *pl) {
    Section *s = A.prog->viewers[v];
    set_view_center_d(v, pl->cx, pl->cy);
    A.state[s->radius->slot] = fw(pl->radius);
    if (s->mobius)
        for (int k = 0; k < 8; k++) A.state[s->mobius->slot + k] = fw(pl->mob[k]);
}

static bool same_place(const ViewPlace *a, const ViewPlace *b) {
    if (a->cx != b->cx || a->cy != b->cy || a->radius != b->radius) return false;
    for (int k = 0; k < 8; k++)
        if (a->mob[k] != b->mob[k]) return false;
    return true;
}

static ViewHistory *view_history(int v) {
    ViewerRT *rt = &A.vrt[v];
    if (!rt->hist) rt->hist = (ViewHistory *)xcalloc(1, sizeof(ViewHistory));
    return rt->hist;
}

static void place_push(ViewPlace *stack, int *n, const ViewPlace *pl) {
    if (*n == VIEW_HISTORY) { /* forget the oldest */
        memmove(stack, stack + 1, sizeof *stack * (VIEW_HISTORY - 1));
        (*n)--;
    }
    stack[(*n)++] = *pl;
}

/* View v is about to move (or has just moved away from `was`): a new gesture remembers where it was, for Back */
static void nav_note(int v, int gesture, const ViewPlace *was) {
    ViewHistory *h = view_history(v);
    double now = fs_time_seconds();
    bool fresh = gesture != h->gesture || (gesture < NAV_SERIAL && now - h->time > 0.6);
    h->gesture = gesture;
    h->time = now;
    if (!fresh) return;
    ViewPlace cur = was ? *was : view_place(v);
    h->nfwd = 0;
    if (h->nback && same_place(&h->back[h->nback - 1], &cur)) return;
    place_push(h->back, &h->nback, &cur);
}

static void nav_begin(int v, int gesture) { nav_note(v, gesture, NULL); }
static int nav_new_gesture(void) { return NAV_SERIAL + (++A.nav_serial & 0xfffffff); }

/* the places Back (or Forward) can go to: the remembered ones that aren't where the view is now */
static int nav_count(int v, bool forward) {
    ViewHistory *h = A.vrt[v].hist;
    if (!h) return 0;
    ViewPlace cur = view_place(v);
    ViewPlace *from = forward ? h->fwd : h->back;
    int n = forward ? h->nfwd : h->nback;
    while (n > 0 && same_place(&from[n - 1], &cur)) n--;
    return n;
}

/* Back (or Forward): false when there is nowhere to go */
static bool nav_go(int v, bool forward) {
    if (!nav_count(v, forward)) return false;
    ViewHistory *h = A.vrt[v].hist;
    ViewPlace cur = view_place(v);
    ViewPlace *from = forward ? h->fwd : h->back, *to = forward ? h->back : h->fwd;
    int *nfrom = forward ? &h->nfwd : &h->nback, *nto = forward ? &h->nback : &h->nfwd;
    while (same_place(&from[*nfrom - 1], &cur)) (*nfrom)--;
    place_push(to, nto, &cur);
    set_view_place(v, &from[--*nfrom]);
    h->gesture = 0; /* what comes next is a new gesture */
    return true;
}

/* Back / Forward from the keyboard or the mouse's side buttons */
static void nav_key(int v, bool forward) {
    if (nav_go(v, forward)) return;
    Section *s = A.prog->viewers[v];
    set_status(false, "%s: no %s place to go to", s->display ? s->display : s->name, forward ? "later" : "earlier");
}

/* ---- the location of a view as text (Copy / Paste location) ------------------------------- */
/* the shortest text that reads back as the same float */
static void float_text(char *out, int cap, float f) {
    for (int d = 6; d <= 9; d++) {
        snprintf(out, (size_t)cap, "%.*g", d, (double)f);
        if ((float)strtod(out, NULL) == f) return;
    }
}

/* a coordinate of a view's centre, with the digits that matter at this radius */
static void coord_text(char *out, int cap, double x, double radius, bool deep) {
    if (!deep || !(radius > 0)) {
        float_text(out, cap, (float)x);
        return;
    }
    int digits = (int)ceil(log10(FS_MAX(fabs(x), radius) / radius)) + 5;
    snprintf(out, (size_t)cap, "%.*g", FS_CLAMP(digits, 8, 15), x); /* (about the precision of the deep views) */
}

/* the centre and radius of a flat view, as written in a script:
   "mandel.center = -0.74364388703 + 0.13182590421i\nmandel.radius = 1e-08\n" */
static void location_text(int v, char *out, int cap) {
    Section *s = A.prog->viewers[v];
    double cx, cy;
    view_center_d(v, &cx, &cy);
    float r = wf(A.state[s->radius->slot]);
    bool deep = r < 1e-3 * FS_MAX(hypot(cx, cy), 1.0);
    char re[48], im[48], rs[48];
    coord_text(re, sizeof re, cx, r, deep);
    coord_text(im, sizeof im, fabs(cy), r, deep);
    float_text(rs, sizeof rs, r);
    snprintf(out, (size_t)cap, "%s.center = %s %c %si\n%s.radius = %s\n", s->name, re, cy < 0 ? '-' : '+', im, s->name, rs);
}

/* the value after `key` and `=` in `text`, up to the end of the line, without spaces */
static bool location_value(const char *text, const char *key, char *out, int cap) {
    for (const char *p = strstr(text, key); p; p = strstr(p + 1, key)) {
        const char *q = p + strlen(key);
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        int n = 0;
        for (q++; *q && *q != '\n' && *q != '\r' && *q != '#' && n < cap - 1; q++)
            if (*q != ' ' && *q != '\t') out[n++] = *q;
        out[n] = 0;
        return n > 0;
    }
    return false;
}

/* Paste location: the centre and the radius in the clipboard (as Copy location writes them, or just a point) */
static bool paste_location(int v) {
    Section *s = A.prog->viewers[v];
    const char *clip = platform_get_clipboard();
    if (!clip) clip = "";
    char buf[256];
    double re = 0, im = 0, r = 0;
    bool has_c = false, has_r = false;
    if (location_value(clip, "center", buf, sizeof buf)) {
        has_c = parse_complex_literal_d(buf, &re, &im);
    } else { /* the whole text: a point */
        int n = 0;
        for (const char *q = clip; *q && n < (int)sizeof buf - 1; q++)
            if (!isspace((unsigned char)*q)) buf[n++] = *q;
        buf[n] = 0;
        has_c = n > 0 && parse_complex_literal_d(buf, &re, &im);
    }
    has_c = has_c && isfinite(re) && isfinite(im);
    if (location_value(clip, "radius", buf, sizeof buf)) {
        char *end;
        r = strtod(buf, &end);
        has_r = end != buf && *end == 0 && r > 0 && r < 1e30;
    }
    if (!has_c && !has_r) {
        set_status(true, "The clipboard has no location for %s (like \"%s.center = -0.5 + 0.1i\" and \"%s.radius = 2\")",
                   s->name, s->name, s->name);
        return false;
    }
    nav_begin(v, nav_new_gesture());
    if (has_c) set_view_center_d(v, re, im);
    if (has_r) A.state[s->radius->slot] = fw((float)r);
    set_status(false, "%s: moved to the %s in the clipboard", s->display ? s->display : s->name,
               has_c && has_r ? "location" : has_c ? "point" : "radius");
    return true;
}

static void copy_location(int v) {
    char text[256];
    location_text(v, text, sizeof text);
    platform_set_clipboard(text);
    Section *s = A.prog->viewers[v];
    set_status(false, "Copied the location of %s (paste it in @config, or in another session with Paste location)",
               s->display ? s->display : s->name);
}

/* ---- shortcuts -------------------------------------------------------------- */
typedef struct Shortcut { UiKey key; int mods; } Shortcut;

static Shortcut parse_shortcut(const char *s) {
    Shortcut sc = {UI_KEY_NONE, 0};
    if (!s) return sc;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    char *tok = buf;
    while (*tok) {
        while (*tok == ' ' || *tok == '+') tok++;
        char *end = tok;
        while (*end && *end != '+') end++;
        char save = *end;
        *end = 0;
        int n = (int)strlen(tok);
        while (n > 0 && tok[n - 1] == ' ') tok[--n] = 0;
        if (n == 0) break;
        if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control") || !strcasecmp(tok, "cmd") || !strcasecmp(tok, "mod"))
            sc.mods |= UI_MOD_MOD;
        else if (!strcasecmp(tok, "shift")) sc.mods |= UI_MOD_SHIFT;
        else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "option")) sc.mods |= UI_MOD_ALT;
        else if (n == 1) sc.key = (UiKey)(tok[0] >= 'a' && tok[0] <= 'z' ? tok[0] - 32 : tok[0]);
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 12)
            sc.key = (UiKey)(UI_KEY_F1 + atoi(tok + 1) - 1);
        else if (!strcasecmp(tok, "space")) sc.key = UI_KEY_SPACE;
        else if (!strcasecmp(tok, "home")) sc.key = UI_KEY_HOME;
        else if (!strcasecmp(tok, "end")) sc.key = UI_KEY_END;
        else if (!strcasecmp(tok, "tab")) sc.key = UI_KEY_TAB;
        if (!save) break;
        tok = end + 1;
    }
    return sc;
}

static Shortcut viewer_shortcut(int v) {
    Section *s = A.prog->viewers[v];
    if (s->shortcut) return parse_shortcut(s->shortcut);
    Shortcut sc = {v < 9 ? (UiKey)('1' + v) : UI_KEY_NONE, UI_MOD_MOD};
    return sc;
}

static void shortcut_text(Shortcut sc, char *out, int cap) {
    out[0] = 0;
    if (sc.key == UI_KEY_NONE) return;
    char k[16];
    if (sc.key >= UI_KEY_F1 && sc.key <= UI_KEY_F12) snprintf(k, sizeof k, "F%d", sc.key - UI_KEY_F1 + 1);
    else if (sc.key == UI_KEY_SPACE) snprintf(k, sizeof k, "Space");
    else if (sc.key == UI_KEY_SLASH && sc.mods == UI_MOD_SHIFT) { /* (Shift+/ is ? on most keyboards) */
        snprintf(out, (size_t)cap, "?");
        return;
    }
    else if (sc.key < 128) snprintf(k, sizeof k, "%c", (char)sc.key);
    else snprintf(k, sizeof k, "?");
    const char *mod = MODKEY "+";
    snprintf(out, (size_t)cap, "%s%s%s%s", sc.mods & UI_MOD_MOD ? mod : "", sc.mods & UI_MOD_SHIFT ? "Shift+" : "",
             sc.mods & UI_MOD_ALT ? "Alt+" : "", k);
}

/* ---- layout tree ------------------------------------------------------------ */
static int node_alloc(void) {
    for (int i = 0; i < MAX_NODES; i++)
        if (A.nodes[i].kind == N_FREE) {
            memset(&A.nodes[i], 0, sizeof A.nodes[i]);
            return i;
        }
    return -1;
}

/* The tree of tiles has its root at A.root, -1 when nothing is shown (a script without views).  A leaf whose
   viewer is gone (see ViewerRT.gone) stays in the tree but takes no room: its neighbour has it until the
   script runs again. */
static bool leaf_gone(int n) {
    int v = A.nodes[n].viewer;
    return A.nodes[n].kind == N_LEAF && A.prog && A.vrt && v >= 0 && v < A.prog->nviewers && A.vrt[v].gone;
}

/* the layout: viewer alone (-1: nothing at all), no floating views */
static void layout_reset(int viewer) {
    memset(A.nodes, 0, sizeof A.nodes);
    for (int i = 0; i < MAX_FLOATS; i++) A.floats[i] = -1;
    A.root = viewer >= 0 ? 0 : -1;
    A.nodes[0].kind = viewer >= 0 ? N_LEAF : N_FREE;
    A.nodes[0].viewer = viewer;
    A.focus_viewer = viewer;
}

static int count_leaves(int n) {
    if (n < 0) return 0;
    if (A.nodes[n].kind == N_LEAF) return 1;
    return count_leaves(A.nodes[n].a) + count_leaves(A.nodes[n].b);
}

/* the leaves that take room (not gone) */
static int count_shown(int n) {
    if (n < 0) return 0;
    if (A.nodes[n].kind == N_LEAF) return leaf_gone(n) ? 0 : 1;
    return count_shown(A.nodes[n].a) + count_shown(A.nodes[n].b);
}

static int find_leaf(int n, int viewer) {
    if (n < 0) return -1;
    if (A.nodes[n].kind == N_LEAF) return A.nodes[n].viewer == viewer ? n : -1;
    int r = find_leaf(A.nodes[n].a, viewer);
    return r >= 0 ? r : find_leaf(A.nodes[n].b, viewer);
}

static int find_parent(int n, int child) {
    if (n < 0 || A.nodes[n].kind == N_LEAF) return -1;
    if (A.nodes[n].a == child || A.nodes[n].b == child) return n;
    int r = find_parent(A.nodes[n].a, child);
    return r >= 0 ? r : find_parent(A.nodes[n].b, child);
}

/* the leaves under n, in order (left to right, top to bottom); returns how many */
static int collect_leaves(int n, int *out, int k) {
    if (n < 0) return k;
    if (A.nodes[n].kind == N_LEAF) {
        if (k < MAX_NODES) out[k++] = n;
        return k;
    }
    k = collect_leaves(A.nodes[n].a, out, k);
    return collect_leaves(A.nodes[n].b, out, k);
}

static int find_float(int viewer) {
    for (int i = 0; i < MAX_FLOATS; i++)
        if (A.floats[i] == viewer) return i;
    return -1;
}

static bool viewer_visible(int v) {
    if (A.prog && A.vrt && v >= 0 && v < A.prog->nviewers && A.vrt[v].gone) return false;
    return find_leaf(A.root, v) >= 0 || find_float(v) >= 0;
}

/* take leaf n out of the tree: its neighbour takes its room (the root: nothing is left) */
static void remove_leaf_any(int n) {
    int p = find_parent(A.root, n);
    if (p < 0) {
        if (n == A.root) {
            A.nodes[n].kind = N_FREE;
            A.root = -1;
        }
        return;
    }
    int sib = A.nodes[p].a == n ? A.nodes[p].b : A.nodes[p].a;
    A.nodes[p] = A.nodes[sib];
    A.nodes[sib].kind = N_FREE;
    A.nodes[n].kind = N_FREE;
}

/* the same, but the last tile on screen stays */
static void remove_leaf(int n) {
    if (find_parent(A.root, n) < 0 || count_shown(A.root) <= 1) return;
    remove_leaf_any(n);
}

/* take viewer v out of wherever it is shown (unless it is the last tile) */
static void detach_viewer(int v) {
    int f = find_float(v);
    if (f >= 0) A.floats[f] = -1;
    int n = find_leaf(A.root, v);
    if (n >= 0) remove_leaf(n);
}

/* show v in leaf n (swapping if v is visible in another tile) */
static void show_in_leaf(int n, int v) {
    int old = A.nodes[n].viewer;
    if (old == v) return;
    int f = find_float(v);
    if (f >= 0) A.floats[f] = -1;
    int other = find_leaf(A.root, v);
    if (other >= 0) A.nodes[other].viewer = old;
    A.nodes[n].viewer = v;
    A.focus_viewer = v;
}

/* split leaf n: v goes to its `side` (DROP_LEFT ...); the line between them goes in the middle of the part of the
   leaf no pane covers (decided by the next layout_compute) */
static void split_leaf(int n, int side, int v) {
    if (count_leaves(A.root) >= MAX_TILES) {
        set_status(true, "Too many views on screen");
        return;
    }
    if (A.nodes[n].kind != N_LEAF || A.nodes[n].viewer == v) return;
    int target = A.nodes[n].viewer;
    int f = find_float(v);
    if (f >= 0) A.floats[f] = -1;
    int other = find_leaf(A.root, v);
    if (other >= 0) {
        remove_leaf_any(other);
        n = find_leaf(A.root, target); /* (the tree changed: the target may have moved up) */
        if (n < 0) return;
    }
    int a = node_alloc(), b = -1;
    if (a < 0) return;
    A.nodes[a] = A.nodes[n];
    b = node_alloc();
    if (b < 0) { A.nodes[a].kind = N_FREE; return; }
    A.nodes[b].kind = N_LEAF;
    A.nodes[b].viewer = v;
    LNode *s = &A.nodes[n];
    s->kind = (side == DROP_LEFT || side == DROP_RIGHT) ? N_HSPLIT : N_VSPLIT;
    s->ratio = -1;
    bool new_first = side == DROP_LEFT || side == DROP_TOP;
    s->a = new_first ? b : a;
    s->b = new_first ? a : b;
    A.focus_viewer = v;
}

/* the layout of a script just opened: its first view, and the second one beside it (nothing without views) */
static void layout_default(int nviewers) {
    layout_reset(nviewers > 0 ? 0 : -1);
    if (nviewers >= 2) {
        split_leaf(A.root, DROP_RIGHT, 1);
        A.focus_viewer = 0;
    }
}

/* The script ran again (the same script, edited): the layout follows the views to their new numbers, remap[v] for
 * each old view v (-1: its section is gone).  A view that is gone leaves its tile to a view new in this run (its
 * section renamed, or another one written in its place), else its tile closes and the neighbour takes the room (a
 * floating view goes away); the other views keep their places.  If nothing is left on screen, the first views show
 * as for a script just opened.  True if a view on screen was closed. */
static bool layout_follow(const int *remap, int old_n, int new_n) {
    bool *taken = (bool *)xcalloc((size_t)new_n + 1, sizeof(bool)); /* (on screen, or the same view as before) */
    for (int ov = 0; ov < old_n; ov++)
        if (remap[ov] >= 0) taken[remap[ov]] = true;
    int fresh = 0, focus = -1;
    bool closed = false;
    int leaves[MAX_NODES], nl = collect_leaves(A.root, leaves, 0);
    for (int i = 0; i < nl + MAX_FLOATS; i++) { /* the tiles in order, then the floating views */
        int *slot = i < nl ? &A.nodes[leaves[i]].viewer : &A.floats[i - nl];
        int ov = *slot;
        if (i >= nl && ov < 0) continue; /* (an empty corner) */
        int nv = ov >= 0 && ov < old_n ? remap[ov] : -1;
        if (nv < 0) { /* gone: the next view new in this run, if any, takes its place */
            while (fresh < new_n && taken[fresh]) fresh++;
            if (fresh < new_n) {
                nv = fresh;
                taken[fresh] = true;
            }
        }
        if (ov >= 0 && ov == A.focus_viewer) focus = nv;
        closed = closed || nv < 0;
        *slot = nv;
    }
    free(taken);
    for (int n; (n = find_leaf(A.root, -1)) >= 0;) remove_leaf_any(n); /* close the tiles left empty */
    if (A.root < 0) { /* no tile left: a floating view takes the window, or the first views as for a new script */
        int f = 0;
        while (f < MAX_FLOATS && A.floats[f] < 0) f++;
        if (f < MAX_FLOATS) {
            A.root = node_alloc();
            A.nodes[A.root].kind = N_LEAF;
            A.nodes[A.root].viewer = A.floats[f];
            A.floats[f] = -1;
        } else {
            layout_default(new_n);
        }
    }
    A.focus_viewer = focus >= 0 && focus < new_n ? focus : A.focus_viewer >= 0 && A.focus_viewer < old_n ? remap[A.focus_viewer] : -1;
    if (A.focus_viewer < 0 || !viewer_visible(A.focus_viewer)) { /* the first view on screen */
        nl = collect_leaves(A.root, leaves, 0);
        A.focus_viewer = nl > 0 ? A.nodes[leaves[0]].viewer : -1;
    }
    return closed;
}

/* A run of the script that closes views (their sections gone) keeps the layout as it was before, by the views'
   names; if a later run brings them all back (an undo, say) while the layout is still as the close left it, they come
   back where they were. */
typedef struct NamedLayout {
    int n;                                  /* names in use (0: nothing kept) */
    LNode nodes[MAX_NODES];                 /* (a leaf's viewer: an index into names, -1 none) */
    int root, floats[MAX_FLOATS], focus;
    char names[MAX_NODES + MAX_FLOATS][64];
    u32 after;                              /* layout_hash of the layout the close left */
} NamedLayout;
static NamedLayout closed_layout;

static int named_view(NamedLayout *L, Program *p, int v) {
    if (v < 0 || v >= p->nviewers) return -1;
    for (int i = 0; i < L->n; i++)
        if (!strcmp(L->names[i], p->viewers[v]->name)) return i;
    if (L->n >= ARRAY_COUNT(L->names)) return -1;
    snprintf(L->names[L->n], sizeof L->names[0], "%s", p->viewers[v]->name);
    return L->n++;
}

/* the layout now, with the names of program p's views */
static void layout_by_names(Program *p, NamedLayout *L) {
    L->n = 0;
    memcpy(L->nodes, A.nodes, sizeof A.nodes);
    L->root = A.root;
    for (int i = 0; i < MAX_NODES; i++)
        if (L->nodes[i].kind == N_LEAF) L->nodes[i].viewer = named_view(L, p, A.nodes[i].viewer);
    for (int i = 0; i < MAX_FLOATS; i++) L->floats[i] = named_view(L, p, A.floats[i]);
    L->focus = named_view(L, p, A.focus_viewer);
}

/* the shape of the layout and the names in it (not the sizes) */
static u32 hash_text(u32 h, const char *s) {
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return (h ^ 0xff) * 16777619u;
}
static u32 hash_node(Program *p, int n, u32 h) {
    if (n < 0) return hash_text(h, "-");
    if (A.nodes[n].kind != N_LEAF) {
        h = hash_text(h, A.nodes[n].kind == N_HSPLIT ? "|" : "/");
        return hash_node(p, A.nodes[n].b, hash_node(p, A.nodes[n].a, h));
    }
    int v = A.nodes[n].viewer;
    return hash_text(h, v >= 0 && v < p->nviewers ? p->viewers[v]->name : "?");
}
static u32 layout_hash(Program *p) {
    u32 h = hash_node(p, A.root, 2166136261u);
    for (int i = 0; i < MAX_FLOATS; i++)
        h = hash_text(h, A.floats[i] >= 0 && A.floats[i] < p->nviewers ? p->viewers[A.floats[i]]->name : "-");
    return h;
}

/* the layout L, the names being views of program p: false (nothing changed) unless they all are */
static bool layout_from_names(const NamedLayout *L, Program *p) {
    int idx[ARRAY_COUNT(L->names)];
    for (int i = 0; i < L->n; i++) {
        idx[i] = -1;
        for (int v = 0; v < p->nviewers && idx[i] < 0; v++)
            if (!strcmp(p->viewers[v]->name, L->names[i])) idx[i] = v;
        if (idx[i] < 0) return false;
    }
    memcpy(A.nodes, L->nodes, sizeof A.nodes);
    A.root = L->root;
    for (int i = 0; i < MAX_NODES; i++)
        if (A.nodes[i].kind == N_LEAF) A.nodes[i].viewer = A.nodes[i].viewer >= 0 ? idx[A.nodes[i].viewer] : -1;
    for (int i = 0; i < MAX_FLOATS; i++) A.floats[i] = L->floats[i] >= 0 ? idx[L->floats[i]] : -1;
    A.focus_viewer = L->focus >= 0 ? idx[L->focus] : -1;
    return true;
}

/* the corners of the part of the window the panes leave free: where views float */
static Rect float_rect(int corner) {
    Rect ar = A.free;
    float w = FS_MAX(ar.w * 0.3f, px(160)), h = FS_MAX(ar.h * 0.3f, px(120));
    float m = px(12);
    Rect r = {corner & 1 ? ar.x + ar.w - w - m : ar.x + m, corner & 2 ? ar.y + ar.h - h - m : ar.y + m, w, h};
    return r;
}

static void layout_compute(int n, Rect r) {
    if (n < 0) return;
    LNode *nd = &A.nodes[n];
    nd->r = r;
    if (nd->kind == N_LEAF) {
        if (A.nvis < MAX_TILES && !leaf_gone(n)) {
            VisTile t = {nd->viewer, r, n, -1, r};
            A.vis[A.nvis++] = t;
        }
        return;
    }
    if (!count_shown(nd->a) || !count_shown(nd->b)) { /* one side is gone: the other one has the room */
        layout_compute(count_shown(nd->a) ? nd->a : nd->b, r);
        return;
    }
    bool h = nd->kind == N_HSPLIT;
    if (nd->ratio < 0) { /* a new split: in the middle of the part no pane covers */
        Rect vr = rect_and(r, A.free);
        float mid = h ? vr.x + vr.w / 2 - r.x : vr.y + vr.h / 2 - r.y, len = h ? r.w : r.h;
        nd->ratio = vr.w > 0 && vr.h > 0 && len > 0 ? FS_CLAMP(mid / len, 0.1f, 0.9f) : 0.5f;
    }
    float g = px(1);
    if (h) {
        float w0 = floorf(r.w * nd->ratio);
        Rect ra = {r.x, r.y, w0 - g, r.h}, rb = {r.x + w0 + g, r.y, r.w - w0 - g, r.h};
        layout_compute(nd->a, ra);
        layout_compute(nd->b, rb);
    } else {
        float h0 = floorf(r.h * nd->ratio);
        Rect ra = {r.x, r.y, r.w, h0 - g}, rb = {r.x, r.y + h0 + g, r.w, r.h - h0 - g};
        layout_compute(nd->a, ra);
        layout_compute(nd->b, rb);
    }
}

/* splitter under (x, y): node index or -1 */
static int splitter_at(int n, float x, float y) {
    if (n < 0) return -1;
    LNode *nd = &A.nodes[n];
    if (nd->kind == N_LEAF) return -1;
    if (!count_shown(nd->a) || !count_shown(nd->b)) return splitter_at(count_shown(nd->a) ? nd->a : nd->b, x, y);
    float t = px(4);
    Rect s;
    if (nd->kind == N_HSPLIT) {
        float sx = nd->r.x + floorf(nd->r.w * nd->ratio);
        s = (Rect){sx - t, nd->r.y, 2 * t, nd->r.h};
    } else {
        float sy = nd->r.y + floorf(nd->r.h * nd->ratio);
        s = (Rect){nd->r.x, sy - t, nd->r.w, 2 * t};
    }
    if (in_rect(s, x, y)) return n;
    int r = splitter_at(nd->a, x, y);
    return r >= 0 ? r : splitter_at(nd->b, x, y);
}


/* ---- tools of a viewer ---------------------------------------------------------- */
/* is `tool` (TOOL_PAN, TOOL_PICK or a script tool) offered in viewer v? */
static bool tool_in_viewer(int tool, int v) {
    if (tool < 0) return true;
    int of = A.prog->tools[tool]->viewer_of;
    return of < 0 || of == v;
}

static const char *tool_name(int tool) {
    return tool == TOOL_PAN ? "Pan" : tool == TOOL_PICK ? "Pick point" : tool == TOOL_INSPECT ? "Inspect" : A.prog->tools[tool]->display;
}

static const char *tool_icon(int tool) {
    return tool == TOOL_PAN ? "pan_tool" : tool == TOOL_PICK ? "my_location" : tool == TOOL_INSPECT ? "manage_search" : A.prog->tools[tool]->icon;
}

/* the first letter of the tool's name, shown when it has no (known) icon */
static void tool_letter(int tool, char out[8]) {
    const char *n = tool_name(tool);
    u32 cp;
    int len = utf8_decode(n, (int)strlen(n), &cp);
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    out[utf8_encode(cp, out)] = 0;
    (void)len;
}

static Shortcut tool_shortcut(int tool) {
    if (tool == TOOL_PAN) return (Shortcut){UI_KEY_H, 0};
    if (tool == TOOL_PICK) return (Shortcut){UI_KEY_P, 0};
    if (tool == TOOL_INSPECT) return (Shortcut){UI_KEY_SLASH, UI_MOD_SHIFT}; /* ? (as in FractalStream) */
    return parse_shortcut(A.prog->tools[tool]->shortcut);
}

/* Does the tool handle event ev (TOOL_EV_*)?  A tool without `on` blocks runs its body on a press and
   while dragging, or (.panning) on a click; with .hover also when the mouse moves. */
static bool tool_handles(int tool, int ev) {
    if (tool < 0 || !A.prog) return false;
    return tool_runs_on(A.prog->tools[tool], ev);
}

static void run_tool(int tool, int viewer, int ev, float re, float im);

/* run an event of a tool where no point was clicked: at the centre of the view */
static void run_tool_at_center(int tool, int v, int ev) {
    Section *s = A.prog->viewers[v];
    run_tool(tool, v, ev, wf(A.state[s->center->slot]), wf(A.state[s->center->slot + 1]));
}

static void select_tool(int v, int tool) {
    ViewerRT *rt = &A.vrt[v];
    if (tool >= 0 && A.prog->tools[tool]->button) { /* an action: runs once, isn't selected */
        if (tool_handles(tool, TOOL_EV_CLICK)) run_tool_at_center(tool, v, TOOL_EV_CLICK);
        else if (tool_handles(tool, TOOL_EV_PRESS)) run_tool_at_center(tool, v, TOOL_EV_PRESS);
        return;
    }
    if (rt->tool != tool) {
        if (tool_handles(rt->tool, TOOL_EV_HIDE)) run_tool_at_center(rt->tool, v, TOOL_EV_HIDE);
        rt->tool = tool;
        free(rt->show_snap); /* the next frame runs `on show` */
        rt->show_snap = NULL;
    }
}

/* the tools offered in viewer v, in the order of its bar: Pan, Pick point (when the viewer has a point to pick),
 * the script's tools for it and for every view, and Inspect; returns how many (at most cap) */
static int viewer_tools(int v, int *tools, int cap) {
    Program *p = A.prog;
    int n = 0;
    if (n < cap) tools[n++] = TOOL_PAN;
    if (p->viewers[v]->picked && n < cap) tools[n++] = TOOL_PICK;
    for (int k = 0; k < p->ntools && n < cap; k++)
        if (tool_in_viewer(k, v)) tools[n++] = k;
    if (n < cap) tools[n++] = TOOL_INSPECT;
    return n;
}

/* ---- what the script prints (the Info pane) ------------------------------------------------------------------- */
/* sources of text: the tools (0 .. ntools - 1), then these */
static int out_prepare(void) { return A.prog->ntools; }
static int out_script(void) { return A.prog->ntools + 1; }
static int out_inspect(void) { return A.prog->ntools + 2; }
static int out_count(void) { return A.prog ? A.prog->ntools + 3 : 0; }
#define OUT_KEEP_BYTES (256 * 1024) /* (what a tool with .keep prints builds up to this, the oldest lines go) */

static void outs_free(void) {
    for (int k = 0; A.outs && k < out_count(); k++) sb_free(&A.outs[k]);
    free(A.outs);
    free(A.out_serial);
    A.outs = NULL;
    A.out_serial = NULL;
}

static int out_total(void) {
    int t = 0;
    for (int k = 0; A.out_serial && k < out_count(); k++) t += A.out_serial[k];
    return t;
}

/* source k's text becomes `text` (append: added at its end); the pane notices */
static void out_set(int k, const char *text, bool append) {
    StrBuf *b = &A.outs[k];
    if (!append && (size_t)b->len == strlen(text) && !strcmp(sb_cstr(b), text)) return;
    if (!*text && append) return;
    if (!append) sb_clear(b);
    sb_puts(b, text);
    if (b->len > OUT_KEEP_BYTES) { /* drop the oldest lines */
        const char *cut = b->data + (b->len - OUT_KEEP_BYTES);
        const char *nl = memchr(cut, '\n', OUT_KEEP_BYTES);
        char *rest = xstrdup(nl ? nl + 1 : cut);
        sb_clear(b);
        sb_puts(b, rest);
        free(rest);
    }
    A.out_serial[k]++;
}

/* for a program that was just loaded (A.prog): nothing printed yet but what @global and @config printed */
static void outs_init(void) {
    A.outs = (StrBuf *)xcalloc((size_t)out_count(), sizeof(StrBuf));
    A.out_serial = (int *)xcalloc((size_t)out_count(), sizeof(int));
    A.out_seen = 0;
    A.info_auto = false;
    A.inspect_viewer = -1;
    if (A.prog->load_output) out_set(out_script(), A.prog->load_output, false);
}

/* a run that printed something because of a click opens the pane, once per run of the script */
static void out_show(void) {
    if (A.info_open || A.info_auto) return;
    A.info_open = true;
    A.info_auto = true;
}

/* ---- program loading ------------------------------------------------------------ */
static void free_viewers(void) {
    if (!A.prog) return;
    for (int v = 0; v < A.prog->nviewers; v++) {
        ViewerRT *rt = &A.vrt[v];
        if (rt->pipeline) gfx_viewer_destroy(rt->pipeline);
        if (rt->pipeline_df) gfx_viewer_destroy(rt->pipeline_df);
        flood_gpu_destroy(&rt->flood);
        flood_gpu_destroy(&rt->flood_df);
        if (rt->target) gfx_target_destroy(rt->target);
        free(rt->snap);
        free(rt->used);
        free(rt->show_snap);
        free(rt->hist);
    }
    free(A.vrt);
    A.vrt = NULL;
    for (int t = 0; t < A.prog->ntools; t++) vec_free(A.draws[t]);
    free(A.draws);
    A.draws = NULL;
}

static void diags_to_editor(Program *p) {
    EditorDiag d[64];
    int n = 0;
    for (int i = 0; i < p->diags.len && n < 64; i++) {
        Diag *g = &p->diags.data[i];
        d[n].line = g->line;
        d[n].col = g->col;
        d[n].end_col = g->end_col;
        d[n].severity = g->severity == DIAG_ERROR ? EDITOR_ERROR : g->severity == DIAG_WARNING ? EDITOR_WARNING : EDITOR_INFO;
        d[n].message = g->msg;
        n++;
    }
    editor_set_diagnostics(A.ed, d, n);
}

static void update_title(void) {
    char t[1200];
    const char *name = A.path[0] ? fs_basename(A.path) : "untitled";
    snprintf(t, sizeof t, "%s - %s%s", A.prog ? A.prog->title : "FractalStream", name,
             editor_is_modified(A.ed) ? " *" : "");
    platform_set_title(t);
}

static void destroy_rts(ViewerRT *vrt, int n) {
    for (int k = 0; k < n; k++) {
        if (vrt[k].pipeline) gfx_viewer_destroy(vrt[k].pipeline);
        if (vrt[k].pipeline_df) gfx_viewer_destroy(vrt[k].pipeline_df);
        flood_gpu_destroy(&vrt[k].flood);
        flood_gpu_destroy(&vrt[k].flood_df);
        free(vrt[k].used);
        free(vrt[k].hist);
    }
    free(vrt);
}

/* ---- completions in the script editor ------------------------------------------------------------------------ */
/* the names offered while typing: the language's words and built-ins, and the script's own names (p may be NULL) */
static void update_completions(Program *p) {
    VEC(char *) names = {0};
    static const char *words[] = {"critical", "root", "derivative", "seed", "continuing", "from", "erase", "all",
                                  "on", "press", "release", "click", "hover", "show", "hide", "button", "append",
                                  "clear", "line", "point", "circle", "disk", "text", "radius", "color", "width",
                                  "List", "Integer", "Real", "Complex", "Quaternion", "Projective", "Boolean", "Bool",
                                  "Color", "up"};
    static const char *internals[] = {".maxIterations", ".epsilon", ".escapeRadius", ".doublePrecision", ".time", ".lastClick",
                                      ".mouse", ".dragStart", ".width", ".height", ".title", ".stuck",
                                      ".iterations", ".pixel", ".panning", ".hover", ".keep", ".violet", ".red",
                                      ".green", ".blue", ".black", ".white", ".gray", ".grey", ".yellow", ".cyan",
                                      ".magenta", ".orange", ".purple", ".pink", ".brown", ".infinity"};
    StrBuf sb = {0};
#define ADD_NAME(name, detail) do { sb_clear(&sb); sb_printf(&sb, "%s\t%s", (name), (detail)); vec_push(names, xstrdup(sb_cstr(&sb))); } while (0)
    for (int k = 0; k < ARRAY_COUNT(keywords); k++) ADD_NAME(keywords[k].word, "keyword");
    for (int k = 0; k < ARRAY_COUNT(words); k++) ADD_NAME(words[k], "keyword");
    for (int k = 0; k < ARRAY_COUNT(internals); k++) ADD_NAME(internals[k], "internal");
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++)
        if (intrinsics[k].name[0] != '_') ADD_NAME(intrinsics[k].name, "built-in");
    ADD_NAME("palette", "built-in");
    if (p) {
        for (int i = 0; i < p->funcs.len; i++) {
            Func *f = p->funcs.data[i];
            if (f->name[0] != '_' && !f->tmpl) ADD_NAME(f->name, f->prelude ? "built-in" : "function");
        }
        for (int i = 0; i < p->globals.len; i++) {
            Sym *g = p->globals.data[i];
            if (strchr(g->name, ' ') || (g->viewer_index >= 0 && strchr(g->name, '.'))) continue; /* (hidden, NAME.center) */
            if (g->is_internal) {
                sb_clear(&sb);
                sb_printf(&sb, ".%s", g->name);
                ADD_NAME(sb_cstr(&sb), "internal");
            } else {
                ADD_NAME(g->name, "variable");
            }
        }
        for (int v = 0; v < p->nviewers; v++) ADD_NAME(p->viewers[v]->name, "view");
        if (p->nviewers) {
            ADD_NAME("center", "of a view");
            ADD_NAME("radius", "of a view");
        }
    }
#undef ADD_NAME
    editor_set_completions(A.ed, (const char *const *)names.data, names.len);
    for (int i = 0; i < names.len; i++) free(names.data[i]);
    vec_free(names);
    sb_free(&sb);
}

/* ---- what a word of the script is (the editor shows it when the mouse rests on the word) ----------------------- */
static const char *hover_info(void *user, const char *word) {
    static char out[700];
    static const struct { const char *name, *text; } words[] = {
        {".maxIterations", ".maxIterations : Z\nthe iteration limit of while and do loops (default 1000)"},
        {".epsilon", ".epsilon : R\nthe tolerance of == and of vanishes (default 1e-6)"},
        {".escapeRadius", ".escapeRadius : R\nz escapes means |z| > .escapeRadius (default 2)"},
        {".doublePrecision", ".doublePrecision : Bool\ndeep zooms: a view switches to double precision (slower) when its pixels get "
                             "too small for single precision (default false)"},
        {".stuck", ".stuck : Bool\ntrue when the last loop hit .maxIterations, or Newton's method failed"},
        {".iterations", ".iterations : Z\nhow many times the last loop ran"},
        {".lastClick", ".lastClick : C\nthe point of the last click or drag"},
        {".dragStart", ".dragStart : C\nwhere the mouse button went down (tools: on drag, on release)"},
        {".mouse", ".mouse : C\nthe point under the mouse"},
        {".time", ".time : R\nseconds since the script started (the views using it are animated)"},
        {".pixel", ".pixel : R\nthe size of a pixel of the view, in the plane (in a tool: of the view clicked)"},
        {".width", ".width : Z\nthe initial width of the window"},
        {".height", ".height : Z\nthe initial height of the window"},
        {".title", ".title\nthe title of the window"},
        {".panning", ".panning (in a tool)\ntrue: dragging pans the view and a click runs the tool"},
        {".hover", ".hover (in a tool)\ntrue: the tool runs whenever the mouse moves"},
        {".keep", ".keep (in a tool)\ntrue: the drawings build up until erase"},
        {".j", ".j : H\nthe quaternion unit j"},
        {".k", ".k : H\nthe quaternion unit k"},
        {"escapes", "z escapes\n|z| > .escapeRadius"},
        {"vanishes", "z vanishes\n|z| < .epsilon"},
        {"critical", "critical t -> f(t) with seed s\na zero of the derivative of f (a critical point), by Newton's method from s; "
                     ".stuck if it fails. Add `continuing from c = P` in a viewer to follow it from pixel to pixel."},
        {"root", "root t -> f(t) with seed s\na zero of f, by Newton's method from s; .stuck if it fails"},
        {"derivative", "derivative t -> f(t) at x\nf'(x), by automatic differentiation"},
        {"continuing", "... continuing from c = P\nthe solution is found at the pixel of P, then followed to the other pixels "
                       "(each one starts from a neighbour's)"},
        {"seed", "with seed s\nwhere Newton's method starts"},
        {"while", "while COND do ... end\nruns at most .maxIterations times; .stuck tells whether it hit the limit and "
                  ".iterations how many times it ran"},
        {"do", "do ... while COND (or: while COND do ... end)"},
        {"for", "for k in a..b do ... end\nall of the range (by STEP: another step); for x in LIST do ... end"},
        {"draw", "draw line between P and Q, draw point at P, draw circle at P with radius r, draw disk ..., "
                 "draw text \"...\" + value at P; with color c (in tools)"},
        {"erase", "erase\nthis tool's drawings (erase all: every tool's; erase \"Name\": that tool's)"},
        {"define", "define f(x) = expression\nor: define f(x) = y where ... end (the results are named after =)"},
        {"where", "define f(x) = y where ... end\nthe body that computes the results"},
        {"px", "N px\nN pixels of the view (a radius or a distance on the screen)"},
        {"mod", "a mod b\nthe remainder, with the sign of b"},
        {"palette", "palette(\"name\", t) : Color\nthe maps rainbow arenberg romao bamo broco corko viko (cyclic) and ice fire "
                    "wheat rose forest ocean winter spring summer fall; period 1 in t"},
        {"on", "on press / drag / release / click / hover / show / hide do ... end\nthe part of a tool that runs on that event"},
        {"color", "color\nwhat a viewer sets for its pixel (it starts black)"},
    };
    for (int k = 0; k < ARRAY_COUNT(words); k++)
        if (!strcmp(words[k].name, word)) return words[k].text;
    Program *p = A.prog;
    if (!p) return NULL;
    StrBuf sb = {0};
    const char *dot = strchr(word, '.');
    if (dot && dot != word) { /* NAME.center, NAME.radius */
        for (int v = 0; v < p->nviewers; v++) {
            Section *vs = p->viewers[v];
            size_t n = (size_t)(dot - word);
            if (strlen(vs->name) != n || strncmp(vs->name, word, n)) continue;
            if (!strcmp(dot + 1, "center")) sb_printf(&sb, "%s : C\nthe centre of the view %s", word, vs->name);
            else if (!strcmp(dot + 1, "radius")) sb_printf(&sb, "%s : R\nthe radius of the largest disk around the centre that fits in the view", word);
        }
    } else if (word[0] == '.') { /* the script's internals: picked points */
        for (int i = 0; i < p->globals.len && !sb.len; i++) {
            Sym *g = p->globals.data[i];
            if (g->is_internal && !strcmp(g->name, word + 1))
                sb_printf(&sb, "%s : %s%s%s", word, type_str(g->type), g->doc ? "\n" : "", g->doc ? g->doc : "");
        }
    } else {
        for (int i = 0; i < p->globals.len && !sb.len; i++) { /* the script's variables */
            Sym *g = p->globals.data[i];
            if (g->is_internal || strcmp(g->name, word) != 0) continue;
            sb_printf(&sb, "%s : %s", g->name, type_str(g->type));
            if (g->has_range) sb_printf(&sb, " in %g..%g", g->range_lo, g->range_hi);
            if (g->label) sb_printf(&sb, "\n\"%s\"", g->label);
            sb_printf(&sb, "\n%s", g->is_config ? "a variable of @config (in the config panel)" : "a global variable");
        }
        bool var = sb.len > 0;
        int shown = 0;
        for (int i = 0; i < p->funcs.len && !var && shown < 4; i++) { /* functions (several for the overloads) */
            Func *f = p->funcs.data[i];
            if (strcmp(f->name, word) != 0 || f->tmpl || f->shadowed) continue; /* (instances: below) */
            sb_printf(&sb, "%s%s(", shown ? "\n" : "", f->name);
            for (int k = 0; k < f->nparams; k++) {
                if (f->generic && !f->param_tsyn[k]) sb_printf(&sb, "%s%s", k ? ", " : "", f->params[k]->name);
                else sb_printf(&sb, "%s%s : %s", k ? ", " : "", f->params[k]->name, f->params[k]->type ? type_str(f->params[k]->type) : "?");
            }
            if (!f->generic) {
                sb_printf(&sb, ")%s%s", f->ret ? " : " : "", f->ret ? type_str(f->ret) : "");
            } else { /* the copies made for the calls */
                sb_puts(&sb, ")\n(parameters without a type take the type of the argument)");
                int ninst = 0;
                for (Func *g = f->instances; g; g = g->next_instance) {
                    if (g->probe || g->failed || ninst >= 4) continue;
                    sb_printf(&sb, "\n%s %s(", ninst ? "    " : "used as", g->name);
                    for (int k = 0; k < g->nparams; k++)
                        sb_printf(&sb, "%s%s : %s", k ? ", " : "", g->params[k]->name, type_str(g->params[k]->type));
                    sb_printf(&sb, ")%s%s", g->ret ? " : " : "", g->ret ? type_str(g->ret) : "");
                    ninst++;
                }
            }
            shown++;
        }
        for (int k = 0; k < ARRAY_COUNT(intrinsics) && !sb.len; k++) { /* built-ins */
            const IntrinsicInfo *in = &intrinsics[k];
            if (strcmp(in->name, word) != 0) continue;
            sb_printf(&sb, "%s(", in->name);
            for (int j = 0; j < in->nparams; j++) sb_printf(&sb, "%s%c", j ? ", " : "", in->params[j] == '*' ? 'x' : in->params[j]);
            sb_printf(&sb, ")");
            if (in->ret != '*') sb_printf(&sb, " : %c", in->ret);
        }
        for (int i = 0; i < p->sections.len && !sb.len; i++) { /* a local of a section (the first one met) */
            Section *sec = p->sections.data[i];
            for (int k = 0; k < sec->locals.len && !sb.len; k++) {
                Sym *l = sec->locals.data[k];
                if (l->name && !strcmp(l->name, word) && l->type)
                    sb_printf(&sb, "%s : %s\na local variable (of the section)", word, type_str(l->type));
            }
        }
        for (int v = 0; v < p->nviewers && !sb.len; v++) /* views */
            if (!strcmp(p->viewers[v]->name, word))
                sb_printf(&sb, "the view %s%s%s%s\n%s is the point of the pixel; %s.center and %s.radius are where it looks",
                           word, p->viewers[v]->display ? " (\"" : "", p->viewers[v]->display ? p->viewers[v]->display : "",
                           p->viewers[v]->display ? "\")" : "", p->viewers[v]->var ? p->viewers[v]->var : "z", word, word);
    }
    snprintf(out, sizeof out, "%s", sb_cstr(&sb));
    sb_free(&sb);
    size_t n = strlen(out);
    while (n && out[n - 1] == '\n') out[--n] = 0;
    return out[0] ? out : NULL;
}

/* ---- values of the script -------------------------------------------------------------------- */
/* globals the app sets by itself */
static bool app_managed(Program *p, Sym *s) {
    return s == p->s_mouse || s == p->s_time || s == p->s_last_click || s == p->s_drag_start || s == p->s_width ||
           s == p->s_height;
}

/* a view's centre and radius (and their hidden parts): the views have their own reset and history */
static bool view_geometry(Program *p, Sym *s) {
    if (s->viewer_index < 0 || s->viewer_index >= p->nviewers) return false;
    Section *v = p->viewers[s->viewer_index];
    return s == v->center || s == v->radius || s == v->center_lo || s == v->mobius;
}

/* lists: only the items in use */
static bool same_value(Type *t, const u32 *a, const u32 *b) {
    if (t->kind == T_LIST) {
        if (a[0] != b[0]) return false;
        int n = FS_CLAMP((i32)a[0], 0, t->n);
        return memcmp(a + t->data_off, b + t->data_off, sizeof(u32) * (size_t)(n * t->stride)) == 0;
    }
    return memcmp(a, b, sizeof(u32) * (size_t)t->size) == 0;
}

/* Compiling `src`, and on success replacing the running program.  LOAD_RUN (the same script, edited or changed on
   disk) keeps the layout, the views and their history, the tools, and the values changed since the last run (unless
   the script now gives them another value); LOAD_OPEN (another script) starts afresh. */
enum { LOAD_OPEN, LOAD_RUN };

/* The script has errors, so the program keeps running as it was; but a view whose @viewer section is gone from the
   text closes all the same (it comes back where it was if its section does, after an undo say; the script running
   without errors closes it for good, see layout_follow). */
static void mark_gone(Program *failed) {
    for (int v = 0; A.prog && v < A.prog->nviewers; v++) {
        bool there = false;
        for (int i = 0; i < failed->sections.len && !there; i++) {
            Section *s = failed->sections.data[i];
            there = s->kind == SEC_VIEWER && s->name && !strcmp(s->name, A.prog->viewers[v]->name);
        }
        A.vrt[v].gone = !there;
    }
    if (A.drag != DRAG_NONE && A.drag_viewer >= 0 && A.prog && A.drag_viewer < A.prog->nviewers && A.vrt[A.drag_viewer].gone)
        A.drag = DRAG_NONE;
    if (A.focus_viewer >= 0 && !viewer_visible(A.focus_viewer)) { /* the focus goes to a view still shown */
        int leaves[MAX_NODES], nl = collect_leaves(A.root, leaves, 0);
        A.focus_viewer = -1;
        for (int i = 0; i < nl && A.focus_viewer < 0; i++)
            if (!leaf_gone(leaves[i])) A.focus_viewer = A.nodes[leaves[i]].viewer;
    }
}

/* the viewers of a program that compiled, on the GPU; NULL (and a message) if the GPU rejects one */
static ViewerRT *make_viewers(Program *p) {
    ViewerRT *vrt = (ViewerRT *)xcalloc((size_t)p->nviewers + 1, sizeof(ViewerRT));
    char err[512];
    for (int v = 0; v < p->nviewers; v++) {
        Section *s = p->viewers[v];
        bool ok;
        if (s->flood) {
            vrt[v].is_flood = true;
            ok = flood_gpu_create(&vrt[v].flood, p, s, err, sizeof err);
        } else {
            u32 n;
            u32 *words = codegen_viewer(p, s, &n, err, sizeof err);
            vrt[v].pipeline = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
            free(words);
            ok = vrt[v].pipeline > 0;
        }
        if (!ok) {
            set_status(true, "The GPU rejected the viewer '%s': %s", s->name, err);
            destroy_rts(vrt, v + 1);
            return NULL;
        }
    }
    return vrt;
}

/* Replace the running program by p (compiled, without errors), whose viewers are on the GPU in vrt (see make_viewers;
   zeroed entries work too, without a GPU: the tests). */
static void adopt_program(Program *p, ViewerRT *vrt, int mode) {
    for (int v = 0; v < p->nviewers; v++) {
        Section *s = p->viewers[v];
        vrt[v].used = (bool *)xcalloc((size_t)p->globals.len + 1, sizeof(bool));
        codegen_viewer_deps(p, s, vrt[v].used);
        for (int i = 0; i < p->globals.len; i++) /* (read by the deep zoom version) */
            if (p->globals.data[i] == s->center_lo) vrt[v].used[i] = true;
        for (int i = 0; i < p->globals.len; i++)
            if (vrt[v].used[i] && p->globals.data[i] == p->s_time) vrt[v].animated = true;
        vrt[v].tool = TOOL_PAN;
        vrt[v].center_bits[0] = p->initial_state[s->center->slot]; /* (the script's centre has its low part: see
                                                                      check_center_lo) */
        vrt[v].center_bits[1] = p->initial_state[s->center->slot + 1];
    }
    /* new state; keep the views, picked points and tools of viewers that still exist */
    bool same = mode == LOAD_RUN && A.prog;
    int old_n = A.prog ? A.prog->nviewers : 0;
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    int *remap = (int *)xmalloc(sizeof(int) * (size_t)(old_n + 1));
    for (int ov = 0; ov < old_n; ov++) remap[ov] = -1;
    for (int ov = 0; same && ov < old_n; ov++) {
        Section *os = A.prog->viewers[ov];
        for (int v = 0; v < p->nviewers; v++) {
            Section *s = p->viewers[v];
            if (strcmp(s->name, os->name) != 0) continue;
            remap[ov] = v;
            memcpy(state + s->center->slot, A.state + os->center->slot, 8);
            memcpy(state + s->radius->slot, A.state + os->radius->slot, 4);
            if (s->center_lo && os->center_lo) {
                memcpy(state + s->center_lo->slot, A.state + os->center_lo->slot, 8);
                memcpy(vrt[v].center_bits, A.vrt[ov].center_bits, sizeof vrt[v].center_bits);
            }
            if (s->mobius && os->mobius) memcpy(state + s->mobius->slot, A.state + os->mobius->slot, 32);
            vrt[v].hist = A.vrt[ov].hist; /* (Back still works) */
            A.vrt[ov].hist = NULL;
            vrt[v].grid = A.vrt[ov].grid;
            if (s->picked && os->picked && s->picked->type == os->picked->type && A.vrt[ov].picked) {
                memcpy(state + s->picked->slot, A.state + os->picked->slot, sizeof(u32) * (size_t)s->picked->type->size);
                vrt[v].picked = true;
            }
            int ot = A.vrt[ov].tool;
            vrt[v].tool = ot < 0 ? ot : TOOL_PAN;
            for (int t = 0; ot >= 0 && t < p->ntools; t++)
                if (!strcmp(p->tools[t]->display, A.prog->tools[ot]->display) &&
                    (p->tools[t]->viewer_of < 0 || p->tools[t]->viewer_of == v))
                    vrt[v].tool = t;
        }
    }
    /* the values changed since the last run (in the panel, by tools) stay, unless the script changed them too */
    for (int i = 0; same && i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        if (s->kind != SYM_GLOBAL || app_managed(p, s) || view_geometry(p, s)) continue;
        for (int k = 0; k < A.prog->globals.len; k++) {
            Sym *os = A.prog->globals.data[k];
            if (strcmp(os->name, s->name) != 0 || os->is_internal != s->is_internal) continue;
            if (os->type == s->type && os->kind == SYM_GLOBAL &&
                same_value(s->type, A.prog->initial_state + os->slot, p->initial_state + s->slot))
                memcpy(state + s->slot, A.state + os->slot, sizeof(u32) * (size_t)s->type->size);
            break;
        }
    }
    /* views an earlier run closed come back with their sections (an undo), if the layout is as that run left it */
    NamedLayout before = {0};
    bool reopened = false;
    if (same && closed_layout.n) {
        if (layout_hash(A.prog) != closed_layout.after) closed_layout.n = 0; /* (the layout changed since) */
        else if (layout_from_names(&closed_layout, p)) reopened = true;
    }
    if (reopened || !same) closed_layout.n = 0;
    if (same && !reopened) layout_by_names(A.prog, &before);
    free_viewers();
    if (A.prog) interp_free(&A.interp);
    outs_free();
    program_free(A.prog);
    free(A.state);
    free(A.uploaded);
    A.prog = p;
    A.state = state;
    A.uploaded = NULL;
    A.vrt = vrt;
    /* the layout follows the views (the ones gone close, the others stay where they are), or starts afresh */
    if (same && !reopened) {
        if (layout_follow(remap, old_n, p->nviewers)) {
            closed_layout = before;
            closed_layout.after = layout_hash(p);
        }
    } else if (!same) {
        layout_default(p->nviewers);
    }
    /* what was going on with a view goes on with the same view, or stops */
    int nd = same && A.drag_viewer >= 0 && A.drag_viewer < old_n ? remap[A.drag_viewer] : -1;
    if (A.drag != DRAG_NONE && nd < 0) A.drag = DRAG_NONE;
    A.drag_viewer = nd;
    int ne = same && A.export_viewer >= 0 && A.export_viewer < old_n ? remap[A.export_viewer] : -1;
    if (A.req_export[0] && ne < 0) A.req_export[0] = 0;
    A.export_viewer = FS_MAX(ne, 0);
    A.hover_viewer = A.hover_tile = -1;
    A.nvis = 0; /* (until the next layout) */
    free(remap);
    A.draws = (DrawItemVec *)xcalloc((size_t)p->ntools + 1, sizeof(DrawItemVec));
    outs_init();
    A.choice_open = NULL;
    A.text_name[0] = 0; /* (a value being typed belongs to the old program) */
    free(A.prepare_snap);
    A.prepare_snap = NULL;
    interp_init(&A.interp, p, A.state);
    A.interp.step_limit = 3e7;
    for (int v = 0; v < p->nviewers; v++) {
        Sym *ps = p->viewers[v]->picked;
        if (ps && (state[ps->slot] || state[ps->slot + 1])) vrt[v].picked = true;
    }
    if (p->nviewers == 0) A.editor_open = true; /* (nothing else to show) */
    update_completions(p);
    update_title();
}

static bool load_source(const char *src, int mode) {
    Program *p = program_load(src, A.path[0] ? A.path : "untitled");
    diags_to_editor(p);
    A.compiled_version = editor_version(A.ed);
    A.script_errors = p->nerrors;
    if (p->nerrors) {
        set_status(true, "%d error%s in the script%s", p->nerrors, p->nerrors == 1 ? "" : "s",
                   A.editor_open ? "" : " (Ctrl+E shows it)");
        if (mode == LOAD_RUN && A.prog) mark_gone(p);
        program_free(p);
        return false;
    }
    ViewerRT *vrt = make_viewers(p);
    if (!vrt) {
        program_free(p);
        return false;
    }
    adopt_program(p, vrt, mode);
    return true;
}

/* ---- kept between sessions: the recent files ------------------------------------------------------------------ */
/* the settings folder: $FS_CONFIG_DIR, else the system's place for them */
static const char *config_dir(void) {
    static char dir[1100];
    const char *env = getenv("FS_CONFIG_DIR");
    if (env && env[0]) {
        snprintf(dir, sizeof dir, "%.1000s", env);
        return dir;
    }
#if FS_OS_WINDOWS
    const char *base = getenv("APPDATA");
    if (!base || !base[0]) return NULL;
    snprintf(dir, sizeof dir, "%.1000s\\FractalStream", base);
#elif FS_OS_MACOS
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    snprintf(dir, sizeof dir, "%.1000s/Library/Application Support/FractalStream", home);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(dir, sizeof dir, "%.1000s/fractalstream", xdg);
    else if (home && home[0]) snprintf(dir, sizeof dir, "%.1000s/.config/fractalstream", home);
    else return NULL;
#endif
    return dir;
}

static void load_recent(void) {
    A.nrecent = 0;
    const char *dir = config_dir();
    if (!dir) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/recent.txt", dir);
    size_t len;
    char *text = fs_read_file(path, &len);
    if (!text) return;
    for (char *line = text; *line && A.nrecent < ARRAY_COUNT(A.recent);) {
        char *end = line + strcspn(line, "\r\n");
        char c = *end;
        *end = 0;
        if (line[0]) snprintf(A.recent[A.nrecent++], sizeof A.recent[0], "%s", line);
        line = c ? end + 1 : end;
    }
    free(text);
}

/* a script was opened or saved: first in the list (which is saved) */
static void note_recent(const char *file) {
    char abs[1024];
    fs_absolute(file, abs, sizeof abs);
    int k = 0;
    while (k < A.nrecent && strcmp(A.recent[k], abs) != 0) k++;
    if (k == A.nrecent && A.nrecent < ARRAY_COUNT(A.recent)) A.nrecent++;
    if (k == ARRAY_COUNT(A.recent)) k--; /* (full: the oldest goes) */
    memmove(A.recent[1], A.recent[0], sizeof A.recent[0] * (size_t)k);
    snprintf(A.recent[0], sizeof A.recent[0], "%s", abs);
    const char *dir = config_dir();
    if (!dir || !fs_make_dirs(dir)) return;
    StrBuf sb = {0};
    for (int i = 0; i < A.nrecent; i++) sb_printf(&sb, "%s\n", A.recent[i]);
    char path[1200];
    snprintf(path, sizeof path, "%s/recent.txt", dir);
    fs_write_file(path, sb.data ? sb.data : "", (size_t)sb.len);
    sb_free(&sb);
}

/* settings.txt: "key=value" lines (the size of the UI) */
static void load_settings(void) {
    A.ui_zoom = 1;
    const char *dir = config_dir();
    if (!dir) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    size_t len;
    char *text = fs_read_file(path, &len);
    if (!text) return;
    const char *z = strstr(text, "ui_zoom=");
    if (z) A.ui_zoom = FS_CLAMP((float)atof(z + 8), 0.5f, 3.0f);
    free(text);
}

static void save_settings(void) {
    const char *dir = config_dir();
    if (!dir || !fs_make_dirs(dir)) return;
    char path[1200], text[64];
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    int n = snprintf(text, sizeof text, "ui_zoom=%.3g\n", A.ui_zoom);
    fs_write_file(path, text, (size_t)n);
}

static void set_ui_zoom(float z) {
    A.ui_zoom = FS_CLAMP(z, 0.5f, 3.0f);
    if (fabsf(A.ui_zoom - 1) < 0.02f) A.ui_zoom = 1;
    save_settings();
    set_status(false, "Size of the interface: %d%% (" MODKEY "+0: 100%%)", (int)lroundf(A.ui_zoom * 100));
}

static bool open_file(const char *path, bool reload) {
    size_t len;
    char *src = fs_read_file(path, &len);
    if (!src) {
        set_status(true, "Can't read %s", path);
        return false;
    }
    snprintf(A.path, sizeof A.path, "%s", path);
    editor_set_text(A.ed, src, (int)len);
    editor_set_unmodified(A.ed);
    A.file_mtime = file_mtime(A.path);
    bool ok = load_source(src, reload ? LOAD_RUN : LOAD_OPEN);
    free(src);
    if (!reload) note_recent(path);
    if (!ok) A.editor_open = true;
    else set_status(false, "Opened %s", fs_basename(path));
    return ok;
}

static bool save_file(const char *path) {
    int len;
    const char *text = editor_get_text(A.ed, &len);
    if (!fs_write_file(path, text, (size_t)len)) {
        set_status(true, "Can't write %s", path);
        return false;
    }
    snprintf(A.path, sizeof A.path, "%s", path);
    editor_set_unmodified(A.ed);
    A.file_mtime = file_mtime(A.path);
    note_recent(path);
    set_status(false, "Saved %s", fs_basename(path));
    update_title();
    return true;
}

/* ---- unsaved changes: asked about before they would be lost ----------------------------------------------------- */
enum { PENDING_NONE, PENDING_OPEN, PENDING_NEW, PENDING_QUIT };

/* May `action` go ahead?  Not while the script has unsaved changes: the dialog asks first, and the action waits. */
static bool unsaved_guard(int action, const char *path) {
    if (A.discard_ok || !editor_is_modified(A.ed)) {
        A.discard_ok = false;
        return true;
    }
    A.pending = action;
    snprintf(A.pending_path, sizeof A.pending_path, "%s", path ? path : "");
    A.modal = 5;
    return false;
}

/* a new script: a copy of `path` (or of the welcome script), not saved anywhere yet (see new_from) */
static void new_from_now(const char *path) {
    size_t len = 0;
    char *src = path ? fs_read_file(path, &len) : NULL;
    if (path && !src) {
        set_status(true, "Can't read %s", path);
        return;
    }
    const char *text = src ? src : welcome_script;
    A.path[0] = 0;
    A.file_mtime = 0;
    editor_set_text(A.ed, text, (int)strlen(text));
    editor_set_unmodified(A.ed);
    if (load_source(text, LOAD_OPEN)) set_status(false, "A new script%s%s: " MODKEY "+S saves it", path ? " from " : "",
                                                 path ? fs_basename(path) : "");
    else A.editor_open = true;
    A.editor_open = true;
    free(src);
    update_title();
}

/* (at the start of the next frame, like every load) */
static void new_from(const char *path) {
    A.req_new = true;
    snprintf(A.req_new_path, sizeof A.req_new_path, "%s", path ? path : "");
}

static void run_editor_now(void) {
    int len;
    const char *text = editor_get_text(A.ed, &len);
    if (load_source(text, LOAD_RUN)) set_status(false, "Running");
}

/* Compiling or opening replaces the viewers and their pictures, which this frame's drawing may already
   show: requests made while the UI is built wait for the start of the next frame. */
static void run_editor(void) { A.req_run = true; }

static void request_open(const char *path) { snprintf(A.req_open, sizeof A.req_open, "%s", path); }

static void export_picture(const char *path, int v, int scale);
static int vpipeline(ViewerRT *rt);

static void process_requests(void) {
    if (A.req_open[0]) {
        char path[1024];
        snprintf(path, sizeof path, "%s", A.req_open);
        A.req_open[0] = 0;
        bool reload = A.req_reload;
        A.req_reload = false;
        if ((reload || unsaved_guard(PENDING_OPEN, path)) && open_file(path, reload) && reload)
            set_status(false, "Reloaded %s (changed on disk)", fs_basename(path));
    }
    if (A.req_new) {
        A.req_new = false;
        new_from_now(A.req_new_path[0] ? A.req_new_path : NULL);
    }
    if (A.req_run) {
        A.req_run = false;
        run_editor_now();
    }
    if (A.req_export[0] && --A.export_wait <= 0) { /* (after a frame showing "Saving...") */
        char path[1024];
        snprintf(path, sizeof path, "%s", A.req_export);
        A.req_export[0] = 0;
        export_picture(path, A.export_viewer, A.export_scale);
    }
}

/* ~/Documents/NAME (or ~/NAME) */
static void default_save_path_named(char *out, int cap, const char *name) {
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = ".";
    char docs[900];
    snprintf(docs, sizeof docs, "%s/Documents", home);
    struct stat st;
    snprintf(out, (size_t)cap, "%.800s/%.200s", stat(docs, &st) == 0 ? docs : home, name);
}

static void default_save_path(char *out, int cap) { default_save_path_named(out, cap, "untitled.fstream"); }

/* a script inside the app (the examples in FractalStream.app): saved elsewhere, the app must stay as it is */
static bool inside_app_bundle(const char *path) {
    char abs[1024];
    fs_absolute(path, abs, sizeof abs);
    return strstr(abs, ".app/Contents/") != NULL;
}

/* Save: where the script came from, unless it has no file yet or its file is inside the app */
static void save_or_ask(void);

static void open_modal(int kind, const char *path);
static const char *examples_dir(void);
static const char *home_dir(void);
static void ask_new(void);

static void ask_save_as(void) {
    char path[1024];
    if (A.path[0] && inside_app_bundle(A.path)) default_save_path_named(path, sizeof path, fs_basename(A.path));
    else if (A.path[0]) snprintf(path, sizeof path, "%s", A.path);
    else default_save_path(path, sizeof path);
    open_modal(1, path);
}

static void save_or_ask(void) {
    if (A.path[0] && !inside_app_bundle(A.path)) save_file(A.path);
    else ask_save_as();
}

/* the dialog starts in the folder of the script open now, else at the examples (else at home) */
static void ask_open(void) {
    char dir[1024], abs[1024], path[1100];
    if (A.path[0]) {
        const char *slash = strrchr(A.path, '/');
        if (!slash) slash = strrchr(A.path, '\\');
        snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - A.path) : 0, A.path);
        fs_absolute(dir[0] ? dir : ".", abs, sizeof abs);
    } else {
        snprintf(abs, sizeof abs, "%.1000s", examples_dir()[0] ? examples_dir() : home_dir() ? home_dir() : ".");
    }
    snprintf(path, sizeof path, "%.1000s/", abs);
    open_modal(2, path);
}

/* the size of the picture of viewer v: `scale` times its size on the screen (within the GPU's limit) */
static void export_size(int v, int scale, int *w, int *h) {
    *w = *h = 1024;
    for (int i = 0; i < A.nvis; i++)
        if (A.vis[i].viewer == v && A.vis[i].r.w >= 1 && A.vis[i].r.h >= 1) {
            *w = (int)A.vis[i].r.w;
            *h = (int)A.vis[i].r.h;
        }
    *w *= scale;
    *h *= scale;
    int maxs = gfx_max_image_size();
    if (maxs > 0 && (*w > maxs || *h > maxs)) {
        float k = (float)maxs / (float)FS_MAX(*w, *h);
        *w = FS_MAX(1, (int)(*w * k));
        *h = FS_MAX(1, (int)(*h * k));
    }
}

/* save the picture of viewer v, `scale` times the size of its tile */
static void ask_export(int v, int scale) {
    A.export_viewer = v;
    A.export_scale = scale;
    char dir[1024];
    if (A.path[0] && !inside_app_bundle(A.path)) {
        snprintf(dir, sizeof dir, "%s", A.path);
        char *slash = strrchr(dir, '/');
        if (!slash) slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        else snprintf(dir, sizeof dir, ".");
    } else {
        default_save_path(dir, sizeof dir);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
    }
    const char *base = A.path[0] ? fs_basename(A.path) : "untitled";
    char stem[256];
    snprintf(stem, sizeof stem, "%.200s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    char path[1100];
    snprintf(path, sizeof path, "%.700s/%.200s-%.60s%s.png", dir, stem, A.prog->viewers[v]->name, scale > 1 ? "-large" : "");
    open_modal(3, path);
}

/* render viewer v again, offscreen, and write it to a PNG file */
static void export_picture(const char *path, int v, int scale) {
    Program *p = A.prog;
    if (!p || v < 0 || v >= p->nviewers) return;
    Section *s = p->viewers[v];
    int w, h;
    export_size(v, scale, &w, &h);
    size_t bytes = sizeof(u32) * (size_t)p->state_words;
    gfx_set_globals(A.state, (u32)bytes);
    if (!A.uploaded) A.uploaded = (u32 *)xmalloc(bytes);
    memcpy(A.uploaded, A.state, bytes);
    /* the picture at screen size is rendered at twice the size and averaged down (smooth edges) */
    int ss = 1, maxs = gfx_max_image_size();
    if (scale == 1 && (maxs <= 0 || (2 * w <= maxs && 2 * h <= maxs))) ss = 2;
    int W = w * ss, H = h * ss;
    u8 *big = (u8 *)xmalloc((size_t)W * H * 4);
    char err[512] = "";
    bool ok = A.vrt[v].is_flood ? flood_render_gpu_ex(p, A.state, s, A.vrt[v].df, W, H, big, NULL, err, sizeof err)
                                : gfx_render_to_memory(vpipeline(&A.vrt[v]), W, H, 64, big, err, sizeof err);
    u8 *rgba = big;
    if (ok && ss > 1) {
        rgba = (u8 *)xmalloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) {
                    int sum = 0;
                    for (int dy = 0; dy < ss; dy++)
                        for (int dx = 0; dx < ss; dx++) sum += big[4 * ((size_t)(y * ss + dy) * W + (x * ss + dx)) + c];
                    rgba[4 * ((size_t)y * w + x) + c] = (u8)((sum + ss * ss / 2) / (ss * ss));
                }
        free(big);
    }
    if (!ok) set_status(true, "Can't render the picture: %s", err);
    else if (!stbi_write_png(path, w, h, 4, rgba, w * 4)) set_status(true, "Can't write %s", path);
    else set_status(false, "Saved %s (%dx%d)", path, w, h);
    free(rgba);
}

/* ---- running tools ----------------------------------------------------------------- */
/* run event ev (TOOL_EV_*) of a tool in a viewer, at the point (re, im) */
static void run_tool(int tool, int viewer, int ev, float re, float im) {
    Program *p = A.prog;
    Section *t = p->tools[tool];
    if (ev == TOOL_EV_PRESS || ev == TOOL_EV_DRAG || ev == TOOL_EV_RELEASE || ev == TOOL_EV_CLICK) set_c(p->s_last_click, re, im);
    if (ev != TOOL_EV_SHOW && ev != TOOL_EV_HIDE) set_c(p->s_mouse, re, im);
    /* remember the picked points: a tool that moves one makes its marker appear */
    float before[64][2];
    ViewPlace was[64]; /* (a tool that moves a view: Back returns) */
    for (int v = 0; v < p->nviewers && v < 64; v++) {
        Sym *ps = p->viewers[v]->picked;
        before[v][0] = ps ? wf(A.state[ps->slot]) : 0;
        before[v][1] = ps ? wf(A.state[ps->slot + 1]) : 0;
        was[v] = view_place(v);
    }
    /* .pixel: the size of a pixel of the viewer, under the mouse */
    A.interp.pixel = 0;
    for (int i = 0; i < A.nvis; i++)
        if (A.vis[i].viewer == viewer) {
            Rect r = A.vis[i].r;
            float x = FS_CLAMP(A.in.mouse_x - r.x, 0.0f, r.w), y = FS_CLAMP(A.in.mouse_y - r.y, 0.0f, r.h);
            A.interp.pixel = viewer_pixel_size(p, A.state, p->viewers[viewer], r.w, r.h, x, y);
            break;
        }
    DrawItemVec out = {0};
    StrBuf printed = {0};
    A.interp.draws = &out;
    A.interp.out = &printed;
    A.interp.erased = A.interp.erased_all = false;
    A.interp.erase_tools = 0;
    bool ok = t->has_events ? interp_run_tool_event(&A.interp, t, ev, re, im) : interp_run_tool(&A.interp, t, re, im);
    if (!ok) set_status(true, "%s: %s", t->display, A.interp.error);
    A.interp.draws = NULL;
    A.interp.out = NULL;
    /* what it printed: like its drawings (replaced at each run, or built up with .keep until `erase`) */
    if (A.interp.erased_all)
        for (int k = 0; k < p->ntools; k++)
            if (k != tool) out_set(k, "", false);
    for (int k = 0; k < p->ntools && k < 64; k++)
        if ((A.interp.erase_tools >> k & 1) && k != tool) out_set(k, "", false);
    bool keep_text = t->keep && !A.interp.erased && !(A.interp.erase_tools >> tool & 1);
    out_set(tool, sb_cstr(&printed), keep_text);
    if (printed.len && (ev == TOOL_EV_PRESS || ev == TOOL_EV_CLICK)) out_show();
    sb_free(&printed);
    for (int k = 0; k < out.len; k++)
        if (out.data[k].viewer < 0) out.data[k].viewer = viewer;
    if (A.interp.erased_all)
        for (int k = 0; k < p->ntools; k++) vec_clear(A.draws[k]);
    for (int k = 0; k < p->ntools && k < 64; k++) /* erase "Name" */
        if (A.interp.erase_tools >> k & 1) {
            if (k == tool) A.interp.erased = true;
            else vec_clear(A.draws[k]);
        }
    if (t->keep && !A.interp.erased) { /* .keep: the drawings build up until `erase` */
        for (int k = 0; k < out.len; k++) vec_push(A.draws[tool], out.data[k]);
        vec_free(out);
    } else {
        vec_free(A.draws[tool]);
        A.draws[tool] = out;
    }
    for (int v = 0; v < p->nviewers && v < 64; v++) {
        Sym *ps = p->viewers[v]->picked;
        if (ps && (wf(A.state[ps->slot]) != before[v][0] || wf(A.state[ps->slot + 1]) != before[v][1]))
            for (int w = 0; w < p->nviewers; w++)
                if (p->viewers[w]->picked == ps) A.vrt[w].picked = true;
        ViewPlace now = view_place(v);
        if (!same_place(&now, &was[v])) nav_note(v, NAV_TOOL, &was[v]);
    }
}

static void pick(int viewer, float re, float im) {
    Section *s = A.prog->viewers[viewer];
    set_c(A.prog->s_last_click, re, im);
    if (!s->picked) return;
    u32 *dst = A.state + s->picked->slot;
    memset(dst, 0, sizeof(u32) * (size_t)s->picked->type->size);
    dst[0] = fw(re);
    dst[1] = fw(im);
    /* every viewer sharing this variable shows the marker */
    for (int v = 0; v < A.prog->nviewers; v++)
        if (A.prog->viewers[v]->picked == s->picked) A.vrt[v].picked = true;
}

/* The Inspect tool: run viewer v's code for the point (re, im) on the CPU and show what it computed, its variables at
 * the end (like FractalStream's Debug tool), after what it printed. */
static void inspect_at(int v, float re, float im) {
    Program *p = A.prog;
    Section *s = p->viewers[v];
    size_t bytes = sizeof(u32) * (size_t)p->state_words;
    u32 *st = (u32 *)xmalloc(bytes); /* (a viewer changes only its own copy of the globals) */
    memcpy(st, A.state, bytes);
    Interp I;
    interp_init(&I, p, st);
    I.step_limit = 3e7;
    StrBuf printed = {0}, sb = {0};
    I.out = &printed;
    float lift[4] = {re, im, 1, 0}; /* (a view of points of the sphere: the pixel's point, exact at infinity) */
    for (int i = 0; i < A.nvis; i++)
        if (A.vis[i].viewer == v) {
            Rect r = A.vis[i].r;
            float x = FS_CLAMP(A.in.mouse_x - r.x, 0.0f, r.w), y = FS_CLAMP(A.in.mouse_y - r.y, 0.0f, r.h);
            I.pixel = viewer_pixel_size(p, st, s, r.w, r.h, x, y);
            if (s->var_proj) viewer_pixel_to_lift(p, st, s, r.w, r.h, x, y, lift);
        }
    float rgb[3];
    bool ok = s->var_proj ? interp_eval_viewer_lift(&I, s, lift, rgb) : interp_eval_viewer(&I, s, re, im, rgb);
    sb_printf(&sb, "View %d, %s, at %s = ", v + 1, s->display ? s->display : s->name, s->var ? s->var : "z");
    if (s->var_proj) {
        u32 a[2] = {fw(lift[0]), fw(lift[1])}, b[2] = {fw(lift[2]), fw(lift[3])}, pt[4];
        proj_make(a, b, pt);
        format_value(t_proj, pt, &sb);
    } else {
        u32 at[2] = {fw(re), fw(im)};
        format_value(t_complex, at, &sb);
    }
    sb_putc(&sb, '\n');
    if (s->flood) sb_puts(&sb, "(The picture carries its solve from pixel to pixel; here it starts from the seed.)\n");
    if (!ok) sb_printf(&sb, "Stopped: %s\n", I.error);
    if (printed.len) sb_printf(&sb, "%s", sb_cstr(&printed));
    sb_puts(&sb, "The values at the end:\n");
    for (int i = 0; i < s->locals.len; i++) {
        Sym *l = s->locals.data[i];
        if (l == s->color || l->name[0] == '_' || l->kind != SYM_LOCAL) continue;
        sb_printf(&sb, "  %s : %s = ", l->name, type_str(l->type));
        format_value(l->type, I.frame + l->slot, &sb);
        sb_putc(&sb, '\n');
    }
    if (s->color) {
        sb_puts(&sb, "  color = ");
        format_value(s->color->type, I.frame + s->color->slot, &sb);
        sb_putc(&sb, '\n');
    }
    sb_printf(&sb, "  .iterations = %d (the last loop)\n  .stuck = %s\n", (int)I.iterations, I.stuck ? "true" : "false");
    out_set(out_inspect(), sb_cstr(&sb), false);
    A.info_open = true;
    A.inspect_viewer = v;
    A.inspect_at[0] = re;
    A.inspect_at[1] = im;
    sb_free(&sb);
    sb_free(&printed);
    interp_free(&I);
    free(st);
}

static void zoom_at(int v, Rect r, float x, float y, float factor) {
    Section *s = A.prog->viewers[v];
    if (s->mobius) { /* hyperbolic map fixing the point under the mouse and its antipode */
        Cx p = screen_to_w(r, x, y);
        Mob t = mob_to_zero(p), k = {cx(factor, 0), cx(0, 0), cx(0, 0), cx(1, 0)};
        mob_set(s->mobius, mob_mul(mob_get(s->mobius), mob_mul(mob_inv(t), mob_mul(k, t))));
        return;
    }
    double re, im, cx, cy;
    plane_of_d(v, r, x, y, &re, &im);
    view_center_d(v, &cx, &cy);
    set_view_center_d(v, re + (cx - re) * factor, im + (cy - im) * factor);
    A.state[s->radius->slot] = fw(wf(A.state[s->radius->slot]) * factor);
}

static void pan_by(int v, Rect r, float dx, float dy) {
    Section *s = A.prog->viewers[v];
    if (s->mobius) { /* rotate the sphere so the point under the mouse follows it */
        float mx = A.in.mouse_x, my = A.in.mouse_y;
        Cx p0 = screen_to_w(r, mx - dx, my - dy), p1 = screen_to_w(r, mx, my);
        Mob rot = mob_mul(mob_inv(mob_to_zero(p0)), mob_to_zero(p1));
        mob_set(s->mobius, mob_mul(mob_get(s->mobius), rot));
        return;
    }
    double sc = view_scale(v, r), cx, cy;
    view_center_d(v, &cx, &cy);
    set_view_center_d(v, cx - dx * sc, cy + dy * sc);
}

static void reset_view(int v) {
    Section *s = A.prog->viewers[v];
    nav_begin(v, nav_new_gesture());
    if (s->mobius) memcpy(A.state + s->mobius->slot, A.prog->initial_state + s->mobius->slot, 32);
    memcpy(A.state + s->center->slot, A.prog->initial_state + s->center->slot, 8);
    if (s->center_lo) memcpy(A.state + s->center_lo->slot, A.prog->initial_state + s->center_lo->slot, 8);
    memcpy(A.state + s->radius->slot, A.prog->initial_state + s->radius->slot, 4);
}

/* ---- dropping a view somewhere -------------------------------------------------------- */
/* Where a view dragged to (x, y) goes: DROP_FLOAT (*leaf: the corner), DROP_REPLACE or a side of a tile (*leaf: its
   node).  Only where no pane covers the views; the sides are those of the part of the tile that shows (a new split
   goes in its middle), and *hl is the part of the new place that will show. */
static int drop_zone(float x, float y, int *leaf, Rect *hl) {
    Rect ar = A.free;
    if (!in_rect(ar, x, y)) return DROP_NONE;
    float cs = 0.16f * FS_MIN(ar.w, ar.h);
    for (int c = 0; c < 4; c++) {
        float cx = c & 1 ? ar.x + ar.w - cs : ar.x, cy = c & 2 ? ar.y + ar.h - cs : ar.y;
        if (x >= cx && x < cx + cs && y >= cy && y < cy + cs) {
            *leaf = c;
            *hl = float_rect(c);
            return DROP_FLOAT;
        }
    }
    for (int i = 0; i < A.nvis; i++) {
        VisTile *t = &A.vis[i];
        if (t->fl >= 0 || !in_rect(t->vr, x, y)) continue;
        Rect r = t->vr;
        float u = (x - r.x) / r.w, v = (y - r.y) / r.h;
        *leaf = t->node;
        if (u > 0.3f && u < 0.7f && v > 0.3f && v < 0.7f) {
            *hl = r;
            return DROP_REPLACE;
        }
        float dl = u, dr = 1 - u, dt = v, db = 1 - v;
        float m = FS_MIN(FS_MIN(dl, dr), FS_MIN(dt, db));
        if (m == dl) { *hl = (Rect){r.x, r.y, r.w / 2, r.h}; return DROP_LEFT; }
        if (m == dr) { *hl = (Rect){r.x + r.w / 2, r.y, r.w / 2, r.h}; return DROP_RIGHT; }
        if (m == dt) { *hl = (Rect){r.x, r.y, r.w, r.h / 2}; return DROP_TOP; }
        *hl = (Rect){r.x, r.y + r.h / 2, r.w, r.h / 2};
        return DROP_BOTTOM;
    }
    return DROP_NONE;
}

static void apply_drop(int viewer, float x, float y) {
    int leaf = -1;
    Rect hl;
    int z = drop_zone(x, y, &leaf, &hl);
    if (z == DROP_NONE) return;
    if (z == DROP_FLOAT) {
        if (find_leaf(A.root, viewer) >= 0 && count_shown(A.root) == 1) {
            set_status(true, "The last view on screen can't float");
            return;
        }
        detach_viewer(viewer);
        A.floats[leaf] = viewer;
        A.focus_viewer = viewer;
        return;
    }
    if (z == DROP_REPLACE) show_in_leaf(leaf, viewer);
    else split_leaf(leaf, z, viewer);
}

/* ---- viewer interaction ------------------------------------------------------------ */
static int tile_layer(const VisTile *t) { return t->fl >= 0 ? UI_LAYER_PANEL : UI_LAYER_BASE; }

static void viewer_interaction(void) {
    const UiInput *in = &A.in;
    float mx = in->mouse_x, my = in->mouse_y;
    Program *p = A.prog;
    if (A.drag == DRAG_VIEWER) {
        ui_set_mouse_captured(true);
        if (in->mouse_released[0]) {
            apply_drop(A.drag_viewer, mx, my);
            A.drag = DRAG_NONE;
        }
        return;
    }
    if (A.drag == DRAG_SPLIT) {
        ui_set_mouse_captured(true);
        LNode *nd = &A.nodes[A.drag_node];
        float t = nd->kind == N_HSPLIT ? (mx - nd->r.x) / nd->r.w : (my - nd->r.y) / nd->r.h;
        nd->ratio = FS_CLAMP(t, 0.1f, 0.9f);
        platform_set_cursor(nd->kind == N_HSPLIT ? UI_CURSOR_HRESIZE : UI_CURSOR_VRESIZE);
        if (!in->mouse_down[0]) A.drag = DRAG_NONE;
        return;
    }
    /* which tile is under the mouse (floats first) */
    int hit = A.hover_tile;
    bool over = false;
    if (hit >= 0) {
        int prev = ui_layer(tile_layer(&A.vis[hit]));
        over = ui_mouse_over(A.vis[hit].r.x, A.vis[hit].r.y, A.vis[hit].r.w, A.vis[hit].r.h);
        ui_layer(prev);
    }
    A.hover_viewer = over ? A.vis[hit].viewer : -1;

    if (A.drag != DRAG_NONE) {
        ui_set_mouse_captured(true);
        VisTile *t = NULL;
        for (int i = 0; i < A.nvis; i++)
            if (A.vis[i].viewer == A.drag_viewer) t = &A.vis[i];
        if (!t) {
            A.drag = DRAG_NONE;
            return;
        }
        float dx = mx - A.last_mx, dy = my - A.last_my;
        if (fabsf(mx - A.press_x) + fabsf(my - A.press_y) > px(4)) A.moved = true;
        float re, im;
        plane_of(t->viewer, t->r, mx, my, &re, &im);
        bool any_down = in->mouse_down[0] || in->mouse_down[1] || in->mouse_down[2];
        switch (A.drag) {
        case DRAG_PAN:
            if (dx || dy) {
                nav_begin(t->viewer, A.drag_gesture);
                pan_by(t->viewer, t->r, dx, dy);
            }
            platform_set_cursor(UI_CURSOR_MOVE);
            break;
        case DRAG_PICK:
            if (dx || dy) pick(t->viewer, re, im);
            platform_set_cursor(UI_CURSOR_CROSSHAIR);
            break;
        case DRAG_INSPECT:
            if (dx || dy) inspect_at(t->viewer, re, im);
            platform_set_cursor(UI_CURSOR_CROSSHAIR);
            break;
        case DRAG_TOOL:
            if ((dx || dy) && tool_handles(A.drag_tool, TOOL_EV_DRAG)) run_tool(A.drag_tool, t->viewer, TOOL_EV_DRAG, re, im);
            if (!any_down) {
                if (tool_handles(A.drag_tool, TOOL_EV_RELEASE)) run_tool(A.drag_tool, t->viewer, TOOL_EV_RELEASE, re, im);
                if (!A.moved && tool_handles(A.drag_tool, TOOL_EV_CLICK)) run_tool(A.drag_tool, t->viewer, TOOL_EV_CLICK, re, im);
            }
            platform_set_cursor(UI_CURSOR_CROSSHAIR);
            break;
        case DRAG_CLICK: /* a tool with .panning: dragging pans, a click runs the tool */
            if (A.moved && (dx || dy)) {
                nav_begin(t->viewer, A.drag_gesture);
                pan_by(t->viewer, t->r, dx, dy);
            }
            if (in->mouse_released[0] && !A.moved && A.drag_tool >= 0 && A.drag_tool < p->ntools) {
                static const int evs[3] = {TOOL_EV_PRESS, TOOL_EV_RELEASE, TOOL_EV_CLICK};
                for (int k = 0; k < 3; k++)
                    if (tool_handles(A.drag_tool, evs[k])) run_tool(A.drag_tool, t->viewer, evs[k], re, im);
            }
            break;
        }
        if (!any_down) A.drag = DRAG_NONE;
        A.last_mx = mx;
        A.last_my = my;
        return;
    }
    /* splitters between tiles */
    int sp = in_rect(A.area, mx, my) ? splitter_at(A.root, mx, my) : -1;
    if (sp >= 0 && !(hit >= 0 && A.vis[hit].fl >= 0) && ui_mouse_over(mx - 1, my - 1, 2, 2)) {
        platform_set_cursor(A.nodes[sp].kind == N_HSPLIT ? UI_CURSOR_HRESIZE : UI_CURSOR_VRESIZE);
        if (in->mouse_pressed[0]) {
            A.drag = DRAG_SPLIT;
            A.drag_node = sp;
        }
        return;
    }
    if (!over) return;
    VisTile *t = &A.vis[hit];
    int v = t->viewer;
    ViewerRT *rt = &A.vrt[v];
    float re, im;
    plane_of(v, t->r, mx, my, &re, &im);
    if (re != wf(A.state[p->s_mouse->slot]) || im != wf(A.state[p->s_mouse->slot + 1])) {
        set_c(p->s_mouse, re, im);
        if (tool_handles(rt->tool, TOOL_EV_HOVER) && (mx != A.last_mx || my != A.last_my)) run_tool(rt->tool, v, TOOL_EV_HOVER, re, im);
    }
    if (in->scroll_y != 0) {
        nav_begin(v, NAV_WHEEL);
        zoom_at(v, t->r, mx, my, powf(0.85f, in->scroll_y));
    }
    if (in->mouse_back || in->mouse_forward) nav_key(v, in->mouse_forward);
    /* the border of a floating view: drag it somewhere else */
    if (t->fl >= 0) {
        float b = px(7);
        Rect inner = {t->r.x + b, t->r.y + b, t->r.w - 2 * b, t->r.h - 2 * b};
        if (!in_rect(inner, mx, my)) {
            platform_set_cursor(UI_CURSOR_MOVE);
            if (in->mouse_pressed[0]) {
                A.drag = DRAG_VIEWER;
                A.drag_viewer = v;
                A.press_x = mx;
                A.press_y = my;
            }
            return;
        }
    }
    bool space = in->key_down[UI_KEY_SPACE] && !ui_wants_keyboard();
    int button = in->mouse_pressed[0] ? 0 : in->mouse_pressed[1] ? 1 : in->mouse_pressed[2] ? 2 : -1;
    platform_set_cursor(rt->tool == TOOL_PAN || space ? UI_CURSOR_HAND : UI_CURSOR_CROSSHAIR);
    if (button < 0) return;
    A.focus_viewer = v;
    A.drag_viewer = v;
    A.press_x = A.last_mx = mx;
    A.press_y = A.last_my = my;
    A.moved = false;
    A.drag_tool = rt->tool;
    A.drag_gesture = nav_new_gesture();
    if (button != 0 || space || rt->tool == TOOL_PAN) {
        A.drag = DRAG_PAN;
        if (button == 0) set_c(p->s_last_click, re, im);
    } else if (rt->tool == TOOL_PICK) {
        A.drag = DRAG_PICK;
        pick(v, re, im);
    } else if (rt->tool == TOOL_INSPECT) {
        A.drag = DRAG_INSPECT;
        set_c(p->s_last_click, re, im);
        inspect_at(v, re, im);
    } else if (p->tools[rt->tool]->panning) {
        A.drag = DRAG_CLICK;
        set_c(p->s_drag_start, re, im);
    } else {
        A.drag = DRAG_TOOL;
        set_c(p->s_drag_start, re, im);
        if (tool_handles(rt->tool, TOOL_EV_PRESS)) run_tool(rt->tool, v, TOOL_EV_PRESS, re, im);
    }
    ui_set_mouse_captured(true);
}

/* is a word of the state part of a view (the centre, radius or view map of a viewer)? */
static bool view_word(Program *p, int w) {
    for (int v = 0; v < p->nviewers; v++) {
        Sym *syms[3] = {p->viewers[v]->center, p->viewers[v]->radius, p->viewers[v]->mobius};
        for (int k = 0; k < 3; k++)
            if (syms[k] && w >= syms[k]->slot && w < syms[k]->slot + syms[k]->type->size) return true;
    }
    return false;
}

/* `on show`: runs when its tool is selected, and again whenever the state changes while it is (not for
   the mouse, the time or the last click, which change all the time).  While a view is being panned or
   zoomed, a change of the views alone waits until the view rests (a slow `on show` would make the
   panning stutter). */
static void tools_show(void) {
    Program *p = A.prog;
    size_t bytes = sizeof(u32) * (size_t)p->state_words;
    Sym *volatile_syms[4] = {p->s_mouse, p->s_time, p->s_last_click, p->s_drag_start};
    static u32 *views_prev;
    static size_t views_bytes;
    static double views_moved_at;
    double now = fs_time_seconds();
    if (views_bytes != bytes) {
        free(views_prev);
        views_prev = (u32 *)xmalloc(bytes);
        memcpy(views_prev, A.state, bytes);
        views_bytes = bytes;
    }
    for (int w = 0; w < p->state_words; w++)
        if (views_prev[w] != A.state[w] && view_word(p, w)) {
            views_moved_at = now;
            break;
        }
    memcpy(views_prev, A.state, bytes);
    for (int i = 0; i < A.nvis; i++) {
        int v = A.vis[i].viewer;
        ViewerRT *rt = &A.vrt[v];
        if (!tool_handles(rt->tool, TOOL_EV_SHOW)) continue;
        if (rt->show_snap) {
            for (int k = 0; k < 4; k++)
                memcpy(rt->show_snap + volatile_syms[k]->slot, A.state + volatile_syms[k]->slot,
                       sizeof(u32) * (size_t)volatile_syms[k]->type->size);
            if (memcmp(rt->show_snap, A.state, bytes) == 0) continue;
            if (now - views_moved_at < 0.3) {
                bool only_views = true;
                for (int w = 0; w < p->state_words && only_views; w++)
                    if (rt->show_snap[w] != A.state[w] && !view_word(p, w)) only_views = false;
                if (only_views) {
                    ui_wake_in(0.35);
                    continue;
                }
            }
        } else {
            rt->show_snap = (u32 *)xmalloc(bytes);
        }
        run_tool_at_center(rt->tool, v, TOOL_EV_SHOW);
        memcpy(rt->show_snap, A.state, bytes);
    }
}

/* ---- drawing tiles ------------------------------------------------------------------ */
static u32 rgb_u32(const float c[3], float a) {
    int r = (int)(FS_CLAMP(c[0], 0.0f, 1.0f) * 255 + 0.5f), g = (int)(FS_CLAMP(c[1], 0.0f, 1.0f) * 255 + 0.5f),
        b = (int)(FS_CLAMP(c[2], 0.0f, 1.0f) * 255 + 0.5f);
    return UI_RGBA(r, g, b, (int)(a * 255));
}

static void draw_items(VisTile *t, const DrawItemVec *items) {
    float sc = view_scale(t->viewer, t->r);
    u32 shadow = UI_RGBA(0, 0, 0, 140);
    for (int k = 0; k < items->len; k++) {
        const DrawItem *d = &items->data[k];
        if (d->viewer != t->viewer) continue;
        u32 col = rgb_u32(d->color, 1.0f);
        float x0, y0, x1, y1;
        pixel_of(t->viewer, t->r, d->p0[0], d->p0[1], &x0, &y0);
        float w = FS_MAX(px(d->width * 0.5f), 1.0f);
        switch (d->shape) {
        case DRAW_LINE:
            pixel_of(t->viewer, t->r, d->p1[0], d->p1[1], &x1, &y1);
            if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f && fabsf(x1) < 1e6f && fabsf(y1) < 1e6f)) break;
            ui_line(x0, y0, x1, y1, w + px(1.5f), shadow);
            ui_line(x0, y0, x1, y1, w, col);
            break;
        case DRAW_POINT:
            if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f)) break;
            ui_disk(x0, y0, px(4.5f), shadow);
            ui_disk(x0, y0, px(3.2f), col);
            break;
        case DRAW_CIRCLE: case DRAW_DISK: {
            float r = d->radius_px ? px(d->radius) : d->radius / sc;
            if (!d->radius_px && A.prog->viewers[t->viewer]->mobius) {
                float xr, yr;
                pixel_of(t->viewer, t->r, d->p0[0] + d->radius, d->p0[1], &xr, &yr);
                r = sqrtf((xr - x0) * (xr - x0) + (yr - y0) * (yr - y0));
            }
            if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f) || !(r < 1e6f)) break;
            if (d->shape == DRAW_DISK) {
                ui_disk(x0, y0, r, col);
            } else {
                ui_circle(x0, y0, r, w + px(1.5f), shadow);
                ui_circle(x0, y0, r, w, col);
            }
            break;
        }
        case DRAW_TEXT: { /* left end of the baseline... simply: the text starts just right of the point */
            if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f) || !d->text[0]) break;
            float lh = ui_font_line_height(UI_FONT_SANS);
            float tx = x0 + px(4), ty = y0 - lh / 2;
            ui_text(UI_FONT_SANS, tx + px(1), ty + px(1), d->text, -1, shadow);
            ui_text(UI_FONT_SANS, tx, ty, d->text, -1, col);
            break;
        }
        }
    }
}

/* a value of the grid: `dec` decimals, "0" for zero */
static void grid_label(char *out, int cap, double v, int dec, bool imag) {
    if (fabs(v) >= 1e9 || (dec > 12 && fabs(v) >= 1e4)) snprintf(out, (size_t)cap, "%.6g", v);
    else snprintf(out, (size_t)cap, "%.*f", dec, v);
    if (!strcmp(out, "-0") || strspn(out, "-0.") == strlen(out)) snprintf(out, (size_t)cap, "0");
    if (imag) strncat(out, "i", (size_t)cap - strlen(out) - 1);
}

/* The axes and grid lines of a flat view, with their values along the bottom and left edges: lines at 1, 2 or 5
   times a power of 10, far enough apart for their values. */
static void draw_grid(VisTile *t) {
    if (A.prog->viewers[t->viewer]->mobius) return;
    Rect r = t->r;
    double cx, cy, sc = view_scale(t->viewer, r); /* (plane units per pixel) */
    view_center_d(t->viewer, &cx, &cy);
    if (!(sc > 0) || !isfinite(sc) || !isfinite(cx) || !isfinite(cy)) return;
    double xl = cx - 0.5 * r.w * sc, xr = cx + 0.5 * r.w * sc, yb = cy - 0.5 * r.h * sc, yt = cy + 0.5 * r.h * sc;
    float cw = ui_text_width(UI_FONT_MONO, "0", 1), lh = ui_font_line_height(UI_FONT_MONO);
    double step = 0;
    int dec = 0;
    for (double e = pow(10, floor(log10(sc * px(60)))); step == 0 && e < 1e300; e *= 10) /* the smallest that fits */
        for (int k = 0; k < 3 && step == 0; k++) {
            double st = e * (k == 0 ? 1 : k == 1 ? 2 : 5);
            int d = FS_CLAMP((int)ceil(-log10(st) - 1e-9), 0, 17);
            char a[64], b[64];
            grid_label(a, sizeof a, xl, d, false);
            grid_label(b, sizeof b, xr, d, false);
            float need = (float)(FS_MAX(strlen(a), strlen(b)) + 2) * cw + px(16);
            if (st / sc >= FS_MAX(need, px(60))) {
                step = st;
                dec = d;
            }
        }
    if (step == 0 || (xr - xl) / step > 400 || (yt - yb) / step > 400) return;
    u32 line = UI_RGBA(255, 255, 255, 46), axis = UI_RGBA(255, 255, 255, 150), ink = UI_RGBA(255, 255, 255, 215),
        shadow = UI_RGBA(0, 0, 0, 170);
    char label[80];
    Rect vr = t->vr; /* (the values go along the edges of the part that shows) */
    for (double k = ceil(xl / step); k * step <= xr; k++) { /* vertical lines, values along the bottom */
        float X = (float)(r.x + 0.5 * r.w + (k * step - cx) / sc);
        ui_rect(X - px(0.5f), r.y, FS_MAX(px(1), 1.0f), r.h, k == 0 ? axis : line);
        if (X < vr.x || X >= vr.x + vr.w) continue;
        grid_label(label, sizeof label, k * step, dec, false);
        float ty = vr.y + vr.h - lh - px(3);
        ui_push_clip(vr.x, vr.y, vr.w, vr.h);
        ui_text(UI_FONT_MONO, X + px(4), ty + px(1), label, -1, shadow);
        ui_text(UI_FONT_MONO, X + px(3), ty, label, -1, ink);
        ui_pop_clip();
    }
    for (double k = ceil(yb / step); k * step <= yt; k++) { /* horizontal lines, values along the left */
        float Y = (float)(r.y + 0.5 * r.h - (k * step - cy) / sc);
        ui_rect(r.x, Y - px(0.5f), r.w, FS_MAX(px(1), 1.0f), k == 0 ? axis : line);
        if (vr.w < px(60)) continue;
        grid_label(label, sizeof label, k * step, dec, true);
        float tx = vr.x + px(40);
        ui_push_clip(vr.x, vr.y, vr.w, vr.h);
        ui_text(UI_FONT_MONO, tx + px(1), Y - lh - px(1) + px(1), label, -1, shadow);
        ui_text(UI_FONT_MONO, tx, Y - lh - px(1), label, -1, ink);
        ui_pop_clip();
    }
}

/* the picture, the drawings, the picked point and the name */
/* A view zoomed in beyond single precision while .doublePrecision is off: a note at its bottom says so, and a click
 * on it turns double precision on (it's a setting of the config panel, written into the script with the others). */
static void deep_note(VisTile *t) {
    Program *p = A.prog;
    ViewerRT *rt = &A.vrt[t->viewer];
    Section *s = p->viewers[t->viewer];
    if (s->mobius || A.state[p->s_double->slot] || !rt->target || !viewer_deep(p, A.state, s, rt->tw, rt->th, false)) return;
    const char *text = "Finer than single precision: click here for double precision (slower)";
    float lh = ui_font_line_height(UI_FONT_SANS), tw = ui_text_width(UI_FONT_SANS, text, -1) + px(20), th = lh + px(10);
    Rect r = t->vr; /* (in the part of the view that shows) */
    if (r.w < px(80) || r.h < th + px(60)) return;
    tw = FS_MIN(tw, r.w - px(12));
    float x = r.x + (r.w - tw) / 2, y = r.y + r.h - th - px(40);
    int prev = ui_layer(UI_LAYER_MENU); /* (above floating views too) */
    ui_push_clip(r.x, r.y, r.w, r.h);
    char id[32];
    snprintf(id, sizeof id, "##deep_note%d", t->viewer);
    UiInteract it = ui_interact(id, x, y, tw, th);
    ui_rrect(x, y, tw, th, px(6), it.hovered ? UI_RGBA(60, 90, 140, 235) : UI_RGBA(28, 36, 52, 225));
    ui_rrect_outline(x, y, tw, th, px(6), px(1), UI_COL_ACCENT);
    ui_label(x + px(10), y, tw - px(20), th, text, UI_ALIGN_CENTER, UI_COL_TEXT);
    if (it.hovered) {
        ui_set_cursor(UI_CURSOR_HAND);
        ui_tooltip("Turns on .doublePrecision (in the config panel): views zoomed in this far are computed with pairs of "
                   "floats, about 14 digits, sharp down to a radius of about 1e-12 times the size of the numbers");
    }
    if (it.clicked) {
        A.state[p->s_double->slot] = 1;
        set_status(false, "Double precision is on (.doublePrecision in the config panel)");
    }
    ui_pop_clip();
    ui_layer(prev);
}

static void draw_tile(VisTile *t) {
    Program *p = A.prog;
    ViewerRT *rt = &A.vrt[t->viewer];
    Rect r = t->r;
    ui_push_clip(r.x, r.y, r.w, r.h);
    if (rt->target) ui_image(rt->target, r.x, r.y, (float)rt->tw, (float)rt->th);
    if (rt->grid) draw_grid(t);
    for (int tool = 0; tool < p->ntools; tool++) draw_items(t, &A.draws[tool]);
    Section *s = p->viewers[t->viewer];
    if (rt->picked && s->picked) {
        float x, y;
        pixel_of(t->viewer, r, wf(A.state[s->picked->slot]), wf(A.state[s->picked->slot + 1]), &x, &y);
        ui_circle(x, y, px(6), px(3), UI_RGBA(0, 0, 0, 150));
        ui_circle(x, y, px(6), px(1.5f), UI_RGBA(255, 255, 255, 230));
    }
    if (A.inspect_viewer == t->viewer && rt->tool == TOOL_INSPECT) { /* the point Inspect looked at */
        float x, y, a = px(5), b = px(10);
        pixel_of(t->viewer, r, A.inspect_at[0], A.inspect_at[1], &x, &y);
        for (int k = 0; k < 2; k++) {
            float th = k ? px(1.5f) : px(4);
            u32 col = k ? UI_RGBA(255, 255, 255, 235) : UI_RGBA(0, 0, 0, 150);
            ui_line(x - b, y, x - a, y, th, col);
            ui_line(x + a, y, x + b, y, th, col);
            ui_line(x, y - b, x, y - a, th, col);
            ui_line(x, y + a, x, y + b, th, col);
        }
    }
    bool several = A.nvis > 1;
    Rect vr = t->vr; /* its labels go in the part that shows */
    ui_push_clip(vr.x, vr.y, vr.w, vr.h);
    if ((p->nviewers > 1 || t->fl >= 0) && vr.w > px(30)) { /* its number (its button in the left bar) and its name */
        const char *name = s->display ? s->display : s->name;
        char num[16];
        snprintf(num, sizeof num, "%d", t->viewer + 1);
        float nw = ui_text_width(UI_FONT_SANS, num, -1) + px(8);
        float tw = nw + ui_text_width(UI_FONT_SANS, name, -1) + px(12), th = ui_font_line_height(UI_FONT_SANS) + px(4);
        bool full = tw <= vr.w - px(12); /* (too narrow for the name: the number alone) */
        if (!full) tw = nw + px(4);
        float lx = vr.x + vr.w - tw - px(6), ly = vr.y + px(6); /* (top right: the coordinates are at the bottom) */
        ui_rrect(lx, ly, tw, th, px(4), UI_RGBA(0, 0, 0, 120));
        ui_text(UI_FONT_SANS, lx + px(6), ly + px(2), num, -1, ui_alpha(UI_COL_ACCENT_HOVER, 0.9f));
        if (full) ui_text(UI_FONT_SANS, lx + px(6) + nw, ly + px(2), name, -1, UI_RGBA(255, 255, 255, 200));
    }
    /* where the mouse is, in the plane (as many decimals as the pixels resolve) */
    if (A.hover_tile >= 0 && &A.vis[A.hover_tile] == t && A.in.mouse_in_window && A.drag != DRAG_VIEWER &&
        in_rect(vr, A.in.mouse_x, A.in.mouse_y)) {
        double re, im, pxs;
        if (s->mobius) {
            float fr, fi, fr1, fi1;
            plane_of(t->viewer, r, A.in.mouse_x, A.in.mouse_y, &fr, &fi);
            plane_of(t->viewer, r, A.in.mouse_x + 1, A.in.mouse_y, &fr1, &fi1);
            re = fr;
            im = fi;
            pxs = hypot(fr1 - fr, fi1 - fi);
        } else { /* (in double precision: deep zooms) */
            plane_of_d(t->viewer, r, A.in.mouse_x, A.in.mouse_y, &re, &im);
            pxs = view_scale(t->viewer, r);
        }
        int d = pxs > 0 && pxs == pxs ? (int)ceil(-log10(pxs)) + 1 : 4;
        d = FS_CLAMP(d, 1, 16);
        char co[200];
        const char *deep = A.vrt[t->viewer].df ? "   (double precision)" : "";
        if (re == re && im == im && fabs(re) < 1e7 && fabs(im) < 1e7)
            snprintf(co, sizeof co, "%.20s = %.*f %c %.*fi%s", s->var ? s->var : "z", d, re, im < 0 ? '-' : '+', d, fabs(im),
                     deep);
        else
            snprintf(co, sizeof co, "%.20s = %.4g %c %.4gi", s->var ? s->var : "z", re, im < 0 ? '-' : '+', fabs(im));
        float tw = ui_text_width(UI_FONT_MONO, co, -1) + px(12), th = ui_font_line_height(UI_FONT_MONO) + px(4);
        float bx = vr.x + px(6), by = vr.y + vr.h - th - px(6);
        ui_rrect(bx, by, tw, th, px(4), UI_RGBA(0, 0, 0, 120));
        ui_text(UI_FONT_MONO, bx + px(6), by + px(2), co, -1, UI_RGBA(255, 255, 255, 210));
    }
    if (t->fl >= 0) ui_rect_outline(r.x, r.y, r.w, r.h, px(2), A.hover_viewer == t->viewer ? UI_COL_ACCENT : UI_COL_BORDER);
    else if (several && A.focus_viewer == t->viewer) ui_rect_outline(vr.x, vr.y, vr.w, vr.h, px(1), UI_COL_ACCENT_DIM);
    ui_pop_clip();
    ui_pop_clip();
}

static void ensure_target(int v, int w, int h) {
    ViewerRT *rt = &A.vrt[v];
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (rt->target && rt->tw == w && rt->th == h) return;
    if (rt->target) gfx_target_destroy(rt->target);
    rt->target = gfx_target_create(w, h);
    rt->tw = w;
    rt->th = h;
    rt->rendered = false;
}

/* ---- the bars at the window's edges ---------------------------------------------------------------------------------
 * Left: the script's button, then a section for each viewer of the script, in its order: the view's button (its
 * number; a click shows the view in place of the focused one, or makes it the focused one; dragging it onto the views
 * places it: the middle of a view replaces it, its sides split it, the corners float) and, while the view is on screen,
 * its tools, the selected one highlighted.  Right: the config's button, then what can be done with the focused view
 * (the one clicked last): back, forward, reset, copy and paste its location, axes and grid, pictures, hide; the keys
 * at the bottom.  The bars stay when the panes next to them open, so their buttons never move away from the mouse. */
enum { BI_SEP, BI_SCRIPT, BI_VIEW, BI_TOOL, BI_CONFIG, BI_INFO, BI_FOCUS, BI_ACTION, BI_HELP };
enum { ACT_BACK, ACT_FORWARD, ACT_RESET, ACT_COPY, ACT_PASTE, ACT_GRID, ACT_SAVE, ACT_SAVE_LARGE, ACT_HIDE, ACT_COUNT };
typedef struct BarItem {
    int kind;
    int viewer;  /* BI_VIEW, BI_TOOL, BI_FOCUS, BI_ACTION */
    int what;    /* BI_TOOL: the tool; BI_ACTION: ACT_* */
    float y, h;  /* from the top of the bar's contents */
} BarItem;
#define BAR_ITEMS 200

static float bar_width(void) { return px(44); }
static float bar_button(void) { return px(34); }

/* the view the right bar acts on: the focused one if it is on screen, else the first one shown */
static int bar_view(void) {
    if (A.focus_viewer >= 0 && A.focus_viewer < A.prog->nviewers && viewer_visible(A.focus_viewer)) return A.focus_viewer;
    return A.nvis > 0 ? A.vis[0].viewer : -1;
}

/* can view v be hidden (it floats, or it isn't the last tile)? */
static bool view_hideable(int v) {
    return viewer_visible(v) && (find_float(v) >= 0 || (find_leaf(A.root, v) >= 0 && count_shown(A.root) > 1));
}

static void hide_view(int v) {
    if (!view_hideable(v)) return;
    int f = find_float(v), leaf = find_leaf(A.root, v);
    if (f >= 0) A.floats[f] = -1;
    else remove_leaf(leaf);
    if (A.focus_viewer == v) /* the focus goes to a view still shown */
        for (int w = 0; w < A.prog->nviewers; w++)
            if (viewer_visible(w)) {
                A.focus_viewer = w;
                break;
            }
}

/* show view v in place of the focused view (or where the focused view is, if it floats) */
static void show_view(int v) {
    if (A.vrt[v].gone) return; /* (its section is gone from the script) */
    if (viewer_visible(v)) {
        A.focus_viewer = v;
        return;
    }
    int leaf = find_leaf(A.root, A.focus_viewer), f = find_float(A.focus_viewer);
    if (leaf >= 0 && !leaf_gone(leaf)) show_in_leaf(leaf, v);
    else if (f >= 0) A.floats[f] = v;
    else if (A.root < 0) { /* (nothing on screen) */
        A.root = node_alloc();
        A.nodes[A.root].kind = N_LEAF;
        A.nodes[A.root].viewer = v;
    } else { /* (no focused view on screen: the first tile, which may be closed) */
        int leaves[MAX_NODES];
        collect_leaves(A.root, leaves, 0);
        show_in_leaf(leaves[0], v);
    }
    A.focus_viewer = v;
}

static int bar_add(BarItem *it, int n, int kind, int viewer, int what, float *y, float h) {
    if (n >= BAR_ITEMS) return n;
    it[n] = (BarItem){kind, viewer, what, *y, h};
    *y += h;
    return n + 1;
}

static int left_bar_items(BarItem *it, float *total) {
    float b = bar_button() + px(3), sep = px(11), y = px(6);
    int n = bar_add(it, 0, BI_SCRIPT, -1, 0, &y, b);
    for (int v = 0; A.prog && v < A.prog->nviewers; v++) {
        n = bar_add(it, n, BI_SEP, -1, 0, &y, sep);
        n = bar_add(it, n, BI_VIEW, v, 0, &y, b);
        if (!viewer_visible(v)) continue;
        int tools[70], nt = viewer_tools(v, tools, 70);
        for (int k = 0; k < nt; k++) n = bar_add(it, n, BI_TOOL, v, tools[k], &y, b);
    }
    *total = y + px(6);
    return n;
}

static int right_bar_items(BarItem *it, float *total) {
    float b = bar_button() + px(3), sep = px(11), y = px(6);
    int n = bar_add(it, 0, BI_CONFIG, -1, 0, &y, b);
    n = bar_add(it, n, BI_INFO, -1, 0, &y, b);
    int v = A.prog ? bar_view() : -1;
    if (v >= 0) {
        bool flat = !A.prog->viewers[v]->mobius;
        n = bar_add(it, n, BI_SEP, -1, 0, &y, sep);
        n = bar_add(it, n, BI_FOCUS, v, 0, &y, b);
        for (int k = 0; k < ACT_COUNT; k++) {
            if (!flat && (k == ACT_COPY || k == ACT_PASTE || k == ACT_GRID)) continue;
            if (k == ACT_HIDE && !view_hideable(v)) continue;
            n = bar_add(it, n, BI_ACTION, v, k, &y, b);
        }
    }
    n = bar_add(it, n, BI_SEP, -1, 0, &y, sep);
    n = bar_add(it, n, BI_HELP, -1, 0, &y, b);
    *total = y + px(6);
    return n;
}

static const char *view_name(int v) {
    Section *s = A.prog->viewers[v];
    return s->display ? s->display : s->name;
}

/* a view's number in a rounded square: its place in the script (1, 2, ...) */
static void view_badge(int v, float x, float y, float b, u32 fill, u32 ink) {
    char num[16];
    snprintf(num, sizeof num, "%d", v + 1);
    if (fill) ui_rrect(x, y, b, b, px(6), fill);
    ui_label(x, y, b, b, num, UI_ALIGN_CENTER, ink);
    if (A.prog->viewers[v]->projective) ui_circle(x + b / 2, y + b / 2, b * 0.36f, px(1), ui_alpha(ink, 0.6f));
}

/* a view's button in the left bar */
static void bar_view_button(int v, float x, float y, float b) {
    bool shown = viewer_visible(v);
    char id[32], tip[512], sc[48];
    snprintf(id, sizeof id, "##bar_view%d", v);
    UiInteract it = ui_interact(id, x, y, b, b);
    if (A.vrt[v].gone) { /* its section is gone from the script (which has errors): closed, struck out */
        view_badge(v, x, y, b, 0, UI_COL_TEXT_OFF);
        ui_line(x + b * 0.25f, y + b * 0.75f, x + b * 0.75f, y + b * 0.25f, px(1.5f), UI_COL_TEXT_OFF);
        if (it.hovered) {
            snprintf(tip, sizeof tip, "%d: %.200s\nIts section is gone from the script: the view is closed (it comes back "
                     "if its section does)", v + 1, view_name(v));
            ui_tooltip(tip);
        }
        return;
    }
    u32 fill = it.held ? UI_COL_WIDGET_ACTIVE : it.hovered || A.hover_viewer == v ? UI_COL_WIDGET_HOVER : shown ? UI_COL_WIDGET : 0;
    view_badge(v, x, y, b, fill, shown ? UI_COL_TEXT : UI_COL_TEXT_DIM);
    if (shown && A.focus_viewer == v && A.nvis > 1) ui_rrect_outline(x, y, b, b, px(6), px(1.5f), UI_COL_ACCENT);
    if (it.hovered) {
        shortcut_text(viewer_shortcut(v), sc, sizeof sc);
        snprintf(tip, sizeof tip, "%d: %.200s%s%s%s\n%s", v + 1, view_name(v), sc[0] ? " (" : "", sc, sc[0] ? ")" : "",
                 shown ? "Click: the buttons on the right act on this view. Drag it onto the views to move it."
                       : "Click: show it in place of the focused view. Or drag it onto the views: the middle of a view "
                         "replaces it, its sides split it, the corners float.");
        ui_tooltip(tip);
    }
    if (it.held && A.drag == DRAG_NONE && fabsf(A.in.mouse_x - it.press_x) + fabsf(A.in.mouse_y - it.press_y) > px(8)) {
        A.drag = DRAG_VIEWER;
        A.drag_viewer = v;
        ui_reset_interaction();
    } else if (it.clicked) {
        show_view(v);
    }
}

static void bar_tool_button(int v, int tool, float x, float y, float b) {
    Program *p = A.prog;
    char id[48], letter[8], tip[300], sc[48];
    snprintf(id, sizeof id, "##bar_tool%d_%d", v, tool);
    tool_letter(tool, letter);
    if (ui_icon_button_ex(id, tool_icon(tool), letter, x, y, b, A.vrt[v].tool == tool ? UI_SELECTED : UI_FLAT)) {
        select_tool(v, tool);
        A.focus_viewer = v;
    }
    shortcut_text(tool_shortcut(tool), sc, sizeof sc);
    snprintf(tip, sizeof tip, "%.200s%s%s%s%s", tool_name(tool), sc[0] ? " (" : "", sc, sc[0] ? ")" : "",
             tool >= 0 && p->tools[tool]->button ? ": runs once" : "");
    ui_tooltip(tip);
}

static void bar_action(int v, int k, float x, float y, float b) {
    static const char *icons[ACT_COUNT] = {"arrow_back", "arrow_forward", "home", "content_copy", "content_paste",
                                           "grid_on", "image", "photo_size_select_large", "visibility_off"};
    static const char *letters[ACT_COUNT] = {"<", ">", "H", "C", "V", "#", "S", "L", "X"};
    static const char *names[ACT_COUNT] = {
        "Back: where the view was before the last zoom, pan or reset (" BACK_KEY ", the mouse's back button)",
        "Forward: undo Back (" FORWARD_KEY ")",
        "Reset: the view the script starts with (Home)",
        "Copy location: the centre and the radius as script lines (\"mandel.center = ...\"), with all their digits (" MODKEY
        "+Shift+C)",
        "Paste location: go to the centre and radius in the clipboard (copied with Copy location, or a point like "
        "-0.75+0.1i) (" MODKEY "+Shift+V)",
        "Axes and grid lines with their values (" MODKEY "+Shift+A)",
        "Save picture: this view at its size on the screen (PNG, without the tools' drawings)",
        "Save it large: this view rendered again at four times the width and height, for print",
        "Hide this view"};
    char id[32], tip[500];
    snprintf(id, sizeof id, "##bar_act%d", k);
    int flags = UI_FLAT;
    if ((k == ACT_BACK || k == ACT_FORWARD) && nav_count(v, k == ACT_FORWARD) == 0) flags |= UI_DISABLED;
    if (k == ACT_GRID && A.vrt[v].grid) flags |= UI_SELECTED;
    if (ui_icon_button_ex(id, icons[k], letters[k], x, y, b, flags)) {
        switch (k) {
        case ACT_BACK: nav_go(v, false); break;
        case ACT_FORWARD: nav_go(v, true); break;
        case ACT_RESET: reset_view(v); break;
        case ACT_COPY: copy_location(v); break;
        case ACT_PASTE: paste_location(v); break;
        case ACT_GRID: A.vrt[v].grid = !A.vrt[v].grid; break;
        case ACT_SAVE: ask_export(v, 1); break;
        case ACT_SAVE_LARGE: ask_export(v, 4); break;
        case ACT_HIDE: hide_view(v); break;
        }
    }
    if (k == ACT_SAVE || k == ACT_SAVE_LARGE) {
        int ew, eh;
        export_size(v, k == ACT_SAVE ? 1 : 4, &ew, &eh);
        snprintf(tip, sizeof tip, "%s (%d x %d)", names[k], ew, eh);
        ui_tooltip(tip);
    } else {
        ui_tooltip(names[k]);
    }
}

/* draw a bar and handle its buttons; returns the viewer whose items the mouse is over (-1 none) */
static int bar_ui(const char *id, Rect r, BarItem *items, int n, float total, float *scroll, bool left) {
    int prev = ui_layer(UI_LAYER_PANEL);
    ui_panel_ex(r.x, r.y, r.w, r.h, UI_COL_PANEL, 0);
    ui_rect(left ? r.x + r.w - px(1) : r.x, r.y, px(1), r.h, UI_COL_BORDER);
    *scroll = FS_CLAMP(*scroll + ui_scroll_area(r.x, r.y, r.w, r.h), 0.0f, FS_MAX(0.0f, total - r.h));
    ui_push_clip(r.x, r.y, r.w, r.h);
    ui_push_id(id);
    float b = bar_button(), bx = r.x + floorf((r.w - b) / 2);
    int over = -1;
    bool errors = A.prog == NULL || A.script_errors > 0;
    for (int i = 0; i < n; i++) {
        BarItem *it = &items[i];
        float y = r.y + it->y - *scroll;
        if (y + it->h <= r.y || y >= r.y + r.h) continue;
        if (it->viewer >= 0 && in_rect((Rect){r.x, y, r.w, it->h}, A.in.mouse_x, A.in.mouse_y) &&
            ui_mouse_over(r.x, y, r.w, it->h))
            over = it->viewer;
        switch (it->kind) {
        case BI_SEP: ui_rect(bx + px(4), y + floorf(it->h / 2), b - px(8), px(1), UI_COL_BORDER); break;
        case BI_SCRIPT:
            if (ui_icon_button_ex("##script", "code", "S", bx, y, b, A.editor_open ? UI_SELECTED : UI_FLAT)) A.editor_open = !A.editor_open;
            ui_tooltip(A.editor_open ? "Hide the script (" MODKEY "+E)" : "Show the script (" MODKEY "+E)");
            if (errors) ui_disk(bx + b - px(6), y + px(6), px(4), UI_COL_ERROR);
            break;
        case BI_CONFIG:
            if (ui_icon_button_ex("##config", "tune", "C", bx, y, b, A.config_open ? UI_SELECTED : UI_FLAT)) A.config_open = !A.config_open;
            ui_tooltip(A.config_open ? "Hide the config (" MODKEY "+T)" : "Show the config (" MODKEY "+T)");
            break;
        case BI_INFO: {
            if (ui_icon_button_ex("##info", "info", "i", bx, y, b, A.info_open ? UI_SELECTED : UI_FLAT)) A.info_open = !A.info_open;
            ui_tooltip(A.info_open ? "Hide the info (" MODKEY "+I)"
                                   : "Show the info: what the tools print, and what the Inspect tool finds (" MODKEY "+I)");
            if (!A.info_open && out_total() != A.out_seen) ui_disk(bx + b - px(6), y + px(6), px(4), UI_COL_ACCENT);
            break;
        }
        case BI_VIEW: bar_view_button(it->viewer, bx, y, b); break;
        case BI_TOOL: bar_tool_button(it->viewer, it->what, bx, y, b); break;
        case BI_FOCUS: { /* which view the buttons below act on */
            UiInteract h = ui_interact("##focus", bx, y, b, b);
            view_badge(it->viewer, bx, y, b, 0, UI_COL_TEXT_DIM);
            if (h.hovered) {
                char tip[300];
                snprintf(tip, sizeof tip, "The buttons below act on view %d: %.200s (the view you clicked last)", it->viewer + 1,
                         view_name(it->viewer));
                ui_tooltip(tip);
            }
            break;
        }
        case BI_ACTION: bar_action(it->viewer, it->what, bx, y, b); break;
        case BI_HELP:
            if (ui_icon_button_ex("##help", "keyboard", "?", bx, y, b, A.help_open ? UI_SELECTED : UI_FLAT)) A.help_open = !A.help_open;
            ui_tooltip("The mouse and the keys (F1)");
            break;
        }
    }
    ui_pop_id();
    ui_pop_clip();
    ui_layer(prev);
    return over;
}

/* outline the tile of viewer v (the bars' buttons for it are under the mouse): light inside dark, to be seen on any
 * picture */
static void outline_view(int v) {
    for (int i = 0; i < A.nvis; i++)
        if (A.vis[i].viewer == v) {
            int prev = ui_layer(UI_LAYER_PANEL);
            Rect t = A.vis[i].vr.w >= 1 ? A.vis[i].vr : A.vis[i].r; /* (the part that shows, if any) */
            float o = px(1);
            ui_rect_outline(t.x, t.y, t.w, t.h, px(4), UI_RGBA(0, 0, 0, 150));
            ui_rect_outline(t.x + o, t.y + o, t.w - 2 * o, t.h - 2 * o, px(2), UI_RGBA(235, 240, 250, 235));
            ui_layer(prev);
        }
}

static void bars(void) {
    BarItem items[BAR_ITEMS];
    float total;
    int n = left_bar_items(items, &total);
    int over = bar_ui("##lbar", A.lbar_r, items, n, total, &A.lbar_scroll, true);
    n = right_bar_items(items, &total);
    int over2 = bar_ui("##rbar", A.rbar_r, items, n, total, &A.rbar_scroll, false);
    if (A.drag == DRAG_NONE) {
        if (over >= 0) outline_view(over);
        else if (over2 >= 0) outline_view(over2);
    }
}

/* the splitter at a pane's inner edge; returns the new edge position while dragged, else NAN */
static float pane_splitter(const char *id, float x, float H) {
    float g = px(5);
    int prev = ui_layer(UI_LAYER_PANEL);
    UiInteract it = ui_interact(id, x - g, 0, 2 * g, H);
    ui_layer(prev);
    if (it.hovered || it.held) ui_set_cursor(UI_CURSOR_HRESIZE);
    if (it.held) return A.in.mouse_x;
    return NAN;
}

/* ---- find / replace ---------------------------------------------------------------------- */
static void open_find(bool replace) {
    A.editor_open = true;
    A.find_open = true;
    A.replace_open = replace || A.replace_open;
    A.find_focus = true;
    char *sel = editor_selected_text(A.ed); /* the selection, if one line, is what to look for */
    if (sel[0] && !strchr(sel, '\n')) snprintf(A.find_buf, sizeof A.find_buf, "%s", sel);
    free(sel);
}

static void close_find(void) {
    A.find_open = A.replace_open = false;
    editor_set_search(A.ed, NULL, false);
    A.editor_grab = true;
}

static void find_step(bool backwards) {
    if (!A.find_buf[0]) return;
    if (!editor_find(A.ed, A.find_buf, A.find_case, backwards)) set_status(true, "\"%s\" is not in the script", A.find_buf);
}

/* the find bar (and replace bar) at the top of the script pane (from x0, ew wide); returns its height */
static float find_bar(float x0, float ew, float y) {
    if (!A.find_open) return 0;
    float rh = px(38), b = px(28), pad = px(6), gap = px(4);
    float h = rh * (A.replace_open ? 2 : 1);
    ui_panel_ex(x0, y, ew, h, UI_COL_PANEL_HI, 0);
    ui_rect(x0, y + h - px(1), ew, px(1), UI_COL_BORDER);
    float by = y + (rh - b) / 2;
    /* buttons from the right: close, replace toggle, match case, next, previous, count */
    float x = x0 + ew - pad - b;
    if (ui_icon_button_ex("##find_close", "close", "x", x, by, b, UI_FLAT)) close_find();
    ui_tooltip("Close (Esc)");
    x -= b + gap;
    if (ui_icon_button_ex("##find_repl", "find_replace", "R", x, by, b, A.replace_open ? UI_SELECTED : UI_FLAT))
        A.replace_open = !A.replace_open;
    ui_tooltip("Replace (" MODKEY "+Alt+F)");
    x -= b + gap;
    if (ui_button_ex(A.find_case ? "Aa##find_case_on" : "Aa##find_case", x, by, b, b, A.find_case ? UI_SELECTED : UI_FLAT))
        A.find_case = !A.find_case;
    ui_tooltip("Match case");
    x -= b + gap;
    if (ui_icon_button_ex("##find_next", "keyboard_arrow_down", "v", x, by, b, UI_FLAT)) find_step(false);
    ui_tooltip("Next (Enter, " MODKEY "+G, F3)");
    x -= b + gap;
    if (ui_icon_button_ex("##find_prev", "keyboard_arrow_up", "^", x, by, b, UI_FLAT)) find_step(true);
    ui_tooltip("Previous (Shift+Enter, " MODKEY "+Shift+G, Shift+F3)");
    int current = 0, count = A.find_buf[0] ? editor_count_matches(A.ed, A.find_buf, A.find_case, &current) : 0;
    char cnt[40] = "";
    if (A.find_buf[0]) {
        if (count == 0) snprintf(cnt, sizeof cnt, "none");
        else if (current) snprintf(cnt, sizeof cnt, "%d of %d", current, count);
        else snprintf(cnt, sizeof cnt, "%d", count);
    }
    float cw = px(64);
    x -= cw + gap;
    ui_label(x, by, cw, b, cnt, UI_ALIGN_CENTER, count == 0 && A.find_buf[0] ? UI_COL_ERROR : UI_COL_TEXT_DIM);
    float fw = FS_MAX(px(60), x - gap - pad - x0);
    int r = ui_text_field_ex("##find_text", x0 + pad, by, fw, b, A.find_buf, (int)sizeof A.find_buf,
                             UI_TF_KEEP_FOCUS | UI_TF_SELECT_ALL, "Find");
    if (A.find_focus) {
        ui_set_focus(ui_id("##find_text"));
        A.find_focus = false;
    }
    if (r & UI_TEXT_CHANGED) { /* look from where the selection starts, as you type */
        int a = editor_anchor(A.ed), c = editor_cursor(A.ed), lo = FS_MIN(a, c);
        editor_set_selection(A.ed, lo, lo);
        if (A.find_buf[0]) editor_find(A.ed, A.find_buf, A.find_case, false);
    }
    if (r & UI_TEXT_SUBMIT) find_step((A.in.mods & UI_MOD_SHIFT) != 0);
    if (r & UI_TEXT_CANCEL) close_find();
    if (A.replace_open && A.find_open) {
        float ry = y + rh + (rh - b) / 2, aw = px(64);
        float ax = x0 + ew - pad - aw;
        if (ui_button_ex("All##repl_all", ax, ry, aw, b, 0) && A.find_buf[0]) {
            int n = editor_replace_all(A.ed, A.find_buf, A.replace_buf, A.find_case);
            set_status(false, "Replaced %d occurrence%s", n, n == 1 ? "" : "s");
        }
        ui_tooltip("Replace every occurrence (one undo step)");
        float rw = px(76);
        ax -= rw + gap;
        bool one = ui_button_ex("Replace##repl_one", ax, ry, rw, b, 0);
        ui_tooltip("Replace the selected occurrence and find the next");
        int rr = ui_text_field_ex("##replace_text", x0 + pad, ry, FS_MAX(px(60), ax - gap - pad - x0), b, A.replace_buf,
                                  (int)sizeof A.replace_buf, UI_TF_KEEP_FOCUS, "Replace with");
        if ((one || (rr & UI_TEXT_SUBMIT)) && A.find_buf[0]) editor_replace(A.ed, A.find_buf, A.replace_buf, A.find_case);
        if (rr & UI_TEXT_CANCEL) close_find();
    }
    if (A.find_open) editor_set_search(A.ed, A.find_buf, A.find_case);
    return A.find_open ? h : 0;
}

/* the script pane, right of the left bar (or the whole window when nothing could be loaded yet) */
static void editor_pane(float H) {
    Rect r = A.editor_r;
    if (!A.editor_open || r.w < 1) return;
    float x0 = r.x, ew = r.w;
    float bh = px(40);
    ui_panel_ex(x0, 0, ew, bh, UI_COL_PANEL_HI, 0);
    float x = x0 + px(8), y = px(6), bhh = bh - px(12);
    ui_push_clip(x0, 0, ew, bh);
    bool wide = ew > px(340);
    float lh = ui_font_line_height(UI_FONT_SANS);
    float rw = wide ? ui_text_width(UI_FONT_SANS, "Run", -1) + bhh + px(12) : bhh;
    if (ui_button_ex("##run", x, y, rw, bhh, UI_ACCENT)) run_editor();
    ui_tooltip("Run the script (" MODKEY "+Enter)");
    ui_icon("play_arrow", ">", x + (wide ? px(2) : 0), y, bhh, UI_COL_ON_ACCENT);
    if (wide) ui_text(UI_FONT_SANS, x + bhh, y + (bhh - lh) / 2, "Run", -1, UI_COL_ON_ACCENT);
    x += rw + px(6);
    if (ui_icon_button_ex("##new", "note_add", "N", x, y, bhh, 0)) ask_new();
    ui_tooltip("A new script, from a template (" MODKEY "+N)");
    x += bhh + px(4);
    if (ui_icon_button_ex("##save", "save", "S", x, y, bhh, 0)) {
        save_or_ask();
    }
    ui_tooltip("Save (" MODKEY "+S), save as (" MODKEY "+Shift+S)");
    x += bhh + px(4);
    if (ui_icon_button_ex("##open", "folder_open", "O", x, y, bhh, 0)) ask_open();
    ui_tooltip("Open a script (" MODKEY "+O), or drop it on the window");
    x += bhh + px(4);
    if (ui_icon_button_ex("##find", "search", "F", x, y, bhh, A.find_open ? UI_SELECTED : 0)) {
        if (A.find_open) close_find();
        else open_find(false);
    }
    ui_tooltip("Find and replace (" MODKEY "+F, " MODKEY "+Alt+F)");
    x += bhh + px(4);
    if (ui_icon_button_ex("##reference", "menu_book", "?", x, y, bhh, A.ref_open ? UI_SELECTED : 0)) A.ref_open = !A.ref_open;
    ui_tooltip("The language reference (F2)");
    x += bhh + px(10);
    ui_toggle("Live", x, y + px(3), px(64), bhh - px(6), &A.live);
    ui_tooltip("Run the script automatically shortly after you stop typing");
    x += px(70);
    char name[300];
    snprintf(name, sizeof name, "%.250s%s", A.path[0] ? fs_basename(A.path) : "untitled", editor_is_modified(A.ed) ? " *" : "");
    if (x < x0 + ew - px(50)) ui_label(x, y, x0 + ew - x - px(10), bhh, name, UI_ALIGN_RIGHT, UI_COL_TEXT_DIM);
    ui_pop_clip();
    editor_set_clock(A.ed, fs_time_seconds());
    static float last_fh;
    float fh = find_bar(x0, ew, bh);
    if (fh != last_fh) editor_reveal(A.ed); /* the editor got shorter or taller: keep the match in view */
    last_fh = fh;
    editor_ui(A.ed, x0, bh + fh, ew, H - bh - fh, A.editor_grab);
    A.editor_grab = false;
    ui_rect(x0 + ew - px(1), 0, px(1), H, UI_COL_BORDER);
    if (A.prog) { /* (alone in the window: nothing to share it with) */
        float nx = pane_splitter("##editor_split", x0 + ew, H);
        if (nx == nx && A.in.fb_w > 0) A.editor_frac = FS_CLAMP((nx - x0) / (float)A.in.fb_w, 0.15f, 0.8f);
    }
}

static float config_row_h(void) { return px(30); }

/* ---- the script's values: which ones changed, back to them, or into the script ----------------------------------- */
static bool block_assigns(Block *b, Sym *s);
static bool stmt_assigns(Stmt *st, Sym *s) {
    if ((st->kind == S_ASSIGN || st->kind == S_APPEND) && st->target && st->target->sym == s) return true;
    if (st->kind == S_DECL && st->sym == s) return true;
    if (st->kind == S_DESTRUCTURE)
        for (int k = 0; k < st->ntargets; k++)
            if (st->targets[k] && st->targets[k]->sym == s) return true;
    return block_assigns(&st->body, s) || block_assigns(&st->else_body, s);
}
static bool block_assigns(Block *b, Sym *s) {
    for (int i = 0; i < b->n; i++)
        if (stmt_assigns(b->stmts[i], s)) return true;
    return false;
}

/* a value the script computes again whenever the state changes: given by a statement of @prepare that always runs
   (one in an `if` only adjusts the value now and then: it stays a setting) */
static bool prepared(Program *p, Sym *s) {
    for (int i = 0; i < p->nprepares; i++) {
        Block *b = &p->prepares[i]->body;
        for (int k = 0; k < b->n; k++) {
            Stmt *st = b->stmts[k];
            if (st->kind != S_IF && st->kind != S_WHILE && st->kind != S_DOWHILE && st->kind != S_FOR &&
                st->kind != S_FOREACH && stmt_assigns(st, s))
                return true;
        }
    }
    return false;
}

/* a setting of the script: not a view's place, not something the app or @prepare keeps up to date (a variable of
   @config is a setting even if @prepare changes it too) */
static bool script_setting(Program *p, Sym *s) {
    return s->kind == SYM_GLOBAL && !app_managed(p, s) && !view_geometry(p, s) && (s->is_config || !prepared(p, s));
}

static bool value_changed(Program *p, Sym *s) {
    return !same_value(s->type, A.state + s->slot, p->initial_state + s->slot);
}

/* has view v moved from where the script puts it? */
static bool view_moved(Program *p, int v) {
    Section *s = p->viewers[v];
    Sym *g[4] = {s->center, s->radius, s->center_lo, s->mobius};
    for (int k = 0; k < 4; k++)
        if (g[k] && value_changed(p, g[k])) return true;
    return false;
}

static int count_changed(Program *p, bool views) {
    int n = 0;
    for (int i = 0; i < p->globals.len; i++)
        if (script_setting(p, p->globals.data[i]) && value_changed(p, p->globals.data[i])) n++;
    for (int v = 0; views && v < p->nviewers; v++)
        if (view_moved(p, v)) n++;
    return n;
}

/* Reset values: the settings as the script has them (the views stay) */
static void reset_values(void) {
    Program *p = A.prog;
    int n = 0;
    for (int i = 0; i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        if (!script_setting(p, s) || !value_changed(p, s)) continue;
        memcpy(A.state + s->slot, p->initial_state + s->slot, sizeof(u32) * (size_t)s->type->size);
        n++;
        for (int v = 0; v < p->nviewers; v++) /* a picked point back where the script has it: no marker */
            if (p->viewers[v]->picked == s) A.vrt[v].picked = s->type->size >= 2 && (A.state[s->slot] || A.state[s->slot + 1]);
    }
    if (n) set_status(false, "%d value%s back as in the script", n, n == 1 ? "" : "s");
    else set_status(false, "The values are as in the script (the views have their own reset: Home)");
}

/* A literal for a value, as the script would write it; false if it has none (infinities...) */
static bool value_literal(StrBuf *sb, Type *t, const u32 *v) {
    char a[48], b[48];
    switch (t->kind) {
    case T_BOOL: sb_puts(sb, v[0] ? "true" : "false"); return true;
    case T_INT: sb_printf(sb, "%d", (i32)v[0]); return true;
    case T_REAL:
        if (!isfinite(wf(v[0]))) return false;
        float_text(a, sizeof a, wf(v[0]));
        sb_puts(sb, a);
        return true;
    case T_COMPLEX: {
        float re = wf(v[0]), im = wf(v[1]);
        if (!isfinite(re) || !isfinite(im)) return false;
        float_text(a, sizeof a, re);
        float_text(b, sizeof b, fabsf(im));
        if (im == 0) sb_puts(sb, a);
        else if (re == 0) sb_printf(sb, "%s%si", im < 0 ? "-" : "", b);
        else sb_printf(sb, "%s %c %si", a, im < 0 ? '-' : '+', b);
        return true;
    }
    case T_QUAT:
        sb_puts(sb, "quat(");
        for (int k = 0; k < 4; k++) {
            if (!isfinite(wf(v[k]))) return false;
            float_text(a, sizeof a, wf(v[k]));
            sb_printf(sb, "%s%s", k ? ", " : "", a);
        }
        sb_puts(sb, ")");
        return true;
    case T_PROJ: { /* the point's complex number, or ∞ */
        double ar = wf(v[0]), ai = wf(v[1]), br = wf(v[2]), bi = wf(v[3]), d = br * br + bi * bi;
        if (isnan(ar) || isnan(ai) || isnan(d)) return false;
        if (d == 0) {
            if (ar == 0 && ai == 0) return false;
            sb_puts(sb, "∞");
            return true;
        }
        u32 z[2] = {fw((float)((ar * br + ai * bi) / d)), fw((float)((ai * br - ar * bi) / d))};
        return value_literal(sb, t_complex, z);
    }
    case T_VEC: case T_LIST: {
        int n = t->kind == T_VEC ? t->n : FS_CLAMP((i32)v[0], 0, t->n);
        const u32 *d = t->kind == T_VEC ? v : v + t->data_off;
        sb_puts(sb, t == t_color ? "rgb(" : "[");
        for (int k = 0; k < n; k++) {
            if (k) sb_puts(sb, ", ");
            if (!value_literal(sb, t->elem, d + k * t->stride)) return false;
        }
        sb_puts(sb, t == t_color ? ")" : "]");
        return true;
    }
    default: return false;
    }
}

/* an expression made of numbers only (what a value written in the script looks like, not a formula) */
static bool literal_like(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case E_INT: case E_REAL: case E_IMAG: case E_BOOL: case E_COLOR_CONST: return true;
    case E_INTERNAL: case E_NAME: return e->sym && e->sym->kind == SYM_CONST; /* .red, pi */
    case E_UNARY: return e->op == TK_MINUS && literal_like(e->a);
    case E_CONVERT: case E_POWI: return literal_like(e->a);
    case E_BINARY:
        return (e->op == TK_PLUS || e->op == TK_MINUS || e->op == TK_STAR || e->op == TK_SLASH || e->op == TK_CARET) &&
               literal_like(e->a) && literal_like(e->b);
    case E_VECTOR: case E_CALL: /* [1, 2], rgb(1, 0, 0), quat(...) */
        if (e->kind == E_CALL && e->fn && !e->fn->prelude) return false;
        for (int k = 0; k < e->nargs; k++)
            if (!literal_like(e->args[k])) return false;
        return true;
    default: return false;
    }
}

/* where expression e is in the text: one line, brackets balanced */
static bool expr_span(const char *text, int len, const int *ls, int nlines, Expr *e, int *start, int *end) {
    if (e->line < 1 || e->line > nlines) return false;
    int l0 = ls[e->line - 1], l1 = e->line < nlines ? ls[e->line] : len;
    int a = l0 + e->col - 1, b = l0 + e->end_col - 1;
    if (a < l0 || b > l1 || b <= a) return false;
    int depth = 0;
    for (int i = a; i < b; i++) {
        char c = text[i];
        if (c == '\n' || c == '#' || c == '"') return false;
        if (c == '(' || c == '[') depth++;
        if ((c == ')' || c == ']') && --depth < 0) return false;
    }
    if (depth) return false;
    *start = a;
    *end = b;
    return true;
}

/* the last top-level statement of @config / @global giving s its value: `x : R = 1`, `x := 1` or `x = 1` */
static Stmt *defining_stmt(Program *p, Sym *s) {
    Stmt *last = NULL;
    for (int i = 0; i < p->sections.len; i++) {
        Section *sec = p->sections.data[i];
        if (sec->kind != SEC_CONFIG && sec->kind != SEC_GLOBAL) continue;
        for (int k = 0; k < sec->body.n; k++) {
            Stmt *st = sec->body.stmts[k];
            if ((st->kind == S_DECL && st->sym == s && st->value) ||
                (st->kind == S_ASSIGN && st->op == TK_ASSIGN && st->target && st->target->sym == s))
                last = st;
        }
    }
    return last;
}

typedef struct KeepEdit { int pos, del, order; char *text; } KeepEdit;
typedef VEC(KeepEdit) KeepEditVec;

static int keep_edit_cmp(const void *a, const void *b) {
    const KeepEdit *x = (const KeepEdit *)a, *y = (const KeepEdit *)b;
    return x->pos != y->pos ? (x->pos < y->pos ? -1 : 1) : x->order - y->order;
}

/* "Write the values into the script": the settings changed in the panel or by tools, and the places of the flat
   views, become the script's own (the script then opens like this).  One undo step in the editor. */
static void write_values(void) {
    Program *p = A.prog;
    if (!p) return;
    int len;
    const char *text = editor_get_text(A.ed, &len);
    if (A.compiled_version != editor_version(A.ed)) {
        set_status(true, "The script changed since it last ran: run it first (" MODKEY "+Enter)");
        return;
    }
    VEC(int) ls = {0}; /* line starts */
    vec_push(ls, 0);
    for (int i = 0; i < len; i++)
        if (text[i] == '\n') vec_push(ls, i + 1);
    int nlines = ls.len;
    KeepEditVec edits = {0};
    StrBuf skipped = {0}, sb = {0};
    int nvalues = 0, nviews = 0;
    for (int i = 0; i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        if (!script_setting(p, s) || !value_changed(p, s)) continue;
        Stmt *st = defining_stmt(p, s);
        int a, b;
        sb_clear(&sb);
        if (!st || !literal_like(st->value) || !expr_span(text, len, ls.data, nlines, st->value, &a, &b) ||
            !value_literal(&sb, s->type, A.state + s->slot)) {
            sb_printf(&skipped, "%s%s%s", skipped.len ? ", " : "", s->is_internal ? "." : "", s->name);
            continue;
        }
        KeepEdit e = {a, b - a, edits.len, xstrdup(sb_cstr(&sb))};
        vec_push(edits, e);
        nvalues++;
    }
    /* the views: NAME.center = ... and NAME.radius = ..., changed where the script has them, else added to @config */
    Section *config = NULL, *first = NULL;
    for (int i = 0; i < p->sections.len; i++) {
        Section *sec = p->sections.data[i];
        if (sec->line < 1 || sec->line > nlines) continue;
        int h = ls.data[sec->line - 1];
        while (h < len && (text[h] == ' ' || text[h] == '\t')) h++;
        if (h >= len || text[h] != '@') continue; /* (no header: a script without sections, or code before the first) */
        if (!first || sec->line < first->line) first = sec;
        if (sec->kind == SEC_CONFIG && (!config || sec->line < config->line)) config = sec;
    }
    StrBuf added = {0};
    for (int v = 0; v < p->nviewers; v++) {
        Section *vs = p->viewers[v];
        if (!view_moved(p, v)) continue;
        if (vs->mobius || !first) {
            sb_printf(&skipped, "%s%s (its place)", skipped.len ? ", " : "", vs->name);
            continue;
        }
        char loc[256];
        location_text(v, loc, sizeof loc); /* "NAME.center = ...\nNAME.radius = ...\n" */
        char *nl = strchr(loc, '\n');
        *nl = 0;
        const char *lines[2] = {loc, nl + 1};
        Sym *syms[2] = {vs->center, vs->radius};
        int written = 0;
        for (int k = 0; k < 2; k++) {
            char *l = (char *)lines[k];
            l[strcspn(l, "\n")] = 0;
            const char *value = strstr(l, " = ") + 3;
            Stmt *st = defining_stmt(p, syms[k]);
            int a, b;
            if (st && literal_like(st->value) && expr_span(text, len, ls.data, nlines, st->value, &a, &b)) {
                KeepEdit e = {a, b - a, edits.len, xstrdup(value)};
                vec_push(edits, e);
                written++;
            } else if (st) { /* a formula gives it: a line of ours would change nothing (or the formula) */
                sb_printf(&skipped, "%s%s", skipped.len ? ", " : "", syms[k]->name);
            } else {
                sb_printf(&added, "%s\n", l);
                written++;
            }
        }
        if (written) nviews++;
    }
    if (added.len) {
        KeepEdit e = {0, 0, edits.len, NULL};
        if (config) { /* at the top of @config */
            e.pos = config->line < nlines ? ls.data[config->line] : len;
            sb_clear(&sb);
            sb_printf(&sb, "%s%s", e.pos == len && len && text[len - 1] != '\n' ? "\n" : "", sb_cstr(&added));
        } else { /* a new @config before the first section */
            e.pos = ls.data[first->line - 1];
            sb_clear(&sb);
            sb_printf(&sb, "@config\n%s\n", sb_cstr(&added));
        }
        e.text = xstrdup(sb_cstr(&sb));
        vec_push(edits, e);
    }
    qsort(edits.data, (size_t)edits.len, sizeof(KeepEdit), keep_edit_cmp);
    EditorEdit *ee = (EditorEdit *)xcalloc((size_t)FS_MAX(edits.len, 1), sizeof(EditorEdit));
    for (int i = 0; i < edits.len; i++) ee[i] = (EditorEdit){edits.data[i].pos, edits.data[i].del, edits.data[i].text};
    bool ok = edits.len == 0 || editor_replace_ranges(A.ed, ee, edits.len);
    free(ee);
    for (int i = 0; i < edits.len; i++) free(edits.data[i].text);
    if (!ok) {
        set_status(true, "Couldn't write the values into the script");
    } else if (nvalues + nviews == 0 && !skipped.len) {
        set_status(false, "The script already has these values");
    } else {
        sb_clear(&sb);
        if (nvalues) sb_printf(&sb, "%d value%s", nvalues, nvalues == 1 ? "" : "s");
        if (nviews) sb_printf(&sb, "%sthe place of %d view%s", nvalues ? " and " : "", nviews, nviews == 1 ? "" : "s");
        if (nvalues + nviews) sb_printf(&sb, " written into the script (" MODKEY "+S saves it)");
        if (skipped.len)
            sb_printf(&sb, "%sNot written (a formula there, or not in @config): %s", nvalues + nviews ? ". " : "",
                      sb_cstr(&skipped));
        set_status(skipped.len > 0 && nvalues + nviews == 0, "%s", sb_cstr(&sb));
        if (nvalues + nviews) run_editor(); /* the script's values are now these */
    }
    vec_free(ls);
    vec_free(edits);
    sb_free(&skipped);
    sb_free(&sb);
    sb_free(&added);
}

/* "1.5", "-2", "0.3i", "-0.8+0.156i", "1-i" */
static bool parse_number_text(const char *s, float *re, float *im) {
    while (*s == ' ') s++;
    char *end;
    *re = *im = 0;
    double a = strtod(s, &end);
    if (end == s) {
        if (*s == 'i') { *im = 1; end = (char *)s + 1; }
        else if (*s == '-' && s[1] == 'i') { *im = -1; end = (char *)s + 2; }
        else return false;
    } else if (*end == 'i') {
        *im = (float)a;
        end++;
    } else {
        *re = (float)a;
        while (*end == ' ') end++;
        if (*end == '+' || *end == '-') {
            const char *t = end;
            const char *q = t + 1;
            while (*q == ' ') q++;
            double b;
            if (*q == 'i') {
                b = 1;
                end = (char *)q;
            } else {
                b = strtod(q, &end);
                if (end == q) return false;
            }
            if (*end != 'i') return false;
            end++;
            *im = (float)(*t == '-' ? -b : b);
        }
    }
    while (*end == ' ') end++;
    return *end == 0;
}

/* A value edited as text (see A.text_name): the field shows `shown` (the value) until it gets the focus, then the text
   typed, which `apply` reads into the value on Enter or when the focus leaves the field (Escape: nothing changes). One
   such field at a time: one that gets the focus first applies the one edited before. */
static void config_text_apply_pending(void) {
    if (!A.text_name[0]) return;
    for (int i = 0; A.prog && i < A.prog->globals.len; i++) {
        Sym *g = A.prog->globals.data[i];
        if (g->kind == SYM_GLOBAL && !g->is_internal && !strcmp(g->name, A.text_name)) {
            if (A.text_apply) A.text_apply(g, A.text_buf);
            break;
        }
    }
    A.text_name[0] = 0;
}

static void config_text_value(Sym *s, const char *id, float x, float y, float w, float h, const char *shown, int flags,
                              const char *placeholder, void (*apply)(Sym *s, const char *text)) {
    u32 fid = ui_id(id);
    bool editing = A.text_name[0] && !strcmp(A.text_name, s->name) && A.text_apply == apply;
    static char copy[1024];
    char *buf = editing ? A.text_buf : copy;
    if (!editing) snprintf(copy, sizeof copy, "%s", shown);
    int r = ui_text_field_ex(id, x, y, w, h, buf, (int)(editing ? sizeof A.text_buf : sizeof copy), flags, placeholder);
    if (!editing) {
        if (ui_focus_id() == fid) { /* just focused: edit a copy */
            config_text_apply_pending();
            snprintf(A.text_buf, sizeof A.text_buf, "%s", copy);
            snprintf(A.text_name, sizeof A.text_name, "%s", s->name);
            A.text_apply = apply;
        }
    } else if (r & UI_TEXT_CANCEL) {
        A.text_name[0] = 0;
    } else if ((r & (UI_TEXT_SUBMIT | UI_TEXT_BLURRED)) || ui_focus_id() != fid) {
        config_text_apply_pending();
    }
}

/* a list of numbers, edited as text: "0, 1, 1 + i" */
static void apply_list_text(Sym *s, const char *text) {
    Type *lt = s->type, *et = lt->elem;
    u32 *v = A.state + s->slot;
    /* parse the items; a bad one leaves the list as it was */
    u32 *tmp = (u32 *)xcalloc((size_t)lt->size, sizeof(u32));
    int n = 0;
    bool ok = true;
    char *copy = xstrdup(text);
    for (char *tok = strtok(copy, ","); tok && ok; tok = strtok(NULL, ",")) {
        float re, im;
        if (!parse_number_text(tok, &re, &im)) { ok = false; break; }
        if (n >= lt->n) break; /* past the capacity */
        u32 *it = tmp + lt->data_off + n * lt->stride;
        if (et->kind == T_INT || et->kind == T_BOOL) it[0] = (u32)(i32)floorf(re + 0.5f);
        else if (et->kind == T_REAL) it[0] = fw(re);
        else { it[0] = fw(re); it[1] = fw(im); }
        n++;
    }
    free(copy);
    if (ok) {
        tmp[0] = (u32)n;
        memcpy(v, tmp, sizeof(u32) * (size_t)lt->size);
    } else {
        set_status(true, "%s: write numbers separated by commas, as in 0, 1.5, 2 - i", s->name);
    }
    free(tmp);
}

static void config_list(Sym *s, float x, float y, float w, float h) {
    Type *lt = s->type, *et = lt->elem;
    u32 *v = A.state + s->slot;
    char id[200];
    snprintf(id, sizeof id, "##cfglist_%s", s->name);
    StrBuf sb = {0};
    int n = FS_CLAMP((i32)v[0], 0, lt->n);
    for (int k = 0; k < n; k++) {
        const u32 *it = v + lt->data_off + k * lt->stride;
        if (k) sb_puts(&sb, ", ");
        if (et->kind == T_INT) sb_printf(&sb, "%d", (i32)it[0]);
        else if (et->kind == T_REAL) sb_printf(&sb, "%.6g", wf(it[0]));
        else if (et->kind == T_COMPLEX) {
            float re = wf(it[0]), im = wf(it[1]);
            char ims[40];
            if (fabsf(im) == 1) snprintf(ims, sizeof ims, "i");
            else snprintf(ims, sizeof ims, "%.6gi", fabsf(im));
            if (im == 0) sb_printf(&sb, "%.6g", re);
            else if (re == 0) sb_printf(&sb, "%s%s", im < 0 ? "-" : "", ims);
            else sb_printf(&sb, "%.6g %c %s", re, im < 0 ? '-' : '+', ims);
        } else if (et->kind == T_BOOL) sb_puts(&sb, it[0] ? "1" : "0");
    }
    config_text_value(s, id, x, y, w, h, sb_cstr(&sb), 0, "1, 2, 3", apply_list_text);
    sb_free(&sb);
}

/* a point of the sphere, typed as a complex number or inf */
static void apply_point_text(Sym *s, const char *text) {
    u32 p[4];
    if (parse_proj_literal(text, p)) memcpy(A.state + s->slot, p, sizeof p);
    else set_status(true, "%s: write a complex number (like 0.5 - 2i) or inf", s->name);
}

static void config_number(Sym *s, float x, float y, float w) {
    u32 *v = A.state + s->slot;
    float h = config_row_h() - px(4);
    char id[160];
    switch (s->type->kind) {
    case T_INT: {
        int val = (i32)v[0];
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        bool ch;
        if (s->has_range && s->range_lo > 0 && s->range_hi >= 100 * s->range_lo) { /* wide ranges: logarithmic */
            double dv = val;
            ch = ui_slider_double(id, x, y, w, h, &dv, s->range_lo, s->range_hi, "%.0f", UI_SLIDER_LOG);
            val = (int)floor(dv + 0.5);
        } else {
            ch = s->has_range ? ui_slider_int(id, x, y, w, h, &val, (int)s->range_lo, (int)s->range_hi)
                              : ui_drag_int(id, x, y, w, h, &val, 0.25f, -2147483647, 2147483647);
        }
        if (ch) v[0] = (u32)val;
        break;
    }
    case T_REAL: {
        double val = wf(v[0]);
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        int flags = s->has_range && s->range_lo > 0 && s->range_hi >= 100 * s->range_lo ? UI_SLIDER_LOG : 0;
        bool ch = s->has_range ? ui_slider_double(id, x, y, w, h, &val, s->range_lo, s->range_hi, "%.5g", flags)
                               : ui_drag_double(id, x, y, w, h, &val, 0.002 * FS_MAX(fabs(val), 0.01), -1e30, 1e30, "%.6g");
        if (ch) v[0] = fw((float)val);
        break;
    }
    case T_COMPLEX: case T_QUAT: {
        int n = s->type->kind == T_COMPLEX ? 2 : 4;
        float cw = (w - px(6) * (n - 1)) / n;
        for (int k = 0; k < n; k++) {
            double val = wf(v[k]);
            snprintf(id, sizeof id, "##cfg_%s_%d", s->name, k);
            if (ui_drag_double(id, x + k * (cw + px(6)), y, cw, h, &val, 0.002 * FS_MAX(fabs(val), 0.01), -1e30, 1e30,
                               k == 0 ? "%.6g" : "%+.6gi"))
                v[k] = fw((float)val);
        }
        break;
    }
    case T_BOOL: {
        bool b = v[0] != 0;
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        if (ui_toggle(id, x, y, h, h, &b)) v[0] = b;
        break;
    }
    case T_PROJ: { /* a point of the sphere: its complex number or ∞, typed */
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        StrBuf sb = {0};
        if (!value_literal(&sb, t_proj, v)) sb_puts(&sb, "?");
        config_text_value(s, id, x, y, w, h, sb_cstr(&sb), UI_TF_SELECT_ALL, "a complex number, or inf", apply_point_text);
        sb_free(&sb);
        break;
    }
    case T_VEC:
        if (s->type == t_color) {
            float cw = (w - px(8) - h) / 3;
            for (int k = 0; k < 3; k++) {
                float val = wf(v[k]);
                snprintf(id, sizeof id, "##cfg_%s_%d", s->name, k);
                if (ui_slider_float(id, x + k * (cw + px(4)), y, cw, h, &val, 0, 1, "%.2f")) v[k] = fw(val);
            }
            float c[3] = {wf(v[0]), wf(v[1]), wf(v[2])};
            ui_rrect(x + w - h, y, h, h, px(4), rgb_u32(c, 1));
            break;
        }
        /* fall through */
    case T_LIST:
        if (type_is_number(s->type->elem) && s->type->elem != t_quat) {
            config_list(s, x, y, w, h);
            break;
        }
        /* fall through */
    default: {
        StrBuf sb = {0};
        type_name(s->type, &sb);
        ui_label(x, y, w, h, sb_cstr(&sb), UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        sb_free(&sb);
        break;
    }
    }
}

/* is s shown by the script's own config items (declared in @config, or given a slider or label there)? */
static bool config_listed(Program *p, Sym *s) {
    for (int i = 0; i < p->config.len; i++)
        if (p->config.data[i].kind == CFG_VAR && p->config.data[i].sym == s) return true;
    return false;
}

/* the views' center and radius, and the iteration settings, unless the script lists them itself */
static bool config_auto(Program *p, Sym *s, int group) {
    if (config_listed(p, s)) return false;
    if (group == 1 && s == p->s_double) { /* (deep zooms: in flat views) */
        for (int v = 0; v < p->nviewers; v++)
            if (!p->viewers[v]->mobius) return true;
        return false;
    }
    if (group == 1) {
        if (s->viewer_index < 0 || s->is_internal || s->is_config) return false;
        Section *v = p->viewers[s->viewer_index]; /* (a view of the Riemann sphere moves by its Möbius map) */
        return !(v->mobius && (s == v->center || s == v->radius));
    }
    return s == p->s_max_iter || s == p->s_escape || s == p->s_epsilon;
}

/* A dot before the label of a value that isn't the script's any more (a view's centre: with its low part); the
   label's tooltip says what the script has. */
static void config_changed_mark(Program *p, Sym *s, float x0, float y, float w) {
    float h = config_row_h() - px(4);
    bool changed = value_changed(p, s);
    if (s->viewer_index >= 0 && s == p->viewers[s->viewer_index]->center && p->viewers[s->viewer_index]->center_lo)
        changed = changed || value_changed(p, p->viewers[s->viewer_index]->center_lo);
    char id[170];
    snprintf(id, sizeof id, "##label_%s", s->name);
    if (!changed || !(script_setting(p, s) || view_geometry(p, s))) {
        if (ui_interact(id, x0, y, w, h).hovered && s->label) ui_tooltip(s->name); /* (the name behind the label) */
        return;
    }
    StrBuf sb = {0};
    sb_printf(&sb, "%s%s: changed; the script has ", s->is_internal ? "." : "", s->name);
    if (!value_literal(&sb, s->type, p->initial_state + s->slot)) sb_puts(&sb, "another value");
    /* the dot: a click sets the value back as the script has it */
    char did[180];
    snprintf(did, sizeof did, "##dot_%s", s->name);
    UiInteract dot = ui_interact(did, x0 - px(10), y, px(10), h);
    ui_disk(x0 - px(5), y + h / 2, dot.hovered ? px(3.5f) : px(2.5f), UI_COL_ACCENT);
    if (dot.hovered) {
        StrBuf tip = {0};
        sb_printf(&tip, "%s (a click goes back to it)", sb_cstr(&sb));
        ui_tooltip(sb_cstr(&tip));
        sb_free(&tip);
    }
    if (ui_interact(id, x0, y, w, h).hovered) ui_tooltip(sb_cstr(&sb));
    sb_free(&sb);
    if (dot.clicked) {
        Section *v = s->viewer_index >= 0 ? p->viewers[s->viewer_index] : NULL;
        if (v && view_geometry(p, s)) nav_begin(s->viewer_index, nav_new_gesture());
        Sym *both[2] = {s, v && s == v->center ? v->center_lo : NULL};
        for (int k = 0; k < 2; k++)
            if (both[k]) memcpy(A.state + both[k]->slot, p->initial_state + both[k]->slot, sizeof(u32) * (size_t)both[k]->type->size);
        for (int k = 0; k < p->nviewers; k++) /* (a picked point back where the script has it: no marker) */
            if (p->viewers[k]->picked == s) A.vrt[k].picked = false;
    }
}

/* The centre of a flat view, with its low part, showing as many digits as the radius asks for and dragged by a
   fraction of the radius.  The two parts go side by side in the value column (x1, width w) when they fit there, else
   on two rows of their own below the label, from x0.  Returns the height used. */
static float config_center(int v, float x0, float x1, float y, float w) {
    Section *s = A.prog->viewers[v];
    float rh = config_row_h(), h = rh - px(4);
    double c[2], r = wf(A.state[s->radius->slot]);
    view_center_d(v, &c[0], &c[1]);
    int digits = 6;
    if (r > 0) digits = FS_CLAMP((int)ceil(log10(FS_MAX(hypot(c[0], c[1]), r) / r)) + 3, 6, 16);
    char fmt[2][24], id[64], text[64];
    snprintf(fmt[0], sizeof fmt[0], "%%.%dg", digits);
    snprintf(fmt[1], sizeof fmt[1], "%%+.%dgi", digits);
    float cw = (w - px(6)) / 2, need = 0;
    for (int k = 0; k < 2; k++) {
        snprintf(text, sizeof text, fmt[k], c[k]);
        need = FS_MAX(need, ui_text_width(UI_FONT_SANS, text, -1) + px(18));
    }
    bool stacked = need > cw;
    float fx = stacked ? x0 + px(12) : x1, fw_ = stacked ? x1 + w - fx : cw;
    bool changed = false;
    for (int k = 0; k < 2; k++) {
        snprintf(id, sizeof id, "##cfg_center_%d_%d", v, k);
        float bx = stacked ? fx : x1 + k * (cw + px(6)), by = stacked ? y + (k + 1) * rh : y;
        if (ui_drag_double(id, bx, by, fw_, h, &c[k], 0.004 * FS_MAX(r, 1e-30), -1e30, 1e30, fmt[k])) changed = true;
        if (ui_last_hovered()) ui_tooltip("Drag to move the view, double-click to type a number");
    }
    if (changed && isfinite(c[0]) && isfinite(c[1])) {
        nav_begin(v, NAV_PANEL);
        set_view_center_d(v, c[0], c[1]);
    }
    return stacked ? 3 * rh : rh;
}

/* the radius of a flat view: dragged on a logarithmic scale */
static void config_radius(int v, float x, float y, float w) {
    Section *s = A.prog->viewers[v];
    double r = wf(A.state[s->radius->slot]);
    char id[64];
    snprintf(id, sizeof id, "##cfg_radius_%d", v);
    if (ui_drag_double_log(id, x, y, w, config_row_h() - px(4), &r, 0.01, 1e-30, 1e30, "%.4g") && r > 0) {
        nav_begin(v, NAV_PANEL);
        A.state[s->radius->slot] = fw((float)r);
    }
}

static void config_label(Sym *s, char *out, int cap) {
    if (s->label) snprintf(out, (size_t)cap, "%s", s->label);
    else snprintf(out, (size_t)cap, "%s%s", s->is_internal ? "." : "", s->name);
}

/* A formula of the script (a one-line `define` in @config): a text field editing the script itself, so
   the program is recompiled as you type (with Live on) or when you press Enter. */
static void config_formula(Func *f, float x0, float y, float w, float lw) {
    char label[160], id[200];
    StrBuf sb = {0};
    sb_printf(&sb, "%s(", f->name);
    for (int i = 0; i < f->nparams; i++) sb_printf(&sb, "%s%s", i ? ", " : "", f->params[i]->name);
    sb_puts(&sb, ") =");
    snprintf(label, sizeof label, "%s", sb_cstr(&sb));
    sb_free(&sb);
    float h = config_row_h() - px(4);
    float lwid = FS_MIN(ui_text_width(UI_FONT_SANS, label, -1) + px(10), lw);
    ui_label(x0, y, lwid, h, label, UI_ALIGN_LEFT, UI_COL_TEXT);
    snprintf(id, sizeof id, "##formula_%s_%d", f->name, f->nparams);
    u32 fid = ui_id(id);
    int len;
    const char *text = editor_get_text(A.ed, &len);
    static char shown[512];
    char *buf = shown;
    if (ui_focus_id() == fid) {
        buf = A.formula_buf;
    } else if (f->src_start >= 0 && f->src_end <= len && f->src_end > f->src_start) {
        snprintf(shown, sizeof shown, "%.*s", f->src_end - f->src_start, text + f->src_start);
        /* where it is now, for when the field gets the focus */
        snprintf(A.formula_buf, sizeof A.formula_buf, "%s", shown);
        A.formula_start = f->src_start;
        A.formula_end = f->src_end;
    } else {
        shown[0] = 0;
    }
    bool focused_before = ui_focus_id() == fid;
    int r = ui_text_field_ex(id, x0 + lwid, y, w - lwid, h, buf, (int)(buf == shown ? sizeof shown : sizeof A.formula_buf),
                             UI_TF_KEEP_FOCUS, "a formula");
    if (!focused_before && ui_focus_id() == fid) {
        /* just focused: edit a copy of the formula as it is in the script */
        snprintf(A.formula_buf, sizeof A.formula_buf, "%s", shown);
        A.formula_start = f->src_start;
        A.formula_end = f->src_end;
    }
    if ((r & UI_TEXT_CHANGED) && buf == A.formula_buf && A.formula_start >= 0 && A.formula_end <= len) {
        int nl = (int)strlen(A.formula_buf);
        editor_delete(A.ed, A.formula_start, A.formula_end - A.formula_start);
        editor_insert(A.ed, A.formula_start, A.formula_buf, nl);
        A.formula_end = A.formula_start + nl;
    }
    if (r & UI_TEXT_SUBMIT) run_editor();
    if (ui_last_hovered()) ui_tooltip("Edits the formula in the script");
}

/* a choice: a button with the current choice, opening the list below it */
static void config_choice(Sym *s, float x, float y, float w) {
    u32 *v = A.state + s->slot;
    float h = config_row_h() - px(4);
    int cur = FS_CLAMP((i32)v[0], 1, s->nchoices);
    char id[200];
    snprintf(id, sizeof id, "%s##cfgc_%s", s->choices[cur - 1], s->name);
    bool open = A.choice_open && !strcmp(A.choice_open, s->name);
    if (ui_button_ex(id, x, y, w, h, open ? UI_SELECTED : 0)) {
        snprintf(A.choice_name, sizeof A.choice_name, "%s", s->name);
        A.choice_open = open ? NULL : A.choice_name;
        open = !open;
    }
    ui_icon("expand_more", "v", x + w - h, y, h, UI_COL_TEXT_DIM);
    if (!open) return;
    int prev = ui_layer(UI_LAYER_POPUP);
    float ih = h, ph = ih * s->nchoices + px(8);
    float py = y + h + px(2);
    if (py + ph > (float)A.in.fb_h - px(4)) py = FS_MAX(px(4), y - ph - px(2)); /* no room below: open upwards */
    ui_panel_ex(x, py, w, ph, UI_COL_PANEL_HI, px(6));
    for (int k = 0; k < s->nchoices; k++) {
        snprintf(id, sizeof id, "%s##cfgci_%s_%d", s->choices[k], s->name, k);
        if (ui_button_ex(id, x + px(4), py + px(4) + k * ih, w - px(8), ih, k + 1 == cur ? UI_SELECTED : UI_FLAT)) {
            v[0] = (u32)(k + 1);
            A.choice_open = NULL;
        }
    }
    Rect area = {x, FS_MIN(y, py), w, FS_MAX(y + h, py + ph) - FS_MIN(y, py)};
    if ((A.in.mouse_pressed[0] || A.in.mouse_pressed[1]) && !in_rect(area, A.in.mouse_x, A.in.mouse_y)) A.choice_open = NULL;
    ui_layer(prev);
}

/* the config, in the right pane */
static void config_pane(Rect pr) {
    if (pr.w < 1 || pr.h < 1) return;
    Program *p = A.prog;
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_COL_PANEL, 0);
    ui_rect(pr.x, pr.y, px(1), pr.h, UI_COL_BORDER);
    float bh = px(40);
    ui_panel_ex(pr.x, pr.y, pr.w, bh, UI_COL_PANEL_HI, 0);
    ui_text(UI_FONT_SANS, pr.x + px(14), pr.y + px(11), "Config", -1, UI_COL_TEXT);
    if (!p) return;
    /* the values changed since the script ran (a dot marks each): into the script, or back */
    int nchanged = count_changed(p, true), nsettings = count_changed(p, false);
    float bx = pr.x + pr.w - px(34), by = pr.y + px(6);
    if (bx > pr.x + px(70) &&
        ui_icon_button_ex("##config_write", "edit_note", "W", bx, by, px(28), nchanged ? UI_FLAT : UI_FLAT | UI_DISABLED))
        write_values();
    ui_tooltip(nchanged ? "Write the changed values (marked with a dot) and the places of the views into the script, so it opens like this"
                        : "Write the values into the script (nothing changed since it ran)");
    bx -= px(32);
    if (bx > pr.x + px(70) &&
        ui_icon_button_ex("##config_reset", "restore", "R", bx, by, px(28), nsettings ? UI_FLAT : UI_FLAT | UI_DISABLED))
        reset_values();
    ui_tooltip("Set the changed values back as the script has them (the views stay: Home resets a view)");
    float rh = config_row_h();
    /* the width of the labels */
    float lw = px(60);
    char label[256];
    for (int i = 0; i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        if (!config_listed(p, s) && !config_auto(p, s, 1) && !config_auto(p, s, 2)) continue;
        config_label(s, label, sizeof label);
        lw = FS_MAX(lw, ui_text_width(UI_FONT_SANS, label, -1) + px(24));
    }
    lw = FS_MIN(lw, pr.w * 0.55f);
    Rect body = {pr.x + px(4), pr.y + bh + px(4), pr.w - px(8), pr.h - bh - px(8)};
    A.config_scroll += ui_scroll_area(body.x, body.y, body.w, body.h);
    ui_push_clip(body.x, body.y, body.w, body.h);
    float y = body.y - A.config_scroll + px(4);
    float x0 = body.x + px(10), x1 = body.x + lw, ww = body.w - lw - px(14);
    /* the script's own items: groups, notes and variables, in order */
    bool any_group = false;
    for (int i = 0; i < p->config.len; i++) {
        ConfigItem *it = &p->config.data[i];
        if (it->kind == CFG_GROUP) {
            bool empty = true; /* only groups that show something get a title */
            for (int k = i + 1; k < p->config.len && p->config.data[k].kind != CFG_GROUP; k++) empty = false;
            if (empty) continue;
            if (any_group) y += px(8);
            ui_text(UI_FONT_SANS, x0, y + px(6), it->text ? it->text : "Script", -1, UI_COL_ACCENT);
            y += rh;
            any_group = true;
        } else if (it->kind == CFG_FORMULA) {
            config_formula(it->fn, x0, y, body.w - px(20), lw);
            y += rh;
        } else if (it->kind == CFG_NOTE) {
            float th = ui_text_wrapped(UI_FONT_SANS, x0, y + px(2), body.w - px(20), it->text, UI_COL_TEXT_DIM, true);
            y += th + px(8);
        } else {
            Sym *s = it->sym;
            config_label(s, label, sizeof label);
            ui_label(x0, y, lw - px(14), rh - px(4), label, UI_ALIGN_LEFT, UI_COL_TEXT);
            config_changed_mark(p, s, x0, y, lw - px(14));
            if (s->nchoices) config_choice(s, x1, y, ww);
            else config_number(s, x1, y, ww);
            y += rh;
        }
    }
    /* views and iteration settings */
    for (int group = 1; group < 3; group++) {
        bool any = false;
        for (int i = 0; i < p->globals.len; i++)
            if (config_auto(p, p->globals.data[i], group)) any = true;
        if (!any && group == 2) continue;
        y += px(8);
        ui_text(UI_FONT_SANS, x0, y + px(6), group == 1 ? "Views" : "Iteration", -1, UI_COL_ACCENT);
        y += rh;
        for (int i = 0; i < p->globals.len + 1; i++) { /* (.doublePrecision after the views) */
            Sym *s = i < p->globals.len ? p->globals.data[i] : p->s_double;
            if (!config_auto(p, s, group) || (s == p->s_double && i < p->globals.len)) continue;
            config_label(s, label, sizeof label);
            ui_label(x0, y, lw - px(14), rh - px(4), label, UI_ALIGN_LEFT, UI_COL_TEXT);
            config_changed_mark(p, s, x0, y, lw - px(14));
            int vs = group == 1 ? s->viewer_index : -1;
            if (vs >= 0 && s == p->viewers[vs]->center) {
                y += config_center(vs, x0, x1, y, ww);
                continue;
            }
            if (vs >= 0 && s == p->viewers[vs]->radius) config_radius(vs, x1, y, ww);
            else config_number(s, x1, y, ww);
            y += rh;
        }
        if (group == 1) {
            for (int v = 0; v < p->nviewers; v++) {
                char id[64];
                snprintf(id, sizeof id, "Reset %s##rv%d", p->viewers[v]->name, v);
                float tw = ui_text_width(UI_FONT_SANS, id, (int)(strstr(id, "##") - id)) + px(16);
                float bx = tw > ww ? x0 : x1; /* (a long name: the whole row) */
                if (ui_button_ex(id, bx, y, x1 + ww - bx, rh - px(4), 0)) reset_view(v);
                y += rh;
            }
        }
    }
    float content = y + A.config_scroll - body.y;
    A.config_scroll = FS_CLAMP(A.config_scroll, 0.0f, FS_MAX(0.0f, content - body.h));
    ui_pop_clip();
}

/* ---- the Info pane: what the script printed, and what Inspect found -------------------------------------------- */
static const char *out_title(int k, char *buf, int cap) {
    Program *p = A.prog;
    if (k < p->ntools) snprintf(buf, (size_t)cap, "%.150s", p->tools[k]->display);
    else if (k == out_prepare()) snprintf(buf, (size_t)cap, "@prepare");
    else if (k == out_script()) snprintf(buf, (size_t)cap, "@global and @config");
    else snprintf(buf, (size_t)cap, "Inspect");
    return buf;
}

/* `text` in the mono font from y, wrapped at w (between words, but not inside "a + bi": a complex number stays on one
 * line when it fits), the continuation lines indented; only the lines between clip_y0 and clip_y1 are drawn.  Returns
 * the height. */
static float info_text(float x, float y, float w, const char *text, float clip_y0, float clip_y1) {
    float y0 = y, lh = ui_font_line_height(UI_FONT_MONO), cw = ui_font_advance(UI_FONT_MONO, 'm');
    int cols0 = FS_MAX(12, (int)(w / FS_MAX(cw, 1.0f))), hang = 4;
    for (const char *q = text; *q;) {
        const char *e = strchr(q, '\n');
        int n = e ? (int)(e - q) : (int)strlen(q);
        int lead = 0; /* (a line's own indentation, kept on its continuation lines) */
        while (lead < n && q[lead] == ' ') lead++;
        /* break points: after a space, unless the space is next to a lone + or - (a + bi) */
        int start = 0;
        for (;;) {
            int indent = start ? FS_MIN(lead + hang, cols0 / 2) : 0, cols = cols0 - indent;
            int end = n, chars = 0, last_break = -1;
            for (int i = start; i < n; i = ui_utf8_next(q, i)) {
                if (chars >= cols) {
                    end = last_break > start ? last_break : i;
                    break;
                }
                bool op_space = q[i] == ' ' && ((i + 2 < n && (q[i + 1] == '+' || q[i + 1] == '-') && q[i + 2] == ' ') ||
                                                (i >= 2 && (q[i - 1] == '+' || q[i - 1] == '-') && q[i - 2] == ' '));
                if (q[i] == ' ' && !op_space) last_break = i + 1;
                chars++;
            }
            if (y + lh > clip_y0 && y < clip_y1 && end > start)
                ui_text(UI_FONT_MONO, x + indent * cw, y, q + start, end - start, UI_COL_TEXT);
            y += lh;
            if (end >= n || end <= start) break;
            start = end;
        }
        if (!e) break;
        q = e + 1;
    }
    return y - y0;
}

static void info_pane(Rect pr) {
    if (pr.w < 1 || pr.h < 1) return;
    Program *p = A.prog;
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_COL_PANEL, 0);
    ui_rect(pr.x, pr.y, px(1), pr.h, UI_COL_BORDER);
    float bh = px(40);
    ui_panel_ex(pr.x, pr.y, pr.w, bh, UI_COL_PANEL_HI, 0);
    ui_text(UI_FONT_SANS, pr.x + px(14), pr.y + px(11), "Info", -1, UI_COL_TEXT);
    A.out_seen = out_total();
    if (!p) return;
    int order[80], n = 0; /* Inspect, the tools in the script's order, @prepare, the script */
    order[n++] = out_inspect();
    for (int k = 0; k < p->ntools && n < 77; k++) order[n++] = k;
    order[n++] = out_prepare();
    order[n++] = out_script();
    bool any = false;
    for (int i = 0; i < n; i++) any = any || A.outs[order[i]].len > 0;
    if (any) {
        if (ui_icon_button_ex("##info_clear", "delete_sweep", "X", pr.x + pr.w - px(34), pr.y + px(6), px(28), UI_FLAT))
            for (int i = 0; i < n; i++) out_set(order[i], "", false);
        ui_tooltip("Clear all of it");
    }
    Rect body = {pr.x + px(4), pr.y + bh + px(4), pr.w - px(8), pr.h - bh - px(8)};
    A.info_scroll += ui_scroll_area(body.x, body.y, body.w, body.h);
    ui_push_clip(body.x, body.y, body.w, body.h);
    float y = body.y - A.info_scroll + px(6), x0 = body.x + px(10), tw = body.w - px(20);
    if (!any)
        y += ui_text_wrapped(UI_FONT_SANS, x0, y, tw,
                             "Nothing yet. Tools and @prepare write here with print (print \"period \" + n), and "
                             "the Inspect tool (the last button under each view, or ?) shows the values a view "
                             "computes at the point you click.",
                             UI_COL_TEXT_DIM, true);
    char title[200], id[48];
    for (int i = 0; i < n; i++) {
        int k = order[i];
        if (!A.outs[k].len) continue;
        float lh = ui_font_line_height(UI_FONT_SANS);
        ui_label(x0, y, tw - px(64), lh + px(6), out_title(k, title, sizeof title), UI_ALIGN_LEFT, UI_COL_ACCENT);
        snprintf(id, sizeof id, "##info_copy%d", k);
        if (ui_icon_button_ex(id, "content_copy", "C", x0 + tw - px(56), y, px(26), UI_FLAT))
            platform_set_clipboard(sb_cstr(&A.outs[k]));
        ui_tooltip("Copy this text");
        snprintf(id, sizeof id, "##info_clear%d", k);
        if (ui_icon_button_ex(id, "close", "x", x0 + tw - px(28), y, px(26), UI_FLAT)) out_set(k, "", false);
        ui_tooltip("Clear it");
        y += lh + px(10);
        y += info_text(x0, y, tw, sb_cstr(&A.outs[k]), body.y, body.y + body.h) + px(12);
    }
    float content = y + A.info_scroll - body.y;
    A.info_scroll = FS_CLAMP(A.info_scroll, 0.0f, FS_MAX(0.0f, content - body.h));
    ui_pop_clip();
}

/* the pane left of the right bar: the info on top of the config when both are open (the line between them drags) */
static void right_pane(float W, float H) {
    Rect pr = A.config_r;
    if (pr.w < 1) return;
    if (A.info_open && A.config_open) {
        float ih = FS_CLAMP(floorf(H * A.info_frac), px(100), FS_MAX(px(100), H - px(140)));
        info_pane((Rect){pr.x, 0, pr.w, ih});
        config_pane((Rect){pr.x, ih, pr.w, H - ih});
        ui_rect(pr.x, ih, pr.w, px(1), UI_COL_BORDER);
        int prev = ui_layer(UI_LAYER_PANEL);
        UiInteract it = ui_interact("##info_split", pr.x, ih - px(4), pr.w, px(8));
        ui_layer(prev);
        if (it.hovered || it.held) ui_set_cursor(UI_CURSOR_VRESIZE);
        if (it.held && H > 0) A.info_frac = FS_CLAMP(A.in.mouse_y / H, 0.1f, 0.85f);
    } else if (A.info_open) {
        info_pane(pr);
    } else {
        config_pane(pr);
    }
    float nx = pane_splitter("##config_split", pr.x, H);
    if (nx == nx && W > 0) A.config_frac = FS_CLAMP((A.rbar_r.x - nx) / W, 0.12f, 0.6f);
}

/* ---- overlays ------------------------------------------------------------------------- */
static void help_overlay(float W, float H) {
    if (!A.help_open) return;
    static const char *lines[][2] = { /* keys, what they do (headings have no second part) */
        {"Mouse", NULL},
        {"Wheel", "zoom at the mouse"},
        {"Right or middle drag", "pan (also Space + drag)"},
        {"Left click / drag", "use the view's selected tool"},
        {"Side buttons", "back / forward: where the view was"},
        {"Left bar", "the script; each view's number (click: show it; drag: split, float) and its tools"},
        {"Right bar", "the config; back, forward, reset, location, grid, pictures of the view clicked last"},
        {"Keys", NULL},
        {MODKEY "+Enter or " MODKEY "+R", "run the script"},
        {MODKEY "+S, " MODKEY "+Shift+S", "save, save as"},
        {MODKEY "+O, " MODKEY "+N", "open a script (or drop it on the window), a new one from a template"},
        {MODKEY "+F, " MODKEY "+Alt+F", "find, replace in the script (" MODKEY "+G: next)"},
        {"Tab", "in the script, while a list of names is offered: take the one shown (Up, Down: choose)"},
        {MODKEY "+E, " MODKEY "+T, " MODKEY "+I", "show / hide the script, the config, the info"},
        {MODKEY "+=, " MODKEY "+-, " MODKEY "+0", "bigger, smaller, normal size (of everything; in the script: of its text)"},
        {MODKEY "+1, 2, ...", "show a view (or the shortcut written in the script)"},
        {"H, P, ?, tool keys", "choose a tool of the view under the mouse (? is Inspect)"},
        {"Home", "reset the view under the mouse"},
        {"Alt+Left, Alt+Right", "back / forward: where the view was (also " MODKEY "+[ and " MODKEY "+])"},
        {MODKEY "+Shift+C, " MODKEY "+Shift+V", "copy / paste the location of the view (centre and radius)"},
        {MODKEY "+Shift+A", "axes and grid lines with their values"},
#if FS_OS_MACOS
        {"F11, Cmd+Ctrl+F", "full screen"},
#else
        {"F11", "full screen"},
#endif
        {"F1, F2 (or " MODKEY "+?)", "this help, the language reference (from the script: at the word under the caret)"},
    };
    int prev = ui_layer(UI_LAYER_POPUP);
    float lh = ui_font_line_height(UI_FONT_MONO) + px(3);
    float kw = 0, dw = 0; /* the columns: keys, what they do */
    for (int i = 0; i < ARRAY_COUNT(lines); i++)
        if (lines[i][1]) {
            kw = FS_MAX(kw, ui_text_width(UI_FONT_MONO, lines[i][0], -1));
            dw = FS_MAX(dw, ui_text_width(UI_FONT_SANS, lines[i][1], -1));
        }
    float pw = FS_MIN(px(32) + kw + px(24) + dw + px(24), W - px(40)), ph = (ARRAY_COUNT(lines) + 2) * lh + px(20);
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    UiInteract bg = ui_interact("##help_bg", 0, 0, W, H);
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 245), px(10));
    for (int i = 0; i < ARRAY_COUNT(lines); i++) {
        float y = pr.y + px(14) + i * lh;
        if (!lines[i][1]) {
            ui_text(UI_FONT_SANS, pr.x + px(18), y, lines[i][0], -1, UI_COL_ACCENT);
            continue;
        }
        ui_text(UI_FONT_MONO, pr.x + px(32), y, lines[i][0], -1, UI_COL_TEXT);
        ui_text(UI_FONT_SANS, pr.x + px(32) + kw + px(24), y, lines[i][1], -1, UI_COL_TEXT);
    }
    ui_label(pr.x, pr.y + ph - lh - px(8), pr.w, lh, "click anywhere to close", UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
    if (bg.clicked) A.help_open = false;
    ui_layer(prev);
}

/* ---- the open / save dialogs: a path, and the folder it is in ------------------------------------------------- */
static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return !n;
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* "a/b/c" -> folder "a/b", name "c"; "a/b/" -> "a/b", ""; "c" -> "", "c"; "/c" -> "/", "c" */
static const char *split_path(const char *path, char *dir, int cap) {
    const char *slash = NULL;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') slash = q;
    if (!slash) {
        dir[0] = 0;
        return path;
    }
    int n = slash == path ? 1 : (int)(slash - path);
    snprintf(dir, (size_t)cap, "%.*s", n, path);
    return slash + 1;
}

/* the examples that come with the program: next to it, above it (build/), or in the app bundle's Resources */
static const char *examples_dir(void) {
    static char dir[1100];
    static bool done;
    if (done) return dir;
    done = true;
    const char *exe = fs_exe_dir();
    static const char *rel[] = {"/examples", "/../examples", "/../Resources/examples", "/../../examples"};
    for (int k = 0; exe[0] && k < ARRAY_COUNT(rel); k++) {
        char p[1100];
        snprintf(p, sizeof p, "%.1000s%s", exe, rel[k]);
        if (fs_is_dir(p) && fs_absolute(p, dir, sizeof dir)) return dir;
    }
    dir[0] = 0;
    return dir;
}

static const char *home_dir(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = getenv("USERPROFILE");
    return home && home[0] ? home : NULL;
}

/* the dialog's path becomes `path` (a folder: its contents are listed), the caret at its end */
static void browse_to(const char *path, bool folder) {
    char abs[1024];
    if (folder && fs_absolute(path, abs, sizeof abs)) path = abs;
    size_t n = strlen(path);
    snprintf(A.modal_buf, sizeof A.modal_buf, "%.1020s%s", path, folder && n && path[n - 1] != '/' && path[n - 1] != '\\' ? "/" : "");
    A.modal_focus = true;
    A.modal_confirm = false;
    A.browse_scroll = 0;
}

static void open_modal(int kind, const char *path) {
    A.modal = kind;
    A.modal_focus = true;
    A.modal_select_all = false; /* (the caret at the end: type a name, or more of it) */
    A.modal_confirm = false;
    A.browse_scroll = 0;
    A.browse_recent = false;
    snprintf(A.modal_buf, sizeof A.modal_buf, "%.1020s", path);
}

/* a new script from a template (the templates among the examples) */
static void ask_new(void) {
    if (!unsaved_guard(PENDING_NEW, NULL)) return;
    char dir[1100];
    const char *ex = examples_dir();
    snprintf(dir, sizeof dir, "%.1000s/templates", ex);
    if (!ex[0] || !fs_is_dir(dir)) snprintf(dir, sizeof dir, "%.1000s", ex[0] ? ex : home_dir() ? home_dir() : ".");
    strcat(dir, "/");
    open_modal(4, dir);
}

/* after the question about unsaved changes: what was asked for */
static void perform_pending(void) {
    int what = A.pending;
    A.pending = PENDING_NONE;
    A.discard_ok = true;
    if (what == PENDING_OPEN) request_open(A.pending_path);
    else if (what == PENDING_NEW) ask_new();
    else if (what == PENDING_QUIT) {
        A.quit_now = true;
        platform_set_should_close(true);
    } else A.discard_ok = false;
}

/* "Save the changes?": Save, Don't save, Cancel */
static void unsaved_dialog(float W, float H) {
    int prev = ui_layer(UI_LAYER_POPUP);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    float pw = FS_MIN(px(520), W - px(40)), ph = px(150);
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 250), px(10));
    char q[400];
    snprintf(q, sizeof q, "Save the changes to %.200s?", A.path[0] ? fs_basename(A.path) : "the new script");
    ui_text(UI_FONT_SANS, pr.x + px(18), pr.y + px(18), q, -1, UI_COL_TEXT);
    const char *what = A.pending == PENDING_QUIT ? "Otherwise they are lost when FractalStream closes."
                       : A.pending == PENDING_NEW ? "Otherwise they are lost with the new script."
                                                  : "Otherwise they are lost when the other script opens.";
    ui_text(UI_FONT_SANS, pr.x + px(18), pr.y + px(48), what, -1, UI_COL_TEXT_DIM);
    float by = pr.y + ph - px(48), bw = px(110);
    bool save = ui_button_ex("Save##uns_save", pr.x + pr.w - 3 * (bw + px(10)), by, bw, px(32), UI_ACCENT);
    bool drop = ui_button_ex("Don't save##uns_drop", pr.x + pr.w - 2 * (bw + px(10)), by, bw, px(32), 0);
    bool cancel = ui_button_ex("Cancel##uns_cancel", pr.x + pr.w - (bw + px(10)), by, bw, px(32), 0);
    if (ui_shortcut(UI_KEY_ENTER, 0)) save = true;
    if (ui_shortcut(UI_KEY_D, UI_MOD_MOD) || ui_shortcut(UI_KEY_D, 0)) drop = true;
    if (ui_shortcut(UI_KEY_ESCAPE, 0)) cancel = true;
    if (save) {
        A.modal = 0;
        if (A.path[0] && !inside_app_bundle(A.path)) {
            if (save_file(A.path)) perform_pending();
            else A.pending = PENDING_NONE;
        } else {
            ask_save_as(); /* (then what was asked for: see the save dialog) */
        }
    } else if (drop) {
        A.modal = 0;
        perform_pending();
    } else if (cancel) {
        A.modal = 0;
        A.pending = PENDING_NONE;
    }
    ui_layer(prev);
}

static void modal_dialog(float W, float H) {
    if (A.modal == 5) {
        unsaved_dialog(W, H);
        return;
    }
    if (!A.modal) {
        if (A.browse) fs_free_dir(A.browse, A.nbrowse);
        A.browse = NULL;
        A.nbrowse = 0;
        A.browse_dir[0] = 0;
        if (A.pending && A.pending != PENDING_NONE) A.pending = PENDING_NONE; /* (a save asked for was cancelled) */
        return;
    }
    int prev = ui_layer(UI_LAYER_POPUP);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    bool saving = A.modal == 1 || A.modal == 3;
    const char *ext = A.modal == 3 ? ".png" : ".fstream";
    float rh = px(26), pad = px(16);
    float pw = FS_MIN(px(720), W - px(40)), ph = FS_MIN(px(560), H - px(40));
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 250), px(10));
    ui_text(UI_FONT_SANS, pr.x + pad, pr.y + px(14),
            A.modal == 1 ? "Save the script as:" : A.modal == 3 ? "Save the picture as (PNG):"
            : A.modal == 4 ? "A new script, from a template (or the welcome example):" : "Open a script:", -1, UI_COL_TEXT);
    char before[1024];
    snprintf(before, sizeof before, "%s", A.modal_buf);
    int r = ui_text_field_ex("##modal_path", pr.x + pad, pr.y + px(44), pr.w - 2 * pad, px(30), A.modal_buf,
                             (int)sizeof A.modal_buf, A.modal_select_all ? UI_TF_SELECT_ALL : 0, "path of the file");
    if (A.modal_focus) {
        ui_set_focus(ui_id("##modal_path"));
        A.modal_focus = false;
    } else {
        A.modal_select_all = false;
    }
    if (strcmp(before, A.modal_buf) != 0) A.modal_confirm = false;
    /* places */
    float x = pr.x + pad, y = pr.y + px(84);
    char doc_dir[1100], script_dir[1100];
    const char *home = home_dir();
    doc_dir[0] = script_dir[0] = 0;
    if (home) snprintf(doc_dir, sizeof doc_dir, "%s/Documents", home);
    if (!doc_dir[0] || !fs_is_dir(doc_dir)) doc_dir[0] = 0;
    if (A.path[0]) split_path(A.path, script_dir, sizeof script_dir);
    if (A.modal == 4) { /* the welcome example (built in) */
        float bw = ui_text_width(UI_FONT_SANS, "Welcome example", -1) + px(24);
        if (ui_button_ex("Welcome example##place_welcome", x, y, bw, px(26), UI_FLAT)) {
            A.modal = 0;
            new_from(NULL);
        }
        x += bw + px(6);
    }
    const char *places[5][3] = {{"Recent", "history", A.modal == 2 && A.nrecent ? "" : NULL},
                                {"Home", "home", home}, {"Documents", "folder", doc_dir[0] ? doc_dir : NULL},
                                {"Examples", "school", examples_dir()[0] ? examples_dir() : NULL},
                                {"The script's folder", "folder_open", A.path[0] ? (script_dir[0] ? script_dir : ".") : NULL}};
    if (strcmp(before, A.modal_buf) != 0) A.browse_recent = false; /* (typing: the folder again) */
    for (int k = 0; k < 5; k++) {
        if (!places[k][2]) continue;
        char id[64];
        snprintf(id, sizeof id, "%s##place%d", places[k][0], k);
        float bw = ui_text_width(UI_FONT_SANS, places[k][0], -1) + px(24);
        if (x + bw > pr.x + pr.w - pad) break;
        if (ui_button_ex(id, x, y, bw, px(26), k == 0 && A.browse_recent ? UI_SELECTED : UI_FLAT)) {
            if (k == 0) {
                A.browse_recent = !A.browse_recent;
                A.browse_scroll = 0;
                A.modal_focus = true;
            } else {
                browse_to(places[k][2], true);
                A.browse_recent = false;
            }
        }
        x += bw + px(6);
    }
    /* the folder of the path, listed (again when it changes) */
    char dir[1024];
    const char *name = split_path(A.modal_buf, dir, sizeof dir);
    if (!A.browse || strcmp(dir, A.browse_dir) != 0) {
        if (A.browse) fs_free_dir(A.browse, A.nbrowse);
        A.browse = fs_list_dir(dir, &A.nbrowse);
        snprintf(A.browse_dir, sizeof A.browse_dir, "%s", dir);
        A.browse_scroll = 0;
    }
    Rect lr = {pr.x + pad, y + px(34), pr.w - 2 * pad, pr.y + pr.h - px(56) - (y + px(34))};
    ui_rrect(lr.x, lr.y, lr.w, lr.h, px(6), UI_RGBA(16, 18, 23, 255));
    ui_rrect_outline(lr.x, lr.y, lr.w, lr.h, px(6), 1, UI_COL_BORDER);
    int rows[4096], nrows = 0; /* entries shown: -1 is "..", then folders and the files of the kind */
    if (A.browse_recent) {    /* (or the recent files: -2 - k is A.recent[k]) */
        for (int k = 0; k < A.nrecent; k++) rows[nrows++] = -2 - k;
    } else {
        if (A.browse) rows[nrows++] = -1;
        for (int i = 0; i < A.nbrowse && nrows < ARRAY_COUNT(rows); i++) {
            FsDirEntry *e = &A.browse[i];
            if (!e->is_dir && !has_ext(e->name, ext)) continue;
            if (!saving && name[0] && !contains_nocase(e->name, name)) continue; /* (opening: typing filters) */
            rows[nrows++] = i;
        }
    }
    float content = nrows * rh;
    A.browse_scroll = FS_CLAMP(A.browse_scroll + ui_scroll_area(lr.x, lr.y, lr.w, lr.h), 0.0f, FS_MAX(0.0f, content - lr.h + px(8)));
    ui_push_clip(lr.x + 1, lr.y + 1, lr.w - 2, lr.h - 2);
    bool submit_file = false;
    for (int k = 0; k < nrows; k++) {
        float ry = lr.y + px(4) + k * rh - A.browse_scroll;
        if (ry + rh < lr.y || ry > lr.y + lr.h) continue;
        int i = rows[k];
        char id[64];
        snprintf(id, sizeof id, "##brow%d", k);
        if (i <= -2) { /* a recent file: its name, and its folder dimmer */
            const char *file = A.recent[-2 - i];
            UiInteract it = ui_interact(id, lr.x + px(4), ry, lr.w - px(8), rh);
            bool chosen = !strcmp(file, A.modal_buf);
            if (it.hovered || chosen) ui_rrect(lr.x + px(4), ry, lr.w - px(8), rh, px(4), chosen ? UI_COL_ACCENT_DIM : UI_COL_WIDGET_HOVER);
            ui_icon("description", NULL, lr.x + px(10), ry + px(3), rh - px(6), UI_COL_TEXT_DIM);
            const char *base = fs_basename(file);
            float nw = ui_text_width(UI_FONT_SANS, base, -1);
            ui_label(lr.x + px(14) + rh, ry, lr.w - rh - px(24), rh, base, UI_ALIGN_LEFT, UI_COL_TEXT);
            ui_label(lr.x + px(26) + rh + nw, ry, lr.w - rh - px(36) - nw, rh, file, UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
            if (it.pressed) {
                snprintf(A.modal_buf, sizeof A.modal_buf, "%s", file);
                snprintf(before, sizeof before, "%s", file); /* (not typing: stay in the list) */
                A.modal_focus = true;
                if (it.double_clicked) submit_file = true;
            }
            continue;
        }
        bool is_dir = i < 0 || A.browse[i].is_dir;
        const char *label = i < 0 ? ".. (the folder above)" : A.browse[i].name;
        UiInteract it = ui_interact(id, lr.x + px(4), ry, lr.w - px(8), rh);
        bool chosen = i >= 0 && !strcmp(A.browse[i].name, name);
        if (it.hovered || chosen) ui_rrect(lr.x + px(4), ry, lr.w - px(8), rh, px(4), chosen ? UI_COL_ACCENT_DIM : UI_COL_WIDGET_HOVER);
        ui_icon(i < 0 ? "arrow_upward" : is_dir ? "folder" : A.modal == 3 ? "image" : "description", NULL, lr.x + px(10),
                ry + px(3), rh - px(6), is_dir ? UI_COL_ACCENT : UI_COL_TEXT_DIM);
        ui_label(lr.x + px(14) + rh, ry, lr.w - rh - px(24), rh, label, UI_ALIGN_LEFT, i < 0 ? UI_COL_TEXT_DIM : UI_COL_TEXT);
        if (!it.pressed) continue;
        char path[2100];
        if (i < 0) { /* the folder above */
            char abs[1024];
            fs_absolute(dir[0] ? dir : ".", abs, sizeof abs);
            char up[1024];
            split_path(abs, up, sizeof up);
            browse_to(up[0] ? up : "/", true);
        } else if (is_dir) {
            snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && strcmp(dir, "/") ? "/" : "", A.browse[i].name);
            browse_to(path, true);
        } else {
            snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && strcmp(dir, "/") ? "/" : "", A.browse[i].name);
            browse_to(path, false);
            if (it.double_clicked) submit_file = true;
        }
    }
    ui_pop_clip();
    if (!A.browse && !A.browse_recent) ui_label(lr.x, lr.y, lr.w, px(40), "(this folder can't be read)", UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
    /* OK: open or save; a folder is opened in the list instead */
    float by = pr.y + pr.h - px(44);
    if (A.modal_confirm)
        ui_label(pr.x + pad, by, pr.w - px(230), px(30), "That file exists: OK replaces it.", UI_ALIGN_LEFT, UI_COL_WARNING);
    bool ok = ui_button_ex("OK##modal_ok", pr.x + pr.w - px(196), by, px(84), px(30), UI_ACCENT);
    bool cancel = ui_button_ex("Cancel##modal_cancel", pr.x + pr.w - px(100), by, px(84), px(30), 0);
    if (r & UI_TEXT_SUBMIT) ok = true;
    ok = ok || submit_file;
    if ((r & UI_TEXT_CANCEL) || ui_shortcut(UI_KEY_ESCAPE, 0)) cancel = true;
    if (ok && A.modal_buf[0]) {
        size_t n = strlen(A.modal_buf);
        bool trailing = A.modal_buf[n - 1] == '/' || A.modal_buf[n - 1] == '\\';
        if (fs_is_dir(A.modal_buf)) {
            if (!trailing) browse_to(A.modal_buf, true);
            else set_status(true, "That is a folder: choose a file in it%s", saving ? ", or type a name" : "");
            ok = false;
        } else if (saving && !has_ext(A.modal_buf, ext) && n + strlen(ext) < sizeof A.modal_buf) {
            strcat(A.modal_buf, ext); /* (the file gets its extension) */
        }
        if (ok && saving && !A.modal_confirm) {
            struct stat st;
            if (stat(A.modal_buf, &st) == 0 && !(A.modal == 1 && !strcmp(A.modal_buf, A.path))) {
                A.modal_confirm = true; /* ask once */
                ok = false;
            }
        }
        if (ok && !saving) {
            struct stat st;
            if (stat(A.modal_buf, &st) != 0) { /* a name typed in part: the file it is (name.fstream), or the only one */
                int first = -1, count = 0, exact = -1;
                for (int k = 0; k < nrows; k++) {
                    int i = rows[k];
                    if (i < 0 || A.browse[i].is_dir) continue;
                    if (first < 0) first = i;
                    count++;
                    size_t nl = strlen(name);
                    if (!strncasecmp(A.browse[i].name, name, nl) && !strcasecmp(A.browse[i].name + nl, ext)) exact = i;
                }
                int pick = exact >= 0 ? exact : count == 1 ? first : -1;
                char path[2100];
                if (pick >= 0 || first >= 0) {
                    snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && strcmp(dir, "/") ? "/" : "", A.browse[pick >= 0 ? pick : first].name);
                    browse_to(path, false);
                }
                if (pick < 0) {
                    if (first < 0) set_status(true, "There is no file %s", A.modal_buf);
                    ok = false;
                }
            }
        }
    }
    if (ok && A.modal_buf[0]) {
        int kind = A.modal;
        A.modal = 0; /* (before what follows, which may open another dialog) */
        if (kind == 1) {
            if (save_file(A.modal_buf) && A.pending) perform_pending(); /* (saved before an open, a new script or quitting) */
        } else if (kind == 4) {
            new_from(A.modal_buf);
        } else if (kind == 3) {
            snprintf(A.req_export, sizeof A.req_export, "%s", A.modal_buf);
            A.export_wait = 2;
            set_status(false, "Saving the picture...");
        } else {
            request_open(A.modal_buf);
        }
    } else if (cancel) {
        A.modal = 0;
    }
    ui_layer(prev);
}

static void status_line(float W, float H) {
    double now = fs_time_seconds();
    if (!A.status[0] || now > A.status_until) return;
    int prev = ui_layer(UI_LAYER_TOOLTIP);
    float a = (float)FS_MIN(1.0, (A.status_until - now) / 0.5);
    float tw = FS_MIN(ui_text_width(UI_FONT_SANS, A.status, -1) + px(24), W - px(20));
    float th = ui_font_line_height(UI_FONT_SANS) + px(12);
    Rect r = {A.free.x + px(10), H - th - px(10), tw, th};
    ui_rrect(r.x, r.y, r.w, r.h, px(6), ui_alpha(A.status_err ? UI_RGBA(80, 24, 28, 235) : UI_RGBA(28, 32, 40, 235), a));
    ui_push_clip(r.x, r.y, r.w, r.h);
    ui_text(UI_FONT_SANS, r.x + px(12), r.y + px(6), A.status, -1, ui_alpha(UI_COL_TEXT, a));
    ui_pop_clip();
    ui_wake_in(0.1);
    ui_layer(prev);
}

/* ---- keyboard ---------------------------------------------------------------------- */
static int target_viewer(void) { return A.hover_viewer >= 0 ? A.hover_viewer : A.focus_viewer; }

static void shortcuts(void) {
    Program *p = A.prog;
    if (ui_shortcut(UI_KEY_F, UI_MOD_MOD)) open_find(false);
    if (ui_shortcut(UI_KEY_F, UI_MOD_MOD | UI_MOD_ALT)) open_find(true);
    if (ui_shortcut(UI_KEY_G, UI_MOD_MOD) || ui_shortcut(UI_KEY_F3, 0)) {
        if (A.find_buf[0]) find_step(false);
        else open_find(false);
    }
    if (ui_shortcut(UI_KEY_G, UI_MOD_MOD | UI_MOD_SHIFT) || ui_shortcut(UI_KEY_F3, UI_MOD_SHIFT)) find_step(true);
    if (ui_shortcut(UI_KEY_ENTER, UI_MOD_MOD) || ui_shortcut(UI_KEY_R, UI_MOD_MOD)) run_editor();
    if (ui_shortcut(UI_KEY_S, UI_MOD_MOD)) {
        save_or_ask();
    }
    if (ui_shortcut(UI_KEY_S, UI_MOD_MOD | UI_MOD_SHIFT)) ask_save_as();
    if (ui_shortcut(UI_KEY_O, UI_MOD_MOD)) ask_open();
    if (ui_shortcut(UI_KEY_N, UI_MOD_MOD)) ask_new();
    /* the size of the interface (when the script has the keyboard, these are the size of its text) */
    while (ui_shortcut_repeat(UI_KEY_EQUAL, UI_MOD_MOD) || ui_shortcut_repeat(UI_KEY_EQUAL, UI_MOD_MOD | UI_MOD_SHIFT))
        set_ui_zoom(A.ui_zoom * 1.1f);
    while (ui_shortcut_repeat(UI_KEY_MINUS, UI_MOD_MOD)) set_ui_zoom(A.ui_zoom / 1.1f);
    if (ui_shortcut(UI_KEY_0, UI_MOD_MOD)) set_ui_zoom(1);
    if (ui_shortcut(UI_KEY_E, UI_MOD_MOD)) A.editor_open = !A.editor_open;
    if (ui_shortcut(UI_KEY_T, UI_MOD_MOD)) A.config_open = !A.config_open;
    if (ui_shortcut(UI_KEY_I, UI_MOD_MOD)) A.info_open = !A.info_open;
    if (ui_shortcut(UI_KEY_F11, 0)) platform_toggle_fullscreen();
#if FS_OS_MACOS
    if (ui_shortcut(UI_KEY_F, UI_MOD_MOD | UI_MOD_CTRL)) platform_toggle_fullscreen(); /* (Cmd+Ctrl+F, as on a Mac) */
#endif
    if (ui_shortcut(UI_KEY_F1, 0)) A.help_open = !A.help_open;
    if (ui_shortcut(UI_KEY_F2, 0) || ui_shortcut(UI_KEY_SLASH, UI_MOD_MOD | UI_MOD_SHIFT)) { /* (F2 or Cmd+?; in the
                                                                    script: the reference at the word under the caret) */
        A.ref_open = !A.ref_open;
        A.help_open = false;
        if (A.ref_open && editor_has_focus(A.ed)) {
            int len, c = editor_cursor(A.ed), a, b;
            const char *t = editor_get_text(A.ed, &len);
            for (a = c; a > 0 && (isalnum((unsigned char)t[a - 1]) || t[a - 1] == '_'); a--) {}
            for (b = c; b < len && (isalnum((unsigned char)t[b]) || t[b] == '_'); b++) {}
            if (a > 0 && (t[a - 1] == '.' || t[a - 1] == '@')) a--;
            if (b > a && b - a < 64) {
                char word[64];
                snprintf(word, sizeof word, "%.*s", b - a, t + a);
                refdoc_lookup(word);
            }
        }
    }
    if (ui_shortcut(UI_KEY_ESCAPE, 0)) {
        if (A.drag == DRAG_VIEWER) A.drag = DRAG_NONE;
        else if (A.help_open) A.help_open = false;
        else if (A.ref_open) A.ref_open = false;
        else if (A.find_open && editor_has_focus(A.ed)) close_find();
    }
    for (int v = 0; v < p->nviewers; v++) {
        Shortcut sc = viewer_shortcut(v);
        if (sc.key == UI_KEY_NONE || (!(sc.mods & (UI_MOD_MOD | UI_MOD_ALT)) && ui_wants_keyboard())) continue;
        if (ui_shortcut(sc.key, sc.mods) && !A.vrt[v].gone) {
            int leaf = find_leaf(A.root, A.focus_viewer);
            if (leaf < 0) leaf = A.root;
            if (leaf >= 0 && A.nodes[leaf].kind == N_LEAF && !leaf_gone(leaf)) show_in_leaf(leaf, v);
            else show_view(v);
        }
    }
    if (ui_wants_keyboard()) return;
    int tv = target_viewer();
    for (int t = 0; t < p->ntools; t++) {
        Shortcut sc = parse_shortcut(p->tools[t]->shortcut);
        if (sc.key == UI_KEY_NONE || !ui_shortcut(sc.key, sc.mods)) continue;
        /* the view under the mouse if the tool belongs there, else a visible view it belongs to */
        int v = tv >= 0 && tool_in_viewer(t, tv) ? tv : -1;
        for (int i = 0; v < 0 && i < A.nvis; i++)
            if (tool_in_viewer(t, A.vis[i].viewer)) v = A.vis[i].viewer;
        if (v >= 0) select_tool(v, t);
    }
    if (tv >= 0 && ui_shortcut(UI_KEY_H, 0)) select_tool(tv, TOOL_PAN);
    if (tv >= 0 && ui_shortcut(UI_KEY_P, 0) && p->viewers[tv]->picked) select_tool(tv, TOOL_PICK);
    if (tv >= 0 && ui_shortcut(UI_KEY_SLASH, UI_MOD_SHIFT)) select_tool(tv, TOOL_INSPECT);
    if (tv >= 0 && ui_shortcut(UI_KEY_HOME, 0)) reset_view(tv);
    if (tv >= 0 && (ui_shortcut(UI_KEY_LEFT, UI_MOD_ALT) || ui_shortcut(UI_KEY_LEFT_BRACKET, UI_MOD_MOD))) nav_key(tv, false);
    if (tv >= 0 && (ui_shortcut(UI_KEY_RIGHT, UI_MOD_ALT) || ui_shortcut(UI_KEY_RIGHT_BRACKET, UI_MOD_MOD))) nav_key(tv, true);
    if (tv >= 0 && ui_shortcut(UI_KEY_C, UI_MOD_MOD | UI_MOD_SHIFT) && !A.prog->viewers[tv]->mobius) copy_location(tv);
    if (tv >= 0 && ui_shortcut(UI_KEY_V, UI_MOD_MOD | UI_MOD_SHIFT) && !A.prog->viewers[tv]->mobius) paste_location(tv);
    if (tv >= 0 && ui_shortcut(UI_KEY_A, UI_MOD_MOD | UI_MOD_SHIFT) && !A.prog->viewers[tv]->mobius)
        A.vrt[tv].grid = !A.vrt[tv].grid;
}

/* @prepare runs whenever the state changed (not for the mouse, the time, the last click) */
static void run_prepare(void) {
    Program *p = A.prog;
    if (!p->nprepares) return;
    size_t bytes = sizeof(u32) * (size_t)p->state_words;
    if (A.prepare_snap) {
        Sym *vol[4] = {p->s_mouse, p->s_time, p->s_last_click, p->s_drag_start};
        for (int k = 0; k < 4; k++) memcpy(A.prepare_snap + vol[k]->slot, A.state + vol[k]->slot, sizeof(u32) * (size_t)vol[k]->type->size);
        if (memcmp(A.prepare_snap, A.state, bytes) == 0) return;
    } else {
        A.prepare_snap = (u32 *)xmalloc(bytes);
    }
    char err[256];
    StrBuf printed = {0};
    if (!program_prepare_ex(p, A.state, &printed, err, sizeof err)) set_status(true, "@prepare: %s", err);
    out_set(out_prepare(), sb_cstr(&printed), false);
    sb_free(&printed);
    memcpy(A.prepare_snap, A.state, bytes);
}

/* ---- rendering ---------------------------------------------------------------------- */
/* the pipelines in use: single or (deep zooms) double-single precision */
static FloodGpu *vflood(ViewerRT *rt) { return rt->df ? &rt->flood_df : &rt->flood; }
static int vpipeline(ViewerRT *rt) { return rt->df ? rt->pipeline_df : rt->pipeline; }

/* Deep zooms: does view v need double-single precision?  When a pixel is smaller than about 1e-6 of
 * the size of the numbers (with some hysteresis); single precision gets noisy from there on. */
static bool wants_df(int v) {
    ViewerRT *rt = &A.vrt[v];
    return !rt->df_failed && viewer_wants_df(A.prog, A.state, A.prog->viewers[v], rt->tw, rt->th, rt->df);
}

/* switch view v to double-single precision or back (making that version the first time) */
static void update_precision(int v) {
    ViewerRT *rt = &A.vrt[v];
    bool want = wants_df(v);
    if (want == rt->df) return;
    Program *p = A.prog;
    Section *s = p->viewers[v];
    const char *name = s->display ? s->display : s->name;
    if (want) {
        char err[512] = "";
        bool ok;
        if (rt->is_flood) {
            ok = rt->flood_df.pipe > 0 || flood_gpu_create_ex(&rt->flood_df, p, s, true, err, sizeof err);
        } else {
            if (!rt->pipeline_df) {
                u32 n;
                u32 *words = codegen_viewer_ex(p, s, true, &n, err, sizeof err);
                rt->pipeline_df = words ? gfx_viewer_create(words, n, err, sizeof err) : 0;
                free(words);
            }
            ok = rt->pipeline_df > 0;
        }
        if (!ok) {
            rt->df_failed = true;
            set_status(true, "Deep zoom: the GPU rejected the double precision version of %s: %s", name, err);
            return;
        }
        set_status(false, "Deep zoom: %s is computed in double precision from here on (slower)", name);
    } else {
        set_status(false, "%s is computed in single precision again", name);
    }
    rt->df = want;
    if (rt->is_flood) flood_gpu_free_buffers(rt->df ? &rt->flood : &rt->flood_df); /* (the other one is idle) */
    free(rt->snap); /* draw it again */
    rt->snap = NULL;
}

static bool viewer_dirty(int v) {
    ViewerRT *rt = &A.vrt[v];
    if (!rt->rendered || !rt->snap) return true;
    Program *p = A.prog;
    for (int i = 0; i < p->globals.len; i++) {
        if (!rt->used[i]) continue;
        Sym *s = p->globals.data[i];
        if (memcmp(rt->snap + s->slot, A.state + s->slot, sizeof(u32) * (size_t)s->type->size) != 0) return true;
    }
    return false;
}

#define BAND_ALIGN 64 /* rows; A.band_rows is a power of two at least this */

static bool render_viewers(void) {
    Program *p = A.prog;
    size_t bytes = sizeof(u32) * (size_t)p->state_words;
    if (!A.uploaded || memcmp(A.uploaded, A.state, bytes) != 0) {
        if (!A.uploaded) A.uploaded = (u32 *)xmalloc(bytes);
        memcpy(A.uploaded, A.state, bytes);
        gfx_set_globals(A.state, (u32)bytes);
    }
    bool busy = false;
    for (int i = 0; i < A.nvis; i++) {
        int v = A.vis[i].viewer;
        ViewerRT *rt = &A.vrt[v];
        if (!rt->target) continue;
        check_center_lo(v);
        update_precision(v);
        if (!A.uploaded || memcmp(A.uploaded, A.state, bytes) != 0) { /* (the centre's low part may have changed) */
            memcpy(A.uploaded, A.state, bytes);
            gfx_set_globals(A.state, (u32)bytes);
        }
        bool dirty = viewer_dirty(v);
        if (dirty) {
            if (!rt->snap) rt->snap = (u32 *)xmalloc(bytes);
            memcpy(rt->snap, A.state, bytes);
        }
        if (rt->is_flood) {
            /* flood fill: restart from the start pixel whenever an input changes */
            if (dirty) {
                int sx, sy;
                flood_start_pixel(p, A.state, p->viewers[v], rt->tw, rt->th, &sx, &sy);
                if (!flood_gpu_start(vflood(rt), rt->tw, rt->th, sx, sy))
                    set_status(true, "Not enough GPU memory for the view '%s'", p->viewers[v]->name);
                rt->rendered = true;
                rt->flood_checked = false;
            } else if (flood_gpu_done(vflood(rt)) && !rt->flood_checked && vflood(rt)->field) {
                /* finished (last frame): did the solution make it from its starting point to the start pixel? */
                rt->flood_checked = true;
                FloodEntry e;
                char err[128];
                FloodGpu *fg = vflood(rt);
                u64 at = ((u64)fg->plan.sy * (u64)fg->w + (u64)fg->plan.sx) * sizeof(FloodEntry);
                if (gfx_buffer_download(fg->field, at, &e, sizeof e, err, sizeof err) && e.status == FLOOD_FAILED)
                    set_status(true, "%s: Newton's method fails at the starting point ('continuing from') or on the way from "
                                     "it into this view; try a starting point inside the view",
                               p->viewers[v]->display ? p->viewers[v]->display : p->viewers[v]->name);
            }
            if (!flood_gpu_done(vflood(rt))) {
                flood_gpu_record(vflood(rt), A.flood_passes, rt->target);
                if (!flood_gpu_done(vflood(rt))) busy = true;
            }
            continue;
        }
        if (dirty) {
            if (!rt->rendered || rt->rows_left == 0) rt->next_row = 0;
            rt->rows_left = rt->th; /* keep going round from where we are, so every row gets refreshed
                                       even while the state changes every frame (panning) */
            rt->rendered = true;
        }
        if (rt->rows_left <= 0) continue;
        if (rt->next_row >= rt->th) rt->next_row = 0;
        /* bands of whole multiples of BAND_ALIGN rows, from a multiple of it: a band's edges never cut through the GPU's
           tiles or compression blocks (Apple GPUs, MoltenVK: garbage blocks next to a partly covered tile while zooming) */
        int rows = FS_MIN(A.band_rows, rt->th - rt->next_row);
        GfxViewerParams vp = {0};
        vp.width = (float)rt->tw;
        vp.height = (float)rt->th;
        vp.x0 = 0;
        vp.x1 = rt->tw;
        vp.y0 = rt->next_row;
        vp.y1 = rt->next_row + rows;
        gfx_frame_render_viewer(rt->target, vpipeline(rt), &vp);
        rt->next_row = vp.y1 >= rt->th ? 0 : vp.y1;
        rt->rows_left -= rows;
        if (rt->rows_left > 0) busy = true;
    }
    return busy;
}

static bool viewers_busy(void) {
    for (int i = 0; A.prog && i < A.nvis; i++) {
        ViewerRT *rt = &A.vrt[A.vis[i].viewer];
        if (rt->animated || rt->rows_left > 0 || (rt->is_flood && !flood_gpu_done(vflood(rt)))) return true;
    }
    return false;
}

/* ---- main loop ----------------------------------------------------------------------- */
/* the bars at the edges, the open panes next to them, the views in between */
static void layout_panes(float W, float H) {
    float bar = bar_width();
    float ew = A.editor_open ? floorf(W * A.editor_frac) : 0;
    float cw = A.config_open || A.info_open ? FS_MAX(floorf(W * A.config_frac), px(300)) : 0;
    /* the views keep at least a third of the window */
    float room = W * 2 / 3 - 2 * bar;
    if (ew + cw > room) {
        float k = FS_MAX(room, 0.0f) / (ew + cw);
        ew = floorf(ew * k);
        cw = floorf(cw * k);
    }
    A.lbar_r = (Rect){0, 0, bar, H};
    A.editor_r = (Rect){bar, 0, ew, H};
    A.config_r = (Rect){W - bar - cw, 0, cw, H};
    A.rbar_r = (Rect){W - bar, 0, bar, H};
    A.free = (Rect){bar + ew, 0, FS_MAX(W - 2 * bar - ew - cw, 1.0f), H};
    A.area = A.free;
}

/* the tiles on screen (A.vis): the tree's, then the floating views; each with the part of it no pane covers */
static void layout_tiles(float W, float H) {
    layout_panes(W, H);
    A.nvis = 0;
    layout_compute(A.root, A.area);
    for (int c = 0; c < MAX_FLOATS; c++)
        if (A.floats[c] >= 0 && A.nvis < MAX_TILES + MAX_FLOATS && !(A.vrt && A.vrt[A.floats[c]].gone)) {
            Rect r = float_rect(c);
            VisTile t = {A.floats[c], r, -1, c, r};
            A.vis[A.nvis++] = t;
        }
    for (int i = 0; i < A.nvis; i++)
        if (A.vis[i].fl < 0) A.vis[i].vr = rect_and(A.vis[i].r, A.free);
}

/* nothing on screen because the script has no view: a word about it, in the middle of the free part of the window */
static void empty_hint(void) {
    if (A.nvis > 0 || !A.prog || A.prog->nviewers > 0) return;
    Rect r = A.free;
    float w = FS_MIN(r.w - px(40), px(420));
    if (w < px(160)) return;
    const char *text = A.editor_open ? "Code without sections is one view of the plane, z the point of each pixel; "
                                       "more views start with @viewer NAME z."
                                     : "Code without sections is one view of the plane, z the point of each pixel; "
                                       "more views start with @viewer NAME z. " MODKEY "+E shows the script.";
    float lh = ui_font_line_height(UI_FONT_SANS), th = ui_text_wrapped(UI_FONT_SANS, 0, 0, w, text, 0, false);
    float x = r.x + (r.w - w) / 2, y = r.y + (r.h - lh - px(8) - th) / 2;
    ui_text(UI_FONT_SANS, x, y, "No views", -1, UI_COL_TEXT);
    ui_text_wrapped(UI_FONT_SANS, x, y + lh + px(8), w, text, UI_COL_TEXT_DIM, true);
}

static void frame_ui(float W, float H);

static void frame(void) {
    UiInput *in = &A.in;
    float W = (float)in->fb_w, H = (float)in->fb_h;
    process_requests();
    Program *p = A.prog;
    if (!p) return;
    in->content_scale *= A.ui_zoom > 0 ? A.ui_zoom : 1.0f; /* (the UI's size; the pictures keep their pixels) */
    ui_begin_frame(in);
    int aw, ah;
    const u8 *atlas = ui_take_atlas_update(&aw, &ah);
    if (atlas) gfx_set_font_atlas(atlas, aw, ah);
    if (in->ndropped) request_open(in->dropped_files[0]); /* (the first file dropped) */

    /* layout */
    layout_tiles(W, H);
    for (int i = 0; i < A.nvis; i++) ensure_target(A.vis[i].viewer, (int)A.vis[i].r.w, (int)A.vis[i].r.h);

    /* time */
    float t = (float)(fs_time_seconds() - A.t0);
    A.state[p->s_time->slot] = fw(t);

    frame_ui(W, H);
    const GfxDrawList *dl = ui_end_frame();
    if (ui_wants_mouse() && A.drag == DRAG_NONE) platform_set_cursor(ui_cursor());

    /* live re-run after a pause in typing */
    double now = fs_time_seconds();
    if (editor_version(A.ed) != A.compiled_version) {
        if (A.edit_time == 0) A.edit_time = now;
        if (editor_version(A.ed) != A.compiled_version && A.live && now - A.edit_time > 0.6) {
            A.req_run = true; /* next frame */
            A.edit_time = 0;
        }
        update_title();
    } else {
        A.edit_time = 0;
    }
    /* external changes to the file */
    if (A.path[0] && now > A.next_file_check) {
        A.next_file_check = now + 1.0;
        long long mt = file_mtime(A.path);
        if (mt && mt != A.file_mtime && !editor_is_modified(A.ed)) {
            request_open(A.path);
            A.req_reload = true;
        }
    }

    run_prepare();
    double t0 = fs_time_seconds();
    bool busy = false;
    if (gfx_frame_begin()) {
        busy = render_viewers();
        static const float bg[3] = {0.063f, 0.071f, 0.086f};
        gfx_frame_draw(dl, bg);
        gfx_frame_end();
    }
    double dt = fs_time_seconds() - t0;
    if (busy || dt > 0.06) {
        if (dt > 0.06 && A.band_rows > BAND_ALIGN) A.band_rows /= 2;
        else if (dt < 0.025 && A.band_rows < 8192) A.band_rows *= 2;
        if (dt > 0.06 && A.flood_passes > 4) A.flood_passes /= 2;
        else if (dt < 0.025 && A.flood_passes < 4096) A.flood_passes *= 2;
    }
}

/* The UI of a frame (after layout_tiles; nothing for the GPU but the draw list): the views' mouse, the tiles, the
   panes, the bars, the overlays and the keys */
static void frame_ui(float W, float H) {
    UiInput *in = &A.in;
    Program *p = A.prog;
    A.hover_tile = -1;
    for (int i = A.nvis - 1; i >= 0; i--)
        if (in_rect(A.vis[i].r, in->mouse_x, in->mouse_y)) {
            A.hover_tile = i;
            break;
        }
    viewer_interaction();
    tools_show();
    for (int i = 0; i < A.nvis; i++) {
        int prev = ui_layer(tile_layer(&A.vis[i]));
        draw_tile(&A.vis[i]);
        if (A.vis[i].fl >= 0) ui_occlude(A.vis[i].r.x, A.vis[i].r.y, A.vis[i].r.w, A.vis[i].r.h);
        ui_layer(prev);
    }
    empty_hint();
    for (int i = 0; i < A.nvis; i++) deep_note(&A.vis[i]);
    /* drop zones while dragging a view */
    if (A.drag == DRAG_VIEWER) {
        int prev = ui_layer(UI_LAYER_TOOLTIP);
        int leaf;
        Rect hl;
        if (drop_zone(in->mouse_x, in->mouse_y, &leaf, &hl) != DROP_NONE) {
            ui_rect(hl.x, hl.y, hl.w, hl.h, UI_RGBA(90, 160, 240, 70));
            ui_rect_outline(hl.x, hl.y, hl.w, hl.h, px(2), UI_COL_ACCENT);
        }
        Section *s = p->viewers[A.drag_viewer];
        const char *name = s->display ? s->display : s->name;
        float tw = ui_text_width(UI_FONT_SANS, name, -1) + px(20);
        ui_rrect(in->mouse_x + px(12), in->mouse_y + px(12), tw, px(28), px(6), UI_COL_ACCENT_DIM);
        ui_text(UI_FONT_SANS, in->mouse_x + px(22), in->mouse_y + px(16), name, -1, UI_COL_TEXT);
        ui_layer(prev);
    }
    editor_pane(H);
    right_pane(W, H);
    bars();
    help_overlay(W, H);
    refdoc_ui(W, H, &A.ref_open);
    modal_dialog(W, H);
    status_line(W, H);
    if (!A.modal) shortcuts();
}

/* nothing could be loaded yet: only the editor (full width) */
static void frame_editor_only(void) {
    UiInput *in = &A.in;
    process_requests();
    if (A.prog) return; /* it compiled: the next frame shows the views */
    in->content_scale *= A.ui_zoom > 0 ? A.ui_zoom : 1.0f;
    ui_begin_frame(in);
    int aw, ah;
    const u8 *atlas = ui_take_atlas_update(&aw, &ah);
    if (atlas) gfx_set_font_atlas(atlas, aw, ah);
    A.editor_open = true;
    A.editor_r = (Rect){0, 0, (float)in->fb_w, (float)in->fb_h};
    A.area = A.free = (Rect){(float)in->fb_w, 0, 0, (float)in->fb_h};
    editor_pane((float)in->fb_h);
    status_line((float)in->fb_w, (float)in->fb_h);
    modal_dialog((float)in->fb_w, (float)in->fb_h);
    if (ui_shortcut(UI_KEY_ENTER, UI_MOD_MOD) || ui_shortcut(UI_KEY_R, UI_MOD_MOD) ||
        (editor_version(A.ed) != A.compiled_version && A.live)) {
        int l;
        const char *t2 = editor_get_text(A.ed, &l);
        load_source(t2, LOAD_OPEN);
    }
    const GfxDrawList *dl = ui_end_frame();
    if (gfx_frame_begin()) {
        static const float bg[3] = {0.063f, 0.071f, 0.086f};
        gfx_frame_draw(dl, bg);
        gfx_frame_end();
    }
}

/* An error before the window opens, on stderr.  On macOS also in a dialog when the app was opened from the
   Finder (its parent is launchd): no terminal shows stderr then, and the app would just vanish. */
static void startup_error(const char *fmt, ...) FS_PRINTF(1, 2);
static void startup_error(const char *fmt, ...) {
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s\n", msg);
#if FS_OS_MACOS
    if (isatty(2) || getppid() != 1) return;
    char text[2500], script[2700]; /* the message in an AppleScript string: \ and " escaped, lines as \n */
    int k = 0;
    for (const char *p = msg; *p && k < (int)sizeof text - 3; p++) {
        if (*p == '"' || *p == '\\' || *p == '\n') text[k++] = '\\';
        text[k++] = *p == '\n' ? 'n' : *p;
    }
    text[k] = 0;
    snprintf(script, sizeof script, "display alert \"FractalStream could not start\" message \"%s\" as critical", text);
    char *args[] = {"/usr/bin/osascript", "-e", script, NULL};
    pid_t pid;
    if (posix_spawn(&pid, args[0], NULL, NULL, args, environ) == 0) waitpid(pid, NULL, 0);
#endif
}

int app_run(const char *script_path) {
    memset(&A, 0, sizeof A);
    A.t0 = fs_time_seconds();
    A.band_rows = 8192;
    A.flood_passes = 64;
    A.editor_frac = 0.45f;
    A.config_frac = 0.27f;
    A.info_frac = 0.4f;
    A.live = true;
    A.hover_viewer = -1;
    A.hover_tile = -1;
    char err[512];
    /* read the script first: it may set the window size */
    char *src = NULL;
    size_t len = 0;
    if (script_path) {
        src = fs_read_file(script_path, &len);
        if (!src) {
            fprintf(stderr, "error: can't read %s\n", script_path);
            return 2;
        }
        snprintf(A.path, sizeof A.path, "%s", script_path);
    }
    int ww = 1280, wh = 800;
    {
        Program *probe = program_load(src ? src : welcome_script, script_path ? script_path : "untitled");
        if (!probe->nerrors) {
            ww = (i32)probe->initial_state[probe->s_width->slot];
            wh = (i32)probe->initial_state[probe->s_height->slot];
        } else {
            StrBuf sb = {0};
            program_format_diags(probe, &sb);
            fprintf(stderr, "%s", sb_cstr(&sb));
            sb_free(&sb);
        }
        program_free(probe);
    }
    if (!script_path) ww = FS_MAX(ww, 1280);
    if (!gfx_preinit_loader()) {
        const char *why = gfx_library_error();
        startup_error("no Vulkan library found. Install your GPU driver (or MoltenVK on macOS).%s%s", why[0] ? "\n" : "", why);
        free(src);
        return 1;
    }
    if (!platform_init(FS_CLAMP(ww, 320, 8192), FS_CLAMP(wh, 240, 8192), "FractalStream", err, sizeof err)) {
        startup_error("%s", err);
        free(src);
        return 1;
    }
    GfxInitDesc gd = {0};
    gd.glfw_window = platform_glfw_window();
    gd.vsync = true;
    gd.validation = getenv("FS_VALIDATION") != NULL;
    if (!gfx_init(&gd, err, sizeof err)) {
        platform_shutdown();
        startup_error("%s", err);
        free(src);
        return 1;
    }
    bool fast_df = app_init_exact_floats();
    if (getenv("FS_DEBUG")) fprintf(stderr, "deep zooms: %s\n", fast_df ? "IEEE floats per shader" : "each operation marked exact");
    A.ed = editor_create();
    {
        /* highlight the built-in function names */
        const char *names[256];
        int n = 0;
        for (int k = 0; k < ARRAY_COUNT(intrinsics) && n < 256; k++)
            if (intrinsics[k].name[0] != '_') names[n++] = intrinsics[k].name;
        static const char *pre[] = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "asinh",
                                    "acosh", "atanh", "exp", "log", "ln", "log2", "log10", "sqrt", "abs", "sign", "floor",
                                    "ceil", "round", "trunc", "fract", "min", "max", "clamp", "mix", "smoothstep", "step",
                                    "arg", "conj", "norm", "cis", "polar", "rgb", "gray", "grey", "hsv"};
        for (int k = 0; k < ARRAY_COUNT(pre) && n < 256; k++) names[n++] = pre[k];
        editor_set_builtins(A.ed, names, n);
        update_completions(NULL); /* (the script's own names come when it compiles) */
        editor_set_hover(A.ed, hover_info, NULL);
    }
    const char *text = src ? src : welcome_script;
    editor_set_text(A.ed, text, (int)strlen(text));
    editor_set_unmodified(A.ed);
    A.file_mtime = file_mtime(A.path);
    A.editor_open = script_path == NULL;
    load_recent();
    load_settings();
    if (script_path) note_recent(script_path);
    layout_reset(0);
    if (!load_source(text, LOAD_OPEN)) A.editor_open = true; /* nothing to show: keep the editor up so the errors can be fixed */
    free(src);
    UiInput *in = &A.in;
    platform_poll(in, false);
    /* FS_AUTOQUIT=seconds closes the window after that long (for automated runs under sanitizers) */
    double autoquit = getenv("FS_AUTOQUIT") ? atof(getenv("FS_AUTOQUIT")) : 0;
    while (!(autoquit > 0 && fs_time_seconds() - A.t0 > autoquit)) {
        if (platform_should_close()) { /* (unsaved changes: asked about first) */
            if (A.quit_now || unsaved_guard(PENDING_QUIT, NULL)) break;
            platform_set_should_close(false);
        }
        if (A.prog) frame();
        else frame_editor_only();
        double wait = viewers_busy() || A.drag != DRAG_NONE ? 0.0 : FS_MIN(ui_redraw_in(), 0.5);
        if (A.status[0] && fs_time_seconds() < A.status_until) wait = FS_MIN(wait, 0.1);
        platform_poll_timeout(in, wait);
    }
    free_viewers();
    if (A.prog) interp_free(&A.interp);
    program_free(A.prog);
    free(A.state);
    free(A.uploaded);
    free(A.prepare_snap);
    editor_destroy(A.ed);
    gfx_shutdown();
    platform_shutdown();
    return 0;
}
