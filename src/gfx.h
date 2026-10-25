/* gfx.h - Vulkan renderer.
 *
 * A small Vulkan 1.0 renderer with one graphics queue and no threads. It is used
 *   - by the GUI: a swapchain on a GLFW window, viewer renders into offscreen
 *     targets, and one 2D draw list (UI + targets as textures) per frame;
 *   - by the headless CLI and the tests: synchronous renders into memory.
 *
 * Objects are small integer handles: > 0 is valid, 0 means "none" / "failed";
 * handles of destroyed objects are reused. At most 64 viewers and 64 targets.
 * Viewers are compiled fragment shaders (see "Viewer shader contract" below);
 * targets are RGBA8 offscreen images.
 *
 * Typical GUI frame:
 *     if (gfx_frame_begin()) {
 *         gfx_set_globals(state, size);                 (when the state changed)
 *         gfx_frame_render_viewer(target, viewer, &p);  (tiles that need it)
 *         gfx_frame_dispatch(...), gfx_frame_buffer_to_target(...)  (compute, see below)
 *         gfx_frame_draw(&drawlist, clear_rgb);         (exactly once)
 *         gfx_frame_end();
 *     }
 *
 * Viewer shader contract (codegen.c produces, gfx.c consumes). A viewer is a
 * SPIR-V 1.0 module with a Fragment entry point `main`, OriginUpperLeft:
 *   - input: gl_FragCoord; output: a vec4 at Location 0 (rgb clamped to [0, 1], a = 1);
 *   - set 0, binding 0: a read-only storage buffer (Block, std430) with the program's
 *     global state; its layout is private to the compiler, gfx just uploads bytes;
 *   - push constants (16 bytes, fragment stage): vec2 size at offset 0 (the logical
 *     tile size in pixels, which maps pixels to the plane), vec2 offset at offset 8
 *     (added to gl_FragCoord.xy, for sub-rectangles and supersampling);
 *   - capabilities: Shader only; extended instructions: GLSL.std.450 only. Deep zooms
 *     keep this interface (double-single arithmetic inside the shader); when the GPU
 *     keeps exact floats they also declare SPV_KHR_float_controls (capability
 *     SignedZeroInfNanPreserve, device extension VK_KHR_shader_float_controls).
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
    float offset_x, offset_y;/* push constant `offset` (a sub-pixel offset: another sample in each pixel) */
    int x0, y0, x1, y1;      /* region of the target to (re)render, in target pixels;
                                pixels outside are preserved. x1<=x0 means the whole target. */
    float blend;             /* the weight w of this render: pixel = w * new + (1 - w) * old. 0 (the default) or 1
                                replaces the pixels; 1/(k+1) averages a (k+1)-th sample into k (supersampling) */
} GfxViewerParams;

/* ---- setup ----------------------------------------------------------- */

/* Load the Vulkan library and hand it to GLFW (glfwInitVulkanLoader) so both
   use the same loader. Call before glfwInit(). False if no Vulkan library was found. */
bool gfx_preinit_loader(void);
/* After gfx_preinit_loader failed: why a Vulkan library file that is there could not be loaded
   (the system's message, e.g. a MoltenVK for another processor), or "" */
const char *gfx_library_error(void);

/* Create instance, device and (with a window) the swapchain. On failure all
   partial state is released and gfx_init may be called again. */
bool gfx_init(const GfxInitDesc *desc, char *err, int errcap);
/* Wait for the GPU and destroy everything. Call before destroying the window. */
void gfx_shutdown(void);

const char *gfx_device_name(void);   /* "" before gfx_init */
bool gfx_has_float_controls(void);    /* shaders can ask for IEEE floats (SPV_KHR_float_controls SignedZeroInfNanPreserve) */
int gfx_max_image_size(void);        /* largest target / render width or height */
float gfx_frame_gpu_ms(void);       /* GPU time of the views' work (renders, fills) of the latest finished frame,
                                       in ms; -1 when the GPU doesn't tell (no timestamps) */
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
 * Order: begin, any number of render_viewer and compute commands (see "compute"
 * below), exactly one draw, end. */
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

