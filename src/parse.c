/* parse.c - tokens -> AST.
 *
 * Expression precedence, loosest first:
 *   or < and < not < comparisons, `escapes`, `vanishes` < + - < * / mod and
 *   juxtaposition (2z, z w, 2(1-w)) < unary minus < ^ (right assoc) < [index]
 * Function application without parentheses: `cos w + 1` is cos(w) + 1 and
 * `sin 2 pi t` is sin(2 pi t); the argument is a run of juxtaposed factors
 * that stops before `*`, `/`, `+`, ..., and before another function name, so
 * `sin x cos x` is sin(x) cos(x).  The parser knows which names are
 * functions because `define`d names are collected before parsing. */
#include "ast.h"

typedef struct Parser {
    Program *prog;
    Token *t;
    int n, pos;
    int abs_depth;
    int func_depth;    /* > 0 while parsing a function body */
    int loop_depth;
    const char **builtins; int nbuiltins;   /* intrinsic + prelude function names */
    const char **ufuncs; int nufuncs;       /* user-defined function names */
    const char **gvars; int ngvars;         /* global variable names (pre-scan) */
    VEC(const char *) lvars;                /* variables declared so far in this section/function */
    int last_line, last_end_col;      /* end of the previous token */
    int nerr_stmt;                    /* errors in the current statement */
    bool no_px;                       /* parsing a radius or width of `draw`, where px is its own unit */
    bool in_draw;                     /* parsing a `draw`: its modifier words don't multiply */
} Parser;

static Token *cur(Parser *p) { return &p->t[p->pos]; }
static Token *peek(Parser *p, int k) { int i = p->pos + k; return &p->t[i < p->n ? i : p->n - 1]; }
static bool at(Parser *p, TokKind k) { return cur(p)->kind == k; }
static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (t->kind != TK_EOF) {
        p->last_line = t->line;
        p->last_end_col = t->col + t->len;
        p->pos++;
    }
    return t;
}
static bool accept(Parser *p, TokKind k) {
    if (at(p, k)) { advance(p); return true; }
    return false;
}

static void perr(Parser *p, Token *t, const char *fmt, ...) FS_PRINTF(3, 4);
static void perr(Parser *p, Token *t, const char *fmt, ...) {
    if (p->nerr_stmt++ > 0) return; /* one error per statement */
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int end = t->col + (t->len > 0 ? t->len : 1);
    prog_diag(p->prog, DIAG_ERROR, t->line, t->col, end, "%s", buf);
}

static const char *tok_desc(Token *t) {
    static char buf[4][96];
    static int k = 0;
    char *b = buf[k++ & 3];
    if (t->kind == TK_IDENT || t->kind == TK_INTERNAL || t->kind == TK_SECTION)
        snprintf(b, 96, "'%s%s'", t->kind == TK_INTERNAL ? "." : t->kind == TK_SECTION ? "@" : "", t->str);
    else if (t->kind == TK_INT || t->kind == TK_REAL || t->kind == TK_IMAG)
        snprintf(b, 96, "the number %.*s", t->len, t->text);
    else
        snprintf(b, 96, "%s", tok_kind_name(t->kind));
    return b;
}

static bool expect(Parser *p, TokKind k, const char *context) {
    if (accept(p, k)) return true;
    perr(p, cur(p), "expected %s %s, found %s", tok_kind_name(k), context, tok_desc(cur(p)));
    return false;
}

static bool name_in(const char **v, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(v[i], name) == 0) return true;
    return false;
}

/* Variables shadow built-in functions (so `step := 0.1` works), but not
 * functions defined in the script. */
static bool is_function_name(Parser *p, const char *name) {
    if (name_in(p->ufuncs, p->nufuncs, name)) return true;
    if (name_in(p->lvars.data, p->lvars.len, name) || name_in(p->gvars, p->ngvars, name)) return false;
    return name_in(p->builtins, p->nbuiltins, name);
}

static void declare_var_name(Parser *p, const char *name) { vec_push(p->lvars, name); }

static Expr *new_expr(Parser *p, ExprKind kind, Token *t) {
    Expr *e = ARENA_NEW(&p->prog->arena, Expr);
    e->kind = kind;
    e->line = t->line;
    e->col = t->col;
    e->end_col = t->col + t->len;
    return e;
}
static void finish(Parser *p, Expr *e) {
    if (p->last_line == e->line && p->last_end_col > e->end_col) e->end_col = p->last_end_col;
}
static Expr *binary(Parser *p, int op, Expr *a, Expr *b) {
    Expr *e = ARENA_NEW(&p->prog->arena, Expr);
    e->kind = E_BINARY;
    e->op = op;
    e->a = a;
    e->b = b;
    e->line = a->line;
    e->col = a->col;
    e->end_col = a->end_col;
    finish(p, e);
    return e;
}
static Expr *unary(Parser *p, int op, Expr *a, Token *optok) {
    Expr *e = new_expr(p, E_UNARY, optok);
    e->op = op;
    e->a = a;
    finish(p, e);
    return e;
}

static Expr *parse_expr(Parser *p);
static Expr *parse_arith(Parser *p);
static Expr *parse_power(Parser *p);
static Expr *parse_unary(Parser *p);

/* can the current token start a juxtaposed factor (implicit multiplication)? */
static bool is_draw_modifier(const char *w) {
    return !strcmp(w, "radius") || !strcmp(w, "color") || !strcmp(w, "colour") || !strcmp(w, "width") ||
           !strcmp(w, "thickness");
}

static bool starts_factor(Parser *p) {
    switch (cur(p)->kind) {
    case TK_IDENT:
        if (strcmp(cur(p)->str, "continuing") == 0) return false; /* ... with seed s continuing from P */
        if (p->in_draw && is_draw_modifier(cur(p)->str)) return false; /* draw ... with color C width W */
        return true;
    case TK_INTERNAL: case TK_LPAREN: return true;
    case TK_BAR: return p->abs_depth == 0;
    default: return false;
    }
}

