/* program.c - loading a script: lex + parse (prelude and user code), check,
 * and run @global/@config to compute the initial state. */
#include "ast.h"

void lex_program(Program *prog, const char *src, int first_line);
void parse_tokens(Program *prog, Token *toks, int ntoks, const char **fnames, int nfnames, bool prelude);
bool check_program(Program *p);

typedef VEC(const char *) NameVec;
static void collect_function_names(Token *t, int n, NameVec *names) {
    for (int i = 0; i + 1 < n; i++)
        if (t[i].kind == TK_DEFINE && t[i + 1].kind == TK_IDENT) vec_push(*names, t[i + 1].str);
}

/* Parse + check + initial state.  Always returns a Program; check
 * p->nerrors / p->diags.  `source` is copied. */
Program *program_load(const char *source, const char *path) {
    types_init();
    Program *p = (Program *)xcalloc(1, sizeof(Program));
    p->path = path ? arena_strndup(&p->arena, path, strlen(path)) : "script";
    p->source = arena_strndup(&p->arena, source, strlen(source));
    p->title = "FractalStream";

    NameVec names = {0};
    for (int k = 0; k < ARRAY_COUNT(intrinsics); k++) vec_push(names, intrinsics[k].name);

    /* prelude */
    lex_program(p, prelude_source, PRELUDE_LINE0 + 1);
    collect_function_names(p->tokens.data, p->tokens.len, &names);
    int nsec = p->sections.len;
    parse_tokens(p, p->tokens.data, p->tokens.len, names.data, names.len, true);
    p->sections.len = nsec; /* the prelude has no sections of its own, only functions */
    vec_clear(p->tokens);

    /* user script */
    lex_program(p, p->source, 1);
    collect_function_names(p->tokens.data, p->tokens.len, &names);
    parse_tokens(p, p->tokens.data, p->tokens.len, names.data, names.len, false);
    vec_free(names);
    if (p->nerrors == 0) check_program(p);

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
        interp_free(&I);
    }
    return p;
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
