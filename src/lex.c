/* lex.c - tokenizer.
 *
 * Newlines are statement separators, except inside ( ) and [ ], where they
 * are ignored so long expressions can be wrapped.  `#` starts a comment.
 * Bytes >= 0x80 are identifier characters, so UTF-8 names like θ work. */
#include "ast.h"

static const struct { const char *word; TokKind kind; } keywords[] = {
    {"define", TK_DEFINE}, {"where", TK_WHERE}, {"end", TK_END}, {"for", TK_FOR}, {"in", TK_IN},
    {"by", TK_BY}, {"do", TK_DO}, {"while", TK_WHILE}, {"if", TK_IF}, {"then", TK_THEN},
    {"else", TK_ELSE}, {"return", TK_RETURN}, {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT}, {"true", TK_TRUE}, {"false", TK_FALSE},
    {"escapes", TK_ESCAPES}, {"vanishes", TK_VANISHES}, {"draw", TK_DRAW}, {"at", TK_AT},
    {"with", TK_WITH}, {"between", TK_BETWEEN}, {"px", TK_PX}, {"mod", TK_MOD}, {"of", TK_OF},
};

const char *tok_kind_name(TokKind k) {
    static const char *names[TK_COUNT] = {
        "end of file", "end of line", "name", "internal name", "integer", "number", "imaginary number",
        "string", "section header", "'('", "')'", "'['", "']'", "','", "':'", "';'", "'|'", "'..'",
        "'+'", "'-'", "'*'", "'/'", "'^'", "'='", "':='", "'+='", "'-='", "'*='", "'/='",
        "'=='", "'!='", "'<'", "'<='", "'>'", "'>='", "'->'",
        "'define'", "'where'", "'end'", "'for'", "'in'", "'by'", "'do'", "'while'", "'if'", "'then'",
        "'else'", "'return'", "'break'", "'continue'", "'and'", "'or'", "'not'", "'true'", "'false'",
        "'escapes'", "'vanishes'", "'draw'", "'at'", "'with'", "'between'", "'px'", "'mod'", "'of'",
    };
    return (k >= 0 && k < TK_COUNT && names[k]) ? names[k] : "?";
}

static bool is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
static bool is_ident_char(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }

typedef struct Lexer {
    Program *prog;
    const char *src;
    int pos, line, line_start;
    int depth; /* ( and [ nesting */
    bool cont; /* after "..." until the next token: blank and comment lines don't end the statement */
} Lexer;

static int lx_col(Lexer *lx, int pos) { return pos - lx->line_start + 1; }

/* A line continues on the next one when it ends with an operator or a comma (it can't end a statement: `x = a +`),
 * after "..." or "…" at its end (as in FractalStream and Matlab), and inside ( ) and [ ]; blank lines and comment
 * lines in between don't end the statement either. */
static bool continues_line(TokKind k) {
    switch (k) {
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_CARET: case TK_COMMA: case TK_ASSIGN:
    case TK_DECLARE: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ: case TK_SLASHEQ: case TK_EQ: case TK_NE: case TK_LT:
    case TK_LE: case TK_GT: case TK_GE: case TK_ARROW: case TK_DOTDOT: case TK_AND: case TK_OR: case TK_NOT: case TK_MOD:
        return true;
    default:
        return false;
    }
}