static Expr **parse_args(Parser *p, TokKind close, int *nout, const char *what) {
    VEC(Expr *) args = {0};
    int saved = p->abs_depth;
    p->abs_depth = 0;
    if (!at(p, close)) {
        for (;;) {
            Expr *a = parse_expr(p);
            if (!a) break;
            vec_push(args, a);
            if (!accept(p, TK_COMMA)) break;
        }
    }
    p->abs_depth = saved;
    expect(p, close, what);
    Expr **out = ARENA_ARRAY(&p->prog->arena, Expr *, args.len ? args.len : 1);
    if (args.len) memcpy(out, args.data, sizeof(Expr *) * (size_t)args.len);
    *nout = args.len;
    vec_free(args);
    return out;
}

/* argument of a function applied without parentheses */
static Expr *parse_app_arg(Parser *p) {
    Expr *e;
    if (at(p, TK_MINUS)) {
        Token *m = advance(p);
        e = unary(p, TK_MINUS, parse_app_arg(p), m);
        return e;
    }
    e = parse_power(p);
    if (!e) return NULL;
    while (starts_factor(p) && !(at(p, TK_IDENT) && is_function_name(p, cur(p)->str))) {
        Expr *b = parse_power(p);
        if (!b) return NULL;
        e = binary(p, TK_STAR, e, b);
    }
    return e;
}

/* `if c then a else b` as a value (no `end`; `else if` chains); the branches extend as far as possible */
static Expr *parse_if_expr(Parser *p) {
    Token *t = advance(p);
    Expr *e = new_expr(p, E_COND, t);
    e->nargs = 3;
    e->args = ARENA_ARRAY(&p->prog->arena, Expr *, 3);
    int saved = p->abs_depth;
    p->abs_depth = 0;
    e->args[0] = parse_expr(p);
    if (!e->args[0] || !expect(p, TK_THEN, "after the condition (as in: if c then a else b)")) goto fail;
    e->args[1] = parse_expr(p);
    if (!e->args[1]) goto fail;
    if (!accept(p, TK_ELSE)) {
        perr(p, cur(p), "a value 'if' needs an 'else' (as in: if c then a else b), found %s", tok_desc(cur(p)));
        goto fail;
    }
    e->args[2] = parse_expr(p);
    if (!e->args[2]) goto fail;
    p->abs_depth = saved;
    finish(p, e);
    return e;
fail:
    p->abs_depth = saved;
    return NULL;
}

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);
    Expr *e;
    switch (t->kind) {
    case TK_INT:
        advance(p);
        e = new_expr(p, E_INT, t);
        e->ival = (i32)t->num;
        e->rval = t->num;
        return e;
    case TK_REAL:
        advance(p);
        e = new_expr(p, E_REAL, t);
        e->rval = t->num;
        return e;
    case TK_IMAG:
        advance(p);
        e = new_expr(p, E_IMAG, t);
        e->rval = t->num;
        return e;
    case TK_TRUE: case TK_FALSE:
        advance(p);
        e = new_expr(p, E_BOOL, t);
        e->ival = t->kind == TK_TRUE;
        return e;
    case TK_STRING:
        advance(p);
        e = new_expr(p, E_STRING, t);
        e->name = t->str;
        return e;
    case TK_INTERNAL:
        advance(p);
        e = new_expr(p, E_INTERNAL, t);
        e->name = t->str;
        return e;
    case TK_IF: return parse_if_expr(p);
    case TK_LPAREN: {
        advance(p);
        int n;
        Expr **items = parse_args(p, TK_RPAREN, &n, "to close '('");
        if (n == 0) {
            perr(p, t, "empty parentheses");
            return NULL;
        }
        if (n == 1) {
            items[0]->paren = true;
            return items[0];
        }
        e = new_expr(p, E_TUPLE, t);
        e->args = items;
        e->nargs = n;
        finish(p, e);
        return e;
    }
    case TK_LBRACKET: {
        advance(p);
        e = new_expr(p, E_VECTOR, t);
        e->args = parse_args(p, TK_RBRACKET, &e->nargs, "to close '['");
        finish(p, e);
        return e;
    }
    case TK_BAR: {
        advance(p);
        e = new_expr(p, E_ABS, t);
        p->abs_depth++;
        e->a = parse_expr(p);
        p->abs_depth--;
        if (!e->a) return NULL;
        expect(p, TK_BAR, "to close |...|");
        finish(p, e);
        return e;
    }
    case TK_IDENT: {
        advance(p);
        if (strcmp(t->str, "i") == 0) {
            e = new_expr(p, E_IMAG, t);
            e->rval = 1.0;
            return e;
        }
        if (at(p, TK_INTERNAL) && !cur(p)->space_before) { /* viewer.prop */
            Token *m = advance(p);
            e = new_expr(p, E_MEMBER, t);
            e->base_name = t->str;
            e->name = m->str;
            finish(p, e);
            return e;
        }
        /* critical z -> f with seed s, root z -> f with seed s, derivative z -> f at p (or a function name) */
        int solve = !strcmp(t->str, "critical") ? SOLVE_CRITICAL : !strcmp(t->str, "root") ? SOLVE_ROOT
                  : !strcmp(t->str, "derivative") ? SOLVE_DERIVATIVE : 0;
        if (solve && at(p, TK_IDENT) &&
            (peek(p, 1)->kind == TK_ARROW ||
             ((peek(p, 1)->kind == TK_WITH || peek(p, 1)->kind == TK_AT) && is_function_name(p, cur(p)->str)))) {
            e = new_expr(p, E_SOLVE, t);
            e->op = solve;
            Token *v = advance(p);
            if (accept(p, TK_ARROW)) {
                e->name = v->str;
                declare_var_name(p, v->str);
                e->a = parse_arith(p);
                if (!e->a) return NULL;
            } else {
                e->base_name = v->str;
            }
            if (solve == SOLVE_DERIVATIVE) {
                if (!expect(p, TK_AT, "(as in: derivative z -> z^3 at 1)")) return NULL;
            } else {
                if (!expect(p, TK_WITH, "(as in: critical z -> z^2 + c with seed 0)")) return NULL;
                if (!at(p, TK_IDENT) || strcmp(cur(p)->str, "seed") != 0) {
                    perr(p, cur(p), "expected 'seed' after 'with', as in: with seed 0");
                    return NULL;
                }
                advance(p);
            }
            e->b = parse_arith(p);
            if (!e->b) return NULL;
            /* ... continuing from [c =] P: follow the solution from pixel to pixel, starting at P */
            if (solve != SOLVE_DERIVATIVE && at(p, TK_IDENT) && !strcmp(cur(p)->str, "continuing")) {
                advance(p);
                if (!at(p, TK_IDENT) || strcmp(cur(p)->str, "from") != 0) {
                    perr(p, cur(p), "expected 'from' after 'continuing', as in: continuing from c = .c");
                    return NULL;
                }
                advance(p);
                if (at(p, TK_IDENT) && peek(p, 1)->kind == TK_ASSIGN) {
                    e->cont_var = advance(p)->str;
                    advance(p);
                }
                e->cont_from = parse_arith(p);
                if (!e->cont_from) return NULL;
            }
            finish(p, e);
            return e;
        }
        if (is_function_name(p, t->str)) {
            e = new_expr(p, E_CALL, t);
            e->name = t->str;
            if (accept(p, TK_LPAREN)) {
                e->args = parse_args(p, TK_RPAREN, &e->nargs, "to close the argument list");
            } else if (starts_factor(p) || at(p, TK_MINUS) || at(p, TK_INT) || at(p, TK_REAL) ||
                       at(p, TK_IMAG) || at(p, TK_LBRACKET) || at(p, TK_BAR)) {
                e->args = ARENA_ARRAY(&p->prog->arena, Expr *, 1);
                e->args[0] = parse_app_arg(p);
                if (!e->args[0]) return NULL;
                e->nargs = 1;
            } else {
                perr(p, t, "'%s' is a function; call it like %s(x)", t->str, t->str);
                return NULL;
            }
            finish(p, e);
            return e;
        }
        e = new_expr(p, E_NAME, t);
        e->name = t->str;
        return e;
    }
    default:
        perr(p, t, "expected a value, found %s", tok_desc(t));
        return NULL;
    }
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    while (e && at(p, TK_LBRACKET)) {
        Token *t = advance(p);
        Expr *ix = new_expr(p, E_INDEX, t);
        ix->line = e->line;
        ix->col = e->col;
        int saved = p->abs_depth;
        p->abs_depth = 0;
        ix->a = e;
        ix->b = parse_expr(p);
        p->abs_depth = saved;
        if (!ix->b) return NULL;
        expect(p, TK_RBRACKET, "to close the index");
        finish(p, ix);
        e = ix;
    }
    return e;
}

