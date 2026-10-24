/* platform.c - GLFW window and input.  See platform.h for the contract.
 *
 * Structure: the GLFW callbacks (cb_*) only convert their arguments and forward to small
 * plat_on_* functions that update the `plat` state.  Those need no window, which is how the
 * tests drive the input layer.  platform_poll() = reset per-frame state, pump GLFW, copy out. */
#include "platform.h"
#include <GLFW/glfw3.h>

#define PLAT_MAX_DROPPED 64
#define PLAT_DOUBLE_CLICK_SECONDS 0.45
#define PLAT_DOUBLE_CLICK_SLOP 6.0f /* pixels */

/* A mouse button event that could not be delivered in the frame it arrived in (see plat_button_conflict). */
typedef struct PlatButtonEvent {
    int button;
    bool press;
    double time;
} PlatButtonEvent;
#define PLAT_MAX_QUEUED 32

typedef struct PlatState {
    GLFWwindow *win;
    UiInput in;                 /* accumulates events between polls */
    double last_poll_time;
    /* Input held back to the next frame so that a press and the release of one button (or key) never share a
     * frame; otherwise a quick click, which can arrive completely inside one poll, would be invisible. */
    PlatButtonEvent queue[PLAT_MAX_QUEUED];
    int nqueue;
    bool key_pressed_edge[UI_KEY_COUNT];    /* pressed during this frame */
    bool key_release_pending[UI_KEY_COUNT]; /* released in the frame it was pressed: key_down stays set one frame */
    /* click counting */
    double last_press_time;
    float last_press_x, last_press_y;
    int click_count;
    /* files dropped this frame (owned copies) */
    char *dropped[PLAT_MAX_DROPPED];
    int ndropped;
    /* mouse cursors, created on first use */
    GLFWcursor *cursors[UI_CURSOR_COUNT];
    bool cursor_tried[UI_CURSOR_COUNT];
    UiCursor cursor;
    /* window <-> framebuffer pixel ratio, refreshed every poll */
    float fb_scale_x, fb_scale_y;
    /* fullscreen toggle remembers the windowed placement */
    int saved_x, saved_y, saved_w, saved_h;
    void (*refresh_fn)(void *user);
    void *refresh_user;
    char last_error[256];
    StrBuf fake_clipboard;      /* used when there is no window (tests) */
} PlatState;

static PlatState plat;

/* ---- key / modifier translation ------------------------------------------------------------ */

static UiKey plat_map_key(int k) {
    if (k >= GLFW_KEY_SPACE && k <= GLFW_KEY_GRAVE_ACCENT) return (UiKey)k; /* ASCII-compatible range */
    if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) return (UiKey)(UI_KEY_F1 + (k - GLFW_KEY_F1));
    if (k >= GLFW_KEY_KP_0 && k <= GLFW_KEY_KP_9) return (UiKey)(UI_KEY_0 + (k - GLFW_KEY_KP_0));
    switch (k) {
    case GLFW_KEY_LEFT: return UI_KEY_LEFT;
    case GLFW_KEY_RIGHT: return UI_KEY_RIGHT;
    case GLFW_KEY_UP: return UI_KEY_UP;
    case GLFW_KEY_DOWN: return UI_KEY_DOWN;
    case GLFW_KEY_HOME: return UI_KEY_HOME;
    case GLFW_KEY_END: return UI_KEY_END;
    case GLFW_KEY_PAGE_UP: return UI_KEY_PAGE_UP;
    case GLFW_KEY_PAGE_DOWN: return UI_KEY_PAGE_DOWN;
    case GLFW_KEY_INSERT: return UI_KEY_INSERT;
    case GLFW_KEY_BACKSPACE: return UI_KEY_BACKSPACE;
    case GLFW_KEY_DELETE: return UI_KEY_DELETE;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: return UI_KEY_ENTER;
    case GLFW_KEY_TAB: return UI_KEY_TAB;
    case GLFW_KEY_ESCAPE: return UI_KEY_ESCAPE;
    case GLFW_KEY_KP_DECIMAL: return UI_KEY_PERIOD;
    case GLFW_KEY_KP_DIVIDE: return UI_KEY_SLASH;
    case GLFW_KEY_KP_SUBTRACT: return UI_KEY_MINUS;
    case GLFW_KEY_KP_ADD: case GLFW_KEY_KP_EQUAL: return UI_KEY_EQUAL; /* so Mod+= / Mod+- zoom shortcuts work on the keypad */
    default: return UI_KEY_NONE;
    }
}

