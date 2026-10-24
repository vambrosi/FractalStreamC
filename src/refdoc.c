/* refdoc.c - the language reference (docs/language.md) shown inside the app.
 *
 * The Markdown is read from the docs folder next to the program (or in the app bundle's Resources) the
 * first time the reference opens, and drawn with a small subset of Markdown: headings, paragraphs, bullet
 * lists, code blocks, tables, and `code` / **bold** inside text.  A search field finds text and scrolls
 * to it (Enter: the next place). */

typedef enum { RD_H1, RD_H2, RD_H3, RD_PARA, RD_BULLET, RD_CODE, RD_TABLE } RdKind;

typedef struct RdBlock {
    RdKind kind;
    char *text;      /* heading / paragraph / bullet text (with its `code` and **bold** marks); code: the lines */
    int indent;      /* bullets: nesting level */
    char **cells;    /* tables: rows x cols, row-major */
    int rows, cols;
    float y, h;      /* where it was laid out last (for the search) */
} RdBlock;

typedef VEC(RdBlock) RdBlockVec;
typedef struct RdRect { float x, y, w, h; } RdRect;

static struct {
    bool loaded;
    char path[1100];
    RdBlockVec blocks;
    float scroll, content_h;
    char query[128];
    int found;       /* the block found last, -1 none */
    bool search_now; /* look for the query at the next draw (refdoc_lookup) */
} rd = {.found = -1};

/* open the reference at `word` (the word under the script's caret) */
static void refdoc_lookup(const char *word) {
    snprintf(rd.query, sizeof rd.query, "%s", word);
    rd.search_now = true;
}

/* the documentation that comes with the program: next to it, above it (build/), or in the app's Resources */
static const char *rd_docs_dir(void) {
    static char dir[1100];
    static bool done;
    if (done) return dir;
    done = true;
    const char *exe = fs_exe_dir();
    static const char *rel[] = {"/docs", "/../docs", "/../Resources/docs", "/../../docs"};
    for (int k = 0; exe[0] && k < ARRAY_COUNT(rel); k++) {
        char p[1100];
        snprintf(p, sizeof p, "%.1000s%s", exe, rel[k]);
        if (fs_is_dir(p) && fs_absolute(p, dir, sizeof dir)) return dir;
    }
    dir[0] = 0;
    return dir;
}

