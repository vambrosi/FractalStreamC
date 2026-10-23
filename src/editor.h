/* editor.h - a multi-line code editor widget for short scripts (up to a few thousand lines).
 *
 * Typical use (one Editor for the lifetime of the app):
 *
 *     Editor *ed = editor_create();
 *     editor_set_text(ed, script, -1);
 *     ...each frame, between ui_begin_frame() and ui_end_frame():
 *     if (editor_ui(ed, x, y, w, h, false)) { text changed: recompile after a short pause, e.g. using
 *                                             editor_version(ed) }
 *     ...after compiling:
 *     editor_set_diagnostics(ed, diags, ndiags);
 *
 * The widget draws itself with ui.c (monospace font, line-number gutter, syntax highlighting, diagnostics
 * underlines and a status bar) and handles the mouse and keyboard of the current UI frame.  It takes the
 * keyboard focus when clicked; the focus goes away when something else is clicked.
 *
 * Text rules: the buffer is always valid UTF-8 with '\n' line endings.  Tabs are expanded to spaces, CR/CRLF
 * become LF and control characters are dropped when text enters the editor (set_text, paste), so cursor
 * columns are plain character counts.  Lines and columns in this API are 1-based; columns count BYTES of the
 * UTF-8 line (what a lexer naturally produces).
 *
 * Everything the user can do has a function below, so tests (and scripts) can drive the editor without a
 * window: editor_type, editor_newline, editor_backspace, editor_move, editor_undo, ...
 */
#ifndef FS_EDITOR_H
#define FS_EDITOR_H

#include "base.h"
#include "ui.h"

typedef struct Editor Editor;

enum { EDITOR_ERROR = 0, EDITOR_WARNING = 1, EDITOR_INFO = 2 };

/* A message attached to a range of the text.  Underlined in the text, marked in the gutter, shown on hover and
 * (for the caret line, or else the first error) in the status bar. */
typedef struct EditorDiag {
    int line;            /* 1-based */
    int col;             /* 1-based byte column where the range starts */
    int end_col;         /* 1-based byte column just past the range; <= col means "the word at col" */
    int severity;        /* EDITOR_ERROR, EDITOR_WARNING or EDITOR_INFO */
    const char *message; /* copied by editor_set_diagnostics */
} EditorDiag;

typedef enum EditorMove {
    EDITOR_MOVE_LEFT, EDITOR_MOVE_RIGHT, EDITOR_MOVE_UP, EDITOR_MOVE_DOWN,
    EDITOR_MOVE_WORD_LEFT, EDITOR_MOVE_WORD_RIGHT,
    EDITOR_MOVE_LINE_START, /* smart Home: first non-blank, then column 1 */
    EDITOR_MOVE_LINE_END, EDITOR_MOVE_DOC_START, EDITOR_MOVE_DOC_END,
    EDITOR_MOVE_PAGE_UP, EDITOR_MOVE_PAGE_DOWN
} EditorMove;

Editor *editor_create(void);
void editor_destroy(Editor *ed);

/* ---- text -------------------------------------------------------------------------------------------------- */
/* Replace everything (len < 0: NUL-terminated).  Resets selection, undo history, scroll and the modified flag. */
void editor_set_text(Editor *ed, const char *utf8, int len);
const char *editor_get_text(const Editor *ed, int *len); /* NUL-terminated, valid until the next edit */
int editor_length(const Editor *ed);                     /* bytes */
bool editor_is_modified(const Editor *ed);               /* edited since set_text / editor_set_unmodified */
void editor_set_unmodified(Editor *ed);                  /* call after saving */
u32 editor_version(const Editor *ed);                    /* changes whenever the text changes (incl. undo) */

/* ---- lines and positions --------------------------------------------------------------------------------------- */
int editor_line_count(Editor *ed);                       /* at least 1 */
int editor_line_start(Editor *ed, int line);             /* byte offset of 1-based line (clamped) */
int editor_line_end(Editor *ed, int line);               /* offset of its '\n' (or the end of the text) */
void editor_offset_to_pos(Editor *ed, int offset, int *line, int *col); /* 1-based line, 1-based byte column */
int editor_pos_to_offset(Editor *ed, int line, int col); /* clamped to the line */

/* ---- cursor and selection ---------------------------------------------------------------------------------------- */
int editor_cursor(const Editor *ed);                     /* byte offset of the caret */
int editor_anchor(const Editor *ed);                     /* other end of the selection (== cursor if none) */
void editor_set_selection(Editor *ed, int anchor, int cursor);
void editor_select_all(Editor *ed);
char *editor_selected_text(const Editor *ed);            /* malloc'd; "" if nothing is selected */
void editor_goto(Editor *ed, int line, int col);         /* 1-based; also scrolls it into view */
void editor_reveal(Editor *ed);                          /* scroll the caret into view at the next draw */
void editor_move(Editor *ed, EditorMove move, bool extend_selection);

