/* check.c - name resolution, type checking, implicit conversions, layout.
 *
 * Scoping (Python-like): a section body and a function body are scopes;
 * if/for/while bodies are not.  Declarations: `x := e` or `x : T = e`
 * (assigning a name that isn't declared is an error).  Numbers have their
 * own types (2 is Z, 2.5 is R, 2i is C); a variable declared without a type
 * takes the widest type assigned to it anywhere: when an assignment needs a
 * wider type, the checker notes it (Program.hints) and program_load checks
 * the script again.  Untyped vectors are at least real.  Variables declared
 * in @global/@config (outside functions) are program state.  Functions see
 * globals, their parameters, outputs and own locals.
 * Implicit promotions: Z -> R -> C -> H (elementwise for vectors). */
#include "ast.h"
#include <ctype.h>

/* ---- types ------------------------------------------------------------- */
Type *t_error, *t_void, *t_bool, *t_int, *t_real, *t_complex, *t_quat, *t_string, *t_color, *t_proj;
static Type *type_intern_list;

static Type *new_type(TypeKind k, int size, int align) {
    Type *t = (Type *)xcalloc(1, sizeof(Type));
    t->kind = k;
    t->size = size;
    t->align = align;
    t->next = type_intern_list;
    type_intern_list = t;
    return t;
}

void types_init(void) {
    if (t_int) return;
    t_error = new_type(T_ERROR, 1, 1);
    t_void = new_type(T_VOID, 0, 1);
    t_bool = new_type(T_BOOL, 1, 1);
    t_int = new_type(T_INT, 1, 1);
    t_real = new_type(T_REAL, 1, 1);
    t_complex = new_type(T_COMPLEX, 2, 2);
    t_quat = new_type(T_QUAT, 4, 4);
    t_string = new_type(T_STRING, 0, 1);
    t_color = type_vec(t_real, 3);
    t_proj = new_type(T_PROJ, 4, 4);
}

static int round_up(int x, int a) { return (x + a - 1) / a * a; }

Type *type_vec(Type *elem, int n) {
    for (Type *t = type_intern_list; t; t = t->next)
        if (t->kind == T_VEC && t->elem == elem && t->n == n) return t;
    int stride = round_up(elem->size, elem->align);
    Type *t = new_type(T_VEC, stride * n, elem->align);
    t->elem = elem;
    t->n = n;
    t->stride = stride;
    return t;
}

Type *type_list(Type *elem, int cap) {
    for (Type *t = type_intern_list; t; t = t->next)
        if (t->kind == T_LIST && t->elem == elem && t->n == cap) return t;
    int stride = round_up(elem->size, elem->align);
    int data_off = round_up(1, elem->align);
    int align = FS_MAX(1, elem->align);
    Type *t = new_type(T_LIST, round_up(data_off + stride * cap, align), align);
    t->elem = elem;
    t->n = cap;
    t->stride = stride;
    t->data_off = data_off;
    return t;
}

Type *type_tuple(Type **items, int n) {
    for (Type *t = type_intern_list; t; t = t->next) {
        if (t->kind != T_TUPLE || t->n != n) continue;
        bool same = true;
        for (int i = 0; i < n; i++)
            if (t->items[i] != items[i]) same = false;
        if (same) return t;
    }
    int off = 0, align = 1;
    for (int i = 0; i < n; i++) {
        off = round_up(off, items[i]->align) + items[i]->size;
        align = FS_MAX(align, items[i]->align);
    }
    Type *t = new_type(T_TUPLE, round_up(off, align), align);
    t->n = n;
    t->items = (Type **)xcalloc((size_t)n, sizeof(Type *));
    memcpy(t->items, items, sizeof(Type *) * (size_t)n);
    return t;
}

int type_number_rank(Type *t) {
    switch (t->kind) {
    case T_INT: return 1;
    case T_REAL: return 2;
    case T_COMPLEX: return 3;
    case T_QUAT: return 4;
    default: return 0;
    }
}
Type *type_number_of_rank(int r) { return r == 1 ? t_int : r == 2 ? t_real : r == 3 ? t_complex : t_quat; }
bool type_is_number(Type *t) { return type_number_rank(t) > 0; }
bool type_is_scalar_real(Type *t) { return t->kind == T_INT || t->kind == T_REAL; }

void type_name(Type *t, StrBuf *out) {
    if (!t) { /* (a parameter of a generic function: any type) */
        sb_puts(out, "any");
        return;
    }
    switch (t->kind) {
    case T_ERROR: sb_puts(out, "<error>"); break;
    case T_VOID: sb_puts(out, "nothing"); break;
    case T_BOOL: sb_puts(out, "Bool"); break;
    case T_INT: sb_puts(out, "Z"); break;
    case T_REAL: sb_puts(out, "R"); break;
    case T_COMPLEX: sb_puts(out, "C"); break;
    case T_QUAT: sb_puts(out, "H"); break;
    case T_PROJ: sb_puts(out, "P"); break;
    case T_STRING: sb_puts(out, "String"); break;
    case T_VEC:
        type_name(t->elem, out);
        sb_printf(out, "^%d", t->n);
        break;
    case T_LIST:
        sb_puts(out, "List of ");
        type_name(t->elem, out);
        break;
    case T_TUPLE:
        sb_putc(out, '(');
        for (int i = 0; i < t->n; i++) {
            if (i) sb_puts(out, ", ");
            type_name(t->items[i], out);
        }
        sb_putc(out, ')');
        break;
    }
}

const char *type_str(Type *t) {
    static char bufs[6][96];
    static int k = 0;
    char *b = bufs[k++ % 6];
    StrBuf sb = {0};
    type_name(t, &sb);
    snprintf(b, 96, "%s", sb_cstr(&sb));
    sb_free(&sb);
    return b;
}

/* ---- diagnostics ------------------------------------------------------- */
#define PRELUDE_LINE0 1000000

void prog_diag(Program *p, DiagSeverity sev, int line, int col, int end_col, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Diag d;
    d.severity = sev;
    d.line = line;
    d.col = col;
    d.end_col = end_col;
    if (line >= PRELUDE_LINE0) {
        d.msg = arena_printf(&p->arena, "internal error in the built-in functions (line %d): %s", line - PRELUDE_LINE0, buf);
        d.line = 1;
        d.col = 1;
        d.end_col = 0;
    } else {
        d.msg = arena_strndup(&p->arena, buf, strlen(buf));
    }
    vec_push(p->diags, d);
    if (sev == DIAG_ERROR) p->nerrors++;
}

/* ---- scopes ------------------------------------------------------------ */
typedef struct Scope Scope;
struct Scope {
    SymVec syms;
    Scope *parent;
};

static Sym *scope_find_local(Scope *s, const char *name) {
    for (int i = s->syms.len - 1; i >= 0; i--)
        if (strcmp(s->syms.data[i]->name, name) == 0) return s->syms.data[i];
    return NULL;
}
static Sym *scope_find(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        Sym *x = scope_find_local(s, name);
        if (x) return x;
    }
    return NULL;
}

typedef struct Checker {
    Program *prog;
    Scope builtin;   /* pi, e, tau */
    Scope globals;   /* program state (parent: builtin) */
    Scope internals; /* .names */
    Scope *scope;    /* current section/function scope */
    Section *sec;
    Func *fn;
    SymVec *locals;  /* frame of the current section/function */
    bool load;       /* in @global/@config outside functions */
    int loop_depth;
    u64 ctx;         /* the instance of a generic function being checked (0: none): see TypeHint */
    int probing;     /* checking a generic function nothing calls (probe_template): it must not change globals */
} Checker;

static Sym *new_sym(Checker *c, const char *name, Type *t, SymKind kind, int line, int col) {
    Sym *s = ARENA_NEW(&c->prog->arena, Sym);
    s->name = (char *)name;
    s->type = t;
    s->kind = kind;
    s->line = line;
    s->col = col;
    s->viewer_index = -1;
    s->hint_ctx = c->ctx;
    return s;
}

#define ERR(c, e, ...) prog_diag((c)->prog, DIAG_ERROR, (e)->line, (e)->col, (e)->end_col, __VA_ARGS__)

/* ---- conversions -------------------------------------------------------- */
static int conv_cost(Type *from, Type *to) {
    if (from == to) return 0;
    if (from == t_error || to == t_error) return 0;
    int rf = type_number_rank(from), rt = type_number_rank(to);
    if (rf && rt) return rf <= rt ? rt - rf : -1;
    if (to == t_proj && rf && rf <= 3) return 4 - rf; /* z -> [z : 1] */
    if (from->kind == T_VEC && to->kind == T_VEC && from->n == to->n) return conv_cost(from->elem, to->elem);
    return -1;
}

static Expr *mk_convert(Checker *c, Expr *e, Type *to) {
    if (e->type == to || e->type == t_error || to == t_error) return e;
    Expr *x = ARENA_NEW(&c->prog->arena, Expr);
    x->kind = E_CONVERT;
    x->line = e->line;
    x->col = e->col;
    x->end_col = e->end_col;
    x->a = e;
    x->type = to;
    return x;
}

/* the type of a letter of IntrinsicInfo.params / .ret */
static Type *intrinsic_type(char pc) {
    return pc == 'Z' ? t_int : pc == 'R' ? t_real : pc == 'C' ? t_complex : pc == 'H' ? t_quat : pc == 'P' ? t_proj : t_bool;
}

static const char *narrowing_hint(Type *from, Type *to) {
    if (from == t_proj) return " (for a point p of the sphere, complex(p) is its complex number)";
    if (from->kind == T_COMPLEX && type_is_scalar_real(to)) return " (use re(z), im(z) or |z| to get a real number)";
    if (from->kind == T_REAL && to->kind == T_INT) return " (use floor, ceil or round to get an integer)";
    if (from->kind == T_COMPLEX && to->kind == T_INT) return " (use re(z), im(z) or |z|, then floor, ceil or round)";
    return "";
}

/* convert e to type `to`, or report an error mentioning `what` */
static Expr *coerce(Checker *c, Expr *e, Type *to, const char *what) {
    if (!e || e->type == t_error || to == t_error) return e;
    if (to->kind == T_LIST && e->kind == E_VECTOR) {
        if (e->nargs > to->n) {
            ERR(c, e, "too many items for %s (capacity %d)", type_str(to), to->n);
            return e;
        }
        for (int i = 0; i < e->nargs; i++) e->args[i] = coerce(c, e->args[i], to->elem, "a list item");
        e->type = to;
        return e;
    }
    int cost = conv_cost(e->type, to);
    if (cost < 0) {
        ERR(c, e, "%s must be %s, but this is %s%s", what, type_str(to), type_str(e->type), narrowing_hint(e->type, to));
        return e;
    }
    return mk_convert(c, e, to);
}

static Type *join_numbers(Type *a, Type *b) {
    return type_number_of_rank(FS_MAX(type_number_rank(a), type_number_rank(b)));
}

/* Type for an untyped declaration initialized with a value of type t: the type of the value (2 is Z, 2.5 is R);
   vectors are at least real.  (It may get wider, see TypeHint.) */
static Type *infer_decl_type(Type *t, Expr *value) {
    (void)value;
    if (t->kind == T_VEC && type_number_rank(t->elem) < 2) return type_vec(t_real, t->n);
    return t;
}

/* the type that holds values of both a and b (numbers: the wider one; vectors of the same length: elementwise), or
   NULL */
static Type *join_types(Type *a, Type *b) {
    if (a == b) return a;
    if (type_is_number(a) && type_is_number(b)) return join_numbers(a, b);
    if ((a == t_proj && conv_cost(b, a) >= 0) || (b == t_proj && conv_cost(a, b) >= 0)) return t_proj;
    if (a->kind == T_VEC && b->kind == T_VEC && a->n == b->n && type_is_number(a->elem) && type_is_number(b->elem))
        return type_vec(join_numbers(a->elem, b->elem), a->n);
    return NULL;
}

/* ---- type syntax ------------------------------------------------------- */
static Type *resolve_tsyn(Checker *c, TypeSyntax *ts) {
    if (!ts) return t_complex;
    const char *b = ts->base;
    Type *base = NULL;
    if (!strcmp(b, "Z") || !strcmp(b, "Integer")) base = t_int;
    else if (!strcmp(b, "R") || !strcmp(b, "Real")) base = t_real;
    else if (!strcmp(b, "C") || !strcmp(b, "Complex")) base = t_complex;
    else if (!strcmp(b, "H") || !strcmp(b, "Quaternion")) base = t_quat;
    else if (!strcmp(b, "P") || !strcmp(b, "Projective")) base = t_proj;
    else if (!strcmp(b, "Bool") || !strcmp(b, "Boolean")) base = t_bool;
    else if (!strcmp(b, "Color") || !strcmp(b, "Colour")) base = t_color;
    else if (!strcmp(b, "List")) {
        Type *e = resolve_tsyn(c, ts->elem);
        if (!type_is_number(e) && e != t_bool && e != t_proj) {
            prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, 0, "lists can only hold numbers, points of P or Bool, not %s", type_str(e));
            return t_error;
        }
        return type_list(e, ts->cap > 0 ? ts->cap : 64);
    } else {
        static const char *plural[][2] = {{"Integers", "Integer"}, {"Reals", "Real"}, {"Quaternions", "Quaternion"},
                                          {"Booleans", "Boolean"}, {"Int", "Integer"}, {"Colors", "Color"}};
        for (int k = 0; k < ARRAY_COUNT(plural); k++)
            if (!strcmp(b, plural[k][0])) {
                prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, ts->col + (int)strlen(b),
                          "unknown type '%s': write %s (or %s)", b, plural[k][1],
                          !strcmp(plural[k][1], "Integer") ? "Z" : !strcmp(plural[k][1], "Real") ? "R" :
                          !strcmp(plural[k][1], "Quaternion") ? "H" : !strcmp(plural[k][1], "Boolean") ? "Bool" : "Color");
                return t_error;
            }
        prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, ts->col + (int)strlen(b),
                  "unknown type '%s' (types are Z, R, C, H, P, Bool, Color, R^n and List of T; or Integer, Real, Complex, "
                  "Quaternion, Projective)", b);
        return t_error;
    }
    if (ts->power) {
        if (!type_is_number(base)) {
            prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, 0, "only number types can have a power, as in R^3");
            return t_error;
        }
        return type_vec(base, ts->power);
    }
    return base;
}

/* ---- internals ---------------------------------------------------------- */
static Sym *add_internal(Checker *c, const char *name, Type *t, SymKind kind, const char *doc) {
    Sym *s = new_sym(c, name, t, kind, 0, 0);
    s->is_internal = true;
    s->doc = doc;
    vec_push(c->internals.syms, s);
    if (kind == SYM_GLOBAL && t != t_string) vec_push(c->prog->globals, s);
    return s;
}

static void add_color(Checker *c, const char *name, float r, float g, float b) {
    Sym *s = add_internal(c, name, t_color, SYM_CONST, "color constant");
    s->readonly = true;
    s->cval[0] = r;
    s->cval[1] = g;
    s->cval[2] = b;
}