static char *rd_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* Parse Markdown text into blocks (the subset written in docs/language.md). */
static void rd_parse(const char *md, RdBlockVec *out) {
    char *copy = xstrdup(md);
    bool code = false;
    RdBlock *open = NULL; /* the paragraph or bullet that continuation lines join */
    StrBuf codebuf = {0};
    VEC(char *) cells = {0};
    int trow = 0, tcols = 0;
    char *line = copy;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *t = rd_trim(line);
        if (!strncmp(t, "```", 3)) {
            if (code) { /* the end of a code block */
                RdBlock b = {RD_CODE, xstrdup(sb_cstr(&codebuf)), 0, NULL, 0, 0, 0, 0};
                vec_push(*out, b);
                sb_clear(&codebuf);
            }
            code = !code;
            open = NULL;
        } else if (code) {
            sb_printf(&codebuf, "%s%s", codebuf.len ? "\n" : "", line);
        } else if (t[0] == '|') { /* a table row; "|---|" rows are skipped */
            open = NULL;
            if (strspn(t, "|-: ") == strlen(t)) goto next;
            int n = 0;
            char *c = t + 1;
            while (*c) {
                char *bar = strchr(c, '|');
                if (!bar) break;
                *bar = 0;
                vec_push(cells, xstrdup(rd_trim(c)));
                n++;
                c = bar + 1;
            }
            if (trow == 0) tcols = n;
            while (n < tcols) { vec_push(cells, xstrdup("")); n++; }
            while (n > tcols) { free(vec_last(cells)); cells.len--; n--; }
            trow++;
            bool last = !nl || rd_trim(nl + 1)[0] != '|';
            if (last) {
                RdBlock b = {RD_TABLE, NULL, 0, cells.data, trow, tcols, 0, 0};
                vec_push(*out, b);
                cells.data = NULL;
                cells.len = cells.cap = 0;
                trow = tcols = 0;
            }
        } else if (t[0] == '#') {
            int level = (int)strspn(t, "#");
            RdBlock b = {level == 1 ? RD_H1 : level == 2 ? RD_H2 : RD_H3, xstrdup(rd_trim(t + level)), 0, NULL, 0, 0, 0, 0};
            vec_push(*out, b);
            open = NULL;
        } else if (!t[0]) {
            open = NULL;
        } else if ((t[0] == '-' || t[0] == '*') && t[1] == ' ') {
            int indent = (int)(t - line) / 2;
            RdBlock b = {RD_BULLET, xstrdup(rd_trim(t + 2)), indent, NULL, 0, 0, 0, 0};
            vec_push(*out, b);
            open = &vec_last(*out);
        } else if (open) { /* continuation of a paragraph or a bullet */
            size_t a = strlen(open->text), l = strlen(t);
            open->text = (char *)xrealloc(open->text, a + l + 2);
            open->text[a] = ' ';
            memcpy(open->text + a + 1, t, l + 1);
        } else {
            RdBlock b = {RD_PARA, xstrdup(t), 0, NULL, 0, 0, 0, 0};
            vec_push(*out, b);
            open = &vec_last(*out);
        }
    next:
        line = nl ? nl + 1 : NULL;
    }
    if (code && codebuf.len) { /* (an unclosed block) */
        RdBlock b = {RD_CODE, xstrdup(sb_cstr(&codebuf)), 0, NULL, 0, 0, 0, 0};
        vec_push(*out, b);
    }
    sb_free(&codebuf);
    for (int i = 0; i < cells.len; i++) free(cells.data[i]);
    vec_free(cells);
    free(copy);
}

static void rd_free_blocks(RdBlockVec *v) {
    for (int i = 0; i < v->len; i++) {
        RdBlock *b = &v->data[i];
        free(b->text);
        for (int k = 0; k < b->rows * b->cols; k++) free(b->cells[k]);
        free(b->cells);
    }
    vec_free(*v);
}

static bool rd_load(void) {
    if (rd.loaded) return rd.blocks.len > 0;
    rd.loaded = true;
    const char *dir = rd_docs_dir();
    if (!dir[0]) return false;
    snprintf(rd.path, sizeof rd.path, "%.1000s/language.md", dir);
    size_t len;
    char *md = fs_read_file(rd.path, &len);
    if (!md) return false;
    rd_parse(md, &rd.blocks);
    free(md);
    /* and how the built-in functions are defined (the prelude, without the palettes' data) */
    StrBuf sb = {0};
    sb_puts(&sb, "## Definitions of the built-in functions\n\n"
                 "Most built-in functions are written in the language itself, and this is their source "
                 "(also printed by `fractalstreamc --builtins`).\n\n```\n");
    bool skipped = false;
    for (const char *l = prelude_source; *l;) {
        const char *e = strchr(l, '\n');
        int n = e ? (int)(e - l) : (int)strlen(l);
        if (!strncmp(l, "define __palette_", 17)) {
            if (!skipped) sb_puts(&sb, "# (the palettes of palette(\"name\", t): __wheel with other colors)\n");
            skipped = true;
        } else {
            sb_printf(&sb, "%.*s\n", n, l);
        }
        l += n + (e ? 1 : 0);
    }
    sb_puts(&sb, "```\n");
    rd_parse(sb_cstr(&sb), &rd.blocks);
    sb_free(&sb);
    return rd.blocks.len > 0;
}

/* ---- text with `code` and **bold**, wrapped ------------------------------------------------------------------ */
#define RD_CODE_COL UI_HEX(0xe8c07d)
#define RD_BOLD_COL UI_HEX(0xffffff)

