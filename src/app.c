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
#endif

#define MAX_TILES 12
#define MAX_NODES (2 * MAX_TILES)
#define MAX_FLOATS 4

#if FS_OS_MACOS
#define MODKEY "Cmd" /* the name of UI_MOD_MOD in labels */
#else
#define MODKEY "Ctrl"
#endif

enum { TOOL_PAN = -2, TOOL_PICK = -1 };
enum { N_FREE, N_LEAF, N_HSPLIT, N_VSPLIT }; /* HSPLIT: a | b, VSPLIT: a over b */
enum { DRAG_NONE, DRAG_PAN, DRAG_TOOL, DRAG_PICK, DRAG_CLICK, DRAG_SPLIT, DRAG_VIEWER };
enum { DROP_NONE, DROP_REPLACE, DROP_LEFT, DROP_RIGHT, DROP_TOP, DROP_BOTTOM, DROP_FLOAT };

typedef struct Rect { float x, y, w, h; } Rect;
typedef struct LNode { int kind; int a, b; float ratio; int viewer; Rect r; } LNode;
typedef struct VisTile { int viewer; Rect r; int node; int fl; } VisTile;

typedef struct ViewerRT {
    int pipeline;      /* fragment shader (viewers without a `continuing` solve) */
    FloodGpu flood;    /* compute shaders (viewers with one) */
    bool is_flood;
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
    /* overlays */
    int menu_viewer;        /* viewer whose menu is open, -1 none */
    bool help_open;
    float config_scroll;
    int modal;              /* 0, 1 save as, 2 open */
    bool modal_focus;
    char modal_buf[1024];
    /* interaction */
    int drag;
    int drag_viewer, drag_node, drag_tool;
    float press_x, press_y, last_mx, last_my;
    bool moved;
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

static void select_tool(int v, int tool, bool flash) {
    A.vrt[v].tool = tool;
    if (flash) A.vrt[v].strip_until = fs_time_seconds() + 1.5;
}

/* ---- program loading ------------------------------------------------------------ */
static void free_viewers(void) {
    if (!A.prog) return;
    for (int v = 0; v < A.prog->nviewers; v++) {
        ViewerRT *rt = &A.vrt[v];
        if (rt->pipeline) gfx_viewer_destroy(rt->pipeline);
        flood_gpu_destroy(&rt->flood);
        if (rt->target) gfx_target_destroy(rt->target);
        free(rt->snap);
        free(rt->used);
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
        flood_gpu_destroy(&vrt[k].flood);
        free(vrt[k].used);
    }
    free(vrt);
}

/* compile `src`; on success replace the running program (keeping views and tools) */
static bool load_source(const char *src, bool reset_layout) {
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
        for (int i = 0; i < p->globals.len; i++)
            if (vrt[v].used[i] && p->globals.data[i] == p->s_time) vrt[v].animated = true;
        vrt[v].tool = TOOL_PAN;
    }
    /* new state; keep the views, picked points and tools of viewers that still exist */
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    int *remap = (int *)xmalloc(sizeof(int) * (size_t)(A.prog ? A.prog->nviewers : 1));
    for (int ov = 0; A.prog && ov < A.prog->nviewers; ov++) {
        remap[ov] = -1;
        Section *os = A.prog->viewers[ov];
        for (int v = 0; v < p->nviewers; v++) {
            Section *s = p->viewers[v];
            if (strcmp(s->name, os->name) != 0) continue;
            remap[ov] = v;
            memcpy(state + s->center->slot, A.state + os->center->slot, 8);
            memcpy(state + s->radius->slot, A.state + os->radius->slot, 4);
            if (s->mobius && os->mobius) memcpy(state + s->mobius->slot, A.state + os->mobius->slot, 32);
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
    /* keep the layout if all the viewers in it still exist */
    bool keep = A.prog && !reset_layout;
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
    interp_init(&A.interp, p, A.state);
    A.interp.step_limit = 3e7;
    if (!keep) layout_reset(0);
    for (int v = 0; v < p->nviewers; v++) {
        Sym *ps = p->viewers[v]->picked;
        if (ps && (state[ps->slot] || state[ps->slot + 1])) vrt[v].picked = true;
    }
    update_title();
    return true;
}

static bool open_file(const char *path) {
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
    bool ok = load_source(src, true);
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

static void run_editor(void) {
    int len;
    const char *text = editor_get_text(A.ed, &len);
    if (load_source(text, false)) set_status(false, "Running");
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

static void ask_save_as(void) {
    A.modal = 1;
    A.modal_focus = true;
    if (A.path[0]) snprintf(A.modal_buf, sizeof A.modal_buf, "%s", A.path);
    else default_save_path(A.modal_buf, sizeof A.modal_buf);
}

static void ask_open(void) {
    A.modal = 2;
    A.modal_focus = true;
    snprintf(A.modal_buf, sizeof A.modal_buf, "%s", A.path);
}

/* ---- running tools ----------------------------------------------------------------- */
static void run_tool(int tool, int viewer, float re, float im) {
    Program *p = A.prog;
    set_c(p->s_last_click, re, im);
    set_c(p->s_mouse, re, im);
    /* remember the picked points: a tool that moves one makes its marker appear */
    float before[64][2];
    for (int v = 0; v < p->nviewers && v < 64; v++) {
        Sym *ps = p->viewers[v]->picked;
        before[v][0] = ps ? wf(A.state[ps->slot]) : 0;
        before[v][1] = ps ? wf(A.state[ps->slot + 1]) : 0;
    }
    DrawItemVec out = {0};
    A.interp.draws = &out;
    if (!interp_run_tool(&A.interp, p->tools[tool], re, im)) set_status(true, "%s: %s", p->tools[tool]->display, A.interp.error);
    A.interp.draws = NULL;
    for (int k = 0; k < out.len; k++)
        if (out.data[k].viewer < 0) out.data[k].viewer = viewer;
    vec_free(A.draws[tool]);
    A.draws[tool] = out;
    for (int v = 0; v < p->nviewers && v < 64; v++) {
        Sym *ps = p->viewers[v]->picked;
        if (ps && (wf(A.state[ps->slot]) != before[v][0] || wf(A.state[ps->slot + 1]) != before[v][1]))
            for (int w = 0; w < p->nviewers; w++)
                if (p->viewers[w]->picked == ps) A.vrt[w].picked = true;
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
    float re, im;
    plane_of(v, r, x, y, &re, &im);
    float cx = wf(A.state[s->center->slot]), cy = wf(A.state[s->center->slot + 1]);
    set_c(s->center, re + (cx - re) * factor, im + (cy - im) * factor);
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
    float sc = view_scale(v, r);
    float cx = wf(A.state[s->center->slot]), cy = wf(A.state[s->center->slot + 1]);
    set_c(s->center, cx - dx * sc, cy + dy * sc);
}

static void reset_view(int v) {
    Section *s = A.prog->viewers[v];
    if (s->mobius) memcpy(A.state + s->mobius->slot, A.prog->initial_state + s->mobius->slot, 32);
    memcpy(A.state + s->center->slot, A.prog->initial_state + s->center->slot, 8);
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
            if (dx || dy) pan_by(t->viewer, t->r, dx, dy);
            platform_set_cursor(UI_CURSOR_MOVE);
            break;
        case DRAG_PICK:
            if (dx || dy) pick(t->viewer, re, im);
            platform_set_cursor(UI_CURSOR_CROSSHAIR);
            break;
        case DRAG_TOOL:
            if ((dx || dy) && A.drag_tool >= 0 && A.drag_tool < p->ntools) run_tool(A.drag_tool, t->viewer, re, im);
            platform_set_cursor(UI_CURSOR_CROSSHAIR);
            break;
        case DRAG_CLICK: /* a tool with .panning: dragging pans, a click runs the tool */
            if (A.moved && (dx || dy)) pan_by(t->viewer, t->r, dx, dy);
            if (in->mouse_released[0] && !A.moved && A.drag_tool >= 0 && A.drag_tool < p->ntools)
                run_tool(A.drag_tool, t->viewer, re, im);
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
        if (rt->tool >= 0 && p->tools[rt->tool]->hover && (mx != A.last_mx || my != A.last_my)) run_tool(rt->tool, v, re, im);
    }
    if (in->scroll_y != 0) zoom_at(v, t->r, mx, my, powf(0.85f, in->scroll_y));
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
    if (button != 0 || space || rt->tool == TOOL_PAN) {
        A.drag = DRAG_PAN;
        if (button == 0) set_c(p->s_last_click, re, im);
    } else if (rt->tool == TOOL_PICK) {
        A.drag = DRAG_PICK;
        pick(v, re, im);
    } else if (p->tools[rt->tool]->panning) {
        A.drag = DRAG_CLICK;
    } else {
        A.drag = DRAG_TOOL;
        run_tool(rt->tool, v, re, im);
    }
    ui_set_mouse_captured(true);
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
        }
    }
}

/* the picture, the drawings, the picked point and the name */
static void draw_tile(VisTile *t) {
    Program *p = A.prog;
    ViewerRT *rt = &A.vrt[t->viewer];
    Rect r = t->r;
    ui_push_clip(r.x, r.y, r.w, r.h);
    if (rt->target) ui_image(rt->target, r.x, r.y, (float)rt->tw, (float)rt->th);
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
        ui_tooltip("Views and tools (Esc)");
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
            snprintf(tip, sizeof tip, "%s%s%s%s", tool_name(tool), sc[0] ? " (" : "", sc, sc[0] ? ")" : "");
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
static int menu_button(const char *id, const char *icon, const char *letter, const char *label, const char *sub, Rect r,
                       bool selected, bool draggable) {
    UiInteract it = ui_interact(id, r.x, r.y, r.w, r.h);
    u32 fill = selected ? UI_COL_ACCENT_DIM : it.held ? UI_COL_WIDGET_ACTIVE : it.hovered ? UI_COL_WIDGET_HOVER : UI_COL_WIDGET;
    ui_rrect(r.x, r.y, r.w, r.h, px(6), fill);
    if (selected) ui_rrect_outline(r.x, r.y, r.w, r.h, px(6), px(1.5f), UI_COL_ACCENT);
    float lh = ui_font_line_height(UI_FONT_SANS);
    float ix = 0;
    if (icon || letter) {
        float is = px(26);
        ui_icon(icon, letter, r.x + px(10), r.y + (r.h - is) / 2, is, UI_COL_TEXT);
        ix = is + px(8);
    }
    float ty = sub && sub[0] ? r.y + r.h / 2 - lh + px(1) : r.y + (r.h - lh) / 2;
    ui_label(r.x + px(8) + ix, ty, r.w - px(16) - ix, lh, label, ix ? UI_ALIGN_LEFT : UI_ALIGN_CENTER, UI_COL_TEXT);
    if (sub && sub[0]) ui_label(r.x + px(8) + ix, ty + lh, r.w - px(16) - ix, lh, sub, ix ? UI_ALIGN_LEFT : UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
    if (draggable && it.held && fabsf(A.in.mouse_x - it.press_x) + fabsf(A.in.mouse_y - it.press_y) > px(8)) return 2;
    return it.clicked ? 1 : 0;
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
    float bw = px(190), bh = px(54), gap = px(12);
    int nviews = p->nviewers + 2;
    int cols = (int)FS_MAX(1.0f, FS_MIN((W - px(80)) / (bw + gap), 5.0f));
    int rows_top = (nviews + cols - 1) / cols, rows_bot = (ntools + cols - 1) / cols;
    float pw = cols * (bw + gap) + gap;
    float head = px(34);
    float ph = head + rows_top * (bh + gap) + px(30) + head + rows_bot * (bh + gap) + px(44);
    Rect pr = {(W - pw) / 2, FS_MAX((H - ph) / 2, px(10)), pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 240), px(10));
    float y = pr.y + px(12);
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
    y += rows_bot * (bh + gap);
    ui_label(pr.x + gap, y, pr.w - 2 * gap, ui_font_line_height(UI_FONT_SANS),
             "Wheel: zoom   Right/middle drag or Space+drag: pan   Home: reset view   F1: help", UI_ALIGN_CENTER,
             UI_COL_TEXT_DIM);
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
    editor_ui(A.ed, 0, bh, ew, H - bh, false);
    ui_rect(ew - px(1), 0, px(1), H, UI_COL_BORDER);
    float nx = pane_splitter("##editor_split", ew, H);
    if (nx == nx && A.in.fb_w > 0) A.editor_frac = FS_CLAMP(nx / (float)A.in.fb_w, 0.15f, 0.8f);
}

static float config_row_h(void) { return px(30); }

static void config_number(Sym *s, float x, float y, float w) {
    u32 *v = A.state + s->slot;
    float h = config_row_h() - px(4);
    char id[160];
    switch (s->type->kind) {
    case T_INT: {
        int val = (i32)v[0];
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        bool ch = s->has_range ? ui_slider_int(id, x, y, w, h, &val, (int)s->range_lo, (int)s->range_hi)
                               : ui_drag_int(id, x, y, w, h, &val, 0.25f, -2147483647, 2147483647);
        if (ch) v[0] = (u32)val;
        break;
    }
    case T_REAL: {
        double val = wf(v[0]);
        snprintf(id, sizeof id, "##cfg_%s", s->name);
        bool ch = s->has_range ? ui_slider_double(id, x, y, w, h, &val, s->range_lo, s->range_hi, "%.5g", 0)
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
    default: {
        StrBuf sb = {0};
        type_name(s->type, &sb);
        ui_label(x, y, w, h, sb_cstr(&sb), UI_ALIGN_LEFT, UI_COL_TEXT_DIM);
        sb_free(&sb);
        break;
    }
    }
}

/* which globals the config pane shows in each group: script variables, views, iteration settings */
static bool config_shows(Program *p, Sym *s, int group) {
    if (group == 0) return s->is_config;
    if (group == 1) return s->viewer_index >= 0 && !s->is_internal;
    return s == p->s_max_iter || s == p->s_escape || s == p->s_epsilon;
}

static void config_label(Sym *s, char *out, int cap) { snprintf(out, (size_t)cap, "%s%s", s->is_internal ? "." : "", s->name); }

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
    float rh = config_row_h();
    float lw = px(60);
    for (int group = 0; group < 3; group++)
        for (int i = 0; i < p->globals.len; i++)
            if (config_shows(p, p->globals.data[i], group)) {
                char label[128];
                config_label(p->globals.data[i], label, sizeof label);
                lw = FS_MAX(lw, ui_text_width(UI_FONT_SANS, label, -1) + px(24));
            }
    lw = FS_MIN(lw, pr.w * 0.55f);
    Rect body = {pr.x + px(4), bh + px(4), pr.w - px(8), pr.h - bh - px(8)};
    A.config_scroll += ui_scroll_area(body.x, body.y, body.w, body.h);
    ui_push_clip(body.x, body.y, body.w, body.h);
    float y = body.y - A.config_scroll + px(4);
    float x0 = body.x + px(10), x1 = body.x + lw, ww = body.w - lw - px(14);
    /* groups: script variables, views, iteration settings */
    for (int group = 0; group < 3; group++) {
        const char *title = group == 0 ? "Script" : group == 1 ? "Views" : "Iteration";
        bool any = false;
        for (int i = 0; i < p->globals.len; i++)
            if (config_shows(p, p->globals.data[i], group)) any = true;
        if (!any) continue;
        ui_text(UI_FONT_SANS, x0, y + px(6), title, -1, UI_COL_ACCENT);
        y += rh;
        for (int i = 0; i < p->globals.len; i++) {
            Sym *s = p->globals.data[i];
            if (!config_shows(p, s, group)) continue;
            char label[128];
            config_label(s, label, sizeof label);
            ui_label(x0, y, lw - px(14), rh - px(4), label, UI_ALIGN_LEFT, UI_COL_TEXT);
            config_number(s, x1, y, ww);
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
        y += px(8);
    }
    float content = y + A.config_scroll - body.y;
    A.config_scroll = FS_CLAMP(A.config_scroll, 0.0f, FS_MAX(0.0f, content - body.h));
    ui_pop_clip();
}

/* ---- overlays ------------------------------------------------------------------------- */
static void help_overlay(float W, float H) {
    if (!A.help_open) return;
    static const char *lines[] = {
        "Mouse",
        "  Wheel                      zoom at the mouse",
        "  Right or middle drag       pan (also Space + drag)",
        "  Left click / drag          use the view's selected tool",
        "  Left edge of a view        its tools; the menu button on top switches, splits and floats views",
        "Keys",
        "  Esc                        open / close the menu of the view under the mouse",
        "  " MODKEY "+Enter or " MODKEY "+R          run the script",
        "  " MODKEY "+S, " MODKEY "+Shift+S          save, save as",
        "  " MODKEY "+O                       open a file (or drop it on the window)",
        "  " MODKEY "+E, " MODKEY "+T                show / hide the script, the config",
        "  " MODKEY "+1, 2, ...               show a view (or the shortcut written in the script)",
        "  H, P, tool keys            choose a tool of the view under the mouse",
        "  Home                       reset the view under the mouse",
        "  F11                        full screen",
        "  F1                         this help",
    };
    int prev = ui_layer(UI_LAYER_POPUP);
    float lh = ui_font_line_height(UI_FONT_MONO) + px(3);
    float pw = FS_MIN(px(860), W - px(40)), ph = (ARRAY_COUNT(lines) + 2) * lh + px(20);
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    UiInteract bg = ui_interact("##help_bg", 0, 0, W, H);
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 245), px(10));
    for (int i = 0; i < ARRAY_COUNT(lines); i++)
        ui_text(lines[i][0] == ' ' ? UI_FONT_MONO : UI_FONT_SANS, pr.x + px(18), pr.y + px(14) + i * lh, lines[i], -1,
                lines[i][0] == ' ' ? UI_COL_TEXT : UI_COL_ACCENT);
    ui_label(pr.x, pr.y + ph - lh - px(8), pr.w, lh, "click anywhere to close", UI_ALIGN_CENTER, UI_COL_TEXT_DIM);
    if (bg.clicked) A.help_open = false;
    ui_layer(prev);
}

static void modal_dialog(float W, float H) {
    if (!A.modal) return;
    int prev = ui_layer(UI_LAYER_POPUP);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    float pw = FS_MIN(px(640), W - px(40)), ph = px(130);
    Rect pr = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 250), px(10));
    ui_text(UI_FONT_SANS, pr.x + px(16), pr.y + px(14), A.modal == 1 ? "Save as:" : "Open:", -1, UI_COL_TEXT);
    int r = ui_text_field_ex("##modal_path", pr.x + px(16), pr.y + px(44), pr.w - px(32), px(30), A.modal_buf,
                             (int)sizeof A.modal_buf, 0, "path of the file");
    if (A.modal_focus) {
        ui_set_focus(ui_id("##modal_path"));
        A.modal_focus = false;
    }
    bool ok = ui_button_ex("OK##modal_ok", pr.x + pr.w - px(196), pr.y + ph - px(44), px(84), px(30), UI_ACCENT);
    bool cancel = ui_button_ex("Cancel##modal_cancel", pr.x + pr.w - px(100), pr.y + ph - px(44), px(84), px(30), 0);
    if (r & UI_TEXT_SUBMIT) ok = true;
    if ((r & UI_TEXT_CANCEL) || ui_shortcut(UI_KEY_ESCAPE, 0)) cancel = true;
    if (ok && A.modal_buf[0]) {
        if (A.modal == 1) save_file(A.modal_buf);
        else open_file(A.modal_buf);
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
}

/* ---- rendering ---------------------------------------------------------------------- */
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
                if (!flood_gpu_start(&rt->flood, rt->tw, rt->th, sx, sy))
                    set_status(true, "Not enough GPU memory for the view '%s'", p->viewers[v]->name);
                rt->rendered = true;
            }
            if (!flood_gpu_done(&rt->flood)) {
                flood_gpu_record(&rt->flood, A.flood_passes, rt->target);
                if (!flood_gpu_done(&rt->flood)) busy = true;
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
        gfx_frame_render_viewer(rt->target, rt->pipeline, &vp);
        rt->next_row = vp.y1 >= rt->th ? 0 : vp.y1;
        rt->rows_left -= rows;
        if (rt->rows_left > 0) busy = true;
    }
    return busy;
}