/* exponent of ^: allows a sign, as in z^-1, and is right associative */
static Expr *parse_exponent(Parser *p) {
    if (at(p, TK_MINUS)) {
        Token *m = advance(p);
        Expr *a = parse_exponent(p);
        return a ? unary(p, TK_MINUS, a, m) : NULL;
    }
    return parse_power(p);
}

static Expr *parse_power(Parser *p) {
    Expr *base = parse_postfix(p);
    if (!base) return NULL;
    if (at(p, TK_CARET)) {
        advance(p);
        Expr *ex = parse_exponent(p);
        if (!ex) return NULL;
        base = binary(p, TK_CARET, base, ex);
    }
    if (at(p, TK_PX) && !p->no_px) { /* `8 px`: a length in pixels of the viewer, 8 * .pixel */
        Token *t = advance(p);
        Expr *px = new_expr(p, E_INTERNAL, t);
        px->name = "pixel";
        base = binary(p, TK_STAR, base, px);
    }
    return base;
}

static Expr *parse_unary(Parser *p) {
    if (at(p, TK_MINUS)) {
        Token *m = advance(p);
        Expr *a = parse_unary(p);
        return a ? unary(p, TK_MINUS, a, m) : NULL;
    }
    if (at(p, TK_PLUS)) {
        advance(p);
        return parse_unary(p);
    }
    return parse_power(p);
}