/* Lay out (and draw if `draw`) text with inline marks in the width w; returns the height. */
static float rd_rich(float x, float y, float w, const char *s, u32 col, bool draw) {
    float lh = FS_MAX(ui_font_line_height(UI_FONT_SANS), ui_font_line_height(UI_FONT_MONO));
    float cx = 0, cy = 0, space = ui_text_width(UI_FONT_SANS, " ", 1);
    bool code = false, bold = false;
    const char *p = s;
    while (*p) {
        if (*p == '`') { code = !code; p++; continue; }
        if (!code && p[0] == '*' && p[1] == '*') { bold = !bold; p += 2; continue; }
        if (*p == ' ') { /* (a space: ends a word) */
            if (cx > 0) cx += space;
            p++;
            continue;
        }
        /* a word: up to a space or a change of style */
        const char *e = p;
        while (*e && *e != ' ' && *e != '`' && !(!code && e[0] == '*' && e[1] == '*')) e++;
        if (code) /* (inside code, spaces belong to the code) */
            while (*e && *e != '`') {
                if (*e == ' ' && e > p) break;
                e++;
            }
        UiFont f = code ? UI_FONT_MONO : UI_FONT_SANS;
        float ww = ui_text_width(f, p, (int)(e - p));
        if (cx > 0 && cx + ww > w) { /* wrap */
            cx = 0;
            cy += lh;
        }
        if (draw) {
            float dy = (lh - ui_font_line_height(f)) * 0.5f;
            ui_text(f, x + cx, y + cy + dy, p, (int)(e - p), code ? RD_CODE_COL : bold ? RD_BOLD_COL : col);
        }
        cx += ww;
        p = e;
    }
    return cy + lh;
}

static bool rd_contains(const char *hay, const char *needle) { /* (ignoring case, and the marks of `code` and **bold**) */
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++) {
        const char *h = hay;
        size_t k = 0;
        while (k < n && *h) {
            if (*h == '`' && needle[k] != '`') { h++; continue; }
            if (h[0] == '*' && h[1] == '*' && needle[k] != '*') { h += 2; continue; }
            if (tolower((unsigned char)*h) != tolower((unsigned char)needle[k])) break;
            h++;
            k++;
        }
        if (k == n) return true;
    }
    return false;
}

/* does a block have the text searched for? */
static bool rd_block_has(RdBlock *b, const char *q) {
    if (b->text && rd_contains(b->text, q)) return true;
    for (int k = 0; k < b->rows * b->cols; k++)
        if (rd_contains(b->cells[k], q)) return true;
    return false;
}

