/* test_editor.c - tests for editor.c.
 *
 * Part 1 drives the editing commands directly (no window, no rendering): buffer, cursor, selection, undo,
 * auto-indent, comments, clipboard, tokenizer, diagnostics.  Part 2 drives the widget with synthetic UiInput frames
 * (the harness of test_ui.c, which test_main.c includes first), checks pixels with the CPU rasterizer and writes
 * build/editor_preview*.png for looking at the result by eye.
 *
 * Notation in expectations (te_show): the caret is '|', a selection is [..] with the '|' on the caret end. */
#ifndef FS_TEST_UI_HARNESS
#error "test_editor.c reuses the harness of test_ui.c: include test_ui.c before it"
#endif

/* ---- helpers --------------------------------------------------------------------------------------------------- */

static Editor *te_new(const char *text) {
    Editor *ed = editor_create();
    editor_set_text(ed, text, -1);
    return ed;
}

static const char *te_text(Editor *ed) { return editor_get_text(ed, NULL); }

/* Type like a keyboard: one editor_type call per character (the clock advances by dt per character if dt > 0). */
static void te_type_timed(Editor *ed, const char *s, double *t, double dt) {
    int len = (int)strlen(s);
    for (int i = 0; i < len;) {
        int n = ui_utf8_next(s, i) - i;
        if (t) editor_set_clock(ed, *t);
        editor_type(ed, s + i, n);
        if (t) *t += dt;
        i += n;
    }
}
static void te_type(Editor *ed, const char *s) { te_type_timed(ed, s, NULL, 0); }

/* The text with the caret shown as '|' and a selection as [..] (the '|' sits on the caret end). */
static const char *te_show(Editor *ed) {
    static char buf[8192];
    int len, c = editor_cursor(ed), a = editor_anchor(ed), o = 0;
    const char *t = editor_get_text(ed, &len);
    int lo = FS_MIN(a, c), hi = FS_MAX(a, c);
    for (int i = 0; i <= len && o < (int)sizeof buf - 8; i++) {
        if (lo == hi) {
            if (i == c) buf[o++] = '|';
        } else if (i == lo) {
            buf[o++] = '[';
            if (c == lo) buf[o++] = '|';
        } else if (i == hi) {
            if (c == hi) buf[o++] = '|';
            buf[o++] = ']';
        }
        if (i < len) buf[o++] = t[i];
    }
    buf[o] = 0;
    return buf;
}

static void te_moves(Editor *ed, EditorMove m, int n, bool extend) {
    for (int i = 0; i < n; i++) editor_move(ed, m, extend);
}

static bool te_valid_utf8(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (!n) return false;
        for (int k = 1; k < n; k++)
            if ((p[k] & 0xC0) != 0x80) return false;
        p += n;
    }
    return true;
}

/* Tokens of one line as "<class>:<text>" separated by spaces; class letters: c omment, s tring, n umber, h eader,
 * i nternal, k eyword, y (type), b uiltin, o perator.  Plain text between tokens is not listed. */
static const char *te_tokens(Editor *ed, const char *line) {
    static char out[1024];
    EdTok toks[64];
    int nt = edp_tokenize(ed, line, (int)strlen(line), toks, 64), o = 0;
    out[0] = 0;
    for (int i = 0; i < nt; i++)
        o += snprintf(out + o, sizeof out - (size_t)o, "%s%c:%.*s", i ? " " : "", "tcsnhikybo"[toks[i].cls],
                      toks[i].end - toks[i].start, line + toks[i].start);
    return out;
}

/* ---- buffer ---------------------------------------------------------------------------------------------------- */

TEST(editor_text_basics) {
    Editor *ed = editor_create();
    CHECK_INT(editor_length(ed), 0);
    CHECK_INT(editor_line_count(ed), 1);
    CHECK(!editor_is_modified(ed));
    CHECK_STR(te_show(ed), "|");

    /* input is normalized: CR/CRLF -> LF, tabs -> spaces (4-column stops), control characters dropped */
    editor_set_text(ed, "a\r\nb\rc\td\x01\x7f" "e\n", -1);
    CHECK_STR(te_text(ed), "a\nb\nc   de\n");
    CHECK_INT(editor_line_count(ed), 4);
    CHECK_INT(editor_length(ed), 11);
    editor_set_text(ed, "abc", 2);
    CHECK_STR(te_text(ed), "ab");
    editor_set_text(ed, NULL, -1);
    CHECK_INT(editor_length(ed), 0);
    editor_set_text(ed, "ok\xff\xfe!\xC2\x85 \xCE\xB8", -1); /* invalid UTF-8 and a C1 control */
    CHECK(te_valid_utf8(te_text(ed)));
    CHECK(strstr(te_text(ed), "\xCE\xB8") != NULL);
    CHECK(strstr(te_text(ed), "\xC2\x85") == NULL);

    /* modified flag and version */
    editor_set_text(ed, "x", -1);
    u32 v0 = editor_version(ed);
    CHECK(!editor_is_modified(ed));
    editor_type(ed, "y", 1);
    CHECK_STR(te_text(ed), "yx");
    CHECK(editor_is_modified(ed));
    CHECK(editor_version(ed) != v0);
    editor_set_unmodified(ed);
    CHECK(!editor_is_modified(ed));
    u32 v1 = editor_version(ed);
    CHECK(editor_undo(ed));
    CHECK_STR(te_text(ed), "x");
    CHECK(editor_version(ed) != v1); /* undo changes the version too (the app recompiles) */
    CHECK(editor_is_modified(ed));
    editor_destroy(ed);
}

TEST(editor_lines_and_positions) {
    Editor *ed = te_new("ab\n\xCE\xB8x\n\nlast");
    CHECK_INT(editor_line_count(ed), 4);
    int starts[] = { 0, 3, 7, 8 }, ends[] = { 2, 6, 7, 12 };
    for (int i = 0; i < 4; i++) {
        CHECK_INT(editor_line_start(ed, i + 1), starts[i]);
        CHECK_INT(editor_line_end(ed, i + 1), ends[i]);
    }
    CHECK_INT(editor_line_start(ed, 0), 0); /* clamped */
    CHECK_INT(editor_line_start(ed, 99), 8);
    CHECK_INT(editor_line_end(ed, 99), 12);

    /* line and BYTE column (1-based) of an offset */
    int offs[] = { 0, 2, 3, 5, 6, 7, 8, 12, 999, -5 };
    int lines[] = { 1, 1, 2, 2, 2, 3, 4, 4, 4, 1 };
    int cols[] = { 1, 3, 1, 3, 4, 1, 1, 5, 5, 1 };
    for (int i = 0; i < 10; i++) {
        int line = 0, col = 0;
        editor_offset_to_pos(ed, offs[i], &line, &col);
        CHECK_MSG(line == lines[i] && col == cols[i], "offset %d -> %d:%d, expected %d:%d", offs[i], line, col, lines[i], cols[i]);
    }
    CHECK_INT(editor_pos_to_offset(ed, 2, 3), 5);
    CHECK_INT(editor_pos_to_offset(ed, 2, 99), 6);
    CHECK_INT(editor_pos_to_offset(ed, 0, 0), 0);
    CHECK_INT(editor_pos_to_offset(ed, 99, 1), 8);
    CHECK_INT(editor_pos_to_offset(ed, 3, 5), 7);
    for (int o = 0; o <= 12; o++) { /* round trip */
        int line, col;
        editor_offset_to_pos(ed, o, &line, &col);
        CHECK_INT(editor_pos_to_offset(ed, line, col), o);
    }
    editor_offset_to_pos(ed, 3, NULL, NULL); /* NULL outputs are allowed */
    editor_destroy(ed);
}

TEST(editor_typing_and_deleting) {
    Editor *ed = te_new("");
    te_type(ed, "abc\xCE\xB8");
    CHECK_STR(te_show(ed), "abc\xCE\xB8|");
    editor_backspace(ed, false);
    CHECK_STR(te_show(ed), "abc|"); /* the whole 2-byte character went */
    editor_move(ed, EDITOR_MOVE_LEFT, false);
    editor_delete_forward(ed, false);
    CHECK_STR(te_show(ed), "ab|");

    editor_set_text(ed, "hello world", -1); /* typing replaces the selection */
    editor_set_selection(ed, 0, 5);
    editor_type(ed, "bye", -1);
    CHECK_STR(te_show(ed), "bye| world");
    editor_set_selection(ed, 5, 8); /* backspace deletes the selection */
    editor_backspace(ed, false);
    CHECK_STR(te_show(ed), "bye w|d");
    editor_set_selection(ed, 6, 1);
    editor_delete_forward(ed, false);
    CHECK_STR(te_show(ed), "b|");

    editor_set_text(ed, "foo bar", -1); /* by words */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_backspace(ed, true);
    CHECK_STR(te_show(ed), "foo |");
    editor_backspace(ed, true);
    CHECK_STR(te_show(ed), "|");
    editor_set_text(ed, "foo bar", -1);
    editor_delete_forward(ed, true);
    CHECK_STR(te_show(ed), "| bar");
    editor_delete_forward(ed, true);
    CHECK_STR(te_show(ed), "|");
    editor_backspace(ed, false); /* nothing to delete at either end */
    editor_delete_forward(ed, false);
    CHECK_INT(editor_length(ed), 0);

    editor_type(ed, "a\tb", -1); /* typed text is normalized like pasted text */
    CHECK_STR(te_show(ed), "a   b|");
    editor_type(ed, "\r\nx", -1);
    CHECK_STR(te_show(ed), "a   b\nx|");
    editor_type(ed, "\x01\x02", -1);
    CHECK_STR(te_show(ed), "a   b\nx|");
    editor_type(ed, NULL, 0);
    editor_destroy(ed);
}