static void add_const(Checker *c, Scope *sc, const char *name, Type *t, float v0, float v1, float v2, float v3) {
    Sym *s = new_sym(c, name, t, SYM_CONST, 0, 0);
    s->readonly = true;
    s->cval[0] = v0;
    s->cval[1] = v1;
    s->cval[2] = v2;
    s->cval[3] = v3;
    vec_push(sc->syms, s);
}

static void setup_internals(Checker *c) {
    Program *p = c->prog;
    p->s_max_iter = add_internal(c, "maxIterations", t_int, SYM_GLOBAL, "iteration limit of every loop");
    p->s_epsilon = add_internal(c, "epsilon", t_real, SYM_GLOBAL, "tolerance of == on R and C, and of `vanishes`");
    p->s_escape = add_internal(c, "escapeRadius", t_real, SYM_GLOBAL, "z escapes means |z| > .escapeRadius");
    p->s_time = add_internal(c, "time", t_real, SYM_GLOBAL,
                             "the time in seconds: it runs from 0 when the script starts, and stops while paused");
    p->s_time->readonly = true;   /* (except in tools: see check_assign_target) */
    p->s_playing = add_internal(c, "playing", t_bool, SYM_GLOBAL, "true while .time runs (and in videos), false while paused");
    p->s_playing->readonly = true;
    p->s_duration = add_internal(c, "duration", t_real, SYM_GLOBAL,
                                 "the length of the animation in seconds: .time loops over it (0, the default: no length)");
    p->s_last_click = add_internal(c, "lastClick", t_complex, SYM_GLOBAL, "point of the last click or drag");
    p->s_mouse = add_internal(c, "mouse", t_complex, SYM_GLOBAL, "point under the mouse");
    p->s_mouse->readonly = true;
    p->s_drag_start = add_internal(c, "dragStart", t_complex, SYM_GLOBAL, "where the button went down (tools)");
    p->s_drag_start->readonly = true;
    p->s_width = add_internal(c, "width", t_int, SYM_GLOBAL, "initial window width");
    p->s_height = add_internal(c, "height", t_int, SYM_GLOBAL, "initial window height");
    p->s_double = add_internal(c, "doublePrecision", t_bool, SYM_GLOBAL,
                               "deep zooms: views switch to double precision when their pixels get too small for single precision");
    p->s_aa = add_internal(c, "antialiasing", t_int, SYM_GLOBAL,
                           "samples per pixel of a view at rest (1: off; at most 16): more samples, smoother fine detail");
    p->s_aa->has_range = true;   /* (a slider in the config panel) */
    p->s_aa->range_lo = 1;
    p->s_aa->range_hi = 16;
    add_internal(c, "title", t_string, SYM_GLOBAL, "window title");
    p->s_stuck = add_internal(c, "stuck", t_bool, SYM_EXEC, "true if the last loop hit .maxIterations");
    p->s_iterations = add_internal(c, "iterations", t_int, SYM_EXEC, "number of iterations of the last loop");
    p->s_pixel = add_internal(c, "pixel", t_real, SYM_EXEC, "size of a pixel of the viewer (in a tool: of the viewer clicked)");
    p->s_pixel->readonly = true;
    add_color(c, "violet", 1, 0, 160.0f / 255.0f);
    add_color(c, "red", 1, 0, 0);
    add_color(c, "green", 0, 1, 0);
    add_color(c, "blue", 0, 0, 1);
    add_color(c, "black", 0, 0, 0);
    add_color(c, "white", 1, 1, 1);
    add_color(c, "gray", 0.5f, 0.5f, 0.5f);
    add_color(c, "grey", 0.5f, 0.5f, 0.5f);
    add_color(c, "yellow", 1, 1, 0);
    add_color(c, "cyan", 0, 1, 1);
    add_color(c, "magenta", 1, 0, 1);
    add_color(c, "orange", 1, 0.5f, 0);
    add_color(c, "purple", 0.5f, 0, 0.5f);
    add_color(c, "pink", 1, 0.75f, 0.8f);
    add_color(c, "brown", 0.6f, 0.4f, 0.2f);
    Sym *j = add_internal(c, "j", t_quat, SYM_CONST, "quaternion unit j");
    j->readonly = true;
    j->cval[2] = 1;
    Sym *k = add_internal(c, "k", t_quat, SYM_CONST, "quaternion unit k");
    k->readonly = true;
    k->cval[3] = 1;
    Sym *inf = add_internal(c, "infinity", t_proj, SYM_CONST, "the point at infinity of the Riemann sphere, [1 : 0]");
    inf->readonly = true;
    inf->cval[0] = 1;
    add_const(c, &c->builtin, "pi", t_real, 3.14159265358979f, 0, 0, 0);
    add_const(c, &c->builtin, "π", t_real, 3.14159265358979f, 0, 0, 0);
    add_const(c, &c->builtin, "∞", t_proj, 1, 0, 0, 0); /* (also .infinity) */
    add_const(c, &c->builtin, "tau", t_real, 6.28318530717959f, 0, 0, 0);
    add_const(c, &c->builtin, "e", t_real, 2.71828182845905f, 0, 0, 0);

    /* viewer state: picked point .var, name.center, name.radius */
    for (int v = 0; v < p->nviewers; v++) {
        Section *s = p->viewers[v];
        if (!s->name || !s->var) continue;
        Sym *picked = scope_find_local(&c->internals, s->var);
        if (!picked) {
            picked = add_internal(c, arena_strndup(&p->arena, s->var, strlen(s->var)), t_complex,
                                  SYM_GLOBAL, "last point picked in the viewer");
            picked->viewer_index = v;
        } else if (picked->viewer_index < 0) {
            prog_diag(p, DIAG_ERROR, s->line, 1, 0, "the viewer variable '%s' clashes with the internal .%s; choose another name", s->var, s->var);
        }
        s->picked = picked;
        s->center = new_sym(c, arena_printf(&p->arena, "%s.center", s->name), t_complex, SYM_GLOBAL, s->line, 1);
        s->center->viewer_index = v;
        s->center->doc = "center of the view";
        s->radius = new_sym(c, arena_printf(&p->arena, "%s.radius", s->name), t_real, SYM_GLOBAL, s->line, 1);
        s->radius->viewer_index = v;
        s->radius->doc = "radius of the largest disk around the center that fits in the view";
        vec_push(p->globals, s->center);
        vec_push(p->globals, s->radius);
        if (!s->projective) { /* deep zooms: the center to twice the precision of a float (the app keeps it) */
            s->center_lo = new_sym(c, arena_printf(&p->arena, "%s.center lo", s->name), t_complex, SYM_GLOBAL, s->line, 1);
            s->center_lo->viewer_index = v;
            s->center_lo->is_internal = true;
            s->center_lo->doc = "low part of the center (deep zooms)";
            vec_push(p->globals, s->center_lo);
        }
        if (s->projective) { /* view of the Riemann sphere: Mobius map (a w + b) / (c w + d) */
            s->mobius = new_sym(c, arena_printf(&p->arena, "%s.mobius", s->name), type_vec(t_complex, 4), SYM_GLOBAL, s->line, 1);
            s->mobius->viewer_index = v;
            s->mobius->is_internal = true;
            s->mobius->doc = "current view of the sphere";
            vec_push(p->globals, s->mobius);
            s->conformal = new_sym(c, arena_printf(&p->arena, "%s.conformal", s->name), t_bool, SYM_GLOBAL, s->line, 1);
            s->conformal->viewer_index = v;
            s->conformal->doc = "a conformal view (the default): the sphere seen from a point of it (stereographic "
                                "projection); false: seen from its center";
            vec_push(p->globals, s->conformal);
        }
    }
}

static Section *find_viewer(Program *p, const char *name) {
    for (int v = 0; v < p->nviewers; v++)
        if (p->viewers[v]->name && strcmp(p->viewers[v]->name, name) == 0) return p->viewers[v];
    return NULL;
}

/* ---- expressions ------------------------------------------------------- */
static Type *chk(Checker *c, Expr *e);
static void check_func(Checker *c, Func *f);
static void layout_frame(SymVec *locals, int *frame_words);
static Type *chk_call(Checker *c, Expr *e);

static bool is_int_const(Expr *e, int *out) {
    if (e->kind == E_INT) {
        *out = e->ival;
        return true;
    }
    if (e->kind == E_UNARY && e->op == TK_MINUS && e->a->kind == E_INT) {
        *out = -e->a->ival;
        return true;
    }
    return false;
}

static Expr *mk_call(Checker *c, Expr *at, const char *name, Expr *a, Expr *b) {
    at->kind = E_CALL;
    at->name = (char *)name;
    at->args = ARENA_ARRAY(&c->prog->arena, Expr *, 2);
    at->args[0] = a;
    at->args[1] = b;
    at->nargs = b ? 2 : 1;
    at->a = at->b = NULL;
    return at;
}

static Type *chk_power(Checker *c, Expr *e) {
    Type *ta = chk(c, e->a);
    if (ta == t_error) return t_error;
    int n;
    if (ta == t_proj) { /* [a : b]^n = [a^n : b^n] */
        if (is_int_const(e->b, &n) && n >= -4096 && n <= 4096) {
            e->kind = E_POWI;
            e->ival = n;
            e->b = NULL;
            return t_proj;
        }
        Type *tb = chk(c, e->b);
        if (tb == t_error) return t_error;
        if (tb != t_int) {
            ERR(c, e->b, "a point of the sphere can only be raised to integer powers, not %s", type_str(tb));
            return t_error;
        }
        mk_call(c, e, "__ppowi", e->a, e->b);
        return chk_call(c, e);
    }
    if (is_int_const(e->b, &n) && type_is_number(ta) && n >= -4096 && n <= 4096) {
        if (ta == t_int && n < 0) e->a = mk_convert(c, e->a, t_real);
        e->kind = E_POWI;
        e->ival = n;
        e->b = NULL;
        return e->a->type;
    }
    Type *tb = chk(c, e->b);
    if (tb == t_error) return t_error;
    if (!type_is_number(ta) || !type_is_number(tb)) {
        ERR(c, e, "can't raise %s to the power %s", type_str(ta), type_str(tb));
        return t_error;
    }
    if (ta == t_quat || tb == t_quat) {
        ERR(c, e, "quaternion powers need a constant integer exponent, as in q^3");
        return t_error;
    }
    if (tb == t_int && type_is_scalar_real(ta)) {
        mk_call(c, e, "__powri", e->a, e->b);
        return chk_call(c, e);
    }
    if (tb == t_int && ta == t_complex) {
        mk_call(c, e, "__powci", e->a, e->b);
        return chk_call(c, e);
    }
    if (type_is_scalar_real(ta) && type_is_scalar_real(tb)) {
        mk_call(c, e, "__powr", e->a, e->b);
        return chk_call(c, e);
    }
    mk_call(c, e, "__powc", e->a, e->b);
    return chk_call(c, e);
}

static bool is_arith(int op) { return op == TK_PLUS || op == TK_MINUS || op == TK_STAR || op == TK_SLASH; }

static const char *op_name(int op) {
    switch (op) {
    case TK_PLUS: return "+";
    case TK_MINUS: return "-";
    case TK_STAR: return "*";
    case TK_SLASH: return "/";
    case TK_MOD: return "mod";
    case TK_EQ: return "==";
    case TK_NE: return "!=";
    case TK_LT: return "<";
    case TK_LE: return "<=";
    case TK_GT: return ">";
    case TK_GE: return ">=";
    case TK_AND: return "and";
    case TK_OR: return "or";
    default: return "?";
    }
}

static Type *chk_binary(Checker *c, Expr *e) {
    int op = e->op;
    if (op == TK_CARET) return chk_power(c, e);
    Type *ta = chk(c, e->a), *tb = chk(c, e->b);
    if (ta == t_error || tb == t_error) return t_error;
    if (op == TK_AND || op == TK_OR) {
        e->a = coerce(c, e->a, t_bool, "each side of 'and'/'or'");
        e->b = coerce(c, e->b, t_bool, "each side of 'and'/'or'");
        return t_bool;
    }
    if ((ta == t_proj || tb == t_proj) && (is_arith(op) || op == TK_EQ || op == TK_NE)) {
        if (join_types(ta, tb) != t_proj) {
            ERR(c, e, "can't use '%s' with %s and %s", op_name(op), type_str(ta), type_str(tb));
            return t_error;
        }
        e->a = mk_convert(c, e->a, t_proj);
        e->b = mk_convert(c, e->b, t_proj);
        if (is_arith(op)) return t_proj; /* (written with the coordinates later: px_lower_expr) */
        /* p == q: closer than .epsilon in the chordal distance */
        Expr *call = ARENA_NEW(&c->prog->arena, Expr);
        *call = *e;
        mk_call(c, call, "__peq", e->a, e->b);
        if (chk_call(c, call) == t_error) return t_error;
        if (op == TK_EQ) {
            *e = *call;
        } else {
            e->kind = E_UNARY;
            e->op = TK_NOT;
            e->a = call;
            e->b = NULL;
        }
        return t_bool;
    }
    if (is_arith(op)) {
        if (type_is_number(ta) && type_is_number(tb)) {
            Type *r = join_numbers(ta, tb);
            if (op == TK_SLASH && r == t_int) r = t_real;
            e->a = mk_convert(c, e->a, r);
            e->b = mk_convert(c, e->b, r);
            return r;
        }
        if (ta->kind == T_VEC && tb->kind == T_VEC) {
            if (ta->n != tb->n) {
                ERR(c, e, "can't combine vectors of different lengths (%s and %s)", type_str(ta), type_str(tb));
                return t_error;
            }
            Type *el = join_numbers(ta->elem, tb->elem);
            if (op == TK_SLASH && el == t_int) el = t_real;
            Type *r = type_vec(el, ta->n);
            e->a = mk_convert(c, e->a, r);
            e->b = mk_convert(c, e->b, r);
            return r;
        }
        if ((ta->kind == T_VEC && type_is_number(tb)) || (tb->kind == T_VEC && type_is_number(ta))) {
            Type *vt = ta->kind == T_VEC ? ta : tb;
            Type *st = ta->kind == T_VEC ? tb : ta;
            Type *el = join_numbers(vt->elem, st);
            if (op == TK_SLASH && el == t_int) el = t_real;
            Type *r = type_vec(el, vt->n);
            if (ta->kind == T_VEC) {
                e->a = mk_convert(c, e->a, r);
                e->b = mk_convert(c, e->b, el);
            } else {
                e->a = mk_convert(c, e->a, el);
                e->b = mk_convert(c, e->b, r);
            }
            return r;
        }
        ERR(c, e, "can't use '%s' with %s and %s", op_name(op), type_str(ta), type_str(tb));
        return t_error;
    }
    if (op == TK_MOD) {
        if (!type_is_scalar_real(ta) || !type_is_scalar_real(tb)) {
            ERR(c, e, "'mod' needs integers or reals, not %s and %s", type_str(ta), type_str(tb));
            return t_error;
        }
        Type *r = join_numbers(ta, tb);
        e->a = mk_convert(c, e->a, r);
        e->b = mk_convert(c, e->b, r);
        return r;
    }
    if (op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE) {
        if (!type_is_scalar_real(ta) || !type_is_scalar_real(tb)) {
            ERR(c, e, "'%s' compares integers or reals; %s and %s can't be ordered (compare |z|, re(z) or im(z) instead)",
                op_name(op), type_str(ta), type_str(tb));
            return t_error;
        }
        Type *r = join_numbers(ta, tb);
        e->a = mk_convert(c, e->a, r);
        e->b = mk_convert(c, e->b, r);
        return t_bool;
    }
    if (op == TK_EQ || op == TK_NE) {
        if (ta == t_bool && tb == t_bool) return t_bool;
        if (type_is_number(ta) && type_is_number(tb)) {
            Type *r = join_numbers(ta, tb);
            e->a = mk_convert(c, e->a, r);
            e->b = mk_convert(c, e->b, r);
            return t_bool;
        }
        if (ta->kind == T_VEC && tb->kind == T_VEC && ta->n == tb->n) {
            Type *r = type_vec(join_numbers(ta->elem, tb->elem), ta->n);
            e->a = mk_convert(c, e->a, r);
            e->b = mk_convert(c, e->b, r);
            return t_bool;
        }
        ERR(c, e, "can't compare %s with %s", type_str(ta), type_str(tb));
        return t_error;
    }
    ERR(c, e, "unknown operator");
    return t_error;
}

