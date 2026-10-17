/* check.c - name resolution, type checking, implicit conversions, layout.
 *
 * Scoping (Python-like): a section body and a function body are scopes;
 * if/for/while bodies are not.  Declarations: `x := e`, `x : T = e`, or a
 * plain `x = e` to a name that is not yet visible.  Untyped numeric
 * variables are complex (C); untyped vectors are at least real.  Variables
 * declared in @global/@config (outside functions) are program state.
 * Functions see globals, their parameters, outputs and own locals.
 * Implicit promotions: Z -> R -> C -> H (elementwise for vectors). */
#include "ast.h"
#include <ctype.h>

/* ---- types ------------------------------------------------------------- */
Type *t_error, *t_void, *t_bool, *t_int, *t_real, *t_complex, *t_quat, *t_string, *t_color;
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
    switch (t->kind) {
    case T_ERROR: sb_puts(out, "<error>"); break;
    case T_VOID: sb_puts(out, "nothing"); break;
    case T_BOOL: sb_puts(out, "Bool"); break;
    case T_INT: sb_puts(out, "Z"); break;
    case T_REAL: sb_puts(out, "R"); break;
    case T_COMPLEX: sb_puts(out, "C"); break;
    case T_QUAT: sb_puts(out, "H"); break;
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
} Checker;

static Sym *new_sym(Checker *c, const char *name, Type *t, SymKind kind, int line, int col) {
    Sym *s = ARENA_NEW(&c->prog->arena, Sym);
    s->name = (char *)name;
    s->type = t;
    s->kind = kind;
    s->line = line;
    s->col = col;
    s->viewer_index = -1;
    return s;
}

#define ERR(c, e, ...) prog_diag((c)->prog, DIAG_ERROR, (e)->line, (e)->col, (e)->end_col, __VA_ARGS__)

