/* autodiff.c - `derivative`, `root` and `critical` (part of the checker; included after check.c).
 *
 * Forward-mode automatic differentiation, up to second order, done as a
 * source transformation of the checked AST:
 *   - every complex variable whose value depends on the variable we
 *     differentiate with respect to ("active") gets companion variables that
 *     hold its first (and second) derivative;
 *   - every assignment to an active variable also computes the derivatives,
 *     through temporaries, so nothing is computed twice;
 *   - if / while / for keep their structure, and conditions only use values,
 *     so loops that stop when a point escapes are fine;
 *   - user functions called with active arguments get a "jet" version that
 *     takes and returns the derivatives too.
 * Only holomorphic operations can be differentiated: + - * /, powers, exp,
 * log, sqrt, trigonometric and hyperbolic functions and their inverses.
 * re, im, |z|, arg and comparisons may influence control flow but not values.
 *
 * `root t -> E with seed s` becomes a call to a synthetic function running
 * Newton's method on E, `critical` on E', `derivative t -> E at p` returns E'.
 * With `continuing from c = P` (viewers only) the solution is carried from
 * pixel to pixel by the flood fill in flood.c: the synthetic function then
 * also walks c from the neighbouring pixel's value to its own in small steps. */

typedef struct Jet { Expr *v, *d1, *d2; } Jet; /* NULL derivative = zero */
typedef struct JetVar { Sym *v, *d1, *d2; } JetVar;
typedef struct SymPair { Sym *from, *to; } SymPair;
typedef VEC(Stmt *) StmtVec;
/* a point of the sphere computed from the variable: jets of the two coordinates (a, b) of a lift [a : b].  The
   lift may be scaled (__pmake makes the largest coordinate 1) by a factor the jets treat as a constant; nothing
   computed from them depends on the scale (notes/projective.md, section 4). */
typedef struct PJet { Jet a, b; } PJet;
typedef struct JetBind { Sym *src; Jet j; } JetBind;    /* a parameter of an inlined function, and its jet */
typedef struct PJetBind { Sym *src; PJet j; } PJetBind;

typedef struct AD {
    Checker *c;
    Program *p;
    Expr *at;              /* position used for new nodes and messages */
    int order;             /* 1 or 2 */
    Func *fn;              /* function being built (gets the temporaries) */
    VEC(SymPair) map;      /* source local -> local of fn */
    SymVec act_src;        /* active source variables (complex) */
    SymVec taint_src;      /* source variables (real, integer, bool) computed from active ones */
    VEC(JetVar) jets;      /* derivative variables of fn's active locals (keyed by v) */
    StmtVec *out;          /* statements being emitted */
    Sym *var_src;          /* the variable we differentiate with respect to (its derivative is 1) */
    bool failed;
    int ntemp;
    VEC(JetBind) cbind;    /* parameters of inlined functions (see ad_bind_params) */
    VEC(PJetBind) pbind;
    bool pmode;            /* the variable is a point of the sphere, moved in a chart: */
    PJet pvar;             /* ... its jets (see ad_pseed) */
} AD;

typedef struct JetFuncCache { Func *src, *jet; int order; struct JetFuncCache *next; } JetFuncCache;
static JetFuncCache *jet_cache;  /* per program load: reset in check_program via jet_cache_reset */

static void jet_cache_reset(void) {
    while (jet_cache) {
        JetFuncCache *n = jet_cache->next;
        free(jet_cache);
        jet_cache = n;
    }
}

/* ---- small helpers ------------------------------------------------------------ */
static bool sym_in(SymVec *v, Sym *s) {
    for (int i = 0; i < v->len; i++)
        if (v->data[i] == s) return true;
    return false;
}

static void ad_fail(AD *ad, Expr *where, const char *fmt, ...) FS_PRINTF(3, 4);
static void ad_fail(AD *ad, Expr *where, const char *fmt, ...) {
    if (ad->failed) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Expr *e = where && where->line > 0 && where->line < PRELUDE_LINE0 ? where : ad->at;
    prog_diag(ad->p, DIAG_ERROR, e->line, e->col, e->end_col, "can't differentiate this: %s", buf);
    ad->failed = true;
}

static Expr *ad_node(AD *ad, ExprKind k, Type *t) {
    Expr *e = ARENA_NEW(&ad->p->arena, Expr);
    e->kind = k;
    e->type = t;
    e->line = ad->at->line;
    e->col = ad->at->col;
    e->end_col = ad->at->end_col;
    return e;
}
static Expr *ad_name(AD *ad, Sym *s) {
    Expr *e = ad_node(ad, E_NAME, s->type);
    e->name = s->name;
    e->sym = s;
    return e;
}
static Expr *ad_cconst(AD *ad, float re, float im) {
    Expr *e = ad_node(ad, E_COLOR_CONST, t_complex);
    e->cval[0] = re;
    e->cval[1] = im;
    return e;
}
static Expr *ad_int(AD *ad, int v) {
    Expr *e = ad_node(ad, E_INT, t_int);
    e->ival = v;
    e->rval = v;
    return e;
}
static Expr *ad_real(AD *ad, float v) {
    Expr *e = ad_node(ad, E_REAL, t_real);
    e->rval = v;
    return e;
}
static Expr *ad_bool(AD *ad, bool v) {
    Expr *e = ad_node(ad, E_BOOL, t_bool);
    e->ival = v;
    return e;
}
static bool ad_is_const(Expr *e, float re) {
    return e && e->kind == E_COLOR_CONST && e->type == t_complex && e->cval[0] == re && e->cval[1] == 0;
}
static Expr *ad_to(AD *ad, Expr *e, Type *t) {
    if (!e || e->type == t) return e;
    Expr *x = ad_node(ad, E_CONVERT, t);
    x->a = e;
    return x;
}
static Expr *ad_bin(AD *ad, int op, Expr *a, Expr *b) {
    Type *t = a->type == t_complex || b->type == t_complex ? t_complex : a->type == t_real || b->type == t_real ? t_real : a->type;
    bool cmp = op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE || op == TK_EQ || op == TK_NE;
    bool logic = op == TK_AND || op == TK_OR;
    if (!logic) {
        if (op == TK_SLASH && t == t_int) t = t_real;
        a = ad_to(ad, a, t);
        b = ad_to(ad, b, t);
    }
    Expr *e = ad_node(ad, E_BINARY, cmp || logic ? t_bool : t);
    e->op = op;
    e->a = a;
    e->b = b;
    return e;
}
static Expr *ad_not(AD *ad, Expr *a) {
    Expr *e = ad_node(ad, E_UNARY, t_bool);
    e->op = TK_NOT;
    e->a = a;
    return e;
}
static Expr *ad_abs(AD *ad, Expr *a) {
    Expr *e = ad_node(ad, E_ABS, a->type == t_int ? t_int : t_real);
    e->a = a;
    return e;
}
static Expr *ad_neg(AD *ad, Expr *a) {
    Expr *e = ad_node(ad, E_UNARY, a->type);
    e->op = TK_MINUS;
    e->a = a;
    return e;
}
static Expr *ad_powi(AD *ad, Expr *a, int n) {
    if (n == 1) return a;
    if (n == 0) return ad_cconst(ad, 1, 0);
    Expr *e = ad_node(ad, E_POWI, a->type);
    e->a = a;
    e->ival = n;
    return e;
}
static Expr *ad_call(AD *ad, Func *f, Expr **args, int n) {
    Expr *e = ad_node(ad, E_CALL, f->ret);
    e->name = f->name;
    e->fn = f;
    e->nargs = n;
    e->args = ARENA_ARRAY(&ad->p->arena, Expr *, n ? n : 1);
    for (int i = 0; i < n; i++) e->args[i] = ad_to(ad, args[i], f->params[i]->type);
    return e;
}
static Expr *ad_intrinsic(AD *ad, int id, Type *t, Expr **args, int n) {
    Expr *e = ad_node(ad, E_CALL, t);
    e->name = "(internal)";
    e->intrinsic = id;
    e->nargs = n;
    e->args = ARENA_ARRAY(&ad->p->arena, Expr *, n ? n : 1);
    for (int i = 0; i < n; i++) e->args[i] = args[i];
    return e;
}

/* the prelude function `name` taking one complex number */
static Func *ad_prelude_c(AD *ad, const char *name) {
    for (int i = 0; i < ad->p->funcs.len; i++) {
        Func *f = ad->p->funcs.data[i];
        if (!strcmp(f->name, name) && f->prelude && f->nparams == 1 && f->params[0]->type == t_complex) {
            if (f->check_state == 0) check_func(ad->c, f);
            return f;
        }
    }
    return NULL;
}
static Expr *ad_fcall(AD *ad, const char *name, Expr *a) {
    Func *f = ad_prelude_c(ad, name);
    if (!f) {
        ad_fail(ad, NULL, "internal error: no built-in %s", name);
        return ad_cconst(ad, 0, 0);
    }
    return ad_call(ad, f, &a, 1);
}

/* ---- statements and temporaries -------------------------------------------------- */
static Sym *ad_local(AD *ad, const char *name, Type *t) {
    Sym *s = new_sym(ad->c, name, t, SYM_LOCAL, ad->at->line, ad->at->col);
    s->owner = ad->fn;
    vec_push(ad->fn->locals, s);
    return s;
}
static Sym *ad_temp(AD *ad, Type *t) { return ad_local(ad, arena_printf(&ad->p->arena, "__t%d", ++ad->ntemp), t); }

static Stmt *ad_stmt_new(AD *ad, StmtKind k) {
    Stmt *s = ARENA_NEW(&ad->p->arena, Stmt);
    s->kind = k;
    s->line = ad->at->line;
    s->col = ad->at->col;
    return s;
}
static void ad_emit(AD *ad, Stmt *s) { vec_push(*ad->out, s); }
static void ad_assign(AD *ad, Sym *x, Expr *v) {
    Stmt *s = ad_stmt_new(ad, S_ASSIGN);
    s->op = TK_ASSIGN;
    s->target = ad_name(ad, x);
    s->value = ad_to(ad, v, x->type);
    s->internal = x->kind == SYM_EXEC;
    ad_emit(ad, s);
}
static Block ad_take_block(AD *ad, StmtVec *v) {
    Block b;
    b.n = v->len;
    b.stmts = ARENA_ARRAY(&ad->p->arena, Stmt *, v->len ? v->len : 1);
    if (v->len) memcpy(b.stmts, v->data, sizeof(Stmt *) * (size_t)v->len);
    vec_free(*v);
    return b;
}
/* keep e in a temporary unless it is already a name or a constant */
static Expr *ad_hold(AD *ad, Expr *e) {
    if (!e || e->kind == E_NAME || e->kind == E_COLOR_CONST || e->kind == E_INT || e->kind == E_REAL) return e;
    Sym *t = ad_temp(ad, e->type);
    ad_assign(ad, t, e);
    return ad_name(ad, t);
}

