/* platform.h - the window and the input, on top of GLFW.
 *
 * One window, created without a graphics API (the renderer, gfx.c, attaches Vulkan to it).
 * Every frame the application calls platform_poll(), which runs the GLFW event loop and fills a
 * UiInput with what happened since the previous call.  Everything the UI sees comes through
 * UiInput; nothing else in the program needs to include GLFW headers for input.
 *
 * Coordinates: all positions are in FRAMEBUFFER PIXELS with the origin at the top-left.  On a
 * Retina Mac the framebuffer is twice the window size; platform.c does the conversion.
 * `content_scale` tells the UI how big to draw things (1.0 normal, 2.0 Retina, 1.5 for 150% on
 * Windows); fonts and widgets are sized with it.
 */
#ifndef FS_PLATFORM_H
#define FS_PLATFORM_H

#include "base.h"

/* ---- keys -----------------------------------------------------------
 * Letters and digits use their upper-case ASCII code (UI_KEY_A == 'A', UI_KEY_1 == '1'), and so do the
 * punctuation keys below, so `(UiKey)'Q'` works.  Everything else is numbered from 128.
 * Keys are physical-layout based (US names), exactly like GLFW. */
typedef enum UiKey {
    UI_KEY_NONE = 0,
    UI_KEY_SPACE = ' ',
    UI_KEY_APOSTROPHE = '\'',
    UI_KEY_COMMA = ',',
    UI_KEY_MINUS = '-',
    UI_KEY_PERIOD = '.',
    UI_KEY_SLASH = '/',
    UI_KEY_0 = '0', UI_KEY_1, UI_KEY_2, UI_KEY_3, UI_KEY_4, UI_KEY_5, UI_KEY_6, UI_KEY_7, UI_KEY_8, UI_KEY_9,
    UI_KEY_SEMICOLON = ';',
    UI_KEY_EQUAL = '=',
    UI_KEY_A = 'A', UI_KEY_B, UI_KEY_C, UI_KEY_D, UI_KEY_E, UI_KEY_F, UI_KEY_G, UI_KEY_H, UI_KEY_I,
    UI_KEY_J, UI_KEY_K, UI_KEY_L, UI_KEY_M, UI_KEY_N, UI_KEY_O, UI_KEY_P, UI_KEY_Q, UI_KEY_R,
    UI_KEY_S, UI_KEY_T, UI_KEY_U, UI_KEY_V, UI_KEY_W, UI_KEY_X, UI_KEY_Y, UI_KEY_Z,
    UI_KEY_LEFT_BRACKET = '[',
    UI_KEY_BACKSLASH = '\\',
    UI_KEY_RIGHT_BRACKET = ']',
    UI_KEY_GRAVE = '`',

    UI_KEY_LEFT = 128, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN,
    UI_KEY_INSERT, UI_KEY_BACKSPACE, UI_KEY_DELETE,
    UI_KEY_ENTER, UI_KEY_TAB, UI_KEY_ESCAPE,
    UI_KEY_F1, UI_KEY_F2, UI_KEY_F3, UI_KEY_F4, UI_KEY_F5, UI_KEY_F6,
    UI_KEY_F7, UI_KEY_F8, UI_KEY_F9, UI_KEY_F10, UI_KEY_F11, UI_KEY_F12, /* consecutive */
    UI_KEY_COUNT
} UiKey;

/* Modifier bits (UiKeyEvent.mods, UiInput.mods).  UI_MOD_MOD is "the shortcut key": Cmd on macOS,
 * Ctrl elsewhere.  UI_MOD_WORD is the modifier for word-wise cursor movement: Alt on macOS, Ctrl
 * elsewhere. */
enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4, UI_MOD_SUPER = 8 };
#ifdef FS_OS_MACOS
#define UI_MOD_MOD UI_MOD_SUPER
#define UI_MOD_WORD UI_MOD_ALT
#else
#define UI_MOD_MOD UI_MOD_CTRL
#define UI_MOD_WORD UI_MOD_CTRL
#endif