static Expr *parse_term(Parser *p) {
    Expr *e = parse_unary(p);
    while (e) {
        if (at(p, TK_STAR) || at(p, TK_SLASH) || at(p, TK_MOD)) {
            int op = advance(p)->kind;
            Expr *b = parse_unary(p);
            if (!b) return NULL;
            e = binary(p, op, e, b);
        } else if (starts_factor(p)) {
            Expr *b = parse_power(p);
            if (!b) return NULL;
            e = binary(p, TK_STAR, e, b);
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_arith(Parser *p) {
    Expr *e = parse_term(p);
    while (e && (at(p, TK_PLUS) || at(p, TK_MINUS))) {
        int op = advance(p)->kind;
        Expr *b = parse_term(p);
        if (!b) return NULL;
        e = binary(p, op, e, b);
    }
    return e;
}

static bool is_cmp(TokKind k) { return k == TK_EQ || k == TK_NE || k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE; }

static Expr *parse_cmp(Parser *p) {
    Expr *e = parse_arith(p);
    if (!e) return NULL;
    if (is_cmp(cur(p)->kind)) {
        int op = advance(p)->kind;
        Expr *b = parse_arith(p);
        if (!b) return NULL;
        e = binary(p, op, e, b);
        if (is_cmp(cur(p)->kind))
            perr(p, cur(p), "chained comparisons are not supported; write a < b and b < c");
        return e;
    }
    bool neg = false;
    if (at(p, TK_NOT) && (peek(p, 1)->kind == TK_ESCAPES || peek(p, 1)->kind == TK_VANISHES)) {
        advance(p);
        neg = true;
    }
    if (at(p, TK_ESCAPES) || at(p, TK_VANISHES)) {
        Token *t = advance(p);
        Expr *pr = ARENA_NEW(&p->prog->arena, Expr);
        pr->kind = E_PRED;
        pr->op = t->kind;
        pr->a = e;
        pr->line = e->line;
        pr->col = e->col;
        pr->end_col = e->end_col;
        finish(p, pr);
        if (neg) {
            Expr *n = unary(p, TK_NOT, pr, t);
            n->line = pr->line;
            n->col = pr->col;
            return n;
        }
        return pr;
    }
    return e;
}

static Expr *parse_not(Parser *p) {
    if (at(p, TK_NOT)) {
        Token *t = advance(p);
        Expr *a = parse_not(p);
        return a ? unary(p, TK_NOT, a, t) : NULL;
    }
    return parse_cmp(p);
}

static Expr *parse_and(Parser *p) {
    Expr *e = parse_not(p);
    while (e && at(p, TK_AND)) {
        advance(p);
        Expr *b = parse_not(p);
        if (!b) return NULL;
        e = binary(p, TK_AND, e, b);
    }
    return e;
}

static Expr *parse_expr(Parser *p) {
    Expr *e = parse_and(p);
    while (e && at(p, TK_OR)) {
        advance(p);
        Expr *b = parse_and(p);
        if (!b) return NULL;
        e = binary(p, TK_OR, e, b);
    }
    return e;
}

/* ---- types ------------------------------------------------------------- */
static TypeSyntax *parse_type(Parser *p) {
    Token *t = cur(p);
    if (!at(p, TK_IDENT)) {
        perr(p, t, "expected a type (Z, R, C, H, Bool, Color, R^3, List of C), found %s", tok_desc(t));
        return NULL;
    }
    advance(p);
    TypeSyntax *ts = ARENA_NEW(&p->prog->arena, TypeSyntax);
    ts->base = t->str;
    ts->line = t->line;
    ts->col = t->col;
    if (strcmp(t->str, "List") == 0) {
        expect(p, TK_OF, "after List (as in List of C)");
        ts->elem = parse_type(p);
        if (!ts->elem) return NULL;
        /* List of C up to 1000: the capacity (default 64) */
        if (at(p, TK_IDENT) && !strcmp(cur(p)->str, "up") && peek(p, 1)->kind == TK_IDENT && !strcmp(peek(p, 1)->str, "to")) {
            advance(p);
            advance(p);
            Token *n = cur(p);
            if (!accept(p, TK_INT) || n->num < 1 || n->num > 4096) {
                perr(p, n, "expected the largest number of items (1 to 4096) after 'up to', as in List of C up to 1000");
                return NULL;
            }
            ts->cap = (int)n->num;
        }
        return ts;
    }
    if (accept(p, TK_CARET)) {
        Token *n = cur(p);
        if (!accept(p, TK_INT) || n->num < 1 || n->num > 64) {
            perr(p, n, "expected the vector length (1 to 64) after ^, as in R^3");
            return NULL;
        }
        ts->power = (int)n->num;
    }
    return ts;
}

/* ---- statements -------------------------------------------------------- */
static Stmt *parse_stmt(Parser *p, int top_level);
static bool parse_config_extras(Parser *p, Stmt *s);

static Stmt *new_stmt(Parser *p, StmtKind kind, Token *t) {
    Stmt *s = ARENA_NEW(&p->prog->arena, Stmt);
    s->kind = kind;
    s->line = t->line;
    s->col = t->col;
    s->end_col = t->col + t->len;
    return s;
}

/* is the `while` at the cursor the end of a do ... while loop (no `do` on its line)? */
static bool while_ends_do(Parser *p) {
    for (int k = p->pos + 1; k < p->n; k++) {
        TokKind kk = p->t[k].kind;
        if (kk == TK_DO) return false;
        if (kk == TK_END || kk == TK_NEWLINE || kk == TK_EOF || kk == TK_SEMI) return true;
    }
    return true;
}

static void skip_line(Parser *p) {
    while (!at(p, TK_NEWLINE) && !at(p, TK_EOF) && !at(p, TK_SECTION) && !at(p, TK_SEMI)) advance(p);
}

/* parse statements until one of the stop tokens (not consumed) */
static Block parse_block(Parser *p, TokKind s1, TokKind s2, bool stop_at_do_while, int top_level) {
    VEC(Stmt *) v = {0};
    for (;;) {
        while (accept(p, TK_NEWLINE) || accept(p, TK_SEMI)) {}
        TokKind k = cur(p)->kind;
        if (k == TK_EOF || k == TK_SECTION || k == s1 || k == s2) break;
        if (stop_at_do_while && k == TK_WHILE && while_ends_do(p)) break;
        p->nerr_stmt = 0;
        int start = p->pos;
        Stmt *s = parse_stmt(p, top_level);
        if (s) vec_push(v, s);
        if (p->nerr_stmt) {
            skip_line(p);
        } else if (!at(p, TK_NEWLINE) && !at(p, TK_SEMI) && !at(p, TK_EOF) && !at(p, TK_SECTION) &&
                   !at(p, TK_END) && !at(p, TK_ELSE) && !(stop_at_do_while && at(p, TK_WHILE))) {
            Token *t = cur(p);
            if (t->kind == TK_INT || t->kind == TK_REAL || t->kind == TK_IMAG)
                perr(p, t, "%s can't follow here (to multiply by a number on the right, write an explicit *)", tok_desc(t));
            else
                perr(p, t, "expected the end of the line, found %s", tok_desc(t));
            skip_line(p);
        }
        if (p->pos == start) advance(p); /* guarantee progress */
    }
    Block b;
    b.n = v.len;
    b.stmts = ARENA_ARRAY(&p->prog->arena, Stmt *, v.len ? v.len : 1);
    if (v.len) memcpy(b.stmts, v.data, sizeof(Stmt *) * (size_t)v.len);
    vec_free(v);
    return b;
}

static void expect_end(Parser *p, Token *opener, const char *what) {
    if (accept(p, TK_END)) return;
    p->nerr_stmt = 0;
    perr(p, at(p, TK_EOF) || at(p, TK_SECTION) ? opener : cur(p), "missing 'end' for the '%s' on line %d", what, opener->line);
}

static Stmt *parse_if(Parser *p) {
    Token *t = advance(p); /* if (or the `if` of `else if`) */
    Stmt *s = new_stmt(p, S_IF, t);
    s->value = parse_expr(p);
    if (!s->value) return NULL;
    if (!expect(p, TK_THEN, "after the condition of 'if'")) return NULL;
    s->body = parse_block(p, TK_ELSE, TK_END, false, 0);
    if (at(p, TK_ELSE)) {
        advance(p);
        if (at(p, TK_IF)) {
            Stmt *inner = parse_if(p); /* consumes the shared `end` */
            if (!inner) return NULL;
            s->else_body.n = 1;
            s->else_body.stmts = ARENA_ARRAY(&p->prog->arena, Stmt *, 1);
            s->else_body.stmts[0] = inner;
            return s;
        }
        s->else_body = parse_block(p, TK_END, TK_END, false, 0);
    }
    expect_end(p, t, "if");
    return s;
}

static Stmt *parse_draw_body(Parser *p, Token *t);
static Stmt *parse_draw(Parser *p) {
    Token *t = advance(p);
    p->in_draw = true;
    Stmt *s = parse_draw_body(p, t);
    p->in_draw = false;
    return s;
}

static Stmt *parse_draw_body(Parser *p, Token *t) {
    Stmt *s = new_stmt(p, S_DRAW, t);
    Token *shape = cur(p);
    const char *nm = shape->kind == TK_IDENT ? shape->str : "";
    if (strcmp(nm, "point") == 0) s->shape = DRAW_POINT;
    else if (strcmp(nm, "circle") == 0) s->shape = DRAW_CIRCLE;
    else if (strcmp(nm, "disk") == 0) s->shape = DRAW_DISK;
    else if (strcmp(nm, "line") == 0 || strcmp(nm, "segment") == 0) s->shape = DRAW_LINE;
    else if (strcmp(nm, "text") == 0) s->shape = DRAW_TEXT;
    else {
        perr(p, shape, "expected point, circle, disk, line or text after 'draw', found %s", tok_desc(shape));
        return NULL;
    }
    advance(p);
    if (s->shape == DRAW_TEXT) { /* draw text "period " + n at P */
        s->text = parse_arith(p);
        if (!s->text) return NULL;
    }
    if (s->shape == DRAW_LINE) {
        if (!expect(p, TK_BETWEEN, "(as in: draw line between a and b)")) return NULL;
        s->p0 = parse_arith(p);
        if (!s->p0 || !expect(p, TK_AND, "between the two end points of the line")) return NULL;
        s->p1 = parse_arith(p);
        if (!s->p1) return NULL;
    } else {
        if (!expect(p, TK_AT, "(as in: draw circle at 0 with radius 1)")) return NULL;
        s->p0 = parse_arith(p);
        if (!s->p0) return NULL;
    }
    /* modifiers: with/and radius|color|width */
    while (at(p, TK_WITH) || (at(p, TK_AND) && peek(p, 1)->kind == TK_IDENT)) {
        advance(p);
        Token *m = cur(p);
        const char *mn = m->kind == TK_IDENT ? m->str : "";
        if (strcmp(mn, "radius") == 0) {
            advance(p);
            p->no_px = true; /* `radius 4 px` stays 4 pixels on the screen when the view is zoomed */
            s->radius = parse_arith(p);
            p->no_px = false;
            if (!s->radius) return NULL;
            s->radius_px = accept(p, TK_PX);
        } else if (strcmp(mn, "color") == 0 || strcmp(mn, "colour") == 0) {
            advance(p);
            s->color = parse_arith(p);
            if (!s->color) return NULL;
        } else if (strcmp(mn, "width") == 0 || strcmp(mn, "thickness") == 0) {
            advance(p);
            p->no_px = true;
            s->width = parse_arith(p);
            p->no_px = false;
            if (!s->width) return NULL;
            accept(p, TK_PX);
        } else {
            perr(p, m, "expected radius, color or width after '%s'", tok_kind_name(p->t[p->pos - 1].kind));
            return NULL;
        }
    }
    if (at(p, TK_IDENT) && is_draw_modifier(cur(p)->str)) {
        perr(p, cur(p), "write 'and' or 'with' before '%s'", cur(p)->str);
        return NULL;
    }
    if (accept(p, TK_IN)) {
        Token *v = cur(p);
        if (!accept(p, TK_IDENT)) {
            perr(p, v, "expected a viewer name after 'in'");
            return NULL;
        }
        s->in_viewer = v->str;
    }
    if ((s->shape == DRAW_CIRCLE || s->shape == DRAW_DISK) && !s->radius) {
        perr(p, t, "a %s needs a radius, as in: draw %s at 0 with radius 1", nm, nm);
        return NULL;
    }
    return s;
}

static Stmt *parse_define(Parser *p, int top_level) {
    Token *t = advance(p);
    if (!top_level) {
        perr(p, t, "functions must be defined at the top level of a section, not inside loops, ifs or other functions");
        return NULL;
    }
    Token *name = cur(p);
    if (!accept(p, TK_IDENT)) {
        perr(p, name, "expected the function name after 'define'");
        return NULL;
    }
    Func *f = ARENA_NEW(&p->prog->arena, Func);
    f->name = name->str;
    f->line = name->line;
    f->col = name->col;
    if (!expect(p, TK_LPAREN, "after the function name")) return NULL;
    VEC(Sym *) params = {0};
    VEC(TypeSyntax *) ptypes = {0};
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            Token *pn = cur(p);
            if (!accept(p, TK_IDENT)) {
                perr(p, pn, "expected a parameter name");
                return NULL;
            }
            Sym *s = ARENA_NEW(&p->prog->arena, Sym);
            s->name = pn->str;
            s->line = pn->line;
            s->col = pn->col;
            s->kind = SYM_LOCAL;
            s->viewer_index = -1;
            TypeSyntax *ts = NULL;
            if (accept(p, TK_COLON)) {
                ts = parse_type(p);
                if (!ts) return NULL;
            }
            vec_push(params, s);
            vec_push(ptypes, ts);
            if (!accept(p, TK_COMMA)) break;
        }
    }
    if (!expect(p, TK_RPAREN, "to close the parameter list")) return NULL;
    f->nparams = params.len;
    f->params = ARENA_ARRAY(&p->prog->arena, Sym *, params.len + 1);
    f->param_tsyn = ARENA_ARRAY(&p->prog->arena, TypeSyntax *, params.len + 1);
    for (int i = 0; i < params.len; i++) {
        f->params[i] = params.data[i];
        f->param_tsyn[i] = ptypes.data[i];
    }
    vec_free(params);
    vec_free(ptypes);
    if (!expect(p, TK_ASSIGN, "after the parameters (as in: define f(z) = z^2 + 1)")) return NULL;
    int vmark = p->lvars.len;
    for (int i = 0; i < f->nparams; i++) declare_var_name(p, f->params[i]->name);
    bool where = false;
    for (int k = p->pos; k < p->n && p->t[k].kind != TK_NEWLINE && p->t[k].kind != TK_EOF; k++)
        if (p->t[k].kind == TK_WHERE) where = true;
    if (!where) {
        f->body_expr = parse_expr(p);
        p->lvars.len = vmark;
        if (!f->body_expr) return NULL;
    } else {
        f->has_where = true;
        bool paren = accept(p, TK_LPAREN);
        VEC(Sym *) outs = {0};
        VEC(TypeSyntax *) otypes = {0};
        for (;;) {
            Token *on = cur(p);
            if (!accept(p, TK_IDENT)) {
                perr(p, on, "expected the name of an output, as in: define f(x) = (u, v) where");
                return NULL;
            }
            Sym *s = ARENA_NEW(&p->prog->arena, Sym);
            s->name = on->str;
            s->line = on->line;
            s->col = on->col;
            s->kind = SYM_LOCAL;
            s->viewer_index = -1;
            TypeSyntax *ts = NULL;
            if (accept(p, TK_COLON)) {
                ts = parse_type(p);
                if (!ts) return NULL;
            }
            vec_push(outs, s);
            vec_push(otypes, ts);
            declare_var_name(p, s->name);
            if (!paren || !accept(p, TK_COMMA)) break;
        }
        if (paren && !expect(p, TK_RPAREN, "to close the list of outputs")) return NULL;
        if (!expect(p, TK_WHERE, "after the outputs")) return NULL;
        f->noutputs = outs.len;
        f->outputs = ARENA_ARRAY(&p->prog->arena, Sym *, outs.len);
        f->output_tsyn = ARENA_ARRAY(&p->prog->arena, TypeSyntax *, outs.len);
        for (int i = 0; i < outs.len; i++) {
            f->outputs[i] = outs.data[i];
            f->output_tsyn[i] = otypes.data[i];
        }
        vec_free(outs);
        vec_free(otypes);
        p->func_depth++;
        int saved_loops = p->loop_depth;
        p->loop_depth = 0;
        f->body = parse_block(p, TK_END, TK_END, false, 0);
        p->loop_depth = saved_loops;
        p->func_depth--;
        p->lvars.len = vmark;
        expect_end(p, t, "define");
    }
    Stmt *s = new_stmt(p, S_DEFINE, t);
    s->fn = f;
    vec_push(p->prog->funcs, f);
    return s;
}

static bool is_lvalue(Expr *e) {
    if (!e) return false;
    if (e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER) return !e->paren;
    if (e->kind == E_INDEX) return is_lvalue(e->a);
    return false;
}

static const char *tool_event_names[TOOL_EV_COUNT] = {"press", "drag", "release", "click", "hover", "show", "hide"};

static Stmt *parse_stmt(Parser *p, int top_level) {
    Token *t = cur(p);
    if (t->kind == TK_IDENT && !strcmp(t->str, "on") && peek(p, 1)->kind == TK_IDENT && peek(p, 2)->kind == TK_DO) {
        /* on press do ... end (tools; the checker says where else not) */
        advance(p);
        Token *ev = advance(p);
        advance(p);
        Stmt *s = new_stmt(p, S_ON, t);
        s->op = -1;
        for (int k = 0; k < TOOL_EV_COUNT; k++)
            if (!strcmp(ev->str, tool_event_names[k])) s->op = k;
        if (s->op < 0) {
            perr(p, ev, "'%s' is not an event; tools can handle press, drag, release, click, hover, show and hide", ev->str);
            return NULL;
        }
        if (!top_level) {
            perr(p, t, "'on %s do' must be at the top of a tool section, not inside another block", ev->str);
            return NULL;
        }
        s->body = parse_block(p, TK_END, TK_END, false, 0);
        expect_end(p, t, "on");
        return s;
    }
    if (t->kind == TK_IDENT && !strcmp(t->str, "erase")) {
        Token *n = peek(p, 1);
        bool all = n->kind == TK_IDENT && !strcmp(n->str, "all");
        TokKind k = peek(p, all ? 2 : 1)->kind;
        if (k == TK_NEWLINE || k == TK_EOF || k == TK_SEMI || k == TK_END || k == TK_ELSE || k == TK_SECTION) {
            advance(p);
            if (all) advance(p);
            Stmt *s = new_stmt(p, S_ERASE, t);
            s->op = all; /* erase all: the drawings of every tool */
            return s;
        }
    }
    switch (t->kind) {
    case TK_IF: return parse_if(p);
    case TK_WHILE: {
        advance(p);
        Stmt *s = new_stmt(p, S_WHILE, t);
        s->value = parse_expr(p);
        if (!s->value || !expect(p, TK_DO, "after the condition of 'while'")) return NULL;
        p->loop_depth++;
        s->body = parse_block(p, TK_END, TK_END, false, 0);
        p->loop_depth--;
        expect_end(p, t, "while");
        return s;
    }
    case TK_DO: {
        advance(p);
        Stmt *s = new_stmt(p, S_DOWHILE, t);
        p->loop_depth++;
        s->body = parse_block(p, TK_EOF, TK_EOF, true, 0);
        p->loop_depth--;
        if (!at(p, TK_WHILE)) {
            p->nerr_stmt = 0;
            perr(p, t, "this 'do' loop needs a closing 'while <condition> end'");
            return NULL;
        }
        advance(p);
        s->value = parse_expr(p);
        if (!s->value) return NULL;
        expect_end(p, t, "do");
        return s;
    }
    case TK_FOR: {
        advance(p);
        Token *v = cur(p);
        if (!accept(p, TK_IDENT)) {
            perr(p, v, "expected the loop variable after 'for'");
            return NULL;
        }
        declare_var_name(p, v->str);
        if (!expect(p, TK_IN, "after the loop variable (as in: for k in 1..10 do)")) return NULL;
        Stmt *s = new_stmt(p, S_FOR, t);
        Expr *var = new_expr(p, E_NAME, v);
        var->name = v->str;
        s->target = var;
        s->from = parse_arith(p);
        if (!s->from) return NULL;
        if (accept(p, TK_DOTDOT)) {
            s->to = parse_arith(p);
            if (!s->to) return NULL;
            if (accept(p, TK_BY)) {
                s->step = parse_arith(p);
                if (!s->step) return NULL;
            }
        } else {
            s->kind = S_FOREACH;
        }
        if (!expect(p, TK_DO, "to start the body of the 'for' loop")) return NULL;
        p->loop_depth++;
        s->body = parse_block(p, TK_END, TK_END, false, 0);
        p->loop_depth--;
        expect_end(p, t, "for");
        return s;
    }
    case TK_BREAK: case TK_CONTINUE:
        advance(p);
        if (p->loop_depth == 0) {
            perr(p, t, "'%s' can only be used inside a loop", t->kind == TK_BREAK ? "break" : "continue");
            return NULL;
        }
        return new_stmt(p, t->kind == TK_BREAK ? S_BREAK : S_CONTINUE, t);
    case TK_RETURN:
        advance(p);
        return new_stmt(p, S_RETURN, t);
    case TK_DRAW: return parse_draw(p);
    case TK_DEFINE: return parse_define(p, top_level);
    case TK_END:
        advance(p);
        perr(p, t, "this 'end' does not close anything");
        return NULL;
    case TK_ELSE:
        advance(p);
        perr(p, t, "'else' without a matching 'if'");
        return NULL;
    case TK_LPAREN: {
        /* (a, b) = f(x)  or  (a, b) := f(x) */
        int depth = 0, k = p->pos;
        for (; k < p->n; k++) {
            if (p->t[k].kind == TK_LPAREN) depth++;
            else if (p->t[k].kind == TK_RPAREN && --depth == 0) break;
        }
        if (k + 1 < p->n && (p->t[k + 1].kind == TK_ASSIGN || p->t[k + 1].kind == TK_DECLARE)) {
            for (int j = p->pos + 1; j < k; j++)
                if (p->t[j].kind == TK_IDENT) declare_var_name(p, p->t[j].str);
            advance(p);
            Stmt *s = new_stmt(p, S_DESTRUCTURE, t);
            s->targets = parse_args(p, TK_RPAREN, &s->ntargets, "to close the list of names");
            for (int i = 0; i < s->ntargets; i++)
                if (!is_lvalue(s->targets[i])) {
                    perr(p, t, "only variables can appear on the left of '='");
                    return NULL;
                }
            s->declare = advance(p)->kind == TK_DECLARE;
            s->value = parse_expr(p);
            return s->value ? s : NULL;
        }
        break;
    }
    default: break;
    }

    if (at(p, TK_IDENT)) {
        TokKind k = peek(p, 1)->kind;
        if (k == TK_DECLARE || k == TK_COLON || k == TK_ASSIGN || k == TK_PLUSEQ || k == TK_MINUSEQ ||
            k == TK_STAREQ || k == TK_SLASHEQ)
            declare_var_name(p, cur(p)->str);
    }
    Expr *e = parse_expr(p);
    if (!e) return NULL;
    if (at(p, TK_COLON) || at(p, TK_DECLARE)) {
        bool typed = at(p, TK_COLON);
        Token *op = advance(p);
        if (e->kind != E_NAME && e->kind != E_INTERNAL && e->kind != E_MEMBER) {
            perr(p, op, "only a plain name can be declared on the left of '%s'", typed ? ":" : ":=");
            return NULL;
        }
        Stmt *s = new_stmt(p, S_DECL, t);
        s->target = e;
        if (typed) {
            s->tsyn = parse_type(p);
            if (!s->tsyn) return NULL;
            if (accept(p, TK_ASSIGN)) {
                s->value = parse_expr(p);
                if (!s->value) return NULL;
            }
        } else {
            s->value = parse_expr(p);
            if (!s->value) return NULL;
        }
        if (!parse_config_extras(p, s)) return NULL;
        return s;
    }
    if (at(p, TK_ASSIGN) || at(p, TK_PLUSEQ) || at(p, TK_MINUSEQ) || at(p, TK_STAREQ) || at(p, TK_SLASHEQ)) {
        Token *op = advance(p);
        if (!is_lvalue(e)) {
            perr(p, op, "the left side of '%.*s' must be a variable", op->len, op->text);
            return NULL;
        }
        Stmt *s = new_stmt(p, S_ASSIGN, t);
        s->op = op->kind;
        s->target = e;
        s->value = parse_expr(p);
        if (!s->value) return NULL;
        if (s->op == TK_ASSIGN && !parse_config_extras(p, s)) return NULL;
        return s;
    }
    if (at(p, TK_EQ) && 0) return NULL;
    Stmt *s = new_stmt(p, S_EXPR, t);
    s->value = e;
    if (e->kind != E_CALL && e->kind != E_STRING) { /* a line of text is a note in @config (the checker says where not) */
        if (at(p, TK_NEWLINE) || at(p, TK_EOF) || at(p, TK_SEMI))
            perr(p, t, "this line computes a value but does nothing with it (did you mean to assign it with '='?)");
        else
            perr(p, cur(p), "expected '=' or the end of the line, found %s", tok_desc(cur(p)));
        return NULL;
    }
    return s;
}

/* After a value in @config: an optional slider range `in a..b` or choice `in ["a", "b"]`, then an
   optional label in quotes. */
static bool parse_config_extras(Parser *p, Stmt *s) {
    if (accept(p, TK_IN)) {
        if (at(p, TK_LBRACKET)) {
            Token *lb = advance(p);
            VEC(char *) items = {0};
            while (!at(p, TK_RBRACKET)) {
                Token *it = cur(p);
                if (!accept(p, TK_STRING)) {
                    perr(p, it, "the choices are texts in quotes, as in: in [\"rainbow\", \"grays\"]");
                    vec_free(items);
                    return false;
                }
                vec_push(items, it->str);
                if (!accept(p, TK_COMMA)) break;
            }
            if (!expect(p, TK_RBRACKET, "to close the list of choices")) {
                vec_free(items);
                return false;
            }
            if (!items.len) {
                perr(p, lb, "give at least one choice, as in: in [\"rainbow\", \"grays\"]");
                return false;
            }
            s->nchoices = items.len;
            s->choices = ARENA_ARRAY(&p->prog->arena, char *, items.len);
            memcpy(s->choices, items.data, sizeof(char *) * (size_t)items.len);
            vec_free(items);
        } else {
            s->range_lo = parse_arith(p);
            if (!s->range_lo || !expect(p, TK_DOTDOT, "in the range (as in: in 0..1)")) return false;
            s->range_hi = parse_arith(p);
            if (!s->range_hi) return false;
        }
    }
    if (at(p, TK_STRING)) s->label = advance(p)->str;
    return true;
}

/* ---- sections ---------------------------------------------------------- */
static Section *new_section(Parser *p, SectionKind kind, int line) {
    vec_clear(p->lvars);
    Section *s = ARENA_NEW(&p->prog->arena, Section);
    s->kind = kind;
    s->line = line;
    vec_push(p->prog->sections, s);
    return s;
}

static void parse_section_header(Parser *p, Section *s, Token *h) {
    VEC(Token *) args = {0};
    while (!at(p, TK_NEWLINE) && !at(p, TK_EOF)) vec_push(args, advance(p));
    int k = 0;
    if (s->kind == SEC_VIEWER) {
        if (args.len < 2 || args.data[0]->kind != TK_IDENT || args.data[1]->kind != TK_IDENT) {
            p->nerr_stmt = 0;
            perr(p, h, "a viewer header needs a name and a variable, as in: @viewer plane z \"z-plane\" \"Ctrl + 1\"");
        } else {
            s->name = args.data[0]->str;
            s->var = args.data[1]->str;
            k = 2;
        }
    }
    for (; k < args.len; k++) {
        Token *a = args.data[k];
        if (a->kind == TK_STRING && !s->display && s->kind != SEC_GLOBAL)
            s->display = a->str; /* @config "Title": a titled group in the config panel */
        else if (a->kind == TK_STRING && !s->shortcut && s->kind != SEC_CONFIG && s->kind != SEC_GLOBAL)
            s->shortcut = a->str;
        else if (a->kind == TK_STRING && !s->icon && s->kind == SEC_TOOL)
            s->icon = a->str;
        else if (a->kind == TK_IDENT && strcmp(a->str, "projective") == 0 && s->kind == SEC_VIEWER)
            s->projective = true;
        else if (a->kind == TK_IDENT && strcmp(a->str, "button") == 0 && s->kind == SEC_TOOL)
            s->button = true;
        else {
            p->nerr_stmt = 0;
            perr(p, a, "unexpected %s in the section header", tok_desc(a));
            break;
        }
    }
    vec_free(args);
}

/* Parse a whole token stream (prelude or user script) into sections. */
void parse_tokens(Program *prog, Token *toks, int ntoks, const char **builtins, int nbuiltins, const char **ufuncs,
                  int nufuncs, const char **gvars, int ngvars, bool prelude) {
    Parser P = {0};
    Parser *p = &P;
    p->prog = prog;
    p->t = toks;
    p->n = ntoks;
    p->builtins = builtins;
    p->nbuiltins = nbuiltins;
    p->ufuncs = ufuncs;
    p->nufuncs = nufuncs;
    p->gvars = gvars;
    p->ngvars = ngvars;
    bool has_headers = false;
    for (int i = 0; i < ntoks; i++)
        if (toks[i].kind == TK_SECTION) has_headers = true;
    int funcs_before = prog->funcs.len;

    if (!has_headers) {
        Section *s = new_section(p, prelude ? SEC_GLOBAL : SEC_VIEWER, 1);
        if (!prelude) {
            s->name = "plane";
            s->var = "z";
            s->display = "z-plane";
            s->shortcut = "Ctrl + 1";
            s->implicit = true;
        }
        s->body = parse_block(p, TK_EOF, TK_EOF, false, 1);
    } else {
        while (accept(p, TK_NEWLINE)) {}
        if (!at(p, TK_SECTION) && !at(p, TK_EOF)) { /* code before the first header: globals */
            Section *s = new_section(p, SEC_GLOBAL, cur(p)->line);
            s->body = parse_block(p, TK_EOF, TK_EOF, false, 1);
        }
        while (at(p, TK_SECTION)) {
            Token *h = advance(p);
            SectionKind kind;
            const char *dot = strchr(h->str, '.');
            char *of_viewer = NULL;
            if (dot && strcmp(dot + 1, "tool") == 0) {
                of_viewer = arena_strndup(&p->prog->arena, h->str, (size_t)(dot - h->str));
                kind = SEC_TOOL;
            } else if (dot) {
                p->nerr_stmt = 0;
                perr(p, h, "unknown section @%s (did you mean @%.*s.tool?)", h->str, (int)(dot - h->str), h->str);
                kind = SEC_GLOBAL;
            } else if (strcmp(h->str, "viewer") == 0) kind = SEC_VIEWER;
            else if (strcmp(h->str, "tool") == 0) kind = SEC_TOOL;
            else if (strcmp(h->str, "config") == 0) kind = SEC_CONFIG;
            else if (strcmp(h->str, "global") == 0 || strcmp(h->str, "globals") == 0) kind = SEC_GLOBAL;
            else {
                p->nerr_stmt = 0;
                perr(p, h, "unknown section @%s (expected @viewer, @tool, @NAME.tool, @config or @global)", h->str);
                kind = SEC_GLOBAL;
            }
            Section *s = new_section(p, kind, h->line);
            s->of_viewer = of_viewer;
            p->nerr_stmt = 0;
            parse_section_header(p, s, h);
            s->body = parse_block(p, TK_EOF, TK_EOF, false, 1);
        }
    }
    if (prelude)
        for (int i = funcs_before; i < prog->funcs.len; i++) prog->funcs.data[i]->prelude = true;
    vec_free(p->lvars);
}