/* one block at (x, y) in the width w; returns its height (drawn only when `draw`) */
static float rd_block(RdBlock *b, float x, float y, float w, bool draw) {
    float lh = ui_font_line_height(UI_FONT_SANS);
    switch (b->kind) {
    case RD_H1: case RD_H2: case RD_H3: {
        float top = b->kind == RD_H1 ? ui_px(6) : ui_px(14);
        float h = rd_rich(x, y + top, w, b->text, UI_COL_ACCENT, draw);
        if (draw && b->kind != RD_H3) ui_rect(x, y + top + h + ui_px(3), w, ui_px(1), b->kind == RD_H1 ? UI_COL_ACCENT_DIM : UI_COL_BORDER);
        return top + h + ui_px(10);
    }
    case RD_PARA: return rd_rich(x, y, w, b->text, UI_COL_TEXT, draw) + ui_px(8);
    case RD_BULLET: {
        float ix = ui_px(14) + b->indent * ui_px(18);
        if (draw) ui_disk(x + ix - ui_px(8), y + lh * 0.5f, ui_px(2.5f), UI_COL_TEXT_DIM);
        return rd_rich(x + ix, y, w - ix, b->text, UI_COL_TEXT, draw) + ui_px(4);
    }
    case RD_CODE: {
        float ml = ui_font_line_height(UI_FONT_MONO);
        int lines = 1;
        for (const char *c = b->text; *c; c++) lines += *c == '\n';
        float h = lines * ml + ui_px(16);
        if (draw) {
            ui_rrect(x, y, w, h, ui_px(6), UI_RGBA(12, 14, 18, 255));
            ui_push_clip(x, y, w, h);
            const char *s = b->text;
            for (int i = 0; i < lines; i++) {
                const char *e = strchr(s, '\n');
                int n = e ? (int)(e - s) : (int)strlen(s);
                const char *hash = memchr(s, '#', (size_t)n); /* comments dimmer */
                int code_n = hash ? (int)(hash - s) : n;
                ui_text(UI_FONT_MONO, x + ui_px(10), y + ui_px(8) + i * ml, s, code_n, UI_COL_TEXT);
                if (hash) ui_text(UI_FONT_MONO, x + ui_px(10) + ui_text_width(UI_FONT_MONO, s, code_n), y + ui_px(8) + i * ml, hash,
                                  n - code_n, UI_COL_TEXT_DIM);
                s = e ? e + 1 : s + n;
            }
            ui_pop_clip();
        }
        return h + ui_px(10);
    }
    case RD_TABLE: {
        /* columns: as wide as their widest cell when it all fits, else shared out */
        float natural[16] = {0}, cw[16];
        int nc = FS_MIN(b->cols, 16);
        float gap = ui_px(16), total = 0;
        for (int c = 0; c < nc; c++) {
            for (int r = 0; r < b->rows; r++) {
                const char *t = b->cells[r * b->cols + c];
                natural[c] = FS_MAX(natural[c], ui_text_width(UI_FONT_SANS, t, -1) + ui_px(4));
            }
            total += natural[c];
        }
        float avail = w - gap * (nc - 1);
        if (total <= avail) {
            for (int c = 0; c < nc; c++) cw[c] = natural[c];
        } else { /* narrow columns keep their width, the others share the rest */
            float fair = avail / nc, rest = avail;
            int wide = 0;
            for (int c = 0; c < nc; c++) {
                if (natural[c] <= fair) rest -= natural[c];
                else wide++;
            }
            for (int c = 0; c < nc; c++) cw[c] = natural[c] <= fair ? natural[c] : rest / FS_MAX(wide, 1);
        }
        float yy = y;
        for (int r = 0; r < b->rows; r++) {
            float rh = 0, xx = x;
            for (int c = 0; c < nc; c++) {
                const char *t = b->cells[r * b->cols + c];
                float h = rd_rich(xx, yy, cw[c], t, r == 0 ? UI_COL_TEXT_DIM : UI_COL_TEXT, draw);
                rh = FS_MAX(rh, h);
                xx += cw[c] + gap;
            }
            if (draw && r < b->rows - 1) ui_rect(x, yy + rh + ui_px(3), w, ui_px(1), UI_RGBA(255, 255, 255, 18));
            yy += rh + ui_px(7);
        }
        return yy - y + ui_px(8);
    }
    }
    return 0;
}