static Sym *lookup_name(Checker *c, const char *name) {
    Sym *s = scope_find(c->scope, name);
    return s;
}

static Type *chk_index(Checker *c, Expr *e) {
    Type *tb = chk(c, e->a);
    Type *ti = chk(c, e->b);
    if (tb == t_error || ti == t_error) return t_error;
    if (tb == t_proj) { /* p[1], p[2]: the coordinates a and b of a lift [a : b] */
        int k;
        if (!is_int_const(e->b, &k) || (k != 1 && k != 2 && k != -1 && k != -2)) {
            ERR(c, e->b, "a point [a : b] of the sphere has two coordinates: p[1] is a and p[2] is b");
            return t_error;
        }
        bool second = k == 2 || k == -1;
        e->kind = E_CALL;
        e->name = second ? "__pb" : "__pa";
        e->intrinsic = second ? I_PB : I_PA;
        e->args = ARENA_ARRAY(&c->prog->arena, Expr *, 1);
        e->args[0] = e->a;
        e->nargs = 1;
        e->a = e->b = NULL;
        return t_complex;
    }
    e->b = coerce(c, e->b, t_int, "an index");
    switch (tb->kind) {
    case T_VEC: case T_LIST: return tb->elem;
    case T_COMPLEX: case T_QUAT: return t_real;
    default:
        ERR(c, e, "%s can't be indexed with [ ]", type_str(tb));
        return t_error;
    }
}

static Type *chk_generic_intrinsic(Checker *c, Expr *e, int id) {
    for (int i = 0; i < e->nargs; i++)
        if (chk(c, e->args[i]) == t_error) return t_error;
    e->intrinsic = id;
    if (id == I_DOT) {
        if (e->nargs != 2) { ERR(c, e, "dot needs two vectors"); return t_error; }
        Type *a = e->args[0]->type, *b = e->args[1]->type;
        if (a->kind != T_VEC || b->kind != T_VEC || a->n != b->n) {
            ERR(c, e, "dot needs two vectors of the same length, not %s and %s", type_str(a), type_str(b));
            return t_error;
        }
        Type *el = join_numbers(a->elem, b->elem);
        Type *r = type_vec(el, a->n);
        e->args[0] = mk_convert(c, e->args[0], r);
        e->args[1] = mk_convert(c, e->args[1], r);
        return el;
    }
    if (id == I_LENGTH) {
        if (e->nargs != 1) { ERR(c, e, "length takes one list or vector"); return t_error; }
        Type *a = e->args[0]->type;
        if (a->kind != T_LIST && a->kind != T_VEC) {
            ERR(c, e, "length needs a list or a vector, not %s", type_str(a));
            return t_error;
        }
        return t_int;
    }
    /* append(L, x), clear(L): L must be a list variable */
    Expr *l = e->args[0];
    if ((id == I_APPEND && e->nargs != 2) || (id == I_CLEAR && e->nargs != 1)) {
        ERR(c, e, id == I_APPEND ? "append needs a list and a value: append(L, x)" : "clear needs a list: clear(L)");
        return t_error;
    }
    if (l->type->kind != T_LIST || !(l->kind == E_NAME || l->kind == E_INTERNAL || l->kind == E_MEMBER)) {
        ERR(c, e, "the first argument of %s must be a list variable", e->name);
        return t_error;
    }
    if (l->sym && l->sym->readonly) ERR(c, e, "%s can't be changed", l->sym->name);
    if (id == I_APPEND) e->args[1] = coerce(c, e->args[1], l->type->elem, "the appended value");
    return t_void;
}

/* ---- type inference: variables declared without a type ------------------------------------------------------ */
/* what an earlier run found out about the variable declared at (line, col) (in the instance ctx of a generic
   function, 0 outside them), or NULL */
static Type *hint_at(Program *p, int line, int col, u64 ctx) {
    for (int i = 0; i < p->nhints; i++)
        if (p->hints[i].line == line && p->hints[i].col == col && p->hints[i].ctx == ctx) return p->hints[i].type;
    return NULL;
}
static Type *hint_for(Checker *c, int line, int col) { return hint_at(c->prog, line, col, c->ctx); }

static bool storable(Type *t) {
    return t && t != t_error && t != t_void && t != t_string && t->kind != T_TUPLE;
}

/* An assignment gives the untyped variable s a value of type vt that it can't hold: note that it must be wider (the
   script is checked again).  False if it can't be (s has a type, or the types don't mix), or on the last run. */
static bool widen_sym(Checker *c, Sym *s, Type *vt) {
    Program *p = c->prog;
    if (!s || !s->untyped || !storable(vt)) return false;
    if (c->probing && s->kind == SYM_GLOBAL) return false; /* (a function nothing calls changes no types) */
    Type *w = s->unknown_type ? vt : join_types(s->type, vt); /* (an output with no value yet takes any) */
    if (!w || w == s->type) return false;
    if (p->final_round) { /* (the runs ran out: a very long chain of variables assigned to each other) */
        for (int i = 0; i < p->diags.len; i++)
            if (p->diags.data[i].line == s->line && p->diags.data[i].col == s->col && strstr(p->diags.data[i].msg, "in time"))
                return true; /* (said once) */
        prog_diag(p, DIAG_ERROR, s->line, s->col, 0,
                  "'%s' is assigned %s values, but its type could not be worked out in time: declare it with its type (%s : %s = ...)",
                  s->name, type_str(vt), s->name, type_str(w));
        return true;
    }
    for (int i = 0; i < p->nhints; i++)
        if (p->hints[i].line == s->line && p->hints[i].col == s->col && p->hints[i].ctx == s->hint_ctx) {
            Type *j = join_types(p->hints[i].type, w);
            p->hints[i].type = j ? j : w;
            p->new_hints = true;
            return true;
        }
    if (p->nhints == p->cap_hints) {
        p->cap_hints = p->cap_hints ? 2 * p->cap_hints : 16;
        p->hints = (TypeHint *)xrealloc(p->hints, sizeof(TypeHint) * (size_t)p->cap_hints);
    }
    p->hints[p->nhints++] = (TypeHint){s->line, s->col, s->hint_ctx, w};
    p->new_hints = true;
    return true;
}

/* ---- generic functions: copies of a template ----------------------------------------------------------------- */
static Expr *clone_expr(Arena *a, Expr *e) {
    if (!e) return NULL;
    Expr *n = ARENA_NEW(a, Expr);
    *n = *e;
    n->a = clone_expr(a, e->a);
    n->b = clone_expr(a, e->b);
    n->cont_from = clone_expr(a, e->cont_from);
    if (e->args) {
        n->args = ARENA_ARRAY(a, Expr *, e->nargs > 0 ? e->nargs : 1);
        for (int i = 0; i < e->nargs; i++) n->args[i] = clone_expr(a, e->args[i]);
    }
    return n;
}

static Block clone_block(Arena *a, Block *b);
static Stmt *clone_stmt(Arena *a, Stmt *s) {
    Stmt *n = ARENA_NEW(a, Stmt);
    *n = *s;
    n->target = clone_expr(a, s->target);
    n->value = clone_expr(a, s->value);
    n->range_lo = clone_expr(a, s->range_lo);
    n->range_hi = clone_expr(a, s->range_hi);
    n->body = clone_block(a, &s->body);
    n->else_body = clone_block(a, &s->else_body);
    n->from = clone_expr(a, s->from);
    n->to = clone_expr(a, s->to);
    n->step = clone_expr(a, s->step);
    if (s->targets) {
        n->targets = ARENA_ARRAY(a, Expr *, s->ntargets > 0 ? s->ntargets : 1);
        for (int i = 0; i < s->ntargets; i++) n->targets[i] = clone_expr(a, s->targets[i]);
    }
    n->p0 = clone_expr(a, s->p0);
    n->p1 = clone_expr(a, s->p1);
    n->radius = clone_expr(a, s->radius);
    n->color = clone_expr(a, s->color);
    n->width = clone_expr(a, s->width);
    n->text = clone_expr(a, s->text);
    return n;
}

static Block clone_block(Arena *a, Block *b) {
    Block n = {0};
    if (b->n <= 0) return n;
    n.n = b->n;
    n.stmts = ARENA_ARRAY(a, Stmt *, b->n);
    for (int i = 0; i < b->n; i++) n.stmts[i] = clone_stmt(a, b->stmts[i]);
    return n;
}