/* ---- cursor ---------------------------------------------------------------------------------------------------- */

TEST(editor_cursor_movement) {
    Editor *ed = te_new("abc\n\xCE\xB8\nlonger line\n"); /* lines start at 0, 4, 7, 19; len 19 */
    te_moves(ed, EDITOR_MOVE_RIGHT, 3, false);
    CHECK_INT(editor_cursor(ed), 3);
    editor_move(ed, EDITOR_MOVE_RIGHT, false);
    CHECK_INT(editor_cursor(ed), 4); /* over the line break */
    editor_move(ed, EDITOR_MOVE_RIGHT, false);
    CHECK_INT(editor_cursor(ed), 6); /* over the 2-byte theta */
    editor_move(ed, EDITOR_MOVE_LEFT, false);
    CHECK_INT(editor_cursor(ed), 4);
    editor_move(ed, EDITOR_MOVE_LEFT, false);
    CHECK_INT(editor_cursor(ed), 3);
    te_moves(ed, EDITOR_MOVE_LEFT, 9, false);
    CHECK_INT(editor_cursor(ed), 0); /* stops at the start */

    /* Up/Down remember the column through short lines */
    editor_goto(ed, 3, 7);
    CHECK_INT(editor_cursor(ed), 13);
    editor_move(ed, EDITOR_MOVE_UP, false);
    CHECK_INT(editor_cursor(ed), 6); /* the theta line is only 1 character long */
    editor_move(ed, EDITOR_MOVE_DOWN, false);
    CHECK_INT(editor_cursor(ed), 13);
    te_moves(ed, EDITOR_MOVE_UP, 2, false);
    CHECK_INT(editor_cursor(ed), 3);
    te_moves(ed, EDITOR_MOVE_DOWN, 2, false);
    CHECK_INT(editor_cursor(ed), 13);
    te_moves(ed, EDITOR_MOVE_UP, 5, false);
    CHECK_INT(editor_cursor(ed), 0); /* Up on the first line goes to the start */
    editor_goto(ed, 4, 1);
    CHECK_INT(editor_cursor(ed), 19);
    editor_move(ed, EDITOR_MOVE_DOWN, false);
    CHECK_INT(editor_cursor(ed), 19);
    editor_move(ed, EDITOR_MOVE_DOC_START, false);
    CHECK_INT(editor_cursor(ed), 0);
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    CHECK_INT(editor_cursor(ed), 19);

    /* smart Home: first non-blank, then column 1; End */
    editor_set_text(ed, "    foo bar\n\n   \nx", -1);
    editor_move(ed, EDITOR_MOVE_LINE_END, false);
    CHECK_INT(editor_cursor(ed), 11);
    editor_move(ed, EDITOR_MOVE_LINE_START, false);
    CHECK_INT(editor_cursor(ed), 4);
    editor_move(ed, EDITOR_MOVE_LINE_START, false);
    CHECK_INT(editor_cursor(ed), 0);
    editor_move(ed, EDITOR_MOVE_LINE_START, false);
    CHECK_INT(editor_cursor(ed), 4);
    editor_goto(ed, 3, 4);
    editor_move(ed, EDITOR_MOVE_LINE_START, false);
    CHECK_INT(editor_cursor(ed), 13); /* a blank line has no first non-blank character */
    editor_destroy(ed);
}

TEST(editor_word_movement) {
    Editor *ed = te_new("foo  bar+baz\n\xCE\xB8x = 1");
    int fwd[] = { 3, 8, 9, 12, 13, 16, 18, 20 }; /* ends of foo, bar, +, baz, the line break, thetax, =, 1 */
    for (int i = 0; i < 8; i++) {
        editor_move(ed, EDITOR_MOVE_WORD_RIGHT, false);
        CHECK_MSG(editor_cursor(ed) == fwd[i], "word right #%d: %d, expected %d", i, editor_cursor(ed), fwd[i]);
    }
    editor_move(ed, EDITOR_MOVE_WORD_RIGHT, false);
    CHECK_INT(editor_cursor(ed), 20);
    int back[] = { 19, 17, 13, 12, 9, 8, 5, 0 };
    for (int i = 0; i < 8; i++) {
        editor_move(ed, EDITOR_MOVE_WORD_LEFT, false);
        CHECK_MSG(editor_cursor(ed) == back[i], "word left #%d: %d, expected %d", i, editor_cursor(ed), back[i]);
    }

    editor_set_text(ed, "abcdef", -1); /* selection extension and collapsing */
    te_moves(ed, EDITOR_MOVE_RIGHT, 2, true);
    CHECK_STR(te_show(ed), "[ab|]cdef");
    editor_move(ed, EDITOR_MOVE_LEFT, true);
    CHECK_STR(te_show(ed), "[a|]bcdef");
    editor_move(ed, EDITOR_MOVE_RIGHT, false); /* without Shift the selection collapses to its end... */
    CHECK_STR(te_show(ed), "a|bcdef");
    editor_set_selection(ed, 1, 4);
    editor_move(ed, EDITOR_MOVE_LEFT, false); /* ...or start */
    CHECK_STR(te_show(ed), "a|bcdef");
    editor_move(ed, EDITOR_MOVE_LINE_END, true);
    CHECK_STR(te_show(ed), "a[bcdef|]");
    editor_move(ed, EDITOR_MOVE_DOC_START, true);
    CHECK_STR(te_show(ed), "[|a]bcdef");

    /* paging */
    StrBuf sb = { 0 };
    for (int i = 0; i < 50; i++) sb_printf(&sb, "L%d\n", i);
    editor_set_text(ed, sb.data, sb.len);
    sb_free(&sb);
    CHECK_INT(editor_line_count(ed), 51);
    editor_goto(ed, 1, 2);
    editor_move(ed, EDITOR_MOVE_PAGE_DOWN, false);
    int line, col;
    editor_offset_to_pos(ed, editor_cursor(ed), &line, &col);
    CHECK(line == 21 && col == 2); /* page_rows is 20 until a widget sets it */
    editor_move(ed, EDITOR_MOVE_PAGE_UP, false);
    editor_offset_to_pos(ed, editor_cursor(ed), &line, &col);
    CHECK(line == 1 && col == 2);
    te_moves(ed, EDITOR_MOVE_PAGE_DOWN, 3, false);
    CHECK_INT(editor_cursor(ed), editor_length(ed)); /* beyond the end: the very end */
    editor_destroy(ed);
}

TEST(editor_selection_api) {
    Editor *ed = te_new("one\ntwo\nthree");
    editor_set_selection(ed, 2, 6);
    CHECK_INT(editor_anchor(ed), 2);
    CHECK_INT(editor_cursor(ed), 6);
    char *sel = editor_selected_text(ed);
    CHECK_STR(sel, "e\ntw");
    free(sel);
    editor_set_selection(ed, 6, 2); /* reversed selection: same text */
    sel = editor_selected_text(ed);
    CHECK_STR(sel, "e\ntw");
    free(sel);
    CHECK_STR(te_show(ed), "on[|e\ntw]o\nthree");
    editor_set_selection(ed, -5, 999);
    CHECK_INT(editor_anchor(ed), 0);
    CHECK_INT(editor_cursor(ed), 13);
    editor_set_selection(ed, 4, 4);
    sel = editor_selected_text(ed);
    CHECK_STR(sel, "");
    free(sel);
    editor_select_all(ed);
    CHECK_STR(te_show(ed), "[one\ntwo\nthree|]");
    editor_goto(ed, 2, 2);
    CHECK_INT(editor_cursor(ed), 5);
    CHECK_INT(editor_anchor(ed), 5);
    editor_goto(ed, 99, 99);
    CHECK_INT(editor_cursor(ed), 13);
    editor_goto(ed, 0, 0);
    CHECK_INT(editor_cursor(ed), 0);
    editor_goto(ed, 3, 99);
    CHECK_INT(editor_cursor(ed), 13);
    editor_destroy(ed);
}

/* ---- undo ------------------------------------------------------------------------------------------------------ */

