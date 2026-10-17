/* ui.h - fonts, 2D draw-list builder and immediate-mode widgets.
 *
 * HOW IT FITS TOGETHER
 *   platform_poll(&in)  ->  ui_begin_frame(&in)  ->  [widgets / primitives]  ->  ui_end_frame()
 *   ui_end_frame() returns the GfxDrawList that gfx_frame_draw() renders.  The font atlas is uploaded with
 *   gfx_set_font_atlas() whenever ui_take_atlas_update() returns pixels (first frame, content-scale change,
 *   font-size change, icons drawn for the first time):
 *
 *       ui_begin_frame(&in);
 *       int aw, ah; const u8 *px = ui_take_atlas_update(&aw, &ah);
 *       if (px) gfx_set_font_atlas(px, aw, ah);
 *       ... build the UI ...
 *       const GfxDrawList *dl = ui_end_frame();
 *       platform_set_cursor(ui_wants_mouse() ? ui_cursor() : my_cursor);
 *       platform_poll_timeout(&in, fmin(ui_redraw_in(), 0.5));   // next frame
 *
 * CONVENTIONS
 *   - All coordinates and sizes are FRAMEBUFFER PIXELS (physical), origin top-left.  Multiply "design"
 *     sizes by ui_scale() (or use ui_px()) so the UI looks the same on Retina / high-DPI screens.
 *   - Colors are packed u32 `r | g<<8 | b<<16 | a<<24` (straight alpha), see UI_RGBA.
 *   - Widgets take an explicit rectangle (x, y, w, h) and a label.  The label is also the widget's identity:
 *     two widgets with the same label in the same ID scope are the same widget.  Use "Name##unique" to show
 *     "Name" but hash the whole string, or ui_push_id() to open a scope.
 *   - Widgets return true when the value they edit changed this frame.
 *   - Overlap: widgets on different LAYERS may overlap (the higher layer gets the mouse); widgets on the same
 *     layer must not overlap.  Draw overlays after (or before) the base UI, it does not matter: ui_layer()
 *     decides what is on top.  Panels / widgets block the mouse for the layers below them.
 *   - Not thread safe; one UI context per process.
 */
#ifndef FS_UI_H
#define FS_UI_H

#include "base.h"
#include "platform.h"

/* ---- draw list (shared with gfx.h, which defines the same types and FS_GFX_DRAWLIST_DEFINED) ---------- */
#ifndef FS_GFX_DRAWLIST_DEFINED
#define FS_GFX_DRAWLIST_DEFINED
typedef struct GfxVertex { float x, y; float u, v; u32 rgba; } GfxVertex; /* pixels, top-left origin */
typedef struct GfxCmd {
    u32 first_index, index_count;
    int texture;                               /* 0 = font atlas, >0 = render target id */
    float clip_x0, clip_y0, clip_x1, clip_y1;  /* scissor in framebuffer pixels */
} GfxCmd;
typedef struct GfxDrawList {
    GfxVertex *verts; int nverts;
    u32 *indices; int nindices;                /* triangles */
    GfxCmd *cmds; int ncmds;
} GfxDrawList;
#endif