static int plat_map_mods(int m) {
    int r = 0;
    if (m & GLFW_MOD_SHIFT) r |= UI_MOD_SHIFT;
    if (m & GLFW_MOD_CONTROL) r |= UI_MOD_CTRL;
    if (m & GLFW_MOD_ALT) r |= UI_MOD_ALT;
    if (m & GLFW_MOD_SUPER) r |= UI_MOD_SUPER;
    return r;
}

/* ---- event handlers (no window needed) ------------------------------------------------------ */

static void plat_push_key(UiKeyEvent e) {
    if (plat.in.nkeys < ARRAY_COUNT(plat.in.keys)) plat.in.keys[plat.in.nkeys++] = e;
}

static void plat_on_key(int glfw_key, int action, int glfw_mods) {
    plat.in.mods = plat_map_mods(glfw_mods); /* modifier-only presses update `mods` too */
    UiKey k = plat_map_key(glfw_key);
    if (k == UI_KEY_NONE) return;
    if (action == GLFW_RELEASE) {
        if (plat.key_pressed_edge[k]) plat.key_release_pending[k] = true; /* a tap inside one poll: show it down once */
        else plat.in.key_down[k] = false;
        return;
    }
    plat.key_release_pending[k] = false; /* pressed again before the held-back release was delivered */
    plat.key_pressed_edge[k] |= action == GLFW_PRESS;
    plat.in.key_down[k] = true;
    UiKeyEvent e = { k, plat.in.mods, action == GLFW_REPEAT, 0 };
    plat_push_key(e);
}

static void plat_on_char(u32 cp) {
    int m = plat.in.mods;
    /* Ctrl+letter (and Cmd+letter) are shortcuts, not text.  Ctrl+Alt is AltGr on Windows: that is text. */
    if (((m & UI_MOD_CTRL) && !(m & UI_MOD_ALT)) || (m & UI_MOD_SUPER)) return;
    if (cp < 32 || (cp >= 127 && cp < 160)) return;
    if (plat.in.ntext < ARRAY_COUNT(plat.in.text)) plat.in.text[plat.in.ntext++] = cp;
    UiKeyEvent e = { UI_KEY_NONE, m, false, cp };
    plat_push_key(e);
}

/* Would delivering this event in the current frame put a press and a release of the button into one frame? */
static bool plat_button_conflict(int button, bool press) {
    return press ? plat.in.mouse_released[button] : plat.in.mouse_pressed[button];
}

static void plat_apply_button(const PlatButtonEvent *e) {
    int b = e->button;
    if (e->press) {
        plat.in.mouse_down[b] = true;
        plat.in.mouse_pressed[b] = true;
        if (b == 0) {
            float x = plat.in.mouse_x, y = plat.in.mouse_y;
            bool near_ = fabsf(x - plat.last_press_x) <= PLAT_DOUBLE_CLICK_SLOP &&
                         fabsf(y - plat.last_press_y) <= PLAT_DOUBLE_CLICK_SLOP;
            if (plat.click_count > 0 && near_ && e->time - plat.last_press_time <= PLAT_DOUBLE_CLICK_SECONDS)
                plat.click_count = plat.click_count % 3 + 1; /* 1 -> 2 -> 3 -> 1 */
            else
                plat.click_count = 1;
            plat.last_press_time = e->time;
            plat.last_press_x = x;
            plat.last_press_y = y;
            plat.in.click_count = plat.click_count;
        }
    } else {
        plat.in.mouse_down[b] = false;
        plat.in.mouse_released[b] = true;
    }
}