TEST(editor_undo_grouping) {
    Editor *ed = te_new("");
    double t = 0;
    te_type_timed(ed, "hello world", &t, 0.1); /* a word keeps the blanks after it: "hello " and "world" */
    CHECK_STR(te_show(ed), "hello world|");
    CHECK(editor_undo(ed));
    CHECK_STR(te_show(ed), "hello |");
    CHECK(editor_undo(ed));
    CHECK_STR(te_show(ed), "|");
    CHECK(!editor_undo(ed));
    CHECK(editor_redo(ed));
    CHECK_STR(te_show(ed), "hello |");
    CHECK(editor_redo(ed));
    CHECK_STR(te_show(ed), "hello world|");
    CHECK(!editor_redo(ed));

    editor_set_text(ed, "", 0); /* a pause of more than a second starts a new group */
    t = 0;
    te_type_timed(ed, "ab", &t, 0.1);
    t += 2.0;
    te_type_timed(ed, "cd", &t, 0.1);
    editor_undo(ed);
    CHECK_STR(te_show(ed), "ab|");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "|");

    editor_set_text(ed, "abcdef", -1); /* a run of backspaces is one step */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    t = 0;
    for (int i = 0; i < 3; i++, t += 0.1) {
        editor_set_clock(ed, t);
        editor_backspace(ed, false);
    }
    CHECK_STR(te_show(ed), "abc|");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "abcdef|");
    editor_redo(ed);
    CHECK_STR(te_show(ed), "abc|");

    editor_set_text(ed, "abcdef", -1); /* and so is a run of Delete */
    for (int i = 0; i < 3; i++, t += 0.1) {
        editor_set_clock(ed, t);
        editor_delete_forward(ed, false);
    }
    CHECK_STR(te_show(ed), "|def");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "|abcdef");

    editor_set_text(ed, "abcd", -1); /* replacing a selection: undo brings the selection back */
    editor_set_selection(ed, 0, 2);
    editor_type(ed, "x", 1);
    CHECK_STR(te_show(ed), "x|cd");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "[ab|]cd");
    CHECK(editor_redo(ed));
    editor_type(ed, "y", 1);
    CHECK(!editor_redo(ed)); /* a new edit forgets the redo history */

    editor_set_text(ed, "", 0); /* line breaks and caret movement end a group */
    te_type(ed, "ab");
    editor_newline(ed);
    te_type(ed, "cd");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "ab\n|");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "ab|");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "|");
    te_type(ed, "ab");
    editor_move(ed, EDITOR_MOVE_LEFT, false);
    te_type(ed, "x");
    CHECK_STR(te_show(ed), "ax|b");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "a|b");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "|");

    editor_set_text(ed, "a\nb\nc", -1); /* a command touching several lines is one step */
    editor_select_all(ed);
    editor_tab(ed, false);
    CHECK_STR(te_text(ed), "    a\n    b\n    c");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "[a\nb\nc|]");
    editor_redo(ed);
    CHECK_STR(te_text(ed), "    a\n    b\n    c");

    editor_set_text(ed, "keep", -1); /* set_text forgets the history */
    CHECK(!editor_undo(ed));
    editor_destroy(ed);
}

/* ---- indentation ----------------------------------------------------------------------------------------------- */

TEST(editor_auto_indent) {
    Editor *ed = te_new("");
    te_type(ed, "for i in 1..3 do");
    editor_newline(ed);
    CHECK_STR(te_show(ed), "for i in 1..3 do\n    |"); /* +4 after do */
    te_type(ed, "x = 1");
    editor_newline(ed);
    CHECK_STR(te_show(ed), "for i in 1..3 do\n    x = 1\n    |"); /* keeps the indentation */
    te_type(ed, "end");
    CHECK_STR(te_show(ed), "for i in 1..3 do\n    x = 1\nend|"); /* electric dedent */
    editor_newline(ed);
    CHECK_STR(te_show(ed), "for i in 1..3 do\n    x = 1\nend\n|");

    editor_set_text(ed, "", 0); /* then / else / where open blocks; a comment after the keyword does not hide it */
    te_type(ed, "if a then");
    editor_newline(ed);
    CHECK_STR(te_show(ed), "if a then\n    |");
    te_type(ed, "y");
    editor_newline(ed);
    te_type(ed, "else");
    CHECK_STR(te_show(ed), "if a then\n    y\nelse|");
    editor_newline(ed);
    CHECK_STR(te_show(ed), "if a then\n    y\nelse\n    |");
    editor_set_text(ed, "define f(z) where # comment", -1);
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_newline(ed);
    CHECK_STR(te_show(ed), "define f(z) where # comment\n    |");

    editor_set_text(ed, "if a then b", -1); /* not at the end of the line: no new level */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_newline(ed);
    CHECK_STR(te_show(ed), "if a then b\n|");
    editor_set_text(ed, "    x = do_it", -1); /* `do_it` is an identifier, not the keyword */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_newline(ed);
    CHECK_STR(te_show(ed), "    x = do_it\n    |");

    editor_set_text(ed, "    foo bar", -1); /* Enter in the middle of a line carries the rest over */
    editor_set_selection(ed, 7, 7);
    editor_newline(ed);
    CHECK_STR(te_show(ed), "    foo\n    | bar");
    editor_set_text(ed, "  ab cd", -1); /* a selection is replaced */
    editor_set_selection(ed, 4, 5);
    editor_newline(ed);
    CHECK_STR(te_show(ed), "  ab\n  |cd");
    editor_destroy(ed);
}

TEST(editor_electric_dedent) {
    Editor *ed = te_new("    ");
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "end");
    CHECK_STR(te_show(ed), "end|");
    editor_undo(ed); /* the last letter and its dedent are a single step */
    CHECK_STR(te_show(ed), "    en|");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "    |");
    editor_redo(ed);
    editor_redo(ed);
    CHECK_STR(te_show(ed), "end|");

    editor_set_text(ed, "        ", -1); /* two levels: one level goes */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "else");
    CHECK_STR(te_show(ed), "    else|");

    editor_set_text(ed, "    ", -1); /* continuing the word gives the indentation back */
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "endpoint");
    CHECK_STR(te_show(ed), "    endpoint|");
    editor_set_text(ed, "    ", -1);
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "end ");
    CHECK_STR(te_show(ed), "end |");

    editor_set_text(ed, "", 0); /* nothing to dedent at column 1, or when the word is not alone */
    te_type(ed, "end");
    CHECK_STR(te_show(ed), "end|");
    editor_set_text(ed, "    x en", -1);
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "d");
    CHECK_STR(te_show(ed), "    x end|");
    editor_set_text(ed, "    sen", -1);
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    te_type(ed, "d");
    CHECK_STR(te_show(ed), "    send|");
    editor_set_text(ed, "    en_", -1);
    editor_set_selection(ed, 6, 6);
    editor_type(ed, "d", 1);
    CHECK_STR(te_show(ed), "    end|_"); /* `end_` is one word */
    editor_destroy(ed);
}

TEST(editor_smart_backspace_and_tab) {
    Editor *ed = te_new("        x");
    editor_set_selection(ed, 8, 8);
    editor_backspace(ed, false); /* in the indentation: back to the previous multiple of 4 */
    CHECK_STR(te_show(ed), "    |x");
    editor_backspace(ed, false);
    CHECK_STR(te_show(ed), "|x");
    editor_set_text(ed, "      x", -1);
    editor_set_selection(ed, 6, 6);
    editor_backspace(ed, false);
    CHECK_STR(te_show(ed), "    |x");
    editor_set_text(ed, "  a   b", -1); /* after text it is a plain backspace */
    editor_set_selection(ed, 6, 6);
    editor_backspace(ed, false);
    CHECK_STR(te_show(ed), "  a  |b");

    editor_set_text(ed, "", 0); /* Tab inserts up to the next multiple of 4 */
    editor_tab(ed, false);
    CHECK_STR(te_show(ed), "    |");
    te_type(ed, "a");
    editor_tab(ed, false);
    CHECK_STR(te_show(ed), "    a   |");
    editor_set_text(ed, "abcdef", -1);
    editor_set_selection(ed, 1, 4); /* a selection inside one line is replaced */
    editor_tab(ed, false);
    CHECK_STR(te_show(ed), "a   |ef");

    editor_set_text(ed, "a\nb\n\nc", -1); /* several lines: indent them all, leave empty lines alone */
    editor_select_all(ed);
    editor_tab(ed, false);
    CHECK_STR(te_text(ed), "    a\n    b\n\n    c");
    CHECK_INT(editor_anchor(ed), 4);
    CHECK_INT(editor_cursor(ed), 18);
    editor_tab(ed, true);
    CHECK_STR(te_text(ed), "a\nb\n\nc");
    editor_tab(ed, true); /* nothing left to remove */
    CHECK_STR(te_text(ed), "a\nb\n\nc");

    editor_set_text(ed, "      a\n  b\nc", -1); /* Shift+Tab removes at most 4 */
    editor_select_all(ed);
    editor_tab(ed, true);
    CHECK_STR(te_text(ed), "  a\nb\nc");
    editor_set_text(ed, "a\nb\nc\n", -1); /* a selection ending at a line start does not include that line */
    editor_set_selection(ed, 0, 4);
    editor_tab(ed, false);
    CHECK_STR(te_text(ed), "    a\n    b\nc\n");
    editor_set_text(ed, "    x", -1); /* Shift+Tab without a selection dedents the caret line */
    editor_set_selection(ed, 5, 5);
    editor_tab(ed, true);
    CHECK_STR(te_show(ed), "x|");
    editor_set_text(ed, "    x", -1);
    editor_set_selection(ed, 2, 2); /* caret inside the removed spaces */
    editor_tab(ed, true);
    CHECK_STR(te_show(ed), "|x");
    editor_destroy(ed);
}