/* ---- colors ------------------------------------------------------------------------------------------ */
#define UI_RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define UI_RGB(r, g, b) UI_RGBA(r, g, b, 255)
#define UI_HEX(h) UI_RGB(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
#define UI_COL_BG UI_HEX(0x101216)            /* behind everything */
#define UI_COL_PANEL UI_HEX(0x181b21)         /* panels, editor background */
#define UI_COL_PANEL_HI UI_HEX(0x20242c)      /* raised surfaces: menus, tooltips */
#define UI_COL_WIDGET UI_HEX(0x262a34)        /* buttons, fields, slider tracks */
#define UI_COL_WIDGET_HOVER UI_HEX(0x303544)
#define UI_COL_WIDGET_ACTIVE UI_HEX(0x3a4153)
#define UI_COL_BORDER UI_HEX(0x363c4a)
#define UI_COL_TEXT UI_HEX(0xdde1e9)
#define UI_COL_TEXT_DIM UI_HEX(0x8b93a5)
#define UI_COL_TEXT_OFF UI_HEX(0x5a6173)      /* disabled */
#define UI_COL_ACCENT UI_HEX(0x5aa0f0)        /* the one accent color */
#define UI_COL_ACCENT_HOVER UI_HEX(0x7ab6f7)
#define UI_COL_ACCENT_DIM UI_HEX(0x2b4f7e)    /* fills: slider value, selected button */
#define UI_COL_ON_ACCENT UI_HEX(0x0b1017)     /* text drawn on an accent fill */
#define UI_COL_SELECTION UI_RGBA(90, 160, 240, 80)
#define UI_COL_ERROR UI_HEX(0xff6b6b)
#define UI_COL_WARNING UI_HEX(0xf0b84a)
#define UI_COL_OK UI_HEX(0x6fcf97)

u32 ui_alpha(u32 rgba, float a);              /* multiply the alpha channel by a (0..1) */
u32 ui_mix(u32 a, u32 b, float t);            /* linear blend, t = 0 gives a */

/* ---- fonts and text ----------------------------------------------------------------------------------- */
typedef enum UiFont { UI_FONT_SANS, UI_FONT_MONO, UI_FONT_COUNT } UiFont;

/* Bake both fonts (and the icons requested so far, see ui_icon) for the given content scale and return the RGBA8
 * atlas (white RGB, glyph coverage in alpha, texel (0,0) opaque white).  Owned by ui.c, valid until the next bake.
 * Called implicitly by ui_begin_frame when the scale or a font size changes or new icons were requested;
 * calling it first (e.g. in tests) is optional. */
const u8 *ui_fonts_init(float scale, int *w, int *h);
/* Returns the atlas pixels if they were (re)built since the last call, else NULL.  Call after ui_begin_frame
 * every frame and upload with gfx_set_font_atlas() when non-NULL (the size can change between rebuilds). */
const u8 *ui_take_atlas_update(int *w, int *h);
/* Logical (unscaled) em size in pixels; defaults: sans 15, mono 15.  Takes effect at the next ui_begin_frame. */
void ui_set_font_size(UiFont f, float logical_px);
float ui_font_size(UiFont f);
float ui_scale(void);                         /* content scale the fonts were baked for */
float ui_px(float logical);                   /* logical * ui_scale() */

float ui_font_line_height(UiFont f);          /* pixels, ascent + descent + gap, rounded */
float ui_font_ascent(UiFont f);               /* pixels from the top of the line box to the baseline */
float ui_font_advance(UiFont f, u32 codepoint);
float ui_text_width(UiFont f, const char *utf8, int len); /* len < 0: NUL-terminated */
/* Draw text with the TOP of its line box at y (so a label in a box of height h goes at y + (h - line)/2).
 * Glyphs are placed on whole pixels.  Codepoints missing from the atlas draw as U+FFFD. */
void ui_text(UiFont f, float x, float y, const char *utf8, int len, u32 rgba);
/* Word-wraps at max_w and returns the height used; with draw == false only measures. */
float ui_text_wrapped(UiFont f, float x, float y, float max_w, const char *utf8, u32 rgba, bool draw);

enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_RIGHT = 2 };
/* A one-line label centered vertically in the box and clipped to it. */
void ui_label(float x, float y, float w, float h, const char *utf8, int align, u32 rgba);

/* ---- icons: Google's Material Icons (Apache License 2.0), named as on https://fonts.google.com/icons ------------
 * Names are the lowercase snake_case ones ("menu", "pan_tool", "my_location", "timeline", ...).  Glyphs are baked on
 * demand into the same atlas as the text: the first time an icon is drawn at a size (rounded to whole pixels, so
 * pass sizes like ui_px(24)) it is only scheduled, and ui_begin_frame rebuilds the atlas before the next frame
 * (once per frame, however many icons are new; ui_take_atlas_update() then returns it as usual).  That first
 * frame shows nothing in the icon's place and calls ui_request_redraw(), so an event-driven main loop runs the
 * next frame immediately.  A program that renders a single frame (a screenshot) must run its UI twice.
 * Icons and sizes requested earlier are kept across rebuilds and content-scale changes (the oldest are dropped
 * only past a generous budget: 512 icon/size pairs or 1M atlas pixels). */
