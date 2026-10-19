/* program.c - loading a script: lex + parse (prelude and user code), check,
 * and run @global/@config to compute the initial state. */
#include "ast.h"

void lex_program(Program *prog, const char *src, int first_line);
void parse_tokens(Program *prog, Token *toks, int ntoks, const char **builtins, int nbuiltins, const char **ufuncs,
                  int nufuncs, const char **gvars, int ngvars, bool prelude);
bool check_program(Program *p);

typedef VEC(const char *) NameVec;
static void collect_function_names(Token *t, int n, NameVec *names) {
    for (int i = 0; i + 1 < n; i++)
        if (t[i].kind == TK_DEFINE && t[i + 1].kind == TK_IDENT) vec_push(*names, t[i + 1].str);
}

/* names assigned at the start of a statement in @global/@config sections
 * (or before the first section): these are global variables */
static void collect_global_names(Token *t, int n, NameVec *names) {
    bool has_sections = false;
    for (int i = 0; i < n; i++)
        if (t[i].kind == TK_SECTION) has_sections = true;
    if (!has_sections) return;
    bool in_load = true;
    for (int i = 0; i + 1 < n; i++) {
        if (t[i].kind == TK_SECTION) {
            in_load = !strcmp(t[i].str, "global") || !strcmp(t[i].str, "globals") || !strcmp(t[i].str, "config");
            continue;
        }
        bool start = i == 0 || t[i - 1].kind == TK_NEWLINE || t[i - 1].kind == TK_SEMI;
        TokKind k = t[i + 1].kind;
        if (in_load && start && t[i].kind == TK_IDENT && (k == TK_DECLARE || k == TK_COLON || k == TK_ASSIGN))
            vec_push(*names, t[i].str);
    }
}

/* "1.5", "-2", "0.3i", "-0.8+0.156i", "1-2i" (no spaces) in double precision */
static bool parse_complex_literal_d(const char *s, double *re, double *im) {
    char *end;
    *re = *im = 0;
    double a = strtod(s, &end);
    if (end == s) {
        if (*s == 'i') { *im = 1; return s[1] == 0; }
        if (*s == '-' && s[1] == 'i' && !s[2]) { *im = -1; return true; }
        return false;
    }
    if (*end == 'i' && end[1] == 0) { *im = a; return true; }
    if (*end == 0) { *re = a; return true; }
    if (*end != '+' && *end != '-') return false;
    const char *t = end;
    double b;
    if (t[1] == 'i') { /* "1+i", "1-i" */
        b = *t == '-' ? -1 : 1;
        end = (char *)t + 1;
    } else {
        b = strtod(t, &end);
    }
    if (*end != 'i' || end[1] != 0) return false;
    *re = a;
    *im = b;
    return true;
}

/* a constant number (literals with + - * /) in double precision */
static bool const_eval_d(Expr *e, double *re, double *im) {
    double ar, ai, br, bi;
    switch (e->kind) {
    case E_INT: *re = e->ival; *im = 0; return true;
    case E_REAL: *re = e->rval; *im = 0; return true;
    case E_IMAG: *re = 0; *im = e->rval; return true;
    case E_CONVERT: return const_eval_d(e->a, re, im);
    case E_UNARY:
        if (e->op != TK_MINUS || !const_eval_d(e->a, &ar, &ai)) return false;
        *re = -ar;
        *im = -ai;
        return true;
    case E_BINARY:
        if (!const_eval_d(e->a, &ar, &ai) || !const_eval_d(e->b, &br, &bi)) return false;
        switch (e->op) {
        case TK_PLUS: *re = ar + br; *im = ai + bi; return true;
        case TK_MINUS: *re = ar - br; *im = ai - bi; return true;
        case TK_STAR: *re = ar * br - ai * bi; *im = ar * bi + ai * br; return true;
        case TK_SLASH: {
            double d = br * br + bi * bi;
            if (d == 0) return false;
            *re = (ar * br + ai * bi) / d;
            *im = (ai * br - ar * bi) / d;
            return true;
        }
        default: return false;
        }
    default: return false;
    }
}

/* Deep zooms: a flat view's centre written in the script with more digits than a float holds
 * (`mandel.center = -0.743643887037151 + 0.131825904205330i`) keeps them in its low part. */