/* ---- source -> target symbols ----------------------------------------------------- */
static Sym *ad_map(AD *ad, Sym *s) {
    if (!s || s->kind != SYM_LOCAL) return s;
    for (int i = ad->map.len - 1; i >= 0; i--) /* (the latest: parameters of inlined functions are mapped again) */
        if (ad->map.data[i].from == s) return ad->map.data[i].to;
    Sym *t = ad_local(ad, s->name, s->type);
    SymPair pr = {s, t};
    vec_push(ad->map, pr);
    return t;
}
static void ad_map_set(AD *ad, Sym *from, Sym *to) {
    SymPair pr = {from, to};
    vec_push(ad->map, pr);
}
static JetVar *ad_jetvar(AD *ad, Sym *target) {
    for (int i = 0; i < ad->jets.len; i++)
        if (ad->jets.data[i].v == target) return &ad->jets.data[i];
    return NULL;
}
/* derivative variables of an active source variable */
static JetVar ad_jets_of(AD *ad, Sym *src) {
    Sym *v = ad_map(ad, src);
    JetVar *j = ad_jetvar(ad, v);
    if (j) return *j;
    JetVar n = {v, ad_local(ad, arena_printf(&ad->p->arena, "%s'", src->name), t_complex),
                ad->order >= 2 ? ad_local(ad, arena_printf(&ad->p->arena, "%s''", src->name), t_complex) : NULL};
    vec_push(ad->jets, n);
    return n;
}

/* deep copy of a source expression into the target function */
static Expr *ad_copy(AD *ad, Expr *e) {
    if (!e) return NULL;
    Expr *x = ARENA_NEW(&ad->p->arena, Expr);
    *x = *e;
    x->sym = ad_map(ad, e->sym);
    x->a = ad_copy(ad, e->a);
    x->b = ad_copy(ad, e->b);
    if (e->nargs) {
        x->args = ARENA_ARRAY(&ad->p->arena, Expr *, e->nargs);
        for (int i = 0; i < e->nargs; i++) x->args[i] = ad_copy(ad, e->args[i]);
    }
    return x;
}

/* deep copy with parameters replaced by argument expressions (inlining one-line functions) */
static Expr *ad_subst(AD *ad, Expr *e, Sym **from, Expr **to, int n) {
    if (!e) return NULL;
    if (e->kind == E_NAME)
        for (int i = 0; i < n; i++)
            if (e->sym == from[i]) return to[i];
    Expr *x = ARENA_NEW(&ad->p->arena, Expr);
    *x = *e;
    x->a = ad_subst(ad, e->a, from, to, n);
    x->b = ad_subst(ad, e->b, from, to, n);
    if (e->nargs) {
        x->args = ARENA_ARRAY(&ad->p->arena, Expr *, e->nargs);
        for (int i = 0; i < e->nargs; i++) x->args[i] = ad_subst(ad, e->args[i], from, to, n);
    }
    return x;
}

/* ---- activity ------------------------------------------------------------------- */
static bool ad_bound(AD *ad, Sym *s) {
    for (int i = 0; i < ad->cbind.len; i++)
        if (ad->cbind.data[i].src == s) return true;
    for (int i = 0; i < ad->pbind.len; i++)
        if (ad->pbind.data[i].src == s) return true;
    return false;
}

static bool ad_depends(AD *ad, Expr *e) {
    if (!e) return false;
    if ((e->kind == E_NAME) && e->sym &&
        (sym_in(&ad->act_src, e->sym) || sym_in(&ad->taint_src, e->sym) || ad_bound(ad, e->sym)))
        return true;
    if (ad_depends(ad, e->a) || ad_depends(ad, e->b)) return true;
    for (int i = 0; i < e->nargs; i++)
        if (ad_depends(ad, e->args[i])) return true;
    return false;
}

static bool ad_mark(AD *ad, Expr *target, bool dep) {
    if (!dep || !target || target->kind != E_NAME || !target->sym || target->sym->kind != SYM_LOCAL) return false;
    Sym *s = target->sym;
    SymVec *v = s->type == t_complex ? &ad->act_src : &ad->taint_src;
    if (sym_in(v, s)) return false;
    vec_push(*v, s);
    return true;
}

static bool ad_analyze_block(AD *ad, Block *b) {
    bool changed = false;
    for (int i = 0; i < b->n; i++) {
        Stmt *s = b->stmts[i];
        switch (s->kind) {
        case S_DECL: case S_ASSIGN: changed |= ad_mark(ad, s->target, ad_depends(ad, s->value)); break;
        case S_DESTRUCTURE: {
            bool dep = ad_depends(ad, s->value);
            for (int k = 0; k < s->ntargets; k++) changed |= ad_mark(ad, s->targets[k], dep);
            break;
        }
        case S_FOR: {
            bool dep = ad_depends(ad, s->from) || ad_depends(ad, s->to) || ad_depends(ad, s->step);
            if (dep && s->sym && !sym_in(&ad->taint_src, s->sym)) {
                vec_push(ad->taint_src, s->sym);
                changed = true;
            }
            changed |= ad_analyze_block(ad, &s->body);
            break;
        }
        case S_FOREACH: {
            if (ad_depends(ad, s->from) && s->sym) {
                SymVec *v = s->sym->type == t_complex ? &ad->act_src : &ad->taint_src;
                if (!sym_in(v, s->sym)) {
                    vec_push(*v, s->sym);
                    changed = true;
                }
            }
            changed |= ad_analyze_block(ad, &s->body);
            break;
        }
        default:
            changed |= ad_analyze_block(ad, &s->body);
            changed |= ad_analyze_block(ad, &s->else_body);
            break;
        }
    }
    return changed;
}

/* ---- jets of expressions ------------------------------------------------------------- */
static Jet ad_jet(AD *ad, Expr *e);
static Func *ad_jet_func(AD *ad, Func *src);
static PJet ad_pjet(AD *ad, Expr *e);
typedef struct BindMark { int map, cbind, pbind; } BindMark;
static BindMark ad_bind_params(AD *ad, Expr *call);
static void ad_unbind(AD *ad, BindMark m) {
    ad->map.len = m.map;
    ad->cbind.len = m.cbind;
    ad->pbind.len = m.pbind;
}
static bool ad_has_proj_param(Func *f) {
    for (int i = 0; i < f->nparams; i++)
        if (f->params[i]->type == t_proj) return true;
    return false;
}

/* x*y with NULL meaning zero */
static Expr *jmul(AD *ad, Expr *x, Expr *y) {
    if (!x || !y) return NULL;
    if (ad_is_const(x, 1)) return y;
    if (ad_is_const(y, 1)) return x;
    return ad_bin(ad, TK_STAR, x, y);
}
static Expr *jadd(AD *ad, Expr *x, Expr *y) {
    if (!x) return y;
    if (!y) return x;
    return ad_bin(ad, TK_PLUS, x, y);
}
static Expr *jsub(AD *ad, Expr *x, Expr *y) {
    if (!y) return x;
    if (!x) return ad_neg(ad, y);
    return ad_bin(ad, TK_MINUS, x, y);
}
static Expr *jdiv(AD *ad, Expr *x, Expr *y) { return x ? ad_bin(ad, TK_SLASH, x, y) : NULL; }
static Expr *jscale(AD *ad, float k, Expr *x) { return x ? jmul(ad, ad_cconst(ad, k, 0), x) : NULL; }

static Jet jet_hold(AD *ad, Expr *v, Expr *d1, Expr *d2) {
    Jet j;
    j.v = ad_hold(ad, ad_to(ad, v, t_complex));
    j.d1 = ad_hold(ad, d1);
    j.d2 = ad->order >= 2 ? ad_hold(ad, d2) : NULL;
    return j;
}

/* chain rule for g(a) given g' and g'' at a */
static Jet jet_chain(AD *ad, Jet a, Expr *v, Expr *f1, Expr *f2) {
    Expr *d1 = jmul(ad, f1, a.d1);
    Expr *d2 = NULL;
    if (ad->order >= 2) d2 = jadd(ad, jmul(ad, f2, jmul(ad, a.d1, a.d1)), jmul(ad, f1, a.d2));
    return jet_hold(ad, v, d1, d2);
}

static Jet jet_const(AD *ad, Expr *e) {
    Jet j = {ad_to(ad, ad_copy(ad, e), t_complex), NULL, NULL};
    return j;
}

/* a^n, n an integer constant */
static Jet jet_powi(AD *ad, Jet a, int n) {
    if (n == 0) return jet_const(ad, ad_cconst(ad, 1, 0));
    if (n == 1) return a;
    Expr *v = ad_powi(ad, a.v, n);
    Expr *pm1 = ad_hold(ad, ad_powi(ad, a.v, n - 1));
    Expr *f1 = jscale(ad, (float)n, pm1);
    Expr *f2 = ad->order >= 2 ? jscale(ad, (float)n * (float)(n - 1), ad_powi(ad, a.v, n - 2)) : NULL;
    return jet_chain(ad, a, v, f1, f2);
}