TEST(editor_toggle_comment) {
    Editor *ed = te_new("a = 1");
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_toggle_comment(ed);
    CHECK_STR(te_show(ed), "# a = 1|");
    editor_toggle_comment(ed);
    CHECK_STR(te_show(ed), "a = 1|");

    editor_set_text(ed, "    a\n  b\n\n    c", -1); /* comment markers go at the smallest indentation */
    editor_select_all(ed);
    editor_toggle_comment(ed);
    CHECK_STR(te_text(ed), "  #   a\n  # b\n\n  #   c");
    editor_toggle_comment(ed); /* all commented: remove them again */
    CHECK_STR(te_text(ed), "    a\n  b\n\n    c");
    editor_undo(ed); /* one step */
    CHECK_STR(te_text(ed), "  #   a\n  # b\n\n  #   c");

    editor_set_text(ed, "# a\nb", -1); /* mixed: comment all */
    editor_select_all(ed);
    editor_toggle_comment(ed);
    CHECK_STR(te_text(ed), "# # a\n# b");
    editor_toggle_comment(ed);
    CHECK_STR(te_text(ed), "# a\nb");
    editor_set_text(ed, "#a\n#  b", -1); /* "#" and "# " are removed */
    editor_select_all(ed);
    editor_toggle_comment(ed);
    CHECK_STR(te_text(ed), "a\n b");

    editor_set_text(ed, "", 0); /* an empty line can start a comment */
    editor_toggle_comment(ed);
    CHECK_STR(te_show(ed), "# |");
    editor_set_text(ed, "a\nb\nc\n", -1); /* only the lines the selection touches */
    editor_set_selection(ed, 2, 3);
    editor_toggle_comment(ed);
    CHECK_STR(te_text(ed), "a\n# b\nc\n");
    editor_destroy(ed);
}

/* ---- clipboard and raw edits ----------------------------------------------------------------------------------- */

TEST(editor_clipboard) {
    platform_set_clipboard("");
    Editor *ed = te_new("hello world");
    editor_set_selection(ed, 0, 5);
    editor_copy(ed);
    CHECK_STR(platform_get_clipboard(), "hello");
    editor_move(ed, EDITOR_MOVE_DOC_END, false);
    editor_paste(ed);
    CHECK_STR(te_show(ed), "hello worldhello|");
    editor_undo(ed); /* a paste is one step */
    CHECK_STR(te_text(ed), "hello world");

    editor_set_text(ed, "line1\nline2\n", -1); /* no selection: the whole line */
    editor_set_selection(ed, 2, 2);
    editor_copy(ed);
    CHECK_STR(platform_get_clipboard(), "line1\n");
    editor_goto(ed, 3, 1); /* the empty last line: nothing to copy */
    editor_copy(ed);
    CHECK_STR(platform_get_clipboard(), "line1\n");
    editor_goto(ed, 2, 3);
    editor_cut(ed);
    CHECK_STR(platform_get_clipboard(), "line2\n");
    CHECK_STR(te_show(ed), "line1\n|");
    editor_paste(ed);
    CHECK_STR(te_show(ed), "line1\nline2\n|");

    editor_set_text(ed, "abcdef", -1);
    editor_set_selection(ed, 1, 4);
    editor_cut(ed);
    CHECK_STR(te_show(ed), "a|ef");
    CHECK_STR(platform_get_clipboard(), "bcd");
    editor_undo(ed);
    CHECK_STR(te_show(ed), "a[bcd|]ef");

    platform_set_clipboard("a\tb\r\nc\x01"); /* pasted text is normalized */
    editor_set_text(ed, "XY", -1);
    editor_set_selection(ed, 1, 1);
    editor_paste(ed);
    CHECK_STR(te_text(ed), "Xa  b\ncY"); /* the tab stops at column 4 of the line */
    editor_set_selection(ed, 0, 2); /* replaces a selection */
    platform_set_clipboard("--");
    editor_paste(ed);
    CHECK_STR(te_show(ed), "--|  b\ncY");
    platform_set_clipboard("");
    editor_paste(ed); /* an empty clipboard changes nothing */
    CHECK_STR(te_show(ed), "--|  b\ncY");
    editor_destroy(ed);
}

TEST(editor_raw_edits) {
    Editor *ed = te_new("hello");
    editor_set_selection(ed, 3, 3);
    editor_insert(ed, 1, "XY", -1); /* before the caret: the caret moves along */
    CHECK_STR(te_show(ed), "hXYel|lo");
    editor_insert(ed, 7, "!", 1); /* after it: unchanged */
    CHECK_STR(te_show(ed), "hXYel|lo!");
    editor_delete(ed, 0, 2);
    CHECK_STR(te_show(ed), "Yel|lo!");
    editor_delete(ed, 2, 3); /* covering the caret: it moves to the start of the range */
    CHECK_STR(te_show(ed), "Ye|!");
    editor_undo(ed);
    CHECK_STR(te_text(ed), "Yello!");
    editor_undo(ed);
    editor_undo(ed);
    editor_undo(ed);
    CHECK_STR(te_text(ed), "hello");
    editor_delete(ed, 3, 100); /* clamped */
    CHECK_STR(te_text(ed), "hel");
    editor_insert(ed, 100, "p", 1);
    CHECK_STR(te_text(ed), "help");
    editor_destroy(ed);
}

/* ---- highlighting and diagnostics ------------------------------------------------------------------------------ */

TEST(editor_tokenizer) {
    Editor *ed = editor_create();
    CHECK_STR(te_tokens(ed, "@viewer plane c \"Name\""), "h:@viewer s:\"Name\"");
    CHECK_STR(te_tokens(ed, "  @tool \"Orbit\" \"O\""), "h:@tool s:\"Orbit\" s:\"O\"");
    CHECK_STR(te_tokens(ed, "@other"), "");
    CHECK_STR(te_tokens(ed, "define f(z : C) where"), "k:define o:( o:: y:C o:) k:where");
    CHECK_STR(te_tokens(ed, "x = 3.5e-2 + 4i # note"), "o:= n:3.5e-2 o:+ n:4i c:# note");
    CHECK_STR(te_tokens(ed, "z = i * 2"), "o:= n:i o:* n:2");
    CHECK_STR(te_tokens(ed, "s = \"a\\\"b\" + 1"), "o:= s:\"a\\\"b\" o:+ n:1");
    CHECK_STR(te_tokens(ed, "\"unfinished"), "s:\"unfinished");
    CHECK_STR(te_tokens(ed, "sin(z) + .5"), "b:sin o:( o:) o:+ n:.5");
    CHECK_STR(te_tokens(ed, "w = z.re + 1"), "o:= i:.re o:+ n:1");
    CHECK_STR(te_tokens(ed, "1e5 2E-3 7e"), "n:1e5 n:2E-3 n:7");
    CHECK_STR(te_tokens(ed, "3i2 + 2.5i"), "n:3 o:+ n:2.5i");
    CHECK_STR(te_tokens(ed, "for k in 1..200 do"), "k:for k:in n:1 o:. o:. n:200 k:do");
    CHECK_STR(te_tokens(ed, "while z not escapes do"), "k:while k:not k:escapes k:do");
    CHECK_STR(te_tokens(ed, "color = .black # c # d"), "k:color o:= i:.black c:# c # d");
    CHECK_STR(te_tokens(ed, "# \"not a string\" do"), "c:# \"not a string\" do");
    CHECK_STR(te_tokens(ed, "ifx = doing"), "o:=");   /* keywords are whole words */
    CHECK_STR(te_tokens(ed, "\xCE\xB8 = \xCF\x80 * 2"), "o:= o:* n:2"); /* theta and pi are plain identifiers */

    const char *mine[] = { "foo", "sin2" };
    editor_set_builtins(ed, mine, 2); /* the list is replaced, not extended */
    CHECK_STR(te_tokens(ed, "foo(sin(z)) + sin2"), "b:foo o:( o:( o:) o:) o:+ b:sin2");
    editor_set_builtins(ed, NULL, 0);
    CHECK_STR(te_tokens(ed, "sin"), "b:sin"); /* back to the default list */
    editor_destroy(ed);
}