static void initial_center_lo(Program *p, Section *v) {
    if (!v->center_lo) return;
    Stmt *last = NULL;
    for (int i = 0; i < p->sections.len; i++) {
        Section *s = p->sections.data[i];
        if (s->kind != SEC_CONFIG && s->kind != SEC_GLOBAL) continue;
        for (int k = 0; k < s->body.n; k++) {
            Stmt *st = s->body.stmts[k];
            if (st->kind == S_ASSIGN && st->op == TK_ASSIGN && st->target && st->target->sym == v->center) last = st;
        }
    }
    double re, im;
    if (!last || !last->value || !const_eval_d(last->value, &re, &im)) return;
    u32 *c = p->initial_state + v->center->slot;
    if (fw((float)re) != c[0] || fw((float)im) != c[1]) return; /* (changed afterwards) */
    u32 *lo = p->initial_state + v->center_lo->slot;
    lo[0] = fw((float)(re - (double)(float)re));
    lo[1] = fw((float)(im - (double)(float)im));
}

/* Parse + check + initial state.  Always returns a Program; check
 * p->nerrors / p->diags.  `source` is copied. */
Program *program_load(const char *source, const char *path) {
    types_init();
    Program *p = (Program *)xcalloc(1, sizeof(Program));
    p->path = path ? arena_strndup(&p->arena, path, strlen(path)) : "script";
    p->source = arena_strndup(&p->arena, source, strlen(source));
    p->title = "FractalStream";

    NameVec builtins = {0}, ufuncs = {0}, gvars = {0};
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++) vec_push(builtins, intrinsics[k].name);
    vec_push(builtins, "palette"); /* palette("fire", t), rewritten by the checker */

    /* prelude */
    lex_program(p, prelude_source, PRELUDE_LINE0 + 1);
    collect_function_names(p->tokens.data, p->tokens.len, &ufuncs);
    int nsec = p->sections.len;
    parse_tokens(p, p->tokens.data, p->tokens.len, builtins.data, builtins.len, ufuncs.data, ufuncs.len, NULL, 0, true);
    p->sections.len = nsec; /* the prelude has no sections of its own, only functions */
    vec_clear(p->tokens);
    for (int k = 0; k < ufuncs.len; k++) vec_push(builtins, ufuncs.data[k]);
    vec_clear(ufuncs);

    /* user script */
    lex_program(p, p->source, 1);
    collect_function_names(p->tokens.data, p->tokens.len, &ufuncs);
    collect_global_names(p->tokens.data, p->tokens.len, &gvars);
    parse_tokens(p, p->tokens.data, p->tokens.len, builtins.data, builtins.len, ufuncs.data, ufuncs.len, gvars.data,
                 gvars.len, false);
    vec_free(builtins);
    vec_free(ufuncs);
    vec_free(gvars);
    if (p->nerrors == 0) check_program(p);
    if (getenv("FS_DUMP_FUNCS")) { /* debugging aid: print generated functions */
        StrBuf sb = {0};
        for (int i = 0; i < p->funcs.len; i++)
            if (p->funcs.data[i]->name[0] == '_' && p->funcs.data[i]->name[1] == '_' && !p->funcs.data[i]->prelude)
                ast_dump_func(p->funcs.data[i], &sb);
        fprintf(stderr, "%s", sb_cstr(&sb));
        sb_free(&sb);
    }

    if (p->nerrors == 0) {
        p->initial_state = (u32 *)arena_alloc(&p->arena, sizeof(u32) * (size_t)p->state_words);
        state_set_defaults(p, p->initial_state);
        Interp I;
        interp_init(&I, p, p->initial_state);
        for (int i = 0; i < p->sections.len; i++) {
            Section *s = p->sections.data[i];
            if (s->kind != SEC_GLOBAL && s->kind != SEC_CONFIG) continue;
            if (!interp_run_section(&I, s)) {
                prog_diag(p, DIAG_ERROR, s->line, 1, 0, "while computing the initial state: %s", I.error);
                break;
            }
        }
        /* @prepare: once for the initial state (then whenever the state changes, see program_prepare) */
        for (int i = 0; i < p->nprepares && !p->nerrors; i++)
            if (!interp_run_section(&I, p->prepares[i])) {
                prog_diag(p, DIAG_ERROR, p->prepares[i]->line, 1, 0, "while running @prepare: %s", I.error);
                break;
            }
        interp_free(&I);
        /* projective viewers start with the view that shows `center` +- `radius` */
        for (int v = 0; v < p->nviewers; v++) {
            Section *s = p->viewers[v];
            if (!s->mobius) continue;
            u32 *m = p->initial_state + s->mobius->slot;
            float r = wf(p->initial_state[s->radius->slot]) / PROJ_EDGE;
            float m8[8] = {r, 0, wf(p->initial_state[s->center->slot]), wf(p->initial_state[s->center->slot + 1]), 0, 0, 1, 0};
            for (int k = 0; k < 8; k++) m[k] = fw(m8[k]);
        }
        for (int v = 0; v < p->nviewers; v++) initial_center_lo(p, p->viewers[v]);
    }
    return p;
}