static bool viewers_busy(void) {
    for (int i = 0; A.prog && i < A.nvis; i++) {
        ViewerRT *rt = &A.vrt[A.vis[i].viewer];
        if (rt->animated || rt->rows_left > 0 || (rt->is_flood && !flood_gpu_done(&rt->flood))) return true;
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
    Program *p = A.prog;
    ui_begin_frame(in);
    int aw, ah;
    const u8 *atlas = ui_take_atlas_update(&aw, &ah);
    if (atlas) gfx_set_font_atlas(atlas, aw, ah);
    for (int k = 0; k < in->ndropped; k++) open_file(in->dropped_files[k]);

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
            int len;
            const char *text = editor_get_text(A.ed, &len);
            load_source(text, false);
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
            open_file(A.path);
            set_status(false, "Reloaded %s (changed on disk)", fs_basename(A.path));
        }
    }

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
        load_source(t2, true);
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
    if (!load_source(text, true)) {
        A.editor_open = true; /* nothing to show: keep the editor up so the errors can be fixed */
    } else {
        if (!script_path && A.prog->nviewers >= 2) {
            split_leaf(A.root, DROP_RIGHT, 1);
            A.focus_viewer = 0;
        }
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
    editor_destroy(A.ed);
    gfx_shutdown();
    platform_shutdown();
    return 0;
}