/* Deliver now, or hold back (in order) for the next frame when a press and a release would share this one. */
static void plat_deliver_button(const PlatButtonEvent *e) {
    if ((plat.nqueue > 0 || plat_button_conflict(e->button, e->press)) && plat.nqueue < PLAT_MAX_QUEUED) {
        plat.queue[plat.nqueue++] = *e;
        return;
    }
    plat_apply_button(e);
}

static void plat_on_mouse_button(int button, int action, int glfw_mods, double now) {
    plat.in.mods = plat_map_mods(glfw_mods);
    if ((button == GLFW_MOUSE_BUTTON_4 || button == GLFW_MOUSE_BUTTON_5) && action == GLFW_PRESS) { /* side buttons */
        if (button == GLFW_MOUSE_BUTTON_4) plat.in.mouse_back = true;
        else plat.in.mouse_forward = true;
        return;
    }
    if (button < 0 || button > 2 || (action != GLFW_PRESS && action != GLFW_RELEASE)) return;
    PlatButtonEvent e = { button, action == GLFW_PRESS, now };
    plat_deliver_button(&e);
}

static void plat_on_cursor(float fb_x, float fb_y) {
    plat.in.mouse_x = fb_x;
    plat.in.mouse_y = fb_y;
}

static void plat_on_scroll(float dx, float dy) {
    plat.in.scroll_x += dx;
    plat.in.scroll_y += dy;
}

static void plat_on_drop(int n, const char **paths) {
    for (int i = 0; i < n && plat.ndropped < PLAT_MAX_DROPPED; i++) plat.dropped[plat.ndropped++] = xstrdup(paths[i]);
}

static void plat_on_focus(bool focused) {
    plat.in.focused = focused;
    if (!focused) { /* we will never see the releases: forget everything that is held */
        plat.nqueue = 0;
        memset(plat.key_release_pending, 0, sizeof plat.key_release_pending);
        for (int i = 0; i < 3; i++) {
            PlatButtonEvent e = { i, false, 0 };
            if (plat.in.mouse_down[i]) plat_deliver_button(&e);
        }
        memset(plat.in.key_down, 0, sizeof plat.in.key_down);
        plat.in.mods = 0;
    }
}

/* Start of a frame: forget last frame's edges and events (the held state stays), then deliver the input that was
 * held back.  Returns true if there was any (delivered now, or still waiting for a later frame): the caller must
 * not sleep before handing this frame out. */
static bool plat_begin_frame(void) {
    UiInput *p = &plat.in;
    memset(p->mouse_pressed, 0, sizeof p->mouse_pressed);
    memset(p->mouse_released, 0, sizeof p->mouse_released);
    p->scroll_x = p->scroll_y = 0;
    p->mouse_back = p->mouse_forward = false;
    p->nkeys = 0;
    p->ntext = 0;
    for (int i = 0; i < plat.ndropped; i++) free(plat.dropped[i]);
    plat.ndropped = 0;

    memset(plat.key_pressed_edge, 0, sizeof plat.key_pressed_edge);
    bool held_back = plat.nqueue > 0;
    for (int k = 0; k < UI_KEY_COUNT; k++) {
        if (!plat.key_release_pending[k]) continue;
        plat.key_release_pending[k] = false;
        p->key_down[k] = false;
        held_back = true;
    }
    int n = 0; /* queued clicks: as many as fit without a press and a release of one button sharing the frame */
    while (n < plat.nqueue && !plat_button_conflict(plat.queue[n].button, plat.queue[n].press)) plat_apply_button(&plat.queue[n++]);
    memmove(plat.queue, plat.queue + n, sizeof plat.queue[0] * (size_t)(plat.nqueue - n));
    plat.nqueue -= n;
    return held_back;
}

