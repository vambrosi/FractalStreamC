/* editor.c - the code editor widget.  See editor.h for the contract.
 *
 * Layout of this file:
 *   1. data structures and the text buffer (a plain byte array plus a lazily rebuilt line index)
 *   2. undo history: every change goes through edp_replace(), which records it
 *   3. tokenizer for syntax highlighting (also used for auto-indent)
 *   4. cursor movement and the editing commands
 *   5. the widget: layout, input, drawing
 * Private helpers are prefixed edp_ because the whole program is one translation unit. */
#include "editor.h"

#define EDP_MAX_BYTES (16 * 1024 * 1024)
#define EDP_INDENT 4
#define EDP_UNDO_PAUSE 1.0 /* seconds of inactivity that end an undo group */
#define EDP_MAX_UNDO 4000
#define EDP_BLINK 0.53     /* caret blink half-period, seconds */
#define EDP_TIP_DELAY 0.35 /* hover time before a diagnostic tooltip appears */

/* ======================================================================================================
 * 1. data and buffer
 * ==================================================================================================== */

typedef enum EdKind { EDK_OTHER, EDK_TYPE, EDK_BACKSPACE, EDK_DELETE } EdKind;

/* One undoable change: at `pos`, `removed` was replaced by `inserted`. */
typedef struct EdRec {
    int pos;
    char *removed;
    int removed_len;
    char *inserted;
    int inserted_len;
    int cursor_before, anchor_before, cursor_after, anchor_after;
    int group; /* records of one command share a group and are undone together */
    EdKind kind;
    double time;
} EdRec;

typedef struct EdDiag {
    int line, col, end_col, severity;
    char *message;
} EdDiag;

struct Editor {
    char *text; /* always NUL-terminated */
    int len, cap;
    VEC(int) lines; /* byte offset of each line start; valid unless lines_dirty */
    bool lines_dirty;
    int max_line_bytes;
    int cursor, anchor;
    int want_col; /* column (characters) kept across Up/Down, or -1 */
    bool modified;
    u32 version;

    VEC(EdRec) undo, redo;
    int group_counter;
    double clock;
    bool merge_ok; /* the previous command was typing/backspace/delete and nothing interrupted it */
    int cmd_first; /* undo.len when the current command began */
    int cmd_cursor_before, cmd_anchor_before, cmd_group;
    bool cmd_merged;
    EdKind cmd_kind;
    int electric_line_start, electric_removed; /* armed after typing `end`/`else` dedented the line */

    /* view */
    float scroll_x, scroll_y;
    bool reveal; /* scroll the caret into view at the next draw */
    int page_rows;
    double caret_epoch;
    /* mouse */
    int drag_mode; /* 0 none, 1 characters, 2 words, 3 lines */
    int drag_a0, drag_a1; /* the word/line the drag started on */
    float sb_grab;
    float hover_x, hover_y; /* where the mouse rested (diagnostic tooltips) */
    double hover_since;
    bool had_focus; /* focus state of the previous frame */
    /* decoration */
    VEC(EdDiag) diags;
    char **builtins;
    int nbuiltins;
};

Editor *editor_create(void) {
    Editor *ed = xcalloc(1, sizeof *ed);
    ed->cap = 256;
    ed->text = xcalloc(1, (size_t)ed->cap);
    ed->lines_dirty = true;
    ed->want_col = -1;
    ed->page_rows = 20;
    return ed;
}

static void edp_rec_free(EdRec *r) {
    free(r->removed);
    free(r->inserted);
}

static void edp_free_history(Editor *ed) {
    for (int i = 0; i < ed->undo.len; i++) edp_rec_free(&ed->undo.data[i]);
    for (int i = 0; i < ed->redo.len; i++) edp_rec_free(&ed->redo.data[i]);
    ed->undo.len = ed->redo.len = 0;
}

static void edp_free_diags(Editor *ed) {
    for (int i = 0; i < ed->diags.len; i++) free(ed->diags.data[i].message);
    ed->diags.len = 0;
}

static void edp_free_builtins(Editor *ed) {
    for (int i = 0; i < ed->nbuiltins; i++) free(ed->builtins[i]);
    free(ed->builtins);
    ed->builtins = NULL;
    ed->nbuiltins = 0;
}

void editor_destroy(Editor *ed) {
    if (!ed) return;
    edp_free_history(ed);
    edp_free_diags(ed);
    edp_free_builtins(ed);
    vec_free(ed->undo);
    vec_free(ed->redo);
    vec_free(ed->diags);
    vec_free(ed->lines);
    free(ed->text);
    free(ed);
}

void editor_set_clock(Editor *ed, double seconds) { ed->clock = seconds; }
int editor_length(const Editor *ed) { return ed->len; }
bool editor_is_modified(const Editor *ed) { return ed->modified; }
void editor_set_unmodified(Editor *ed) { ed->modified = false; }
u32 editor_version(const Editor *ed) { return ed->version; }
int editor_cursor(const Editor *ed) { return ed->cursor; }
int editor_anchor(const Editor *ed) { return ed->anchor; }

const char *editor_get_text(const Editor *ed, int *len) {
    if (len) *len = ed->len;
    return ed->text;
}

/* Replace [pos, pos+del_len) by ins.  No history, no cursor handling.  False if it would exceed the size limit. */
static bool edp_raw(Editor *ed, int pos, int del_len, const char *ins, int ins_len) {
    if (pos < 0 || del_len < 0 || ins_len < 0 || pos + del_len > ed->len) return false;
    if ((long long)ed->len - del_len + ins_len > EDP_MAX_BYTES) return false;
    int need = ed->len - del_len + ins_len + 1;
    if (need > ed->cap) {
        ed->cap = FS_MAX(need, ed->cap * 2);
        ed->text = xrealloc(ed->text, (size_t)ed->cap);
    }
    memmove(ed->text + pos + ins_len, ed->text + pos + del_len, (size_t)(ed->len - pos - del_len + 1));
    if (ins_len) memcpy(ed->text + pos, ins, (size_t)ins_len);
    ed->len += ins_len - del_len;
    ed->lines_dirty = true;
    ed->version++;
    ed->modified = true;
    return true;
}

static void edp_index(Editor *ed) {
    if (!ed->lines_dirty) return;
    vec_clear(ed->lines);
    vec_push(ed->lines, 0);
    int start = 0, longest = 0;
    for (int i = 0; i < ed->len; i++) {
        if (ed->text[i] == '\n') {
            longest = FS_MAX(longest, i - start);
            vec_push(ed->lines, i + 1);
            start = i + 1;
        }
    }
    ed->max_line_bytes = FS_MAX(longest, ed->len - start);
    ed->lines_dirty = false;
}

int editor_line_count(Editor *ed) {
    edp_index(ed);
    return ed->lines.len;
}

/* 0-based line containing the offset (binary search in the line index). */
static int edp_line_of(Editor *ed, int offset) {
    edp_index(ed);
    int lo = 0, hi = ed->lines.len - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ed->lines.data[mid] <= offset) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int edp_ls(Editor *ed, int line0) { /* start of 0-based line */
    edp_index(ed);
    return ed->lines.data[FS_CLAMP(line0, 0, ed->lines.len - 1)];
}

static int edp_le(Editor *ed, int line0) { /* end of 0-based line: offset of its '\n' or the end of text */
    edp_index(ed);
    line0 = FS_CLAMP(line0, 0, ed->lines.len - 1);
    return line0 + 1 < ed->lines.len ? ed->lines.data[line0 + 1] - 1 : ed->len;
}

int editor_line_start(Editor *ed, int line) { return edp_ls(ed, line - 1); }
int editor_line_end(Editor *ed, int line) { return edp_le(ed, line - 1); }

void editor_offset_to_pos(Editor *ed, int offset, int *line, int *col) {
    offset = FS_CLAMP(offset, 0, ed->len);
    int l = edp_line_of(ed, offset);
    if (line) *line = l + 1;
    if (col) *col = offset - edp_ls(ed, l) + 1;
}

int editor_pos_to_offset(Editor *ed, int line, int col) {
    int l = FS_CLAMP(line - 1, 0, editor_line_count(ed) - 1);
    int ls = edp_ls(ed, l), le = edp_le(ed, l);
    return FS_CLAMP(ls + col - 1, ls, le);
}