static u64 fnv_str(u64 h, const char *s) {
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

/* an output declared without a type: the widest type assigned to it (a placeholder until something is) */
static void untyped_output(Program *p, Sym *s, u64 ctx) {
    Type *h = hint_at(p, s->line, s->col, ctx);
    s->type = h ? h : t_int;
    s->untyped = true;
    s->unknown_type = !h;
    s->hint_ctx = ctx;
}

/* output i of f, when it is a parameter (an output named as one, see parse.c): the parameter's index, or -1 */
static int output_param(const Func *f, int i) {
    for (int j = 0; j < f->nparams; j++)
        if (f->outputs[i] == f->params[j]) return j;
    return -1;
}

/* The copy of the generic function t for arguments of types at (the existing one, or a new one) */
static Func *instantiate(Checker *c, Func *t, Type **at, int call_line) {
    for (Func *g = t->instances; g; g = g->next_instance) {
        bool same = true;
        for (int i = 0; i < t->nparams && same; i++)
            if (g->arg_types[i] != at[i]) same = false;
        if (same) return g;
    }
    Arena *a = &c->prog->arena;
    Func *f = ARENA_NEW(a, Func);
    *f = *t;
    f->generic = false;
    f->tmpl = t;
    f->instances = f->next_instance = NULL;
    f->next_overload = NULL;
    f->locals = (SymVec){0};
    f->check_state = 0;
    f->failed = f->uses_draw = f->spv_emitted = f->probe = false;
    f->ncalls = 0;
    f->ret = NULL;
    f->spv_fn = 0;
    f->call_line = call_line;
    f->arg_types = ARENA_ARRAY(a, Type *, t->nparams + 1);
    u64 h = fnv_str(1469598103934665603ull, t->name);
    for (int i = 0; i < t->nparams; i++) {
        f->arg_types[i] = at[i];
        h = fnv_str(fnv_str(h, ","), type_str(at[i]));
    }
    f->ctx = h ? h : 1;
    f->params = ARENA_ARRAY(a, Sym *, t->nparams + 1);
    for (int i = 0; i < t->nparams; i++) {
        Sym *s = ARENA_NEW(a, Sym);
        *s = *t->params[i];
        s->owner = f;
        s->hint_ctx = f->ctx;
        if (!t->param_tsyn[i]) { /* the argument's type, or wider if the function assigns wider values to it */
            Type *hh = hint_at(c->prog, s->line, s->col, f->ctx), *j = hh ? join_types(at[i], hh) : NULL;
            s->type = j ? j : at[i];
            s->untyped = true;
        }
        f->params[i] = s;
    }
    f->outputs = ARENA_ARRAY(a, Sym *, t->noutputs + 1);
    for (int i = 0; i < t->noutputs; i++) {
        int j = output_param(t, i);
        if (j >= 0) { /* (the instance's parameter) */
            f->outputs[i] = f->params[j];
            continue;
        }
        Sym *s = ARENA_NEW(a, Sym);
        *s = *t->outputs[i];
        s->owner = f;
        s->hint_ctx = f->ctx;
        if (!t->output_tsyn[i]) untyped_output(c->prog, s, f->ctx);
        f->outputs[i] = s;
    }
    f->body_expr = clone_expr(a, t->body_expr);
    f->body = clone_block(a, &t->body);
    Func **pp = &t->instances;
    while (*pp) pp = &(*pp)->next_instance;
    *pp = f;
    vec_push(c->prog->funcs, f);
    return f;
}

/* "f(z : C, n : Z)": an instance as the messages show it */
static void instance_name(Func *f, StrBuf *sb) {
    sb_printf(sb, "%s(", f->name);
    for (int i = 0; i < f->nparams; i++) {
        sb_printf(sb, "%s%s : ", i ? ", " : "", f->params[i]->name);
        type_name(f->params[i]->type, sb);
    }
    sb_putc(sb, ')');
}

static Type *chk_call(Checker *c, Expr *e) {
    const char *name = e->name;
    if (name && !strcmp(name, "palette") && e->nargs == 2 && e->args[0]->kind == E_STRING) {
        /* palette("fire", t): the color map of that name */
        static const char *maps[] = {"rainbow", "arenberg", "romao", "bamo", "broco", "corko", "viko", "ice", "fire",
                                     "wheat", "rose", "forest", "ocean", "winter", "spring", "summer", "fall"};
        const char *m = e->args[0]->name;
        int k = 0;
        while (k < ARRAY_COUNT(maps) && strcmp(maps[k], m) != 0) k++;
        if (k == ARRAY_COUNT(maps)) {
            ERR(c, e->args[0], "there is no palette \"%s\"; the palettes are rainbow, arenberg, romao, bamo, broco, "
                               "corko, viko, ice, fire, wheat, rose, forest, ocean, winter, spring, summer and fall", m);
            return t_error;
        }
        e->name = arena_printf(&c->prog->arena, "__palette_%s", m);
        name = e->name;
        e->args[0] = e->args[1];
        e->nargs = 1;
    }
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++)
        if (strcmp(intrinsics[k].name, name) == 0 && intrinsics[k].params[0] == '*')
            return chk_generic_intrinsic(c, e, intrinsics[k].id);
    for (int i = 0; i < e->nargs; i++) {
        if (e->args[i]->kind == E_TUPLE) {
            ERR(c, e->args[i], "a list of values in parentheses can't be passed as one argument");
            return t_error;
        }
        if (chk(c, e->args[i]) == t_error) return t_error;
    }
    /* candidates: intrinsics, then functions */
    int best_cost = 1 << 30, nbest = 0, best_intr = -1;
    Func *best_fn = NULL;
    bool any_name = false;
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++) {
        const IntrinsicInfo *in = &intrinsics[k];
        if (strcmp(in->name, name) != 0) continue;
        any_name = true;
        if (in->nparams != e->nargs) continue;
        int cost = 0;
        for (int i = 0; i < e->nargs && cost >= 0; i++) {
            char pc = in->params[i];
            Type *pt = intrinsic_type(pc);
            int ci = conv_cost(e->args[i]->type, pt);
            cost = ci < 0 ? -1 : cost + ci;
        }
        if (cost < 0) continue;
        cost *= 2; /* (see below) */
        if (cost < best_cost) { best_cost = cost; nbest = 1; best_intr = k; best_fn = NULL; }
        else if (cost == best_cost) nbest++;
    }
    Func *head = NULL;
    for (int i = 0; i < c->prog->funcs.len; i++)
        if (strcmp(c->prog->funcs.data[i]->name, name) == 0) { head = c->prog->funcs.data[i]; break; }
    bool in_prelude = c->fn && c->fn->prelude;
    for (Func *f = head; f; f = f->next_overload) {
        if (in_prelude ? !f->prelude : f->shadowed) continue; /* built-ins only call built-ins */
        any_name = true;
        if (f->nparams != e->nargs) continue;
        int cost = 0;
        for (int i = 0; i < e->nargs && cost >= 0; i++) {
            int ci = f->generic && !f->param_tsyn[i] ? (storable(e->args[i]->type) ? 0 : -1) /* (takes any value) */
                                                     : conv_cost(e->args[i]->type, f->params[i]->type);
            cost = ci < 0 ? -1 : cost + ci;
        }
        if (cost < 0) continue;
        cost = 2 * cost + (f->generic ? 1 : 0); /* when a generic function fits as well, the one with types wins */
        if (cost < best_cost) { best_cost = cost; nbest = 1; best_fn = f; best_intr = -1; }
        else if (cost == best_cost) nbest++;
    }
    if (!any_name) {
        ERR(c, e, "unknown function '%s'", name);
        return t_error;
    }
    if (nbest == 0) {
        /* a single candidate of the right arity: point at the bad argument */
        Func *only = NULL;
        int ncand = 0;
        for (Func *f = head; f; f = f->next_overload)
            if (f->nparams == e->nargs) { only = f; ncand++; }
        if (ncand == 1) {
            for (int i = 0; i < e->nargs; i++) {
                if (only->generic && !only->param_tsyn[i]) {
                    if (storable(e->args[i]->type)) continue;
                    ERR(c, e->args[i], "argument %d of '%s' must be a value, but this is %s", i + 1, name,
                        type_str(e->args[i]->type));
                    return t_error;
                }
                if (conv_cost(e->args[i]->type, only->params[i]->type) < 0) {
                    ERR(c, e->args[i], "argument %d of '%s' must be %s, but this is %s%s", i + 1, name,
                        type_str(only->params[i]->type), type_str(e->args[i]->type),
                        narrowing_hint(e->args[i]->type, only->params[i]->type));
                    return t_error;
                }
            }
        }
        StrBuf sb = {0};
        for (int i = 0; i < e->nargs; i++) {
            if (i) sb_puts(&sb, ", ");
            type_name(e->args[i]->type, &sb);
        }
        StrBuf alts = {0};
        int nalt = 0;
        for (Func *f = head; f && nalt < 6; f = f->next_overload, nalt++) {
            sb_puts(&alts, nalt ? ", " : "");
            sb_printf(&alts, "%s(", name);
            for (int i = 0; i < f->nparams; i++) {
                if (i) sb_puts(&alts, ", ");
                if (f->generic && !f->param_tsyn[i]) sb_puts(&alts, "any");
                else type_name(f->params[i]->type, &alts);
            }
            sb_putc(&alts, ')');
        }
        bool points = false;
        for (int i = 0; i < e->nargs; i++)
            if (e->args[i]->type == t_proj) points = true;
        ERR(c, e, "'%s' can't be applied to (%s)%s%s%s", name, sb_cstr(&sb), nalt ? "; it accepts " : "", sb_cstr(&alts),
            points ? " (for a point p of the sphere, complex(p) is its complex number)" : "");
        sb_free(&sb);
        sb_free(&alts);
        return t_error;
    }
    if (nbest > 1) {
        ERR(c, e, "the call to '%s' is ambiguous; add a type to the arguments", name);
        return t_error;
    }
    if (best_intr >= 0) {
        const IntrinsicInfo *in = &intrinsics[best_intr];
        e->intrinsic = in->id;
        for (int i = 0; i < e->nargs; i++) {
            char pc = in->params[i];
            Type *pt = intrinsic_type(pc);
            e->args[i] = mk_convert(c, e->args[i], pt);
        }
        char r = in->ret;
        return intrinsic_type(r);
    }
    Func *f = best_fn;
    if (f->generic) { /* the copy of the function for these argument types */
        for (Func *g = f->instances; g; g = g->next_instance)
            if (g->check_state == 1) {
                ERR(c, e, "'%s' calls itself; recursive functions are not supported", name);
                return t_error;
            }
        Type *at[64];
        if (e->nargs > 64) {
            ERR(c, e, "too many arguments");
            return t_error;
        }
        for (int i = 0; i < e->nargs; i++) at[i] = f->param_tsyn[i] ? f->params[i]->type : e->args[i]->type;
        f = instantiate(c, f, at, e->line);
    }
    for (int i = 0; i < e->nargs; i++) e->args[i] = mk_convert(c, e->args[i], f->params[i]->type);
    e->fn = f;
    if (f->check_state == 1) {
        ERR(c, e, "'%s' calls itself; recursive functions are not supported", name);
        return t_error;
    }
    if (f->check_state == 0) check_func(c, f);
    if (f->failed) return t_error;
    if (f->uses_draw && c->fn) c->fn->uses_draw = true;
    if (f->uses_draw && c->sec && c->sec->kind == SEC_VIEWER && !c->fn) {
        ERR(c, e, "'%s' draws, and viewers can't draw (only tools can)", name);
        return t_error;
    }
    f->ncalls++;
    return f->ret;
}

/* `derivative`, `root` and `critical` are handled in autodiff.c */
static Type *chk_solve(Checker *c, Expr *e);

static Type *chk(Checker *c, Expr *e) {
    Type *t = t_error;
    switch (e->kind) {
    case E_INT: t = t_int; break;
    case E_REAL: t = t_real; break;
    case E_IMAG: t = t_complex; break;
    case E_BOOL: t = t_bool; break;
    case E_STRING:
        ERR(c, e, "text in quotes can only be used for .title");
        break;
    case E_NAME: {
        Sym *s = lookup_name(c, e->name);
        if (!s) {
            if (c->sec && c->sec->kind != SEC_VIEWER && c->prog->nviewers && !c->fn) {
                for (int v = 0; v < c->prog->nviewers; v++)
                    if (c->prog->viewers[v]->var && strcmp(c->prog->viewers[v]->var, e->name) == 0) {
                        ERR(c, e, "'%s' is only defined inside the viewer '%s'; use .%s for the point picked in it",
                            e->name, c->prog->viewers[v]->name, e->name);
                        return e->type = t_error;
                    }
            }
            ERR(c, e, "unknown name '%s'", e->name);
            break;
        }
        e->sym = s;
        t = s->type;
        break;
    }
    case E_INTERNAL: {
        Sym *s = scope_find_local(&c->internals, e->name);
        if (!s) {
            ERR(c, e, "unknown internal '.%s' (see the list of internals in the language guide, e.g. .maxIterations, .epsilon, .escapeRadius, .lastClick, .red)", e->name);
            break;
        }
        if (s->type == t_string) {
            ERR(c, e, ".%s can only be set, not read", e->name);
            break;
        }
        e->sym = s;
        t = s->type;
        break;
    }
    case E_MEMBER: {
        Section *v = find_viewer(c->prog, e->base_name);
        if (!v) {
            ERR(c, e, "'%s' is not a viewer name", e->base_name);
            break;
        }
        if (strcmp(e->name, "center") == 0) e->sym = v->center;
        else if (strcmp(e->name, "radius") == 0) e->sym = v->radius;
        else if (strcmp(e->name, "conformal") == 0 && v->conformal) e->sym = v->conformal;
        else {
            ERR(c, e, "viewers have .center and .radius%s, not .%s", v->conformal ? " (and .conformal)" : "", e->name);
            break;
        }
        t = e->sym->type;
        break;
    }
    case E_UNARY: {
        Type *ta = chk(c, e->a);
        if (ta == t_error) break;
        if (e->op == TK_NOT) {
            e->a = coerce(c, e->a, t_bool, "the operand of 'not'");
            t = t_bool;
        } else if (type_is_number(ta) || ta->kind == T_VEC || ta == t_proj) {
            t = ta;
        } else {
            ERR(c, e, "can't negate %s", type_str(ta));
        }
        break;
    }
    case E_BINARY: t = chk_binary(c, e); break;
    case E_ABS: {
        Type *ta = chk(c, e->a);
        if (ta == t_error) break;
        if (ta == t_proj) { /* |p| = |a| / |b| */
            mk_call(c, e, "__pabs", e->a, NULL);
            t = chk_call(c, e);
            break;
        }
        if (ta == t_int || ta == t_real) t = ta;
        else if (ta == t_complex || ta == t_quat || ta->kind == T_VEC) t = t_real;
        else if (ta->kind == T_LIST) t = t_int;
        else ERR(c, e, "|...| needs a number, vector or list, not %s", type_str(ta));
        break;
    }
    case E_PRED: {
        Type *ta = chk(c, e->a);
        if (ta == t_error) break;
        if (ta == t_proj) { /* |a| > .escapeRadius |b|, or close to 0 on the sphere */
            mk_call(c, e, e->op == TK_ESCAPES ? "__pescapes" : "__pvanishes", e->a, NULL);
            t = chk_call(c, e);
            break;
        }
        if (!type_is_number(ta) && ta->kind != T_VEC)
            ERR(c, e, "'%s' needs a number or a vector, not %s", e->op == TK_ESCAPES ? "escapes" : "vanishes", type_str(ta));
        else
            t = t_bool;
        break;
    }
    case E_CALL: t = chk_call(c, e); break;
    case E_INDEX: t = chk_index(c, e); break;
    case E_VECTOR: {
        if (e->op == TK_COLON) { /* [a : b]: a point of the Riemann sphere */
            Type *ta = chk(c, e->args[0]), *tb = chk(c, e->args[1]);
            if (ta == t_error || tb == t_error) break;
            e->args[0] = coerce(c, e->args[0], t_complex, "a coordinate in [a : b]");
            e->args[1] = coerce(c, e->args[1], t_complex, "a coordinate in [a : b]");
            e->kind = E_CALL;
            e->name = "__pmake";
            e->intrinsic = I_PMAKE;
            e->op = 0;
            t = t_proj;
            break;
        }
        if (e->nargs == 0) {
            ERR(c, e, "an empty [] is only allowed as the value of a List");
            break;
        }
        Type *el = t_int;
        bool ok = true, points = false;
        for (int i = 0; i < e->nargs; i++) {
            Type *ti = chk(c, e->args[i]);
            if (ti == t_error) { ok = false; continue; }
            if (ti == t_proj) points = true;
        }
        if (ok && points) { /* [0, ∞, 1 + i]: a list of points of the sphere */
            for (int i = 0; i < e->nargs; i++) e->args[i] = coerce(c, e->args[i], t_proj, "an item of a list of points");
            t = type_list(t_proj, FS_MAX(e->nargs, 64));
            break;
        }
        for (int i = 0; i < e->nargs && ok; i++) {
            Type *ti = e->args[i]->type;
            if (!type_is_number(ti)) {
                ERR(c, e->args[i], "vector entries must be numbers, not %s", type_str(ti));
                ok = false;
                continue;
            }
            el = join_numbers(el, ti);
        }
        if (!ok) break;
        for (int i = 0; i < e->nargs; i++) e->args[i] = mk_convert(c, e->args[i], el);
        t = type_vec(el, e->nargs);
        break;
    }
    case E_TUPLE:
        ERR(c, e, "a list of values in parentheses can only be the result of a function");
        break;
    case E_POWI: case E_CONVERT: case E_COLOR_CONST:
        t = e->type;
        break;
    case E_SOLVE: t = chk_solve(c, e); break;
    case E_COND: {
        Type *tc = chk(c, e->args[0]);
        Type *ta = chk(c, e->args[1]), *tb = chk(c, e->args[2]);
        if (tc == t_error || ta == t_error || tb == t_error) break;
        e->args[0] = coerce(c, e->args[0], t_bool, "the condition of 'if'");
        if (type_is_number(ta) && type_is_number(tb)) t = join_numbers(ta, tb);
        else if ((ta == t_proj || tb == t_proj) && join_types(ta, tb) == t_proj) t = t_proj;
        else if (ta == tb && ta != t_void && ta->kind != T_TUPLE) t = ta;
        else if (ta->kind == T_VEC && tb->kind == T_VEC && ta->n == tb->n) t = type_vec(join_numbers(ta->elem, tb->elem), ta->n);
        else {
            ERR(c, e, "the two values of this 'if' are %s and %s, which don't mix", type_str(ta), type_str(tb));
            break;
        }
        e->args[1] = coerce(c, e->args[1], t, "the value after 'then'");
        e->args[2] = coerce(c, e->args[2], t, "the value after 'else'");
        break;
    }
    }
    e->type = t;
    return t;
}

/* ---- "did you mean ...?" ------------------------------------------------------------------------------------- */
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int sub = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            d[i][j] = FS_MIN(sub, FS_MIN(d[i - 1][j] + 1, d[i][j - 1] + 1));
        }
    return d[la][lb];
}