/* End of a frame: complete the fields only known at poll time and hand the snapshot out. */
static void plat_end_frame(UiInput *out, double now) {
    UiInput *p = &plat.in;
    if (plat.win) {
        int ww = 0, wh = 0, fw = 0, fh = 0;
        float sx = 1, sy = 1;
        glfwGetWindowSize(plat.win, &ww, &wh);
        glfwGetFramebufferSize(plat.win, &fw, &fh);
        glfwGetWindowContentScale(plat.win, &sx, &sy);
        p->fb_w = fw;
        p->fb_h = fh;
        p->content_scale = sx > 0 ? sx : 1.0f;
        plat.fb_scale_x = ww > 0 ? (float)fw / (float)ww : 1.0f;
        plat.fb_scale_y = wh > 0 ? (float)fh / (float)wh : 1.0f;
        p->focused = glfwGetWindowAttrib(plat.win, GLFW_FOCUSED) != 0;
    }
    if (p->content_scale <= 0) p->content_scale = 1.0f;
    p->time = now;
    p->dt = plat.last_poll_time > 0 ? (float)(now - plat.last_poll_time) : 1.0f / 60.0f;
    if (p->dt < 0) p->dt = 0;
    plat.last_poll_time = now;
    p->dropped_files = plat.dropped;
    p->ndropped = plat.ndropped;
    *out = *p;
}

/* ---- GLFW callbacks -------------------------------------------------------------------------- */

static void cb_error(int code, const char *desc) {
    snprintf(plat.last_error, sizeof plat.last_error, "%s", desc ? desc : "unknown GLFW error");
}
static void cb_key(GLFWwindow *w, int key, int scancode, int action, int mods) { plat_on_key(key, action, mods); }
static void cb_char(GLFWwindow *w, unsigned int cp) { plat_on_char(cp); }
static void cb_mouse_button(GLFWwindow *w, int button, int action, int mods) {
    plat_on_mouse_button(button, action, mods, fs_time_seconds());
}
static void cb_cursor_pos(GLFWwindow *w, double x, double y) {
    plat_on_cursor((float)x * plat.fb_scale_x, (float)y * plat.fb_scale_y);
}
static void cb_scroll(GLFWwindow *w, double dx, double dy) { plat_on_scroll((float)dx, (float)dy); }
static void cb_drop(GLFWwindow *w, int n, const char **paths) { plat_on_drop(n, paths); }
static void cb_cursor_enter(GLFWwindow *w, int entered) { plat.in.mouse_in_window = entered != 0; }
static void cb_focus(GLFWwindow *w, int focused) { plat_on_focus(focused != 0); }
static void cb_refresh(GLFWwindow *w) {
    if (plat.refresh_fn) plat.refresh_fn(plat.refresh_user);
}

/* ---- window ---------------------------------------------------------------------------------- */

static void plat_set_err(char *err, int errcap, const char *what) {
    if (!err || errcap <= 0) return;
    snprintf(err, (size_t)errcap, "%s%s%s", what, plat.last_error[0] ? ": " : "", plat.last_error);
}