/* Run the @prepare sections on `state` (after the state changed).  False (message in err) on an error. */
bool program_prepare(Program *p, u32 *state, char *err, int errcap) {
    if (!p->nprepares) return true;
    Interp I;
    interp_init(&I, p, state);
    I.step_limit = 3e7;
    bool ok = true;
    for (int i = 0; i < p->nprepares && ok; i++) ok = interp_run_section(&I, p->prepares[i]);
    if (!ok && err) snprintf(err, (size_t)errcap, "%s", I.error);
    interp_free(&I);
    return ok;
}

void program_free(Program *p) {
    if (!p) return;
    for (int i = 0; i < p->sections.len; i++) vec_free(p->sections.data[i]->locals);
    for (int i = 0; i < p->funcs.len; i++) vec_free(p->funcs.data[i]->locals);
    vec_free(p->tokens);
    vec_free(p->sections);
    vec_free(p->funcs);
    vec_free(p->globals);
    vec_free(p->consts);
    vec_free(p->config);
    vec_free(p->diags);
    arena_free(&p->arena);
    free(p);
}

static const char *source_line(const char *src, int line, int *len) {
    const char *s = src;
    for (int l = 1; l < line && *s; s++)
        if (*s == '\n') l++;
    const char *e = s;
    while (*e && *e != '\n') e++;
    *len = (int)(e - s);
    return s;
}

/* Deep zooms: is a w x h picture of flat viewer v too fine for single precision?  When a pixel is smaller than
 * about 1e-6 of the size of the numbers (single precision gets noisy from there on); `already` (rendering in
 * double-single precision) lowers the threshold a little, to avoid flapping. */
bool viewer_deep(Program *p, const u32 *state, Section *v, int w, int h, bool already) {
    if (v->mobius || !v->center_lo || w <= 0 || h <= 0) return false;
    double cx = (double)wf(state[v->center->slot]) + wf(state[v->center_lo->slot]);
    double cy = (double)wf(state[v->center->slot + 1]) + wf(state[v->center_lo->slot + 1]);
    double pixel = (double)wf(state[v->radius->slot]) / (0.5 * FS_MIN(w, h));
    return pixel < (already ? 2e-6 : 1e-6) * FS_MAX(hypot(cx, cy), 1.0);
}

/* ... and is it computed in double-single precision (codegen_viewer_ex) then: when .doublePrecision is on */
bool viewer_wants_df(Program *p, const u32 *state, Section *v, int w, int h, bool already) {
    return state[p->s_double->slot] != 0 && viewer_deep(p, state, v, w, h, already);
}

/* "path:line:col: error: message" followed by the source line and a marker */
void program_format_diags(Program *p, StrBuf *out) {
    for (int i = 0; i < p->diags.len; i++) {
        Diag *d = &p->diags.data[i];
        const char *sev = d->severity == DIAG_ERROR ? "error" : d->severity == DIAG_WARNING ? "warning" : "note";
        sb_printf(out, "%s:%d:%d: %s: %s\n", p->path, d->line, d->col, sev, d->msg);
        int len;
        const char *ln = source_line(p->source, d->line, &len);
        if (len > 0 && d->col >= 1) {
            sb_printf(out, "    %.*s\n    ", len, ln);
            for (int k = 1; k < d->col && k <= len; k++) sb_putc(out, ln[k - 1] == '\t' ? '\t' : ' ');
            int w = d->end_col > d->col ? d->end_col - d->col : 1;
            for (int k = 0; k < w && d->col + k <= len + 1; k++) sb_putc(out, '^');
            sb_putc(out, '\n');
        }
    }
}