TEST(editor_diagnostics) {
    Editor *ed = te_new("let x = 1\nfoo bar\n\xCE\xB8 = 1\nabc");
    char msg[32];
    snprintf(msg, sizeof msg, "bad %s", "thing");
    EditorDiag d[] = {
        { 1, 5, 8, EDITOR_ERROR, msg },
        { 2, 1, 0, EDITOR_WARNING, "word at col" },
        { 9, 1, 2, EDITOR_INFO, "no such line" },
    };
    editor_set_diagnostics(ed, d, 3);
    snprintf(msg, sizeof msg, "CHANGED"); /* the messages were copied */
    CHECK_INT(ed->diags.len, 3);
    CHECK_STR(ed->diags.data[0].message, "bad thing");

    int a, b;
    CHECK(edp_diag_range(ed, &ed->diags.data[0], &a, &b)); /* explicit range, 1-based, end exclusive */
    CHECK(a == 4 && b == 7);
    CHECK(edp_diag_range(ed, &ed->diags.data[1], &a, &b)); /* the word at col */
    CHECK(a == 10 && b == 13);
    CHECK(!edp_diag_range(ed, &ed->diags.data[2], &a, &b)); /* line out of range: ignored */

    EditorDiag e[] = {
        { 3, 1, 3, EDITOR_ERROR, "theta" },    /* byte columns: theta is 2 bytes */
        { 4, 50, 0, EDITOR_ERROR, "past the end" },
        { 1, 4, 0, EDITOR_INFO, "space" },     /* not a word: one character */
        { 1, 10, 0, EDITOR_INFO, "at the end" },
    };
    editor_set_diagnostics(ed, e, 4);
    CHECK_INT(ed->diags.len, 4);
    CHECK(edp_diag_range(ed, &ed->diags.data[0], &a, &b));
    CHECK(a == 18 && b == 20);
    CHECK(edp_diag_range(ed, &ed->diags.data[1], &a, &b)); /* clamped: underline the last character */
    CHECK(a == 27 && b == 28);
    CHECK(edp_diag_range(ed, &ed->diags.data[2], &a, &b));
    CHECK(a == 3 && b == 4);
    CHECK(edp_diag_range(ed, &ed->diags.data[3], &a, &b));
    CHECK(a == 8 && b == 9);

    snprintf(msg, sizeof msg, "bad thing");
    editor_set_diagnostics(ed, d, 2); /* status bar: the caret line's diagnostic, else the first error */
    bool on_line;
    editor_goto(ed, 2, 1);
    const EdDiag *sd = edp_status_diag(ed, &on_line);
    CHECK(sd && on_line && strcmp(sd->message, "word at col") == 0);
    editor_goto(ed, 4, 1);
    sd = edp_status_diag(ed, &on_line);
    CHECK(sd && !on_line && strcmp(sd->message, "bad thing") == 0);
    editor_clear_diagnostics(ed);
    CHECK_INT(ed->diags.len, 0);
    CHECK(edp_status_diag(ed, &on_line) == NULL);
    editor_set_diagnostics(ed, NULL, 0);
    editor_destroy(ed);
}

/* ====================================================================================================
 * Part 2: the widget, driven with synthetic input frames
 * ================================================================================================== */

#define TE_X 20.0f
#define TE_Y 20.0f
#define TE_W 480.0f
#define TE_H 260.0f
static float te_scale = 1.0f;

/* A fresh UI at the given content scale; one empty frame so the fonts are baked and the metrics are known. */
static void te_setup(float scale) {
    te_scale = scale;
    tu_reset(scale);
    T.fb_w = (int)(640 * scale);
    T.fb_h = (int)(400 * scale);
    tu_idle();
}

static bool te_frame(Editor *ed, bool grab) {
    tu_begin();
    bool changed = editor_ui(ed, TE_X * te_scale, TE_Y * te_scale, TE_W * te_scale, TE_H * te_scale, grab);
    tu_end();
    return changed;
}

static EdLayout te_layout(Editor *ed) {
    EdLayout L;
    edp_layout(ed, TE_X * te_scale, TE_Y * te_scale, TE_W * te_scale, TE_H * te_scale, &L);
    return L;
}

/* One key press (with modifiers) in a frame of its own; returns whether the text changed. */
static bool te_key(Editor *ed, UiKey k, int mods) {
    tu_key(k, mods);
    bool changed = te_frame(ed, false);
    T.mods = 0;
    return changed;
}

/* Pixel x of a byte offset, and y of the middle of a (0-based) line, with the current scroll position. */
static float te_x(Editor *ed, int offset) {
    EdLayout L = te_layout(ed);
    int line;
    editor_offset_to_pos(ed, offset, &line, NULL);
    int ls = editor_line_start(ed, line);
    return L.tx + L.pad - ed->scroll_x + ui_text_width(UI_FONT_MONO, ed->text + ls, offset - ls);
}
static float te_y(Editor *ed, int line0) {
    EdLayout L = te_layout(ed);
    return L.ty + ((float)line0 + 0.5f) * L.row_h - ed->scroll_y;
}

static void te_click_at(Editor *ed, float x, float y, int clicks) {
    tu_move(x, y);
    tu_press(0, clicks);
    te_frame(ed, false);
    tu_release(0);
    te_frame(ed, false);
}

/* Click on the boundary in front of character (line0, col0); `clicks` = 1, 2 or 3 for a double/triple click. */
static void te_click(Editor *ed, int line0, int col0, int clicks) {
    te_click_at(ed, te_x(ed, editor_pos_to_offset(ed, line0 + 1, col0 + 1)), te_y(ed, line0), clicks);
}

static Editor *te_numbered(int lines) {
    StrBuf sb = { 0 };
    for (int i = 0; i < lines; i++) sb_printf(&sb, "line %d\n", i);
    Editor *ed = te_new(sb.data);
    sb_free(&sb);
    return ed;
}

TEST(editor_widget_click_and_type) {
    te_setup(1.0f);
    Editor *ed = te_new("hello\nworld\n");
    te_frame(ed, false);
    CHECK(!editor_has_focus(ed));
    CHECK(!ui_wants_keyboard());
    te_click(ed, 1, 2, 1);
    CHECK(editor_has_focus(ed));
    CHECK(ui_wants_keyboard()); /* the app must not treat letters as shortcuts now */
    CHECK_INT(editor_cursor(ed), 8);

    tu_type("XY");
    CHECK(te_frame(ed, false)); /* returns true when the text changed */
    CHECK_STR(te_text(ed), "hello\nwoXYrld\n");
    CHECK(!te_frame(ed, false));
    CHECK(te_key(ed, UI_KEY_BACKSPACE, 0));
    CHECK_STR(te_text(ed), "hello\nwoXrld\n");
    tu_type("a"); /* typing and keys keep their order inside one frame */
    tu_key(UI_KEY_BACKSPACE, 0);
    tu_type("b");
    te_frame(ed, false);
    CHECK_STR(te_text(ed), "hello\nwoXbrld\n");

    te_key(ed, UI_KEY_Z, UI_MOD_MOD); /* undo / redo (both spellings) */
    CHECK_STR(te_text(ed), "hello\nwoXrld\n");
    te_key(ed, UI_KEY_Z, UI_MOD_MOD | UI_MOD_SHIFT);
    CHECK_STR(te_text(ed), "hello\nwoXbrld\n");
    te_key(ed, UI_KEY_Z, UI_MOD_MOD);
    te_key(ed, UI_KEY_Y, UI_MOD_MOD);
    CHECK_STR(te_text(ed), "hello\nwoXbrld\n");

    te_key(ed, UI_KEY_HOME, 0); /* movement and selection keys */
    CHECK_INT(editor_cursor(ed), 6);
    te_key(ed, UI_KEY_RIGHT, UI_MOD_SHIFT);
    te_key(ed, UI_KEY_RIGHT, UI_MOD_SHIFT);
    CHECK_STR(te_show(ed), "hello\n[wo|]Xbrld\n");
    te_key(ed, UI_KEY_END, UI_MOD_SHIFT);
    CHECK_STR(te_show(ed), "hello\n[woXbrld|]\n");
    te_key(ed, UI_KEY_A, UI_MOD_MOD);
    CHECK_INT(editor_anchor(ed), 0);
    CHECK_INT(editor_cursor(ed), editor_length(ed));
    tu_type("Q"); /* typing replaces the selection */
    te_frame(ed, false);
    CHECK_STR(te_text(ed), "Q");

    editor_set_text(ed, "", 0); /* Enter auto-indents, Tab inserts spaces */
    te_key(ed, UI_KEY_TAB, 0);
    tu_type("do");
    te_key(ed, UI_KEY_ENTER, 0);
    CHECK_STR(te_show(ed), "    do\n        |"); /* the indentation of the line plus one level after `do` */
    te_key(ed, UI_KEY_TAB, UI_MOD_SHIFT);
    CHECK_STR(te_show(ed), "    do\n    |");
    editor_destroy(ed);
}