/* One entry of the keyboard event stream.  Key presses and typed characters share ONE ordered stream
 * so that "type a, press Backspace, type b" is replayed in the right order:
 *   - key press / auto-repeat:  key != UI_KEY_NONE, text == 0
 *   - typed character:          key == UI_KEY_NONE, text == the Unicode codepoint (>= 32)
 * (The same codepoints are also listed in UiInput.text for code that only cares about typing.)
 * Characters typed while Ctrl (or Cmd) is held without Alt are NOT reported as text: those are shortcuts. */
typedef struct UiKeyEvent {
    UiKey key;
    int mods;
    bool repeat;
    u32 text;
} UiKeyEvent;

typedef struct UiInput {
    float mouse_x, mouse_y;                   /* framebuffer pixels */
    bool mouse_down[3];                       /* left, right, middle */
    bool mouse_pressed[3], mouse_released[3]; /* edges this frame; a press and the release of one button never share a
                                                 frame (a click that arrives within one poll is delivered over two) */
    int click_count;                          /* 1, 2, 3 for single/double/triple click on the last left press */
    bool mouse_back, mouse_forward;           /* the side buttons of the mouse (4 and 5) were pressed this frame */
    float scroll_x, scroll_y;                 /* wheel delta this frame (lines; positive y = away from the user) */
    UiKeyEvent keys[64];                      /* ordered key presses, repeats and typed characters this frame */
    int nkeys;
    u32 text[64];                             /* typed codepoints this frame (also present in `keys`) */
    int ntext;
    bool key_down[UI_KEY_COUNT];              /* keys currently held (for "hold space to pan" style behaviour) */
    int mods;                                 /* current modifiers */
    double time;                              /* seconds, monotonic */
    float dt;                                 /* seconds since the previous platform_poll */
    float content_scale;                      /* 1.0, 2.0 on Retina */
    int fb_w, fb_h;                           /* framebuffer size in pixels */
    char **dropped_files;                     /* paths dropped on the window this frame (valid until next poll) */
    int ndropped;
    bool mouse_in_window;
    bool focused;                             /* window has keyboard focus */
} UiInput;

typedef enum UiCursor {
    UI_CURSOR_ARROW, UI_CURSOR_IBEAM, UI_CURSOR_HAND, UI_CURSOR_HRESIZE,
    UI_CURSOR_VRESIZE, UI_CURSOR_MOVE, UI_CURSOR_CROSSHAIR, UI_CURSOR_COUNT
} UiCursor;

/* ---- window ---------------------------------------------------------- */

/* glfwInit + create the window (GLFW_NO_API, resizable, hidden until the first poll is not needed).
 * The window size is in screen coordinates; on Windows/X11 it is scaled by the monitor's content scale.
 * Returns false and fills `err` on failure (e.g. no display).  Call gfx_preinit_loader() before this. */
bool platform_init(int w, int h, const char *title, char *err, int errcap);
void platform_shutdown(void);
void *platform_glfw_window(void); /* GLFWwindow*, or NULL before platform_init */
bool platform_should_close(void);
void platform_set_should_close(bool close);
void platform_set_title(const char *title);
void platform_set_size(int w, int h);       /* window size in screen coordinates */
void platform_get_size(int *w, int *h);     /* window size in screen coordinates */
void platform_toggle_fullscreen(void);      /* on the monitor the window is mostly on */
bool platform_is_fullscreen(void);

/* Process events and fill `in`.  wait == true blocks until an event arrives (or ~0.5 s have passed);
 * use it when nothing is animating so an idle app uses no CPU.  Always call once per frame.
 * If input had to be held back to keep a press and a release apart (see UiInput.mouse_pressed), the next call
 * delivers it and returns without blocking. */
void platform_poll(UiInput *in, bool wait);
/* Same, with an explicit timeout in seconds (0 = do not block). */
void platform_poll_timeout(UiInput *in, double timeout);
/* Wake a blocked platform_poll from another thread (e.g. a compile finished). */
void platform_post_empty_event(void);
/* Called (on the main thread, from inside platform_poll) while the OS is resizing the window, so the
 * application can redraw instead of showing a stretched frame.  Optional. */
void platform_set_refresh_callback(void (*fn)(void *user), void *user);

/* Clipboard as UTF-8.  The returned pointer is valid until the next clipboard call; may be NULL. */
const char *platform_get_clipboard(void);
void platform_set_clipboard(const char *utf8);

void platform_set_cursor(UiCursor c);

#endif