u32 ui_icon_lookup(const char *name);         /* codepoint of a Material Icons name, 0 if unknown (exact match) */
/* Draw an icon centered in the square (x, y, size, size); size is its em, e.g. 24 px for a standard icon (the
 * glyph's ink is a bit smaller, as designed).  If `name` is NULL, empty or unknown, `fallback` (UTF-8, e.g. the
 * first letter of a tool's name) is drawn in the sans font instead (immediately, at the normal font size), with
 * its width and cap height centered in the square.  A NULL/empty fallback then draws nothing. */
void ui_icon(const char *name, const char *fallback, float x, float y, float size, u32 rgba);

/* UTF-8 and word helpers on NUL-terminated strings (byte offsets).  Words are runs of letters/digits/_ (including
 * Greek and accented letters), or runs of other symbols; '\n' always stops a movement. */
int ui_utf8_prev(const char *s, int i);       /* start of the character before offset i */
int ui_utf8_next(const char *s, int i);       /* offset after the character at i */
int ui_word_left(const char *s, int i);       /* where Ctrl+Left (Alt+Left on macOS) goes from i */
int ui_word_right(const char *s, int i);      /* where Ctrl+Right goes from i */
void ui_word_at(const char *s, int i, int *start, int *end); /* the word under offset i (double click) */

/* ---- frame ------------------------------------------------------------------------------------------- */
void ui_begin_frame(const UiInput *in);
const GfxDrawList *ui_end_frame(void);        /* valid until the next ui_begin_frame */
const UiInput *ui_input(void);                /* this frame's input snapshot */

/* Layers: primitives go to the current layer; higher layers draw on top and get the mouse first. */
enum { UI_LAYER_BASE = 0, UI_LAYER_PANEL, UI_LAYER_MENU, UI_LAYER_POPUP, UI_LAYER_TOOLTIP, UI_LAYER_COUNT };
int ui_layer(int layer);                      /* returns the previous layer; each layer has its own clip stack */
void ui_push_clip(float x, float y, float w, float h); /* intersected with the current clip */
void ui_pop_clip(void);

/* ---- primitives --------------------------------------------------------------------------------------
 * Axis-aligned rectangles snap to whole pixels.  Lines, circles and rounded shapes are anti-aliased with a
 * 1 px feathered edge.  Thickness is in pixels. */
void ui_rect(float x, float y, float w, float h, u32 rgba);
void ui_rect_outline(float x, float y, float w, float h, float thickness, u32 rgba); /* drawn inside the rect */
void ui_rect_gradient(float x, float y, float w, float h, u32 top, u32 bottom);
void ui_rrect(float x, float y, float w, float h, float radius, u32 rgba);
void ui_rrect_outline(float x, float y, float w, float h, float radius, float thickness, u32 rgba); /* inside */
void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 rgba);
void ui_polyline(const float *xy, int npoints, bool closed, float thickness, u32 rgba);
void ui_circle(float cx, float cy, float radius, float thickness, u32 rgba);
void ui_disk(float cx, float cy, float radius, u32 rgba);
void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 rgba);
/* A textured quad.  `texture` is a gfx render-target id (>0), or 0 for the font atlas. */
void ui_image(int texture, float x, float y, float w, float h);
void ui_image_ex(int texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 tint);

/* ---- identity, focus and mouse capture (for custom widgets) ------------------------------------------- */
u32 ui_id(const char *str);                   /* hash of str within the current ID scope */
void ui_push_id(const char *str);
void ui_push_id_int(int n);
void ui_push_id_ptr(const void *p);
void ui_pop_id(void);