TEST(editor_widget_focus) {
    te_setup(1.0f);
    Editor *ed = te_new("abc");
    char field[32] = "";
    tu_type("zzz"); /* without the focus the editor ignores the keyboard */
    CHECK(!te_frame(ed, false));
    CHECK_STR(te_text(ed), "abc");
    tu_type("x"); /* grab_focus takes it right away */
    CHECK(te_frame(ed, true));
    CHECK(editor_has_focus(ed));
    CHECK_STR(te_text(ed), "xabc");
    tu_type("y"); /* and keeps it */
    te_frame(ed, false);
    CHECK_STR(te_text(ed), "xyabc");

    tu_move(600, 380); /* a click elsewhere takes it away */
    tu_press(0, 1);
    te_frame(ed, false);
    tu_release(0);
    te_frame(ed, false);
    CHECK(!editor_has_focus(ed));
    tu_type("q");
    CHECK(!te_frame(ed, false));

    EdLayout L = te_layout(ed); /* a click on the gutter, the scrollbar or the status bar gives it the focus */
    float cx = L.x + L.w * 0.5f, cy = L.y + L.th * 0.5f;
    float spots[3][2] = { { L.x + 4, cy }, { L.tx + L.tw + L.sb_w * 0.5f, cy }, { cx, L.y + L.th + L.status_h * 0.5f } };
    for (int i = 0; i < 3; i++) {
        tu_move(600, 380);
        tu_press(0, 1);
        te_frame(ed, false);
        tu_release(0);
        te_frame(ed, false);
        CHECK(!editor_has_focus(ed));
        te_click_at(ed, spots[i][0], spots[i][1], 1);
        CHECK_MSG(editor_has_focus(ed), "spot %d", i);
    }

    tu_move(-100, -100); /* a text field takes the focus from the editor, and the editor takes it back */
    for (int step = 0; step < 4; step++) {
        float mx = step == 0 || step == 2 ? 100 : 100, my = step % 2 == 0 ? 310 : 100;
        tu_move(mx, my);
        tu_press(0, 1);
        for (int rep = 0; rep < 2; rep++) {
            tu_begin();
            editor_ui(ed, TE_X, TE_Y, TE_W, TE_H, false);
            ui_text_field("field", 20, 300, 200, 24, field, sizeof field);
            tu_end();
            if (rep == 0) tu_release(0);
        }
        if (step % 2 == 0) CHECK(!editor_has_focus(ed) && ui_wants_keyboard());
        else CHECK(editor_has_focus(ed));
    }
    editor_destroy(ed);
}

TEST(editor_widget_mouse_selection) {
    te_setup(1.0f);
    Editor *ed = te_new("hello world\nsecond line\nthird\n"); /* lines start at 0, 12, 24 */
    te_frame(ed, false);
    tu_move(te_x(ed, 1), te_y(ed, 0)); /* drag from (0,1) to (1,3) */
    tu_press(0, 1);
    te_frame(ed, false);
    tu_move(te_x(ed, 15), te_y(ed, 1));
    te_frame(ed, false);
    CHECK_INT(editor_anchor(ed), 1);
    CHECK_INT(editor_cursor(ed), 15);
    tu_release(0);
    te_frame(ed, false);
    tu_move(te_x(ed, 3), te_y(ed, 0)); /* the selection stays after the release; moving the mouse changes nothing */
    te_frame(ed, false);
    CHECK_INT(editor_anchor(ed), 1);
    CHECK_INT(editor_cursor(ed), 15);

    T.mods = UI_MOD_SHIFT; /* Shift+click extends */
    te_click(ed, 2, 2, 1);
    T.mods = 0;
    CHECK_INT(editor_anchor(ed), 1);
    CHECK_INT(editor_cursor(ed), 26);

    te_click(ed, 1, 2, 2); /* double click: the word */
    CHECK_INT(editor_anchor(ed), 12);
    CHECK_INT(editor_cursor(ed), 18);
    te_click(ed, 1, 2, 3); /* triple click: the line with its break */
    CHECK_INT(editor_anchor(ed), 12);
    CHECK_INT(editor_cursor(ed), 24);

    EdLayout L = te_layout(ed); /* a click in the gutter selects the line */
    te_click_at(ed, L.x + 6, te_y(ed, 2), 1);
    CHECK_INT(editor_anchor(ed), 24);
    CHECK_INT(editor_cursor(ed), 30);
    tu_move(L.x + 6, te_y(ed, 0)); /* dragging down the gutter extends by lines */
    tu_press(0, 1);
    te_frame(ed, false);
    tu_move(L.x + 6, te_y(ed, 1));
    te_frame(ed, false);
    tu_release(0);
    te_frame(ed, false);
    CHECK_INT(editor_anchor(ed), 0);
    CHECK_INT(editor_cursor(ed), 24);

    te_click(ed, 0, 5, 1); /* clicking past the end of a line / the text */
    CHECK_INT(editor_cursor(ed), 5);
    te_click_at(ed, L.tx + L.tw - 4, te_y(ed, 0), 1);
    CHECK_INT(editor_cursor(ed), 11);
    te_click_at(ed, L.tx + 50, L.ty + L.th - 3, 1); /* below the last line: the end of the last line */
    CHECK_INT(editor_cursor(ed), 30);
    editor_destroy(ed);
}

TEST(editor_widget_scrolling) {
    te_setup(1.0f);
    Editor *ed = te_numbered(100); /* 101 lines: the last one is empty */
    te_frame(ed, false);
    EdLayout L = te_layout(ed);
    CHECK_NEAR(ed->scroll_y, 0, 0);
    CHECK(ed->page_rows >= 5);

    tu_move(L.tx + 50, L.ty + 50); /* the wheel scrolls 3 lines per notch, only over the editor */
    T.scroll_y = -2;
    te_frame(ed, false);
    CHECK_NEAR(ed->scroll_y, 6 * L.row_h, 0.01);
    T.scroll_y = 50;
    te_frame(ed, false);
    CHECK_NEAR(ed->scroll_y, 0, 0);
    tu_move(600, 380);
    T.scroll_y = -2;
    te_frame(ed, false);
    CHECK_NEAR(ed->scroll_y, 0, 0);
    tu_move(L.tx + 50, L.ty + 50);
    T.scroll_y = -1000; /* clamped at the end: the last line is still visible */
    te_frame(ed, false);
    float max_scroll = (float)(101 + 1) * L.row_h - L.th;
    CHECK_NEAR(ed->scroll_y, max_scroll, 0.01);
    tu_move(-100, -100);

    te_frame(ed, true); /* keys that move the caret scroll it into view */
    editor_set_selection(ed, 0, 0);
    ed->scroll_y = 0;
    te_key(ed, UI_KEY_END, UI_MOD_MOD);
    int last = 100;
    CHECK(ed->scroll_y <= (float)last * L.row_h + 0.01f && (float)(last + 1) * L.row_h <= ed->scroll_y + L.th + 0.01f);
    te_key(ed, UI_KEY_HOME, UI_MOD_MOD);
    CHECK_NEAR(ed->scroll_y, 0, 0);
    te_key(ed, UI_KEY_PAGE_DOWN, 0);
    int line;
    editor_offset_to_pos(ed, editor_cursor(ed), &line, NULL);
    CHECK_INT(line, 1 + ed->page_rows);
    float top = (float)(line - 1) * L.row_h;
    CHECK(top >= ed->scroll_y - 0.01f && top + L.row_h <= ed->scroll_y + L.th + 0.01f);
    te_key(ed, UI_KEY_PAGE_UP, 0);
    editor_offset_to_pos(ed, editor_cursor(ed), &line, NULL);
    CHECK_INT(line, 1);
    editor_goto(ed, 60, 1); /* goto scrolls too */
    te_frame(ed, false);
    float row60 = 59 * L.row_h;
    CHECK(row60 >= ed->scroll_y - 0.01f && row60 + L.row_h <= ed->scroll_y + L.th + 0.01f);

    /* the scrollbar thumb can be dragged */
    ed->scroll_y = 0;
    te_frame(ed, false);
    float content_h = 102 * L.row_h, thumb_h = FS_CLAMP(L.th * L.th / content_h, ui_px(24), L.th), travel = L.th - thumb_h;
    float sbx = L.tx + L.tw + L.sb_w * 0.5f;
    tu_move(sbx, L.ty + thumb_h * 0.5f);
    tu_press(0, 1);
    te_frame(ed, false);
    tu_move(sbx, L.ty + thumb_h * 0.5f + travel * 0.5f);
    te_frame(ed, false);
    CHECK_NEAR(ed->scroll_y, max_scroll * 0.5f, 2.0);
    tu_move(sbx, L.ty + L.th + 200); /* dragging beyond the end clamps */
    te_frame(ed, false);
    CHECK_NEAR(ed->scroll_y, max_scroll, 0.01);
    tu_release(0);
    te_frame(ed, false);

    /* dragging a selection beyond the bottom edge scrolls along */
    ed->scroll_y = 0;
    editor_set_selection(ed, 0, 0);
    te_frame(ed, false);
    tu_move(L.tx + 40, L.ty + 20);
    tu_press(0, 1);
    te_frame(ed, false);
    tu_move(L.tx + 40, L.ty + L.th + 80);
    for (int i = 0; i < 10; i++) te_frame(ed, false);
    CHECK(ed->scroll_y > 4 * L.row_h);
    editor_offset_to_pos(ed, editor_cursor(ed), &line, NULL);
    CHECK(line > ed->page_rows);
    tu_release(0);
    te_frame(ed, false);
    editor_destroy(ed);
}