/* elementary holomorphic functions: v = g(a), with g' and g'' written with a and v */
static bool jet_elementary(AD *ad, const char *n, Jet a, Jet *out) {
    Expr *v, *f1, *f2 = NULL;
    Expr *one = ad_cconst(ad, 1, 0);
    bool o2 = ad->order >= 2;
    if (!strcmp(n, "exp")) {
        v = ad_hold(ad, ad_fcall(ad, "exp", a.v));
        f1 = v;
        f2 = v;
    } else if (!strcmp(n, "log") || !strcmp(n, "ln")) {
        v = ad_fcall(ad, "log", a.v);
        f1 = ad_hold(ad, ad_bin(ad, TK_SLASH, one, a.v));
        if (o2) f2 = ad_neg(ad, ad_bin(ad, TK_STAR, f1, f1));
    } else if (!strcmp(n, "sqrt")) {
        v = ad_hold(ad, ad_fcall(ad, "sqrt", a.v));
        f1 = ad_hold(ad, ad_bin(ad, TK_SLASH, ad_cconst(ad, 0.5f, 0), v));
        if (o2) f2 = ad_neg(ad, ad_bin(ad, TK_SLASH, f1, ad_bin(ad, TK_STAR, ad_cconst(ad, 2, 0), a.v)));
    } else if (!strcmp(n, "sin")) {
        v = ad_hold(ad, ad_fcall(ad, "sin", a.v));
        f1 = ad_fcall(ad, "cos", a.v);
        if (o2) f2 = ad_neg(ad, v);
    } else if (!strcmp(n, "cos")) {
        v = ad_hold(ad, ad_fcall(ad, "cos", a.v));
        f1 = ad_neg(ad, ad_fcall(ad, "sin", a.v));
        if (o2) f2 = ad_neg(ad, v);
    } else if (!strcmp(n, "tan")) {
        v = ad_hold(ad, ad_fcall(ad, "tan", a.v));
        f1 = ad_hold(ad, ad_bin(ad, TK_PLUS, one, ad_bin(ad, TK_STAR, v, v)));
        if (o2) f2 = ad_bin(ad, TK_STAR, ad_cconst(ad, 2, 0), ad_bin(ad, TK_STAR, v, f1));
    } else if (!strcmp(n, "sinh")) {
        v = ad_hold(ad, ad_fcall(ad, "sinh", a.v));
        f1 = ad_fcall(ad, "cosh", a.v);
        if (o2) f2 = v;
    } else if (!strcmp(n, "cosh")) {
        v = ad_hold(ad, ad_fcall(ad, "cosh", a.v));
        f1 = ad_fcall(ad, "sinh", a.v);
        if (o2) f2 = v;
    } else if (!strcmp(n, "tanh")) {
        v = ad_hold(ad, ad_fcall(ad, "tanh", a.v));
        f1 = ad_hold(ad, ad_bin(ad, TK_MINUS, one, ad_bin(ad, TK_STAR, v, v)));
        if (o2) f2 = ad_bin(ad, TK_STAR, ad_cconst(ad, -2, 0), ad_bin(ad, TK_STAR, v, f1));
    } else if (!strcmp(n, "asin") || !strcmp(n, "acos")) {
        v = ad_fcall(ad, n, a.v);
        Expr *r = ad_fcall(ad, "sqrt", ad_bin(ad, TK_MINUS, one, ad_bin(ad, TK_STAR, a.v, a.v)));
        f1 = ad_hold(ad, ad_bin(ad, TK_SLASH, ad_cconst(ad, n[1] == 's' ? 1.0f : -1.0f, 0), r));
        if (o2) f2 = ad_bin(ad, TK_STAR, a.v, ad_powi(ad, f1, 3)); /* d/da (+-(1-a^2)^-1/2) = a f1^3 */
    } else if (!strcmp(n, "atan")) {
        v = ad_fcall(ad, "atan", a.v);
        f1 = ad_hold(ad, ad_bin(ad, TK_SLASH, one, ad_bin(ad, TK_PLUS, one, ad_bin(ad, TK_STAR, a.v, a.v))));
        if (o2) f2 = ad_bin(ad, TK_STAR, ad_cconst(ad, -2, 0), ad_bin(ad, TK_STAR, a.v, ad_bin(ad, TK_STAR, f1, f1)));
    } else {
        return false;
    }
    *out = jet_chain(ad, a, v, f1, f2);
    return true;
}

/* a^b: __powc(a, b) / __powci(a, n) from the checker's rewriting of ^ */
static Jet jet_pow(AD *ad, Expr *e) {
    Func *f = e->fn;
    Expr *ea = e->args[0], *eb = e->args[1];
    Jet a = ad_jet(ad, ea);
    if (!ad_depends(ad, eb)) {
        /* constant exponent b: g = a^b, g' = b a^(b-1) = b g / a, g'' = b (b-1) g / a^2 */
        Expr *b = ad_hold(ad, ad_to(ad, ad_copy(ad, eb), t_complex));
        Expr *args[2] = {a.v, ad_copy(ad, eb)};
        Expr *v = ad_hold(ad, ad_call(ad, f, args, 2));
        Expr *ga = ad_hold(ad, ad_bin(ad, TK_SLASH, v, a.v));
        Expr *f1 = ad_hold(ad, ad_bin(ad, TK_STAR, b, ga));
        Expr *f2 = ad->order >= 2 ? ad_bin(ad, TK_SLASH, ad_bin(ad, TK_STAR, ad_bin(ad, TK_MINUS, b, ad_cconst(ad, 1, 0)), f1), a.v) : NULL;
        return jet_chain(ad, a, v, f1, f2);
    }
    if (eb->type != t_complex) {
        ad_fail(ad, eb, "the exponent is a real or integer number computed from the variable");
        Jet z = {ad_cconst(ad, 0, 0), NULL, NULL};
        return z;
    }
    /* a^b = exp(b log a) */
    Jet b = ad_jet(ad, eb);
    Jet la, prod, out;
    jet_elementary(ad, "log", a, &la);
    Expr *d1 = jadd(ad, jmul(ad, b.d1, la.v), jmul(ad, b.v, la.d1));
    Expr *d2 = NULL;
    if (ad->order >= 2)
        d2 = jadd(ad, jadd(ad, jmul(ad, b.d2, la.v), jscale(ad, 2, jmul(ad, b.d1, la.d1))), jmul(ad, b.v, la.d2));
    prod = jet_hold(ad, ad_bin(ad, TK_STAR, b.v, la.v), d1, d2);
    jet_elementary(ad, "exp", prod, &out);
    return out;
}

/* call of a user function with a block body (or a prelude function we have no rule for):
 * call its jet version and return the jet of its first output */
static Jet jet_call_func(AD *ad, Expr *e, Jet *outs, int maxouts) {
    Func *jf = ad_jet_func(ad, e->fn);
    Jet zero = {ad_cconst(ad, 0, 0), NULL, NULL};
    if (!jf || ad->failed) return zero;
    Func *src = e->fn;
    VEC(Expr *) args = {0};
    for (int i = 0; i < src->nparams; i++) {
        Expr *arg = e->args[i];
        if (src->params[i]->type == t_complex) {
            Jet a = ad_jet(ad, arg);
            vec_push(args, a.v);
            vec_push(args, a.d1 ? a.d1 : ad_cconst(ad, 0, 0));
            if (ad->order >= 2) vec_push(args, a.d2 ? a.d2 : ad_cconst(ad, 0, 0));
        } else {
            if (ad_depends(ad, arg))
                ad_fail(ad, arg, "argument %d of '%s' is a real or integer number computed from the variable", i + 1, src->name);
            vec_push(args, ad_copy(ad, arg));
        }
    }
    Expr *call = ad_call(ad, jf, args.data, args.len);
    vec_free(args);
    /* (r, r', r'', s, ...) = jet_f(...) */
    Type *rt = jf->ret;
    int nitems = rt->kind == T_TUPLE ? rt->n : 1;
    Stmt *d = ad_stmt_new(ad, S_DESTRUCTURE);
    d->value = call;
    d->ntargets = nitems;
    d->targets = ARENA_ARRAY(&ad->p->arena, Expr *, nitems);
    Sym **res = ARENA_ARRAY(&ad->p->arena, Sym *, nitems);
    for (int k = 0; k < nitems; k++) {
        res[k] = ad_temp(ad, rt->kind == T_TUPLE ? rt->items[k] : rt);
        d->targets[k] = ad_name(ad, res[k]);
    }
    if (rt->kind == T_TUPLE) {
        ad_emit(ad, d);
    } else {
        ad_assign(ad, res[0], call);
    }
    /* unpack into one jet per output */
    int k = 0, nout = 0;
    int nsrc = src->noutputs ? src->noutputs : src->ret->kind == T_TUPLE ? src->ret->n : 1;
    for (int o = 0; o < nsrc; o++) {
        Type *ot = src->noutputs ? src->outputs[o]->type : src->ret->kind == T_TUPLE ? src->ret->items[o] : src->ret;
        Jet j = zero;
        if (ot == t_complex) {
            j.v = ad_name(ad, res[k++]);
            j.d1 = ad_name(ad, res[k++]);
            j.d2 = ad->order >= 2 ? ad_name(ad, res[k++]) : NULL;
        } else {
            j.v = ad_name(ad, res[k++]);
            j.d1 = j.d2 = NULL;
        }
        if (nout < maxouts) outs[nout++] = j;
    }
    return nout ? outs[0] : zero;
}

