/* gfx.h - Vulkan renderer (owner: gfx agent).
 *
 * A small Vulkan 1.0 renderer with one graphics queue and no threads. It is used
 *   - by the GUI: a swapchain on a GLFW window, viewer renders into offscreen
 *     targets, and one 2D draw list (UI + targets as textures) per frame;
 *   - by the headless CLI and the tests: synchronous renders into memory.
 *
 * Objects are small integer handles: > 0 is valid, 0 means "none" / "failed";
 * handles of destroyed objects are reused. At most 64 viewers and 64 targets.
 * Viewers are compiled fragment shaders (see "Viewer shader contract" in
 * agents/architecture.md); targets are RGBA8 offscreen images.
 *
 * Typical GUI frame:
 *     if (gfx_frame_begin()) {
 *         gfx_set_globals(state, size);                 (when the state changed)
 *         gfx_frame_render_viewer(target, viewer, &p);  (tiles that need it)
 *         gfx_frame_draw(&drawlist, clear_rgb);         (exactly once)
 *         gfx_frame_end();
 *     }
 *
 * Errors: functions with an `err` buffer write a readable message into it
 * (when err != NULL) and return false/0. Functions without one print the
 * problem to stderr and do nothing. After an unrecoverable error (device lost)
 * gfx_fatal_error() returns a message and frames are no longer rendered.
 *
 * Loader: Vulkan is loaded at runtime (libvulkan.so.1, vulkan-1.dll, or
 * MoltenVK on macOS). Environment variables:
 *     FS_GPU=<substring>         pick the GPU whose name contains it (case-insensitive)
 *     FS_VULKAN_LIBRARY=<path>   load this Vulkan library / MoltenVK dylib first
 *     FS_VULKAN_VALIDATION=1     enable the validation layer (as GfxInitDesc.validation)
 */
#ifndef FS_GFX_H
#define FS_GFX_H

#include "base.h"

/* ---- 2D draw lists (produced by ui.c, consumed here) ---------------
 * Vertices are in framebuffer pixels, origin top-left. rgba packs R in the
 * low byte (r | g<<8 | b<<16 | a<<24), straight alpha, blended with
 * src*a + dst*(1-a). Fragment color = vertex color * texture(uv).
 * Texture 0 is the font atlas, > 0 a render target. Clip rects are scissors
 * in framebuffer pixels. ui.h defines the same structs behind this guard. */
#ifndef FS_GFX_DRAWLIST_DEFINED
#define FS_GFX_DRAWLIST_DEFINED
typedef struct GfxVertex { float x, y; float u, v; u32 rgba; } GfxVertex;
typedef struct GfxCmd { u32 first_index, index_count; int texture;
                        float clip_x0, clip_y0, clip_x1, clip_y1; } GfxCmd;
typedef struct GfxDrawList { GfxVertex *verts; int nverts; u32 *indices; int nindices;
                             GfxCmd *cmds; int ncmds; } GfxDrawList;
#endif

typedef struct GfxInitDesc {
    void *glfw_window;   /* GLFWwindow*, or NULL for headless (no surface/swapchain) */
    bool vsync;
    bool validation;     /* enable VK_LAYER_KHRONOS_validation when available; print messages to stderr */
} GfxInitDesc;

typedef struct GfxViewerParams {
    float width, height;     /* push constant `size`: logical tile size in pixels */
    float offset_x, offset_y;/* push constant `offset` */
    int x0, y0, x1, y1;      /* region of the target to (re)render, in target pixels;
                                pixels outside are preserved. x1<=x0 means the whole target. */
} GfxViewerParams;

/* ---- setup ----------------------------------------------------------- */

/* Load the Vulkan library and hand it to GLFW (glfwInitVulkanLoader) so both
   use the same loader. Call before glfwInit(). False if no Vulkan library was found. */
bool gfx_preinit_loader(void);

/* Create instance, device and (with a window) the swapchain. On failure all
   partial state is released and gfx_init may be called again. */
bool gfx_init(const GfxInitDesc *desc, char *err, int errcap);
/* Wait for the GPU and destroy everything. Call before destroying the window. */
void gfx_shutdown(void);

const char *gfx_device_name(void);   /* "" before gfx_init */
int gfx_max_image_size(void);        /* largest target / render width or height */
const char *gfx_fatal_error(void);   /* NULL while healthy */
/* Validation messages of severity "error" seen so far (across init/shutdown);
   -1 when the validation layer is not active. */
int gfx_validation_errors(void);

/* ---- resources --------------------------------------------------------- */

bool gfx_set_font_atlas(const u8 *rgba, int w, int h);   /* texture 0; may be called again (rebuild) */
void gfx_set_globals(const void *data, u32 size);        /* copied; used by viewer renders recorded afterwards */

int  gfx_viewer_create(const u32 *spirv, u32 nwords, char *err, int errcap); /* >0 handle, 0 on error */
void gfx_viewer_destroy(int viewer);

int  gfx_target_create(int w, int h);  /* RGBA8 offscreen image, usable as draw-list texture id (>0);
                                          starts opaque black */
void gfx_target_destroy(int target);   /* safe while frames are in flight (deferred) */
bool gfx_target_size(int target, int *w, int *h);

/* ---- windowed frame -----------------------------------------------------
 * Order: begin, any number of render_viewer, exactly one draw, end. */
bool gfx_frame_begin(void);            /* false: skip this frame (minimized / swapchain being rebuilt) */
void gfx_frame_render_viewer(int target, int viewer, const GfxViewerParams *p);
void gfx_frame_draw(const GfxDrawList *dl, const float clear_rgb[3]);
void gfx_frame_end(void);
void gfx_framebuffer_size(int *w, int *h); /* size of the current swapchain images (0,0 headless) */

/* ---- synchronous rendering (headless or windowed) ----------------------
 * Submitted and waited for before returning. Inside a frame they still work, but
 * run before the commands recorded so far in that frame (submitted at frame_end). */

/* Render a viewer into memory (rgba8, w*h*4 bytes, rows top to bottom).
   Renders in horizontal bands of at most `band_rows` rows per submission
   (0 = whole image) to avoid GPU watchdog timeouts on slow shaders.
   Push constants: size = (w, h), offset = 0. */
bool gfx_render_to_memory(int viewer, int w, int h, int band_rows, u8 *rgba_out, char *err, int errcap);

/* Same as gfx_frame_render_viewer, but submitted and waited for immediately. */
bool gfx_target_render(int target, int viewer, const GfxViewerParams *p, char *err, int errcap);
/* Clear the target and draw a draw list into it (as gfx_frame_draw does into the
   window). Commands that sample the target itself are skipped. */
bool gfx_target_draw(int target, const GfxDrawList *dl, const float clear_rgb[3], char *err, int errcap);
/* Copy a target into memory (rgba8, w*h*4 bytes, rows top to bottom). */
bool gfx_target_read(int target, u8 *rgba_out, char *err, int errcap);

#endif