static bool same_ignoring_case(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

/* the variable visible here whose name is closest to `name` (a typo), or NULL */
static const char *closest_name(Checker *c, const char *name) {
    const char *best = NULL;
    int len = (int)strlen(name), bestd = len <= 4 ? 2 : 3;
    for (Scope *s = c->scope; s; s = s->parent)
        for (int i = 0; i < s->syms.len; i++) {
            Sym *x = s->syms.data[i];
            if (x->name[0] == '_' || x->name[0] == '.' || x->kind == SYM_CONST) continue;
            int xl = (int)strlen(x->name), d;
            if (same_ignoring_case(name, x->name)) d = 0;
            else if (len > 1 && xl > 1) d = edit_distance(name, x->name);
            else if (xl == 1 && len == 2 && strchr(name, x->name[0])) d = 1; /* (wx for w; but not n for c) */
            else continue;
            if (d < bestd) {
                bestd = d;
                best = x->name;
            }
        }
    return best;
}

/* ---- statements -------------------------------------------------------- */
static void chk_block(Checker *c, Block *b);

static Sym *declare(Checker *c, Expr *at, const char *name, Type *t) {
    if (strcmp(name, "i") == 0) {
        ERR(c, at, "'i' is the imaginary unit and can't be used as a variable name");
        return NULL;
    }
    Scope *sc = c->load && !c->fn ? &c->globals : c->scope;
    Sym *old = scope_find_local(sc, name);
    if (old) {
        if (old->type != t && t != t_error && old->type != t_error) {
            ERR(c, at, "'%s' was already declared as %s on line %d", name, type_str(old->type), old->line);
            return NULL;
        }
        return old;
    }
    if (c->load && !c->fn) {
        Sym *s = new_sym(c, name, t, SYM_GLOBAL, at->line, at->col);
        s->is_config = c->sec && c->sec->kind == SEC_CONFIG;
        vec_push(sc->syms, s);
        if (t->kind == T_TUPLE || t == t_void || t == t_string) {
            ERR(c, at, "'%s' can't hold %s", name, type_str(t));
            return s;
        }
        vec_push(c->prog->globals, s);
        return s;
    }
    Sym *s = new_sym(c, name, t, SYM_LOCAL, at->line, at->col);
    s->owner = c->fn;
    vec_push(sc->syms, s);
    vec_push(*c->locals, s);
    return s;
}

static bool const_number(Expr *e, double *out) {
    if (e->kind == E_INT || e->kind == E_REAL) { *out = e->rval; return true; }
    if (e->kind == E_UNARY && e->op == TK_MINUS && (e->a->kind == E_INT || e->a->kind == E_REAL)) {
        *out = -e->a->rval;
        return true;
    }
    return false;
}

static void chk_range(Checker *c, Stmt *s, Sym *sym) {
    if (!s->range_lo) return;
    double lo, hi;
    if (!c->sec || c->sec->kind != SEC_CONFIG || c->fn) {
        ERR(c, s->range_lo, "a slider range 'in a..b' is only allowed in @config");
        return;
    }
    if (!const_number(s->range_lo, &lo) || !const_number(s->range_hi, &hi)) {
        ERR(c, s->range_lo, "the slider range must be written with plain numbers, as in: in 0..1");
        return;
    }
    if (!sym || !type_is_scalar_real(sym->type)) {
        ERR(c, s->range_lo, "only Z and R variables can have a slider range");
        return;
    }
    if (sym->type == t_int) { /* x := 1 in 0.1..10 is a real number (the range is written with real numbers) */
        bool real = false;
        Expr *ends[2] = {s->range_lo, s->range_hi};
        for (int k = 0; k < 2; k++) {
            Expr *e = ends[k]->kind == E_UNARY ? ends[k]->a : ends[k];
            if (e->kind == E_REAL) real = true;
        }
        if (real && widen_sym(c, sym, t_real)) return;
        if (lo != floor(lo) || hi != floor(hi)) {
            ERR(c, s->range_lo, "'%s' is an integer, so its slider range must be integers (or declare %s : R = ...)",
                sym->name, sym->name);
            return;
        }
    }
    sym->has_range = true;
    sym->range_lo = lo;
    sym->range_hi = hi;
}

/* is e a text, or a sum with a text in it ("period " + n)? */
static bool is_text(Expr *e) {
    if (!e) return false;
    if (e->kind == E_STRING) return true;
    return e->kind == E_BINARY && e->op == TK_PLUS && (is_text(e->a) || is_text(e->b));
}

/* a shortcut like "Shift + p" in a comparable form ("SHIFT+P"); false if there is none */
static bool shortcut_key(const char *sc, char *out, int cap) {
    int n = 0;
    for (const char *q = sc; q && *q && n < cap - 1; q++)
        if (*q != ' ') out[n++] = (char)toupper((unsigned char)*q);
    out[n] = 0;
    return n > 0;
}

/* the text of `draw text`: texts in quotes and numbers joined with + */
static void chk_text(Checker *c, Expr *e) {
    if (e->kind == E_STRING) {
        e->type = t_string;
        return;
    }
    if (e->kind == E_BINARY && e->op == TK_PLUS && (is_text(e->a) || is_text(e->b))) {
        chk_text(c, e->a);
        chk_text(c, e->b);
        e->type = t_string;
        return;
    }
    Type *t = chk(c, e);
    if (t != t_error && !type_is_number(t) && t != t_bool && t != t_proj && t->kind != T_VEC && t->kind != T_LIST)
        ERR(c, e, "only texts in quotes, numbers, points, vectors and lists can be written (joined with +), not %s", type_str(t));
}

static void config_add_var(Checker *c, Sym *sym) {
    for (int i = 0; i < c->prog->config.len; i++)
        if (c->prog->config.data[i].kind == CFG_VAR && c->prog->config.data[i].sym == sym) return;
    ConfigItem it = {CFG_VAR, NULL, sym};
    vec_push(c->prog->config, it);
}

/* The slider range, choices and label of a declaration or assignment, and its place in the config panel:
   variables declared in @config, and any variable given a range, choices or a label there. */
static void chk_config_extras(Checker *c, Stmt *s, Sym *sym, bool declared) {
    bool extras = s->range_lo || s->label || s->choices;
    bool in_config = c->sec && c->sec->kind == SEC_CONFIG && !c->fn;
    if (extras && !in_config) {
        ERR(c, s->range_lo ? s->range_lo : s->target, "slider ranges, choices and labels belong in @config");
        return;
    }
    if (!sym || !in_config || sym->kind != SYM_GLOBAL || sym->type == t_error) return;
    if (s->range_lo) chk_range(c, s, sym);
    if (s->choices) {
        if (sym->type != t_int) {
            ERR(c, s->target, "a choice needs an integer variable: its value is the number of the choice, as in "
                              "scheme : Z = 1 in [\"rainbow\", \"grays\"]");
            return;
        }
        sym->choices = s->choices;
        sym->nchoices = s->nchoices;
        sym->has_range = true;
        sym->range_lo = 1;
        sym->range_hi = s->nchoices;
    }
    if (s->label) sym->label = s->label;
    if ((declared && sym->is_config) || extras) config_add_var(c, sym);
}

static void check_assign_target(Checker *c, Expr *t) {
    Sym *s = t->sym;
    if (!s) return;
    bool tool = c->sec && c->sec->kind == SEC_TOOL && !c->fn;
    if (s == c->prog->s_time && !tool) /* (a tool setting .time moves the clock there) */
        ERR(c, t, ".time can't be changed here; a tool can set it (the clock goes there)");
    else if ((s->readonly && s != c->prog->s_time) || s->kind == SYM_CONST)
        ERR(c, t, "%s%s can't be changed", s->is_internal ? "." : "", s->name);
    else if (s->kind == SYM_EXEC) ERR(c, t, ".%s is set by loops and can't be assigned", s->name);
}

static void chk_stmt(Checker *c, Stmt *s) {
    switch (s->kind) {
    case S_DEFINE:
        /* a one-line function in @config is a formula you can edit in the config panel */
        if (c->sec && c->sec->kind == SEC_CONFIG && !c->fn && s->fn && s->fn->body_expr && s->fn->src_start >= 0) {
            ConfigItem it = {CFG_FORMULA, NULL, NULL, s->fn};
            vec_push(c->prog->config, it);
        }
        break;
    case S_EXPR:
        if (s->value && s->value->kind == E_STRING) {
            if (c->sec && c->sec->kind == SEC_CONFIG && !c->fn) { /* a note in the config panel */
                ConfigItem it = {CFG_NOTE, s->value->name, NULL};
                vec_push(c->prog->config, it);
                s->value = NULL; /* nothing to run */
            } else {
                ERR(c, s->value, "a line of text only means something in @config, where it is shown in the config panel");
            }
            break;
        }
        if (s->value) chk(c, s->value);
        break;
    case S_DECL: {
        Expr *tg = s->target;
        if (tg->kind != E_NAME) { /* internal or viewer property: treat as an assignment */
            s->kind = S_ASSIGN;
            s->op = TK_ASSIGN;
            if (!s->value) {
                ERR(c, tg, "give a value here, as in %s = ...", tg->name);
                return;
            }
            chk_stmt(c, s);
            return;
        }
        Type *t;
        if (s->tsyn) {
            t = resolve_tsyn(c, s->tsyn);
            if (s->value) {
                if (s->value->kind == E_VECTOR && s->value->nargs == 0 && t->kind == T_LIST) s->value->type = t;
                else if (chk(c, s->value) != t_error) s->value = coerce(c, s->value, t, "the value");
            }
        } else {
            Type *vt = chk(c, s->value);
            if (vt == t_error) { declare(c, tg, tg->name, t_error); return; }
            if (vt == t_void) { ERR(c, s->value, "this doesn't produce a value"); return; }
            t = infer_decl_type(vt, s->value);
            Type *h = hint_for(c, tg->line, tg->col), *j = h ? join_types(t, h) : NULL; /* (assigned wider values) */
            if (j) t = j;
            Sym *old = scope_find_local(c->load && !c->fn ? &c->globals : c->scope, tg->name);
            if (old && old->untyped && old->type != t && old->type != t_error) { /* declared again (t := 0 in one
                                                                                   branch, t := 0.5 in the other) */
                if (conv_cost(t, old->type) >= 0) {
                    t = old->type;
                } else if (widen_sym(c, old, t)) { /* (checked again with the wider type) */
                    s->sym = tg->sym = old;
                    tg->type = old->type;
                    return;
                }
            }
            s->value = coerce(c, s->value, t, "the value");
        }
        Sym *sym = declare(c, tg, tg->name, t);
        if (sym && !s->tsyn && sym->line == tg->line && sym->col == tg->col) sym->untyped = true;
        s->sym = sym;
        tg->sym = sym;
        tg->type = t;
        chk_config_extras(c, s, sym, true);
        break;
    }
    case S_ASSIGN: {
        Expr *tg = s->target;
        if (tg->kind == E_INTERNAL && strcmp(tg->name, "title") == 0) {
            if (!s->value || s->value->kind != E_STRING || !c->load || c->fn) {
                ERR(c, tg, ".title must be set to text in quotes in @config, as in .title = \"Julia sets\"");
                return;
            }
            c->prog->title = s->value->name;
            s->kind = S_EXPR;
            s->value = NULL;
            return;
        }
        if (tg->kind == E_INTERNAL && (!strcmp(tg->name, "panning") || !strcmp(tg->name, "hover") || !strcmp(tg->name, "keep"))) {
            ERR(c, tg, ".%s is a tool setting; set it with a plain true/false at the top of a @tool section", tg->name);
            return;
        }
        if (tg->kind == E_NAME) {
            Sym *sym = lookup_name(c, tg->name);
            bool constant = sym && sym->kind == SYM_CONST && scope_find_local(&c->builtin, tg->name) == sym;
            if (constant) sym = NULL; /* (pi, e: a variable of the script may shadow them, but they can't change) */
            if (!sym) { /* (a typo would otherwise make a new variable) */
                const char *guess = closest_name(c, tg->name); /* (not `near`: a macro on Windows) */
                Type *vt = chk(c, s->value);
                if (constant)
                    ERR(c, tg, "'%s' is a constant and can't be changed (a variable of your own may have its name: %s := ...)",
                        tg->name, tg->name);
                else if (guess)
                    ERR(c, tg, "'%s' isn't declared: did you mean '%s'? (A new variable is declared with %s := ...)",
                        tg->name, guess, tg->name);
                else if (vt != t_error && vt != t_void && s->op == TK_ASSIGN)
                    ERR(c, tg, "'%s' isn't declared: declare it with %s := ... (or with its type: %s : %s = ...)",
                        tg->name, tg->name, tg->name, type_str(infer_decl_type(vt, s->value)));
                else
                    ERR(c, tg, "'%s' isn't declared: declare it with %s := ...", tg->name, tg->name);
                declare(c, tg, tg->name, t_error); /* (no more errors about it) */
                return;
            }
        }
        if (tg->kind == E_INDEX) {
            Type *at = chk(c, tg->a);
            if (at == t_error) return; /* (said already) */
            if (at == t_proj) {
                ERR(c, tg, "the coordinates of a point of the sphere can't be set one by one; write p = [a : b]");
                return;
            }
        }
        Type *tt = chk(c, tg);
        if (tt == t_error) return;
        Expr *base = tg;
        while (base->kind == E_INDEX) base = base->a;
        check_assign_target(c, base);
        if (s->op != TK_ASSIGN) { /* x += v  ->  x = x + v */
            Expr *bin = ARENA_NEW(&c->prog->arena, Expr);
            *bin = *s->value;
            bin->kind = E_BINARY;
            bin->op = s->op == TK_PLUSEQ ? TK_PLUS : s->op == TK_MINUSEQ ? TK_MINUS : s->op == TK_STAREQ ? TK_STAR : TK_SLASH;
            Expr *lhs = ARENA_NEW(&c->prog->arena, Expr);
            *lhs = *tg;
            bin->a = lhs;
            bin->b = s->value;
            bin->type = NULL;
            bin->paren = false;
            bin->args = NULL;
            bin->nargs = 0;
            bin->name = NULL;
            bin->sym = NULL;
            bin->fn = NULL;
            bin->intrinsic = 0;
            s->value = bin;
            s->op = TK_ASSIGN;
        }
        Type *vt = chk(c, s->value);
        if (vt == t_error) return;
        if (conv_cost(vt, tt) < 0) { /* an untyped variable gets wider (the script is checked again) */
            if (tg == base && widen_sym(c, base->sym, vt)) return;
            if (tg->kind == E_INDEX && tg->a == base && base->sym && base->sym->type->kind == T_VEC) {
                Type *el = join_types(base->sym->type->elem, vt);
                if (el && type_is_number(el) && widen_sym(c, base->sym, type_vec(el, base->sym->type->n))) return;
            }
        }
        s->value = coerce(c, s->value, tt, "the new value");
        if (tg == base && base->sym) base->sym->unknown_type = false; /* (an output has its type now) */
        if (s->range_lo || s->label || s->choices) {
            if (tg != base || !base->sym) ERR(c, tg, "only a whole variable can have a slider, choices or a label");
            else chk_config_extras(c, s, base->sym, false);
        }
        break;
    }
    case S_DESTRUCTURE: {
        Expr *v = s->value;
        if (v->kind == E_TUPLE) {
            /* (x, y) = (x^2 + c - a y, x): every value is computed before any is assigned.
               Becomes `if true then tmp1 = e1; tmp2 = e2; x = tmp1; y = tmp2 end`. */
            if (v->nargs != s->ntargets) {
                ERR(c, v, "%d names but %d values", s->ntargets, v->nargs);
                return;
            }
            int n = s->ntargets;
            Stmt **st = ARENA_ARRAY(&c->prog->arena, Stmt *, 2 * n);
            for (int i = 0; i < n; i++) {
                Type *it = chk(c, v->args[i]);
                if (it == t_error) return;
                if (it == t_void || it->kind == T_TUPLE) {
                    ERR(c, v->args[i], "this doesn't produce a single value");
                    return;
                }
                if (s->declare) it = infer_decl_type(it, v->args[i]); /* (x, y) := (0, 1) declares complex numbers */
                v->args[i] = coerce(c, v->args[i], it, "the value");
                Sym *tmp = new_sym(c, arena_printf(&c->prog->arena, "__pa%d", i + 1), it, SYM_LOCAL, s->line, s->col);
                tmp->owner = c->fn;
                vec_push(*c->locals, tmp);
                vec_push(c->scope->syms, tmp); /* so the assignments below can refer to it */
                Expr *tn = ARENA_NEW(&c->prog->arena, Expr);
                *tn = *v->args[i];
                tn->kind = E_NAME;
                tn->name = tmp->name;
                tn->sym = tmp;
                tn->type = it;
                tn->a = tn->b = NULL;
                tn->nargs = 0;
                tn->args = NULL;
                tn->fn = NULL;
                tn->intrinsic = 0;
                Stmt *a = ARENA_NEW(&c->prog->arena, Stmt);
                a->kind = S_ASSIGN;
                a->op = TK_ASSIGN;
                a->line = s->line;
                a->col = s->col;
                a->target = tn;
                a->value = v->args[i];
                st[i] = a;
                Expr *tv = ARENA_NEW(&c->prog->arena, Expr);
                *tv = *tn;
                Stmt *b = ARENA_NEW(&c->prog->arena, Stmt);
                b->kind = s->declare ? S_DECL : S_ASSIGN;
                b->op = TK_ASSIGN;
                b->line = s->line;
                b->col = s->col;
                b->target = s->targets[i];
                b->value = tv;
                st[n + i] = b;
            }
            for (int i = 0; i < n; i++) chk_stmt(c, st[n + i]);
            Expr *tru = ARENA_NEW(&c->prog->arena, Expr);
            *tru = *v;
            tru->kind = E_BOOL;
            tru->ival = 1;
            tru->type = t_bool;
            tru->nargs = 0;
            tru->args = NULL;
            s->kind = S_IF;
            s->value = tru;
            s->body.stmts = st;
            s->body.n = 2 * n;
            s->else_body.n = 0;
            return;
        }
        if (v->kind != E_CALL) {
            ERR(c, v, "only a function returning several values, or several values in parentheses, can be unpacked like this");
            return;
        }
        Type *vt = chk(c, v);
        if (vt == t_error) return;
        if (vt->kind != T_TUPLE || vt->n != s->ntargets) {
            ERR(c, v, "this gives %s, which can't be unpacked into %d names", type_str(vt), s->ntargets);
            return;
        }
        for (int i = 0; i < s->ntargets; i++) {
            Expr *tg = s->targets[i];
            Type *it = vt->items[i];
            if (tg->kind == E_NAME) {
                Sym *sym = s->declare ? NULL : lookup_name(c, tg->name);
                if (sym && sym->kind == SYM_CONST && scope_find_local(&c->builtin, tg->name) == sym) sym = NULL;
                if (!sym && !s->declare) {
                    ERR(c, tg, "'%s' isn't declared: declare the names with (...) := ...", tg->name);
                    return;
                }
                if (!sym) {
                    Type *dt = infer_decl_type(it, v), *h = hint_for(c, tg->line, tg->col), *j = h ? join_types(dt, h) : NULL;
                    if (j) dt = j;
                    Sym *old = scope_find_local(c->load && !c->fn ? &c->globals : c->scope, tg->name);
                    if (old && old->untyped && old->type != dt && old->type != t_error) { /* declared again: as in S_DECL */
                        if (conv_cost(dt, old->type) >= 0) dt = old->type;
                        else if (widen_sym(c, old, dt)) sym = old; /* (checked again with the wider type) */
                    }
                    if (!sym) sym = declare(c, tg, tg->name, dt); /* (n, ok) := f(x) keeps f's types */
                    if (!sym) return;
                    if (sym->line == tg->line && sym->col == tg->col) sym->untyped = true;
                }
                tg->sym = sym;
                tg->type = sym->type;
            } else if (chk(c, tg) == t_error) {
                return;
            }
            Expr *base = tg;
            while (base->kind == E_INDEX) base = base->a;
            check_assign_target(c, base);
            if (conv_cost(it, tg->type) < 0) { /* an untyped variable gets wider, as in S_ASSIGN */
                if (tg == base && widen_sym(c, base->sym, it)) continue;
                if (tg->kind == E_INDEX && tg->a == base && base->sym && base->sym->type->kind == T_VEC) {
                    Type *el = join_types(base->sym->type->elem, it);
                    if (el && type_is_number(el) && widen_sym(c, base->sym, type_vec(el, base->sym->type->n))) continue;
                }
                ERR(c, tg, "value %d is %s and can't be stored in %s", i + 1, type_str(it), type_str(tg->type));
            }
            if (tg == base && base->sym) base->sym->unknown_type = false;
        }
        break;
    }
    case S_IF: {
        if (chk(c, s->value) != t_error) s->value = coerce(c, s->value, t_bool, "the condition of 'if'");
        chk_block(c, &s->body);
        chk_block(c, &s->else_body);
        break;
    }
    case S_WHILE: case S_DOWHILE: {
        if (s->kind == S_DOWHILE) {
            c->loop_depth++;
            chk_block(c, &s->body);
            c->loop_depth--;
        }
        if (chk(c, s->value) != t_error) s->value = coerce(c, s->value, t_bool, "the loop condition");
        if (s->kind == S_WHILE) {
            c->loop_depth++;
            chk_block(c, &s->body);
            c->loop_depth--;
        }
        break;
    }
    case S_FOR: case S_FOREACH: {
        Type *elt = t_int;
        if (s->kind == S_FOR) {
            Expr **parts[3] = {&s->from, &s->to, &s->step};
            for (int k = 0; k < 3; k++) {
                if (!*parts[k]) continue;
                Type *pt = chk(c, *parts[k]);
                if (pt == t_error) continue;
                if (pt != t_int) {
                    ERR(c, *parts[k], "'for' loops count with integers, but this is %s%s", type_str(pt), narrowing_hint(pt, t_int));
                    continue;
                }
            }
        } else {
            Type *ct = chk(c, s->from);
            if (ct == t_error) return;
            if (ct->kind != T_LIST && ct->kind != T_VEC) {
                ERR(c, s->from, "'for x in ...' needs a list or a vector (or a range like 1..10), not %s", type_str(ct));
                return;
            }
            elt = ct->elem;
        }
        /* the loop variable is always a fresh local (even in @global) */
        Scope *sc = c->scope;
        Sym *old = scope_find_local(sc, s->target->name);
        Sym *v;
        if (old && old->type == elt && old->kind == SYM_LOCAL) {
            v = old;
        } else {
            v = new_sym(c, s->target->name, elt, SYM_LOCAL, s->line, s->col);
            v->owner = c->fn;
            vec_push(sc->syms, v);
            vec_push(*c->locals, v);
        }
        s->sym = v;
        s->target->sym = v;
        s->target->type = elt;
        c->loop_depth++;
        chk_block(c, &s->body);
        c->loop_depth--;
        break;
    }
    case S_BREAK: case S_CONTINUE: case S_RETURN: break;
    case S_DRAW: {
        bool ok_place = (c->fn != NULL) || (c->sec && c->sec->kind == SEC_TOOL);
        if (!ok_place) {
            prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "'draw' can only be used in tools (viewers produce a color instead)");
            return;
        }
        if (c->fn) c->fn->uses_draw = true;
        if (s->text) chk_text(c, s->text);
        if (s->p0 && chk(c, s->p0) != t_error) s->p0 = coerce(c, s->p0, t_complex, "the position");
        if (s->p1 && chk(c, s->p1) != t_error) s->p1 = coerce(c, s->p1, t_complex, "the end point");
        if (s->radius && chk(c, s->radius) != t_error) s->radius = coerce(c, s->radius, t_real, "the radius");
        if (s->width && chk(c, s->width) != t_error) s->width = coerce(c, s->width, t_real, "the width");
        if (s->color && chk(c, s->color) != t_error) s->color = coerce(c, s->color, t_color, "the color");
        s->viewer_index = -1;
        if (s->in_viewer) {
            Section *v = find_viewer(c->prog, s->in_viewer);
            if (!v) prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "'%s' is not a viewer name", s->in_viewer);
            else s->viewer_index = v->index;
        }
        break;
    }
    case S_APPEND: break;
    case S_PRINT: /* (anywhere: on the GPU it does nothing; the Inspect tool shows what a viewer prints) */
        chk_text(c, s->text);
        break;
    case S_ERASE: {
        bool ok_place = (c->fn != NULL) || (c->sec && c->sec->kind == SEC_TOOL);
        if (!ok_place) prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "'erase' can only be used in tools");
        if (c->fn) c->fn->uses_draw = true;
        if (s->op == 2) { /* erase "Name": the drawings of the tools named so */
            s->erase_tools = 0;
            bool found = false;
            for (int t = 0; t < c->prog->ntools; t++)
                if (!strcmp(c->prog->tools[t]->display, s->label)) {
                    found = true;
                    if (t < 64) s->erase_tools |= (u64)1 << t;
                }
            if (!found) prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "there is no tool named \"%s\"", s->label);
        }
        break;
    }
    case S_ON: {
        if (!c->sec || c->sec->kind != SEC_TOOL || c->fn) {
            prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "'on ... do' handles an event of a tool, so it belongs in a tool section");
            break;
        }
        /* each handler is its own scope: share values between events through globals */
        Scope hscope = {{0}, c->scope};
        Scope *saved = c->scope;
        c->scope = &hscope;
        chk_block(c, &s->body);
        c->scope = saved;
        vec_free(hscope.syms);
        break;
    }
    }
}