static Jet ad_jet(AD *ad, Expr *e) {
    Jet zero = {ad_cconst(ad, 0, 0), NULL, NULL};
    if (ad->failed) return zero;
    if (!ad_depends(ad, e)) return jet_const(ad, e);
    switch (e->kind) {
    case E_NAME: {
        for (int i = ad->cbind.len - 1; i >= 0; i--)
            if (ad->cbind.data[i].src == e->sym) return ad->cbind.data[i].j;
        if (e->sym == ad->var_src && !ad->pmode) {
            Jet j = {ad_name(ad, ad_map(ad, e->sym)), ad_cconst(ad, 1, 0), NULL};
            return j;
        }
        if (e->sym->type != t_complex) {
            ad_fail(ad, e, "'%s' is a real or integer number computed from the variable", e->sym->name);
            return zero;
        }
        JetVar jv = ad_jets_of(ad, e->sym);
        Jet j = {ad_name(ad, jv.v), ad_name(ad, jv.d1), jv.d2 ? ad_name(ad, jv.d2) : NULL};
        return j;
    }
    case E_CONVERT:
        if (e->a->type == t_complex && e->type == t_complex) return ad_jet(ad, e->a);
        ad_fail(ad, e->a, "this real number is computed from the variable (with re, im, |.|, arg or a comparison), "
                          "so the result is not holomorphic");
        return zero;
    case E_UNARY:
        if (e->op == TK_MINUS) {
            Jet a = ad_jet(ad, e->a);
            return jet_hold(ad, ad_neg(ad, a.v), a.d1 ? ad_neg(ad, a.d1) : NULL, a.d2 ? ad_neg(ad, a.d2) : NULL);
        }
        break;
    case E_BINARY: {
        if (e->type != t_complex) break;
        Jet a = ad_jet(ad, e->a), b = ad_jet(ad, e->b);
        switch (e->op) {
        case TK_PLUS: return jet_hold(ad, ad_bin(ad, TK_PLUS, a.v, b.v), jadd(ad, a.d1, b.d1), jadd(ad, a.d2, b.d2));
        case TK_MINUS: return jet_hold(ad, ad_bin(ad, TK_MINUS, a.v, b.v), jsub(ad, a.d1, b.d1), jsub(ad, a.d2, b.d2));
        case TK_STAR: {
            Expr *d1 = jadd(ad, jmul(ad, a.d1, b.v), jmul(ad, a.v, b.d1));
            Expr *d2 = NULL;
            if (ad->order >= 2)
                d2 = jadd(ad, jadd(ad, jmul(ad, a.d2, b.v), jscale(ad, 2, jmul(ad, a.d1, b.d1))), jmul(ad, a.v, b.d2));
            return jet_hold(ad, ad_bin(ad, TK_STAR, a.v, b.v), d1, d2);
        }
        case TK_SLASH: {
            Expr *q = ad_hold(ad, ad_bin(ad, TK_SLASH, a.v, b.v));
            Expr *d1 = ad_hold(ad, jdiv(ad, jsub(ad, a.d1, jmul(ad, q, b.d1)), b.v));
            Expr *d2 = NULL;
            if (ad->order >= 2)
                d2 = jdiv(ad, jsub(ad, jsub(ad, a.d2, jscale(ad, 2, jmul(ad, d1, b.d1))), jmul(ad, q, b.d2)), b.v);
            return jet_hold(ad, q, d1, d2);
        }
        default: break;
        }
        break;
    }
    case E_POWI:
        if (e->type == t_complex) return jet_powi(ad, ad_jet(ad, e->a), e->ival);
        break;
    case E_COND: {
        /* if c then a else b: the condition only chooses; each branch computes its own jet */
        if (e->type != t_complex) break;
        Sym *v = ad_temp(ad, t_complex), *d1 = ad_temp(ad, t_complex), *d2 = ad->order >= 2 ? ad_temp(ad, t_complex) : NULL;
        Stmt *iff = ad_stmt_new(ad, S_IF);
        iff->value = ad_copy(ad, e->args[0]);
        StmtVec *saved = ad->out;
        for (int k = 1; k <= 2; k++) {
            StmtVec br = {0};
            ad->out = &br;
            Jet j = ad_jet(ad, e->args[k]);
            ad_assign(ad, v, j.v);
            ad_assign(ad, d1, j.d1 ? j.d1 : ad_cconst(ad, 0, 0));
            if (d2) ad_assign(ad, d2, j.d2 ? j.d2 : ad_cconst(ad, 0, 0));
            if (k == 1) iff->body = ad_take_block(ad, &br);
            else iff->else_body = ad_take_block(ad, &br);
        }
        ad->out = saved;
        ad_emit(ad, iff);
        Jet r = {ad_name(ad, v), ad_name(ad, d1), d2 ? ad_name(ad, d2) : NULL};
        return r;
    }
    case E_CALL: {
        if (e->intrinsic == I_PA || e->intrinsic == I_PB) { /* a coordinate of a lift */
            PJet pj = ad_pjet(ad, e->args[0]);
            return e->intrinsic == I_PA ? pj.a : pj.b;
        }
        if (e->intrinsic || !e->fn) break;
        Func *f = e->fn;
        if (f->body_expr && f->body_expr->kind != E_TUPLE && f->ret == t_complex && ad_has_proj_param(f)) {
            BindMark m = ad_bind_params(ad, e); /* (a one-line function of points, inlined) */
            Jet r = ad_jet(ad, f->body_expr);
            ad_unbind(ad, m);
            return r;
        }
        if (f->prelude) {
            if (!strcmp(f->name, "conj")) {
                ad_fail(ad, e, "conj is not holomorphic");
                return zero;
            }
            if ((!strcmp(f->name, "__powc") || !strcmp(f->name, "__powci")) && e->nargs == 2) return jet_pow(ad, e);
            Jet r;
            if (e->nargs == 1 && f->params[0]->type == t_complex && jet_elementary(ad, f->name, ad_jet(ad, e->args[0]), &r))
                return r;
        }
        if (f->body_expr && f->body_expr->kind != E_TUPLE && f->ret == t_complex) {
            /* one-line function: inline it */
            Sym *from[32];
            Expr *to[32];
            int n = FS_MIN(f->nparams, 32);
            for (int i = 0; i < n; i++) {
                from[i] = f->params[i];
                to[i] = e->args[i];
            }
            return ad_jet(ad, ad_subst(ad, f->body_expr, from, to, n));
        }
        if (f->prelude && f->body_expr) {
            ad_fail(ad, e, "'%s' is not holomorphic (or has no derivative rule)", f->name);
            return zero;
        }
        if (f->ret != t_complex && f->ret->kind != T_TUPLE) break;
        Jet outs[1];
        return jet_call_func(ad, e, outs, 1);
    }
    default: break;
    }
    ad_fail(ad, e, "only + - * /, powers, exp, log, sqrt, trigonometric and hyperbolic functions (and functions "
                   "written with them) can be differentiated");
    return zero;
}

/* ---- jets of statements ----------------------------------------------------------- */
static Block ad_block(AD *ad, Block *b);

static Stmt *ad_copy_stmt_shallow(AD *ad, Stmt *s) {
    Stmt *x = ARENA_NEW(&ad->p->arena, Stmt);
    *x = *s;
    x->target = ad_copy(ad, s->target);
    x->value = ad_copy(ad, s->value);
    x->from = ad_copy(ad, s->from);
    x->to = ad_copy(ad, s->to);
    x->step = ad_copy(ad, s->step);
    x->sym = ad_map(ad, s->sym);
    x->p0 = ad_copy(ad, s->p0);
    x->p1 = ad_copy(ad, s->p1);
    x->radius = ad_copy(ad, s->radius);
    x->color = ad_copy(ad, s->color);
    x->width = ad_copy(ad, s->width);
    if (s->ntargets) {
        x->targets = ARENA_ARRAY(&ad->p->arena, Expr *, s->ntargets);
        for (int i = 0; i < s->ntargets; i++) x->targets[i] = ad_copy(ad, s->targets[i]);
    }
    x->body.n = x->else_body.n = 0;
    return x;
}

/* assign a jet to the (possibly active) source variable `src` */
static void ad_store(AD *ad, Sym *src, Jet j) {
    Sym *v = ad_map(ad, src);
    if (sym_in(&ad->act_src, src)) {
        JetVar jv = ad_jets_of(ad, src);
        ad_assign(ad, v, j.v);
        ad_assign(ad, jv.d1, j.d1 ? j.d1 : ad_cconst(ad, 0, 0));
        if (jv.d2) ad_assign(ad, jv.d2, j.d2 ? j.d2 : ad_cconst(ad, 0, 0));
    } else {
        ad_assign(ad, v, j.v);
    }
}

static void ad_stmt(AD *ad, Stmt *s) {
    if (ad->failed) return;
    switch (s->kind) {
    case S_DEFINE: case S_APPEND: case S_DRAW: case S_ON: case S_ERASE: case S_PRINT:
        return; /* drawing and printing have no place in a derivative */
    case S_EXPR:
        if (!s->value) return;
        if (ad_depends(ad, s->value)) {
            ad_fail(ad, s->value, "this uses a value computed from the variable in a way that has no derivative");
            return;
        }
        ad_emit(ad, ad_copy_stmt_shallow(ad, s));
        return;
    case S_DECL: case S_ASSIGN: {
        Expr *tg = s->target;
        bool local = tg->kind == E_NAME && tg->sym && tg->sym->kind == SYM_LOCAL;
        if (local && sym_in(&ad->act_src, tg->sym)) {
            Jet j = s->value ? ad_jet(ad, s->value) : jet_const(ad, ad_cconst(ad, 0, 0));
            ad_store(ad, tg->sym, j);
            return;
        }
        if (!local && s->value && ad_depends(ad, s->value)) {
            ad_fail(ad, s->value, "a value computed from the variable can't be stored in %s (it would lose its derivative)",
                    tg->kind == E_INDEX ? "a vector or list" : "a global variable");
            return;
        }
        ad_emit(ad, ad_copy_stmt_shallow(ad, s));
        return;
    }
    case S_DESTRUCTURE: {
        if (!ad_depends(ad, s->value)) {
            ad_emit(ad, ad_copy_stmt_shallow(ad, s));
            return;
        }
        Expr *call = s->value;
        if (call->kind != E_CALL || !call->fn) {
            ad_fail(ad, call, "can't unpack this");
            return;
        }
        Jet outs[16];
        jet_call_func(ad, call, outs, 16);
        if (ad->failed) return;
        for (int k = 0; k < s->ntargets && k < 16; k++) {
            Expr *tg = s->targets[k];
            if (tg->kind != E_NAME || !tg->sym || tg->sym->kind != SYM_LOCAL) {
                ad_fail(ad, tg, "a value computed from the variable can't be stored in a global variable");
                return;
            }
            Jet j = outs[k];
            if (j.v->type != tg->sym->type && tg->sym->type == t_complex) j.v = ad_to(ad, j.v, t_complex);
            ad_store(ad, tg->sym, j);
        }
        return;
    }
    case S_IF: case S_WHILE: case S_DOWHILE: case S_FOR: case S_FOREACH: {
        if (s->kind == S_FOREACH && ad_depends(ad, s->from) && s->sym && s->sym->type == t_complex) {
            ad_fail(ad, s->from, "can't loop over values computed from the variable");
            return;
        }
        Stmt *x = ad_copy_stmt_shallow(ad, s);
        x->body = ad_block(ad, &s->body);
        x->else_body = ad_block(ad, &s->else_body);
        ad_emit(ad, x);
        return;
    }
    case S_BREAK: case S_CONTINUE: case S_RETURN:
        ad_emit(ad, ad_copy_stmt_shallow(ad, s));
        return;
    }
}

static Block ad_block(AD *ad, Block *b) {
    StmtVec v = {0};
    StmtVec *saved = ad->out;
    ad->out = &v;
    for (int i = 0; i < b->n; i++) ad_stmt(ad, b->stmts[i]);
    ad->out = saved;
    return ad_take_block(ad, &v);
}

static void ad_free(AD *ad) {
    vec_free(ad->map);
    vec_free(ad->act_src);
    vec_free(ad->taint_src);
    vec_free(ad->jets);
    vec_free(ad->cbind);
    vec_free(ad->pbind);
}