/* the reference: a panel over everything; Esc or the close button closes it */
static void refdoc_ui(float W, float H, bool *open) {
    if (!*open) return;
    int prev = ui_layer(UI_LAYER_POPUP);
    ui_panel_ex(0, 0, W, H, UI_RGBA(8, 10, 14, 150), 0);
    float pw = FS_MIN(ui_px(900), W - ui_px(40)), ph = H - ui_px(40);
    RdRect pr = {(W - pw) / 2, ui_px(20), pw, ph};
    ui_panel_ex(pr.x, pr.y, pr.w, pr.h, UI_RGBA(24, 27, 33, 250), ui_px(10));
    float pad = ui_px(18), bar = ui_px(48);
    ui_text(UI_FONT_SANS, pr.x + pad, pr.y + ui_px(14), "The FractalStreamC language", -1, UI_COL_TEXT);
    float fw = FS_MIN(ui_px(260), pr.w * 0.4f);
    int r = ui_text_field_ex("##ref_find", pr.x + pr.w - fw - ui_px(52), pr.y + ui_px(9), fw, ui_px(30), rd.query, (int)sizeof rd.query,
                             UI_TF_KEEP_FOCUS, "find in the reference");
    if (ui_icon_button_ex("##ref_close", "close", "x", pr.x + pr.w - ui_px(42), pr.y + ui_px(9), ui_px(30), UI_FLAT))
        *open = false; /* (Esc: see the app's shortcuts) */
    ui_tooltip("Close (Esc, F2)");
    RdRect body = {pr.x + 1, pr.y + bar, pr.w - 2, pr.h - bar - 1};
    ui_rect(body.x, body.y - 1, body.w, ui_px(1), UI_COL_BORDER);
    if (!rd_load()) {
        char msg[1300];
        snprintf(msg, sizeof msg, "The reference (docs/language.md) is not where the program expects it%s%s.",
                 rd.path[0] ? ": " : "", rd.path);
        ui_text_wrapped(UI_FONT_SANS, body.x + pad, body.y + pad, body.w - 2 * pad, msg, UI_COL_TEXT_DIM, true);
        ui_layer(prev);
        return;
    }
    float tw = body.w - 2 * pad;
    /* search: typing finds the first place, Enter the next one (where the blocks were laid out last) */
    bool step = (r & UI_TEXT_SUBMIT) != 0, changed = (r & UI_TEXT_CHANGED) != 0, lookup = false;
    if (rd.search_now && rd.content_h > 0) {
        changed = lookup = true;
        rd.search_now = false;
    }
    if ((step || changed) && rd.query[0]) {
        int start = changed ? 0 : rd.found + 1, n = rd.blocks.len, hit = -1;
        for (int k = 0; k < n && lookup && hit < 0; k++) /* (a word looked up: where the text explains it first) */
            if (rd.blocks.data[k].kind != RD_CODE && rd_block_has(&rd.blocks.data[k], rd.query)) hit = k;
        for (int k = 0; k < n && hit < 0; k++) {
            int i = (start + k) % n;
            if (rd_block_has(&rd.blocks.data[i], rd.query)) hit = i;
        }
        rd.found = hit;
        if (hit >= 0) rd.scroll = rd.blocks.data[hit].y - ui_px(30);
    } else if (!rd.query[0]) {
        rd.found = -1;
    }
    if (rd.search_now) ui_request_redraw(); /* (laid out below, searched at the next frame) */
    rd.scroll += ui_scroll_area(body.x, body.y, body.w, body.h);
    if (ui_has_focus(ui_id("##ref_find")) == false) { /* keys scroll when the field doesn't have them */
        if (ui_shortcut(UI_KEY_PAGE_DOWN, 0) || ui_shortcut(UI_KEY_SPACE, 0)) rd.scroll += body.h * 0.9f;
        if (ui_shortcut(UI_KEY_PAGE_UP, 0)) rd.scroll -= body.h * 0.9f;
        if (ui_shortcut(UI_KEY_DOWN, 0)) rd.scroll += ui_px(40);
        if (ui_shortcut(UI_KEY_UP, 0)) rd.scroll -= ui_px(40);
        if (ui_shortcut(UI_KEY_HOME, 0)) rd.scroll = 0;
        if (ui_shortcut(UI_KEY_END, 0)) rd.scroll = 1e9f;
    }
    rd.scroll = FS_CLAMP(rd.scroll, 0.0f, FS_MAX(0.0f, rd.content_h - body.h + pad));
    ui_push_clip(body.x, body.y, body.w, body.h);
    float y = 0;
    for (int i = 0; i < rd.blocks.len; i++) {
        RdBlock *b = &rd.blocks.data[i];
        float sy = body.y + pad + y - rd.scroll;
        bool visible = sy < body.y + body.h && sy + (b->h > 0 ? b->h : body.h) > body.y;
        if (visible && i == rd.found) ui_rrect(body.x + pad - ui_px(6), sy - ui_px(3), tw + ui_px(12), FS_MAX(b->h - ui_px(4), ui_px(10)), ui_px(4),
                                               UI_COL_ACCENT_DIM);
        float h = rd_block(b, body.x + pad, sy, tw, visible);
        b->y = y;
        b->h = h;
        y += h;
    }
    rd.content_h = y + pad;
    ui_pop_clip();
    if (rd.query[0] && rd.found < 0)
        ui_label(pr.x + pr.w - fw - ui_px(52), pr.y + ui_px(38), fw, ui_px(14), "not found", UI_ALIGN_RIGHT, UI_COL_WARNING);
    ui_layer(prev);
}