/* ---- compute -------------------------------------------------------------
 * Storage buffers, compute pipelines, and commands recorded into the current
 * frame (between gfx_frame_begin and gfx_frame_draw) or into a batch (between
 * gfx_batch_begin and gfx_batch_end; for the CLI and tests, also headless).
 * Each recorded command ends with the barrier that orders it before every later
 * command and makes its writes visible to them, so the commands take effect in
 * the order of the calls and a frame may chain thousands of small dispatches that
 * read what earlier ones wrote.
 *
 * Compute shader contract: SPIR-V 1.0, GLCompute entry point "main" with its
 * LocalSize in the module; set 0, binding 0 = the globals (as for viewers:
 * gfx_set_globals, read-only); bindings 1..nbuffers = storage buffers
 * (std430, read/write); optional push constant block of push_bytes bytes.
 * At most 128 storage buffers and 64 compute pipelines; handles as for targets. */

/* Device-local storage buffer, zero-filled, usable as a copy source and
   destination. `bytes` is rounded up to a multiple of 4 (see gfx_buffer_size)
   and must be <= gfx_max_buffer_size(). >0 handle, 0 on error (message on stderr). */
int  gfx_buffer_create(u64 bytes);
void gfx_buffer_destroy(int buf);    /* safe while frames are in flight (deferred) */
u64  gfx_buffer_size(int buf);       /* bytes (a multiple of 4); 0 for an invalid handle */
u64  gfx_max_buffer_size(void);      /* the GPU's maxStorageBufferRange (may be 128 MiB), 0 before gfx_init */
/* Synchronous copies between a buffer and memory. They happen after all work
   submitted so far (a download sees its results), but before the commands of a
   frame or batch that is still being recorded. */
bool gfx_buffer_upload(int buf, u64 offset, const void *data, u64 bytes, char *err, int errcap);
bool gfx_buffer_download(int buf, u64 offset, void *data, u64 bytes, char *err, int errcap);

typedef struct GfxComputeDesc {
    const u32 *spirv; u32 nwords;  /* SPIR-V 1.0 compute shader (see the contract above) */
    int nbuffers;                  /* storage buffers at bindings 1..nbuffers, 0..8 */
    u32 push_bytes;                /* push constant block (compute stage): 0..128, a multiple of 4 */
} GfxComputeDesc;
int  gfx_compute_create(const GfxComputeDesc *d, char *err, int errcap); /* >0 handle, 0 on error */
void gfx_compute_destroy(int pipe);  /* safe while frames are in flight (deferred) */

/* Recording (into the open batch, else into the current frame before gfx_frame_draw).
   Invalid arguments skip the command: in a frame with a message on stderr, in a
   batch they make gfx_batch_end fail. Offsets and sizes are in bytes. */
void gfx_frame_fill_buffer(int buf, u64 offset, u64 bytes, u32 value); /* offset, bytes: multiples of 4 */
/* Binds bufs[0..nbuffers-1] at bindings 1..nbuffers, the globals current at this
   call at binding 0, pushes push_bytes from `push` (NULL = zeros), and runs
   nx*ny*nz workgroups (none if any is 0). */
void gfx_frame_dispatch(int pipe, const int *bufs, const void *push, u32 nx, u32 ny, u32 nz);
/* Copies between a target and tightly packed RGBA8 rows (w*h*4 bytes, top row first,
   R in the lowest byte) at `offset` (a multiple of 4) in a buffer. The target can be
   drawn afterwards as usual; target_to_buffer can keep the previous picture. */
void gfx_frame_buffer_to_target(int buf, u64 offset, int target);
void gfx_frame_target_to_buffer(int target, int buf, u64 offset);

/* A batch: the same recording without a window. Not inside a frame (and no frame
   can begin while a batch is open). Globals as set by gfx_set_globals before each
   dispatch. Only the four calls above record into a batch (viewer renders and
   draws do not: use gfx_target_render / gfx_target_draw). gfx_batch_end submits,
   waits, and reports the first recording error. */
bool gfx_batch_begin(void);          /* false (message on stderr) if not possible */
bool gfx_batch_end(char *err, int errcap);

#endif