/* Number of characters between two offsets (the visual column of `offset` when `from` is a line start). */
static int edp_chars_between(const Editor *ed, int from, int to) {
    int n = 0;
    for (int i = from; i < to; i++)
        if (((unsigned char)ed->text[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Offset reached by advancing `cols` characters from the start of a line, stopping at the line end. */
static int edp_advance_chars(Editor *ed, int line0, int cols) {
    int i = edp_ls(ed, line0), le = edp_le(ed, line0);
    while (cols > 0 && i < le) {
        i = ui_utf8_next(ed->text, i);
        cols--;
    }
    return FS_MIN(i, le);
}

/* ---- input sanitising ------------------------------------------------------------------------------------------ */

/* Normalized copy of s: valid UTF-8, '\n' line ends, no control characters, tabs expanded to 4-column stops.
 * `col0` is the column the text will start at (for tab stops).  malloc'd. */
static char *edp_normalize(const char *s, int len, int col0, int *out_len) {
    StrBuf sb = { 0 };
    int col = col0;
    for (int i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') {
            i += (i + 1 < len && s[i + 1] == '\n') ? 2 : 1;
            sb_putc(&sb, '\n');
            col = 0;
        } else if (c == '\n') {
            sb_putc(&sb, '\n');
            col = 0;
            i++;
        } else if (c == '\t') {
            int n = EDP_INDENT - col % EDP_INDENT;
            for (int k = 0; k < n; k++) sb_putc(&sb, ' ');
            col += n;
            i++;
        } else if (c < 32 || c == 127) {
            i++;
        } else {
            u32 cp;
            int l = utf8_decode(s + i, len - i, &cp);
            if (l < 1) l = 1;
            i += l;
            if (cp >= 0x80 && cp < 0xA0) continue; /* C1 controls */
            char u[4];
            int n = utf8_encode(cp, u);
            sb_append(&sb, u, n);
            col++;
        }
    }
    int n = sb.len;
    char *out = xmalloc((size_t)n + 1);
    if (n) memcpy(out, sb.data, (size_t)n);
    out[n] = 0;
    sb_free(&sb);
    *out_len = n;
    return out;
}

void editor_set_text(Editor *ed, const char *utf8, int len) {
    if (!utf8) utf8 = "";
    if (len < 0) len = (int)strlen(utf8);
    int n;
    char *norm = edp_normalize(utf8, len, 0, &n);
    edp_free_history(ed);
    ed->len = 0;
    ed->text[0] = 0;
    edp_raw(ed, 0, 0, norm, n);
    free(norm);
    ed->cursor = ed->anchor = 0;
    ed->want_col = -1;
    ed->scroll_x = ed->scroll_y = 0;
    ed->merge_ok = false;
    ed->electric_removed = 0;
    ed->modified = false;
    ed->drag_mode = 0;
    ed->reveal = true;
}

/* ======================================================================================================
 * 2. undo history
 * ==================================================================================================== */

static bool edp_is_word_cp(u32 cp) {
    if (cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
    return cp >= 0x80 && !(cp >= 0x2190 && cp <= 0x22FF) && cp != 0xD7 && cp != 0xF7; /* letters; not arrows/operators */
}

/* Character class for undo grouping: 0 blank/punctuation, 1 word, 2 newline (never merged). */
static int edp_merge_class(u32 cp) { return cp == '\n' ? 2 : edp_is_word_cp(cp) ? 1 : 0; }

static u32 edp_cp_at(const char *s, int i) {
    u32 cp = 0;
    int avail = 0;
    while (avail < 4 && s[i + avail]) avail++;
    if (avail) utf8_decode(s + i, avail, &cp);
    return cp;
}

/* Start of a command: remember the caret for the undo record and the merge candidate. */
static void edp_begin(Editor *ed, EdKind kind) {
    ed->cmd_first = ed->undo.len;
    ed->cmd_cursor_before = ed->cursor;
    ed->cmd_anchor_before = ed->anchor;
    ed->cmd_group = 0;
    ed->cmd_merged = false;
    ed->cmd_kind = kind;
}

/* End of a command: stamp the caret into the records it touched. */
static void edp_end(Editor *ed) {
    int first = ed->cmd_merged && ed->cmd_first > 0 ? ed->cmd_first - 1 : ed->cmd_first;
    for (int i = first; i < ed->undo.len; i++) {
        ed->undo.data[i].cursor_after = ed->cursor;
        ed->undo.data[i].anchor_after = ed->anchor;
    }
    ed->merge_ok = ed->cmd_kind != EDK_OTHER;
    if (ed->undo.len > EDP_MAX_UNDO) { /* forget the oldest records */
        int drop = ed->undo.len - EDP_MAX_UNDO;
        for (int i = 0; i < drop; i++) edp_rec_free(&ed->undo.data[i]);
        memmove(ed->undo.data, ed->undo.data + drop, sizeof(EdRec) * (size_t)(ed->undo.len - drop));
        ed->undo.len -= drop;
    }
}

/* Anything that is not typing ends the current undo group and the "just dedented" state. */
static void edp_interrupt(Editor *ed) {
    ed->merge_ok = false;
    ed->electric_removed = 0;
}

/* The one place where text changes (besides undo/redo): replaces [pos, pos+del_len) with ins and records it,
 * merging into the previous record while the user keeps typing a word / backspacing / deleting. */
static bool edp_replace(Editor *ed, int pos, int del_len, const char *ins, int ins_len) {
    if (del_len == 0 && ins_len == 0) return true;
    char *removed = xstrndup(ed->text + pos, (size_t)del_len);
    if (!edp_raw(ed, pos, del_len, ins, ins_len)) {
        free(removed);
        return false;
    }
    for (int i = 0; i < ed->redo.len; i++) edp_rec_free(&ed->redo.data[i]);
    ed->redo.len = 0;

    bool first = ed->undo.len == ed->cmd_first && !ed->cmd_merged;
    if (first && ed->merge_ok && ed->undo.len > 0) {
        EdRec *p = &ed->undo.data[ed->undo.len - 1];
        if (p->kind == ed->cmd_kind && ed->cmd_kind != EDK_OTHER && ed->clock - p->time < EDP_UNDO_PAUSE) {
            if (ed->cmd_kind == EDK_TYPE && del_len == 0 && p->removed_len == 0 && pos == p->pos + p->inserted_len) {
                int last = ui_utf8_prev(p->inserted, p->inserted_len);
                u32 a = edp_cp_at(p->inserted, last), b = edp_cp_at(ins, 0);
                int ca = edp_merge_class(a), cb = edp_merge_class(b); /* a word keeps the blanks/punctuation after it */
                if (ca != 2 && cb != 2 && (ca == cb || (ca == 1 && cb == 0))) {
                    p->inserted = xrealloc(p->inserted, (size_t)(p->inserted_len + ins_len + 1));
                    memcpy(p->inserted + p->inserted_len, ins, (size_t)ins_len);
                    p->inserted_len += ins_len;
                    p->inserted[p->inserted_len] = 0;
                    p->time = ed->clock;
                    ed->cmd_merged = true;
                    free(removed);
                    return true;
                }
            } else if (ed->cmd_kind == EDK_BACKSPACE && ins_len == 0 && p->inserted_len == 0 && pos + del_len == p->pos) {
                char *joined = xmalloc((size_t)(del_len + p->removed_len + 1));
                memcpy(joined, removed, (size_t)del_len);
                memcpy(joined + del_len, p->removed, (size_t)p->removed_len + 1);
                free(p->removed);
                p->removed = joined;
                p->removed_len += del_len;
                p->pos = pos;
                p->time = ed->clock;
                ed->cmd_merged = true;
                free(removed);
                return true;
            } else if (ed->cmd_kind == EDK_DELETE && ins_len == 0 && p->inserted_len == 0 && pos == p->pos) {
                p->removed = xrealloc(p->removed, (size_t)(p->removed_len + del_len + 1));
                memcpy(p->removed + p->removed_len, removed, (size_t)del_len + 1);
                p->removed_len += del_len;
                p->time = ed->clock;
                ed->cmd_merged = true;
                free(removed);
                return true;
            }
        }
    }
    if (ed->cmd_group == 0) ed->cmd_group = ++ed->group_counter;
    EdRec r = { pos, removed, del_len, xstrndup(ins, (size_t)ins_len), ins_len, ed->cmd_cursor_before, ed->cmd_anchor_before,
                0, 0, ed->cmd_group, ed->cmd_kind, ed->clock };
    vec_push(ed->undo, r);
    return true;
}

bool editor_undo(Editor *ed) {
    if (ed->undo.len == 0) return false;
    int group = ed->undo.data[ed->undo.len - 1].group, cur = 0, anc = 0;
    while (ed->undo.len > 0 && ed->undo.data[ed->undo.len - 1].group == group) {
        EdRec r = vec_pop(ed->undo);
        edp_raw(ed, r.pos, r.inserted_len, r.removed, r.removed_len);
        cur = r.cursor_before;
        anc = r.anchor_before;
        vec_push(ed->redo, r);
    }
    ed->cursor = FS_CLAMP(cur, 0, ed->len);
    ed->anchor = FS_CLAMP(anc, 0, ed->len);
    ed->want_col = -1;
    ed->reveal = true;
    edp_interrupt(ed);
    return true;
}

bool editor_redo(Editor *ed) {
    if (ed->redo.len == 0) return false;
    int group = ed->redo.data[ed->redo.len - 1].group, cur = 0, anc = 0;
    while (ed->redo.len > 0 && ed->redo.data[ed->redo.len - 1].group == group) {
        EdRec r = vec_pop(ed->redo);
        edp_raw(ed, r.pos, r.removed_len, r.inserted, r.inserted_len);
        cur = r.cursor_after;
        anc = r.anchor_after;
        vec_push(ed->undo, r);
    }
    ed->cursor = FS_CLAMP(cur, 0, ed->len);
    ed->anchor = FS_CLAMP(anc, 0, ed->len);
    ed->want_col = -1;
    ed->reveal = true;
    edp_interrupt(ed);
    return true;
}

/* ======================================================================================================
 * 3. tokenizer
 * ==================================================================================================== */

typedef enum EdTokClass {
    EDTK_TEXT, EDTK_COMMENT, EDTK_STRING, EDTK_NUMBER, EDTK_HEADER, EDTK_INTERNAL, EDTK_KEYWORD, EDTK_TYPE,
    EDTK_BUILTIN, EDTK_OPERATOR
} EdTokClass;

typedef struct EdTok {
    int start, end;
    u8 cls;
} EdTok;

static const char *const edp_keywords[] = {
    "define", "where", "end", "for", "in", "by", "do", "while", "if", "then", "else", "return", "break", "continue",
    "and", "or", "not", "true", "false", "escapes", "vanishes", "draw", "at", "with", "between", "to", "radius", "px",
    "point", "circle", "disk", "line", "color", "width", "List", "of",
};
static const char *const edp_types[] = { "Z", "R", "C", "H", "Color", "Bool", "Integers", "Reals", "Complex", "Quaternions" };
static const char *const edp_default_builtins[] = { /* docs/language.md "Built-in functions" (the app can replace the list) */
    "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "exp", "log", "ln",
    "sqrt", "atan2", "log2", "log10", "floor", "ceil", "round", "trunc", "fract", "sign", "min", "max", "clamp", "mix",
    "smoothstep", "step", "isnan", "isinf", "re", "im", "arg", "conj", "norm", "abs", "complex", "cis", "polar", "quat",
    "dot", "length", "append", "clear", "rgb", "hsv", "gray", "pi", "tau",
};

static bool edp_in_list(const char *const *list, int n, const char *s, int len) {
    for (int i = 0; i < n; i++)
        if ((int)strlen(list[i]) == len && memcmp(list[i], s, (size_t)len) == 0) return true;
    return false;
}

static bool edp_is_builtin(const Editor *ed, const char *s, int len) {
    if (ed && ed->builtins) {
        for (int i = 0; i < ed->nbuiltins; i++)
            if ((int)strlen(ed->builtins[i]) == len && memcmp(ed->builtins[i], s, (size_t)len) == 0) return true;
        return false;
    }
    return edp_in_list(edp_default_builtins, ARRAY_COUNT(edp_default_builtins), s, len);
}

static bool edp_ident_start(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
static bool edp_ident_char(unsigned char c) { return edp_ident_start(c) || (c >= '0' && c <= '9'); }
static bool edp_digit(int c) { return c >= '0' && c <= '9'; }

/* Split one line (s, n) into highlighted tokens; text between tokens is plain.  Returns the token count. */
static int edp_tokenize(const Editor *ed, const char *s, int n, EdTok *out, int maxtok) {
    int nt = 0, i = 0;
#define EDP_EMIT(a, b, c) do { if (nt < maxtok) { out[nt].start = (a); out[nt].end = (b); out[nt].cls = (u8)(c); nt++; } } while (0)
    while (i < n && (s[i] == ' ')) i++;
    if (i < n && s[i] == '@') { /* section header */
        int j = i + 1;
        while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
        static const char *const heads[] = { "@viewer", "@tool", "@config", "@global" };
        if (edp_in_list(heads, 4, s + i, j - i)) { EDP_EMIT(i, j, EDTK_HEADER); i = j; }
    }
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') { i++; continue; }
        if (c == '#') { EDP_EMIT(i, n, EDTK_COMMENT); break; }
        if (c == '"') {
            int j = i + 1;
            while (j < n && s[j] != '"') j += (s[j] == '\\' && j + 1 < n) ? 2 : 1;
            if (j < n) j++;
            EDP_EMIT(i, FS_MIN(j, n), EDTK_STRING);
            i = FS_MIN(j, n);
            continue;
        }
        bool prev_ident = i > 0 && (edp_ident_char((unsigned char)s[i - 1]) || s[i - 1] == ')' || s[i - 1] == ']' || s[i - 1] == '.');
        if (edp_digit(c) || (c == '.' && i + 1 < n && edp_digit(s[i + 1]) && !prev_ident)) {
            int j = i;
            while (j < n && edp_digit(s[j])) j++;
            if (j + 1 < n && s[j] == '.' && edp_digit(s[j + 1])) {
                j++;
                while (j < n && edp_digit(s[j])) j++;
            }
            if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                int k = j + 1;
                if (k < n && (s[k] == '+' || s[k] == '-')) k++;
                if (k < n && edp_digit(s[k])) {
                    while (k < n && edp_digit(s[k])) k++;
                    j = k;
                }
            }
            if (j < n && s[j] == 'i' && !(j + 1 < n && edp_ident_char((unsigned char)s[j + 1]))) j++; /* 3i */
            EDP_EMIT(i, j, EDTK_NUMBER);
            i = j;
            continue;
        }
        if (c == '.' && i + 1 < n && edp_ident_start((unsigned char)s[i + 1])) { /* .internal */
            int j = i + 1;
            while (j < n && edp_ident_char((unsigned char)s[j])) j++;
            EDP_EMIT(i, j, EDTK_INTERNAL);
            i = j;
            continue;
        }
        if (edp_ident_start(c)) {
            int j = i + 1;
            while (j < n && edp_ident_char((unsigned char)s[j])) j++;
            int len = j - i;
            if (edp_in_list(edp_keywords, ARRAY_COUNT(edp_keywords), s + i, len)) EDP_EMIT(i, j, EDTK_KEYWORD);
            else if (edp_in_list(edp_types, ARRAY_COUNT(edp_types), s + i, len)) EDP_EMIT(i, j, EDTK_TYPE);
            else if (edp_is_builtin(ed, s + i, len)) EDP_EMIT(i, j, EDTK_BUILTIN);
            else if (len == 1 && c == 'i') EDP_EMIT(i, j, EDTK_NUMBER); /* the imaginary unit */
            i = j;
            continue;
        }
        if (c && strchr("+-*/^=<>!()[]{}:,;|&~%.", c)) EDP_EMIT(i, i + 1, EDTK_OPERATOR);
        i++;
    }
#undef EDP_EMIT
    return nt;
}

/* Does the text s[0..n) (the line up to the caret) end in a word that opens an indented block? */
static bool edp_opens_block(const Editor *ed, const char *s, int n) {
    EdTok toks[256];
    int nt = edp_tokenize(ed, s, n, toks, 256);
    for (int i = nt - 1; i >= 0; i--) {
        if (toks[i].cls == EDTK_COMMENT) continue;
        if (toks[i].cls != EDTK_KEYWORD) return false;
        for (int k = toks[i].end; k < (i + 1 < nt ? toks[i + 1].start : n); k++)
            if (s[k] != ' ') return false; /* plain words after the keyword: `if a then b` */
        static const char *const openers[] = { "do", "then", "else", "where" };
        return edp_in_list(openers, 4, s + toks[i].start, toks[i].end - toks[i].start);
    }
    return false;
}

void editor_set_builtins(Editor *ed, const char *const *names, int n) {
    edp_free_builtins(ed);
    if (!names || n < 0) return;
    ed->builtins = xcalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) ed->builtins[i] = xstrdup(names[i]);
    ed->nbuiltins = n;
}

void editor_set_diagnostics(Editor *ed, const EditorDiag *diags, int n) {
    edp_free_diags(ed);
    for (int i = 0; i < n; i++) {
        EdDiag d = { diags[i].line, diags[i].col, diags[i].end_col, diags[i].severity, xstrdup(diags[i].message ? diags[i].message : "") };
        vec_push(ed->diags, d);
    }
}

void editor_clear_diagnostics(Editor *ed) { edp_free_diags(ed); }

/* ======================================================================================================
 * 4. movement and editing commands
 * ==================================================================================================== */

void editor_set_selection(Editor *ed, int anchor, int cursor) {
    ed->anchor = FS_CLAMP(anchor, 0, ed->len);
    ed->cursor = FS_CLAMP(cursor, 0, ed->len);
    ed->want_col = -1;
    ed->reveal = true;
    edp_interrupt(ed);
}

void editor_select_all(Editor *ed) { editor_set_selection(ed, 0, ed->len); }

void editor_goto(Editor *ed, int line, int col) {
    int o = editor_pos_to_offset(ed, line, col);
    editor_set_selection(ed, o, o);
}

char *editor_selected_text(const Editor *ed) {
    int lo = FS_MIN(ed->cursor, ed->anchor), hi = FS_MAX(ed->cursor, ed->anchor);
    return xstrndup(ed->text + lo, (size_t)(hi - lo));
}

void editor_move(Editor *ed, EditorMove m, bool extend) {
    int lo = FS_MIN(ed->cursor, ed->anchor), hi = FS_MAX(ed->cursor, ed->anchor);
    int c = ed->cursor, line = edp_line_of(ed, c);
    bool vertical = false;
    switch (m) {
    case EDITOR_MOVE_LEFT: c = (!extend && lo != hi) ? lo : ui_utf8_prev(ed->text, c); break;
    case EDITOR_MOVE_RIGHT: c = (!extend && lo != hi) ? hi : ui_utf8_next(ed->text, c); break;
    case EDITOR_MOVE_UP:
    case EDITOR_MOVE_DOWN:
    case EDITOR_MOVE_PAGE_UP:
    case EDITOR_MOVE_PAGE_DOWN: {
        int step = (m == EDITOR_MOVE_UP || m == EDITOR_MOVE_DOWN) ? 1 : FS_MAX(1, ed->page_rows);
        int dir = (m == EDITOR_MOVE_UP || m == EDITOR_MOVE_PAGE_UP) ? -1 : 1;
        int target = line + dir * step;
        vertical = true;
        if (ed->want_col < 0) ed->want_col = edp_chars_between(ed, edp_ls(ed, line), c);
        if (target < 0) { c = 0; ed->want_col = -1; vertical = false; }
        else if (target >= ed->lines.len) { c = ed->len; ed->want_col = -1; vertical = false; }
        else c = edp_advance_chars(ed, target, ed->want_col);
        break;
    }
    case EDITOR_MOVE_WORD_LEFT: c = ui_word_left(ed->text, c); break;
    case EDITOR_MOVE_WORD_RIGHT: c = ui_word_right(ed->text, c); break;
    case EDITOR_MOVE_LINE_START: {
        int ls = edp_ls(ed, line), fn = ls;
        while (fn < ed->len && ed->text[fn] == ' ') fn++;
        c = (c == fn && fn != ls) ? ls : fn; /* first non-blank, then column 1 */
        if (fn == edp_le(ed, line)) c = ls;  /* blank line */
        break;
    }
    case EDITOR_MOVE_LINE_END: c = edp_le(ed, line); break;
    case EDITOR_MOVE_DOC_START: c = 0; break;
    case EDITOR_MOVE_DOC_END: c = ed->len; break;
    }
    ed->cursor = FS_CLAMP(c, 0, ed->len);
    if (!extend) ed->anchor = ed->cursor;
    if (!vertical) ed->want_col = -1;
    ed->reveal = true;
    edp_interrupt(ed);
}

static void edp_selection(const Editor *ed, int *lo, int *hi) {
    *lo = FS_MIN(ed->cursor, ed->anchor);
    *hi = FS_MAX(ed->cursor, ed->anchor);
}

/* Typing `end` or `else` as the only word on an indented line moves it one level out.  If the user keeps typing
 * a longer word (`endpoint`), the indentation is given back.
 * edp_electric_spaces() looks ahead: if typing `norm` over [lo, hi) completes such a word, it returns how many
 * spaces the line loses (else 0). */
static int edp_electric_spaces(Editor *ed, int lo, int hi, const char *norm, int n) {
    if (n > 4 || !edp_is_word_cp(edp_cp_at(norm, ui_utf8_prev(norm, n)))) return 0;
    int ls = edp_ls(ed, edp_line_of(ed, lo)), k = 0;
    while (ls + k < lo && ed->text[ls + k] == ' ') k++;
    int before = lo - (ls + k);
    if (k < 1 || before + n < 3 || before + n > 4 || edp_is_word_cp(edp_cp_at(ed->text, hi))) return 0;
    char w[8];
    memcpy(w, ed->text + ls + k, (size_t)before);
    memcpy(w + before, norm, (size_t)n);
    int wl = before + n;
    bool hit = (wl == 3 && memcmp(w, "end", 3) == 0) || (wl == 4 && memcmp(w, "else", 4) == 0);
    return hit ? FS_MIN(EDP_INDENT, k) : 0;
}

/* Called right after a letter was typed (the caret is behind it): dedent by `dedent` spaces, or give back the
 * indentation a previous dedent took if the word goes on. */
static void edp_electric(Editor *ed, u32 typed, int dedent) {
    int ls = edp_ls(ed, edp_line_of(ed, ed->cursor));
    if (ed->electric_removed > 0) {
        int k = ed->electric_removed;
        ed->electric_removed = 0;
        if (edp_is_word_cp(typed) && ed->electric_line_start == ls) {
            edp_replace(ed, ls, 0, "    ", k);
            ed->cursor = ed->anchor = ed->cursor + k;
            return;
        }
    }
    if (dedent > 0) {
        edp_replace(ed, ls, dedent, "", 0);
        ed->cursor = ed->anchor = ed->cursor - dedent;
        ed->electric_line_start = ls;
        ed->electric_removed = dedent;
    }
}

void editor_type(Editor *ed, const char *utf8, int len) {
    if (!utf8) return;
    if (len < 0) len = (int)strlen(utf8);
    int lo, hi, n;
    edp_selection(ed, &lo, &hi);
    char *norm = edp_normalize(utf8, len, edp_chars_between(ed, edp_ls(ed, edp_line_of(ed, lo)), lo), &n);
    if (n == 0) { free(norm); return; }
    edp_begin(ed, EDK_TYPE);
    int dedent = edp_electric_spaces(ed, lo, hi, norm, n);
    if (dedent) ed->merge_ok = false; /* the last letter and the dedent it causes are one undo step */
    if (edp_replace(ed, lo, hi - lo, norm, n)) {
        ed->cursor = ed->anchor = lo + n;
        if (n <= 4) edp_electric(ed, edp_cp_at(norm, ui_utf8_prev(norm, n)), dedent);
        else ed->electric_removed = 0;
    }
    free(norm);
    ed->want_col = -1;
    ed->reveal = true;
    edp_end(ed);
}

void editor_newline(Editor *ed) {
    int lo, hi;
    edp_selection(ed, &lo, &hi);
    int ls = edp_ls(ed, edp_line_of(ed, lo));
    int indent = 0;
    while (ls + indent < lo && ed->text[ls + indent] == ' ') indent++;
    bool more = edp_opens_block(ed, ed->text + ls, lo - ls);
    char ins[1 + 4096];
    int total = FS_MIN(indent + (more ? EDP_INDENT : 0), 4000);
    ins[0] = '\n';
    memset(ins + 1, ' ', (size_t)total);
    edp_begin(ed, EDK_OTHER);
    if (edp_replace(ed, lo, hi - lo, ins, 1 + total)) ed->cursor = ed->anchor = lo + 1 + total;
    ed->want_col = -1;
    ed->reveal = true;
    ed->electric_removed = 0;
    edp_end(ed);
}

void editor_backspace(Editor *ed, bool word) {
    int lo, hi;
    edp_selection(ed, &lo, &hi);
    edp_begin(ed, lo != hi ? EDK_OTHER : EDK_BACKSPACE);
    if (lo != hi) {
        if (edp_replace(ed, lo, hi - lo, "", 0)) ed->cursor = ed->anchor = lo;
    } else if (ed->cursor > 0) {
        int c = ed->cursor, from;
        if (word) {
            from = ui_word_left(ed->text, c);
        } else {
            int ls = edp_ls(ed, edp_line_of(ed, c));
            bool blank = true;
            for (int i = ls; i < c; i++) if (ed->text[i] != ' ') { blank = false; break; }
            if (blank && c > ls) { /* in the indentation: back to the previous multiple of 4 */
                int n = (c - ls) % EDP_INDENT;
                from = c - (n ? n : EDP_INDENT);
            } else {
                from = ui_utf8_prev(ed->text, c);
            }
        }
        if (edp_replace(ed, from, c - from, "", 0)) ed->cursor = ed->anchor = from;
    }
    ed->want_col = -1;
    ed->reveal = true;
    ed->electric_removed = 0;
    edp_end(ed);
}

void editor_delete_forward(Editor *ed, bool word) {
    int lo, hi;
    edp_selection(ed, &lo, &hi);
    edp_begin(ed, lo != hi ? EDK_OTHER : EDK_DELETE);
    if (lo != hi) {
        if (edp_replace(ed, lo, hi - lo, "", 0)) ed->cursor = ed->anchor = lo;
    } else if (ed->cursor < ed->len) {
        int c = ed->cursor, to = word ? ui_word_right(ed->text, c) : ui_utf8_next(ed->text, c);
        edp_replace(ed, c, to - c, "", 0);
    }
    ed->want_col = -1;
    ed->reveal = true;
    ed->electric_removed = 0;
    edp_end(ed);
}

/* ---- commands that edit whole lines (indent, dedent, comment) ------------------------------------------------------- */

typedef struct EdLineEdit {
    int pos, del, ins; /* at ORIGINAL offsets; ins is the number of spaces/characters inserted at pos */
    const char *text;  /* inserted text */
} EdLineEdit;

/* Where an offset ends up after the edits (sorted by pos, non-overlapping) are applied. */
static int edp_map_pos(int p, const EdLineEdit *e, int n) {
    int shift = 0;
    for (int i = 0; i < n; i++) {
        if (p >= e[i].pos + e[i].del) shift += e[i].ins - e[i].del;
        else {
            if (p > e[i].pos) shift += e[i].pos + e[i].ins - p; /* inside a removed span: collapse to its end */
            break;
        }
    }
    return p + shift;
}

/* Apply the edits (ascending) in one undo group and keep the selection on the same text. */
static void edp_apply_line_edits(Editor *ed, const EdLineEdit *e, int n) {
    if (n == 0) return;
    int cur = edp_map_pos(ed->cursor, e, n), anc = edp_map_pos(ed->anchor, e, n);
    edp_begin(ed, EDK_OTHER);
    for (int i = n - 1; i >= 0; i--) edp_replace(ed, e[i].pos, e[i].del, e[i].text, e[i].ins);
    ed->cursor = FS_CLAMP(cur, 0, ed->len);
    ed->anchor = FS_CLAMP(anc, 0, ed->len);
    ed->want_col = -1;
    ed->reveal = true;
    edp_end(ed);
}

/* The 0-based lines a selection touches (a selection ending at the start of a line does not include that line). */
static void edp_selected_lines(Editor *ed, int *l0, int *l1) {
    int lo, hi;
    edp_selection(ed, &lo, &hi);
    *l0 = edp_line_of(ed, lo);
    *l1 = edp_line_of(ed, hi);
    if (hi > lo && *l1 > *l0 && hi == edp_ls(ed, *l1)) (*l1)--;
}

void editor_tab(Editor *ed, bool shift) {
    int l0, l1, lo, hi;
    edp_selected_lines(ed, &l0, &l1);
    edp_selection(ed, &lo, &hi);
    if (!shift && l0 == l1) { /* insert spaces up to the next tab stop, replacing the selection */
        int col = edp_chars_between(ed, edp_ls(ed, l0), lo);
        int n = EDP_INDENT - col % EDP_INDENT;
        edp_begin(ed, EDK_OTHER);
        if (edp_replace(ed, lo, hi - lo, "    ", n)) ed->cursor = ed->anchor = lo + n;
        ed->want_col = -1;
        ed->reveal = true;
        edp_end(ed);
        return;
    }
    EdLineEdit *e = xmalloc(sizeof(EdLineEdit) * (size_t)(l1 - l0 + 1));
    int n = 0;
    for (int l = l0; l <= l1; l++) {
        int ls = edp_ls(ed, l), le = edp_le(ed, l);
        if (!shift) {
            if (ls == le) continue; /* leave empty lines alone */
            EdLineEdit x = { ls, 0, EDP_INDENT, "    " };
            e[n++] = x;
        } else {
            int k = 0;
            while (k < EDP_INDENT && ls + k < le && ed->text[ls + k] == ' ') k++;
            if (k == 0) continue;
            EdLineEdit x = { ls, k, 0, "" };
            e[n++] = x;
        }
    }
    edp_apply_line_edits(ed, e, n);
    free(e);
}

void editor_toggle_comment(Editor *ed) {
    int l0, l1;
    edp_selected_lines(ed, &l0, &l1);
    bool all = true, any = false;
    int min_indent = 1 << 30;
    for (int l = l0; l <= l1; l++) {
        int ls = edp_ls(ed, l), le = edp_le(ed, l), k = 0;
        while (ls + k < le && ed->text[ls + k] == ' ') k++;
        if (ls + k == le) continue; /* blank */
        any = true;
        if (ed->text[ls + k] != '#') all = false;
        min_indent = FS_MIN(min_indent, k);
    }
    if (!any) { all = false; min_indent = 0; }
    EdLineEdit *e = xmalloc(sizeof(EdLineEdit) * (size_t)(l1 - l0 + 1));
    int n = 0;
    for (int l = l0; l <= l1; l++) {
        int ls = edp_ls(ed, l), le = edp_le(ed, l), k = 0;
        while (ls + k < le && ed->text[ls + k] == ' ') k++;
        bool blank = ls + k == le;
        if (all) {
            if (blank) continue;
            int del = (ls + k + 1 < le && ed->text[ls + k + 1] == ' ') ? 2 : 1; /* "# " or "#" */
            EdLineEdit x = { ls + k, del, 0, "" };
            e[n++] = x;
        } else {
            if (blank && any) continue;
            EdLineEdit x = { ls + (blank ? 0 : min_indent), 0, 2, "# " };
            e[n++] = x;
        }
    }
    edp_apply_line_edits(ed, e, n);
    free(e);
}

/* ---- clipboard ------------------------------------------------------------------------------------------------------ */

/* The range copy/cut works on: the selection, or the whole caret line including its newline. */
static void edp_clip_range(Editor *ed, int *a, int *b) {
    edp_selection(ed, a, b);
    if (*a == *b) {
        int line = edp_line_of(ed, ed->cursor);
        *a = edp_ls(ed, line);
        *b = FS_MIN(edp_le(ed, line) + 1, ed->len);
    }
}

void editor_copy(Editor *ed) {
    int a, b;
    edp_clip_range(ed, &a, &b);
    if (b <= a) return;
    char *s = xstrndup(ed->text + a, (size_t)(b - a));
    platform_set_clipboard(s);
    free(s);
}

void editor_cut(Editor *ed) {
    int a, b;
    edp_clip_range(ed, &a, &b);
    if (b <= a) return;
    editor_copy(ed);
    edp_begin(ed, EDK_OTHER);
    if (edp_replace(ed, a, b - a, "", 0)) ed->cursor = ed->anchor = a;
    ed->want_col = -1;
    ed->reveal = true;
    edp_end(ed);
}

void editor_paste(Editor *ed) {
    const char *clip = platform_get_clipboard();
    if (!clip || !clip[0]) return;
    int lo, hi, n;
    edp_selection(ed, &lo, &hi);
    char *norm = edp_normalize(clip, (int)strlen(clip), edp_chars_between(ed, edp_ls(ed, edp_line_of(ed, lo)), lo), &n);
    edp_begin(ed, EDK_OTHER);
    if (n > 0 && edp_replace(ed, lo, hi - lo, norm, n)) ed->cursor = ed->anchor = lo + n;
    free(norm);
    ed->want_col = -1;
    ed->reveal = true;
    edp_end(ed);
}

/* ---- raw edits ----------------------------------------------------------------------------------------------------- */

void editor_insert(Editor *ed, int offset, const char *utf8, int len) {
    if (!utf8) return;
    if (len < 0) len = (int)strlen(utf8);
    offset = FS_CLAMP(offset, 0, ed->len);
    int cur = ed->cursor, anc = ed->anchor;
    edp_begin(ed, EDK_OTHER);
    if (edp_replace(ed, offset, 0, utf8, len)) {
        ed->cursor = cur >= offset ? cur + len : cur;
        ed->anchor = anc >= offset ? anc + len : anc;
    }
    edp_end(ed);
}

void editor_delete(Editor *ed, int offset, int len) {
    offset = FS_CLAMP(offset, 0, ed->len);
    len = FS_CLAMP(len, 0, ed->len - offset);
    int cur = ed->cursor, anc = ed->anchor;
    edp_begin(ed, EDK_OTHER);
    if (edp_replace(ed, offset, len, "", 0)) {
        ed->cursor = cur >= offset + len ? cur - len : cur > offset ? offset : cur;
        ed->anchor = anc >= offset + len ? anc - len : anc > offset ? offset : anc;
    }
    edp_end(ed);
}

/* ======================================================================================================
 * 5. the widget
 * ==================================================================================================== */

/* Syntax colors: a calm dark-theme palette. */
#define EDC_COMMENT UI_HEX(0x7b849a)
#define EDC_STRING UI_HEX(0x9ece6a)
#define EDC_NUMBER UI_HEX(0xf2a65a)
#define EDC_INTERNAL UI_HEX(0x6fd6cb)
#define EDC_KEYWORD UI_HEX(0xc792ea)
#define EDC_TYPE UI_HEX(0xe5c07b)
#define EDC_BUILTIN UI_HEX(0x82aaff)
#define EDC_OPERATOR UI_HEX(0x9aa4bb)
#define EDC_GUTTER_BG UI_HEX(0x14171c)

static u32 edp_class_color(int cls) {
    switch (cls) {
    case EDTK_COMMENT: return EDC_COMMENT;
    case EDTK_STRING: return EDC_STRING;
    case EDTK_NUMBER: return EDC_NUMBER;
    case EDTK_HEADER: return UI_COL_ACCENT;
    case EDTK_INTERNAL: return EDC_INTERNAL;
    case EDTK_KEYWORD: return EDC_KEYWORD;
    case EDTK_TYPE: return EDC_TYPE;
    case EDTK_BUILTIN: return EDC_BUILTIN;
    case EDTK_OPERATOR: return EDC_OPERATOR;
    default: return UI_COL_TEXT;
    }
}

static u32 edp_severity_color(int sev) {
    return sev == EDITOR_ERROR ? UI_COL_ERROR : sev == EDITOR_WARNING ? UI_COL_WARNING : UI_COL_ACCENT;
}

/* Where everything is, in framebuffer pixels (computed from the rectangle and the font every frame). */
typedef struct EdLayout {
    float x, y, w, h;               /* the whole widget */
    float cell_w, line_h, row_h;    /* monospace cell, glyph line height, row pitch */
    float gutter_w, sb_w, status_h; /* line numbers + markers, scrollbar, status bar */
    float tx, ty, tw, th;           /* text viewport */
    float pad;                      /* space between the gutter and the first column */
    int nlines;
} EdLayout;

static u32 edp_id(const Editor *ed) { /* identity for the UI focus system, valid outside frames too */
    u64 p = (u64)(uintptr_t)ed;
    return (((u32)(p ^ (p >> 32))) * 2654435761u) | 1u;
}

static void edp_layout(Editor *ed, float x, float y, float w, float h, EdLayout *L) {
    UiFont f = UI_FONT_MONO;
    L->x = x; L->y = y; L->w = w; L->h = h;
    L->cell_w = ui_font_advance(f, 'M');
    L->line_h = ui_font_line_height(f);
    L->row_h = FS_MAX(L->line_h, floorf(ui_font_size(f) * ui_scale() * 1.4f + 0.5f));
    L->nlines = editor_line_count(ed);
    int digits = 3, n = L->nlines;
    while (n >= 1000) { digits++; n /= 10; }
    L->gutter_w = ui_px(18) + (float)digits * L->cell_w + ui_px(10);
    L->sb_w = ui_px(10);
    L->status_h = floorf(L->row_h + ui_px(6));
    L->tx = x + L->gutter_w;
    L->ty = y;
    L->tw = FS_MAX(1.0f, w - L->gutter_w - L->sb_w);
    L->th = FS_MAX(1.0f, h - L->status_h);
    L->pad = ui_px(6);
}

static float edp_content_h(const EdLayout *L) { return ((float)L->nlines + 1.0f) * L->row_h; }

static void edp_clamp_scroll(Editor *ed, const EdLayout *L) {
    float max_y = FS_MAX(0.0f, edp_content_h(L) - L->th);
    float max_x = FS_MAX(0.0f, (float)ed->max_line_bytes * L->cell_w + L->pad + L->cell_w * 4 - L->tw);
    ed->scroll_y = FS_CLAMP(ed->scroll_y, 0.0f, max_y);
    ed->scroll_x = FS_CLAMP(ed->scroll_x, 0.0f, max_x);
}

/* x (relative to the start of the text column, before scrolling) of `offset` on the line starting at `ls`. */
static float edp_col_x(const Editor *ed, int ls, int offset) {
    return ui_text_width(UI_FONT_MONO, ed->text + ls, offset - ls);
}

/* Offset under a mouse position (clamped to the text). */
static int edp_hit(Editor *ed, const EdLayout *L, float mx, float my) {
    int line = FS_CLAMP((int)floorf((my - L->ty + ed->scroll_y) / L->row_h), 0, L->nlines - 1);
    int ls = edp_ls(ed, line), le = edp_le(ed, line);
    float x = mx - (L->tx + L->pad) + ed->scroll_x, acc = 0;
    int i = ls;
    while (i < le) {
        u32 cp = edp_cp_at(ed->text, i);
        float a = ui_font_advance(UI_FONT_MONO, cp);
        if (x < acc + a * 0.5f) return i;
        acc += a;
        i = ui_utf8_next(ed->text, i);
    }
    return le;
}

static void edp_reveal_caret(Editor *ed, const EdLayout *L) {
    int line = edp_line_of(ed, ed->cursor), ls = edp_ls(ed, line);
    float top = (float)line * L->row_h, bottom = top + L->row_h;
    if (top < ed->scroll_y) ed->scroll_y = top;
    else if (bottom > ed->scroll_y + L->th) ed->scroll_y = bottom - L->th;
    float cx = L->pad + edp_col_x(ed, ls, ed->cursor), margin = L->cell_w * 4;
    if (cx < ed->scroll_x + margin) ed->scroll_x = FS_MAX(0.0f, cx - margin);
    else if (cx > ed->scroll_x + L->tw - margin) ed->scroll_x = cx - L->tw + margin;
    ed->reveal = false;
}

/* Byte range of a diagnostic on its line; false if its line does not exist. */
static bool edp_diag_range(Editor *ed, const EdDiag *d, int *a, int *b) {
    if (d->line < 1 || d->line > editor_line_count(ed)) return false;
    int ls = edp_ls(ed, d->line - 1), le = edp_le(ed, d->line - 1);
    int s = FS_CLAMP(ls + d->col - 1, ls, le), e;
    if (d->end_col > d->col) {
        e = FS_CLAMP(ls + d->end_col - 1, s, le);
    } else { /* "the word at col": the identifier under it, else one character */
        e = s;
        if (s < le && edp_is_word_cp(edp_cp_at(ed->text, s))) {
            while (e < le && edp_is_word_cp(edp_cp_at(ed->text, e))) e = ui_utf8_next(ed->text, e);
        } else if (s < le) {
            e = ui_utf8_next(ed->text, s);
        }
    }
    if (e <= s && s > ls) s = ui_utf8_prev(ed->text, s), e = ui_utf8_next(ed->text, s); /* at end of line: underline the last char */
    *a = s;
    *b = e;
    return true;
}

/* The diagnostic shown in the status bar: the caret line's (errors first), else the first error, else the first. */
static const EdDiag *edp_status_diag(Editor *ed, bool *on_caret_line) {
    int caret_line;
    editor_offset_to_pos(ed, ed->cursor, &caret_line, NULL);
    const EdDiag *best = NULL;
    for (int i = 0; i < ed->diags.len; i++) {
        const EdDiag *d = &ed->diags.data[i];
        if (d->line == caret_line && (!best || d->severity < best->severity)) best = d;
    }
    *on_caret_line = best != NULL;
    if (best) return best;
    for (int i = 0; i < ed->diags.len; i++) {
        const EdDiag *d = &ed->diags.data[i];
        if (!best || d->severity < best->severity || (d->severity == best->severity && d->line < best->line)) best = d;
    }
    return best;
}

/* Copy `s` into out (capacity cap), shortened with "..." if it is wider than max_w pixels. */
static void edp_fit_text(UiFont f, const char *s, float max_w, char *out, int cap) {
    int n = FS_MIN((int)strlen(s), cap - 4);
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* do not cut a UTF-8 sequence */
    if (ui_text_width(f, s, n) > max_w) {
        float dots = ui_text_width(f, "...", 3);
        int i = 0;
        for (int next = ui_utf8_next(s, 0); i < n && next <= n && ui_text_width(f, s, next) + dots <= max_w; next = ui_utf8_next(s, i)) i = next;
        memcpy(out, s, (size_t)i);
        memcpy(out + i, "...", 4);
        return;
    }
    memcpy(out, s, (size_t)n);
    out[n] = 0;
}

static void edp_zoom(int delta) {
    float size = delta == 0 ? 15.0f : FS_CLAMP(ui_font_size(UI_FONT_MONO) + (float)delta, 8.0f, 40.0f);
    ui_set_font_size(UI_FONT_MONO, size);
}

/* Handle one key event; true if the editor used it. */
static bool edp_handle_key(Editor *ed, const UiKeyEvent *e) {
    int m = e->mods & 15;
    bool shift = (m & UI_MOD_SHIFT) != 0, mod = (m & UI_MOD_MOD) != 0, word = (m & UI_MOD_WORD) != 0;
    if (e->text) {
        char u[4];
        editor_type(ed, u, utf8_encode(e->text, u));
        return true;
    }
    switch (e->key) {
    case UI_KEY_LEFT:
    case UI_KEY_RIGHT: {
        bool left = e->key == UI_KEY_LEFT;
        EditorMove mv = left ? EDITOR_MOVE_LEFT : EDITOR_MOVE_RIGHT;
#ifdef FS_OS_MACOS
        if (mod) mv = left ? EDITOR_MOVE_LINE_START : EDITOR_MOVE_LINE_END;
        else
#endif
        if (word) mv = left ? EDITOR_MOVE_WORD_LEFT : EDITOR_MOVE_WORD_RIGHT;
        editor_move(ed, mv, shift);
        return true;
    }
    case UI_KEY_UP:
    case UI_KEY_DOWN: {
        bool up = e->key == UI_KEY_UP;
#ifdef FS_OS_MACOS
        editor_move(ed, mod ? (up ? EDITOR_MOVE_DOC_START : EDITOR_MOVE_DOC_END) : (up ? EDITOR_MOVE_UP : EDITOR_MOVE_DOWN), shift);
#else
        editor_move(ed, up ? EDITOR_MOVE_UP : EDITOR_MOVE_DOWN, shift);
#endif
        return true;
    }
    case UI_KEY_HOME: editor_move(ed, mod ? EDITOR_MOVE_DOC_START : EDITOR_MOVE_LINE_START, shift); return true;
    case UI_KEY_END: editor_move(ed, mod ? EDITOR_MOVE_DOC_END : EDITOR_MOVE_LINE_END, shift); return true;
    case UI_KEY_PAGE_UP: editor_move(ed, EDITOR_MOVE_PAGE_UP, shift); return true;
    case UI_KEY_PAGE_DOWN: editor_move(ed, EDITOR_MOVE_PAGE_DOWN, shift); return true;
    case UI_KEY_BACKSPACE: editor_backspace(ed, word); return true;
    case UI_KEY_DELETE: editor_delete_forward(ed, word); return true;
    case UI_KEY_ENTER:
        if (mod) return false; /* Mod+Enter is the application's (run) */
        editor_newline(ed);
        return true;
    case UI_KEY_TAB:
        if (mod || (m & UI_MOD_ALT)) return false;
        editor_tab(ed, shift);
        return true;
    case UI_KEY_A: if (!mod) return false; editor_select_all(ed); return true;
    case UI_KEY_C: if (!mod) return false; editor_copy(ed); return true;
    case UI_KEY_X: if (!mod) return false; editor_cut(ed); return true;
    case UI_KEY_V: if (!mod) return false; editor_paste(ed); return true;
    case UI_KEY_Z: if (!mod) return false; if (shift) editor_redo(ed); else editor_undo(ed); return true;
    case UI_KEY_Y: if (!mod || shift) return false; editor_redo(ed); return true;
    case UI_KEY_SLASH: if (!mod) return false; editor_toggle_comment(ed); return true;
    case UI_KEY_EQUAL: if (!mod) return false; edp_zoom(1); return true;
    case UI_KEY_MINUS: if (!mod) return false; edp_zoom(-1); return true;
    case UI_KEY_0: if (!mod) return false; edp_zoom(0); return true;
    default: return false;
    }
}

bool editor_has_focus(const Editor *ed) { return ui_focus_id() == edp_id(ed); }

/* The text of one visible row: highlighted runs. */
static void edp_draw_row_text(Editor *ed, const EdLayout *L, int ls, int le, float x, float ytop) {
    EdTok toks[256];
    int nt = edp_tokenize(ed, ed->text + ls, le - ls, toks, 256);
    int pos = 0;
    float px = x;
    for (int i = 0; i <= nt; i++) {
        int ts = i < nt ? toks[i].start : le - ls, te = i < nt ? toks[i].end : le - ls;
        if (ts > pos) { /* plain text before the token */
            ui_text(UI_FONT_MONO, px, ytop, ed->text + ls + pos, ts - pos, UI_COL_TEXT);
            px += ui_text_width(UI_FONT_MONO, ed->text + ls + pos, ts - pos);
        }
        if (i == nt) break;
        ui_text(UI_FONT_MONO, px, ytop, ed->text + ls + ts, te - ts, edp_class_color(toks[i].cls));
        px += ui_text_width(UI_FONT_MONO, ed->text + ls + ts, te - ts);
        pos = te;
    }
    (void)L;
}

static void edp_draw_underline(const EdLayout *L, float x0, float x1, float row_y, int severity) {
    u32 col = edp_severity_color(severity);
    float base = row_y + floorf((L->row_h - L->line_h) * 0.5f) + ui_font_ascent(UI_FONT_MONO) + ui_px(2.0f);
    if (x1 - x0 < 2) x1 = x0 + L->cell_w;
    if (severity == EDITOR_INFO) {
        ui_rect(x0, base, x1 - x0, FS_MAX(1.0f, floorf(ui_scale())), col);
        return;
    }
    float step = ui_px(2.0f), amp = ui_px(1.5f), pts[2 * 512];
    int n = 0;
    for (float x = x0; x < x1 && n < 511; x += step, n++) {
        pts[2 * n] = x;
        pts[2 * n + 1] = base + ((n & 1) ? amp : 0.0f);
    }
    pts[2 * n] = x1;
    pts[2 * n + 1] = base + ((n & 1) ? amp : 0.0f);
    ui_polyline(pts, n + 1, false, FS_MAX(1.0f, ui_scale()), col);
}

bool editor_ui(Editor *ed, float x, float y, float w, float h, bool grab_focus) {
    const UiInput *in = ui_input();
    u32 version0 = ed->version;
    EdLayout L;
    edp_layout(ed, x, y, w, h, &L);
    ed->clock = in->time;
    ed->page_rows = FS_MAX(1, (int)(L.th / L.row_h) - 1);

    u32 id = edp_id(ed);
    if (grab_focus) ui_set_focus(id);
    bool focused = ui_has_focus(id);

    /* ---- mouse: text area, gutter, scrollbar, status bar ---- */
    ui_occlude(x, y, w, h);
    UiInteract rt = ui_interact_id(id + 1, L.tx, L.ty, L.tw, L.th);
    UiInteract rg = ui_interact_id(id + 2, x, y, L.gutter_w, L.th);
    UiInteract rs = ui_interact_id(id + 3, L.tx + L.tw, L.ty, L.sb_w, L.th);
    UiInteract rb = ui_interact_id(id + 4, x, y + L.th, w, L.status_h);
    if (rt.hovered) ui_set_cursor(UI_CURSOR_IBEAM);

    if (rt.pressed || rg.pressed || rs.pressed || rb.pressed) { /* a click anywhere on the widget gives it the keyboard */
        ui_set_focus(id);
        focused = true;
    }
    if (focused && !ed->had_focus) ed->caret_epoch = in->time; /* a fresh focus shows the caret right away */
    ed->had_focus = focused;
    if (rt.pressed || rg.pressed) {
        int off = edp_hit(ed, &L, in->mouse_x, in->mouse_y);
        int line = edp_line_of(ed, off);
        if (rg.pressed || in->click_count >= 3) { /* gutter click or triple click: the whole line */
            ed->anchor = edp_ls(ed, line);
            ed->cursor = FS_MIN(edp_le(ed, line) + 1, ed->len);
            ed->drag_mode = 3;
            ed->drag_a0 = ed->anchor;
            ed->drag_a1 = ed->cursor;
        } else if (in->click_count == 2) {
            int a, b;
            ui_word_at(ed->text, off, &a, &b);
            ed->anchor = a;
            ed->cursor = b;
            ed->drag_mode = 2;
            ed->drag_a0 = a;
            ed->drag_a1 = b;
        } else {
            ed->cursor = off;
            if (!(in->mods & UI_MOD_SHIFT)) ed->anchor = off;
            ed->drag_mode = 1;
        }
        ed->want_col = -1;
        ed->caret_epoch = in->time;
        edp_interrupt(ed);
    } else if ((rt.held || rg.held) && ed->drag_mode) {
        int off = edp_hit(ed, &L, in->mouse_x, in->mouse_y);
        int line = edp_line_of(ed, off);
        if (ed->drag_mode == 1) {
            ed->cursor = off;
        } else if (ed->drag_mode == 2) {
            int a, b;
            ui_word_at(ed->text, off, &a, &b);
            if (off < ed->drag_a0) { ed->cursor = a; ed->anchor = ed->drag_a1; }
            else { ed->cursor = b; ed->anchor = ed->drag_a0; }
        } else {
            int ls = edp_ls(ed, line), le = FS_MIN(edp_le(ed, line) + 1, ed->len);
            if (ls < ed->drag_a0) { ed->cursor = ls; ed->anchor = ed->drag_a1; }
            else { ed->cursor = le; ed->anchor = ed->drag_a0; }
        }
        /* dragging past an edge scrolls; keep frames coming while the mouse is held still outside */
        float dy = 0, dx = 0;
        if (in->mouse_y < L.ty) dy = in->mouse_y - L.ty;
        else if (in->mouse_y > L.ty + L.th) dy = in->mouse_y - (L.ty + L.th);
        if (in->mouse_x < L.tx) dx = in->mouse_x - L.tx;
        else if (in->mouse_x > L.tx + L.tw) dx = in->mouse_x - (L.tx + L.tw);
        if (dx != 0 || dy != 0) {
            ed->scroll_y += dy * 0.3f;
            ed->scroll_x += dx * 0.3f;
            ui_request_redraw();
        }
        ed->caret_epoch = in->time;
    }
    if (!rt.held && !rg.held) ed->drag_mode = 0;

    /* wheel: vertical, or horizontal with Shift / a horizontal wheel */
    if (ui_mouse_over(x, y, w, L.th) && (in->scroll_y != 0 || in->scroll_x != 0)) {
        float sy = in->scroll_y, sx = in->scroll_x;
        if (in->mods & UI_MOD_SHIFT) { sx += sy; sy = 0; }
        ed->scroll_y -= sy * L.row_h * 3.0f;
        ed->scroll_x -= sx * L.cell_w * 6.0f;
    }

    /* scrollbar */
    float content_h = edp_content_h(&L), max_scroll = FS_MAX(0.0f, content_h - L.th);
    float thumb_h = FS_CLAMP(L.th * L.th / content_h, ui_px(24), L.th), travel = FS_MAX(1.0f, L.th - thumb_h);
    float thumb_y = L.ty + (max_scroll > 0 ? ed->scroll_y / max_scroll * travel : 0.0f);
    if (rs.pressed) {
        bool on_thumb = in->mouse_y >= thumb_y && in->mouse_y < thumb_y + thumb_h;
        ed->sb_grab = on_thumb ? in->mouse_y - thumb_y : thumb_h * 0.5f;
    }
    if (rs.held && max_scroll > 0) ed->scroll_y = (in->mouse_y - ed->sb_grab - L.ty) / travel * max_scroll;

    /* ---- keyboard ---- */
    if (focused) {
        int n;
        const UiKeyEvent *ev = ui_key_events(&n);
        for (int i = 0; i < n; i++) {
            if (ui_key_consumed(i)) continue;
            if (edp_handle_key(ed, &ev[i])) {
                ui_key_consume(i);
                ed->caret_epoch = in->time;
            }
        }
        edp_layout(ed, x, y, w, h, &L); /* the font size or line count may have changed */
    }
    if (ed->reveal) edp_reveal_caret(ed, &L);
    edp_clamp_scroll(ed, &L);

    /* ---- drawing ---- */
    int caret_line = edp_line_of(ed, ed->cursor);
    int sel_lo, sel_hi;
    edp_selection(ed, &sel_lo, &sel_hi);
    ui_rect(x, y, w, h, UI_COL_PANEL);
    ui_rect(x, y, L.gutter_w, L.th, EDC_GUTTER_BG);
    ui_rect(x + L.gutter_w - 1, y, 1, L.th, UI_COL_BORDER);

    int first = FS_MAX(0, (int)floorf(ed->scroll_y / L.row_h));
    int last = FS_MIN(L.nlines - 1, (int)floorf((ed->scroll_y + L.th) / L.row_h));
    float text_dy = floorf((L.row_h - L.line_h) * 0.5f), caret_w = ceilf(ui_scale() * 1.5f);
    float marker_cx = x + ui_px(9);
    ui_push_clip(x, y, w, L.th);
    for (int line = first; line <= last; line++) {
        float ry = L.ty + (float)line * L.row_h - ed->scroll_y;
        int ls = edp_ls(ed, line), le = edp_le(ed, line);
        int worst = -1;
        for (int i = 0; i < ed->diags.len; i++)
            if (ed->diags.data[i].line == line + 1 && (worst < 0 || ed->diags.data[i].severity < worst)) worst = ed->diags.data[i].severity;

        /* row backgrounds */
        ui_push_clip(L.tx, L.ty, L.tw, L.th);
        EdTok head[1];
        bool is_header = edp_tokenize(ed, ed->text + ls, le - ls, head, 1) > 0 && head[0].cls == EDTK_HEADER;
        if (is_header) ui_rect(L.tx, ry, L.tw, L.row_h, ui_alpha(UI_COL_ACCENT, 0.10f));
        if (worst == EDITOR_ERROR) ui_rect(L.tx, ry, L.tw, L.row_h, ui_alpha(UI_COL_ERROR, 0.07f));
        if (line == caret_line && sel_lo == sel_hi) ui_rect(L.tx, ry, L.tw, L.row_h, ui_alpha(0xFFFFFFFFu, 0.045f));
        float x0 = L.tx + L.pad - ed->scroll_x;
        if (sel_hi > sel_lo && sel_lo <= le && sel_hi > ls) { /* selection */
            int a = FS_MAX(sel_lo, ls), b = FS_MIN(sel_hi, le);
            float sx0 = x0 + edp_col_x(ed, ls, a), sx1 = x0 + edp_col_x(ed, ls, b);
            if (sel_hi > le) sx1 += L.cell_w * 0.6f; /* the selected line break */
            ui_rect(sx0, ry, sx1 - sx0, L.row_h, UI_COL_SELECTION);
        }
        edp_draw_row_text(ed, &L, ls, le, x0, ry + text_dy);
        for (int i = 0; i < ed->diags.len; i++) { /* underlines */
            const EdDiag *d = &ed->diags.data[i];
            int a, b;
            if (d->line != line + 1 || !edp_diag_range(ed, d, &a, &b)) continue;
            edp_draw_underline(&L, x0 + edp_col_x(ed, ls, a), x0 + edp_col_x(ed, ls, b), ry, d->severity);
        }
        if (focused && line == caret_line) { /* caret */
            double ph = fmod(in->time - ed->caret_epoch, 2 * EDP_BLINK);
            if (ph < 0) ph += 2 * EDP_BLINK;
            if (ph < EDP_BLINK) ui_rect(x0 + edp_col_x(ed, ls, ed->cursor), ry + text_dy, caret_w, L.line_h, UI_COL_TEXT);
            ui_wake_in((ph < EDP_BLINK ? EDP_BLINK : 2 * EDP_BLINK) - ph);
        }
        ui_pop_clip();

        /* gutter: marker and line number */
        if (worst >= 0) ui_disk(marker_cx, ry + L.row_h * 0.5f, ui_px(3.5f), edp_severity_color(worst));
        char num[16];
        int nl = snprintf(num, sizeof num, "%d", line + 1);
        float nw = ui_text_width(UI_FONT_MONO, num, nl);
        ui_text(UI_FONT_MONO, x + L.gutter_w - ui_px(8) - nw, ry + text_dy, num, nl, line == caret_line ? UI_COL_TEXT_DIM : UI_COL_TEXT_OFF);
    }
    ui_pop_clip();

    /* scrollbar thumb */
    if (max_scroll > 0) {
        bool lit = rs.hovered || rs.held;
        ui_rrect(L.tx + L.tw + ui_px(2), thumb_y + 1, L.sb_w - ui_px(4), thumb_h - 2, ui_px(3),
                 ui_alpha(UI_COL_TEXT_DIM, lit ? 0.55f : 0.28f));
    }

    /* ---- status bar ---- */
    float sy = y + L.th;
    ui_rect(x, sy, w, L.status_h, EDC_GUTTER_BG);
    ui_rect(x, sy, w, 1, UI_COL_BORDER);
    int caret_col_chars = edp_chars_between(ed, edp_ls(ed, caret_line), ed->cursor) + 1;
    char pos[64];
    int pn = snprintf(pos, sizeof pos, "Ln %d, Col %d", caret_line + 1, caret_col_chars);
    if (sel_hi > sel_lo) pn += snprintf(pos + pn, sizeof pos - (size_t)pn, "  (%d selected)", edp_chars_between(ed, sel_lo, sel_hi));
    float pw = ui_text_width(UI_FONT_SANS, pos, pn);
    float status_pad = ui_px(10);
    ui_label(x + w - status_pad - pw - 2, sy + 1, pw + 4, L.status_h - 1, pos, UI_ALIGN_RIGHT, UI_COL_TEXT_DIM);
    bool on_line = false;
    const EdDiag *sd = edp_status_diag(ed, &on_line);
    if (sd) { /* the message; clicking it jumps to the problem */
        char msg[512];
        if (on_line) snprintf(msg, sizeof msg, "%s", sd->message);
        else snprintf(msg, sizeof msg, "Line %d: %s", sd->line, sd->message);
        float dot = ui_px(4), msg_x = x + status_pad + 2 * dot + ui_px(8);
        float msg_w = (x + w - status_pad - pw - ui_px(16)) - msg_x;
        bool over_msg = rb.hovered && in->mouse_x < msg_x + msg_w;
        ui_disk(x + status_pad + dot, sy + L.status_h * 0.5f + 0.5f, dot, edp_severity_color(sd->severity));
        if (msg_w > L.cell_w * 3) {
            char fit[512];
            edp_fit_text(UI_FONT_SANS, msg, msg_w, fit, sizeof fit);
            ui_label(msg_x, sy + 1, msg_w, L.status_h - 1, fit, UI_ALIGN_LEFT, over_msg ? UI_COL_TEXT : edp_severity_color(sd->severity));
        }
        if (over_msg) ui_set_cursor(UI_CURSOR_HAND);
        if (rb.clicked && over_msg) editor_goto(ed, sd->line, sd->col);
    }
    ui_rect_outline(x, y, w, h, 1, focused ? UI_COL_ACCENT : UI_COL_BORDER);

    /* ---- hovering a diagnostic (the gutter marker or the underlined range) for a moment shows its message ---- */
    if (fabsf(in->mouse_x - ed->hover_x) > 2 || fabsf(in->mouse_y - ed->hover_y) > 2) {
        ed->hover_x = in->mouse_x;
        ed->hover_y = in->mouse_y;
        ed->hover_since = in->time;
    }
    if (ed->drag_mode == 0 && ed->diags.len > 0 && ui_mouse_over(x, y, w, L.th)) {
        int line = FS_CLAMP((int)floorf((in->mouse_y - L.ty + ed->scroll_y) / L.row_h), 0, L.nlines - 1);
        bool in_gutter = in->mouse_x < L.tx;
        float x0 = L.tx + L.pad - ed->scroll_x;
        for (int i = 0; i < ed->diags.len; i++) {
            const EdDiag *d = &ed->diags.data[i];
            int a, b;
            if (d->line != line + 1 || !edp_diag_range(ed, d, &a, &b)) continue;
            int ls = edp_ls(ed, line);
            bool hit = in_gutter ? in->mouse_x < x + ui_px(18)
                                 : (in->mouse_x >= x0 + edp_col_x(ed, ls, a) - 2 && in->mouse_x < x0 + edp_col_x(ed, ls, b) + 2);
            if (!hit || !d->message[0]) continue;
            double waited = in->time - ed->hover_since;
            if (waited >= EDP_TIP_DELAY) ui_tooltip_now(d->message);
            else ui_wake_in(EDP_TIP_DELAY - waited);
            break;
        }
    }
    return ed->version != version0;
}