/* ---- points of the sphere --------------------------------------------------------------- */
static Expr *ad_pcomp(AD *ad, Expr *p, int k) { /* p[1] or p[2] */
    Expr *args[1] = {p};
    return ad_intrinsic(ad, k ? I_PB : I_PA, t_complex, args, 1);
}
static Expr *ad_pmake(AD *ad, Expr *a, Expr *b) {
    Expr *args[2] = {ad_to(ad, a, t_complex), ad_to(ad, b, t_complex)};
    return ad_intrinsic(ad, I_PMAKE, t_proj, args, 2);
}
static Jet ad_jet_held(AD *ad, Jet j) { return jet_hold(ad, j.v, j.d1, j.d2); }

/* The parameters of the one-line function called by `call`, bound to the jets (or values) of its arguments, so
   that its body can be differentiated in place: each argument is computed once, into temporaries. */
static BindMark ad_bind_params(AD *ad, Expr *call) {
    BindMark m = {ad->map.len, ad->cbind.len, ad->pbind.len};
    Func *f = call->fn;
    int n = f->nparams; /* (generated functions of points can have many: one per piece of a formula) */
    PJet *pj = (PJet *)xcalloc((size_t)FS_MAX(n, 1), sizeof(PJet));
    Jet *cj = (Jet *)xcalloc((size_t)FS_MAX(n, 1), sizeof(Jet));
    Sym **tmp = (Sym **)xcalloc((size_t)FS_MAX(n, 1), sizeof(Sym *));
    int *kind = (int *)xcalloc((size_t)FS_MAX(n, 1), sizeof(int));
    for (int i = 0; i < n; i++) { /* (all the arguments first: they belong to the caller) */
        Sym *ps = f->params[i];
        Expr *arg = call->args[i];
        bool dep = ad_depends(ad, arg);
        kind[i] = 0;
        if (dep && ps->type == t_proj) {
            PJet a = ad_pjet(ad, arg);
            pj[i].a = ad_jet_held(ad, a.a);
            pj[i].b = ad_jet_held(ad, a.b);
            tmp[i] = ad_temp(ad, t_proj);
            ad_assign(ad, tmp[i], ad_pmake(ad, pj[i].a.v, pj[i].b.v));
            kind[i] = 2;
        } else if (dep && ps->type == t_complex) {
            cj[i] = ad_jet_held(ad, ad_jet(ad, arg));
            tmp[i] = ad_temp(ad, t_complex);
            ad_assign(ad, tmp[i], cj[i].v);
            kind[i] = 1;
        } else {
            if (dep) ad_fail(ad, arg, "argument %d of '%s' is a real or integer number computed from the variable", i + 1, f->name);
            tmp[i] = ad_temp(ad, ps->type);
            ad_assign(ad, tmp[i], ad_copy(ad, arg));
        }
    }
    for (int i = 0; i < n; i++) {
        ad_map_set(ad, f->params[i], tmp[i]);
        if (kind[i] == 2) {
            PJetBind b = {f->params[i], pj[i]};
            vec_push(ad->pbind, b);
        } else if (kind[i] == 1) {
            JetBind b = {f->params[i], cj[i]};
            vec_push(ad->cbind, b);
        }
    }
    free(pj);
    free(cj);
    free(tmp);
    free(kind);
    return m;
}

/* the jets of the coordinates of a point of the sphere computed from the variable */
static PJet ad_pjet(AD *ad, Expr *e) {
    PJet zero = {{ad_cconst(ad, 1, 0), NULL, NULL}, {ad_cconst(ad, 1, 0), NULL, NULL}};
    if (ad->failed) return zero;
    if (!ad_depends(ad, e)) { /* a constant point */
        Expr *v = ad_hold(ad, ad_copy(ad, e));
        PJet r = {{ad_pcomp(ad, v, 0), NULL, NULL}, {ad_pcomp(ad, v, 1), NULL, NULL}};
        return r;
    }
    switch (e->kind) {
    case E_NAME:
        if (e->sym == ad->var_src && ad->pmode) return ad->pvar;
        for (int i = ad->pbind.len - 1; i >= 0; i--)
            if (ad->pbind.data[i].src == e->sym) return ad->pbind.data[i].j;
        ad_fail(ad, e, "'%s' is a point of the sphere computed from the variable and kept in a variable; write the "
                       "computation as one formula (a one-line function of the variable)", e->sym->name);
        return zero;
    case E_CALL:
        if (e->intrinsic == I_PMAKE) {
            PJet r = {ad_jet(ad, e->args[0]), ad_jet(ad, e->args[1])};
            return r;
        }
        if (!e->intrinsic && e->fn && e->fn->body_expr && e->fn->body_expr->kind != E_TUPLE) {
            BindMark m = ad_bind_params(ad, e);
            PJet r = ad_pjet(ad, e->fn->body_expr);
            ad_unbind(ad, m);
            return r;
        }
        ad_fail(ad, e, "only one-line functions of points of the sphere (define f(z : P) = ...) can be differentiated");
        return zero;
    case E_COND: { /* if c then p else q: each branch computes its own jets */
        Sym *s[6];
        int ns = ad->order >= 2 ? 6 : 4;
        for (int k = 0; k < ns; k++) s[k] = ad_temp(ad, t_complex);
        Stmt *iff = ad_stmt_new(ad, S_IF);
        iff->value = ad_copy(ad, e->args[0]);
        StmtVec *saved = ad->out;
        for (int k = 1; k <= 2; k++) {
            StmtVec br = {0};
            ad->out = &br;
            PJet j = ad_pjet(ad, e->args[k]);
            Jet *c2[2] = {&j.a, &j.b};
            for (int q = 0; q < 2; q++) {
                ad_assign(ad, s[q], c2[q]->v);
                ad_assign(ad, s[2 + q], c2[q]->d1 ? c2[q]->d1 : ad_cconst(ad, 0, 0));
                if (ns == 6) ad_assign(ad, s[4 + q], c2[q]->d2 ? c2[q]->d2 : ad_cconst(ad, 0, 0));
            }
            if (k == 1) iff->body = ad_take_block(ad, &br);
            else iff->else_body = ad_take_block(ad, &br);
        }
        ad->out = saved;
        ad_emit(ad, iff);
        PJet r = {{ad_name(ad, s[0]), ad_name(ad, s[2]), ns == 6 ? ad_name(ad, s[4]) : NULL},
                  {ad_name(ad, s[1]), ad_name(ad, s[3]), ns == 6 ? ad_name(ad, s[5]) : NULL}};
        return r;
    }
    default: break;
    }
    ad_fail(ad, e, "this point of the sphere can't be differentiated");
    return zero;
}

/* The variable as a point of the sphere moves in a chart: the coordinate x of the chart U0 (points [x : 1]) when
   chart0, of U-infinity ([1 : x]) otherwise.  Its jets: (x, 1) or (1, x) with derivatives (1, 0) or (0, 1). */
typedef struct PChart { Sym *chart0, *x, *ca, *cb, *da, *db, *t; } PChart;

static void ad_pseed(AD *ad, PChart *pc) {
    Stmt *iff = ad_stmt_new(ad, S_IF);
    iff->value = ad_name(ad, pc->chart0);
    StmtVec *saved = ad->out;
    for (int k = 0; k < 2; k++) {
        StmtVec br = {0};
        ad->out = &br;
        ad_assign(ad, pc->ca, k == 0 ? ad_name(ad, pc->x) : ad_cconst(ad, 1, 0));
        ad_assign(ad, pc->cb, k == 0 ? ad_cconst(ad, 1, 0) : ad_name(ad, pc->x));
        ad_assign(ad, pc->da, ad_cconst(ad, k == 0 ? 1 : 0, 0));
        ad_assign(ad, pc->db, ad_cconst(ad, k == 0 ? 0 : 1, 0));
        if (k == 0) iff->body = ad_take_block(ad, &br);
        else iff->else_body = ad_take_block(ad, &br);
    }
    ad->out = saved;
    ad_emit(ad, iff);
    ad_assign(ad, pc->t, ad_pmake(ad, ad_name(ad, pc->ca), ad_name(ad, pc->cb))); /* (for conditions that use it) */
    PJet v = {{ad_name(ad, pc->ca), ad_name(ad, pc->da), NULL}, {ad_name(ad, pc->cb), ad_name(ad, pc->db), NULL}};
    ad->pvar = v;
}

/* the natural chart of the point p (the coordinate of smaller size): chart0 and x */
static void ad_pchart_of(AD *ad, PChart *pc, Expr *p) {
    Expr *a = ad_hold(ad, ad_pcomp(ad, p, 0)), *b = ad_hold(ad, ad_pcomp(ad, p, 1));
    ad_assign(ad, pc->chart0, ad_bin(ad, TK_LE, ad_abs(ad, a), ad_abs(ad, b)));
    Stmt *iff = ad_stmt_new(ad, S_IF);
    iff->value = ad_name(ad, pc->chart0);
    StmtVec *saved = ad->out;
    StmtVec b0 = {0}, b1 = {0};
    ad->out = &b0;
    ad_assign(ad, pc->x, ad_bin(ad, TK_SLASH, a, b));
    iff->body = ad_take_block(ad, &b0);
    ad->out = &b1;
    ad_assign(ad, pc->x, ad_bin(ad, TK_SLASH, b, a));
    iff->else_body = ad_take_block(ad, &b1);
    ad->out = saved;
    ad_emit(ad, iff);
}

/* the derivative of the target coordinate, in the natural chart of the result: for a point [A : B],
   (A'B - AB') / B^2 near 0 (|A| <= |B|) and (B'A - BA') / A^2 near infinity */
static Expr *ad_ptarget_d1(AD *ad, PJet j) {
    Jet A = ad_jet_held(ad, j.a), B = ad_jet_held(ad, j.b);
    Sym *r = ad_temp(ad, t_complex);
    Stmt *iff = ad_stmt_new(ad, S_IF);
    iff->value = ad_bin(ad, TK_LE, ad_abs(ad, A.v), ad_abs(ad, B.v));
    StmtVec *saved = ad->out;
    StmtVec b0 = {0}, b1 = {0};
    ad->out = &b0;
    Expr *n0 = jsub(ad, jmul(ad, A.d1, B.v), jmul(ad, A.v, B.d1));
    ad_assign(ad, r, n0 ? ad_bin(ad, TK_SLASH, n0, ad_bin(ad, TK_STAR, B.v, B.v)) : ad_cconst(ad, 0, 0));
    iff->body = ad_take_block(ad, &b0);
    ad->out = &b1;
    Expr *n1 = jsub(ad, jmul(ad, B.d1, A.v), jmul(ad, B.v, A.d1));
    ad_assign(ad, r, n1 ? ad_bin(ad, TK_SLASH, n1, ad_bin(ad, TK_STAR, A.v, A.v)) : ad_cconst(ad, 0, 0));
    iff->else_body = ad_take_block(ad, &b1);
    ad->out = saved;
    ad_emit(ad, iff);
    return ad_name(ad, r);
}