u32 ui_active_id(void);                       /* widget holding the mouse (button went down on it), or 0 */
void ui_set_active(u32 id);
u32 ui_focus_id(void);                        /* widget holding the keyboard, or 0 */
bool ui_has_focus(u32 id);                    /* custom widgets must call this every frame while they are drawn */
void ui_set_focus(u32 id);                    /* 0 removes the focus.  A click on empty space also removes it. */
void ui_reset_interaction(void);              /* forget focus, mouse capture and hover state (e.g. after changing screens) */
/* True if the mouse is inside the rectangle (clipped to the current clip) and not covered by something on a
 * higher layer.  Does not look at which widget is active. */
bool ui_mouse_over(float x, float y, float w, float h);
/* Mark a region as covered: layers below do not get the mouse there, and ui_wants_mouse() is true over it. */
void ui_occlude(float x, float y, float w, float h);
/* Tell the UI the application owns the mouse this frame (it is panning a viewer, say): widgets ignore it.
 * Reset at every ui_begin_frame. */
void ui_set_mouse_captured(bool captured);

/* Generic interaction for a rectangle: hover, press, drag, click.  Registers the rect as covering. */
typedef struct UiInteract {
    bool hovered;        /* mouse over it, nothing above it, and no other widget is being dragged */
    bool pressed;        /* left button went down on it this frame */
    bool held;           /* left button is down and the press began on it */
    bool released;       /* the button went up this frame after a press that began on it */
    bool clicked;        /* released while still over it */
    bool double_clicked; /* pressed as the second click of a double click */
    float dx, dy;        /* mouse movement since the previous frame while held */
    float press_x, press_y; /* where the press began */
    float held_time;     /* seconds since the press began, while held */
} UiInteract;
UiInteract ui_interact(const char *id, float x, float y, float w, float h);
UiInteract ui_interact_id(u32 id, float x, float y, float w, float h);
/* The last ui_interact* (hence the last widget) was hovered: `ui_button(..); if (ui_last_hovered()) ...`. */
bool ui_last_hovered(void);

/* Keyboard events of this frame, in order (see UiKeyEvent).  Widgets mark the events they handled as consumed. */
const UiKeyEvent *ui_key_events(int *n);
bool ui_key_consumed(int i);
void ui_key_consume(int i);
/* True once if the key was pressed with exactly these modifiers (UI_MOD_MOD, UI_MOD_SHIFT ...) this frame and
 * nobody consumed it; consumes it.  Use for global shortcuts: call it AFTER the widgets so a focused text
 * widget gets first pick. */
bool ui_shortcut(UiKey key, int mods);
bool ui_shortcut_repeat(UiKey key, int mods); /* same, but also fires for auto-repeat events (hold Ctrl+Z) */

/* What the UI wants from the application this frame (valid after the widgets ran). */
bool ui_wants_mouse(void);                    /* mouse is over a widget/panel or one is being dragged */
bool ui_wants_keyboard(void);                 /* a text widget has the focus: do not treat keys as shortcuts */
UiCursor ui_cursor(void);                     /* cursor shape the widgets asked for */
void ui_set_cursor(UiCursor c);
void ui_request_redraw(void);                 /* run another frame immediately (animations, auto-scroll) */
void ui_wake_in(double seconds);              /* ask for a frame within this many seconds (custom blinking carets etc.) */
/* Seconds until the UI needs a frame even without input (caret blink, tooltip delay); 0 = right now,
 * 1e9 = never.  Pass min(this, 0.5) as the timeout of platform_poll_timeout(). */
double ui_redraw_in(void);

/* ---- widgets ------------------------------------------------------------------------------------------- */
/* flags for ui_button_ex */
enum { UI_DISABLED = 1, UI_ACCENT = 2 /* primary action */, UI_SELECTED = 4 /* toggled-on look */, UI_FLAT = 8 /* no fill until hovered */ };