/* ---- editing (what the keyboard does) --------------------------------------------------------------------------- */
void editor_type(Editor *ed, const char *utf8, int len); /* typed text: replaces the selection; typing `end`/`else` at the
                                                            start of a line dedents it */
void editor_newline(Editor *ed);                         /* Enter: auto-indent (+4 after do/then/else/where) */
void editor_backspace(Editor *ed, bool word);            /* in leading spaces removes back to the previous multiple of 4 */
void editor_delete_forward(Editor *ed, bool word);
void editor_tab(Editor *ed, bool shift);                 /* indent/dedent the selected lines, or insert spaces to the next tab stop */
void editor_toggle_comment(Editor *ed);                  /* "# " on the selected lines (or remove it if all have it) */
void editor_copy(Editor *ed);                            /* clipboard; with no selection the whole line */
void editor_cut(Editor *ed);
void editor_paste(Editor *ed);
bool editor_undo(Editor *ed);                            /* typing is grouped by word and by pauses of 1 s */
bool editor_redo(Editor *ed);
/* Raw edits at an offset (undoable).  The caret and selection are adjusted as if another party edited. */
void editor_insert(Editor *ed, int offset, const char *utf8, int len);
void editor_delete(Editor *ed, int offset, int len);
/* Several replacements as one undo step: `del` bytes at `pos` (offsets in the text as it is now; sorted, not
 * overlapping) become `text`.  False (nothing done) if they are out of order or out of range. */
typedef struct EditorEdit { int pos, del; const char *text; } EditorEdit;
bool editor_replace_ranges(Editor *ed, const EditorEdit *edits, int n);

/* ---- highlighting and diagnostics ----------------------------------------------------------------------------------- */
void editor_set_builtins(Editor *ed, const char *const *names, int n); /* function names to highlight */
/* Names offered while a word is typed (2 characters or more): "name", or "name\tdetail" to show a detail
 * (e.g. "palette\tfunction").  Up / Down choose, Tab or Enter take one, Esc closes the list. */
void editor_set_completions(Editor *ed, const char *const *names, int n);
/* What a word is, shown when the mouse rests on it (".maxIterations", "f", "mandel.center"...): `fn` returns a text
 * (kept until the next call) or NULL. */
typedef const char *(*EditorHoverFn)(void *user, const char *word);
void editor_set_hover(Editor *ed, EditorHoverFn fn, void *user);

/* Search (one line of text; match_case false folds ASCII letters).  The searched text is highlighted in the
 * rows while editor_set_search holds it (NULL or "" stops). */
void editor_set_search(Editor *ed, const char *needle, bool match_case);
bool editor_find(Editor *ed, const char *needle, bool match_case, bool backwards); /* select the next (previous)
                                                                                     match, wrapping; reveals it */
int editor_count_matches(Editor *ed, const char *needle, bool match_case, int *current); /* *current: 1-based index of
                                                                                     the selected match, or 0 */
bool editor_replace(Editor *ed, const char *needle, const char *with, bool match_case); /* if the selection is a match:
                                                                                     replace it; then select the next */
int editor_replace_all(Editor *ed, const char *needle, const char *with, bool match_case); /* one undo step; count */
void editor_set_diagnostics(Editor *ed, const EditorDiag *diags, int n);
void editor_clear_diagnostics(Editor *ed);

/* ---- the widget -------------------------------------------------------------------------------------------------------- */
/* Draw the editor into the rectangle and process this frame's input.  `grab_focus` gives it the keyboard focus
 * right now (pass true once, e.g. when the editor panel opens); it also gets the focus when clicked.  Returns true
 * if the text changed during this call.  Everything is drawn inside (x, y, w, h). */
bool editor_ui(Editor *ed, float x, float y, float w, float h, bool grab_focus);
bool editor_has_focus(const Editor *ed);
/* The opacity of the widget's own backgrounds (the text, the gutter, the status bar): 1 (the default) is opaque;
 * below 1 what was drawn under the widget shows through, e.g. a picture under a translucent pane. */
void editor_set_background_alpha(Editor *ed, float alpha);
/* Time source for undo grouping (seconds); editor_ui sets it from the UI input.  Tests can set it by hand. */
void editor_set_clock(Editor *ed, double seconds);

#endif