/* ---- jet versions of user functions ------------------------------------------------- */
/* jet_f(p1, p1', p1'', p2, ...) = (o1, o1', o1'', ...): complex parameters and outputs come with their
 * derivatives; all complex parameters are treated as active (callers pass zeros when they are not). */
static Func *ad_jet_func(AD *ad, Func *src) {
    for (JetFuncCache *jc = jet_cache; jc; jc = jc->next)
        if (jc->src == src && jc->order == ad->order) return jc->jet;
    if (src->failed) return NULL;
    Program *p = ad->p;
    Func *f = ARENA_NEW(&p->arena, Func);
    f->name = arena_printf(&p->arena, "__jet%d_%s", ad->order, src->name);
    f->line = src->line;
    f->col = src->col;
    f->has_where = true;
    f->check_state = 1;
    JetFuncCache *jc = (JetFuncCache *)xcalloc(1, sizeof *jc);
    jc->src = src;
    jc->jet = f;
    jc->order = ad->order;
    jc->next = jet_cache;
    jet_cache = jc;

    AD sub = {0};
    sub.c = ad->c;
    sub.p = p;
    sub.at = ad->at;
    sub.order = ad->order;
    sub.fn = f;
    /* parameters */
    VEC(Sym *) params = {0};
    for (int i = 0; i < src->nparams; i++) {
        Sym *sp = src->params[i];
        Sym *np = ad_local(&sub, sp->name, sp->type);
        ad_map_set(&sub, sp, np);
        vec_push(params, np);
        if (sp->type == t_complex) {
            vec_push(sub.act_src, sp);
            JetVar jv = ad_jets_of(&sub, sp);
            vec_push(params, jv.d1);
            if (jv.d2) vec_push(params, jv.d2);
        }
    }
    /* outputs (a one-line function gets one synthetic output per result) */
    VEC(Sym *) outs = {0};
    Sym *src_outs[16];
    int nsrc_outs = 0;
    Block body_src = src->body;
    if (src->body_expr) {
        /* treat `define f(x) = e` as `define f(x) = r where r = e end` */
        Expr *be = src->body_expr;
        int nitems = be->kind == E_TUPLE ? be->nargs : 1;
        Stmt **st = ARENA_ARRAY(&p->arena, Stmt *, nitems);
        for (int k = 0; k < nitems && k < 16; k++) {
            Expr *item = be->kind == E_TUPLE ? be->args[k] : be;
            Sym *r = new_sym(ad->c, arena_printf(&p->arena, "__r%d", k), item->type, SYM_LOCAL, src->line, src->col);
            r->owner = src;
            src_outs[nsrc_outs++] = r;
            st[k] = ad_stmt_new(&sub, S_ASSIGN);
            st[k]->op = TK_ASSIGN;
            st[k]->target = ad_name(&sub, r);
            st[k]->value = item;
        }
        body_src.stmts = st;
        body_src.n = nitems;
    } else {
        for (int k = 0; k < src->noutputs && k < 16; k++) src_outs[nsrc_outs++] = src->outputs[k];
    }
    /* activity: complex parameters are active; propagate through the body */
    while (ad_analyze_block(&sub, &body_src)) {}
    for (int k = 0; k < nsrc_outs; k++) {
        Sym *so = src_outs[k];
        Sym *no = ad_map(&sub, so);
        vec_push(outs, no);
        if (so->type == t_complex) {
            if (!sym_in(&sub.act_src, so)) vec_push(sub.act_src, so); /* zero derivatives, but present */
            JetVar jv = ad_jets_of(&sub, so);
            vec_push(outs, jv.d1);
            if (jv.d2) vec_push(outs, jv.d2);
        }
    }
    /* derivative outputs start at zero (an output may never be assigned) */
    StmtVec body = {0};
    sub.out = &body;
    Block tb = ad_block(&sub, &body_src);
    sub.out = &body;
    for (int i = 0; i < tb.n; i++) vec_push(body, tb.stmts[i]);
    f->body = ad_take_block(&sub, &body);
    f->nparams = params.len;
    f->params = ARENA_ARRAY(&p->arena, Sym *, params.len ? params.len : 1);
    f->param_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, params.len ? params.len : 1);
    for (int i = 0; i < params.len; i++) f->params[i] = params.data[i];
    f->noutputs = outs.len;
    f->outputs = ARENA_ARRAY(&p->arena, Sym *, outs.len ? outs.len : 1);
    f->output_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, outs.len ? outs.len : 1);
    Type *items[64];
    for (int i = 0; i < outs.len && i < 64; i++) {
        f->outputs[i] = outs.data[i];
        items[i] = outs.data[i]->type;
    }
    f->ret = outs.len == 1 ? items[0] : type_tuple(items, FS_MIN(outs.len, 64));
    if (outs.len > 64) ad_fail(ad, NULL, "'%s' has too many results to differentiate", src->name);
    /* frame: parameters first, then outputs, then the rest (as check_func lays out user functions) */
    SymVec ordered = {0};
    for (int i = 0; i < f->nparams; i++) vec_push(ordered, f->params[i]);
    for (int i = 0; i < f->noutputs; i++)
        if (!sym_in(&ordered, f->outputs[i])) vec_push(ordered, f->outputs[i]);
    for (int i = 0; i < f->locals.len; i++)
        if (!sym_in(&ordered, f->locals.data[i])) vec_push(ordered, f->locals.data[i]);
    vec_free(f->locals);
    f->locals = ordered;
    layout_frame(&f->locals, &f->frame_words);
    f->check_state = 2;
    if (sub.failed) {
        ad->failed = true;
        f->failed = true;
    }
    vec_push(p->funcs, f);
    vec_free(params);
    vec_free(outs);
    ad_free(&sub);
    return f;
}

/* ---- dependency of section locals on a variable (for continuation) ---------------------------- */
static bool ad_expr_uses(Expr *e, SymVec *set) {
    if (!e) return false;
    if (e->kind == E_NAME && e->sym && sym_in(set, e->sym)) return true;
    if (ad_expr_uses(e->a, set) || ad_expr_uses(e->b, set)) return true;
    for (int i = 0; i < e->nargs; i++)
        if (ad_expr_uses(e->args[i], set)) return true;
    return false;
}
static bool ad_dep_block(Block *b, SymVec *set) {
    bool changed = false;
    for (int i = 0; i < b->n; i++) {
        Stmt *s = b->stmts[i];
        Expr *targets[64];
        int nt = 0;
        bool dep = false;
        if (s->kind == S_DECL || s->kind == S_ASSIGN) {
            targets[nt++] = s->target;
            dep = ad_expr_uses(s->value, set);
        } else if (s->kind == S_DESTRUCTURE) {
            for (int k = 0; k < s->ntargets && nt < 64; k++) targets[nt++] = s->targets[k];
            dep = ad_expr_uses(s->value, set);
        }
        for (int k = 0; k < nt; k++) {
            Expr *t = targets[k];
            while (t && t->kind == E_INDEX) t = t->a;
            if (dep && t && t->sym && t->sym->kind == SYM_LOCAL && !sym_in(set, t->sym)) {
                vec_push(*set, t->sym);
                changed = true;
            }
        }
        changed |= ad_dep_block(&s->body, set);
        changed |= ad_dep_block(&s->else_body, set);
    }
    return changed;
}

static void ad_collect_captures(Expr *e, Sym *var, SymVec *out) {
    if (!e) return;
    if (e->kind == E_NAME && e->sym && e->sym->kind == SYM_LOCAL && e->sym != var && !sym_in(out, e->sym)) vec_push(*out, e->sym);
    ad_collect_captures(e->a, var, out);
    ad_collect_captures(e->b, var, out);
    for (int i = 0; i < e->nargs; i++) ad_collect_captures(e->args[i], var, out);
}

/* ---- the solve itself ----------------------------------------------------------------- */
/* Newton loop:
 *   k = 0; conv = false; prev = 1e30
 *   while k < limit do
 *       k += 1; .stuck = false; <jets>; if .stuck then break end   -- a loop of the expression got stuck
 *       step = num / den; t -= step; a = |step|; scale = 1 + |t|
 *       if a <= .epsilon scale then conv = true; break end          -- converged
 *       if a <= 1e-3 scale and prev / 2 <= a < 1.4 prev then conv = true; break end
 *                                    -- steps stopped shrinking: rounding noise of 32-bit floats, or a multiple
 *                                       root (steps growing faster: moving away from a pole, not converging)
 *       if not (a < 1e30) then break end                             -- diverged (or NaN)
 *       prev = a
 *   end */
static Stmt *ad_if_break(AD *ad, Expr *cond, Sym *set_true) {
    Stmt *s = ad_stmt_new(ad, S_IF);
    s->value = cond;
    StmtVec body = {0};
    StmtVec *saved = ad->out;
    ad->out = &body;
    if (set_true) ad_assign(ad, set_true, ad_bool(ad, true));
    ad_emit(ad, ad_stmt_new(ad, S_BREAK));
    s->body = ad_take_block(ad, &body);
    ad->out = saved;
    return s;
}

/* what a Newton step divides: the value and its derivative (root), or the first and second derivatives (critical).
   For a point of the sphere [A : B]: root solves A/B = 0 (step A B / (A'B - AB')), critical solves
   K = A B' - B A' = 0, which is where the map's derivative vanishes in any chart (step K / K', K' = A B'' - B A''). */
static void ad_solve_terms(AD *ad, int op, Expr *body, bool pbody, Expr **num, Expr **den) {
    if (!pbody) {
        Jet j = ad_jet(ad, body);
        *num = op == SOLVE_CRITICAL ? j.d1 : j.v;
        *den = op == SOLVE_CRITICAL ? j.d2 : j.d1;
        return;
    }
    PJet pj = ad_pjet(ad, body);
    Jet A = ad_jet_held(ad, pj.a), B = ad_jet_held(ad, pj.b);
    if (op == SOLVE_ROOT) {
        *num = jmul(ad, A.v, B.v);
        *den = jsub(ad, jmul(ad, A.d1, B.v), jmul(ad, A.v, B.d1));
    } else {
        *num = jsub(ad, jmul(ad, A.v, B.d1), jmul(ad, B.v, A.d1));
        *den = jsub(ad, jmul(ad, A.v, B.d2), jmul(ad, B.v, A.d2));
    }
}