TEST(editor_widget_keys_left_to_the_app) {
    te_setup(1.0f);
    Editor *ed = te_new("abc");
    te_frame(ed, true);
    /* Mod+Enter (run), Escape and F-keys are not the editor's: the app can still use them */
    tu_key(UI_KEY_ENTER, UI_MOD_MOD);
    tu_key(UI_KEY_ESCAPE, 0);
    tu_key(UI_KEY_F5, 0);
    tu_key(UI_KEY_TAB, UI_MOD_CTRL);
    tu_key(UI_KEY_A, UI_MOD_MOD);
    tu_begin();
    editor_ui(ed, TE_X, TE_Y, TE_W, TE_H, false);
    CHECK(ui_shortcut(UI_KEY_ENTER, UI_MOD_MOD));
    CHECK(ui_shortcut(UI_KEY_ESCAPE, 0));
    CHECK(ui_shortcut(UI_KEY_F5, 0));
    CHECK(ui_shortcut(UI_KEY_TAB, UI_MOD_CTRL));
    CHECK(!ui_shortcut(UI_KEY_A, UI_MOD_MOD)); /* consumed by Select All */
    tu_end();
    T.mods = 0;
    CHECK_STR(te_text(ed), "abc");
    CHECK_INT(editor_cursor(ed), 3);
    editor_destroy(ed);
}

TEST(editor_widget_status_bar_and_wake) {
    te_setup(1.0f);
    Editor *ed = te_new("a = 1\nb = 2\n");
    EditorDiag d = { 2, 3, 0, EDITOR_ERROR, "boom" };
    editor_set_diagnostics(ed, &d, 1);
    te_frame(ed, false);
    CHECK(ui_redraw_in() > 1e8); /* nothing to animate while unfocused */
    EdLayout L = te_layout(ed);
    float msg_x = L.x + ui_px(10) + ui_px(8) + ui_px(8) + 10, msg_y = L.y + L.th + L.status_h * 0.5f;
    te_click_at(ed, msg_x, msg_y, 1); /* clicking the message jumps to the problem */
    CHECK_INT(editor_cursor(ed), 8);
    CHECK(editor_has_focus(ed));
    CHECK(ui_redraw_in() > 0 && ui_redraw_in() <= 0.54); /* the caret blinks */
    te_frame(ed, false);
    CHECK(ui_redraw_in() <= 0.54);
    editor_destroy(ed);
}

TEST(editor_widget_zoom_and_utf8) {
    te_setup(1.0f);
    Editor *ed = te_new("\xCE\xB8x = 1\n");
    te_frame(ed, true);
    /* clicks land between characters, also with multi-byte ones */
    float tw = ui_font_advance(UI_FONT_MONO, 0x3B8);
    float x0 = te_x(ed, 0), y = te_y(ed, 0);
    te_click_at(ed, x0 + tw * 0.3f, y, 1);
    CHECK_INT(editor_cursor(ed), 0);
    te_click_at(ed, x0 + tw * 0.7f, y, 1);
    CHECK_INT(editor_cursor(ed), 2);
    te_click_at(ed, te_x(ed, 3) + 1, y, 1);
    CHECK_INT(editor_cursor(ed), 3);

    float size = ui_font_size(UI_FONT_MONO); /* Mod+= / Mod+- / Mod+0 change the code font size */
    te_key(ed, UI_KEY_EQUAL, UI_MOD_MOD);
    CHECK_NEAR(ui_font_size(UI_FONT_MONO), size + 1, 0);
    te_key(ed, UI_KEY_MINUS, UI_MOD_MOD);
    te_key(ed, UI_KEY_MINUS, UI_MOD_MOD);
    CHECK_NEAR(ui_font_size(UI_FONT_MONO), size - 1, 0);
    te_key(ed, UI_KEY_0, UI_MOD_MOD);
    CHECK_NEAR(ui_font_size(UI_FONT_MONO), 15, 0);
    te_frame(ed, false); /* re-bake with the default size for the tests that follow */
    editor_destroy(ed);
}

/* ---- pixels ---------------------------------------------------------------------------------------------------- */

static RasterImage te_shot(Editor *ed, bool grab) {
    tu_begin();
    ui_rect(0, 0, (float)T.fb_w, (float)T.fb_h, UI_COL_BG);
    editor_ui(ed, TE_X * te_scale, TE_Y * te_scale, TE_W * te_scale, TE_H * te_scale, grab);
    const GfxDrawList *dl = tu_end();
    return tu_render(dl, 0xFF000000u);
}

/* Pixels in [x0,x1) x [y0,y1) within `tol` of `col`. */
static int te_count(const RasterImage *im, float x0, float y0, float x1, float y1, u32 col, int tol) {
    int n = 0;
    for (int y = (int)y0; y < (int)y1; y++)
        for (int x = (int)x0; x < (int)x1; x++)
            if (x >= 0 && y >= 0 && x < im->w && y < im->h && raster_color_dist(raster_pixel(im, x, y), col) <= tol) n++;
    return n;
}

static u32 te_px(const RasterImage *im, float x, float y) { return raster_pixel(im, (int)x, (int)y); }

TEST(editor_widget_render) {
    for (int pass = 0; pass < 2; pass++) {
        float s = pass ? 2.0f : 1.0f;
        te_setup(s);
        Editor *ed = te_new("# c\ndefine f(z) = sin(z) + 3.5\n@viewer\n");
        RasterImage im = te_shot(ed, false);
        EdLayout L = te_layout(ed);
        float row1 = te_y(ed, 1) - L.row_h * 0.5f, row2 = te_y(ed, 2) - L.row_h * 0.5f, row0 = te_y(ed, 0) - L.row_h * 0.5f;
        /* syntax colors: each word has pixels of its class color, and none of the others */
        CHECK(te_count(&im, te_x(ed, 4), row1, te_x(ed, 10), row1 + L.row_h, EDC_KEYWORD, 40) >= 8);
        CHECK(te_count(&im, te_x(ed, 4), row1, te_x(ed, 10), row1 + L.row_h, EDC_BUILTIN, 40) == 0);
        CHECK(te_count(&im, te_x(ed, 18), row1, te_x(ed, 21), row1 + L.row_h, EDC_BUILTIN, 40) >= 5); /* sin */
        CHECK(te_count(&im, te_x(ed, 27), row1, te_x(ed, 30), row1 + L.row_h, EDC_NUMBER, 40) >= 3);   /* 3.5 */
        CHECK(te_count(&im, te_x(ed, 11), row1, te_x(ed, 12), row1 + L.row_h, UI_COL_TEXT, 24) >= 2);  /* f */
        CHECK(te_count(&im, te_x(ed, 0), row0, te_x(ed, 3), row0 + L.row_h, EDC_COMMENT, 40) >= 3);
        CHECK(te_count(&im, te_x(ed, 31), row2, te_x(ed, 38), row2 + L.row_h, UI_COL_ACCENT, 40) >= 5);
        /* backgrounds: plain row, current line, header row, gutter, borders, no scrollbar for a short text */
        float rx = L.tx + L.tw - 6 * s;
        CHECK(raster_color_dist(te_px(&im, rx, te_y(ed, 1)), UI_COL_PANEL) <= 2);
        CHECK(raster_color_dist(te_px(&im, rx, te_y(ed, 0)), ui_mix(UI_COL_PANEL, 0xFFFFFFFFu, 0.045f)) <= 3);
        CHECK(raster_color_dist(te_px(&im, rx, te_y(ed, 2)), ui_mix(UI_COL_PANEL, UI_COL_ACCENT, 0.10f)) <= 3);
        CHECK(raster_color_dist(te_px(&im, L.x + 3 * s, te_y(ed, 1)), EDC_GUTTER_BG) <= 2);
        CHECK(raster_color_dist(te_px(&im, L.tx - 1, te_y(ed, 1)), UI_COL_BORDER) <= 2);
        CHECK(raster_color_dist(te_px(&im, L.x, L.y + 50 * s), UI_COL_BORDER) <= 2);
        CHECK(raster_color_dist(te_px(&im, L.tx + L.tw + L.sb_w * 0.5f, L.ty + 20 * s), UI_COL_PANEL) <= 2);
        CHECK(te_count(&im, L.x + 4 * s, row1, L.tx - 2, row1 + L.row_h, UI_COL_TEXT_OFF, 30) >= 2); /* line number "2" */
        raster_free(&im);

        /* a selection is filled with the selection color; the current-line tint goes away */
        editor_set_selection(ed, editor_pos_to_offset(ed, 2, 4), editor_pos_to_offset(ed, 2, 10));
        im = te_shot(ed, false);
        u32 sel = ui_mix(UI_COL_PANEL, UI_RGB(90, 160, 240), 80.0f / 255.0f);
        CHECK(raster_color_dist(te_px(&im, te_x(ed, 8) + 1, row1), sel) <= 3);
        CHECK(raster_color_dist(te_px(&im, te_x(ed, 20) + 1, row1), UI_COL_PANEL) <= 2);
        CHECK(raster_color_dist(te_px(&im, rx, row1 + 1), UI_COL_PANEL) <= 2);
        raster_free(&im);

        /* diagnostics: underline in the range only, marker in the gutter, tinted row, message in the status bar */
        editor_set_selection(ed, 0, 0);
        EditorDiag d = { 2, 1, 7, EDITOR_ERROR, "expected something" };
        editor_set_diagnostics(ed, &d, 1);
        im = te_shot(ed, false);
        float base = row1 + floorf((L.row_h - L.line_h) * 0.5f) + ui_font_ascent(UI_FONT_MONO) + ui_px(2); /* just below the baseline */
        CHECK(te_count(&im, te_x(ed, 4), base - 1, te_x(ed, 10), base + 4 * s, UI_COL_ERROR, 100) >= 6);
        CHECK(te_count(&im, te_x(ed, 14), base - 1, te_x(ed, 30), base + 4 * s, UI_COL_ERROR, 100) == 0);
        CHECK(raster_color_dist(te_px(&im, L.x + ui_px(9), row1 + L.row_h * 0.5f), UI_COL_ERROR) <= 12);
        CHECK(raster_color_dist(te_px(&im, rx, row1 + 1), ui_mix(UI_COL_PANEL, UI_COL_ERROR, 0.07f)) <= 3);
        float sy = L.y + L.th;
        CHECK(te_count(&im, L.x, sy + 2, L.x + L.w, sy + L.status_h, UI_COL_ERROR, 30) >= 12);
        raster_free(&im);

        /* the caret: drawn when focused, blinking */
        te_click(ed, 1, 6, 1);
        im = te_shot(ed, false);
        float cx = te_x(ed, editor_cursor(ed)), cy = te_y(ed, 1);
        CHECK(raster_color_dist(te_px(&im, cx, cy), UI_COL_TEXT) <= 8);
        CHECK(raster_color_dist(te_px(&im, L.x, L.y + 50 * s), UI_COL_ACCENT) <= 2); /* focused border */
        raster_free(&im);
        T.time += 0.6;
        im = te_shot(ed, false);
        CHECK(raster_color_dist(te_px(&im, cx, cy), UI_COL_TEXT) > 40);
        raster_free(&im);

        /* a long text has a scrollbar thumb */
        Editor *big = te_numbered(100);
        im = te_shot(big, false);
        CHECK(raster_color_dist(te_px(&im, L.tx + L.tw + L.sb_w * 0.5f, L.ty + 10 * s), UI_COL_PANEL) > 8);
        raster_free(&im);
        editor_destroy(big);
        editor_destroy(ed);
    }
}