static void chk_block(Checker *c, Block *b) {
    for (int i = 0; i < b->n; i++) chk_stmt(c, b->stmts[i]);
}

/* ---- functions --------------------------------------------------------- */
static void layout_frame(SymVec *locals, int *frame_words) {
    int off = 0;
    for (int i = 0; i < locals->len; i++) {
        Sym *s = locals->data[i];
        off = round_up(off, s->type->align);
        s->slot = off;
        off += s->type->size;
    }
    *frame_words = off;
}

static void resolve_signature(Checker *c, Func *f) {
    for (int i = 0; i < f->nparams; i++) { /* a parameter without a type makes the function generic */
        f->params[i]->type = f->param_tsyn[i] ? resolve_tsyn(c, f->param_tsyn[i]) : NULL;
        f->params[i]->owner = f;
        if (!f->param_tsyn[i]) f->generic = true;
    }
    for (int i = 0; i < f->noutputs; i++) {
        if (output_param(f, i) >= 0) continue; /* (typed as a parameter) */
        f->outputs[i]->owner = f;
        if (f->output_tsyn[i]) f->outputs[i]->type = resolve_tsyn(c, f->output_tsyn[i]);
        else if (!f->generic) untyped_output(c->prog, f->outputs[i], 0);
        else f->outputs[i]->type = NULL; /* (each instance has its own) */
    }
}

/* Errors found in an instance of a generic function: say which one (once for each place) */
static void note_instance_errors(Checker *c, Func *f, int d0) {
    Program *p = c->prog;
    int w = d0;
    for (int i = d0; i < p->diags.len; i++) {
        Diag d = p->diags.data[i];
        if (!d.noted) {
            bool dup = false; /* the same error in another instance */
            for (int k = 0; k < d0 && !dup; k++) {
                Diag *o = &p->diags.data[k];
                size_t n = strlen(d.msg);
                if (o->line == d.line && o->col == d.col && o->noted && !strncmp(o->msg, d.msg, n) && o->msg[n] == ' ')
                    dup = true;
            }
            if (dup) continue; /* (still counted in nerrors) */
            StrBuf sb = {0};
            sb_printf(&sb, "%s (%s ", d.msg, f->probe ? "checked as" : "in");
            instance_name(f, &sb);
            if (f->probe) sb_puts(&sb, ", since nothing calls it yet");
            else if (f->call_line > 0 && f->call_line < PRELUDE_LINE0) sb_printf(&sb, ", called on line %d", f->call_line);
            sb_putc(&sb, ')');
            d.msg = arena_strndup(&p->arena, sb_cstr(&sb), (size_t)sb.len);
            sb_free(&sb);
            d.noted = true;
        }
        p->diags.data[w++] = d;
    }
    p->diags.len = w;
}


/* ---- the Riemann sphere: arithmetic on P, lowered to the components of lifts ----------------------------------
 * A value of type P is a lift (a, b) of the point [a : b] (notes/projective.md).  The checker leaves + - * / ^n and
 * numbers turned into points as P-typed nodes; at the end of each function and section they are rewritten into
 * complex arithmetic on the components: [a : b] + [c : d] = [ad + bc : bd], and so on, ending in one __pmake(N, D)
 * (which scales the result so that its largest component is 1).
 *
 * Done naively, a sum picks up factors from the denominators: z^2 + 1 would be [a^2 b + b^2 b : b^2 b]... and
 * (z^2 + 1)/(z^2 - 1) would be [0 : 0] at infinity (b = 0), where it is 1.  So the monomials in the components of
 * the points involved (a^i b^j) are kept apart, symbolically, and common ones cancel: z^2 + 1 is computed as
 * [a^2 + b^2 : b^2] and the quotient as [a^2 + b^2 : a^2 - b^2], the standard lift of the rational map.  Points
 * named by variables (and parameters) get symbols; others ([x : y], calls...) are taken as they are.
 *
 * An expression whose pieces include calls of functions (that might change globals, or be slow) becomes a call of
 * a small generated function of those pieces, so that each is computed once. */
#define PX_SYMS 6       /* points with symbols in one expression (the others are taken as they are) */
#define PX_MAX_COST 6000 /* expanded size beyond which an expression must be split */

typedef struct PxVal {
    Expr *n, *d;           /* the lift (N, D) = (monomial mn * n, monomial md * d) */
    int nc, dc;            /* the sizes of n and d, as computed (shared parts counted each time) */
    int mn[2 * PX_SYMS], md[2 * PX_SYMS]; /* exponents of a_k, b_k (k: the symbol) */
} PxVal;

typedef struct PxLow {
    Checker *c;
    Expr *at;
    Expr *syms[PX_SYMS];   /* the point of each symbol (a name) */
    int nsyms;
    bool too_big;
} PxLow;

static bool px_arith(Expr *e) {
    if (!e || e->type != t_proj) return false;
    if (e->kind == E_BINARY) return is_arith(e->op);
    return (e->kind == E_UNARY && e->op == TK_MINUS) || e->kind == E_POWI || e->kind == E_CONVERT;
}