bool platform_init(int w, int h, const char *title, char *err, int errcap) {
    if (plat.win) return true;
    memset(&plat, 0, sizeof plat);
    plat.fb_scale_x = plat.fb_scale_y = 1.0f;
    plat.in.content_scale = 1.0f;
    plat.in.focused = true;
    glfwSetErrorCallback(cb_error);
    /* macOS: GLFW would make the .app's Resources folder the working directory, and a script named on the
       command line (relative to where the program started) would no longer be found or saved there */
    glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
    if (!glfwInit()) {
        plat_set_err(err, errcap, "cannot initialize the window system");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE); /* Windows/X11: a "1280 wide" window is 1280 logical pixels */
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "fractalstreamc");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "fractalstreamc");
    plat.last_error[0] = 0;
    plat.win = glfwCreateWindow(w, h, title ? title : "FractalStreamC", NULL, NULL);
    if (!plat.win) {
        plat_set_err(err, errcap, "cannot create the window");
        glfwTerminate();
        return false;
    }
    glfwSetKeyCallback(plat.win, cb_key);
    glfwSetCharCallback(plat.win, cb_char);
    glfwSetMouseButtonCallback(plat.win, cb_mouse_button);
    glfwSetCursorPosCallback(plat.win, cb_cursor_pos);
    glfwSetScrollCallback(plat.win, cb_scroll);
    glfwSetDropCallback(plat.win, cb_drop);
    glfwSetCursorEnterCallback(plat.win, cb_cursor_enter);
    glfwSetWindowFocusCallback(plat.win, cb_focus);
    glfwSetWindowRefreshCallback(plat.win, cb_refresh);

    int ww = 1, wh = 1, fw = 1, fh = 1;
    glfwGetWindowSize(plat.win, &ww, &wh);
    glfwGetFramebufferSize(plat.win, &fw, &fh);
    plat.fb_scale_x = (float)fw / (float)(ww > 0 ? ww : 1);
    plat.fb_scale_y = (float)fh / (float)(wh > 0 ? wh : 1);
    double mx = 0, my = 0;
    glfwGetCursorPos(plat.win, &mx, &my);
    plat_on_cursor((float)mx * plat.fb_scale_x, (float)my * plat.fb_scale_y);
    plat.in.mouse_in_window = glfwGetWindowAttrib(plat.win, GLFW_HOVERED) != 0;
    plat.in.focused = glfwGetWindowAttrib(plat.win, GLFW_FOCUSED) != 0;
    plat.cursor = UI_CURSOR_ARROW;
    return true;
}

void platform_shutdown(void) {
    for (int i = 0; i < plat.ndropped; i++) free(plat.dropped[i]);
    plat.ndropped = 0;
    sb_free(&plat.fake_clipboard);
    if (!plat.win) return;
    for (int i = 0; i < UI_CURSOR_COUNT; i++)
        if (plat.cursors[i]) glfwDestroyCursor(plat.cursors[i]);
    glfwDestroyWindow(plat.win);
    glfwTerminate();
    memset(&plat, 0, sizeof plat);
}

void *platform_glfw_window(void) { return plat.win; }
bool platform_should_close(void) { return plat.win ? glfwWindowShouldClose(plat.win) != 0 : true; }
void platform_set_should_close(bool close) { if (plat.win) glfwSetWindowShouldClose(plat.win, close ? GLFW_TRUE : GLFW_FALSE); }
void platform_set_title(const char *title) { if (plat.win) glfwSetWindowTitle(plat.win, title ? title : ""); }
void platform_set_size(int w, int h) { if (plat.win) glfwSetWindowSize(plat.win, w, h); }
void platform_get_size(int *w, int *h) {
    int ww = 0, wh = 0;
    if (plat.win) glfwGetWindowSize(plat.win, &ww, &wh);
    if (w) *w = ww;
    if (h) *h = wh;
}

bool platform_is_fullscreen(void) { return plat.win && glfwGetWindowMonitor(plat.win) != NULL; }

/* The monitor that contains most of the window (a projector, say), falling back to the primary one. */
static GLFWmonitor *plat_window_monitor(void) {
    int wx, wy, ww, wh, n = 0;
    glfwGetWindowPos(plat.win, &wx, &wy);
    glfwGetWindowSize(plat.win, &ww, &wh);
    GLFWmonitor **mons = glfwGetMonitors(&n);
    GLFWmonitor *best = glfwGetPrimaryMonitor();
    long best_area = 0;
    for (int i = 0; i < n; i++) {
        const GLFWvidmode *vm = glfwGetVideoMode(mons[i]);
        int mx, my;
        if (!vm) continue;
        glfwGetMonitorPos(mons[i], &mx, &my);
        int ox = FS_MIN(wx + ww, mx + vm->width) - FS_MAX(wx, mx);
        int oy = FS_MIN(wy + wh, my + vm->height) - FS_MAX(wy, my);
        long area = ox > 0 && oy > 0 ? (long)ox * oy : 0;
        if (area > best_area) { best_area = area; best = mons[i]; }
    }
    return best;
}