/* (pc: the variable is a point of the sphere, moved in charts: Newton's method on the coordinate pc->x, which
   changes chart when it leaves the unit disk) */
static void ad_newton(AD *ad, int op, Expr *body, bool pbody, Sym *t, PChart *pc, Sym *conv, Sym *k, Sym *total, Expr *limit) {
    Sym *prev = ad_temp(ad, t_real), *a = ad_temp(ad, t_real), *scale = ad_temp(ad, t_real);
    Sym *X = pc ? pc->x : t;
    ad_assign(ad, k, ad_int(ad, 0));
    ad_assign(ad, conv, ad_bool(ad, false));
    ad_assign(ad, prev, ad_real(ad, 1e30f));
    StmtVec loop = {0};
    StmtVec *saved = ad->out;
    ad->out = &loop;
    ad_assign(ad, k, ad_bin(ad, TK_PLUS, ad_name(ad, k), ad_int(ad, 1)));
    if (total) ad_assign(ad, total, ad_bin(ad, TK_PLUS, ad_name(ad, total), ad_int(ad, 1)));
    /* an evaluation that gets stuck (a loop of the expression reaches .maxIterations) is not a value to trust */
    ad_assign(ad, ad->p->s_stuck, ad_bool(ad, false));
    if (pc) ad_pseed(ad, pc);
    Expr *num, *den;
    ad_solve_terms(ad, op, body, pbody, &num, &den);
    ad_emit(ad, ad_if_break(ad, ad_name(ad, ad->p->s_stuck), NULL));
    if (!den && !ad->failed) {
        prog_diag(ad->p, DIAG_ERROR, ad->at->line, ad->at->col, ad->at->end_col,
                  "the %s is constant, so Newton's method can't be used here",
                  op == SOLVE_CRITICAL ? "second derivative" : "derivative");
        ad->failed = true;
    }
    if (!den) den = ad_cconst(ad, 0, 0);
    if (!num) num = ad_cconst(ad, 0, 0);
    Sym *step = ad_temp(ad, t_complex);
    ad_assign(ad, step, ad_bin(ad, TK_SLASH, num, den));
    ad_assign(ad, X, ad_bin(ad, TK_MINUS, ad_name(ad, X), ad_name(ad, step)));
    ad_assign(ad, a, ad_abs(ad, ad_name(ad, step)));
    ad_assign(ad, scale, ad_bin(ad, TK_PLUS, ad_real(ad, 1), ad_abs(ad, ad_name(ad, X))));
    Expr *eps = ad_name(ad, ad->p->s_epsilon);
    ad_emit(ad, ad_if_break(ad, ad_bin(ad, TK_LE, ad_name(ad, a), ad_bin(ad, TK_STAR, eps, ad_name(ad, scale))), conv));
    Expr *small = ad_bin(ad, TK_LE, ad_name(ad, a), ad_bin(ad, TK_STAR, ad_real(ad, 1e-3f), ad_name(ad, scale)));
    /* (not shrinking, but not growing much either: next to a pole of order m, where they are small too, the steps grow
       by 1 + 1/m, so 2 for a simple pole (1.5 for the derivative's double pole when looking for a critical point)) */
    Expr *stalled = ad_bin(ad, TK_AND, ad_bin(ad, TK_GE, ad_name(ad, a), ad_bin(ad, TK_STAR, ad_real(ad, 0.5f), ad_name(ad, prev))),
                           ad_bin(ad, TK_LT, ad_name(ad, a), ad_bin(ad, TK_STAR, ad_real(ad, 1.4f), ad_name(ad, prev))));
    ad_emit(ad, ad_if_break(ad, ad_bin(ad, TK_AND, small, stalled), conv));
    ad_emit(ad, ad_if_break(ad, ad_not(ad, ad_bin(ad, TK_LT, ad_name(ad, a), ad_real(ad, 1e30f))), NULL));
    ad_assign(ad, prev, ad_name(ad, a));
    if (pc) { /* out of the unit disk of its chart: the other chart */
        Stmt *sw = ad_stmt_new(ad, S_IF);
        sw->value = ad_bin(ad, TK_GT, ad_abs(ad, ad_name(ad, X)), ad_real(ad, 1));
        StmtVec b = {0};
        StmtVec *in = ad->out;
        ad->out = &b;
        ad_assign(ad, X, ad_bin(ad, TK_SLASH, ad_cconst(ad, 1, 0), ad_name(ad, X)));
        ad_assign(ad, pc->chart0, ad_not(ad, ad_name(ad, pc->chart0)));
        ad_assign(ad, prev, ad_real(ad, 1e30f));
        sw->body = ad_take_block(ad, &b);
        ad->out = in;
        ad_emit(ad, sw);
    }
    ad->out = saved;
    Stmt *wh = ad_stmt_new(ad, S_WHILE);
    wh->quiet = true;
    wh->value = ad_bin(ad, TK_LT, ad_name(ad, k), limit);
    wh->body = ad_take_block(ad, &loop);
    ad_emit(ad, wh);
}