static Expr *px_node(PxLow *L, ExprKind k, Type *t) {
    Expr *e = ARENA_NEW(&L->c->prog->arena, Expr);
    e->kind = k;
    e->type = t;
    e->line = L->at->line;
    e->col = L->at->col;
    e->end_col = L->at->end_col;
    return e;
}
static Expr *px_const(PxLow *L, float re, float im) {
    Expr *e = px_node(L, E_COLOR_CONST, t_complex);
    e->cval[0] = re;
    e->cval[1] = im;
    return e;
}
static bool px_is_const(Expr *e, float re) {
    return e && e->kind == E_COLOR_CONST && e->type == t_complex && e->cval[0] == re && e->cval[1] == 0;
}
static Expr *px_bin(PxLow *L, int op, Expr *x, Expr *y) {
    if (op == TK_STAR) {
        if (px_is_const(x, 1)) return y;
        if (px_is_const(y, 1)) return x;
    }
    if ((op == TK_PLUS || op == TK_MINUS) && px_is_const(y, 0)) return x;
    if (op == TK_PLUS && px_is_const(x, 0)) return y;
    Expr *e = px_node(L, E_BINARY, t_complex);
    e->op = op;
    e->a = x;
    e->b = y;
    return e;
}
static Expr *px_neg(PxLow *L, Expr *x) {
    if (x->kind == E_COLOR_CONST && x->type == t_complex) return px_const(L, -x->cval[0], -x->cval[1]);
    Expr *e = px_node(L, E_UNARY, t_complex);
    e->op = TK_MINUS;
    e->a = x;
    return e;
}
static Expr *px_pow(PxLow *L, Expr *x, int k) {
    if (k == 0) return px_const(L, 1, 0);
    if (k == 1 || px_is_const(x, 1)) return x;
    Expr *e = px_node(L, E_POWI, t_complex);
    e->a = x;
    e->ival = k;
    return e;
}
static Expr *px_intrinsic(PxLow *L, int id, Type *t, Expr *a, Expr *b) {
    Expr *e = px_node(L, E_CALL, t);
    e->name = id == I_PMAKE ? "__pmake" : id == I_PA ? "__pa" : "__pb";
    e->intrinsic = id;
    e->nargs = b ? 2 : 1;
    e->args = ARENA_ARRAY(&L->c->prog->arena, Expr *, 2);
    e->args[0] = a;
    e->args[1] = b;
    return e;
}
/* the component a (k = 0) or b (k = 1) of the point e */
static Expr *px_comp(PxLow *L, Expr *e, int k) {
    if (e->kind == E_CALL && e->intrinsic == I_PMAKE) return e->args[k]; /* (scaling doesn't matter here) */
    if ((e->kind == E_NAME || e->kind == E_INTERNAL) && e->sym && e->sym->kind == SYM_CONST)
        return px_const(L, e->sym->cval[2 * k], e->sym->cval[2 * k + 1]);
    return px_intrinsic(L, k ? I_PB : I_PA, t_complex, clone_expr(&L->c->prog->arena, e), NULL);
}
static int px_size(Expr *e) { /* (of a leaf) */
    if (!e) return 0;
    int n = 1 + px_size(e->a) + px_size(e->b);
    for (int i = 0; i < e->nargs; i++) n += px_size(e->args[i]);
    return n;
}

/* the product of the monomials of the symbols with exponents m (NULL: 1), and its size */
static Expr *px_mono(PxLow *L, const int *m, int *cost) {
    Expr *r = NULL;
    for (int s = 0; s < L->nsyms; s++)
        for (int k = 0; k < 2; k++) {
            int e = m[2 * s + k];
            if (e <= 0) continue;
            Expr *f = px_pow(L, px_comp(L, L->syms[s], k), e);
            r = r ? px_bin(L, TK_STAR, r, f) : f;
            *cost += 4;
        }
    return r;
}
static Expr *px_times_mono(PxLow *L, const int *m, Expr *x, int *cost) {
    Expr *mo = px_mono(L, m, cost);
    if (!mo) return x;
    *cost += 1;
    return px_bin(L, TK_STAR, mo, x);
}

static void px_cancel(PxVal *v) {
    for (int k = 0; k < 2 * PX_SYMS; k++) {
        int e = FS_MIN(v->mn[k], v->md[k]);
        v->mn[k] -= e;
        v->md[k] -= e;
    }
}

static PxVal px_leaf(PxLow *L, Expr *n, Expr *d) {
    PxVal v = {0};
    v.n = n;
    v.d = d;
    v.nc = px_size(n);
    v.dc = px_size(d);
    return v;
}

static PxVal px_eval(PxLow *L, Expr *e) {
    if (e->kind == E_CONVERT && e->type == t_proj) /* a number z: [z : 1] */
        return px_leaf(L, mk_convert(L->c, e->a, t_complex), px_const(L, 1, 0));
    if (e->kind == E_UNARY) {
        PxVal v = px_eval(L, e->a);
        v.n = px_neg(L, v.n);
        v.nc++;
        return v;
    }
    if (e->kind == E_POWI) {
        PxVal v = px_eval(L, e->a), r = {0};
        int k = e->ival, m = k < 0 ? -k : k;
        if (k < 0) { /* 1/p = [b : a] */
            Expr *t = v.n;
            v.n = v.d;
            v.d = t;
            int tc = v.nc;
            v.nc = v.dc;
            v.dc = tc;
            for (int i = 0; i < 2 * PX_SYMS; i++) {
                int x = v.mn[i];
                v.mn[i] = v.md[i];
                v.md[i] = x;
            }
        }
        r.n = px_pow(L, v.n, m);
        r.d = px_pow(L, v.d, m);
        r.nc = v.nc + 2;
        r.dc = v.dc + 2;
        for (int i = 0; i < 2 * PX_SYMS; i++) {
            r.mn[i] = v.mn[i] * m;
            r.md[i] = v.md[i] * m;
        }
        return r;
    }
    if (e->kind == E_BINARY) {
        PxVal x = px_eval(L, e->a), y = px_eval(L, e->b), r = {0};
        int op = e->op;
        if (op == TK_STAR || op == TK_SLASH) {
            bool div = op == TK_SLASH; /* p / q = p * [d : c] */
            Expr *yn = div ? y.d : y.n, *yd = div ? y.n : y.d;
            int ync = div ? y.dc : y.nc, ydc = div ? y.nc : y.dc;
            r.n = px_bin(L, TK_STAR, x.n, yn);
            r.d = px_bin(L, TK_STAR, x.d, yd);
            r.nc = x.nc + ync + 1;
            r.dc = x.dc + ydc + 1;
            for (int i = 0; i < 2 * PX_SYMS; i++) {
                r.mn[i] = x.mn[i] + (div ? y.md[i] : y.mn[i]);
                r.md[i] = x.md[i] + (div ? y.mn[i] : y.md[i]);
            }
        } else { /* [N1 : D1] + [N2 : D2] = [N1 D2 + N2 D1 : D1 D2], the monomial common to both terms kept apart */
            int t1[2 * PX_SYMS], t2[2 * PX_SYMS];
            for (int i = 0; i < 2 * PX_SYMS; i++) {
                t1[i] = x.mn[i] + y.md[i];
                t2[i] = y.mn[i] + x.md[i];
                int g = FS_MIN(t1[i], t2[i]);
                t1[i] -= g;
                t2[i] -= g;
                r.mn[i] = g;
                r.md[i] = x.md[i] + y.md[i];
            }
            int c1 = 0, c2 = 0;
            Expr *a1 = px_times_mono(L, t1, px_bin(L, TK_STAR, x.n, y.d), &c1);
            Expr *a2 = px_times_mono(L, t2, px_bin(L, TK_STAR, y.n, x.d), &c2);
            r.n = px_bin(L, op, a1, a2);
            r.nc = x.nc + y.dc + y.nc + x.dc + c1 + c2 + 3;
            r.d = px_bin(L, TK_STAR, x.d, y.d);
            r.dc = x.dc + y.dc + 1;
        }
        px_cancel(&r);
        if (r.nc + r.dc > PX_MAX_COST) L->too_big = true;
        return r;
    }
    /* a point: a symbol for a variable, else its components as they are */
    if ((e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER) && e->sym && e->sym->kind != SYM_CONST) {
        int s = 0;
        while (s < L->nsyms && L->syms[s]->sym != e->sym) s++;
        if (s == L->nsyms && L->nsyms < PX_SYMS) L->syms[L->nsyms++] = e;
        if (s < L->nsyms) {
            PxVal v = px_leaf(L, px_const(L, 1, 0), px_const(L, 1, 0));
            v.mn[2 * s] = 1;
            v.md[2 * s + 1] = 1;
            return v;
        }
    }
    return px_leaf(L, px_comp(L, e, 0), px_comp(L, e, 1));
}

/* The arithmetic tree e (P), with cheap pieces only: __pmake(N, D) */
static Expr *px_lower_tree(Checker *c, Expr *e) {
    PxLow L = {0};
    L.c = c;
    L.at = e;
    PxVal v = px_eval(&L, e);
    int cost = 0;
    Expr *n = px_times_mono(&L, v.mn, v.n, &cost), *d = px_times_mono(&L, v.md, v.d, &cost);
    if (L.too_big)
        ERR(c, e, "this computation with points of the sphere gets too long once written with their coordinates; "
                  "split it with intermediate variables (p := ...)");
    return px_intrinsic(&L, I_PMAKE, t_proj, n, d);
}

/* can a piece of an arithmetic tree be computed several times (no calls of functions)? */
static bool px_cheap(Expr *e) {
    if (!e) return true;
    if (e->kind == E_CALL && !e->intrinsic) return false;
    if (e->kind == E_COND || e->kind == E_SOLVE || e->kind == E_TUPLE) return false;
    if (!px_cheap(e->a) || !px_cheap(e->b)) return false;
    for (int i = 0; i < e->nargs; i++)
        if (!px_cheap(e->args[i])) return false;
    return true;
}

typedef VEC(Expr *) ExprVec;

/* the pieces of the arithmetic tree at *slot that are not arithmetic on P: calls px_lower_expr on them, and (if
   collect) replaces each by a parameter of f, noting the piece in `pieces` */
static void px_lower_expr(Checker *c, Expr **slot);
static void px_pieces(Checker *c, Expr **slot, bool collect, Func *f, SymVec *params, ExprVec *pieces) {
    Expr *e = *slot;
    if (px_arith(e)) {
        if (e->kind == E_CONVERT) { /* a number */
            Expr *a = e->a;
            if (a->kind == E_INT || a->kind == E_REAL || a->kind == E_IMAG || a->kind == E_COLOR_CONST) return;
            px_pieces(c, &e->a, collect, f, params, pieces);
            return;
        }
        px_pieces(c, &e->a, collect, f, params, pieces);
        if (e->kind == E_BINARY) px_pieces(c, &e->b, collect, f, params, pieces);
        return;
    }
    px_lower_expr(c, slot);
    if (!collect) return;
    e = *slot;
    if (e->kind == E_NAME && e->sym && e->sym->kind == SYM_CONST) return;
    /* Each piece is an argument, computed by the caller in order (a global read before a call that changes it keeps
       its old value). The same variable is the same argument, so that its powers cancel (exact at 0 and ∞), unless
       a call in between may have changed it (a global). */
    Sym *s = NULL;
    bool var = (e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER) && e->sym;
    for (int i = pieces->len - 1; var && i >= 0 && !s; i--) {
        Expr *q = pieces->data[i];
        if ((q->kind == E_NAME || q->kind == E_INTERNAL || q->kind == E_MEMBER) && q->sym == e->sym) s = params->data[i];
        else if (e->sym->kind == SYM_GLOBAL && !px_cheap(q)) break;
    }
    if (!s) {
        s = new_sym(c, arena_printf(&c->prog->arena, "__x%d", params->len + 1), e->type, SYM_LOCAL, e->line, e->col);
        s->owner = f;
        vec_push(*params, s);
        vec_push(*pieces, e);
    }
    Expr *n = ARENA_NEW(&c->prog->arena, Expr);
    *n = *e;
    n->kind = E_NAME;
    n->name = s->name;
    n->sym = s;
    n->a = n->b = NULL;
    n->args = NULL;
    n->nargs = 0;
    n->fn = NULL;
    n->intrinsic = 0;
    *slot = n;
}

static void px_lower_expr(Checker *c, Expr **slot) {
    Expr *e = *slot;
    if (!e) return;
    if (!px_arith(e)) {
        px_lower_expr(c, &e->a);
        px_lower_expr(c, &e->b);
        px_lower_expr(c, &e->cont_from);
        for (int i = 0; i < e->nargs; i++) px_lower_expr(c, &e->args[i]);
        return;
    }
    if (px_cheap(e)) {
        px_pieces(c, slot, false, NULL, NULL, NULL);
        *slot = px_lower_tree(c, e);
        return;
    }
    /* a generated one-line function of the pieces, called with them */
    static int counter = 0;
    Program *p = c->prog;
    Func *f = ARENA_NEW(&p->arena, Func);
    f->name = arena_printf(&p->arena, "__p%d", ++counter);
    f->line = e->line;
    f->col = e->col;
    f->src_start = f->src_end = -1;
    SymVec params = {0};
    ExprVec pieces = {0};
    px_pieces(c, slot, true, f, &params, &pieces);
    f->nparams = params.len;
    f->params = ARENA_ARRAY(&p->arena, Sym *, params.len + 1);
    f->param_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, params.len + 1);
    for (int i = 0; i < params.len; i++) {
        f->params[i] = params.data[i];
        vec_push(f->locals, params.data[i]);
    }
    f->outputs = ARENA_ARRAY(&p->arena, Sym *, 1);
    f->output_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, 1);
    f->body_expr = px_lower_tree(c, *slot);
    f->ret = t_proj;
    f->check_state = 2;
    layout_frame(&f->locals, &f->frame_words);
    vec_push(p->funcs, f);
    Expr *call = ARENA_NEW(&p->arena, Expr);
    *call = *e;
    call->kind = E_CALL;
    call->name = f->name;
    call->fn = f;
    call->intrinsic = 0;
    call->a = call->b = NULL;
    call->nargs = pieces.len;
    call->args = ARENA_ARRAY(&p->arena, Expr *, pieces.len + 1);
    for (int i = 0; i < pieces.len; i++) call->args[i] = pieces.data[i];
    call->type = t_proj;
    f->ncalls++;
    *slot = call;
    vec_free(params);
    vec_free(pieces);
}

static void px_lower_block(Checker *c, Block *b) {
    for (int i = 0; i < b->n; i++) {
        Stmt *s = b->stmts[i];
        Expr **slots[] = {&s->target, &s->value, &s->range_lo, &s->range_hi, &s->from, &s->to, &s->step,
                          &s->p0, &s->p1, &s->radius, &s->color, &s->width, &s->text};
        for (int k = 0; k < ARRAY_COUNT(slots); k++) px_lower_expr(c, slots[k]);
        for (int k = 0; k < s->ntargets; k++) px_lower_expr(c, &s->targets[k]);
        px_lower_block(c, &s->body);
        px_lower_block(c, &s->else_body);
    }
}

