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

enum { TOOL_PAN = -2, TOOL_PICK = -1 };
enum { N_FREE, N_LEAF, N_HSPLIT, N_VSPLIT }; /* HSPLIT: a | b, VSPLIT: a over b */
enum { DRAG_NONE, DRAG_PAN, DRAG_TOOL, DRAG_PICK, DRAG_CLICK, DRAG_SPLIT, DRAG_VIEWER };
enum { DROP_NONE, DROP_REPLACE, DROP_LEFT, DROP_RIGHT, DROP_TOP, DROP_BOTTOM, DROP_FLOAT };

typedef struct Rect { float x, y, w, h; } Rect;
typedef struct LNode { int kind; int a, b; float ratio; int viewer; Rect r; } LNode;
typedef struct VisTile { int viewer; Rect r; int node; int fl; } VisTile;

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
    double strip_until; /* show the tool strip until then (after a change by keyboard) */
    u32 *show_snap;    /* the state when the selected tool last ran `on show` (NULL: run it) */
    bool flood_checked; /* flood viewers: the start pixel's result was looked at */
    ViewHistory *hist;  /* Back / Forward (made on the first move; kept when the script runs again) */
    bool grid;          /* show the axes and a grid with their values (flat views) */
} ViewerRT;

typedef struct App {
    Program *prog;
    u32 *state, *uploaded;
    Interp interp;
    ViewerRT *vrt;
    DrawItemVec *draws; /* per user tool */
    LNode nodes[MAX_NODES];
    int root;
    int floats[MAX_FLOATS]; /* viewer shown in each corner (-1 none): TL TR BL BR */
    int focus_viewer;
    VisTile vis[MAX_TILES + MAX_FLOATS];
    int nvis;
    Rect area;              /* viewer area, between the panes */
    int band_rows;
    int flood_passes;       /* flood-fill passes per frame (adapts to the GPU) */
    /* panes */
    bool editor_open, config_open;
    float editor_frac, config_frac;
    Rect editor_r, config_r; /* the panes (open or collapsed to a bar) */
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
    bool req_run, req_reload;
    char req_open[1024];
    char req_export[1024];  /* save a picture there */
    int export_viewer, export_scale, export_wait;
    /* overlays */
    int menu_viewer;        /* viewer whose menu is open, -1 none */
    float menu_scroll;      /* (the menu scrolls in a small window) */
    bool help_open;
    /* find / replace in the script */
    bool find_open, replace_open, find_focus, find_case, editor_grab;
    char find_buf[256], replace_buf[256];
    float config_scroll;
    const char *choice_open;  /* config choice whose list is open (points to choice_name), NULL none */
    char choice_name[128];
    char list_buf[1024];    /* the list being edited in the config pane */
    bool list_editing;
    /* the formula being edited in the config pane: its text, and where it is in the script */
    char formula_buf[512];
    int formula_start, formula_end;
    int modal;              /* 0, 1 save as, 2 open, 3 save a picture */
    bool modal_focus, modal_select_all;
    bool modal_confirm;     /* saving over a file: the next OK replaces it */
    char modal_buf[1024];
    char browse_dir[1024];  /* the folder listed in the dialog, and its entries */
    FsDirEntry *browse;
    int nbrowse;
    float browse_scroll;
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
    "# This is a script: edit it and the pictures follow as soon as there are no errors.\n"
    "# Ctrl+Enter runs it, Ctrl+S saves it, F1 shows all the keys (Cmd instead of Ctrl on a Mac).\n"
    "# Move the mouse to the left edge of a picture for its tools; the menu button at the top of\n"
    "# that strip switches and splits views. Try \"Pick point\" in the parameter plane: the Julia\n"
    "# set follows the point. The language is described in docs/language.md.\n"
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

/* ---- small helpers ----------------------------------------------------------- */
static float px(float v) { return ui_px(v); }
static bool in_rect(Rect r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

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

static void layout_reset(int viewer) {
    memset(A.nodes, 0, sizeof A.nodes);
    for (int i = 0; i < MAX_FLOATS; i++) A.floats[i] = -1;
    A.root = 0;
    A.nodes[0].kind = N_LEAF;
    A.nodes[0].viewer = viewer;
    A.focus_viewer = viewer;
}

static int count_leaves(int n) {
    if (A.nodes[n].kind == N_LEAF) return 1;
    return count_leaves(A.nodes[n].a) + count_leaves(A.nodes[n].b);
}

static int find_leaf(int n, int viewer) {
    if (A.nodes[n].kind == N_LEAF) return A.nodes[n].viewer == viewer ? n : -1;
    int r = find_leaf(A.nodes[n].a, viewer);
    return r >= 0 ? r : find_leaf(A.nodes[n].b, viewer);
}

static int find_parent(int n, int child) {
    if (A.nodes[n].kind == N_LEAF) return -1;
    if (A.nodes[n].a == child || A.nodes[n].b == child) return n;
    int r = find_parent(A.nodes[n].a, child);
    return r >= 0 ? r : find_parent(A.nodes[n].b, child);
}

static int find_float(int viewer) {
    for (int i = 0; i < MAX_FLOATS; i++)
        if (A.floats[i] == viewer) return i;
    return -1;
}

static bool viewer_visible(int v) { return find_leaf(A.root, v) >= 0 || find_float(v) >= 0; }

static void remove_leaf(int n) {
    int p = find_parent(A.root, n);
    if (p < 0) return; /* the last tile stays */
    int sib = A.nodes[p].a == n ? A.nodes[p].b : A.nodes[p].a;
    A.nodes[p] = A.nodes[sib];
    A.nodes[sib].kind = N_FREE;
    A.nodes[n].kind = N_FREE;
}

/* take viewer v out of wherever it is shown (unless it is the last tile) */
static void detach_viewer(int v) {
    int f = find_float(v);
    if (f >= 0) A.floats[f] = -1;
    int n = find_leaf(A.root, v);
    if (n >= 0 && count_leaves(A.root) > 1) remove_leaf(n);
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

static void split_leaf(int n, int side, int v) {
    if (count_leaves(A.root) >= MAX_TILES) {
        set_status(true, "Too many views on screen");
        return;
    }
    if (A.nodes[n].viewer == v) return;
    int f = find_float(v);
    if (f >= 0) A.floats[f] = -1;
    int other = find_leaf(A.root, v);
    if (other >= 0) {
        remove_leaf(other);
        /* the tree changed: find the target again */
        n = find_leaf(A.root, A.nodes[n].kind == N_LEAF ? A.nodes[n].viewer : -1);
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
    s->ratio = 0.5f;
    bool new_first = side == DROP_LEFT || side == DROP_TOP;
    s->a = new_first ? b : a;
    s->b = new_first ? a : b;
    A.focus_viewer = v;
}

static Rect float_rect(int corner) {
    Rect ar = A.area;
    float w = FS_MAX(ar.w * 0.3f, px(160)), h = FS_MAX(ar.h * 0.3f, px(120));
    float m = px(12);
    Rect r = {corner & 1 ? ar.x + ar.w - w - m : ar.x + m, corner & 2 ? ar.y + ar.h - h - m : ar.y + m, w, h};
    return r;
}

static void layout_compute(int n, Rect r) {
    LNode *nd = &A.nodes[n];
    nd->r = r;
    if (nd->kind == N_LEAF) {
        if (A.nvis < MAX_TILES) {
            VisTile t = {nd->viewer, r, n, -1};
            A.vis[A.nvis++] = t;
        }
        return;
    }
    float g = px(1);
    if (nd->kind == N_HSPLIT) {
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
    LNode *nd = &A.nodes[n];
    if (nd->kind == N_LEAF) return -1;
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
    return tool == TOOL_PAN ? "Pan" : tool == TOOL_PICK ? "Pick point" : A.prog->tools[tool]->display;
}

static const char *tool_icon(int tool) {
    return tool == TOOL_PAN ? "pan_tool" : tool == TOOL_PICK ? "my_location" : A.prog->tools[tool]->icon;
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

static void select_tool(int v, int tool, bool flash) {
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
    if (flash) rt->strip_until = fs_time_seconds() + 1.5;
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

/* compile `src`; on success replace the running program.  LOAD_RUN (the same script, edited or changed on disk) keeps
   the layout, the views and their history, the tools, and the values changed since the last run (unless the script
   now gives them another value); LOAD_OPEN (another script) starts afresh. */
enum { LOAD_OPEN, LOAD_RUN };
static bool load_source(const char *src, int mode) {
    Program *p = program_load(src, A.path[0] ? A.path : "untitled");
    diags_to_editor(p);
    A.compiled_version = editor_version(A.ed);
    A.script_errors = p->nerrors;
    if (p->nerrors) {
        set_status(true, "%d error%s in the script%s", p->nerrors, p->nerrors == 1 ? "" : "s",
                   A.editor_open ? "" : " (Ctrl+E shows it)");
        program_free(p);
        return false;
    }
    if (p->nviewers == 0) {
        set_status(true, "The script has no @viewer section");
        program_free(p);
        return false;
    }
    ViewerRT *vrt = (ViewerRT *)xcalloc((size_t)p->nviewers, sizeof(ViewerRT));
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
        vrt[v].used = (bool *)xcalloc((size_t)p->globals.len + 1, sizeof(bool));
        if (!ok) {
            set_status(true, "The GPU rejected the viewer '%s': %s", s->name, err);
            destroy_rts(vrt, v + 1);
            program_free(p);
            return false;
        }
        codegen_viewer_deps(p, s, vrt[v].used);
        for (int i = 0; i < p->globals.len; i++) /* (read by the deep zoom version) */
            if (p->globals.data[i] == s->center_lo) vrt[v].used[i] = true;
        for (int i = 0; i < p->globals.len; i++)
            if (vrt[v].used[i] && p->globals.data[i] == p->s_time) vrt[v].animated = true;
        vrt[v].tool = TOOL_PAN;
    }
    /* new state; keep the views, picked points and tools of viewers that still exist */
    bool same = mode == LOAD_RUN && A.prog;
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    int *remap = (int *)xmalloc(sizeof(int) * (size_t)(A.prog ? A.prog->nviewers : 1));
    for (int ov = 0; A.prog && ov < A.prog->nviewers; ov++) remap[ov] = -1;
    for (int ov = 0; same && ov < A.prog->nviewers; ov++) {
        remap[ov] = -1;
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
    /* keep the layout if all the viewers in it still exist */
    bool keep = same;
    for (int i = 0; keep && i < MAX_NODES; i++)
        if (A.nodes[i].kind == N_LEAF && remap[A.nodes[i].viewer] < 0) keep = false;
    for (int i = 0; keep && i < MAX_FLOATS; i++)
        if (A.floats[i] >= 0 && remap[A.floats[i]] < 0) keep = false;
    if (keep) {
        for (int i = 0; i < MAX_NODES; i++)
            if (A.nodes[i].kind == N_LEAF) A.nodes[i].viewer = remap[A.nodes[i].viewer];
        for (int i = 0; i < MAX_FLOATS; i++)
            if (A.floats[i] >= 0) A.floats[i] = remap[A.floats[i]];
        A.focus_viewer = A.focus_viewer >= 0 && A.focus_viewer < A.prog->nviewers ? remap[A.focus_viewer] : 0;
        if (A.focus_viewer < 0) A.focus_viewer = 0;
    }
    free(remap);
    free_viewers();
    if (A.prog) interp_free(&A.interp);
    program_free(A.prog);
    free(A.state);
    free(A.uploaded);
    A.prog = p;
    A.state = state;
    A.uploaded = NULL;
    A.vrt = vrt;
    A.draws = (DrawItemVec *)xcalloc((size_t)p->ntools + 1, sizeof(DrawItemVec));
    A.menu_viewer = -1;
    A.choice_open = NULL;
    free(A.prepare_snap);
    A.prepare_snap = NULL;
    interp_init(&A.interp, p, A.state);
    A.interp.step_limit = 3e7;
    if (!keep) {
        layout_reset(0);
        if (p->nviewers >= 2) { /* the first two views side by side */
            split_leaf(A.root, DROP_RIGHT, 1);
            A.focus_viewer = 0;
        }
    }
    for (int v = 0; v < p->nviewers; v++) {
        Sym *ps = p->viewers[v]->picked;
        if (ps && (state[ps->slot] || state[ps->slot + 1])) vrt[v].picked = true;
    }
    update_title();
    return true;
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
    if (!ok) A.editor_open = true;
    else set_status(false, "Opened %s", fs_basename(path));
    return ok;
}

static void save_file(const char *path) {
    int len;
    const char *text = editor_get_text(A.ed, &len);
    if (!fs_write_file(path, text, (size_t)len)) {
        set_status(true, "Can't write %s", path);
        return;
    }
    snprintf(A.path, sizeof A.path, "%s", path);
    editor_set_unmodified(A.ed);
    A.file_mtime = file_mtime(A.path);
    set_status(false, "Saved %s", fs_basename(path));
    update_title();
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
        if (open_file(path, reload) && reload) set_status(false, "Reloaded %s (changed on disk)", fs_basename(path));
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

static void default_save_path(char *out, int cap) {
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = ".";
    char docs[900];
    snprintf(docs, sizeof docs, "%s/Documents", home);
    struct stat st;
    snprintf(out, (size_t)cap, "%s/untitled.fstream", stat(docs, &st) == 0 ? docs : home);
}

static void open_modal(int kind, const char *path);
static const char *examples_dir(void);
static const char *home_dir(void);

static void ask_save_as(void) {
    char path[1024];
    if (A.path[0]) snprintf(path, sizeof path, "%s", A.path);
    else default_save_path(path, sizeof path);
    open_modal(1, path);
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
    if (A.path[0]) {
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
    A.interp.draws = &out;
    A.interp.erased = A.interp.erased_all = false;
    A.interp.erase_tools = 0;
    bool ok = t->has_events ? interp_run_tool_event(&A.interp, t, ev, re, im) : interp_run_tool(&A.interp, t, re, im);
    if (!ok) set_status(true, "%s: %s", t->display, A.interp.error);
    A.interp.draws = NULL;
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
static int drop_zone(float x, float y, int *leaf, Rect *hl) {
    Rect ar = A.area;
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
        if (t->fl >= 0 || !in_rect(t->r, x, y)) continue;
        Rect r = t->r;
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
        if (find_leaf(A.root, viewer) >= 0 && count_leaves(A.root) == 1) {
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
    for (double k = ceil(xl / step); k * step <= xr; k++) { /* vertical lines, values along the bottom */
        float X = (float)(r.x + 0.5 * r.w + (k * step - cx) / sc);
        ui_rect(X - px(0.5f), r.y, FS_MAX(px(1), 1.0f), r.h, k == 0 ? axis : line);
        grid_label(label, sizeof label, k * step, dec, false);
        float ty = r.y + r.h - lh - px(3);
        ui_text(UI_FONT_MONO, X + px(4), ty + px(1), label, -1, shadow);
        ui_text(UI_FONT_MONO, X + px(3), ty, label, -1, ink);
    }
    for (double k = ceil(yb / step); k * step <= yt; k++) { /* horizontal lines, values along the left */
        float Y = (float)(r.y + 0.5 * r.h - (k * step - cy) / sc);
        ui_rect(r.x, Y - px(0.5f), r.w, FS_MAX(px(1), 1.0f), k == 0 ? axis : line);
        grid_label(label, sizeof label, k * step, dec, true);
        float tx = r.x + px(40); /* (right of the tool strip's badge) */
        ui_text(UI_FONT_MONO, tx + px(1), Y - lh - px(1) + px(1), label, -1, shadow);
        ui_text(UI_FONT_MONO, tx, Y - lh - px(1), label, -1, ink);
    }
}

/* the picture, the drawings, the picked point and the name */
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
    bool several = A.nvis > 1;
    if (several || t->fl >= 0) {
        const char *name = s->display ? s->display : s->name;
        float tw = ui_text_width(UI_FONT_SANS, name, -1) + px(12), th = ui_font_line_height(UI_FONT_SANS) + px(4);
        ui_rrect(r.x + r.w - tw - px(6), r.y + r.h - th - px(6), tw, th, px(4), UI_RGBA(0, 0, 0, 120));
        ui_text(UI_FONT_SANS, r.x + r.w - tw, r.y + r.h - th - px(4), name, -1, UI_RGBA(255, 255, 255, 200));
    }
    /* where the mouse is, in the plane (as many decimals as the pixels resolve) */
    if (A.hover_tile >= 0 && &A.vis[A.hover_tile] == t && A.in.mouse_in_window && A.menu_viewer < 0 && A.drag != DRAG_VIEWER &&
        in_rect(r, A.in.mouse_x, A.in.mouse_y)) {
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
        float bx = r.x + px(6), by = r.y + r.h - th - px(6);
        ui_rrect(bx, by, tw, th, px(4), UI_RGBA(0, 0, 0, 120));
        ui_text(UI_FONT_MONO, bx + px(6), by + px(2), co, -1, UI_RGBA(255, 255, 255, 210));
    }
    if (t->fl >= 0) ui_rect_outline(r.x, r.y, r.w, r.h, px(2), A.hover_viewer == t->viewer ? UI_COL_ACCENT : UI_COL_BORDER);
    else if (several && A.focus_viewer == t->viewer) ui_rect_outline(r.x, r.y, r.w, r.h, px(1), UI_COL_ACCENT_DIM);
    ui_pop_clip();
}

/* The tool strip at the left edge of a tile (menu, Pan, Pick point, the viewer's tools), shown when the
 * mouse comes near that edge; otherwise a small badge shows the selected tool.  Also the close button. */
static float strip_button(void) { return px(34); }

static void tile_chrome(int i) {
    VisTile *t = &A.vis[i];
    Program *p = A.prog;
    int v = t->viewer;
    ViewerRT *rt = &A.vrt[v];
    Rect r = t->r;
    float mx = A.in.mouse_x, my = A.in.mouse_y;
    float b = strip_button(), pad = px(4), gap = px(2);
    bool mouse_here = A.hover_tile == i && A.in.mouse_in_window;
    bool dragging = A.drag != DRAG_NONE;
    int prev = ui_layer(UI_LAYER_PANEL);
    ui_push_clip(r.x, r.y, r.w, r.h);
    /* which buttons */
    int tools[66], n = 0;
    tools[n++] = TOOL_PAN;
    if (p->viewers[v]->picked) tools[n++] = TOOL_PICK;
    for (int k = 0; k < p->ntools && n < 66; k++)
        if (tool_in_viewer(k, v)) tools[n++] = k;
    float sx = r.x + px(6), sy = r.y + px(6);
    float sh = pad * 2 + b * (n + 1) + gap * n + px(6);
    bool is_near = mouse_here && mx < r.x + b + px(40) && my < sy + sh + px(20);
    bool show = !dragging && (is_near || fs_time_seconds() < rt->strip_until || A.menu_viewer == v);
    if (show) {
        ui_panel_ex(sx, sy, b + 2 * pad, FS_MIN(sh, r.h - px(12)), UI_RGBA(20, 22, 28, 215), px(8));
        float y = sy + pad;
        char id[64], letter[8], tip[160], sc[48];
        snprintf(id, sizeof id, "##strip_menu%d", v);
        if (ui_icon_button_ex(id, "menu", "=", sx + pad, y, b, A.menu_viewer == v ? UI_SELECTED : UI_FLAT))
            A.menu_viewer = A.menu_viewer == v ? -1 : v;
        ui_tooltip("Views, tools and pictures (Esc)");
        y += b + px(6);
        ui_rect(sx + pad + px(4), y - px(4), b - px(8), px(1), UI_COL_BORDER);
        for (int k = 0; k < n; k++) {
            if (y + b > r.y + r.h - px(6)) break; /* the menu lists the rest */
            int tool = tools[k];
            tool_letter(tool, letter);
            snprintf(id, sizeof id, "##strip_tool%d_%d", v, tool);
            if (ui_icon_button_ex(id, tool_icon(tool), letter, sx + pad, y, b, rt->tool == tool ? UI_SELECTED : UI_FLAT))
                select_tool(v, tool, false);
            shortcut_text(tool_shortcut(tool), sc, sizeof sc);
            snprintf(tip, sizeof tip, "%s%s%s%s%s", tool_name(tool), sc[0] ? " (" : "", sc, sc[0] ? ")" : "",
                     tool >= 0 && p->tools[tool]->button ? ": runs once" : "");
            ui_tooltip(tip);
            y += b + gap;
        }
    } else if (!dragging) {
        /* badge: the selected tool */
        float bs = px(26);
        char letter[8];
        tool_letter(rt->tool, letter);
        ui_rrect(sx, sy, bs, bs, px(6), UI_RGBA(20, 22, 28, 150));
        ui_icon(tool_icon(rt->tool), letter, sx, sy, bs, UI_RGBA(230, 235, 245, 200));
    }
    /* close button */
    bool closable = t->fl >= 0 || count_leaves(A.root) > 1;
    if (closable && mouse_here && !dragging) {
        char id[32];
        snprintf(id, sizeof id, "##close%d", v);
        if (ui_icon_button_ex(id, "close", "x", r.x + r.w - px(30), r.y + px(6), px(24), UI_FLAT)) {
            if (t->fl >= 0) A.floats[t->fl] = -1;
            else remove_leaf(t->node);
            if (A.menu_viewer == v) A.menu_viewer = -1;
        }
        ui_tooltip("Close this view");
    }
    ui_pop_clip();
    ui_layer(prev);
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

/* ---- the menu of a tile -------------------------------------------------------------- */
/* a button with a second, dimmer line (shortcut); returns 1 on click, 2 when dragged */
static int menu_button_ex(const char *id, const char *icon, const char *letter, const char *label, const char *sub, Rect r,
                          bool selected, bool draggable, bool disabled) {
    UiInteract it = ui_interact(id, r.x, r.y, r.w, r.h);
    if (disabled) it.held = it.hovered = it.clicked = false;
    u32 fill = selected ? UI_COL_ACCENT_DIM : it.held ? UI_COL_WIDGET_ACTIVE : it.hovered ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET;
    u32 ink = disabled ? UI_COL_TEXT_DIM : UI_COL_TEXT;
    ui_rrect(r.x, r.y, r.w, r.h, px(6), fill);
    if (selected) ui_rrect_outline(r.x, r.y, r.w, r.h, px(6), px(1.5f), UI_COL_ACCENT);
    float lh = ui_font_line_height(UI_FONT_SANS);
    float ix = 0;
    if (icon || letter) {
        float is = px(26);
        ui_icon(icon, letter, r.x + px(10), r.y + (r.h - is) / 2, is, ink);
        ix = is + px(8);
    }
    float ty = sub && sub[0] ? r.y + r.h / 2 - lh + px(1) : r.y + (r.h - lh) / 2;
    ui_label(r.x + px(8) + ix, ty, r.w - px(16) - ix, lh, label, ix ? UI_ALIGN_LEFT : UI_ALIGN_CENTER, ink);
    if (sub && sub[0]) ui_label(r.x + px(8) + ix, ty + lh, r.w - px(16) - ix, lh, sub, ix ? UI_ALIGN_LEFT : UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
    if (draggable && it.held && fabsf(A.in.mouse_x - it.press_x) + fabsf(A.in.mouse_y - it.press_y) > px(8)) return 2;
    return it.clicked ? 1 : 0;
}

static int menu_button(const char *id, const char *icon, const char *letter, const char *label, const char *sub, Rect r,
                       bool selected, bool draggable) {
    return menu_button_ex(id, icon, letter, label, sub, r, selected, draggable, false);
}

static void menu_overlay(float W, float H) {
    int mv = A.menu_viewer;
    if (mv < 0 || !A.prog || mv >= A.prog->nviewers) {
        A.menu_viewer = -1;
        return;
    }
    Program *p = A.prog;
    int prev = ui_layer(UI_LAYER_MENU);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 175), 0);
    UiInteract bg = ui_interact("##menu_bg", 0, 0, W, H);
    ui_layer(UI_LAYER_POPUP); /* the panel and its buttons sit above the click-catcher */
    int tools[66], ntools = 0;
    tools[ntools++] = TOOL_PAN;
    if (p->viewers[mv]->picked) tools[ntools++] = TOOL_PICK;
    for (int k = 0; k < p->ntools && ntools < 66; k++)
        if (tool_in_viewer(k, mv)) tools[ntools++] = k;
    float bw = W < px(760) ? px(150) : px(190), bh = px(54), gap = px(12), head = px(34);
    int nviews = p->nviewers + 2;
    int cols = (int)FS_MAX(1.0f, FS_MIN((W - px(40)) / (bw + gap), 5.0f));
    bool flat = !p->viewers[mv]->mobius;
    int nplace = flat ? 6 : 3; /* back, forward, reset (copy and paste the location, the grid) */
    int rows = (nviews + cols - 1) / cols + (ntools + cols - 1) / cols + 1 + (nplace + cols - 1) / cols;
    int rows_top = (nviews + cols - 1) / cols, rows_bot = (ntools + cols - 1) / cols, rows_place = (nplace + cols - 1) / cols;
    float pw = cols * (bw + gap) + gap;
    float ph = px(12) + 4 * (head - px(10)) + 3 * px(22) + rows * (bh + gap) + ui_font_line_height(UI_FONT_SANS) + px(14);
    if (ph > H - px(20)) { /* a small window: smaller buttons (and the panel scrolls if that isn't enough) */
        bh = px(42);
        gap = px(8);
        head = px(30);
        pw = cols * (bw + gap) + gap;
        ph = px(12) + 4 * (head - px(10)) + 3 * px(22) + rows * (bh + gap) + ui_font_line_height(UI_FONT_SANS) + px(14);
    }
    float vh = FS_MIN(ph, H - px(20));
    Rect pr = {(W - pw) / 2, FS_MAX((H - vh) / 2, px(10)), pw, vh};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 240), px(10));
    A.menu_scroll = FS_CLAMP(A.menu_scroll + ui_scroll_area(pr.x, pr.y, pr.w, pr.h), 0.0f, ph - vh);
    ui_push_clip(pr.x, pr.y, pr.w, pr.h);
    float y = pr.y + px(12) - A.menu_scroll;
    ui_text(UI_FONT_SANS, pr.x + gap, y, "Views (click: show here, drag: split or float)", -1, UI_COL_TEXT_DIM);
    y += head - px(10);
    char sub[64], id[64], label[160], letter[8];
    for (int k = 0; k < nviews; k++) {
        Rect r = {pr.x + gap + (k % cols) * (bw + gap), y + (k / cols) * (bh + gap), bw, bh};
        if (k == p->nviewers) {
            const char *eh = MODKEY "+E";
            if (menu_button("##m_editor", "code", NULL, "Script", eh, r, A.editor_open, false) == 1) {
                A.editor_open = !A.editor_open;
                A.menu_viewer = -1;
            }
            continue;
        }
        if (k == p->nviewers + 1) {
            const char *ch = MODKEY "+T";
            if (menu_button("##m_config", "tune", NULL, "Config", ch, r, A.config_open, false) == 1) {
                A.config_open = !A.config_open;
                A.menu_viewer = -1;
            }
            continue;
        }
        int v = k;
        Section *s = p->viewers[v];
        shortcut_text(viewer_shortcut(v), sub, sizeof sub);
        snprintf(id, sizeof id, "##m_view%d", v);
        int res = menu_button(id, s->projective ? "public" : "crop_free", NULL, s->display ? s->display : s->name, sub, r,
                              viewer_visible(v), true);
        if (res == 1) {
            int leaf = find_leaf(A.root, mv);
            if (leaf >= 0) show_in_leaf(leaf, v);
            else {
                int f = find_float(mv);
                if (f >= 0 && !viewer_visible(v)) A.floats[f] = v;
            }
            A.focus_viewer = v;
            A.menu_viewer = -1;
        } else if (res == 2) {
            A.drag = DRAG_VIEWER;
            A.drag_viewer = v;
            A.menu_viewer = -1;
            ui_reset_interaction();
        }
        if (ui_last_hovered()) ui_tooltip("Click to show it here, or drag it onto the screen: middle replaces, sides split, corners float");
    }
    y += rows_top * (bh + gap) + px(8);
    ui_separator(pr.x + gap, y, pr.w - 2 * gap);
    y += px(14);
    snprintf(label, sizeof label, "Tools of %s", p->viewers[mv]->display ? p->viewers[mv]->display : p->viewers[mv]->name);
    ui_text(UI_FONT_SANS, pr.x + gap, y, label, -1, UI_COL_TEXT_DIM);
    y += head - px(10);
    for (int k = 0; k < ntools; k++) {
        Rect r = {pr.x + gap + (k % cols) * (bw + gap), y + (k / cols) * (bh + gap), bw, bh};
        int tool = tools[k];
        shortcut_text(tool_shortcut(tool), sub, sizeof sub);
        snprintf(id, sizeof id, "##m_tool%d", tool);
        tool_letter(tool, letter);
        if (menu_button(id, tool_icon(tool), letter, tool_name(tool), sub, r, A.vrt[mv].tool == tool, false) == 1) {
            select_tool(mv, tool, false);
            A.menu_viewer = -1;
        }
    }
    y += rows_bot * (bh + gap) + px(8);
    /* pictures */
    ui_separator(pr.x + gap, y, pr.w - 2 * gap);
    y += px(14);
    snprintf(label, sizeof label, "Picture of %s (PNG, without the tools' drawings)",
             p->viewers[mv]->display ? p->viewers[mv]->display : p->viewers[mv]->name);
    ui_text(UI_FONT_SANS, pr.x + gap, y, label, -1, UI_COL_TEXT_DIM);
    y += head - px(10);
    for (int k = 0; k < 2; k++) {
        int scale = k ? 4 : 1, ew, eh;
        export_size(mv, scale, &ew, &eh);
        snprintf(sub, sizeof sub, "%d x %d", ew, eh);
        Rect r = {pr.x + gap + k * (bw + gap), y, bw, bh};
        if (menu_button(k ? "##m_png4" : "##m_png", k ? "photo_size_select_large" : "image", NULL,
                        k ? "Save it large" : "Save picture", sub, r, false, false) == 1) {
            ask_export(mv, scale);
            A.menu_viewer = -1;
        }
        if (ui_last_hovered())
            ui_tooltip(k ? "Render this view again at four times the width and height (for print), and save it"
                         : "Save this view at its size on the screen (rendered at twice the size and smoothed)");
    }
    y += bh + gap + px(8);
    /* where the view looks: back, forward, home; the location as text */
    ui_separator(pr.x + gap, y, pr.w - 2 * gap);
    y += px(14);
    if (flat) {
        char loc[256];
        location_text(mv, loc, sizeof loc);
        char *nl = strchr(loc, '\n'), *r = nl ? nl + 1 : loc;
        if (nl) *nl = 0;
        if ((nl = strchr(r, '\n')) != NULL) *nl = 0;
        snprintf(label, sizeof label, "Place:  %.90s     %.60s", loc, r == loc ? "" : r);
    } else {
        snprintf(label, sizeof label, "Place on the Riemann sphere");
    }
    ui_label(pr.x + gap, y, pr.w - 2 * gap, ui_font_line_height(UI_FONT_SANS), label, UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
    y += head - px(10);
    for (int k = 0; k < nplace; k++) {
        static const char *icons[6] = {"arrow_back", "arrow_forward", "home", "content_copy", "content_paste", "grid_on"};
        static const char *names[6] = {"Back", "Forward", "Reset view", "Copy location", "Paste location", "Axes and grid"};
        static const char *subs[6] = {BACK_KEY, FORWARD_KEY, "Home", "centre and radius", "from the clipboard",
                                      MODKEY "+Shift+A"};
        static const char *tips[6] = {
            "Where this view was before the last zoom, pan or reset (the mouse's back button too)",
            "Undo Back", "The view the script starts with",
            "Copy the centre and the radius as script lines (\"mandel.center = ...\"), with all their digits",
            "Go to the centre and radius in the clipboard (copied with Copy location, or a point like -0.75+0.1i)",
            "Show the axes, and grid lines with their values"};
        Rect r = {pr.x + gap + (k % cols) * (bw + gap), y + (k / cols) * (bh + gap), bw, bh};
        char bid[32];
        snprintf(bid, sizeof bid, "##m_place%d", k);
        bool off = k < 2 && nav_count(mv, k == 1) == 0;
        if (menu_button_ex(bid, icons[k], NULL, names[k], subs[k], r, k == 5 && A.vrt[mv].grid, false, off) == 1) {
            if (k < 2) nav_go(mv, k == 1);
            else if (k == 2) reset_view(mv);
            else if (k == 3) copy_location(mv);
            else if (k == 4) paste_location(mv);
            else A.vrt[mv].grid = !A.vrt[mv].grid;
            A.menu_viewer = -1;
        }
        if (ui_last_hovered()) ui_tooltip(tips[k]);
    }
    y += rows_place * (bh + gap);
    ui_label(pr.x + gap, y, pr.w - 2 * gap, ui_font_line_height(UI_FONT_SANS),
             "Wheel: zoom   Right/middle drag or Space+drag: pan   Home: reset view   F1: help", UI_ALIGN_CENTER,
             UI_COL_TEXT_DIM);
    ui_pop_clip();
    if (bg.clicked && !in_rect(pr, A.in.mouse_x, A.in.mouse_y)) A.menu_viewer = -1;
    ui_layer(prev);
}

/* ---- side panes ----------------------------------------------------------------------- */
static float pane_bar(void) { return px(30); }

/* a collapsed pane: a thin bar with an icon; a click opens it */
static bool collapsed_bar(const char *id, Rect r, const char *icon, const char *arrow, const char *tip, bool alert) {
    ui_panel_ex(r.x, r.y, r.w, r.h, UI_COL_PANEL, 0);
    UiInteract it = ui_interact(id, r.x, r.y, r.w, r.h);
    if (it.hovered) ui_rect(r.x, r.y, r.w, r.h, UI_RGBA(255, 255, 255, 12));
    float s = px(22);
    ui_icon(icon, NULL, r.x + (r.w - s) / 2, r.y + px(10), s, it.hovered ? UI_COL_ACCENT_HOVER : UI_COL_TEXT_DIM);
    ui_icon(arrow, NULL, r.x + (r.w - s) / 2, r.y + px(38), s, it.hovered ? UI_COL_ACCENT_HOVER : UI_COL_TEXT_DIM);
    if (alert) ui_disk(r.x + r.w / 2, r.y + px(76), px(4), UI_COL_ERROR);
    if (it.hovered) ui_tooltip_now(tip);
    return it.clicked;
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

/* the find bar (and replace bar) at the top of the script pane; returns its height */
static float find_bar(float ew, float y) {
    if (!A.find_open) return 0;
    float rh = px(38), b = px(28), pad = px(6), gap = px(4);
    float h = rh * (A.replace_open ? 2 : 1);
    ui_panel_ex(0, y, ew, h, UI_COL_PANEL_HI, 0);
    ui_rect(0, y + h - px(1), ew, px(1), UI_COL_BORDER);
    float by = y + (rh - b) / 2;
    /* buttons from the right: close, replace toggle, match case, next, previous, count */
    float x = ew - pad - b;
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
    float fw = FS_MAX(px(60), x - gap - pad);
    int r = ui_text_field_ex("##find_text", pad, by, fw, b, A.find_buf, (int)sizeof A.find_buf,
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
        float ax = ew - pad - aw;
        if (ui_button_ex("All##repl_all", ax, ry, aw, b, 0) && A.find_buf[0]) {
            int n = editor_replace_all(A.ed, A.find_buf, A.replace_buf, A.find_case);
            set_status(false, "Replaced %d occurrence%s", n, n == 1 ? "" : "s");
        }
        ui_tooltip("Replace every occurrence (one undo step)");
        float rw = px(76);
        ax -= rw + gap;
        bool one = ui_button_ex("Replace##repl_one", ax, ry, rw, b, 0);
        ui_tooltip("Replace the selected occurrence and find the next");
        int rr = ui_text_field_ex("##replace_text", pad, ry, FS_MAX(px(60), ax - gap - pad), b, A.replace_buf,
                                  (int)sizeof A.replace_buf, UI_TF_KEEP_FOCUS, "Replace with");
        if ((one || (rr & UI_TEXT_SUBMIT)) && A.find_buf[0]) editor_replace(A.ed, A.find_buf, A.replace_buf, A.find_case);
        if (rr & UI_TEXT_CANCEL) close_find();
    }
    if (A.find_open) editor_set_search(A.ed, A.find_buf, A.find_case);
    return A.find_open ? h : 0;
}

static void editor_pane(float H) {
    Rect r = A.editor_r;
    if (!A.editor_open) {
        bool errors = A.prog == NULL || A.script_errors > 0;
        const char *tip = "Show the script (" MODKEY "+E)";
        if (collapsed_bar("##editor_bar", r, "code", "chevron_right", tip, errors)) A.editor_open = true;
        ui_rect(r.x + r.w - px(1), 0, px(1), H, UI_COL_BORDER);
        return;
    }
    float ew = r.w;
    float bh = px(40);
    ui_panel_ex(0, 0, ew, bh, UI_COL_PANEL_HI, 0);
    float x = px(8), y = px(6), bhh = bh - px(12);
    ui_push_clip(0, 0, ew - bhh - px(8), bh);
    bool wide = ew > px(340);
    float lh = ui_font_line_height(UI_FONT_SANS);
    float rw = wide ? ui_text_width(UI_FONT_SANS, "Run", -1) + bhh + px(12) : bhh;
    if (ui_button_ex("##run", x, y, rw, bhh, UI_ACCENT)) run_editor();
    ui_tooltip("Run the script (" MODKEY "+Enter)");
    ui_icon("play_arrow", ">", x + (wide ? px(2) : 0), y, bhh, UI_COL_ON_ACCENT);
    if (wide) ui_text(UI_FONT_SANS, x + bhh, y + (bhh - lh) / 2, "Run", -1, UI_COL_ON_ACCENT);
    x += rw + px(6);
    if (ui_icon_button_ex("##save", "save", "S", x, y, bhh, 0)) {
        if (A.path[0]) save_file(A.path);
        else ask_save_as();
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
    x += bhh + px(10);
    ui_toggle("Live", x, y + px(3), px(64), bhh - px(6), &A.live);
    ui_tooltip("Run the script automatically shortly after you stop typing");
    x += px(70);
    ui_pop_clip();
    /* collapse */
    if (ui_icon_button_ex("##editor_collapse", "chevron_left", "<", ew - bhh - px(6), y, bhh, UI_FLAT)) A.editor_open = false;
    ui_tooltip("Hide the script (" MODKEY "+E)");
    char name[300];
    snprintf(name, sizeof name, "%.250s%s", A.path[0] ? fs_basename(A.path) : "untitled", editor_is_modified(A.ed) ? " *" : "");
    if (x < ew - bhh - px(50)) ui_label(x, y, ew - x - bhh - px(16), bhh, name, UI_ALIGN_RIGHT, UI_COL_TEXT_DIM);
    editor_set_clock(A.ed, fs_time_seconds());
    static float last_fh;
    float fh = find_bar(ew, bh);
    if (fh != last_fh) editor_reveal(A.ed); /* the editor got shorter or taller: keep the match in view */
    last_fh = fh;
    editor_ui(A.ed, 0, bh + fh, ew, H - bh - fh, A.editor_grab);
    A.editor_grab = false;
    ui_rect(ew - px(1), 0, px(1), H, UI_COL_BORDER);
    float nx = pane_splitter("##editor_split", ew, H);
    if (nx == nx && A.in.fb_w > 0) A.editor_frac = FS_CLAMP(nx / (float)A.in.fb_w, 0.15f, 0.8f);
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

/* a value the script computes again whenever the state changes (@prepare): not a setting */
static bool prepared(Program *p, Sym *s) {
    for (int i = 0; i < p->nprepares; i++)
        if (block_assigns(&p->prepares[i]->body, s)) return true;
    return false;
}

/* a setting of the script (not a view's place, not something the app or @prepare keeps up to date) */
static bool script_setting(Program *p, Sym *s) {
    return s->kind == SYM_GLOBAL && !app_managed(p, s) && !view_geometry(p, s) && !prepared(p, s);
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
        for (int k = 0; k < 2; k++) {
            char *l = (char *)lines[k];
            l[strcspn(l, "\n")] = 0;
            const char *value = strstr(l, " = ") + 3;
            Stmt *st = defining_stmt(p, syms[k]);
            int a, b;
            if (st && literal_like(st->value) && expr_span(text, len, ls.data, nlines, st->value, &a, &b)) {
                KeepEdit e = {a, b - a, edits.len, xstrdup(value)};
                vec_push(edits, e);
            } else {
                sb_printf(&added, "%s\n", l);
            }
        }
        nviews++;
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

/* a list of numbers, edited as text: "0, 1, 1 + i" (Enter applies it) */
static void config_list(Sym *s, float x, float y, float w, float h) {
    Type *lt = s->type, *et = lt->elem;
    u32 *v = A.state + s->slot;
    char id[200];
    snprintf(id, sizeof id, "##cfglist_%s", s->name);
    u32 fid = ui_id(id);
    static char shown[1024];
    char *buf = shown;
    if (ui_focus_id() == fid && A.list_editing) {
        buf = A.list_buf;
    } else {
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
        snprintf(shown, sizeof shown, "%s", sb_cstr(&sb));
        sb_free(&sb);
    }
    bool before = ui_focus_id() == fid;
    int r = ui_text_field_ex(id, x, y, w, h, buf, buf == shown ? (int)sizeof shown : (int)sizeof A.list_buf, 0, "1, 2, 3");
    if (!before && ui_focus_id() == fid) { /* just focused: edit a copy */
        snprintf(A.list_buf, sizeof A.list_buf, "%s", shown);
        A.list_editing = true;
    }
    if ((r & UI_TEXT_SUBMIT) || (r & UI_TEXT_BLURRED)) {
        if (!(r & UI_TEXT_CANCEL) && buf == A.list_buf) {
            /* parse the items; a bad one leaves the list as it was */
            u32 *tmp = (u32 *)xcalloc((size_t)lt->size, sizeof(u32));
            int n = 0;
            bool ok = true;
            char *copy = xstrdup(A.list_buf);
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
        A.list_editing = false;
    }
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
    char id[160];
    snprintf(id, sizeof id, "##label_%s", s->name);
    bool hovered = ui_interact(id, x0, y, w, config_row_h() - px(4)).hovered; /* (for the tooltip) */
    bool changed = value_changed(p, s);
    if (s->viewer_index >= 0 && s == p->viewers[s->viewer_index]->center && p->viewers[s->viewer_index]->center_lo)
        changed = changed || value_changed(p, p->viewers[s->viewer_index]->center_lo);
    if (!changed || !(script_setting(p, s) || view_geometry(p, s))) {
        if (hovered && s->label) ui_tooltip(s->name);
        return;
    }
    ui_disk(x0 - px(5), y + (config_row_h() - px(4)) / 2, px(2.5f), UI_COL_ACCENT);
    if (!hovered) return;
    StrBuf sb = {0};
    sb_printf(&sb, "%s%s: changed; the script has ", s->is_internal ? "." : "", s->name);
    if (!value_literal(&sb, s->type, p->initial_state + s->slot)) sb_puts(&sb, "another value");
    ui_tooltip(sb_cstr(&sb));
    sb_free(&sb);
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

static void config_pane(float W, float H) {
    Rect pr = A.config_r;
    const char *key = MODKEY "+T";
    if (!A.config_open) {
        char tip[64];
        snprintf(tip, sizeof tip, "Show the config (%s)", key);
        if (collapsed_bar("##config_bar", pr, "tune", "chevron_left", tip, false)) A.config_open = true;
        ui_rect(pr.x, 0, px(1), H, UI_COL_BORDER);
        return;
    }
    Program *p = A.prog;
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_COL_PANEL, 0);
    ui_rect(pr.x, 0, px(1), H, UI_COL_BORDER);
    float bh = px(40);
    ui_panel_ex(pr.x, 0, pr.w, bh, UI_COL_PANEL_HI, 0);
    ui_text(UI_FONT_SANS, pr.x + px(14), px(11), "Config", -1, UI_COL_TEXT);
    if (ui_icon_button_ex("##config_collapse", "chevron_right", ">", pr.x + pr.w - px(34), px(6), px(28), UI_FLAT))
        A.config_open = false;
    char tip[64];
    snprintf(tip, sizeof tip, "Hide the config (%s)", key);
    ui_tooltip(tip);
    float nx = pane_splitter("##config_split", pr.x, H);
    if (nx == nx && W > 0) A.config_frac = FS_CLAMP((W - nx) / W, 0.12f, 0.6f);
    if (!p) return;
    /* the values changed since the script ran (a dot marks each): into the script, or back */
    int nchanged = count_changed(p, true), nsettings = count_changed(p, false);
    float bx = pr.x + pr.w - px(34) - px(32);
    if (bx > pr.x + px(70) &&
        ui_icon_button_ex("##config_write", "edit_note", "W", bx, px(6), px(28), nchanged ? UI_FLAT : UI_FLAT | UI_DISABLED))
        write_values();
    ui_tooltip(nchanged ? "Write the changed values (marked with a dot) and the places of the views into the script, so it opens like this"
                        : "Write the values into the script (nothing changed since it ran)");
    bx -= px(32);
    if (bx > pr.x + px(70) &&
        ui_icon_button_ex("##config_reset", "restore", "R", bx, px(6), px(28), nsettings ? UI_FLAT : UI_FLAT | UI_DISABLED))
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
    Rect body = {pr.x + px(4), bh + px(4), pr.w - px(8), pr.h - bh - px(8)};
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
        for (int i = 0; i < p->globals.len; i++) {
            Sym *s = p->globals.data[i];
            if (!config_auto(p, s, group)) continue;
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
                if (ui_button_ex(id, x1, y, ww, rh - px(4), 0)) reset_view(v);
                y += rh;
            }
        }
    }
    float content = y + A.config_scroll - body.y;
    A.config_scroll = FS_CLAMP(A.config_scroll, 0.0f, FS_MAX(0.0f, content - body.h));
    ui_pop_clip();
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
        {"Left edge of a view", "its tools; the menu button on top switches, splits and floats views"},
        {"Keys", NULL},
        {"Esc", "the menu of the view under the mouse (views, tools, pictures, place)"},
        {MODKEY "+Enter or " MODKEY "+R", "run the script"},
        {MODKEY "+S, " MODKEY "+Shift+S", "save, save as"},
        {MODKEY "+O", "open a file (or drop it on the window)"},
        {MODKEY "+F, " MODKEY "+Alt+F", "find, replace in the script (" MODKEY "+G: next)"},
        {MODKEY "+E, " MODKEY "+T", "show / hide the script, the config"},
        {MODKEY "+1, 2, ...", "show a view (or the shortcut written in the script)"},
        {"H, P, tool keys", "choose a tool of the view under the mouse"},
        {"Home", "reset the view under the mouse"},
        {"Alt+Left, Alt+Right", "back / forward: where the view was (also " MODKEY "+[ and " MODKEY "+])"},
        {MODKEY "+Shift+C, " MODKEY "+Shift+V", "copy / paste the location of the view (centre and radius)"},
        {MODKEY "+Shift+A", "axes and grid lines with their values"},
        {"F11", "full screen"},
        {"F1", "this help"},
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
    snprintf(A.modal_buf, sizeof A.modal_buf, "%s%s", path, folder && n && path[n - 1] != '/' && path[n - 1] != '\\' ? "/" : "");
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
    snprintf(A.modal_buf, sizeof A.modal_buf, "%.1020s", path);
}

static void modal_dialog(float W, float H) {
    if (!A.modal) {
        if (A.browse) fs_free_dir(A.browse, A.nbrowse);
        A.browse = NULL;
        A.nbrowse = 0;
        A.browse_dir[0] = 0;
        return;
    }
    int prev = ui_layer(UI_LAYER_POPUP);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    bool saving = A.modal != 2;
    const char *ext = A.modal == 3 ? ".png" : ".fstream";
    float rh = px(26), pad = px(16);
    float pw = FS_MIN(px(720), W - px(40)), ph = FS_MIN(px(560), H - px(40));
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 250), px(10));
    ui_text(UI_FONT_SANS, pr.x + pad, pr.y + px(14),
            A.modal == 1 ? "Save the script as:" : A.modal == 3 ? "Save the picture as (PNG):" : "Open a script:", -1, UI_COL_TEXT);
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
    const char *places[4][3] = {{"Home", "home", home}, {"Documents", "folder", doc_dir[0] ? doc_dir : NULL},
                                {"Examples", "school", examples_dir()[0] ? examples_dir() : NULL},
                                {"The script's folder", "folder_open", A.path[0] ? (script_dir[0] ? script_dir : ".") : NULL}};
    for (int k = 0; k < 4; k++) {
        if (!places[k][2]) continue;
        char id[64];
        snprintf(id, sizeof id, "%s##place%d", places[k][0], k);
        float bw = ui_text_width(UI_FONT_SANS, places[k][0], -1) + px(24);
        if (x + bw > pr.x + pr.w - pad) break;
        if (ui_button_ex(id, x, y, bw, px(26), UI_FLAT)) browse_to(places[k][2], true);
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
    if (A.browse) rows[nrows++] = -1;
    for (int i = 0; i < A.nbrowse && nrows < ARRAY_COUNT(rows); i++) {
        FsDirEntry *e = &A.browse[i];
        if (!e->is_dir && !has_ext(e->name, ext)) continue;
        if (!saving && name[0] && !contains_nocase(e->name, name)) continue; /* (opening: typing filters) */
        rows[nrows++] = i;
    }
    float content = nrows * rh;
    A.browse_scroll = FS_CLAMP(A.browse_scroll + ui_scroll_area(lr.x, lr.y, lr.w, lr.h), 0.0f, FS_MAX(0.0f, content - lr.h + px(8)));
    ui_push_clip(lr.x + 1, lr.y + 1, lr.w - 2, lr.h - 2);
    bool submit_file = false;
    for (int k = 0; k < nrows; k++) {
        float ry = lr.y + px(4) + k * rh - A.browse_scroll;
        if (ry + rh < lr.y || ry > lr.y + lr.h) continue;
        int i = rows[k];
        bool is_dir = i < 0 || A.browse[i].is_dir;
        const char *label = i < 0 ? ".. (the folder above)" : A.browse[i].name;
        char id[64];
        snprintf(id, sizeof id, "##brow%d", k);
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
    if (!A.browse) ui_label(lr.x, lr.y, lr.w, px(40), "(this folder can't be read)", UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
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
            if (stat(A.modal_buf, &st) != 0) {
                set_status(true, "There is no file %s", A.modal_buf);
                ok = false;
            }
        }
    }
    if (ok && A.modal_buf[0]) {
        if (A.modal == 1) save_file(A.modal_buf);
        else if (A.modal == 3) {
            snprintf(A.req_export, sizeof A.req_export, "%s", A.modal_buf);
            A.export_wait = 2;
            set_status(false, "Saving the picture...");
        }
        else request_open(A.modal_buf);
        A.modal = 0;
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
    Rect r = {A.area.x + px(10), H - th - px(10), tw, th};
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
        if (A.path[0]) save_file(A.path);
        else ask_save_as();
    }
    if (ui_shortcut(UI_KEY_S, UI_MOD_MOD | UI_MOD_SHIFT)) ask_save_as();
    if (ui_shortcut(UI_KEY_O, UI_MOD_MOD)) ask_open();
    if (ui_shortcut(UI_KEY_E, UI_MOD_MOD)) A.editor_open = !A.editor_open;
    if (ui_shortcut(UI_KEY_T, UI_MOD_MOD)) A.config_open = !A.config_open;
    if (ui_shortcut(UI_KEY_F11, 0)) platform_toggle_fullscreen();
    if (ui_shortcut(UI_KEY_F1, 0)) A.help_open = !A.help_open;
    if (ui_shortcut(UI_KEY_ESCAPE, 0)) {
        if (A.drag == DRAG_VIEWER) A.drag = DRAG_NONE;
        else if (A.help_open) A.help_open = false;
        else if (A.find_open && editor_has_focus(A.ed)) close_find();
        else if (A.menu_viewer >= 0) A.menu_viewer = -1;
        else A.menu_viewer = target_viewer();
    }
    for (int v = 0; v < p->nviewers; v++) {
        Shortcut sc = viewer_shortcut(v);
        if (sc.key == UI_KEY_NONE || (!(sc.mods & (UI_MOD_MOD | UI_MOD_ALT)) && ui_wants_keyboard())) continue;
        if (ui_shortcut(sc.key, sc.mods)) {
            int leaf = find_leaf(A.root, A.focus_viewer);
            if (leaf < 0) leaf = A.root;
            if (A.nodes[leaf].kind == N_LEAF) show_in_leaf(leaf, v);
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
        if (v >= 0) select_tool(v, t, true);
    }
    if (tv >= 0 && ui_shortcut(UI_KEY_H, 0)) select_tool(tv, TOOL_PAN, true);
    if (tv >= 0 && ui_shortcut(UI_KEY_P, 0) && p->viewers[tv]->picked) select_tool(tv, TOOL_PICK, true);
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
    if (!program_prepare(p, A.state, err, sizeof err)) set_status(true, "@prepare: %s", err);
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
        int rows = FS_MIN(FS_MIN(A.band_rows, rt->th - rt->next_row), rt->rows_left);
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
static void layout_panes(float W, float H) {
    float bar = pane_bar();
    float ew = A.editor_open ? floorf(W * A.editor_frac) : bar;
    float cw = A.config_open ? floorf(W * A.config_frac) : bar;
    if (A.config_open) cw = FS_MAX(cw, px(260));
    /* the views keep at least a third of the window */
    float avail = W - ew - cw;
    if (avail < W / 3) {
        float k = (W * 2 / 3) / FS_MAX(ew + cw, 1.0f);
        ew = FS_MAX(bar, floorf(ew * k));
        cw = FS_MAX(bar, floorf(cw * k));
    }
    A.editor_r = (Rect){0, 0, ew, H};
    A.config_r = (Rect){W - cw, 0, cw, H};
    A.area = (Rect){ew, 0, W - ew - cw, H};
}

static void frame(void) {
    UiInput *in = &A.in;
    float W = (float)in->fb_w, H = (float)in->fb_h;
    process_requests();
    Program *p = A.prog;
    if (!p) return;
    ui_begin_frame(in);
    int aw, ah;
    const u8 *atlas = ui_take_atlas_update(&aw, &ah);
    if (atlas) gfx_set_font_atlas(atlas, aw, ah);
    for (int k = 0; k < in->ndropped; k++) open_file(in->dropped_files[k], false);

    /* layout */
    layout_panes(W, H);
    A.nvis = 0;
    layout_compute(A.root, A.area);
    for (int c = 0; c < MAX_FLOATS; c++)
        if (A.floats[c] >= 0 && A.nvis < MAX_TILES + MAX_FLOATS) {
            VisTile t = {A.floats[c], float_rect(c), -1, c};
            A.vis[A.nvis++] = t;
        }
    for (int i = 0; i < A.nvis; i++) ensure_target(A.vis[i].viewer, (int)A.vis[i].r.w, (int)A.vis[i].r.h);
    A.hover_tile = -1;
    for (int i = A.nvis - 1; i >= 0; i--)
        if (in_rect(A.vis[i].r, in->mouse_x, in->mouse_y)) {
            A.hover_tile = i;
            break;
        }

    /* time */
    float t = (float)(fs_time_seconds() - A.t0);
    A.state[p->s_time->slot] = fw(t);

    viewer_interaction();
    tools_show();
    for (int i = 0; i < A.nvis; i++) {
        int prev = ui_layer(tile_layer(&A.vis[i]));
        draw_tile(&A.vis[i]);
        if (A.vis[i].fl >= 0) ui_occlude(A.vis[i].r.x, A.vis[i].r.y, A.vis[i].r.w, A.vis[i].r.h);
        ui_layer(prev);
    }
    for (int i = 0; i < A.nvis; i++) tile_chrome(i);
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
    config_pane(W, H);
    menu_overlay(W, H);
    help_overlay(W, H);
    modal_dialog(W, H);
    status_line(W, H);
    if (!A.modal) shortcuts();
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
        if (dt > 0.06 && A.band_rows > 16) A.band_rows /= 2;
        else if (dt < 0.025 && A.band_rows < 8192) A.band_rows *= 2;
        if (dt > 0.06 && A.flood_passes > 4) A.flood_passes /= 2;
        else if (dt < 0.025 && A.flood_passes < 4096) A.flood_passes *= 2;
    }
}

/* nothing could be loaded yet: only the editor (full width) */
static void frame_editor_only(void) {
    UiInput *in = &A.in;
    process_requests();
    if (A.prog) return; /* it compiled: the next frame shows the views */
    ui_begin_frame(in);
    int aw, ah;
    const u8 *atlas = ui_take_atlas_update(&aw, &ah);
    if (atlas) gfx_set_font_atlas(atlas, aw, ah);
    A.editor_open = true;
    A.editor_r = (Rect){0, 0, (float)in->fb_w, (float)in->fb_h};
    A.area = (Rect){(float)in->fb_w, 0, 0, (float)in->fb_h};
    editor_pane((float)in->fb_h);
    status_line((float)in->fb_w, (float)in->fb_h);
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

int app_run(const char *script_path) {
    memset(&A, 0, sizeof A);
    A.t0 = fs_time_seconds();
    A.band_rows = 8192;
    A.flood_passes = 64;
    A.editor_frac = 0.4f;
    A.config_frac = 0.22f;
    A.live = true;
    A.hover_viewer = -1;
    A.hover_tile = -1;
    A.menu_viewer = -1;
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
        fprintf(stderr, "error: no Vulkan library found. Install your GPU driver (or MoltenVK on macOS).\n");
        free(src);
        return 1;
    }
    if (!platform_init(FS_CLAMP(ww, 320, 8192), FS_CLAMP(wh, 240, 8192), "FractalStream", err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        free(src);
        return 1;
    }
    GfxInitDesc gd = {0};
    gd.glfw_window = platform_glfw_window();
    gd.vsync = true;
    gd.validation = getenv("FS_VALIDATION") != NULL;
    if (!gfx_init(&gd, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        platform_shutdown();
        free(src);
        return 1;
    }
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
    }
    const char *text = src ? src : welcome_script;
    editor_set_text(A.ed, text, (int)strlen(text));
    editor_set_unmodified(A.ed);
    A.file_mtime = file_mtime(A.path);
    A.editor_open = script_path == NULL;
    layout_reset(0);
    if (!load_source(text, LOAD_OPEN)) {
        A.editor_open = true; /* nothing to show: keep the editor up so the errors can be fixed */
    } else {
        for (int v = 0; v < A.prog->nviewers; v++) A.vrt[v].strip_until = A.t0 + 3.0; /* show where the tools are */
    }
    free(src);
    UiInput *in = &A.in;
    platform_poll(in, false);
    /* FS_AUTOQUIT=seconds closes the window after that long (for automated runs under sanitizers) */
    double autoquit = getenv("FS_AUTOQUIT") ? atof(getenv("FS_AUTOQUIT")) : 0;
    while (!platform_should_close() && !(autoquit > 0 && fs_time_seconds() - A.t0 > autoquit)) {
        if (A.prog) frame();
        else frame_editor_only();
        double wait = viewers_busy() || A.drag != DRAG_NONE ? 0.0 : FS_MIN(ui_redraw_in(), 0.5);
        if (A.status[0] && fs_time_seconds() < A.status_until) wait = FS_MIN(wait, 0.1);
        for (int v = 0; A.prog && v < A.prog->nviewers; v++)
            if (A.vrt[v].strip_until > fs_time_seconds()) wait = FS_MIN(wait, 0.2);
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