void lex_program(Program *prog, const char *src, int first_line) {
    Lexer lx = {prog, src, 0, first_line, 0, 0, false};
    bool space = true;
    for (;;) {
        const u8 *s = (const u8 *)src;
        int c = s[lx.pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lx.pos++;
            space = true;
            continue;
        }
        if (c == '#') {
            while (s[lx.pos] && s[lx.pos] != '\n') lx.pos++;
            continue;
        }
        Token t = {0};
        t.line = lx.line;
        t.col = lx_col(&lx, lx.pos);
        t.text = src + lx.pos;
        t.space_before = space;
        space = false;
        int start = lx.pos;
        if (c == 0) {
            t.kind = TK_EOF;
            vec_push(prog->tokens, t);
            break;
        }
        if ((c == '.' && s[lx.pos + 1] == '.' && s[lx.pos + 2] == '.') || (c == 0xE2 && s[lx.pos + 1] == 0x80 && s[lx.pos + 2] == 0xA6)) {
            int q = lx.pos + 3; /* "..." or "…": only spaces or a comment may follow, then the line goes on */
            while (s[q] == ' ' || s[q] == '\t' || s[q] == '\r') q++;
            if (s[q] == '#')
                while (s[q] && s[q] != '\n') q++;
            if (s[q] == '\n' || s[q] == 0) {
                lx.pos = q;
                if (s[q] == '\n') {
                    lx.pos++;
                    lx.line++;
                    lx.line_start = lx.pos;
                }
                space = true;
                lx.cont = true;
                continue;
            }
        }
        if (c == '\n') {
            lx.pos++;
            lx.line++;
            lx.line_start = lx.pos;
            space = true;
            if (lx.depth > 0 || lx.cont) continue;
            if (prog->tokens.len > 0 && continues_line(vec_last(prog->tokens).kind)) continue;
            if (prog->tokens.len == 0 || vec_last(prog->tokens).kind == TK_NEWLINE) continue;
            t.kind = TK_NEWLINE;
            t.len = 1;
            vec_push(prog->tokens, t);
            continue;
        }
        lx.cont = false;   /* a token: a "..." before it has done its job */
        if (is_ident_start(c)) {
            while (is_ident_char(s[lx.pos])) lx.pos++;
            t.len = lx.pos - start;
            t.kind = TK_IDENT;
            for (int k = 0; k < ARRAY_COUNT(keywords); k++)
                if ((int)strlen(keywords[k].word) == t.len && memcmp(keywords[k].word, t.text, (size_t)t.len) == 0)
                    t.kind = keywords[k].kind;
            t.str = arena_strndup(&prog->arena, t.text, (size_t)t.len);
            vec_push(prog->tokens, t);
            continue;
        }
        if (is_digit(c)) {
            bool real = false;
            while (is_digit(s[lx.pos])) lx.pos++;
            if (s[lx.pos] == '.' && is_digit(s[lx.pos + 1])) {
                real = true;
                lx.pos++;
                while (is_digit(s[lx.pos])) lx.pos++;
            }
            if ((s[lx.pos] == 'e' || s[lx.pos] == 'E') &&
                (is_digit(s[lx.pos + 1]) || ((s[lx.pos + 1] == '+' || s[lx.pos + 1] == '-') && is_digit(s[lx.pos + 2])))) {
                real = true;
                lx.pos += 2;
                while (is_digit(s[lx.pos])) lx.pos++;
            }
            char buf[64];
            int n = FS_MIN(lx.pos - start, 63);
            memcpy(buf, src + start, (size_t)n);
            buf[n] = 0;
            t.num = strtod(buf, NULL);
            t.kind = real ? TK_REAL : TK_INT;
            if (s[lx.pos] == 'i' && !is_ident_char(s[lx.pos + 1])) {
                lx.pos++;
                t.kind = TK_IMAG;
            }
            if (t.kind == TK_INT && t.num > 2147483647.0)
                prog_diag(prog, DIAG_ERROR, t.line, t.col, t.col + n, "integer %s is too large (the limit is 2147483647); write it as %s.0 for a real number", buf, buf);
            t.len = lx.pos - start;
            vec_push(prog->tokens, t);
            continue;
        }
        if (c == '"') {
            lx.pos++;
            StrBuf sb = {0};
            while (s[lx.pos] && s[lx.pos] != '"' && s[lx.pos] != '\n') {
                if (s[lx.pos] == '\\' && s[lx.pos + 1]) {
                    lx.pos++;
                    char e = (char)s[lx.pos];
                    sb_putc(&sb, e == 'n' ? '\n' : e == 't' ? '\t' : e);
                } else {
                    sb_putc(&sb, (char)s[lx.pos]);
                }
                lx.pos++;
            }
            if (s[lx.pos] != '"')
                prog_diag(prog, DIAG_ERROR, t.line, t.col, lx_col(&lx, lx.pos), "this string is missing its closing \"");
            else
                lx.pos++;
            t.kind = TK_STRING;
            t.len = lx.pos - start;
            int nt = prog->tokens.len; /* "abc " + "def" is one text (a long note: "abc " + at the end of a line) */
            if (nt >= 2 && prog->tokens.data[nt - 1].kind == TK_PLUS && prog->tokens.data[nt - 2].kind == TK_STRING) {
                prog->tokens.len--;
                Token *prev = &vec_last(prog->tokens);
                StrBuf all = {0};
                sb_puts(&all, prev->str);
                sb_puts(&all, sb_cstr(&sb));
                prev->str = arena_strndup(&prog->arena, sb_cstr(&all), (size_t)all.len);
                sb_free(&all);
                sb_free(&sb);
                continue;
            }
            t.str = arena_strndup(&prog->arena, sb_cstr(&sb), (size_t)sb.len);
            sb_free(&sb);
            vec_push(prog->tokens, t);
            continue;
        }
        if (c == '@' && is_ident_start(s[lx.pos + 1])) {
            lx.pos++;
            while (is_ident_char(s[lx.pos])) lx.pos++;
            if (s[lx.pos] == '.' && is_ident_start(s[lx.pos + 1])) { /* @julia.tool */
                lx.pos++;
                while (is_ident_char(s[lx.pos])) lx.pos++;
            }
            t.kind = TK_SECTION;
            t.len = lx.pos - start;
            t.str = arena_strndup(&prog->arena, t.text + 1, (size_t)t.len - 1);
            /* a section header must start its line */
            for (int p = lx.line_start; p < start; p++)
                if (s[p] != ' ' && s[p] != '\t') {
                    prog_diag(prog, DIAG_ERROR, t.line, t.col, t.col + t.len, "section headers like @%s must be at the start of a line", t.str);
                    break;
                }
            vec_push(prog->tokens, t);
            continue;
        }
        if (c == '.' && is_ident_start(s[lx.pos + 1])) {
            lx.pos++;
            while (is_ident_char(s[lx.pos])) lx.pos++;
            t.kind = TK_INTERNAL;
            t.len = lx.pos - start;
            t.str = arena_strndup(&prog->arena, t.text + 1, (size_t)t.len - 1);
            vec_push(prog->tokens, t);
            continue;
        }
        /* punctuation */
        int c2 = s[lx.pos + 1];
        TokKind k = TK_EOF;
        int len = 1;
        switch (c) {
        case '(': k = TK_LPAREN; lx.depth++; break;
        case ')': k = TK_RPAREN; if (lx.depth > 0) lx.depth--; break;
        case '[': k = TK_LBRACKET; lx.depth++; break;
        case ']': k = TK_RBRACKET; if (lx.depth > 0) lx.depth--; break;
        case ',': k = TK_COMMA; break;
        case ';': k = TK_SEMI; break;
        case '|': k = TK_BAR; break;
        case '^': k = TK_CARET; break;
        case '.': if (c2 == '.') { k = TK_DOTDOT; len = 2; } break;
        case ':': if (c2 == '=') { k = TK_DECLARE; len = 2; } else k = TK_COLON; break;
        case '+': if (c2 == '=') { k = TK_PLUSEQ; len = 2; } else k = TK_PLUS; break;
        case '-': if (c2 == '=') { k = TK_MINUSEQ; len = 2; } else if (c2 == '>') { k = TK_ARROW; len = 2; } else k = TK_MINUS; break;
        case '*': if (c2 == '=') { k = TK_STAREQ; len = 2; } else if (c2 == '*') { k = TK_CARET; len = 2; } else k = TK_STAR; break;
        case '/': if (c2 == '=') { k = TK_SLASHEQ; len = 2; } else k = TK_SLASH; break;
        case '=': if (c2 == '=') { k = TK_EQ; len = 2; } else k = TK_ASSIGN; break;
        case '!': if (c2 == '=') { k = TK_NE; len = 2; } break;
        case '<': if (c2 == '=') { k = TK_LE; len = 2; } else k = TK_LT; break;
        case '>': if (c2 == '=') { k = TK_GE; len = 2; } else k = TK_GT; break;
        }
        if (k == TK_EOF) {
            if (c == '.' && is_digit(c2))
                prog_diag(prog, DIAG_ERROR, t.line, t.col, t.col + 1, "write a leading zero, like 0.5 instead of .5");
            else
                prog_diag(prog, DIAG_ERROR, t.line, t.col, t.col + 1, "unexpected character '%c'", c);
            lx.pos++;
            continue;
        }
        lx.pos += len;
        t.kind = k;
        t.len = len;
        vec_push(prog->tokens, t);
    }
}