static void check_func(Checker *c, Func *f) {
    Checker saved = *c;
    Scope fscope = {{0}, &c->globals};
    f->check_state = 1;
    c->scope = &fscope;
    c->fn = f;
    c->locals = &f->locals;
    c->load = false;
    c->loop_depth = 0;
    c->ctx = f->tmpl ? f->ctx : 0;
    int errs = c->prog->nerrors, d0 = c->prog->diags.len;
    for (int i = 0; i < f->nparams; i++) {
        for (int j = 0; j < i; j++)
            if (!strcmp(f->params[i]->name, f->params[j]->name))
                prog_diag(c->prog, DIAG_ERROR, f->params[i]->line, f->params[i]->col, 0, "the parameter '%s' appears twice", f->params[i]->name);
        vec_push(fscope.syms, f->params[i]);
        vec_push(f->locals, f->params[i]);
    }
    for (int i = 0; i < f->noutputs; i++) {
        if (output_param(f, i) >= 0) continue;
        vec_push(fscope.syms, f->outputs[i]);
        vec_push(f->locals, f->outputs[i]);
    }
    if (f->body_expr) {
        Expr *b = f->body_expr;
        if (b->kind == E_TUPLE) {
            Type *items[16];
            bool ok = b->nargs <= 16;
            for (int i = 0; ok && i < b->nargs; i++) {
                items[i] = chk(c, b->args[i]);
                if (items[i] == t_error) ok = false;
            }
            f->ret = ok ? type_tuple(items, b->nargs) : t_error;
            b->type = f->ret;
        } else {
            f->ret = chk(c, b);
            if (f->ret == t_void) {
                ERR(c, b, "a function must produce a value");
                f->ret = t_error;
            }
        }
    } else {
        chk_block(c, &f->body);
        if (f->noutputs == 0) {
            f->ret = t_void; /* define f(x) where ... end */
        } else if (f->noutputs == 1) {
            f->ret = f->outputs[0]->type;
        } else {
            Type *items[16];
            int n = FS_MIN(f->noutputs, 16);
            for (int i = 0; i < n; i++) items[i] = f->outputs[i]->type;
            f->ret = type_tuple(items, n);
        }
    }
    if (c->prog->nerrors == errs) { /* arithmetic on points of the sphere -> on their coordinates */
        px_lower_expr(c, &f->body_expr);
        px_lower_block(c, &f->body);
    }
    if (c->prog->nerrors != errs || f->ret == t_error) f->failed = true;
    if (f->tmpl) note_instance_errors(c, f, d0);
    else /* (errors of a function with types are its own, even when it is first checked from an instance) */
        for (int i = d0; i < c->prog->diags.len; i++) c->prog->diags.data[i].noted = true;
    layout_frame(&f->locals, &f->frame_words);
    vec_free(fscope.syms);
    f->check_state = 2;
    *c = saved;
}

/* A generic function nothing calls is checked anyway (for typos...), as if called with complex numbers, or real
   numbers, or integers: its errors are shown when none of them works (those of the complex numbers). */
static void probe_template(Checker *c, Func *t) {
    Program *p = c->prog;
    static Type **tries[3] = {&t_complex, &t_real, &t_int};
    Diag *saved = NULL;
    int nsaved = 0, saved_errors = 0;
    for (int k = 0; k < 3; k++) {
        Type *at[64];
        for (int i = 0; i < t->nparams && i < 64; i++) at[i] = t->param_tsyn[i] ? t->params[i]->type : *tries[k];
        int d0 = p->diags.len, e0 = p->nerrors;
        Func *g = instantiate(c, t, at, 0);
        g->probe = true;
        if (g->check_state == 0) {
            c->scope = &c->globals;
            c->probing++;
            check_func(c, g);
            c->probing--;
        }
        if (!g->failed) {
            free(saved);
            return;
        }
        if (k == 0) { /* keep the complex numbers' errors */
            nsaved = p->diags.len - d0;
            saved_errors = p->nerrors - e0;
            saved = (Diag *)xmalloc(sizeof(Diag) * (size_t)FS_MAX(nsaved, 1));
            memcpy(saved, p->diags.data + d0, sizeof(Diag) * (size_t)nsaved);
        }
        p->diags.len = d0;
        p->nerrors = e0;
    }
    for (int i = 0; i < nsaved; i++) vec_push(p->diags, saved[i]);
    p->nerrors += saved_errors;
    free(saved);
}

/* ---- sections ----------------------------------------------------------- */
static void extract_tool_settings(Checker *c, Section *s) {
    for (int i = 0; i < s->body.n; i++) {
        Stmt *st = s->body.stmts[i];
        if (st->kind == S_ON) { /* the event handlers */
            if (s->on[st->op])
                prog_diag(c->prog, DIAG_ERROR, st->line, st->col, st->end_col, "this tool already has an 'on' block for this event");
            s->on[st->op] = &st->body;
            s->has_events = true;
            continue;
        }
        if (st->kind != S_ASSIGN && st->kind != S_DECL) continue;
        Expr *tg = st->target;
        if (tg->kind != E_INTERNAL || (strcmp(tg->name, "panning") && strcmp(tg->name, "hover") && strcmp(tg->name, "keep")))
            continue;
        if (!st->value || st->value->kind != E_BOOL) {
            ERR(c, tg, ".%s must be set to true or false", tg->name);
        } else if (!strcmp(tg->name, "panning")) {
            s->panning = st->value->ival != 0;
        } else if (!strcmp(tg->name, "keep")) {
            s->keep = st->value->ival != 0;
        } else {
            s->hover = st->value->ival != 0;
        }
        st->kind = S_EXPR;
        st->value = NULL;
    }
    for (int i = 0; i < s->body.n; i++) {
        Stmt *st = s->body.stmts[i];
        if (st->kind != S_ON && !(st->kind == S_EXPR && !st->value)) s->has_main = true;
    }
}

static void check_section(Checker *c, Section *s) {
    Scope sscope = {{0}, &c->globals};
    c->sec = s;
    c->fn = NULL;
    c->scope = &sscope;
    c->locals = &s->locals;
    c->load = s->kind == SEC_GLOBAL || s->kind == SEC_CONFIG;
    c->loop_depth = 0;
    if (s->kind == SEC_VIEWER) {
        Expr at = {0};
        at.line = s->line;
        at.col = 1;
        if (s->var) s->coord = declare(c, &at, s->var, s->var_proj ? t_proj : t_complex);
        s->color = declare(c, &at, "color", t_color);
    }
    if (s->kind == SEC_TOOL && s->viewer_of >= 0) {
        /* @NAME.tool: the viewer's variable is the point that was clicked */
        Section *v = c->prog->viewers[s->viewer_of];
        Expr at = {0};
        at.line = s->line;
        at.col = 1;
        if (v->var) s->coord = declare(c, &at, v->var, t_complex);
    }
    if (s->kind == SEC_TOOL) extract_tool_settings(c, s);
    if (s->kind == SEC_CONFIG) { /* each @config section is a group of the config panel */
        ConfigItem it = {CFG_GROUP, s->display, NULL};
        vec_push(c->prog->config, it);
    }
    int errs = c->prog->nerrors;
    chk_block(c, &s->body);
    if (c->prog->nerrors == errs) px_lower_block(c, &s->body); /* (points of the sphere, see check_func) */
    layout_frame(&s->locals, &s->frame_words);
    /* load sections declare globals into c->globals directly; locals only for loop variables */
    vec_free(sscope.syms);
    c->sec = NULL;
}

static void layout_globals(Program *p) {
    int off = 0;
    for (int i = 0; i < p->globals.len; i++) {
        Sym *s = p->globals.data[i];
        off = round_up(off, s->type->align);
        s->slot = off;
        off += s->type->size;
    }
    p->state_words = round_up(FS_MAX(off, 4), 4);
}

static void jet_cache_reset(void);

bool check_program(Program *p) {
    types_init();
    jet_cache_reset(); /* jet functions belong to the program being checked */
    Checker C = {0};
    Checker *c = &C;
    c->prog = p;
    c->globals.parent = &c->builtin;
    /* collect viewers and tools */
    int nv = 0, nt = 0, np = 0;
    for (int i = 0; i < p->sections.len; i++) {
        if (p->sections.data[i]->kind == SEC_VIEWER) nv++;
        if (p->sections.data[i]->kind == SEC_TOOL) nt++;
        if (p->sections.data[i]->kind == SEC_PREPARE) np++;
    }
    p->viewers = ARENA_ARRAY(&p->arena, Section *, nv + 1);
    p->tools = ARENA_ARRAY(&p->arena, Section *, nt + 1);
    p->prepares = ARENA_ARRAY(&p->arena, Section *, np + 1);
    for (int i = 0; i < p->sections.len; i++) {
        Section *s = p->sections.data[i];
        if (s->kind == SEC_VIEWER) {
            for (int v = 0; v < p->nviewers; v++)
                if (s->name && p->viewers[v]->name && !strcmp(s->name, p->viewers[v]->name))
                    prog_diag(p, DIAG_ERROR, s->line, 1, 0, "there is already a viewer named '%s'", s->name);
            s->index = p->nviewers;
            p->viewers[p->nviewers++] = s;
            if (!s->display) s->display = s->name ? s->name : "viewer";
        }
        if (s->kind == SEC_TOOL) {
            s->index = p->ntools;
            p->tools[p->ntools++] = s;
            if (!s->display) s->display = arena_printf(&p->arena, "Tool %d", p->ntools);
        }
        if (s->kind == SEC_PREPARE) p->prepares[p->nprepares++] = s;
    }
    for (int t = 0; t < p->ntools; t++) {
        Section *s = p->tools[t];
        s->viewer_of = -1;
        if (!s->of_viewer) continue;
        Section *v = find_viewer(p, s->of_viewer);
        if (v) s->viewer_of = v->index;
        else prog_diag(p, DIAG_ERROR, s->line, 1, 0, "@%s.tool: there is no viewer named '%s'", s->of_viewer, s->of_viewer);
    }
    /* tools that would answer the same key in the same viewer */
    for (int t = 0; t < p->ntools; t++) {
        Section *s = p->tools[t];
        char k1[64];
        if (!shortcut_key(s->shortcut, k1, sizeof k1)) continue;
        if (!strcmp(k1, "H") || !strcmp(k1, "P"))
            prog_diag(p, DIAG_WARNING, s->line, 1, 0, "'%s' is also the key of %s; this tool takes it", s->shortcut,
                      k1[0] == 'H' ? "Pan" : "Pick point");
        for (int u = 0; u < t; u++) {
            Section *o = p->tools[u];
            char k2[64];
            if (!shortcut_key(o->shortcut, k2, sizeof k2) || strcmp(k1, k2) != 0) continue;
            if (s->viewer_of >= 0 && o->viewer_of >= 0 && s->viewer_of != o->viewer_of) continue;
            prog_diag(p, DIAG_WARNING, s->line, 1, 0, "the tool \"%s\" already uses the key '%s'", o->display, s->shortcut);
            break;
        }
    }
    if (p->nviewers == 0)
        prog_diag(p, DIAG_WARNING, 1, 1, 0, "the script has no @viewer section, so there is nothing to show");
    setup_internals(c);
    /* function signatures and overload chains */
    for (int i = 0; i < p->funcs.len; i++) resolve_signature(c, p->funcs.data[i]);
    for (int i = 0; i < p->funcs.len; i++) {
        Func *f = p->funcs.data[i];
        if (!f->prelude) /* instructions of the machine (re, im, complex...) can't be replaced */
            for (int k = 0; k < ARRAY_COUNT(intrinsics); k++) {
                const IntrinsicInfo *in = &intrinsics[k];
                if (strcmp(in->name, f->name) != 0 || in->nparams != f->nparams) continue;
                bool same = true;
                for (int q = 0; q < f->nparams && same; q++) {
                    char pc = in->params[q];
                    Type *pt = intrinsic_type(pc);
                    if (f->param_tsyn[q] && f->params[q]->type != pt) same = false; /* (no type: any) */
                }
                if (same) {
                    prog_diag(p, DIAG_ERROR, f->line, f->col, 0, "'%s' is a built-in function; choose another name", f->name);
                    break;
                }
            }
        Func *head = NULL;
        for (int j = 0; j < i && !head; j++)
            if (strcmp(f->name, p->funcs.data[j]->name) == 0) head = p->funcs.data[j];
        if (!head) continue;
        bool dup = false;
        for (Func *g = head; g; g = g->next_overload) {
            bool same = f->nparams == g->nparams && !g->shadowed, over = false; /* over: f takes what g takes */
            for (int k = 0; same && k < f->nparams; k++) {
                bool fa = !f->param_tsyn[k], ga = !g->param_tsyn[k]; /* (no type: any) */
                if (fa && !ga) over = true;
                else if (fa != ga || f->params[k]->type != g->params[k]->type) same = false;
            }
            if (!same) continue;
            if (g->prelude && !f->prelude) {
                g->shadowed = true; /* the script's own rainbow(t) replaces the built-in one (built-ins keep theirs) */
                continue;
            }
            if (over) { /* f(x) besides f(x : R): both (the one with types wins when both fit) */
                continue;
            } else {
                prog_diag(p, DIAG_ERROR, f->line, f->col, 0, "'%s' is already defined with the same parameter types (line %d)", f->name, g->line);
                dup = true;
            }
            break;
        }
        if (dup) continue;
        Func *last = head;
        while (last->next_overload) last = last->next_overload;
        if (last != f) last->next_overload = f;
    }
    /* 1: @global/@config in file order, 2: remaining user functions, 3: viewers and tools */
    for (int i = 0; i < p->sections.len; i++) {
        Section *s = p->sections.data[i];
        if (s->kind == SEC_GLOBAL || s->kind == SEC_CONFIG) check_section(c, s);
    }
    for (int i = 0; i < p->funcs.len; i++) {
        Func *f = p->funcs.data[i];
        if (!f->prelude && !f->generic && !f->tmpl && f->check_state == 0) {
            c->scope = &c->globals;
            check_func(c, f);
        }
    }
    for (int i = 0; i < p->sections.len; i++) {
        Section *s = p->sections.data[i];
        if (s->kind == SEC_VIEWER || s->kind == SEC_TOOL || s->kind == SEC_PREPARE) check_section(c, s);
    }
    for (int i = 0, n = p->funcs.len; i < n; i++) { /* generic functions nothing calls */
        Func *f = p->funcs.data[i];
        if (f->generic && !f->prelude && !f->instances) probe_template(c, f);
    }
    layout_globals(p);
    vec_free(c->builtin.syms);
    vec_free(c->globals.syms);
    vec_free(c->internals.syms);
    return p->nerrors == 0;
}