void platform_toggle_fullscreen(void) {
    if (!plat.win) return;
    if (glfwGetWindowMonitor(plat.win)) {
        glfwSetWindowMonitor(plat.win, NULL, plat.saved_x, plat.saved_y, plat.saved_w, plat.saved_h, 0);
        return;
    }
    GLFWmonitor *m = plat_window_monitor();
    const GLFWvidmode *vm = m ? glfwGetVideoMode(m) : NULL;
    if (!vm) return;
    glfwGetWindowPos(plat.win, &plat.saved_x, &plat.saved_y);
    glfwGetWindowSize(plat.win, &plat.saved_w, &plat.saved_h);
    glfwSetWindowMonitor(plat.win, m, 0, 0, vm->width, vm->height, vm->refreshRate);
}

/* ---- polling ----------------------------------------------------------------------------------- */

void platform_poll_timeout(UiInput *in, double timeout) {
    bool held_back = plat_begin_frame(); /* input that arrived last time but had to wait: do not sleep on it */
    if (plat.win) {
        if (timeout > 0 && !held_back) glfwWaitEventsTimeout(timeout);
        else glfwPollEvents();
    }
    plat_end_frame(in, fs_time_seconds());
}

void platform_poll(UiInput *in, bool wait) { platform_poll_timeout(in, wait ? 0.5 : 0.0); }

void platform_post_empty_event(void) { if (plat.win) glfwPostEmptyEvent(); }

void platform_set_refresh_callback(void (*fn)(void *user), void *user) {
    plat.refresh_fn = fn;
    plat.refresh_user = user;
}

/* ---- clipboard and cursor ----------------------------------------------------------------------- */

const char *platform_get_clipboard(void) {
    if (plat.win) return glfwGetClipboardString(plat.win);
    return plat.fake_clipboard.data; /* headless (tests): an in-process clipboard */
}

void platform_set_clipboard(const char *utf8) {
    if (!utf8) utf8 = "";
    if (plat.win) { glfwSetClipboardString(plat.win, utf8); return; }
    sb_clear(&plat.fake_clipboard);
    sb_puts(&plat.fake_clipboard, utf8);
}

static int plat_cursor_shape(UiCursor c) {
    switch (c) {
    case UI_CURSOR_IBEAM: return GLFW_IBEAM_CURSOR;
    case UI_CURSOR_HAND: return GLFW_POINTING_HAND_CURSOR;
    case UI_CURSOR_HRESIZE: return GLFW_RESIZE_EW_CURSOR;
    case UI_CURSOR_VRESIZE: return GLFW_RESIZE_NS_CURSOR;
    case UI_CURSOR_MOVE: return GLFW_RESIZE_ALL_CURSOR;
    case UI_CURSOR_CROSSHAIR: return GLFW_CROSSHAIR_CURSOR;
    default: return GLFW_ARROW_CURSOR;
    }
}

void platform_set_cursor(UiCursor c) {
    if (!plat.win || (int)c < 0 || c >= UI_CURSOR_COUNT) return;
    if (!plat.cursor_tried[c]) { /* some systems lack some shapes: then we silently keep the arrow */
        plat.cursor_tried[c] = true;
        plat.cursors[c] = glfwCreateStandardCursor(plat_cursor_shape(c));
    }
    if (c == plat.cursor) return;
    plat.cursor = c;
    glfwSetCursor(plat.win, plat.cursors[c]); /* NULL = default arrow */
}