TEST(editor_widget_big_document) {
    te_setup(1.0f);
    StrBuf sb = { 0 };
    for (int i = 0; i < 30000; i++) sb_printf(&sb, "x%d = sin(z) * %d.5 + c # line %d\n", i, i, i);
    Editor *ed = te_new(sb.data);
    sb_free(&sb);
    CHECK_INT(editor_line_count(ed), 30001);
    tu_begin();
    editor_ui(ed, TE_X, TE_Y, TE_W, TE_H, true);
    const GfxDrawList *dl = tu_end();
    CHECK(dl->nverts < 40000); /* only the visible lines are drawn */
    te_key(ed, UI_KEY_END, UI_MOD_MOD);
    tu_type("tail");
    CHECK(te_frame(ed, false));
    CHECK_INT(editor_line_count(ed), 30001);
    CHECK(strcmp(te_text(ed) + editor_length(ed) - 4, "tail") == 0);
    CHECK(ed->scroll_y > 600000);
    editor_goto(ed, 15000, 1);
    te_frame(ed, false);
    for (int i = 0; i < 20; i++) te_key(ed, UI_KEY_DOWN, 0);
    int line;
    editor_offset_to_pos(ed, editor_cursor(ed), &line, NULL);
    CHECK_INT(line, 15020);
    CHECK(editor_undo(ed)); /* the history works on a big text too */
    editor_destroy(ed);
}

/* ---- the preview picture ----------------------------------------------------------------------------------------- */

static const char *te_sample =
    "# Julia set of z^2 + c, coloured by escape time.\n"
    "@config\n"
    ".title = \"Julia set\"\n"
    ".maxIterations = 300\n"
    "plane.radius = 1.6\n"
    "\n"
    "@viewer plane z \"Dynamical plane\" \"Ctrl + 2\"\n"
    "c := -0.8 + 0.156i\n"
    "while z not escapes do\n"
    "    z = z^2 + c\n"
    "end\n"
    "if .stuck then\n"
    "    color = .black\n"
    "else\n"
    "    t : R = .iterations / 50\n"
    "    color = hsv(t, 0.8)\n"
    "end\n"
    "\n"
    "@tool \"Orbit\" \"O\"\n"
    "w := .lastClick\n"
    "for k in 1..200 do\n"
    "    w = w^2 + c\n"
    "    draw point at w with color .white\n"
    "end\n";

static void te_preview(float scale, const char *png_path, bool crop) {
    te_setup(scale);
    T.fb_w = (int)(780 * scale);
    T.fb_h = (int)(540 * scale);
    Editor *ed = te_new(te_sample);
    EditorDiag diags[] = {
        { 16, 13, 24, EDITOR_ERROR, "hsv takes 3 arguments (hue, saturation, value), but 2 were given" },
        { 8, 1, 2, EDITOR_WARNING, "c is never changed: consider a @global instead" },
        { 15, 5, 6, EDITOR_INFO, "t : R is inferred from the right-hand side" },
    };
    editor_set_diagnostics(ed, diags, 3);
    RasterImage im = { 0 };
    for (int frame = 0; frame < 4; frame++) {
        if (frame == 1) { /* double-click "escapes" on line 9 */
            float x = te_x(ed, editor_pos_to_offset(ed, 9, 16)), y = te_y(ed, 8);
            tu_move(x, y);
            tu_press(0, 2);
        }
        if (frame == 2) {
            tu_release(0);
            float x = te_x(ed, editor_pos_to_offset(ed, 16, 17)), y = te_y(ed, 15); /* rest on the error underline */
            tu_move(x, y);
        }
        if (frame == 3) T.time += 0.6;
        tu_begin();
        ui_rect(0, 0, (float)T.fb_w, (float)T.fb_h, UI_COL_BG);
        editor_ui(ed, 10 * scale, 10 * scale, 760 * scale, 520 * scale, frame == 0);
        const GfxDrawList *dl = tu_end();
        if (frame == 3) im = tu_render(dl, 0xFF000000u);
    }
    CHECK(im.px != NULL);
    if (im.px) {
        if (crop) raster_save(&im, 0, 0, (int)(560 * scale), (int)(300 * scale), 1, png_path);
        else raster_save(&im, 0, 0, im.w, im.h, 1, png_path);
        raster_free(&im);
    }
    editor_destroy(ed);
}

TEST(editor_preview) {
    te_preview(1.0f, "build/editor_preview.png", false);
    te_preview(2.0f, "build/editor_preview_2x.png", true);
}

static const TestCase tests_editor[] = {
    TESTCASE(editor_text_basics),
    TESTCASE(editor_lines_and_positions),
    TESTCASE(editor_typing_and_deleting),
    TESTCASE(editor_cursor_movement),
    TESTCASE(editor_word_movement),
    TESTCASE(editor_selection_api),
    TESTCASE(editor_undo_grouping),
    TESTCASE(editor_auto_indent),
    TESTCASE(editor_electric_dedent),
    TESTCASE(editor_smart_backspace_and_tab),
    TESTCASE(editor_toggle_comment),
    TESTCASE(editor_clipboard),
    TESTCASE(editor_raw_edits),
    TESTCASE(editor_tokenizer),
    TESTCASE(editor_diagnostics),
    TESTCASE(editor_widget_click_and_type),
    TESTCASE(editor_widget_focus),
    TESTCASE(editor_widget_mouse_selection),
    TESTCASE(editor_widget_scrolling),
    TESTCASE(editor_widget_keys_left_to_the_app),
    TESTCASE(editor_widget_status_bar_and_wake),
    TESTCASE(editor_widget_zoom_and_utf8),
    TESTCASE(editor_widget_render),
    TESTCASE(editor_widget_big_document),
    TESTCASE(editor_preview),
};