/* ---- conversions -------------------------------------------------------- */
static int conv_cost(Type *from, Type *to) {
    if (from == to) return 0;
    if (from == t_error || to == t_error) return 0;
    int rf = type_number_rank(from), rt = type_number_rank(to);
    if (rf && rt) return rf <= rt ? rt - rf : -1;
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

static const char *narrowing_hint(Type *from, Type *to) {
    if (from->kind == T_COMPLEX && type_is_scalar_real(to))
        return " (use re(z), im(z) or |z| to get a real number; untyped variables are complex, so declare reals as `x : R = ...`)";
    if (from->kind == T_REAL && to->kind == T_INT) return " (use floor, ceil or round to get an integer)";
    if (from->kind == T_COMPLEX && to->kind == T_INT) return " (untyped variables are complex; declare integers as `n : Z = ...`)";
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

/* a value written only with numbers and constants (0, 2.5, 1 + 2i, 2 pi) */
static bool is_number_literal(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case E_INT: case E_REAL: case E_IMAG: return true;
    case E_NAME: return e->sym && e->sym->kind == SYM_CONST && type_is_number(e->sym->type);
    case E_CONVERT: return is_number_literal(e->a);
    case E_UNARY: return e->op == TK_MINUS && is_number_literal(e->a);
    case E_POWI: return is_number_literal(e->a);
    case E_BINARY:
        return (e->op == TK_PLUS || e->op == TK_MINUS || e->op == TK_STAR || e->op == TK_SLASH) && is_number_literal(e->a) &&
               is_number_literal(e->b);
    default: return false;
    }
}

/* Type for an untyped declaration initialised with `value` (of type t): the type of the value, except
   that plain numbers are complex (`z := 0` is the usual start of an iteration).  Vectors are at least real. */
static Type *infer_decl_type(Type *t, Expr *value) {
    if (type_is_number(t)) {
        if (t->kind == T_QUAT) return t_quat;
        return !value || is_number_literal(value) ? t_complex : t;
    }
    if (t->kind == T_VEC && type_number_rank(t->elem) < 2) return type_vec(t_real, t->n);
    return t;
}

/* ---- type syntax ------------------------------------------------------- */
static Type *resolve_tsyn(Checker *c, TypeSyntax *ts) {
    if (!ts) return t_complex;
    const char *b = ts->base;
    Type *base = NULL;
    if (!strcmp(b, "Z") || !strcmp(b, "Integers") || !strcmp(b, "Integer") || !strcmp(b, "Int")) base = t_int;
    else if (!strcmp(b, "R") || !strcmp(b, "Reals") || !strcmp(b, "Real")) base = t_real;
    else if (!strcmp(b, "C") || !strcmp(b, "Complex")) base = t_complex;
    else if (!strcmp(b, "H") || !strcmp(b, "Quaternions") || !strcmp(b, "Quaternion")) base = t_quat;
    else if (!strcmp(b, "Bool") || !strcmp(b, "Boolean") || !strcmp(b, "Booleans") || !strcmp(b, "B")) base = t_bool;
    else if (!strcmp(b, "Color") || !strcmp(b, "Colour")) base = t_color;
    else if (!strcmp(b, "List")) {
        Type *e = resolve_tsyn(c, ts->elem);
        if (!type_is_number(e) && e != t_bool) {
            prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, 0, "lists can only hold numbers or Bool, not %s", type_str(e));
            return t_error;
        }
        return type_list(e, ts->cap > 0 ? ts->cap : 64);
    } else {
        prog_diag(c->prog, DIAG_ERROR, ts->line, ts->col, ts->col + (int)strlen(b),
                  "unknown type '%s' (types are Z, R, C, H, Bool, Color, R^n and List of T)", b);
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
    Sym *s = add_internal(c, name, t_color, SYM_CONST, "colour constant");
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
    p->s_time = add_internal(c, "time", t_real, SYM_GLOBAL, "seconds since the script started");
    p->s_time->readonly = true;
    p->s_last_click = add_internal(c, "lastClick", t_complex, SYM_GLOBAL, "point of the last click or drag");
    p->s_mouse = add_internal(c, "mouse", t_complex, SYM_GLOBAL, "point under the mouse");
    p->s_mouse->readonly = true;
    p->s_drag_start = add_internal(c, "dragStart", t_complex, SYM_GLOBAL, "where the button went down (tools)");
    p->s_drag_start->readonly = true;
    p->s_width = add_internal(c, "width", t_int, SYM_GLOBAL, "initial window width");
    p->s_height = add_internal(c, "height", t_int, SYM_GLOBAL, "initial window height");
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
    add_const(c, &c->builtin, "pi", t_real, 3.14159265358979f, 0, 0, 0);
    add_const(c, &c->builtin, "π", t_real, 3.14159265358979f, 0, 0, 0);
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
        s->center->doc = "centre of the view";
        s->radius = new_sym(c, arena_printf(&p->arena, "%s.radius", s->name), t_real, SYM_GLOBAL, s->line, 1);
        s->radius->viewer_index = v;
        s->radius->doc = "radius of the largest disk around the centre that fits in the view";
        vec_push(p->globals, s->center);
        vec_push(p->globals, s->radius);
        if (s->projective) { /* view of the Riemann sphere: Mobius map (a w + b) / (c w + d) */
            s->mobius = new_sym(c, arena_printf(&p->arena, "%s.mobius", s->name), type_vec(t_complex, 4), SYM_GLOBAL, s->line, 1);
            s->mobius->viewer_index = v;
            s->mobius->is_internal = true;
            s->mobius->doc = "current view of the sphere";
            vec_push(p->globals, s->mobius);
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

static Type *chk_call(Checker *c, Expr *e) {
    const char *name = e->name;
    if (name && !strcmp(name, "palette") && e->nargs == 2 && e->args[0]->kind == E_STRING) {
        /* palette("fire", t): the colour map of that name */
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
            Type *pt = pc == 'Z' ? t_int : pc == 'R' ? t_real : pc == 'C' ? t_complex : pc == 'H' ? t_quat : t_bool;
            int ci = conv_cost(e->args[i]->type, pt);
            cost = ci < 0 ? -1 : cost + ci;
        }
        if (cost < 0) continue;
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
            int ci = conv_cost(e->args[i]->type, f->params[i]->type);
            cost = ci < 0 ? -1 : cost + ci;
        }
        if (cost < 0) continue;
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
            for (int i = 0; i < e->nargs; i++)
                if (conv_cost(e->args[i]->type, only->params[i]->type) < 0) {
                    ERR(c, e->args[i], "argument %d of '%s' must be %s, but this is %s%s", i + 1, name,
                        type_str(only->params[i]->type), type_str(e->args[i]->type),
                        narrowing_hint(e->args[i]->type, only->params[i]->type));
                    return t_error;
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
                type_name(f->params[i]->type, &alts);
            }
            sb_putc(&alts, ')');
        }
        ERR(c, e, "'%s' can't be applied to (%s)%s%s", name, sb_cstr(&sb), nalt ? "; it accepts " : "", sb_cstr(&alts));
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
            Type *pt = pc == 'Z' ? t_int : pc == 'R' ? t_real : pc == 'C' ? t_complex : pc == 'H' ? t_quat : t_bool;
            e->args[i] = mk_convert(c, e->args[i], pt);
        }
        char r = in->ret;
        return r == 'Z' ? t_int : r == 'R' ? t_real : r == 'C' ? t_complex : r == 'H' ? t_quat : t_bool;
    }
    Func *f = best_fn;
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
        if (strcmp(e->name, "center") == 0 || strcmp(e->name, "centre") == 0) e->sym = v->center;
        else if (strcmp(e->name, "radius") == 0) e->sym = v->radius;
        else {
            ERR(c, e, "viewers have .center and .radius, not .%s", e->name);
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
        } else if (type_is_number(ta) || ta->kind == T_VEC) {
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
        if (ta == t_int || ta == t_real) t = ta;
        else if (ta == t_complex || ta == t_quat || ta->kind == T_VEC) t = t_real;
        else if (ta->kind == T_LIST) t = t_int;
        else ERR(c, e, "|...| needs a number, vector or list, not %s", type_str(ta));
        break;
    }
    case E_PRED: {
        Type *ta = chk(c, e->a);
        if (ta == t_error) break;
        if (!type_is_number(ta) && ta->kind != T_VEC)
            ERR(c, e, "'%s' needs a number or a vector, not %s", e->op == TK_ESCAPES ? "escapes" : "vanishes", type_str(ta));
        else
            t = t_bool;
        break;
    }
    case E_CALL: t = chk_call(c, e); break;
    case E_INDEX: t = chk_index(c, e); break;
    case E_VECTOR: {
        if (e->nargs == 0) {
            ERR(c, e, "an empty [] is only allowed as the value of a List");
            break;
        }
        Type *el = t_int;
        bool ok = true;
        for (int i = 0; i < e->nargs; i++) {
            Type *ti = chk(c, e->args[i]);
            if (ti == t_error) { ok = false; continue; }
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
    sym->has_range = true;
    sym->range_lo = lo;
    sym->range_hi = hi;
}

/* built-in colour helpers that a script may define again (scripts written for the first FractalStream did) */
static bool replaceable_builtin(const char *name) {
    static const char *names[] = {"rainbow", "cycle", "blend", "dark", "light", "invert"};
    for (int i = 0; i < ARRAY_COUNT(names); i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

/* a shortcut like "Shift + p" in a comparable form ("SHIFT+P"); false if there is none */
static bool shortcut_key(const char *sc, char *out, int cap) {
    int n = 0;
    for (const char *q = sc; q && *q && n < cap - 1; q++)
        if (*q != ' ') out[n++] = (char)toupper((unsigned char)*q);
    out[n] = 0;
    return n > 0;
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
    if (s->readonly || s->kind == SYM_CONST) ERR(c, t, "%s%s can't be changed", s->is_internal ? "." : "", s->name);
    else if (s->kind == SYM_EXEC) ERR(c, t, ".%s is set by loops and can't be assigned", s->name);
}

static void chk_stmt(Checker *c, Stmt *s) {
    switch (s->kind) {
    case S_DEFINE: break;
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
            s->value = coerce(c, s->value, t, "the value");
        }
        Sym *sym = declare(c, tg, tg->name, t);
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
            if (sym && sym->kind == SYM_CONST && scope_find_local(&c->builtin, tg->name) == sym) sym = NULL; /* shadow pi, e */
            if (!sym) {
                if (s->op != TK_ASSIGN) {
                    ERR(c, tg, "'%s' has no value yet; set it with '=' first", tg->name);
                    return;
                }
                Type *vt = chk(c, s->value);
                if (vt == t_error) { declare(c, tg, tg->name, t_error); return; }
                if (vt == t_void) { ERR(c, s->value, "this doesn't produce a value"); return; }
                Type *t = infer_decl_type(vt, s->value);
                sym = declare(c, tg, tg->name, t);
                s->value = coerce(c, s->value, t, "the value");
                s->implicit_decl = true;
                s->sym = sym;
                tg->sym = sym;
                tg->type = t;
                chk_config_extras(c, s, sym, true);
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
        s->value = coerce(c, s->value, tt, "the new value");
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
                if (!sym) {
                    sym = declare(c, tg, tg->name, infer_decl_type(it, v)); /* (n, ok) := f(x) keeps f's types */
                    if (!sym) return;
                }
                tg->sym = sym;
                tg->type = sym->type;
            } else if (chk(c, tg) == t_error) {
                return;
            }
            Expr *base = tg;
            while (base->kind == E_INDEX) base = base->a;
            check_assign_target(c, base);
            if (conv_cost(it, tg->type) < 0)
                ERR(c, tg, "value %d is %s and can't be stored in %s", i + 1, type_str(it), type_str(tg->type));
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
    case S_ERASE: {
        bool ok_place = (c->fn != NULL) || (c->sec && c->sec->kind == SEC_TOOL);
        if (!ok_place) prog_diag(c->prog, DIAG_ERROR, s->line, s->col, s->end_col, "'erase' can only be used in tools");
        if (c->fn) c->fn->uses_draw = true;
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
    for (int i = 0; i < f->nparams; i++) {
        f->params[i]->type = resolve_tsyn(c, f->param_tsyn[i]);
        f->params[i]->owner = f;
    }
    for (int i = 0; i < f->noutputs; i++) {
        f->outputs[i]->type = resolve_tsyn(c, f->output_tsyn[i]);
        f->outputs[i]->owner = f;
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
    int errs = c->prog->nerrors;
    for (int i = 0; i < f->nparams; i++) {
        for (int j = 0; j < i; j++)
            if (!strcmp(f->params[i]->name, f->params[j]->name))
                prog_diag(c->prog, DIAG_ERROR, f->params[i]->line, f->params[i]->col, 0, "the parameter '%s' appears twice", f->params[i]->name);
        vec_push(fscope.syms, f->params[i]);
        vec_push(f->locals, f->params[i]);
    }
    for (int i = 0; i < f->noutputs; i++) {
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
        if (f->noutputs == 1) {
            f->ret = f->outputs[0]->type;
        } else {
            Type *items[16];
            int n = FS_MIN(f->noutputs, 16);
            for (int i = 0; i < n; i++) items[i] = f->outputs[i]->type;
            f->ret = type_tuple(items, n);
        }
    }
    if (c->prog->nerrors != errs || f->ret == t_error) f->failed = true;
    layout_frame(&f->locals, &f->frame_words);
    vec_free(fscope.syms);
    f->check_state = 2;
    *c = saved;
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
        if (s->var) s->coord = declare(c, &at, s->var, t_complex);
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
    chk_block(c, &s->body);
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
    int nv = 0, nt = 0;
    for (int i = 0; i < p->sections.len; i++) {
        if (p->sections.data[i]->kind == SEC_VIEWER) nv++;
        if (p->sections.data[i]->kind == SEC_TOOL) nt++;
    }
    p->viewers = ARENA_ARRAY(&p->arena, Section *, nv + 1);
    p->tools = ARENA_ARRAY(&p->arena, Section *, nt + 1);
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
        if (!f->prelude) /* functions of the machine itself (sin of a real...) can't be replaced */
            for (int k = 0; k < ARRAY_COUNT(intrinsics); k++) {
                const IntrinsicInfo *in = &intrinsics[k];
                if (strcmp(in->name, f->name) != 0 || in->nparams != f->nparams) continue;
                bool same = true;
                for (int q = 0; q < f->nparams && same; q++) {
                    char pc = in->params[q];
                    Type *pt = pc == 'Z' ? t_int : pc == 'R' ? t_real : pc == 'C' ? t_complex : pc == 'H' ? t_quat : t_bool;
                    if (f->params[q]->type != pt) same = false;
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
            bool same = f->nparams == g->nparams && !g->shadowed;
            for (int k = 0; same && k < f->nparams; k++)
                if (f->params[k]->type != g->params[k]->type) same = false;
            if (!same) continue;
            if (g->prelude && !f->prelude && replaceable_builtin(g->name)) {
                g->shadowed = true; /* the script's own rainbow(t) replaces the built-in one (built-ins keep theirs) */
            } else if (g->prelude && !f->prelude) {
                prog_diag(p, DIAG_ERROR, f->line, f->col, 0, "'%s' is a built-in function; choose another name", f->name);
                dup = true;
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
        if (!f->prelude && f->check_state == 0) {
            c->scope = &c->globals;
            check_func(c, f);
        }
    }
    for (int i = 0; i < p->sections.len; i++) {
        Section *s = p->sections.data[i];
        if (s->kind == SEC_VIEWER || s->kind == SEC_TOOL) check_section(c, s);
    }
    layout_globals(p);
    vec_free(c->builtin.syms);
    vec_free(c->globals.syms);
    vec_free(c->internals.syms);
    return p->nerrors == 0;
}