static Type *chk_solve(Checker *c, Expr *e) {
    static int counter = 0;
    Program *p = c->prog;
    bool cont = e->cont_from != NULL;
    Type *seed_t = chk(c, e->b);
    if (seed_t == t_error) return t_error;
    bool pmode = seed_t == t_proj; /* at a point of the sphere: the variable is one too, moved in charts */
    if (!pmode) e->b = coerce(c, e->b, t_complex, e->op == SOLVE_DERIVATIVE ? "the point after 'at'" : "the seed");
    if (pmode && cont) {
        ERR(c, e, "'continuing' is not available for points of the sphere yet");
        return t_error;
    }
    Section *viewer = NULL;
    Sym *cvar = NULL;
    if (cont) {
        if (chk(c, e->cont_from) == t_error) return t_error;
        e->cont_from = coerce(c, e->cont_from, t_complex, "the starting point after 'continuing from'");
        if (c->fn || !c->sec || c->sec->kind != SEC_VIEWER) {
            ERR(c, e, "'continuing' follows a solution from pixel to pixel, so it can only be used in a viewer "
                      "(not in functions, tools or @global)");
            return t_error;
        }
        viewer = c->sec;
        if (e->cont_var && (!viewer->var || strcmp(e->cont_var, viewer->var) != 0)) {
            ERR(c, e, "the solution can only be continued along the viewer's variable '%s'", viewer->var ? viewer->var : "?");
            return t_error;
        }
        cvar = viewer->coord;
        if (viewer->projective || viewer->var_proj) {
            ERR(c, e, "'continuing' is not available in projective viewers yet");
            return t_error;
        }
        SymVec none = {0};
        SymVec used = {0};
        ad_collect_captures(e->cont_from, NULL, &used);
        bool uses_local = used.len > 0;
        vec_free(used);
        vec_free(none);
        if (uses_local) {
            ERR(c, e->cont_from, "the starting point must be the same for every pixel (use global values, like .%s)",
                viewer->var ? viewer->var : "c");
            return t_error;
        }
        if (viewer->flood) {
            ERR(c, e, "a viewer can have only one 'continuing' solve");
            return t_error;
        }
    }
    Func *f = ARENA_NEW(&p->arena, Func);
    const char *what = e->op == SOLVE_CRITICAL ? "critical" : e->op == SOLVE_ROOT ? "root" : "derivative";
    f->name = arena_printf(&p->arena, "__%s%d", what, ++counter);
    f->line = e->line;
    f->col = e->col;
    f->has_where = true;
    f->check_state = 1;
    /* the variable of the lambda, and its body */
    Sym *var = new_sym(c, e->name ? e->name : "x", pmode ? t_proj : t_complex, SYM_LOCAL, e->line, e->col);
    var->owner = f;
    Expr *body = e->a;
    if (!e->name) { /* a function name: body = f(x) */
        body = ARENA_NEW(&p->arena, Expr);
        *body = *e;
        body->kind = E_CALL;
        body->name = e->base_name;
        body->a = body->b = NULL;
        body->cont_from = NULL;
        body->nargs = 1;
        body->args = ARENA_ARRAY(&p->arena, Expr *, 1);
        Expr *arg = ARENA_NEW(&p->arena, Expr);
        *arg = *body;
        arg->kind = E_NAME;
        arg->name = var->name;
        arg->nargs = 0;
        arg->args = NULL;
        body->args[0] = arg;
    }
    Expr *spare = !pmode && !cont ? clone_expr(&p->arena, body) : NULL; /* (in case it is a map to the sphere) */
    Scope lam = {{0}, c->scope};
    vec_push(lam.syms, var);
    Scope *saved_scope = c->scope;
    c->scope = &lam;
    Type *bt = chk(c, body);
    c->scope = saved_scope;
    vec_free(lam.syms);
    if (bt == t_error) return t_error;
    if (bt == t_proj && spare) {
        /* a map to the sphere at a complex number: the variable is a point too, so that both sides use natural charts
           (a multiplier is a product of derivatives at points of a cycle, each the source of one and the target of
           another). If the expression only works with a complex variable, it stays one. */
        int d0 = p->diags.len, e0 = p->nerrors;
        Sym *pv = new_sym(c, var->name, t_proj, SYM_LOCAL, e->line, e->col);
        pv->owner = f;
        Scope plam = {{0}, c->scope};
        vec_push(plam.syms, pv);
        c->scope = &plam;
        Type *pt = chk(c, spare);
        c->scope = saved_scope;
        vec_free(plam.syms);
        if (pt == t_proj && p->nerrors == e0) {
            pmode = true;
            var = pv;
            body = spare;
            e->b = coerce(c, e->b, t_proj, "the point");
        } else {
            p->diags.len = d0;
            p->nerrors = e0;
        }
    }
    bool pbody = bt == t_proj; /* a map to the sphere */
    if (!pbody && (!type_is_number(bt) || bt == t_quat)) {
        ERR(c, body, "the expression after '->' must be a number or a point of the sphere, not %s", type_str(bt));
        return t_error;
    }
    if (pbody && cont) {
        ERR(c, e, "'continuing' is not available for points of the sphere yet");
        return t_error;
    }
    if (!pbody) body = coerce(c, body, t_complex, "the expression");
    px_lower_expr(c, &body); /* (points of the sphere: written with their coordinates) */
    Type *rtype = e->op == SOLVE_DERIVATIVE || !pmode ? t_complex : t_proj;
    /* captured locals of the caller become parameters */
    SymVec caps = {0};
    ad_collect_captures(body, var, &caps);
    if (cont) {
        for (int i = 0; i < caps.len; i++)
            if (caps.data[i] == cvar) {
                caps.data[i] = caps.data[--caps.len];
                break;
            }
        SymVec dep = {0};
        vec_push(dep, cvar);
        while (ad_dep_block(&viewer->body, &dep)) {}
        for (int i = 0; i < caps.len; i++)
            if (sym_in(&dep, caps.data[i])) {
                ERR(c, body, "'%s' is computed from %s, but a continued solve re-evaluates the expression while %s moves "
                             "from pixel to pixel; compute it inside the expression instead (for example in a function of %s)",
                    caps.data[i]->name, cvar->name, cvar->name, cvar->name);
                vec_free(dep);
                vec_free(caps);
                return t_error;
            }
        vec_free(dep);
    }
    AD ad = {0};
    ad.c = c;
    ad.p = p;
    ad.at = e;
    ad.order = e->op == SOLVE_CRITICAL ? 2 : 1;
    ad.fn = f;
    /* parameters: seed, [from, to,] captures */
    VEC(Sym *) params = {0};
    Sym *seed = ad_local(&ad, "seed", pmode ? t_proj : t_complex);
    vec_push(params, seed);
    Sym *from = NULL, *to = NULL, *cc = NULL;
    if (cont) {
        from = ad_local(&ad, "from", t_complex);
        to = ad_local(&ad, "to", t_complex);
        vec_push(params, from);
        vec_push(params, to);
    }
    for (int i = 0; i < caps.len; i++) {
        Sym *np = ad_local(&ad, caps.data[i]->name, caps.data[i]->type);
        ad_map_set(&ad, caps.data[i], np);
        vec_push(params, np);
    }
    Sym *t = ad_local(&ad, var->name, pmode ? t_proj : t_complex);
    ad_map_set(&ad, var, t);
    vec_push(ad.act_src, var);
    ad.var_src = var;
    ad.pmode = pmode;
    PChart pc = {0};
    if (pmode) {
        pc.chart0 = ad_local(&ad, "chart0", t_bool);
        pc.x = ad_local(&ad, "x", t_complex);
        pc.ca = ad_local(&ad, "a", t_complex);
        pc.cb = ad_local(&ad, "b", t_complex);
        pc.da = ad_local(&ad, "a'", t_complex);
        pc.db = ad_local(&ad, "b'", t_complex);
        pc.t = t;
    }
    if (cont) {
        cc = ad_local(&ad, cvar->name, t_complex);
        ad_map_set(&ad, cvar, cc);
    }
    Sym *res = ad_local(&ad, "result", rtype);
    StmtVec out = {0};
    ad.out = &out;
    ad_assign(&ad, t, ad_name(&ad, seed));
    if (pmode) ad_pchart_of(&ad, &pc, ad_name(&ad, seed));
    if (e->op == SOLVE_DERIVATIVE) {
        if (pmode) ad_pseed(&ad, &pc);
        if (pbody) { /* in the natural charts of the point and of its image */
            ad_assign(&ad, res, ad_ptarget_d1(&ad, ad_pjet(&ad, body)));
        } else {
            Jet j = ad_jet(&ad, body);
            ad_assign(&ad, res, j.d1 ? j.d1 : ad_cconst(&ad, 0, 0));
        }
    } else if (!cont) {
        Sym *conv = ad_local(&ad, "converged", t_bool), *k = ad_local(&ad, "steps", t_int);
        ad_newton(&ad, e->op, body, pbody, t, pmode ? &pc : NULL, conv, k, NULL, ad_name(&ad, p->s_max_iter));
        if (pmode) ad_pseed(&ad, &pc); /* (t = the point of x in its chart) */
        ad_assign(&ad, res, ad_name(&ad, t));
        ad_assign(&ad, p->s_stuck, ad_not(&ad, ad_name(&ad, conv)));
        ad_assign(&ad, p->s_iterations, ad_name(&ad, k));
    } else {
        /* attempts with 1, 4, 16, 64 steps from `from` to `to`, each step a short Newton run */
        Sym *ok = ad_local(&ad, "ok", t_bool), *nsub = ad_local(&ad, "nsub", t_int), *j = ad_local(&ad, "j", t_int);
        Sym *conv = ad_local(&ad, "converged", t_bool), *k = ad_local(&ad, "steps", t_int);
        Sym *total = ad_local(&ad, "total", t_int);
        ad_assign(&ad, ok, ad_bool(&ad, false));
        ad_assign(&ad, nsub, ad_int(&ad, 1));
        ad_assign(&ad, total, ad_int(&ad, 0));
        StmtVec attempt = {0};
        ad.out = &attempt;
        ad_assign(&ad, t, ad_name(&ad, seed));
        ad_assign(&ad, ok, ad_bool(&ad, true));
        ad_assign(&ad, j, ad_int(&ad, 0));
        {
            StmtVec sub = {0};
            ad.out = &sub;
            ad_assign(&ad, j, ad_bin(&ad, TK_PLUS, ad_name(&ad, j), ad_int(&ad, 1)));
            /* cc = from + (to - from) * (j / nsub) */
            Expr *frac = ad_bin(&ad, TK_SLASH, ad_name(&ad, j), ad_name(&ad, nsub));
            ad_assign(&ad, cc, ad_bin(&ad, TK_PLUS, ad_name(&ad, from),
                                      ad_bin(&ad, TK_STAR, ad_bin(&ad, TK_MINUS, ad_name(&ad, to), ad_name(&ad, from)), frac)));
            ad_newton(&ad, e->op, body, false, t, NULL, conv, k, total, ad_int(&ad, 30));
            Stmt *iff = ad_stmt_new(&ad, S_IF);
            iff->value = ad_not(&ad, ad_name(&ad, conv));
            {
                StmtVec fb = {0};
                ad.out = &fb;
                ad_assign(&ad, ok, ad_bool(&ad, false));
                ad_emit(&ad, ad_stmt_new(&ad, S_BREAK));
                iff->body = ad_take_block(&ad, &fb);
                ad.out = &sub;
            }
            ad_emit(&ad, iff);
            Stmt *wj = ad_stmt_new(&ad, S_WHILE);
            wj->quiet = true;
            wj->value = ad_bin(&ad, TK_LT, ad_name(&ad, j), ad_name(&ad, nsub));
            wj->body = ad_take_block(&ad, &sub);
            ad.out = &attempt;
            ad_emit(&ad, wj);
        }
        Stmt *done = ad_stmt_new(&ad, S_IF);
        done->value = ad_name(&ad, ok);
        {
            StmtVec db = {0};
            ad.out = &db;
            ad_emit(&ad, ad_stmt_new(&ad, S_BREAK));
            done->body = ad_take_block(&ad, &db);
            ad.out = &attempt;
        }
        ad_emit(&ad, done);
        ad_assign(&ad, nsub, ad_bin(&ad, TK_STAR, ad_name(&ad, nsub), ad_int(&ad, 4)));
        ad.out = &out;
        Stmt *wa = ad_stmt_new(&ad, S_WHILE);
        wa->quiet = true;
        wa->value = ad_bin(&ad, TK_LE, ad_name(&ad, nsub), ad_int(&ad, 64));
        wa->body = ad_take_block(&ad, &attempt);
        ad_emit(&ad, wa);
        ad_assign(&ad, res, ad_name(&ad, t));
        ad_assign(&ad, p->s_stuck, ad_not(&ad, ad_name(&ad, ok)));
        ad_assign(&ad, p->s_iterations, ad_name(&ad, total));
        Expr *oargs[2] = {ad_name(&ad, t), ad_name(&ad, ok)};
        Stmt *fo = ad_stmt_new(&ad, S_EXPR);
        fo->value = ad_intrinsic(&ad, I_FLOOD_OUT, t_void, oargs, 2);
        ad_emit(&ad, fo);
    }
    f->body = ad_take_block(&ad, &out);
    bool failed = ad.failed;
    /* finish the function */
    f->nparams = params.len;
    f->params = ARENA_ARRAY(&p->arena, Sym *, params.len);
    f->param_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, params.len);
    for (int i = 0; i < params.len; i++) f->params[i] = params.data[i];
    f->noutputs = 1;
    f->outputs = ARENA_ARRAY(&p->arena, Sym *, 1);
    f->outputs[0] = res;
    f->output_tsyn = ARENA_ARRAY(&p->arena, TypeSyntax *, 1);
    f->ret = rtype;
    SymVec ordered = {0};
    for (int i = 0; i < f->nparams; i++) vec_push(ordered, f->params[i]);
    vec_push(ordered, res);
    for (int i = 0; i < f->locals.len; i++)
        if (!sym_in(&ordered, f->locals.data[i])) vec_push(ordered, f->locals.data[i]);
    vec_free(f->locals);
    f->locals = ordered;
    layout_frame(&f->locals, &f->frame_words);
    f->check_state = 2;
    f->failed = failed;
    vec_push(p->funcs, f);
    ad_free(&ad);
    vec_free(params);
    if (failed) {
        vec_free(caps);
        return t_error;
    }
    /* rewrite e into a call: f(seed, [flood seed / start, c,] captures...) */
    Expr *seed_e = e->b;
    int nargs = f->nparams;
    Expr **args = ARENA_ARRAY(&p->arena, Expr *, nargs);
    int a = 0;
    AD at = {0};
    at.c = c;
    at.p = p;
    at.at = e;
    if (cont) {
        Expr *sargs[1] = {seed_e};
        args[a++] = ad_intrinsic(&at, I_FLOOD_SEED, t_complex, sargs, 1);
        Expr *fargs[2] = {e->cont_from, ad_name(&at, cvar)};
        args[a++] = ad_intrinsic(&at, I_FLOOD_FROM, t_complex, fargs, 2);
        args[a++] = ad_name(&at, cvar);
        viewer->flood = f;
        viewer->flood_start = e->cont_from;
    } else {
        args[a++] = seed_e;
    }
    for (int i = 0; i < caps.len; i++) args[a++] = ad_name(&at, caps.data[i]);
    vec_free(caps);
    e->kind = E_CALL;
    e->name = f->name;
    e->fn = f;
    e->intrinsic = 0;
    e->nargs = nargs;
    e->args = args;
    e->a = e->b = NULL;
    e->cont_from = NULL;
    if (c->fn && f->uses_draw) c->fn->uses_draw = true;
    return rtype;
}