void ui_panel(float x, float y, float w, float h);                       /* PANEL fill + border, covers the mouse */
void ui_panel_ex(float x, float y, float w, float h, u32 fill, float radius); /* custom fill, e.g. a translucent overlay */
void ui_separator(float x, float y, float w);                            /* horizontal 1 px rule */

bool ui_button(const char *label, float x, float y, float w, float h);   /* true when clicked */
bool ui_button_ex(const char *label, float x, float y, float w, float h, int flags);
/* Square icon button (x, y, size, size) that looks and behaves like ui_button (hover, pressed, UI_SELECTED when
 * `selected`); true when clicked.  `id` is its identity (see "Conventions"; it is not shown), `name` and `fallback`
 * are as in ui_icon, drawn at about 60% of `size`.  Follow it with ui_tooltip() to label it.  The _ex form takes
 * the UI_DISABLED / UI_ACCENT / UI_SELECTED / UI_FLAT flags of ui_button_ex. */
bool ui_icon_button(const char *id, const char *name, const char *fallback, float x, float y, float size, bool selected);
bool ui_icon_button_ex(const char *id, const char *name, const char *fallback, float x, float y, float size, int flags);
/* Checkbox with the label to its right.  Returns true when toggled. */
bool ui_toggle(const char *label, float x, float y, float w, float h, bool *value);

/* Sliders show "label ......... value" inside the track.  fmt is a printf format for the value (NULL: "%.4g"). */
enum { UI_SLIDER_LOG = 1 /* logarithmic scale: lo and hi must be > 0 */ };
bool ui_slider_double(const char *label, float x, float y, float w, float h, double *v, double lo, double hi, const char *fmt, int flags);
bool ui_slider_float(const char *label, float x, float y, float w, float h, float *v, float lo, float hi, const char *fmt);
bool ui_slider_int(const char *label, float x, float y, float w, float h, int *v, int lo, int hi);
/* Drag fields: press and drag horizontally to change the value by `speed` per logical pixel (hold Shift
 * for 10x finer); double-click to type a number (Enter or click elsewhere accepts, Escape cancels).
 * If lo < hi the value is clamped to [lo, hi]. */
bool ui_drag_double(const char *label, float x, float y, float w, float h, double *v, double speed, double lo, double hi, const char *fmt);
bool ui_drag_float(const char *label, float x, float y, float w, float h, float *v, float speed, float lo, float hi, const char *fmt);
bool ui_drag_int(const char *label, float x, float y, float w, float h, int *v, float speed, int lo, int hi);

/* Single-line text field editing `buf` (capacity `cap` bytes, UTF-8, NUL-terminated).  Click to focus;
 * supports selection, word movement, clipboard (Mod+A/C/X/V).  ui_text_field returns true when the text
 * changed; the _ex form returns UI_TEXT_* bits: CHANGED any edit, SUBMIT Enter pressed, CANCEL Escape pressed
 * (the text is restored to what it was when the field got the focus), BLURRED the focus left the field. */
enum { UI_TEXT_CHANGED = 1, UI_TEXT_SUBMIT = 2, UI_TEXT_CANCEL = 4, UI_TEXT_BLURRED = 8 };
enum { UI_TF_KEEP_FOCUS = 1 /* Enter does not remove the focus */, UI_TF_SELECT_ALL = 2 /* select everything on focus */,
       UI_TF_READONLY = 4 };
bool ui_text_field(const char *label, float x, float y, float w, float h, char *buf, int cap);
int ui_text_field_ex(const char *label, float x, float y, float w, float h, char *buf, int cap, int flags, const char *placeholder);

/* Tooltip for the widget drawn just before (shown after the mouse rests on it for ~0.5 s). */
void ui_tooltip(const char *text);
/* Show a tooltip next to the mouse right now (wrapped), on the top layer. */
void ui_tooltip_now(const char *text);

/* Mouse wheel over a region: returns how many pixels to scroll (positive = further down the content), or 0 if the
 * mouse is elsewhere.  The application still decides what the wheel does elsewhere: check ui_wants_mouse(). */
float ui_scroll_area(float x, float y, float w, float h);

#endif
