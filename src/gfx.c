/* gfx.c - Vulkan renderer. API: gfx.h. Contracts: agents/architecture.md.
 *
 * Sections:
 *    1. Vulkan library loading and function pointers
 *    2. state, errors, memory / buffer / texture helpers
 *    3. deferred destruction ("garbage") and synchronous submissions
 *    4. fixed objects: render passes, layouts, pipelines
 *    5. instance and device (gfx_init / gfx_shutdown)
 *    6. swapchain
 *    7. resources: font atlas, globals, viewers, targets
 *    8. command recording (viewer renders, draw lists, readback)
 *    9. windowed frames
 *   10. synchronous API (target render/draw/read, render to memory)
 *   11. compute: storage buffers, compute pipelines, recording, batches
 *
 * Synchronization model (read this before changing anything):
 *   - One queue. Submissions are numbered (gx.submitted counts them). A fence signal
 *     covers all work submitted earlier to the same queue, so when the fence of
 *     submission S has been waited on, every submission <= S is complete: gx.completed = S.
 *   - Objects the GPU may still use are not destroyed right away but queued in
 *     gx.garbage with the number of the last submission that may use them. Objects
 *     queued while a frame is being recorded get GFX_SERIAL_PENDING, replaced by the
 *     frame's number when it is submitted. gfx_collect_garbage() destroys what is done.
 *   - Targets are in SHADER_READ_ONLY_OPTIMAL whenever no command is using them: they
 *     are cleared to opaque black at creation, render passes move them to
 *     COLOR_ATTACHMENT_OPTIMAL and back, readback moves them to TRANSFER_SRC and back.
 *     The external dependencies of the render passes order color writes against
 *     shader reads / copies (both ways), within a command buffer and across frames.
 *   - Up to GFX_FRAMES frames in flight. Each frame slot owns a command buffer, fence,
 *     acquire semaphore, descriptor pool and globals / vertex / index buffers, reused
 *     only after waiting for the slot's fence. Each swapchain image owns the semaphore
 *     that its present waits on.
 *   - Synchronous work (uploads, target init, gfx_target_*, gfx_render_to_memory) uses
 *     its own command buffer, fence and buffers and waits before returning.
 *   - A batch (gfx_batch_begin/end) is a third recording context, like a frame slot
 *     without a swapchain: own command buffer, fence, globals buffer and descriptor
 *     pools; it is submitted and waited for at gfx_batch_end. At most one of "a frame"
 *     and "a batch" is being recorded at a time; objects released meanwhile get
 *     GFX_SERIAL_PENDING until that recording is submitted.
 *   - Storage buffers are only accessed by compute shaders and transfers. Every
 *     recorded compute/transfer command ends with a barrier that makes its writes
 *     visible to (and orders it before) all later compute and transfer commands, in
 *     this and later submissions. Synchronous uploads/downloads start with a barrier
 *     against earlier work and end with the same kind of barrier.
 */
#include "gfx.h"
#include "shaders_spv.h"
#include <ctype.h>

#ifndef VK_NO_PROTOTYPES
#error "gfx.c expects VK_NO_PROTOTYPES and the GLFW/Vulkan headers (see src/all.c)"
#endif

#if FS_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if FS_OS_MACOS
#include <limits.h>
#include <mach-o/dyld.h>
#endif

#define GFX_FRAMES 2                 /* frames in flight */
#define GFX_MAX_TARGETS 64
#define GFX_MAX_VIEWERS 64
#define GFX_MAX_BUFFERS 128          /* storage buffers (gfx_buffer_create) */
#define GFX_MAX_COMPUTES 64          /* compute pipelines (gfx_compute_create) */
#define GFX_COMPUTE_MAX_BUFFERS 8    /* storage buffers per compute pipeline (bindings 1..8) */
#define GFX_COMPUTE_POOL_SETS 64     /* descriptor sets per compute descriptor pool */
#define GFX_TRANSFER_CHUNK ((VkDeviceSize)16 << 20) /* staging bytes per upload/download submission */
#define GFX_MAX_SWAPCHAIN_IMAGES 16
#define GFX_FRAME_GLOBAL_SETS 128    /* gfx_set_globals + render pairs per frame */
#define GFX_TEXTURE_SETS (4 * GFX_MAX_TARGETS + 8) /* live + not yet collected */
#define GFX_SERIAL_PENDING UINT64_MAX
#define GFX_TARGET_FORMAT VK_FORMAT_R8G8B8A8_UNORM

/* The 2D pipeline's vertex input (gfx_pipeline) assumes this layout. */
_Static_assert(sizeof(GfxVertex) == 20 && offsetof(GfxVertex, u) == 8 && offsetof(GfxVertex, rgba) == 16,
               "GfxVertex layout changed: update the vertex attributes in gfx_pipeline");

/* ======================================================================
 * 1. Vulkan library loading and function pointers
 * ====================================================================== */

/* The loaded library stays loaded until the process exits: GLFW keeps a pointer
   to its vkGetInstanceProcAddr (see gfx_preinit_loader). */
static void *gfx_lib;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

/* Function lists. Each entry becomes a static function pointer with the Vulkan name,
   so the code below reads like ordinary Vulkan code. */
#define GFX_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define GFX_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define GFX_SURFACE_FUNCS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define GFX_DEBUG_FUNCS(X) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)
#define GFX_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdFillBuffer) X(vkCmdCopyBuffer)
#define GFX_SWAPCHAIN_FUNCS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define GFX_DECLARE_FN(name) static PFN_##name name;
GFX_GLOBAL_FUNCS(GFX_DECLARE_FN)
GFX_INSTANCE_FUNCS(GFX_DECLARE_FN)
GFX_SURFACE_FUNCS(GFX_DECLARE_FN)
GFX_DEBUG_FUNCS(GFX_DECLARE_FN)
GFX_DEVICE_FUNCS(GFX_DECLARE_FN)
GFX_SWAPCHAIN_FUNCS(GFX_DECLARE_FN)
#undef GFX_DECLARE_FN

/* Each loader returns NULL on success or the name of the first missing function. */
#define GFX_LOAD_FN(name, getter, handle) \
    name = (PFN_##name)getter(handle, #name); if (!name) return #name;
static const char *gfx_load_global_funcs(void) {
#define X(name) GFX_LOAD_FN(name, vkGetInstanceProcAddr, VK_NULL_HANDLE)
    GFX_GLOBAL_FUNCS(X)
#undef X
    return NULL;
}
static const char *gfx_load_instance_funcs(VkInstance inst) {
#define X(name) GFX_LOAD_FN(name, vkGetInstanceProcAddr, inst)
    GFX_INSTANCE_FUNCS(X)
#undef X
    return NULL;
}
static const char *gfx_load_surface_funcs(VkInstance inst) {
#define X(name) GFX_LOAD_FN(name, vkGetInstanceProcAddr, inst)
    GFX_SURFACE_FUNCS(X)
#undef X
    return NULL;
}
static const char *gfx_load_debug_funcs(VkInstance inst) {
#define X(name) GFX_LOAD_FN(name, vkGetInstanceProcAddr, inst)
    GFX_DEBUG_FUNCS(X)
#undef X
    return NULL;
}
static const char *gfx_load_device_funcs(VkDevice dev) {
#define X(name) GFX_LOAD_FN(name, vkGetDeviceProcAddr, dev)
    GFX_DEVICE_FUNCS(X)
#undef X
    return NULL;
}
static const char *gfx_load_swapchain_funcs(VkDevice dev) {
#define X(name) GFX_LOAD_FN(name, vkGetDeviceProcAddr, dev)
    GFX_SWAPCHAIN_FUNCS(X)
#undef X
    return NULL;
}
#undef GFX_LOAD_FN

static char gfx_lib_error[600]; /* why the first library file that exists did not load (gfx_library_error) */

static bool gfx_try_library(const char *path) {
#if FS_OS_WINDOWS
    HMODULE lib = LoadLibraryA(path);
    if (!lib) return false;
    PFN_vkGetInstanceProcAddr fn =
        (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!fn) { FreeLibrary(lib); return false; }
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char *why = dlerror(); /* (a file that is there but can't be used: wrong architecture, damaged) */
        if (path[0] == '/' && !gfx_lib_error[0] && access(path, F_OK) == 0)
            snprintf(gfx_lib_error, sizeof gfx_lib_error, "%s", why ? why : path);
        return false;
    }
    void *sym = dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkGetInstanceProcAddr fn;
    memcpy(&fn, &sym, sizeof fn);   /* POSIX: dlsym results convert to function pointers */
    if (!fn) { dlclose(lib); return false; }
#endif
    gfx_lib = (void *)lib;
    vkGetInstanceProcAddr = fn;
    return true;
}

#if FS_OS_MACOS
/* Directory of the running executable (resolving symlinks), "" if unknown. */
static void gfx_exe_dir(char *out, size_t cap) {
    char buf[4096], real[PATH_MAX];
    uint32_t size = sizeof buf;
    out[0] = 0;
    if (_NSGetExecutablePath(buf, &size) != 0) return;
    snprintf(out, cap, "%s", realpath(buf, real) ? real : buf);
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
}
#endif

static const char *gfx_library_names(void) {
#if FS_OS_WINDOWS
    return "vulkan-1.dll";
#elif FS_OS_MACOS
    return "libMoltenVK.dylib / libvulkan.1.dylib";
#else
    return "libvulkan.so.1";
#endif
}

static bool gfx_load_library(void) {
    if (vkGetInstanceProcAddr) return true;
    const char *env = getenv("FS_VULKAN_LIBRARY");
    if (env && *env && gfx_try_library(env)) return true;
#if FS_OS_WINDOWS
    return gfx_try_library("vulkan-1.dll");
#elif FS_OS_MACOS
    /* A bundled MoltenVK (next to the binary or in the .app's Frameworks) is used
       directly as the Vulkan implementation; otherwise a system loader / MoltenVK. */
    static const char *next_to_exe[] = {
        "libMoltenVK.dylib", "../Frameworks/libMoltenVK.dylib", "../Frameworks/libvulkan.1.dylib",
    };
    static const char *system_libs[] = {
        "libvulkan.1.dylib", "libMoltenVK.dylib",
        "/usr/local/lib/libvulkan.1.dylib", "/usr/local/lib/libMoltenVK.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib", "/opt/homebrew/lib/libMoltenVK.dylib",
    };
    char dir[4096], path[4200];
    gfx_exe_dir(dir, sizeof dir);
    for (int i = 0; dir[0] && i < ARRAY_COUNT(next_to_exe); i++) {
        snprintf(path, sizeof path, "%s/%s", dir, next_to_exe[i]);
        if (gfx_try_library(path)) return true;
    }
    for (int i = 0; i < ARRAY_COUNT(system_libs); i++)
        if (gfx_try_library(system_libs[i])) return true;
    return false;
#else
    return gfx_try_library("libvulkan.so.1") || gfx_try_library("libvulkan.so");
#endif
}

const char *gfx_library_error(void) { return gfx_lib_error; }

bool gfx_preinit_loader(void) {
    if (!gfx_load_library()) return false;
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    return true;
}

/* ======================================================================
 * 2. State, errors, memory / buffer / texture helpers
 * ====================================================================== */

typedef struct GfxHostBuffer {     /* host-visible, host-coherent, persistently mapped */
    VkBuffer buf;
    VkDeviceMemory mem;
    u8 *map;
    VkDeviceSize size;
} GfxHostBuffer;

typedef struct GfxTexture {    /* an RGBA8 image sampled by the 2D pipeline: font atlas or target */
    bool used;
    int w, h;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkFramebuffer fb;          /* targets only (compatible with rp_clear and rp_load) */
    VkDescriptorSet set;       /* combined image sampler, from gx.tex_pool */
} GfxTexture;

typedef struct GfxViewer {
    bool used;
    VkPipeline pipeline;
} GfxViewer;

typedef struct GfxStorage {    /* a storage buffer (gfx_buffer_create): device-local */
    bool used;
    u64 uid;                   /* unique per creation; descriptor set cache key */
    VkDeviceSize size;         /* multiple of 4 */
    VkBuffer buf;
    VkDeviceMemory mem;
} GfxStorage;

typedef struct GfxCompute {    /* a compute pipeline (gfx_compute_create) */
    bool used;
    u64 uid;                   /* unique per creation; descriptor set cache key */
    int nbuffers;              /* storage buffers at bindings 1..nbuffers */
    u32 push_bytes;
    VkDescriptorSetLayout dsl; /* binding 0: globals (dynamic offset), 1..n: storage buffers */
    VkPipelineLayout layout;
    VkPipeline pipeline;
} GfxCompute;

typedef struct GfxComputeSet { /* a descriptor set cached for one recording (frame or batch) */
    u64 pipe_uid;
    u64 buf_uid[GFX_COMPUTE_MAX_BUFFERS];
    VkBuffer globals;          /* globals buffer and range at binding 0 (chunks selected */
    VkDeviceSize globals_range;/* by the dynamic offset) */
    VkDescriptorSet set;
} GfxComputeSet;
typedef VEC(GfxComputeSet) GfxComputeSetVec;
typedef VEC(VkDescriptorPool) GfxPoolVec;

/* A recording context: a frame slot, or the batch (which uses only cmd, fence,
   serial, globals and the compute fields). */
typedef struct GfxFrame {
    VkCommandBuffer cmd;
    VkFence fence;             /* signaled when this slot's last submission completed */
    VkSemaphore acquired;      /* swapchain image acquired */
    u64 serial;                /* number of this slot's last submission */
    VkDescriptorPool dpool;    /* globals descriptor sets; reset at frame begin */
    GfxHostBuffer globals, verts, indices;
    VkDeviceSize globals_used; /* bytes of `globals` used by this frame */
    VkDescriptorSet gset;      /* descriptor set of the latest globals chunk ... */
    u64 gset_version;          /* ... and the gfx_globals_version it holds */
    bool gchunk;               /* `globals` holds a chunk for this recording: */
    u64 gchunk_version;        /*   the gfx_globals_version it holds, */
    VkDeviceSize gchunk_off, gchunk_size; /* its place in `globals` */
    /* compute: descriptor sets cached for this recording, allocated from `cpools`
       (all reset when the recording begins; more pools are added as needed) */
    GfxPoolVec cpools;
    int cpool, cpool_sets;     /* pool allocated from now, sets taken from it */
    GfxComputeSetVec csets;
    VkPipeline bound_pipe;     /* compute state bound in `cmd` (to skip rebinding) */
    VkDescriptorSet bound_set;
    u32 bound_offset;
} GfxFrame;

typedef struct GfxGarbage {    /* objects waiting for the GPU to finish with them */
    u64 serial;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl;
    VkFramebuffer framebuffer;
    VkImageView view;
    VkImage image;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDescriptorSet set;       /* from gx.tex_pool */
} GfxGarbage;
typedef VEC(GfxGarbage) GfxGarbageVec;

typedef struct GfxState {
    GLFWwindow *window;
    bool vsync;
    bool validation;           /* validation layer enabled */
    bool props2;               /* VK_KHR_get_physical_device_properties2 enabled */
    bool float_controls;       /* VK_KHR_shader_float_controls with SignedZeroInfNanPreserve for 32-bit floats */
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice device;
    VkQueue queue;
    u32 qfamily;
    VkDeviceSize storage_align; /* alignment of globals chunks */

    /* fixed objects */
    VkCommandPool cmdpool;
    VkSampler sampler;
    VkDescriptorSetLayout viewer_dsl, ui_dsl;
    VkPipelineLayout viewer_layout, ui_layout;
    VkRenderPass rp_clear;      /* targets: clear, UNDEFINED -> SHADER_READ_ONLY */
    VkRenderPass rp_load;       /* targets: load, SHADER_READ_ONLY -> SHADER_READ_ONLY */
    VkShaderModule vs_fullscreen, vs_ui, fs_ui;
    VkPipeline ui_pipe_target;  /* 2D pipeline for RGBA8 targets */
    VkDescriptorPool tex_pool;  /* texture descriptor sets (freed individually) */

    /* swapchain */
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    int sc_fb_w, sc_fb_h;       /* window framebuffer size the swapchain was built for */
    bool sc_dirty;              /* rebuild before the next frame */
    u32 sc_count;
    VkImage sc_images[GFX_MAX_SWAPCHAIN_IMAGES];
    VkImageView sc_views[GFX_MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer sc_fbs[GFX_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore sc_present[GFX_MAX_SWAPCHAIN_IMAGES]; /* per image; created once, never shrunk */
    VkRenderPass rp_swap;
    VkFormat rp_swap_format;
    VkPipeline ui_pipe_swap;

    /* frames */
    GfxFrame frames[GFX_FRAMES];
    int frame;                  /* current frame slot */
    u32 image_index;            /* acquired swapchain image */
    bool recording;             /* between gfx_frame_begin and gfx_frame_end */
    bool drawn;                 /* gfx_frame_draw was called this frame */
    bool frame_rendered;        /* a view was drawn (or a flood pass recorded) in this frame */

    /* synchronous work */
    VkCommandBuffer sync_cmd;
    VkFence sync_fence;
    VkDescriptorPool sync_dpool;
    GfxHostBuffer sync_globals, sync_verts, sync_indices, staging;

    /* batch (gfx_batch_begin/end) */
    GfxFrame batch;
    bool batching;              /* between gfx_batch_begin and gfx_batch_end */
    int batch_errors;           /* recording errors in this batch ... */
    char batch_err[512];        /* ... and the first one's message */

    /* deferred destruction */
    u64 submitted, completed;
    GfxGarbageVec garbage;

    /* objects */
    GfxTexture atlas;
    GfxTexture targets[GFX_MAX_TARGETS];
    GfxViewer viewers[GFX_MAX_VIEWERS];
    GfxStorage buffers[GFX_MAX_BUFFERS];
    GfxCompute computes[GFX_MAX_COMPUTES];
    u64 next_uid;               /* for GfxStorage.uid / GfxCompute.uid */

    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    char err[512];              /* message of the last failure */
    char fatal[512];            /* set after an unrecoverable error */
} GfxState;

static GfxState gx;            /* all renderer state; zero when not initialized */

/* The global state block persists across gfx_init/gfx_shutdown (it is plain CPU data). */
static u8 *gfx_globals;
static u32 gfx_globals_size, gfx_globals_cap;
static u64 gfx_globals_version;

static int gfx_validation_error_total;  /* survives gfx_shutdown */
static bool gfx_validation_ever;
static int gfx_warn_count;

static bool gfx_fail(const char *fmt, ...) FS_PRINTF(1, 2);
static bool gfx_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gx.err, sizeof gx.err, fmt, ap);
    va_end(ap);
    return false;
}

/* Problems in functions without an error buffer go to stderr (rate limited). */
static void gfx_warn(const char *fmt, ...) FS_PRINTF(1, 2);
static void gfx_warn(const char *fmt, ...) {
    if (gfx_warn_count >= 100) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gfx: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (++gfx_warn_count == 100) fprintf(stderr, "gfx: (further messages suppressed)\n");
}

static void gfx_copy_err(char *err, int errcap) {
    if (err && errcap > 0) snprintf(err, (size_t)errcap, "%s", gx.err[0] ? gx.err : "unknown error");
}

static const char *gfx_result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY (out of memory)";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY (out of GPU memory)";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST (GPU crashed or was reset)";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER (no usable Vulkan driver)";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV (invalid shader)";
    default: return "unknown VkResult";
    }
}

/* Checks a VkResult; on failure records "<what> failed: <result>". */
static bool gfx_vk(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return true;
    gfx_fail("%s failed: %s (%d)", what, gfx_result_name(r), (int)r);
    if (r == VK_ERROR_DEVICE_LOST && !gx.fatal[0]) {
        snprintf(gx.fatal, sizeof gx.fatal, "%s", gx.err);
        fprintf(stderr, "gfx: fatal: %s\n", gx.fatal);
    }
    return false;
}

static int gfx_memory_type(u32 type_bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < gx.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (gx.memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* Allocate memory with the `want` properties, else with `fallback` ones. */
static bool gfx_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want,
                      VkMemoryPropertyFlags fallback, VkDeviceMemory *out) {
    int type = gfx_memory_type(req.memoryTypeBits, want);
    if (type < 0) type = gfx_memory_type(req.memoryTypeBits, fallback);
    if (type < 0) return gfx_fail("no suitable GPU memory type");
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (u32)type;
    return gfx_vk(vkAllocateMemory(gx.device, &ai, NULL, out), "vkAllocateMemory");
}

/* Frees a buffer immediately: the caller guarantees the GPU no longer uses it. */
static void gfx_hostbuf_free(GfxHostBuffer *b) {
    if (gx.device) {
        vkDestroyBuffer(gx.device, b->buf, NULL);
        vkFreeMemory(gx.device, b->mem, NULL); /* also unmaps */
    }
    memset(b, 0, sizeof *b);
}

static bool gfx_hostbuf_create(GfxHostBuffer *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!gfx_vk(vkCreateBuffer(gx.device, &ci, NULL, &b->buf), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gx.device, b->buf, &req);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    void *map = NULL;
    bool ok = gfx_alloc(req, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, host, &b->mem) &&
              gfx_vk(vkBindBufferMemory(gx.device, b->buf, b->mem, 0), "vkBindBufferMemory") &&
              gfx_vk(vkMapMemory(gx.device, b->mem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
    if (!ok) { gfx_hostbuf_free(b); return false; }
    b->map = (u8 *)map;
    b->size = size;
    return true;
}

static void gfx_defer(GfxGarbage g);

/* Make sure `b` holds at least `need` bytes (contents are not kept). The old
   buffer is freed right away, or deferred when recorded commands that have not
   completed yet may use it. */
static bool gfx_hostbuf_reserve(GfxHostBuffer *b, VkDeviceSize need, VkBufferUsageFlags usage, bool defer) {
    if (b->buf && b->size >= need) return true;
    VkDeviceSize size = FS_MAX(need, FS_MAX(b->size * 2, (VkDeviceSize)64 * 1024));
    if (b->buf && defer) {
        GfxGarbage g = { 0 };
        g.buffer = b->buf;
        g.memory = b->mem;
        gfx_defer(g);
        memset(b, 0, sizeof *b);
    } else {
        gfx_hostbuf_free(b);
    }
    return gfx_hostbuf_create(b, size, usage);
}

static void gfx_destroy_garbage(GfxGarbage *g) {
    vkDestroyPipeline(gx.device, g->pipeline, NULL);
    vkDestroyPipelineLayout(gx.device, g->layout, NULL);
    vkDestroyDescriptorSetLayout(gx.device, g->dsl, NULL);
    vkDestroyFramebuffer(gx.device, g->framebuffer, NULL);
    vkDestroyImageView(gx.device, g->view, NULL);
    vkDestroyImage(gx.device, g->image, NULL);
    vkDestroyBuffer(gx.device, g->buffer, NULL);
    vkFreeMemory(gx.device, g->memory, NULL);
    if (g->set) vkFreeDescriptorSets(gx.device, gx.tex_pool, 1, &g->set);
}

static GfxGarbage gfx_texture_garbage(const GfxTexture *t) {
    GfxGarbage g = { 0 };
    g.framebuffer = t->fb;
    g.view = t->view;
    g.image = t->image;
    g.memory = t->mem;
    g.set = t->set;
    return g;
}

/* Destroys a texture immediately (GPU known to be done with it). */
static void gfx_texture_destroy_now(GfxTexture *t) {
    GfxGarbage g = gfx_texture_garbage(t);
    if (gx.device) gfx_destroy_garbage(&g);
    memset(t, 0, sizeof *t);
}

static VkClearValue gfx_clear_color(float r, float g, float b, float a) {
    VkClearValue v;
    memset(&v, 0, sizeof v);
    v.color.float32[0] = r;
    v.color.float32[1] = g;
    v.color.float32[2] = b;
    v.color.float32[3] = a;
    return v;
}

/* Creates an RGBA8 image with view and texture descriptor set (+ framebuffer for targets).
   The image starts in UNDEFINED layout; the caller initializes it. */
static bool gfx_texture_create(GfxTexture *t, int w, int h, VkImageUsageFlags usage, bool framebuffer) {
    memset(t, 0, sizeof *t);
    t->w = w;
    t->h = h;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = GFX_TARGET_FORMAT;
    ci.extent.width = (u32)w;
    ci.extent.height = (u32)h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkResult r;

    if (!gfx_vk(vkCreateImage(gx.device, &ci, NULL, &t->image), "vkCreateImage")) goto fail;
    vkGetImageMemoryRequirements(gx.device, t->image, &req);
    if (!gfx_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)) goto fail;
    if (!gfx_vk(vkBindImageMemory(gx.device, t->image, t->mem, 0), "vkBindImageMemory")) goto fail;

    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = GFX_TARGET_FORMAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (!gfx_vk(vkCreateImageView(gx.device, &vi, NULL, &t->view), "vkCreateImageView")) goto fail;

    if (framebuffer) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = gx.rp_clear;
        fi.attachmentCount = 1;
        fi.pAttachments = &t->view;
        fi.width = (u32)w;
        fi.height = (u32)h;
        fi.layers = 1;
        if (!gfx_vk(vkCreateFramebuffer(gx.device, &fi, NULL, &t->fb), "vkCreateFramebuffer")) goto fail;
    }

    ai.descriptorPool = gx.tex_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &gx.ui_dsl;
    r = vkAllocateDescriptorSets(gx.device, &ai, &t->set);
    if (r != VK_SUCCESS) {
        t->set = VK_NULL_HANDLE;
        gfx_fail("too many textures (vkAllocateDescriptorSets: %s)", gfx_result_name(r));
        goto fail;
    }
    ii.sampler = gx.sampler;
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.dstSet = t->set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(gx.device, 1, &wr, 0, NULL);
    t->used = true;
    return true;
fail:
    gfx_texture_destroy_now(t);
    return false;
}

static void gfx_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                              VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                              VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ======================================================================
 * 3. Deferred destruction and synchronous submissions
 * ====================================================================== */

static void gfx_defer(GfxGarbage g) {
    g.serial = gx.recording || gx.batching ? GFX_SERIAL_PENDING : gx.submitted;
    vec_push(gx.garbage, g);
}

/* Called right after a frame or batch was submitted (or failed to be): objects
   released while it was recorded were possibly used by that submission. */
static void gfx_resolve_pending(void) {
    for (int i = 0; i < gx.garbage.len; i++)
        if (gx.garbage.data[i].serial == GFX_SERIAL_PENDING) gx.garbage.data[i].serial = gx.submitted;
}

static void gfx_collect_garbage(void) {
    int keep = 0;
    for (int i = 0; i < gx.garbage.len; i++) {
        GfxGarbage *g = &gx.garbage.data[i];
        if (g->serial <= gx.completed) gfx_destroy_garbage(g);
        else gx.garbage.data[keep++] = *g;
    }
    gx.garbage.len = keep;
}

/* Begins recording the synchronous command buffer. Do all fallible preparation
   (buffer uploads, descriptor sets) before this: there is no way to abort. */
static VkCommandBuffer gfx_sync_begin(void) {
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!gfx_vk(vkBeginCommandBuffer(gx.sync_cmd, &bi), "vkBeginCommandBuffer")) return VK_NULL_HANDLE;
    return gx.sync_cmd;
}

/* Submits the synchronous command buffer and waits for it. */
static bool gfx_sync_end(void) {
    if (!gfx_vk(vkEndCommandBuffer(gx.sync_cmd), "vkEndCommandBuffer")) return false;
    if (!gfx_vk(vkResetFences(gx.device, 1, &gx.sync_fence), "vkResetFences")) return false;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &gx.sync_cmd;
    if (!gfx_vk(vkQueueSubmit(gx.queue, 1, &si, gx.sync_fence), "vkQueueSubmit")) return false;
    u64 serial = ++gx.submitted;
    if (!gfx_vk(vkWaitForFences(gx.device, 1, &gx.sync_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
        return false;
    gx.completed = serial;
    gfx_collect_garbage();
    return true;
}

/* ======================================================================
 * 4. Fixed objects: render passes, layouts, pipelines
 * ====================================================================== */

static bool gfx_shader_module(const u32 *code, size_t bytes, VkShaderModule *out) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    return gfx_vk(vkCreateShaderModule(gx.device, &ci, NULL, out), "vkCreateShaderModule");
}

/* Render pass with one color attachment. Targets: `target` = true; the external
   dependencies order this pass's color writes after earlier writes, shader reads
   and copies of the image, and before later shader reads and copies.
   Swapchain: waits for the acquire semaphore (signaled at COLOR_ATTACHMENT_OUTPUT). */
static bool gfx_render_pass(VkFormat format, VkAttachmentLoadOp load, VkImageLayout initial,
                            VkImageLayout final_layout, bool target, VkRenderPass *out) {
    VkAttachmentDescription a = { 0 };
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = load;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = initial;
    a.finalLayout = final_layout;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency deps[2];
    memset(deps, 0, sizeof deps);
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    u32 ndeps = 1;
    if (target) {
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ndeps = 2;
    } else {
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0;
    }
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = ndeps;
    ci.pDependencies = deps;
    return gfx_vk(vkCreateRenderPass(gx.device, &ci, NULL, out), "vkCreateRenderPass");
}

/* Viewer pipeline (ui = false: full-screen triangle, no blending) or 2D pipeline
   (ui = true: GfxVertex input, alpha blending). Viewport and scissor are dynamic. */
static bool gfx_pipeline(VkShaderModule vs, VkShaderModule fs, bool ui, VkRenderPass rp, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";

    VkVertexInputBindingDescription vb = { 0, sizeof(GfxVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, (u32)offsetof(GfxVertex, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, (u32)offsetof(GfxVertex, u) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, (u32)offsetof(GfxVertex, rgba) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (ui) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = va;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb = { 0 };
    cb.blendEnable = ui ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;           /* src*a + dst*(1-a) */
    cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &ds;
    ci.layout = ui ? gx.ui_layout : gx.viewer_layout;
    ci.renderPass = rp;
    return gfx_vk(vkCreateGraphicsPipelines(gx.device, VK_NULL_HANDLE, 1, &ci, NULL, out),
                  "vkCreateGraphicsPipelines");
}

static bool gfx_descriptor_pool(u32 max_sets, VkDescriptorType type, bool free_sets, VkDescriptorPool *out) {
    VkDescriptorPoolSize size = { type, max_sets };
    VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    ci.flags = free_sets ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : 0;
    ci.maxSets = max_sets;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &size;
    return gfx_vk(vkCreateDescriptorPool(gx.device, &ci, NULL, out), "vkCreateDescriptorPool");
}

static bool gfx_set_layout(VkDescriptorType type, VkDescriptorSetLayout *out) {
    VkDescriptorSetLayoutBinding b = { 0, type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = 1;
    ci.pBindings = &b;
    return gfx_vk(vkCreateDescriptorSetLayout(gx.device, &ci, NULL, out), "vkCreateDescriptorSetLayout");
}

static bool gfx_pipeline_layout(VkDescriptorSetLayout dsl, VkShaderStageFlags pc_stage, VkPipelineLayout *out) {
    VkPushConstantRange pc = { pc_stage, 0, 16 };
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &dsl;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &pc;
    return gfx_vk(vkCreatePipelineLayout(gx.device, &ci, NULL, out), "vkCreatePipelineLayout");
}

static bool gfx_create_fixed_objects(void) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gx.qfamily;
    if (!gfx_vk(vkCreateCommandPool(gx.device, &pci, NULL, &gx.cmdpool), "vkCreateCommandPool")) return false;

    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = gx.cmdpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (!gfx_vk(vkAllocateCommandBuffers(gx.device, &cai, &gx.sync_cmd), "vkAllocateCommandBuffers") ||
        !gfx_vk(vkCreateFence(gx.device, &fci, NULL, &gx.sync_fence), "vkCreateFence"))
        return false;

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (!gfx_vk(vkCreateSampler(gx.device, &sci, NULL, &gx.sampler), "vkCreateSampler")) return false;

    gx.storage_align = FS_MAX(gx.props.limits.minStorageBufferOffsetAlignment, (VkDeviceSize)16);
    return gfx_set_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &gx.viewer_dsl) &&
           gfx_set_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &gx.ui_dsl) &&
           gfx_pipeline_layout(gx.viewer_dsl, VK_SHADER_STAGE_FRAGMENT_BIT, &gx.viewer_layout) &&
           gfx_pipeline_layout(gx.ui_dsl, VK_SHADER_STAGE_VERTEX_BIT, &gx.ui_layout) &&
           gfx_render_pass(GFX_TARGET_FORMAT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, &gx.rp_clear) &&
           gfx_render_pass(GFX_TARGET_FORMAT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, &gx.rp_load) &&
           gfx_shader_module(gfx_spv_fullscreen_vert, sizeof gfx_spv_fullscreen_vert, &gx.vs_fullscreen) &&
           gfx_shader_module(gfx_spv_ui_vert, sizeof gfx_spv_ui_vert, &gx.vs_ui) &&
           gfx_shader_module(gfx_spv_ui_frag, sizeof gfx_spv_ui_frag, &gx.fs_ui) &&
           gfx_pipeline(gx.vs_ui, gx.fs_ui, true, gx.rp_clear, &gx.ui_pipe_target) &&
           gfx_descriptor_pool(GFX_TEXTURE_SETS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, true, &gx.tex_pool) &&
           gfx_descriptor_pool(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, &gx.sync_dpool);
}

static bool gfx_create_frames(void) {
    for (int i = 0; i < GFX_FRAMES; i++) {
        GfxFrame *f = &gx.frames[i];
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = gx.cmdpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;   /* so the first wait returns at once */
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!gfx_vk(vkAllocateCommandBuffers(gx.device, &cai, &f->cmd), "vkAllocateCommandBuffers") ||
            !gfx_vk(vkCreateFence(gx.device, &fci, NULL, &f->fence), "vkCreateFence") ||
            !gfx_vk(vkCreateSemaphore(gx.device, &sci, NULL, &f->acquired), "vkCreateSemaphore") ||
            !gfx_descriptor_pool(GFX_FRAME_GLOBAL_SETS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, &f->dpool))
            return false;
    }
    /* The batch: a command buffer and a fence (no swapchain objects, no viewer sets). */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = gx.cmdpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return gfx_vk(vkAllocateCommandBuffers(gx.device, &cai, &gx.batch.cmd), "vkAllocateCommandBuffers") &&
           gfx_vk(vkCreateFence(gx.device, &fci, NULL, &gx.batch.fence), "vkCreateFence");
}

/* ======================================================================
 * 5. Instance and device
 * ====================================================================== */

static VKAPI_ATTR VkBool32 VKAPI_CALL gfx_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                         void *user) {
    (void)user;
    bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    bool validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
    /* General (loader) warnings are mostly noise about unrelated drivers. */
    if (!error && !(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
        return VK_FALSE;
    if (error && validation) gfx_validation_error_total++;
    fprintf(stderr, "vulkan %s: %s\n", error ? "error" : "warning",
            data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT gfx_debug_messenger_info(void) {
    VkDebugUtilsMessengerCreateInfoEXT ci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = gfx_debug_callback;
    return ci;
}

static bool gfx_has_ext(const VkExtensionProperties *exts, u32 n, const char *name) {
    for (u32 i = 0; i < n; i++)
        if (strcmp(exts[i].extensionName, name) == 0) return true;
    return false;
}

/* Instance extensions provided by the implementation or by `layer` (NULL = none). */
static VkExtensionProperties *gfx_instance_exts(const char *layer, u32 *n) {
    *n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, n, NULL) != VK_SUCCESS || *n == 0) return NULL;
    VkExtensionProperties *exts = (VkExtensionProperties *)xcalloc(*n, sizeof *exts);
    if (vkEnumerateInstanceExtensionProperties(layer, n, exts) < 0) *n = 0;
    return exts;
}

static bool gfx_has_validation_layer(void) {
    u32 n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || n == 0) return false;
    VkLayerProperties *layers = (VkLayerProperties *)xcalloc(n, sizeof *layers);
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&n, layers) >= 0)
        for (u32 i = 0; i < n; i++)
            if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
    free(layers);
    return found;
}

static bool gfx_create_instance(bool want_validation) {
    const char *missing = gfx_load_global_funcs();
    if (missing) return gfx_fail("the Vulkan library does not provide %s", missing);

    const char *enable[40];
    u32 ne = 0;
    if (gx.window) {
        u32 count = 0;
        const char **req = glfwGetRequiredInstanceExtensions(&count);
        if (!req)
            return gfx_fail("GLFW cannot create Vulkan surfaces (no Vulkan loader for GLFW: "
                            "gfx_preinit_loader() must be called before glfwInit())");
        for (u32 i = 0; i < count && ne < 32; i++) enable[ne++] = req[i];
    }
    u32 next = 0;
    VkExtensionProperties *exts = gfx_instance_exts(NULL, &next);
    for (u32 i = 0; i < ne; i++)
        if (!gfx_has_ext(exts, next, enable[i])) {
            gfx_fail("required Vulkan instance extension %s is not available", enable[i]);
            free(exts);
            return false;
        }
    VkInstanceCreateFlags flags = 0;
    if (gfx_has_ext(exts, next, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {   /* MoltenVK */
        enable[ne++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    bool props2 = gfx_has_ext(exts, next, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (props2) enable[ne++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;

    const char *layers[1];
    u32 nlayers = 0;
    bool debug = false;
    if (want_validation) {
        if (gfx_has_validation_layer()) {
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
            gx.validation = true;
            u32 nlext = 0;
            VkExtensionProperties *lext = gfx_instance_exts(layers[0], &nlext);
            debug = gfx_has_ext(exts, next, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                    gfx_has_ext(lext, nlext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            free(lext);
            if (debug) enable[ne++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            fprintf(stderr, "gfx: validation requested, but VK_LAYER_KHRONOS_validation is not installed\n");
        }
    }
    free(exts);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "FractalStream";
    app.applicationVersion = 1;
    app.pEngineName = "FractalStream";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;
    VkDebugUtilsMessengerCreateInfoEXT dci = gfx_debug_messenger_info();
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pNext = debug ? &dci : NULL;   /* also reports problems in vkCreate/DestroyInstance */
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = nlayers;
    ci.ppEnabledLayerNames = layers;
    ci.enabledExtensionCount = ne;
    ci.ppEnabledExtensionNames = enable;
    VkResult r = vkCreateInstance(&ci, NULL, &gx.instance);
    gx.props2 = props2;
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
        return gfx_fail("no Vulkan driver found (install the GPU's Vulkan driver, or Mesa lavapipe for software rendering)");
    if (!gfx_vk(r, "vkCreateInstance")) { gx.instance = VK_NULL_HANDLE; return false; }

    missing = gfx_load_instance_funcs(gx.instance);
    if (missing) {
        if (vkDestroyInstance) vkDestroyInstance(gx.instance, NULL);
        gx.instance = VK_NULL_HANDLE;
        return gfx_fail("the Vulkan library does not provide %s", missing);
    }
    if (gx.window && (missing = gfx_load_surface_funcs(gx.instance)) != NULL)
        return gfx_fail("the Vulkan library does not provide %s", missing);
    if (debug && !gfx_load_debug_funcs(gx.instance)) {
        if (!gfx_vk(vkCreateDebugUtilsMessengerEXT(gx.instance, &dci, NULL, &gx.messenger),
                    "vkCreateDebugUtilsMessengerEXT"))
            gx.messenger = VK_NULL_HANDLE;
    }
    if (gx.validation) gfx_validation_ever = true;
    return true;
}

static bool gfx_contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return n == 0;
}

static VkExtensionProperties *gfx_device_exts(VkPhysicalDevice pd, u32 *n) {
    *n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, n, NULL) != VK_SUCCESS || *n == 0) return NULL;
    VkExtensionProperties *exts = (VkExtensionProperties *)xcalloc(*n, sizeof *exts);
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, n, exts) < 0) *n = 0;
    return exts;
}

/* Queue family with graphics (and presentation to our surface when windowed), or -1. */
static int gfx_queue_family(VkPhysicalDevice pd) {
    u32 n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    VkQueueFamilyProperties *q = (VkQueueFamilyProperties *)xcalloc(FS_MAX(n, 1u), sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    int found = -1;
    for (u32 i = 0; i < n && found < 0; i++) {
        if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || q[i].queueCount == 0) continue;
        VkBool32 present = VK_TRUE;
        if (gx.surface && vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, gx.surface, &present) != VK_SUCCESS)
            present = VK_FALSE;
        if (present) found = (int)i;
    }
    free(q);
    return found;
}

/* The device's driver (VK_KHR_driver_properties): its id (0 if unknown) and name, e.g. "MoltenVK" */
static int gfx_driver_of(VkPhysicalDevice d, char *name, int cap) {
    if (name && cap > 0) name[0] = 0;
    if (!gx.props2) return 0;
    u32 n = 0;
    VkExtensionProperties *exts = gfx_device_exts(d, &n);
    bool has = gfx_has_ext(exts, n, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    free(exts);
    PFN_vkGetPhysicalDeviceProperties2KHR props2 =
        (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(gx.instance, "vkGetPhysicalDeviceProperties2KHR");
    if (!has || !props2) return 0;
    VkPhysicalDeviceDriverProperties dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &dp;
    props2(d, &p2);
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s %s", dp.driverName, dp.driverInfo);
    return (int)dp.driverID;
}

static bool gfx_pick_device(void) {
    u32 n = 0;
    if (!gfx_vk(vkEnumeratePhysicalDevices(gx.instance, &n, NULL), "vkEnumeratePhysicalDevices")) return false;
    if (n == 0) return gfx_fail("no Vulkan devices found");
    VkPhysicalDevice devs[16];
    n = FS_MIN(n, 16u);
    if (vkEnumeratePhysicalDevices(gx.instance, &n, devs) < 0) return gfx_fail("vkEnumeratePhysicalDevices failed");

    const char *want = getenv("FS_GPU");
    int best = -1, best_score = -1, best_q = 0;
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        int q = gfx_queue_family(devs[i]);
        if (q < 0) continue;
        if (gx.window) {
            u32 ndext = 0;
            VkExtensionProperties *dext = gfx_device_exts(devs[i], &ndext);
            bool swapchain = gfx_has_ext(dext, ndext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            free(dext);
            if (!swapchain) continue;
        }
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 40
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 30
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 20
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 10 : 0;
        char driver[600];
        int driver_id = gfx_driver_of(devs[i], driver, sizeof driver);
        /* the same GPU through two drivers (a Vulkan SDK on macOS has MoltenVK and KosmicKrisp): MoltenVK, the one
           the macOS app bundles */
        if (driver_id == VK_DRIVER_ID_MOLTENVK) score += 1;
        if (want && *want && (gfx_contains_nocase(p.deviceName, want) || gfx_contains_nocase(driver, want))) score += 1000;
        if (getenv("FS_DEBUG"))
            fprintf(stderr, "gfx: device %u: %s (type %d, Vulkan %u.%u, driver: %s)\n", i, p.deviceName, (int)p.deviceType,
                    VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), driver[0] ? driver : "?");
        if (score > best_score) { best = (int)i; best_score = score; best_q = q; }
    }
    if (best < 0)
        return gfx_fail("no Vulkan device can %s", gx.window ? "draw to this window" : "render graphics");
    gx.phys = devs[best];
    gx.qfamily = (u32)best_q;
    vkGetPhysicalDeviceProperties(gx.phys, &gx.props);
    vkGetPhysicalDeviceMemoryProperties(gx.phys, &gx.memprops);
    snprintf(gx.device_name, sizeof gx.device_name, "%s", gx.props.deviceName);
    if (want && *want && best_score < 1000)
        gfx_warn("FS_GPU=\"%s\" matches no usable device; using \"%s\"", want, gx.device_name);
    if (getenv("FS_DEBUG")) fprintf(stderr, "gfx: using device %d (%s)\n", best, gx.device_name);
    return true;
}

static bool gfx_create_device(void) {
    u32 next = 0;
    VkExtensionProperties *exts = gfx_device_exts(gx.phys, &next);
    const char *enable[4];
    u32 ne = 0;
    if (gx.window) enable[ne++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* Implementations that are not fully conformant (MoltenVK) expose this and require
       it to be enabled; we use nothing it restricts. */
    if (gfx_has_ext(exts, next, "VK_KHR_portability_subset")) enable[ne++] = "VK_KHR_portability_subset";
    /* IEEE float behaviour that a shader can ask for (exact double-single arithmetic, see codegen.c) */
    gx.float_controls = false;
    if (gx.props2 && gfx_has_ext(exts, next, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) {
        PFN_vkGetPhysicalDeviceProperties2KHR props2 =
            (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(gx.instance, "vkGetPhysicalDeviceProperties2KHR");
        if (props2) {
            VkPhysicalDeviceFloatControlsProperties fc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES };
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            p2.pNext = &fc;
            props2(gx.phys, &p2);
            if (fc.shaderSignedZeroInfNanPreserveFloat32) {
                enable[ne++] = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME;
                gx.float_controls = true;
            }
        }
    }
    free(exts);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = gx.qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = ne;
    ci.ppEnabledExtensionNames = enable;
    if (!gfx_vk(vkCreateDevice(gx.phys, &ci, NULL, &gx.device), "vkCreateDevice")) {
        gx.device = VK_NULL_HANDLE;
        return false;
    }
    const char *missing = gfx_load_device_funcs(gx.device);
    if (!missing && gx.window) missing = gfx_load_swapchain_funcs(gx.device);
    if (missing) {
        if (vkDestroyDevice) vkDestroyDevice(gx.device, NULL);
        gx.device = VK_NULL_HANDLE;
        return gfx_fail("the Vulkan driver does not provide %s", missing);
    }
    vkGetDeviceQueue(gx.device, gx.qfamily, 0, &gx.queue);
    return true;
}

static bool gfx_swapchain_build_or_skip(void);

bool gfx_init(const GfxInitDesc *desc, char *err, int errcap) {
    if (gx.instance) {
        if (err && errcap > 0) snprintf(err, (size_t)errcap, "gfx_init: already initialized");
        return false;
    }
    memset(&gx, 0, sizeof gx);
    gx.window = desc ? (GLFWwindow *)desc->glfw_window : NULL;
    gx.vsync = desc ? desc->vsync : true;
    static const u8 white[4] = { 255, 255, 255, 255 };

    bool ok = gfx_load_library() ||
              gfx_fail("could not load the Vulkan library (%s); install a Vulkan driver", gfx_library_names());
    const char *env_validation = getenv("FS_VULKAN_VALIDATION");
    bool validation = (desc && desc->validation) || (env_validation && atoi(env_validation) != 0);
    ok = ok && gfx_create_instance(validation);
    if (ok && gx.window)
        ok = gfx_vk(glfwCreateWindowSurface(gx.instance, gx.window, NULL, &gx.surface), "glfwCreateWindowSurface");
    ok = ok && gfx_pick_device() && gfx_create_device() && gfx_create_fixed_objects() && gfx_create_frames();
    ok = ok && gfx_set_font_atlas(white, 1, 1);
    ok = ok && (!gx.window || gfx_swapchain_build_or_skip());
    if (!ok) {
        char msg[sizeof gx.err];
        snprintf(msg, sizeof msg, "%s", gx.err);
        gfx_shutdown();
        snprintf(gx.err, sizeof gx.err, "%s", msg);
        gfx_copy_err(err, errcap);
        return false;
    }
    return true;
}

static void gfx_swapchain_destroy_images(void);

void gfx_shutdown(void) {
    if (gx.device) {
        vkDeviceWaitIdle(gx.device);
        gx.recording = false;
        gx.batching = false;    /* an open batch is dropped (destroying the pool frees its commands) */
        gx.completed = gx.submitted;
        for (int i = 0; i < gx.garbage.len; i++) gfx_destroy_garbage(&gx.garbage.data[i]);
        gx.garbage.len = 0;
        for (int i = 0; i < GFX_MAX_TARGETS; i++) gfx_texture_destroy_now(&gx.targets[i]);
        for (int i = 0; i < GFX_MAX_VIEWERS; i++) vkDestroyPipeline(gx.device, gx.viewers[i].pipeline, NULL);
        for (int i = 0; i < GFX_MAX_BUFFERS; i++) {
            vkDestroyBuffer(gx.device, gx.buffers[i].buf, NULL);
            vkFreeMemory(gx.device, gx.buffers[i].mem, NULL);
        }
        for (int i = 0; i < GFX_MAX_COMPUTES; i++) {
            vkDestroyPipeline(gx.device, gx.computes[i].pipeline, NULL);
            vkDestroyPipelineLayout(gx.device, gx.computes[i].layout, NULL);
            vkDestroyDescriptorSetLayout(gx.device, gx.computes[i].dsl, NULL);
        }
        gfx_texture_destroy_now(&gx.atlas);
        for (int i = 0; i <= GFX_FRAMES; i++) {   /* the frame slots and the batch */
            GfxFrame *f = i < GFX_FRAMES ? &gx.frames[i] : &gx.batch;
            gfx_hostbuf_free(&f->globals);
            gfx_hostbuf_free(&f->verts);
            gfx_hostbuf_free(&f->indices);
            vkDestroyDescriptorPool(gx.device, f->dpool, NULL);
            for (int k = 0; k < f->cpools.len; k++) vkDestroyDescriptorPool(gx.device, f->cpools.data[k], NULL);
            vec_free(f->cpools);
            vec_free(f->csets);
            vkDestroyFence(gx.device, f->fence, NULL);
            vkDestroySemaphore(gx.device, f->acquired, NULL);
        }
        gfx_hostbuf_free(&gx.sync_globals);
        gfx_hostbuf_free(&gx.sync_verts);
        gfx_hostbuf_free(&gx.sync_indices);
        gfx_hostbuf_free(&gx.staging);
        gfx_swapchain_destroy_images();
        if (gx.swapchain) vkDestroySwapchainKHR(gx.device, gx.swapchain, NULL);
        for (int i = 0; i < GFX_MAX_SWAPCHAIN_IMAGES; i++) vkDestroySemaphore(gx.device, gx.sc_present[i], NULL);
        vkDestroyPipeline(gx.device, gx.ui_pipe_swap, NULL);
        vkDestroyRenderPass(gx.device, gx.rp_swap, NULL);
        vkDestroyPipeline(gx.device, gx.ui_pipe_target, NULL);
        vkDestroyShaderModule(gx.device, gx.vs_fullscreen, NULL);
        vkDestroyShaderModule(gx.device, gx.vs_ui, NULL);
        vkDestroyShaderModule(gx.device, gx.fs_ui, NULL);
        vkDestroyRenderPass(gx.device, gx.rp_clear, NULL);
        vkDestroyRenderPass(gx.device, gx.rp_load, NULL);
        vkDestroyPipelineLayout(gx.device, gx.viewer_layout, NULL);
        vkDestroyPipelineLayout(gx.device, gx.ui_layout, NULL);
        vkDestroyDescriptorSetLayout(gx.device, gx.viewer_dsl, NULL);
        vkDestroyDescriptorSetLayout(gx.device, gx.ui_dsl, NULL);
        vkDestroyDescriptorPool(gx.device, gx.tex_pool, NULL);
        vkDestroyDescriptorPool(gx.device, gx.sync_dpool, NULL);
        vkDestroySampler(gx.device, gx.sampler, NULL);
        vkDestroyFence(gx.device, gx.sync_fence, NULL);
        vkDestroyCommandPool(gx.device, gx.cmdpool, NULL);   /* frees the command buffers */
        vkDestroyDevice(gx.device, NULL);
    }
    if (gx.instance) {
        if (gx.surface) vkDestroySurfaceKHR(gx.instance, gx.surface, NULL);
        if (gx.messenger) vkDestroyDebugUtilsMessengerEXT(gx.instance, gx.messenger, NULL);
        vkDestroyInstance(gx.instance, NULL);
    }
    vec_free(gx.garbage);
    memset(&gx, 0, sizeof gx);
}

const char *gfx_device_name(void) { return gx.device_name; }
bool gfx_has_float_controls(void) { return gx.float_controls; }
int gfx_max_image_size(void) { return gx.device ? (int)gx.props.limits.maxImageDimension2D : 0; }
const char *gfx_fatal_error(void) { return gx.fatal[0] ? gx.fatal : NULL; }
int gfx_validation_errors(void) { return gfx_validation_ever ? gfx_validation_error_total : -1; }

/* ======================================================================
 * 6. Swapchain
 * ====================================================================== */

/* Destroys views and framebuffers of the swapchain images (GPU must be idle). */
static void gfx_swapchain_destroy_images(void) {
    for (u32 i = 0; i < gx.sc_count; i++) {
        vkDestroyFramebuffer(gx.device, gx.sc_fbs[i], NULL);
        vkDestroyImageView(gx.device, gx.sc_views[i], NULL);
        gx.sc_fbs[i] = VK_NULL_HANDLE;
        gx.sc_views[i] = VK_NULL_HANDLE;
    }
    gx.sc_count = 0;
}

static VkSurfaceFormatKHR gfx_choose_surface_format(void) {
    VkSurfaceFormatKHR fallback = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    VkSurfaceFormatKHR formats[64];
    u32 n = ARRAY_COUNT(formats);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(gx.phys, gx.surface, &n, formats) < 0 || n == 0) return fallback;
    if (n == 1 && formats[0].format == VK_FORMAT_UNDEFINED) return fallback;  /* "anything goes" */
    /* UNORM (not sRGB): colors are not gamma-converted anywhere (architecture.md). */
    for (u32 i = 0; i < n; i++)
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[i];
    for (u32 i = 0; i < n; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
            return formats[i];
    return formats[0];
}

static VkPresentModeKHR gfx_choose_present_mode(void) {
    if (gx.vsync) return VK_PRESENT_MODE_FIFO_KHR;   /* always supported */
    VkPresentModeKHR modes[16];
    u32 n = ARRAY_COUNT(modes);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(gx.phys, gx.surface, &n, modes) < 0) n = 0;
    for (u32 i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return modes[i];
    for (u32 i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return modes[i];
    return VK_PRESENT_MODE_FIFO_KHR;
}

/* (Re)creates the swapchain for the window's current size. Returns 1 when ready,
   0 when there is nothing to draw on (minimized; retried next frame), -1 on error. */
static int gfx_swapchain_build(void) {
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(gx.window, &fbw, &fbh);
    gx.sc_fb_w = fbw;
    gx.sc_fb_h = fbh;
    gx.sc_dirty = true;
    if (fbw <= 0 || fbh <= 0) return 0;

    VkSurfaceCapabilitiesKHR caps;
    if (!gfx_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gx.phys, gx.surface, &caps),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return -1;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   /* the surface takes the swapchain's size */
        extent.width = (u32)FS_CLAMP((u32)fbw, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = (u32)FS_CLAMP((u32)fbh, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return 0;

    /* Wait for all frames: the old swapchain's images and framebuffers go away. */
    if (!gfx_vk(vkDeviceWaitIdle(gx.device), "vkDeviceWaitIdle")) return -1;
    gx.completed = gx.submitted;
    gfx_collect_garbage();

    VkSurfaceFormatKHR format = gfx_choose_surface_format();
    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        static const VkCompositeAlphaFlagBitsKHR others[] = { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
        for (int i = 0; i < ARRAY_COUNT(others); i++)
            if (caps.supportedCompositeAlpha & others[i]) { alpha = others[i]; break; }
    }
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = gx.surface;
    ci.minImageCount = count;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = gfx_choose_present_mode();
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = gx.swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(gx.device, &ci, NULL, &sc);
    /* The old swapchain is retired even if creation failed. */
    gfx_swapchain_destroy_images();
    if (gx.swapchain) vkDestroySwapchainKHR(gx.device, gx.swapchain, NULL);
    gx.swapchain = VK_NULL_HANDLE;
    if (!gfx_vk(r, "vkCreateSwapchainKHR")) return -1;
    gx.swapchain = sc;
    gx.sc_format = format.format;
    gx.sc_extent = extent;

    /* Render pass and 2D pipeline for this format (usually created once). */
    if (gx.rp_swap && gx.rp_swap_format != format.format) {
        vkDestroyPipeline(gx.device, gx.ui_pipe_swap, NULL);
        vkDestroyRenderPass(gx.device, gx.rp_swap, NULL);
        gx.ui_pipe_swap = VK_NULL_HANDLE;
        gx.rp_swap = VK_NULL_HANDLE;
    }
    if (!gx.rp_swap) {
        if (!gfx_render_pass(format.format, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false, &gx.rp_swap))
            return -1;
        gx.rp_swap_format = format.format;
        if (!gfx_pipeline(gx.vs_ui, gx.fs_ui, true, gx.rp_swap, &gx.ui_pipe_swap)) return -1;
    }

    u32 n = 0;
    if (!gfx_vk(vkGetSwapchainImagesKHR(gx.device, gx.swapchain, &n, NULL), "vkGetSwapchainImagesKHR")) return -1;
    if (n > GFX_MAX_SWAPCHAIN_IMAGES) { gfx_fail("too many swapchain images (%u)", n); return -1; }
    if (!gfx_vk(vkGetSwapchainImagesKHR(gx.device, gx.swapchain, &n, gx.sc_images), "vkGetSwapchainImagesKHR"))
        return -1;
    for (u32 i = 0; i < n; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = gx.sc_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (!gfx_vk(vkCreateImageView(gx.device, &vi, NULL, &gx.sc_views[i]), "vkCreateImageView")) return -1;
        gx.sc_count = i + 1;   /* so a failure below still destroys what exists */
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = gx.rp_swap;
        fi.attachmentCount = 1;
        fi.pAttachments = &gx.sc_views[i];
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        if (!gfx_vk(vkCreateFramebuffer(gx.device, &fi, NULL, &gx.sc_fbs[i]), "vkCreateFramebuffer")) return -1;
        if (!gx.sc_present[i]) {
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (!gfx_vk(vkCreateSemaphore(gx.device, &sci, NULL, &gx.sc_present[i]), "vkCreateSemaphore"))
                return -1;
        }
    }
    gx.sc_dirty = false;
    return 1;
}

/* For gfx_init: an initially minimized window is fine, errors are not. */
static bool gfx_swapchain_build_or_skip(void) { return gfx_swapchain_build() >= 0; }

void gfx_framebuffer_size(int *w, int *h) {
    bool ok = gx.swapchain && gx.sc_count > 0;
    if (w) *w = ok ? (int)gx.sc_extent.width : 0;
    if (h) *h = ok ? (int)gx.sc_extent.height : 0;
}

/* ======================================================================
 * 7. Resources: font atlas, globals, viewers, targets
 * ====================================================================== */

bool gfx_set_font_atlas(const u8 *rgba, int w, int h) {
    if (!gx.device) { gfx_warn("gfx_set_font_atlas: not initialized"); return false; }
    int max = (int)gx.props.limits.maxImageDimension2D;
    if (!rgba || w <= 0 || h <= 0 || w > max || h > max) {
        gfx_fail("invalid font atlas size %dx%d", w, h);
        gfx_warn("%s", gx.err);
        return false;
    }
    VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    GfxTexture t;
    if (!gfx_hostbuf_reserve(&gx.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
        !gfx_texture_create(&t, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) {
        gfx_warn("gfx_set_font_atlas: %s", gx.err);
        return false;
    }
    memcpy(gx.staging.map, rgba, (size_t)size);
    VkCommandBuffer cmd = gfx_sync_begin();
    bool ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        gfx_image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (u32)w;
        region.imageExtent.height = (u32)h;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, gx.staging.buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        gfx_image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        ok = gfx_sync_end();
    }
    if (!ok) {
        gfx_warn("gfx_set_font_atlas: %s", gx.err);
        gfx_texture_destroy_now(&t);   /* waited for (or the device is lost) */
        return false;
    }
    if (gx.atlas.used) gfx_defer(gfx_texture_garbage(&gx.atlas));  /* in-flight frames may sample it */
    gx.atlas = t;
    return true;
}

void gfx_set_globals(const void *data, u32 size) {
    if (size > gfx_globals_cap) {
        gfx_globals = (u8 *)xrealloc(gfx_globals, size);
        gfx_globals_cap = size;
    }
    if (size && data) memcpy(gfx_globals, data, size);
    else if (size) memset(gfx_globals, 0, size);
    gfx_globals_size = size;
    gfx_globals_version++;
}

/* Bytes uploaded per globals chunk: at least 16, a multiple of 16, within the device limit. */
static VkDeviceSize gfx_globals_upload_size(void) {
    VkDeviceSize size = ((VkDeviceSize)gfx_globals_size + 15) & ~(VkDeviceSize)15;
    size = FS_MAX(size, (VkDeviceSize)16);
    VkDeviceSize limit = gx.props.limits.maxStorageBufferRange & ~(VkDeviceSize)15;
    if (size > limit) {
        gfx_warn("global state (%u bytes) exceeds the GPU's storage buffer limit (%u bytes)",
                 gfx_globals_size, (u32)limit);
        size = limit;
    }
    return size;
}

static void gfx_globals_copy(u8 *dst, VkDeviceSize size) {
    size_t n = FS_MIN((size_t)gfx_globals_size, (size_t)size);
    if (n) memcpy(dst, gfx_globals, n);
    memset(dst + n, 0, (size_t)size - n);
}

static void gfx_write_storage_set(VkDescriptorSet set, VkBuffer buf, VkDeviceSize offset, VkDeviceSize range) {
    VkDescriptorBufferInfo bi = { buf, offset, range };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(gx.device, 1, &wr, 0, NULL);
}

static bool gfx_alloc_globals_set(VkDescriptorPool pool, VkDescriptorSet *out) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &gx.viewer_dsl;
    VkResult r = vkAllocateDescriptorSets(gx.device, &ai, out);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL)
        return gfx_fail("more than %d gfx_set_globals/render pairs in one frame", GFX_FRAME_GLOBAL_SETS);
    return gfx_vk(r, "vkAllocateDescriptorSets");
}

/* Makes sure the globals buffer of recording `f` (frame or batch) holds the current
   globals: every new gfx_set_globals version gets its own chunk of the buffer, at
   f->gchunk_off (a multiple of gx.storage_align), f->gchunk_size bytes. */
static bool gfx_frame_globals_chunk(GfxFrame *f) {
    if (f->gchunk && f->gchunk_version == gfx_globals_version) return true;
    VkDeviceSize size = gfx_globals_upload_size();
    VkDeviceSize off = (f->globals_used + gx.storage_align - 1) / gx.storage_align * gx.storage_align;
    if (!f->globals.buf || off + size > f->globals.size) {
        /* Grow. Chunks recorded earlier in this frame keep the old buffer (deferred). */
        if (!gfx_hostbuf_reserve(&f->globals, off + size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
            return false;
        off = 0;
    }
    gfx_globals_copy(f->globals.map + off, size);
    f->globals_used = off + size;
    f->gchunk = true;
    f->gchunk_version = gfx_globals_version;
    f->gchunk_off = off;
    f->gchunk_size = size;
    return true;
}

/* Descriptor set with the current globals for viewer renders recorded in frame `f`. */
static VkDescriptorSet gfx_frame_globals_set(GfxFrame *f) {
    if (f->gset && f->gset_version == gfx_globals_version) return f->gset;
    VkDescriptorSet set;
    if (!gfx_frame_globals_chunk(f) || !gfx_alloc_globals_set(f->dpool, &set)) return VK_NULL_HANDLE;
    gfx_write_storage_set(set, f->globals.buf, f->gchunk_off, f->gchunk_size);
    f->gset = set;
    f->gset_version = gfx_globals_version;
    return set;
}

/* Forgets the per-recording state of frame slot or batch `f` before it records
   again; the GPU is done with its previous submission. (The viewer descriptor
   pool f->dpool is reset by gfx_frame_begin, which can fail on it.) */
static void gfx_rec_reset(GfxFrame *f) {
    f->globals_used = 0;
    f->gset = VK_NULL_HANDLE;
    f->gset_version = 0;
    f->gchunk = false;
    if (f->cpool > 0 || f->cpool_sets > 0)
        for (int i = 0; i <= f->cpool && i < f->cpools.len; i++)
            vkResetDescriptorPool(gx.device, f->cpools.data[i], 0);   /* cannot fail */
    f->cpool = 0;
    f->cpool_sets = 0;
    f->csets.len = 0;
    f->bound_pipe = VK_NULL_HANDLE;
    f->bound_set = VK_NULL_HANDLE;
    f->bound_offset = 0;
}

/* Descriptor set with the current globals for the synchronous command buffer
   (safe to rewrite: every synchronous submission is waited for). */
static VkDescriptorSet gfx_sync_globals_set(void) {
    VkDeviceSize size = gfx_globals_upload_size();
    VkDescriptorSet set;
    if (!gfx_hostbuf_reserve(&gx.sync_globals, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return VK_NULL_HANDLE;
    if (!gfx_vk(vkResetDescriptorPool(gx.device, gx.sync_dpool, 0), "vkResetDescriptorPool")) return VK_NULL_HANDLE;
    if (!gfx_alloc_globals_set(gx.sync_dpool, &set)) return VK_NULL_HANDLE;
    gfx_globals_copy(gx.sync_globals.map, size);
    gfx_write_storage_set(set, gx.sync_globals.buf, 0, size);
    return set;
}

/* Checks that `w` is SPIR-V with an entry point named "main" of execution model
   `model` (4 = Fragment, 5 = GLCompute). Compute shaders must also declare their
   workgroup size (LocalSize execution mode or a WorkgroupSize built-in). */
static bool gfx_check_spirv(const u32 *w, u32 n, u32 model) {
    if (!w || n < 5 || w[0] != 0x07230203u) return gfx_fail("not a SPIR-V module (bad magic number or too short)");
    u32 entry = 0;    /* id of the entry point function */
    bool local_size = false;
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 i = 5; i < n;) {
            u32 op = w[i] & 0xFFFFu, len = w[i] >> 16;
            if (len == 0 || len > n - i) return gfx_fail("malformed SPIR-V (bad instruction length at word %u)", i);
            /* OpEntryPoint (15): execution model, function id, name... */
            if (pass == 0 && op == 15 && len >= 5 && w[i + 1] == model && memcmp(&w[i + 3], "main", 5) == 0)
                entry = w[i + 2];
            /* OpExecutionMode (16): function id, LocalSize (17), x, y, z */
            if (pass == 1 && op == 16 && len >= 6 && w[i + 1] == entry && w[i + 2] == 17) local_size = true;
            /* OpDecorate (71): id, BuiltIn (11), WorkgroupSize (25) */
            if (pass == 1 && op == 71 && len >= 4 && w[i + 2] == 11 && w[i + 3] == 25) local_size = true;
            i += len;
        }
        if (!entry)
            return gfx_fail("the SPIR-V module has no %s shader entry point named \"main\"",
                            model == 5 ? "compute" : "fragment");
        if (model != 5) return true;
    }
    return local_size || gfx_fail("the compute shader does not declare its workgroup size (LocalSize)");
}

int gfx_viewer_create(const u32 *spirv, u32 nwords, char *err, int errcap) {
    if (!gx.device) { gfx_fail("gfx is not initialized"); gfx_copy_err(err, errcap); return 0; }
    int slot = -1;
    for (int i = 0; i < GFX_MAX_VIEWERS && slot < 0; i++) if (!gx.viewers[i].used) slot = i;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool ok = gfx_check_spirv(spirv, nwords, 4) &&
              (slot >= 0 || gfx_fail("too many viewers (at most %d)", GFX_MAX_VIEWERS)) &&
              gfx_shader_module(spirv, (size_t)nwords * 4, &module) &&
              gfx_pipeline(gx.vs_fullscreen, module, false, gx.rp_clear, &pipe);
    if (module) vkDestroyShaderModule(gx.device, module, NULL);   /* the pipeline keeps its own copy */
    if (!ok) { gfx_copy_err(err, errcap); return 0; }
    gx.viewers[slot].used = true;
    gx.viewers[slot].pipeline = pipe;
    return slot + 1;
}

static GfxViewer *gfx_viewer_get(int viewer) {
    if (!gx.device || viewer < 1 || viewer > GFX_MAX_VIEWERS || !gx.viewers[viewer - 1].used) return NULL;
    return &gx.viewers[viewer - 1];
}

void gfx_viewer_destroy(int viewer) {
    GfxViewer *v = gfx_viewer_get(viewer);
    if (!v) return;
    GfxGarbage g = { 0 };
    g.pipeline = v->pipeline;
    gfx_defer(g);
    memset(v, 0, sizeof *v);
    gfx_collect_garbage();
}

static GfxTexture *gfx_target_get(int target) {
    if (!gx.device || target < 1 || target > GFX_MAX_TARGETS || !gx.targets[target - 1].used) return NULL;
    return &gx.targets[target - 1];
}

/* Texture descriptor set for a draw-list texture id (0 = atlas), or NULL. */
static VkDescriptorSet gfx_texture_set(int id) {
    if (id == 0) return gx.atlas.set;
    GfxTexture *t = gfx_target_get(id);
    return t ? t->set : VK_NULL_HANDLE;
}

int gfx_target_create(int w, int h) {
    if (!gx.device) { gfx_warn("gfx_target_create: not initialized"); return 0; }
    int max = (int)gx.props.limits.maxImageDimension2D;
    if (w <= 0 || h <= 0 || w > max || h > max) {
        gfx_fail("invalid target size %dx%d (largest supported: %d)", w, h, max);
        gfx_warn("%s", gx.err);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < GFX_MAX_TARGETS && slot < 0; i++) if (!gx.targets[i].used) slot = i;
    if (slot < 0) {
        gfx_fail("too many render targets (at most %d)", GFX_MAX_TARGETS);
        gfx_warn("%s", gx.err);
        return 0;
    }
    GfxTexture *t = &gx.targets[slot];
    if (!gfx_texture_create(t, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true)) {
        gfx_warn("gfx_target_create: %s", gx.err);
        return 0;
    }
    /* Clear to opaque black; this also moves it to SHADER_READ_ONLY_OPTIMAL. */
    VkCommandBuffer cmd = gfx_sync_begin();
    bool ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        VkClearValue clear = gfx_clear_color(0, 0, 0, 1);
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = gx.rp_clear;
        bi.framebuffer = t->fb;
        bi.renderArea.extent.width = (u32)w;
        bi.renderArea.extent.height = (u32)h;
        bi.clearValueCount = 1;
        bi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);
        ok = gfx_sync_end();
    }
    if (!ok) {
        gfx_warn("gfx_target_create: %s", gx.err);
        gfx_texture_destroy_now(t);    /* waited for (or the device is lost) */
        return 0;
    }
    return slot + 1;
}

void gfx_target_destroy(int target) {
    GfxTexture *t = gfx_target_get(target);
    if (!t) return;
    gfx_defer(gfx_texture_garbage(t));
    memset(t, 0, sizeof *t);
    gfx_collect_garbage();
}

bool gfx_target_size(int target, int *w, int *h) {
    GfxTexture *t = gfx_target_get(target);
    if (w) *w = t ? t->w : 0;
    if (h) *h = t ? t->h : 0;
    return t != NULL;
}

/* ======================================================================
 * 8. Command recording
 * ====================================================================== */

/* The rectangle of `t` that a viewer render covers; false if it is empty. */
static bool gfx_viewer_region(const GfxTexture *t, const GfxViewerParams *p, VkRect2D *area, bool *full) {
    int x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;
    if (p->x1 > p->x0) {
        x0 = FS_CLAMP(p->x0, 0, t->w);
        x1 = FS_CLAMP(p->x1, 0, t->w);
        y0 = FS_CLAMP(p->y0, 0, t->h);
        y1 = FS_CLAMP(p->y1, 0, t->h);
    }
    if (x1 <= x0 || y1 <= y0) return false;
    *full = x0 == 0 && y0 == 0 && x1 == t->w && y1 == t->h;
    area->offset.x = x0;
    area->offset.y = y0;
    area->extent.width = (u32)(x1 - x0);
    area->extent.height = (u32)(y1 - y0);
    return true;
}

/* Viewer render into a target. The viewport is always the whole target (so
   gl_FragCoord is in target pixels); render area and scissor limit the region.
   A partial region loads the old contents, so pixels outside it are preserved. */
static void gfx_record_viewer(VkCommandBuffer cmd, const GfxTexture *t, const GfxViewer *v,
                              const GfxViewerParams *p, VkDescriptorSet globals) {
    VkRect2D area;
    bool full;
    if (!gfx_viewer_region(t, p, &area, &full)) return;
    VkClearValue clear = gfx_clear_color(0, 0, 0, 1);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = full ? gx.rp_clear : gx.rp_load;
    bi.framebuffer = t->fb;
    bi.renderArea = area;
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
    VkViewport vp = { 0.0f, 0.0f, (float)t->w, (float)t->h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &area);
    float pc[4] = { p->width, p->height, p->offset_x, p->offset_y };
    vkCmdPushConstants(cmd, gx.viewer_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gx.viewer_layout, 0, 1, &globals, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

/* Validates a draw list and copies it into the given buffers (which the GPU must not
   be using). False when there is nothing (valid) to draw. */
static bool gfx_upload_drawlist(const GfxDrawList *dl, GfxHostBuffer *vb, GfxHostBuffer *ib) {
    if (!dl || dl->ncmds <= 0 || dl->nverts <= 0 || dl->nindices <= 0 || !dl->verts || !dl->indices || !dl->cmds)
        return false;
    for (int i = 0; i < dl->nindices; i++)
        if (dl->indices[i] >= (u32)dl->nverts) {
            gfx_warn("draw list index %d is out of range (%u, %d vertices); list not drawn", i, dl->indices[i], dl->nverts);
            return false;
        }
    VkDeviceSize vsize = (VkDeviceSize)dl->nverts * sizeof(GfxVertex);
    VkDeviceSize isize = (VkDeviceSize)dl->nindices * sizeof(u32);
    if (!gfx_hostbuf_reserve(vb, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false) ||
        !gfx_hostbuf_reserve(ib, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false)) {
        gfx_warn("draw list upload: %s", gx.err);
        return false;
    }
    memcpy(vb->map, dl->verts, (size_t)vsize);
    memcpy(ib->map, dl->indices, (size_t)isize);
    return true;
}

static float gfx_clampf(float v, float lo, float hi) {
    if (!(v > lo)) return lo;   /* also NaN */
    return v > hi ? hi : v;
}

/* Records an uploaded draw list inside a render pass of size fbw x fbh.
   Commands sampling texture `skip` (the target being drawn into) are skipped. */
static void gfx_record_drawlist(VkCommandBuffer cmd, const GfxDrawList *dl, int fbw, int fbh, VkPipeline pipe,
                                const GfxHostBuffer *vb, const GfxHostBuffer *ib, int skip) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0.0f, 0.0f, (float)fbw, (float)fbh, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    float pc[4] = { 2.0f / (float)fbw, 2.0f / (float)fbh, -1.0f, -1.0f };  /* pixels -> NDC */
    vkCmdPushConstants(cmd, gx.ui_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buf, &zero);
    vkCmdBindIndexBuffer(cmd, ib->buf, 0, VK_INDEX_TYPE_UINT32);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (int i = 0; i < dl->ncmds; i++) {
        const GfxCmd *c = &dl->cmds[i];
        if (c->index_count == 0) continue;
        if ((u64)c->first_index + c->index_count > (u64)dl->nindices) {
            gfx_warn("draw command %d exceeds the index list; skipped", i);
            continue;
        }
        if (skip > 0 && c->texture == skip) continue;
        VkDescriptorSet set = gfx_texture_set(c->texture);
        if (!set) { gfx_warn("draw command %d uses unknown texture %d; skipped", i, c->texture); continue; }
        /* Scissor: the pixels whose centers lie inside the clip rectangle. */
        int x0 = (int)ceilf(gfx_clampf(c->clip_x0, 0, (float)fbw) - 0.5f);
        int y0 = (int)ceilf(gfx_clampf(c->clip_y0, 0, (float)fbh) - 0.5f);
        int x1 = (int)ceilf(gfx_clampf(c->clip_x1, 0, (float)fbw) - 0.5f);
        int y1 = (int)ceilf(gfx_clampf(c->clip_y1, 0, (float)fbh) - 0.5f);
        x0 = FS_MAX(x0, 0);
        y0 = FS_MAX(y0, 0);
        if (x1 <= x0 || y1 <= y0) continue;
        VkRect2D sc = { { x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) } };
        vkCmdSetScissor(cmd, 0, 1, &sc);
        if (set != bound) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gx.ui_layout, 0, 1, &set, 0, NULL);
            bound = set;
        }
        vkCmdDrawIndexed(cmd, c->index_count, 1, c->first_index, 0, 0);
    }
}

/* Copies rows [y0, y1) of a target into out (rows of t->w pixels, starting at row y0). */
static bool gfx_read_rows(const GfxTexture *t, int y0, int y1, u8 *out) {
    VkDeviceSize size = (VkDeviceSize)t->w * (VkDeviceSize)(y1 - y0) * 4;
    if (!gfx_hostbuf_reserve(&gx.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        return false;
    VkCommandBuffer cmd = gfx_sync_begin();
    if (!cmd) return false;
    gfx_image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.y = y0;
    region.imageExtent.width = (u32)t->w;
    region.imageExtent.height = (u32)(y1 - y0);
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gx.staging.buf, 1, &region);
    gfx_image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = gx.staging.buf;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    if (!gfx_sync_end()) return false;
    memcpy(out + (size_t)y0 * (size_t)t->w * 4, gx.staging.map, (size_t)size);
    return true;
}

/* ======================================================================
 * 9. Windowed frames
 * ====================================================================== */

bool gfx_frame_begin(void) {
    if (!gx.device || gx.fatal[0]) return false;
    if (!gx.window) { gfx_warn("gfx_frame_begin: no window (initialized headless)"); return false; }
    if (gx.recording) { gfx_warn("gfx_frame_begin: the previous frame was not ended"); return false; }
    if (gx.batching) { gfx_warn("gfx_frame_begin: a batch is open (call gfx_batch_end first)"); return false; }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(gx.window, &fbw, &fbh);
    if (fbw <= 0 || fbh <= 0) return false;   /* minimized */
    if (!gx.swapchain || gx.sc_dirty || fbw != gx.sc_fb_w || fbh != gx.sc_fb_h) {
        int r = gfx_swapchain_build();
        if (r < 0) gfx_warn("swapchain: %s", gx.err);
        if (r <= 0) return false;
    }

    GfxFrame *f = &gx.frames[gx.frame];
    if (!gfx_vk(vkWaitForFences(gx.device, 1, &f->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
        gfx_warn("%s", gx.err);
        return false;
    }
    gx.completed = FS_MAX(gx.completed, f->serial);
    gfx_collect_garbage();

    VkResult r = vkAcquireNextImageKHR(gx.device, gx.swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE,
                                       &gx.image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { gx.sc_dirty = true; return false; }
    if (r == VK_SUBOPTIMAL_KHR) gx.sc_dirty = true;   /* still usable; rebuilt after this frame */
    else if (!gfx_vk(r, "vkAcquireNextImageKHR")) { gfx_warn("%s", gx.err); gx.sc_dirty = true; return false; }

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!gfx_vk(vkResetDescriptorPool(gx.device, f->dpool, 0), "vkResetDescriptorPool") ||
        !gfx_vk(vkBeginCommandBuffer(f->cmd, &bi), "vkBeginCommandBuffer")) {
        /* The acquired image cannot be given back without a submit; make the next
           frame rebuild the swapchain, which waits and resets everything. */
        gfx_warn("%s", gx.err);
        gx.sc_dirty = true;
        return false;
    }
    gfx_rec_reset(f);
    gx.recording = true;
    gx.drawn = false;
    gx.frame_rendered = false;
    return true;
}

void gfx_frame_render_viewer(int target, int viewer, const GfxViewerParams *p) {
    if (!gx.recording) { gfx_warn("gfx_frame_render_viewer outside gfx_frame_begin/end"); return; }
    if (gx.drawn) { gfx_warn("gfx_frame_render_viewer after gfx_frame_draw; ignored"); return; }
    GfxTexture *t = gfx_target_get(target);
    GfxViewer *v = gfx_viewer_get(viewer);
    if (!t || !v || !p) { gfx_warn("gfx_frame_render_viewer: invalid target %d or viewer %d", target, viewer); return; }
    GfxFrame *f = &gx.frames[gx.frame];
    VkDescriptorSet set = gfx_frame_globals_set(f);
    if (!set) { gfx_warn("gfx_frame_render_viewer: %s", gx.err); return; }
    gfx_record_viewer(f->cmd, t, v, p, set);
    gx.frame_rendered = true;
}

/* Debugging the order of a frame's work (FS_SYNC, see docs/building.md): the views drawn in a frame are sampled by its
   2D pass, which the render passes' external dependencies order. FS_SYNC=barrier adds a pipeline barrier between them;
   FS_SYNC=wait submits the views' work first and waits for it (slow, but leaves nothing to the driver). */
static void gfx_frame_sync_views(GfxFrame *f) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("FS_SYNC");
        mode = !e ? 0 : !strcmp(e, "barrier") ? 1 : !strcmp(e, "wait") ? 2 : 0;
    }
    if (mode == 1) {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    } else if (mode == 2) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f->cmd;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!gfx_vk(vkEndCommandBuffer(f->cmd), "vkEndCommandBuffer") ||
            !gfx_vk(vkQueueSubmit(gx.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit") ||
            !gfx_vk(vkDeviceWaitIdle(gx.device), "vkDeviceWaitIdle") ||
            !gfx_vk(vkBeginCommandBuffer(f->cmd, &bi), "vkBeginCommandBuffer"))
            gfx_warn("FS_SYNC=wait: %s", gx.err);
    }
}

void gfx_frame_draw(const GfxDrawList *dl, const float clear_rgb[3]) {
    if (!gx.recording) { gfx_warn("gfx_frame_draw outside gfx_frame_begin/end"); return; }
    if (gx.drawn) { gfx_warn("gfx_frame_draw called twice in one frame; ignored"); return; }
    GfxFrame *f = &gx.frames[gx.frame];
    if (gx.frame_rendered) gfx_frame_sync_views(f);
    /* This slot's buffers are free: its previous submission was waited for in begin. */
    bool have = gfx_upload_drawlist(dl, &f->verts, &f->indices);
    VkClearValue clear = clear_rgb ? gfx_clear_color(clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f)
                                   : gfx_clear_color(0, 0, 0, 1);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = gx.rp_swap;
    bi.framebuffer = gx.sc_fbs[gx.image_index];
    bi.renderArea.extent = gx.sc_extent;
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(f->cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (have)
        gfx_record_drawlist(f->cmd, dl, (int)gx.sc_extent.width, (int)gx.sc_extent.height, gx.ui_pipe_swap,
                            &f->verts, &f->indices, 0);
    vkCmdEndRenderPass(f->cmd);
    gx.drawn = true;
}

void gfx_frame_end(void) {
    if (!gx.recording) { gfx_warn("gfx_frame_end without gfx_frame_begin"); return; }
    if (!gx.drawn) gfx_frame_draw(NULL, NULL);   /* the image must be rendered before present */
    GfxFrame *f = &gx.frames[gx.frame];
    gx.recording = false;
    gx.frame = (gx.frame + 1) % GFX_FRAMES;

    bool ok = gfx_vk(vkEndCommandBuffer(f->cmd), "vkEndCommandBuffer") &&
              gfx_vk(vkResetFences(gx.device, 1, &f->fence), "vkResetFences");
    if (ok) {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f->cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &gx.sc_present[gx.image_index];
        ok = gfx_vk(vkQueueSubmit(gx.queue, 1, &si, f->fence), "vkQueueSubmit");
    }
    if (ok) f->serial = ++gx.submitted;
    gfx_resolve_pending();
    if (!ok) {
        /* The fence may now never be signaled: stop rendering frames. */
        if (!gx.fatal[0]) snprintf(gx.fatal, sizeof gx.fatal, "%s", gx.err);
        gfx_warn("fatal: %s", gx.fatal);
        return;
    }

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &gx.sc_present[gx.image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &gx.swapchain;
    pi.pImageIndices = &gx.image_index;
    VkResult r = vkQueuePresentKHR(gx.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) gx.sc_dirty = true;
    else if (!gfx_vk(r, "vkQueuePresentKHR")) { gfx_warn("%s", gx.err); gx.sc_dirty = true; }
}

/* ======================================================================
 * 10. Synchronous API
 * ====================================================================== */

bool gfx_target_render(int target, int viewer, const GfxViewerParams *p, char *err, int errcap) {
    GfxTexture *t = gfx_target_get(target);
    GfxViewer *v = gfx_viewer_get(viewer);
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = (gx.device || gfx_fail("gfx is not initialized")) &&
              (t || gfx_fail("invalid target %d", target)) &&
              (v || gfx_fail("invalid viewer %d", viewer)) &&
              (p || gfx_fail("no viewer parameters")) &&
              (set = gfx_sync_globals_set()) != VK_NULL_HANDLE &&
              (cmd = gfx_sync_begin()) != VK_NULL_HANDLE;
    if (ok) {
        gfx_record_viewer(cmd, t, v, p, set);
        ok = gfx_sync_end();
    }
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}

bool gfx_target_draw(int target, const GfxDrawList *dl, const float clear_rgb[3], char *err, int errcap) {
    GfxTexture *t = gfx_target_get(target);
    if (!gx.device || !t) {
        gfx_fail(gx.device ? "invalid target %d" : "gfx is not initialized", target);
        gfx_copy_err(err, errcap);
        return false;
    }
    bool have = gfx_upload_drawlist(dl, &gx.sync_verts, &gx.sync_indices);
    VkCommandBuffer cmd = gfx_sync_begin();
    bool ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        VkClearValue clear = clear_rgb ? gfx_clear_color(clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f)
                                       : gfx_clear_color(0, 0, 0, 1);
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = gx.rp_clear;
        bi.framebuffer = t->fb;
        bi.renderArea.extent.width = (u32)t->w;
        bi.renderArea.extent.height = (u32)t->h;
        bi.clearValueCount = 1;
        bi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        if (have) gfx_record_drawlist(cmd, dl, t->w, t->h, gx.ui_pipe_target, &gx.sync_verts, &gx.sync_indices, target);
        vkCmdEndRenderPass(cmd);
        ok = gfx_sync_end();
    }
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}

bool gfx_target_read(int target, u8 *rgba_out, char *err, int errcap) {
    GfxTexture *t = gfx_target_get(target);
    bool ok = (gx.device || gfx_fail("gfx is not initialized")) &&
              (t || gfx_fail("invalid target %d", target)) &&
              (rgba_out || gfx_fail("no output buffer")) &&
              gfx_read_rows(t, 0, t->h, rgba_out);
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}

bool gfx_render_to_memory(int viewer, int w, int h, int band_rows, u8 *rgba_out, char *err, int errcap) {
    int max = gx.device ? (int)gx.props.limits.maxImageDimension2D : 0;
    bool ok = (gx.device || gfx_fail("gfx is not initialized")) &&
              (gfx_viewer_get(viewer) || gfx_fail("invalid viewer %d", viewer)) &&
              (rgba_out || gfx_fail("no output buffer")) &&
              ((w > 0 && h > 0 && w <= max && h <= max) ||
               gfx_fail("invalid image size %dx%d (largest supported: %d)", w, h, max));
    int target = ok ? gfx_target_create(w, h) : 0;
    if (ok && !target) ok = false;   /* gx.err says why */
    if (band_rows <= 0 || band_rows > h) band_rows = h;
    GfxViewerParams p;
    memset(&p, 0, sizeof p);
    p.width = (float)w;
    p.height = (float)h;
    for (int y = 0; ok && y < h; y += band_rows) {
        p.x0 = 0;
        p.x1 = w;
        p.y0 = y;
        p.y1 = FS_MIN(h, y + band_rows);
        /* Each band is one submission that is waited for, then copied out. */
        ok = gfx_target_render(target, viewer, &p, NULL, 0) && gfx_read_rows(&gx.targets[target - 1], p.y0, p.y1, rgba_out);
    }
    if (target) {
        char keep[sizeof gx.err];
        snprintf(keep, sizeof keep, "%s", gx.err);
        gfx_target_destroy(target);
        snprintf(gx.err, sizeof gx.err, "%s", keep);
    }
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}

/* ======================================================================
 * 11. Compute: storage buffers, compute pipelines, recording, batches
 * ====================================================================== */

static GfxStorage *gfx_storage_get(int buf) {
    if (!gx.device || buf < 1 || buf > GFX_MAX_BUFFERS || !gx.buffers[buf - 1].used) return NULL;
    return &gx.buffers[buf - 1];
}

static GfxCompute *gfx_compute_get(int pipe) {
    if (!gx.device || pipe < 1 || pipe > GFX_MAX_COMPUTES || !gx.computes[pipe - 1].used) return NULL;
    return &gx.computes[pipe - 1];
}

/* Ends a compute or transfer command: its writes (src_access in src_stage) become
   visible to all later compute shaders and transfers, which also wait for it
   (so later writes cannot overtake its reads). Covers later submissions too. */
static void gfx_compute_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src_access) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

u64 gfx_max_buffer_size(void) {
    return gx.device ? (u64)gx.props.limits.maxStorageBufferRange & ~(u64)3 : 0;
}

int gfx_buffer_create(u64 bytes) {
    if (!gx.device) { gfx_warn("gfx_buffer_create: not initialized"); return 0; }
    u64 limit = gfx_max_buffer_size();
    if (bytes == 0 || bytes > limit) {
        if (bytes == 0) gfx_fail("invalid storage buffer size 0");
        else gfx_fail("a storage buffer of %llu bytes exceeds this GPU's limit of %llu bytes (maxStorageBufferRange)",
                      (unsigned long long)bytes, (unsigned long long)limit);
        gfx_warn("gfx_buffer_create: %s", gx.err);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < GFX_MAX_BUFFERS && slot < 0; i++) if (!gx.buffers[i].used) slot = i;
    if (slot < 0) {
        gfx_fail("too many storage buffers (at most %d)", GFX_MAX_BUFFERS);
        gfx_warn("gfx_buffer_create: %s", gx.err);
        return 0;
    }
    GfxStorage *b = &gx.buffers[slot];
    memset(b, 0, sizeof *b);
    b->size = (bytes + 3) & ~(VkDeviceSize)3;   /* vkCmdFillBuffer works in 4-byte words */
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = b->size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements req;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = gfx_vk(vkCreateBuffer(gx.device, &ci, NULL, &b->buf), "vkCreateBuffer");
    if (ok) {
        vkGetBufferMemoryRequirements(gx.device, b->buf, &req);
        ok = gfx_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &b->mem) &&
             gfx_vk(vkBindBufferMemory(gx.device, b->buf, b->mem, 0), "vkBindBufferMemory") &&
             (cmd = gfx_sync_begin()) != VK_NULL_HANDLE;
    }
    if (ok) {   /* zero-fill */
        vkCmdFillBuffer(cmd, b->buf, 0, VK_WHOLE_SIZE, 0);
        gfx_compute_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        ok = gfx_sync_end();
    }
    if (!ok) {   /* never used by the GPU, or waited for (or the device is lost) */
        gfx_warn("gfx_buffer_create: %s", gx.err);
        vkDestroyBuffer(gx.device, b->buf, NULL);
        vkFreeMemory(gx.device, b->mem, NULL);
        memset(b, 0, sizeof *b);
        return 0;
    }
    b->used = true;
    b->uid = ++gx.next_uid;
    return slot + 1;
}

void gfx_buffer_destroy(int buf) {
    GfxStorage *b = gfx_storage_get(buf);
    if (!b) return;
    GfxGarbage g = { 0 };
    g.buffer = b->buf;
    g.memory = b->mem;
    gfx_defer(g);
    memset(b, 0, sizeof *b);
    gfx_collect_garbage();
}

u64 gfx_buffer_size(int buf) {
    GfxStorage *b = gfx_storage_get(buf);
    return b ? b->size : 0;
}

/* Synchronous copy between storage buffer `b` and host memory: from `src` (upload)
   or into `dst` (download), through the staging buffer in chunks of at most
   GFX_TRANSFER_CHUNK bytes (one submission each). */
static bool gfx_storage_transfer(GfxStorage *b, VkDeviceSize offset, const u8 *src, u8 *dst, VkDeviceSize bytes) {
    bool upload = src != NULL;
    VkDeviceSize chunk = FS_MIN(bytes, GFX_TRANSFER_CHUNK);
    if (!gfx_hostbuf_reserve(&gx.staging, chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        return false;
    for (VkDeviceSize done = 0; done < bytes; done += chunk) {
        VkDeviceSize n = FS_MIN(chunk, bytes - done);
        if (upload) memcpy(gx.staging.map, src + done, (size_t)n);
        VkCommandBuffer cmd = gfx_sync_begin();
        if (!cmd) return false;
        /* After all earlier work on the buffer (its writes, and reads that this upload must not overtake). */
        gfx_compute_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (upload) {
            VkBufferCopy region = { 0, offset + done, n };
            vkCmdCopyBuffer(cmd, gx.staging.buf, b->buf, 1, &region);
            gfx_compute_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        } else {
            VkBufferCopy region = { offset + done, 0, n };
            vkCmdCopyBuffer(cmd, b->buf, gx.staging.buf, 1, &region);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        if (!gfx_sync_end()) return false;
        if (!upload) memcpy(dst + done, gx.staging.map, (size_t)n);
    }
    return true;
}

/* Shared argument checks of gfx_buffer_upload / gfx_buffer_download. */
static bool gfx_buffer_io(int buf, u64 offset, const u8 *src, u8 *dst, u64 bytes, char *err, int errcap) {
    GfxStorage *b = gfx_storage_get(buf);
    bool ok = (gx.device || gfx_fail("gfx is not initialized")) &&
              (b || gfx_fail("invalid storage buffer %d", buf)) &&
              (src || dst || bytes == 0 || gfx_fail("no data")) &&
              ((offset <= b->size && bytes <= b->size - offset) ||
               gfx_fail("bytes [%llu, %llu) are outside storage buffer %d (%llu bytes)", (unsigned long long)offset,
                        (unsigned long long)(offset + bytes), buf, (unsigned long long)b->size)) &&
              (bytes == 0 || gfx_storage_transfer(b, offset, src, dst, bytes));
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}

bool gfx_buffer_upload(int buf, u64 offset, const void *data, u64 bytes, char *err, int errcap) {
    return gfx_buffer_io(buf, offset, (const u8 *)data, NULL, bytes, err, errcap);
}

bool gfx_buffer_download(int buf, u64 offset, void *data, u64 bytes, char *err, int errcap) {
    return gfx_buffer_io(buf, offset, NULL, (u8 *)data, bytes, err, errcap);
}

int gfx_compute_create(const GfxComputeDesc *d, char *err, int errcap) {
    if (!gx.device) { gfx_fail("gfx is not initialized"); gfx_copy_err(err, errcap); return 0; }
    const VkPhysicalDeviceLimits *lim = &gx.props.limits;
    int slot = -1;
    for (int i = 0; i < GFX_MAX_COMPUTES && slot < 0; i++) if (!gx.computes[i].used) slot = i;
    bool ok = (d || gfx_fail("no compute pipeline description")) &&
              gfx_check_spirv(d->spirv, d->nwords, 5) &&
              ((d->nbuffers >= 0 && d->nbuffers <= GFX_COMPUTE_MAX_BUFFERS) ||
               gfx_fail("nbuffers = %d: a compute pipeline takes 0 to %d storage buffers", d->nbuffers,
                        GFX_COMPUTE_MAX_BUFFERS)) &&
              ((u32)d->nbuffers + 1 <= lim->maxPerStageDescriptorStorageBuffers ||
               gfx_fail("this GPU allows at most %u storage buffers per shader (the globals buffer included)",
                        lim->maxPerStageDescriptorStorageBuffers)) &&
              ((d->push_bytes <= 128 && d->push_bytes <= lim->maxPushConstantsSize && d->push_bytes % 4 == 0) ||
               gfx_fail("push_bytes = %u: push constants take a multiple of 4 bytes, at most 128", d->push_bytes)) &&
              (slot >= 0 || gfx_fail("too many compute pipelines (at most %d)", GFX_MAX_COMPUTES));
    if (!ok) { gfx_copy_err(err, errcap); return 0; }

    GfxCompute c;
    memset(&c, 0, sizeof c);
    c.nbuffers = d->nbuffers;
    c.push_bytes = d->push_bytes;
    /* Set 0: binding 0 = globals (dynamic offset: the chunk of the recording's globals
       buffer holding the current gfx_set_globals version), bindings 1..n = buffers. */
    VkDescriptorSetLayoutBinding b[1 + GFX_COMPUTE_MAX_BUFFERS];
    memset(b, 0, sizeof b);
    for (int i = 0; i <= c.nbuffers; i++) {
        b[i].binding = (u32)i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = (u32)c.nbuffers + 1;
    dci.pBindings = b;
    VkPushConstantRange pc = { VK_SHADER_STAGE_COMPUTE_BIT, 0, c.push_bytes };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &c.dsl;
    lci.pushConstantRangeCount = c.push_bytes ? 1 : 0;
    lci.pPushConstantRanges = &pc;
    VkShaderModule module = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.pName = "main";
    ok = gfx_vk(vkCreateDescriptorSetLayout(gx.device, &dci, NULL, &c.dsl), "vkCreateDescriptorSetLayout") &&
         gfx_vk(vkCreatePipelineLayout(gx.device, &lci, NULL, &c.layout), "vkCreatePipelineLayout") &&
         gfx_shader_module(d->spirv, (size_t)d->nwords * 4, &module);
    if (ok) {
        pci.stage.module = module;
        pci.layout = c.layout;
        ok = gfx_vk(vkCreateComputePipelines(gx.device, VK_NULL_HANDLE, 1, &pci, NULL, &c.pipeline),
                    "vkCreateComputePipelines");
    }
    if (module) vkDestroyShaderModule(gx.device, module, NULL);   /* the pipeline keeps its own copy */
    if (!ok) {   /* nothing was used by the GPU */
        vkDestroyPipeline(gx.device, c.pipeline, NULL);
        vkDestroyPipelineLayout(gx.device, c.layout, NULL);
        vkDestroyDescriptorSetLayout(gx.device, c.dsl, NULL);
        gfx_copy_err(err, errcap);
        return 0;
    }
    c.used = true;
    c.uid = ++gx.next_uid;
    gx.computes[slot] = c;
    return slot + 1;
}

void gfx_compute_destroy(int pipe) {
    GfxCompute *c = gfx_compute_get(pipe);
    if (!c) return;
    GfxGarbage g = { 0 };
    g.pipeline = c->pipeline;
    g.layout = c->layout;
    g.dsl = c->dsl;
    gfx_defer(g);
    memset(c, 0, sizeof *c);
    gfx_collect_garbage();
}

/* The recording that gfx_frame_fill_buffer / dispatch / copies go into: the open
   batch, else the frame being recorded (before gfx_frame_draw); else NULL. */
static GfxFrame *gfx_rec(const char *fn) {
    if (gx.batching) return &gx.batch;
    if (gx.recording && !gx.drawn) return &gx.frames[gx.frame];
    gfx_warn(gx.recording ? "%s after gfx_frame_draw; ignored" : "%s outside a frame or batch; ignored", fn);
    return NULL;
}

/* A recording error: the command is skipped. In a batch it makes gfx_batch_end fail
   (with the first message); in a frame it is printed to stderr. */
static void gfx_rec_fail(const char *fmt, ...) FS_PRINTF(1, 2);
static void gfx_rec_fail(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!gx.batching) gfx_warn("%s", msg);
    else if (gx.batch_errors++ == 0) snprintf(gx.batch_err, sizeof gx.batch_err, "%s", msg);
}

/* The descriptor set binding pipeline `c`'s buffers `sb` and the current globals
   chunk of recording `f` (its buffer; the chunk is picked by the dynamic offset).
   Cached for the rest of the recording, so repeated dispatches allocate nothing. */
static VkDescriptorSet gfx_compute_set(GfxFrame *f, const GfxCompute *c, GfxStorage *const *sb) {
    for (int i = f->csets.len - 1; i >= 0; i--) {
        const GfxComputeSet *s = &f->csets.data[i];
        if (s->pipe_uid != c->uid || s->globals != f->globals.buf || s->globals_range != f->gchunk_size) continue;
        int k = 0;
        while (k < c->nbuffers && s->buf_uid[k] == sb[k]->uid) k++;
        if (k == c->nbuffers) return s->set;
    }
    /* Allocate from the current pool, moving on to a new one when it is full. Each set
       takes at most 1 + GFX_COMPUTE_MAX_BUFFERS descriptors, which the pools are sized for. */
    if (f->cpool_sets == GFX_COMPUTE_POOL_SETS) {
        f->cpool++;
        f->cpool_sets = 0;
    }
    if (f->cpool == f->cpools.len) {
        VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, GFX_COMPUTE_POOL_SETS },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GFX_COMPUTE_POOL_SETS * GFX_COMPUTE_MAX_BUFFERS },
        };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = GFX_COMPUTE_POOL_SETS;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        VkDescriptorPool pool;
        if (!gfx_vk(vkCreateDescriptorPool(gx.device, &pci, NULL, &pool), "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
        vec_push(f->cpools, pool);
    }
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = f->cpools.data[f->cpool];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c->dsl;
    GfxComputeSet s;
    memset(&s, 0, sizeof s);
    if (!gfx_vk(vkAllocateDescriptorSets(gx.device, &ai, &s.set), "vkAllocateDescriptorSets")) return VK_NULL_HANDLE;
    f->cpool_sets++;

    VkDescriptorBufferInfo bi[1 + GFX_COMPUTE_MAX_BUFFERS];
    VkWriteDescriptorSet wr[1 + GFX_COMPUTE_MAX_BUFFERS];
    memset(wr, 0, sizeof wr);
    for (int i = 0; i <= c->nbuffers; i++) {
        bi[i].buffer = i == 0 ? f->globals.buf : sb[i - 1]->buf;
        bi[i].offset = 0;
        bi[i].range = i == 0 ? f->gchunk_size : sb[i - 1]->size;
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = s.set;
        wr[i].dstBinding = (u32)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(gx.device, (u32)c->nbuffers + 1, wr, 0, NULL);
    s.pipe_uid = c->uid;
    for (int k = 0; k < c->nbuffers; k++) s.buf_uid[k] = sb[k]->uid;
    s.globals = f->globals.buf;
    s.globals_range = f->gchunk_size;
    vec_push(f->csets, s);
    return s.set;
}

void gfx_frame_fill_buffer(int buf, u64 offset, u64 bytes, u32 value) {
    GfxFrame *f = gfx_rec("gfx_frame_fill_buffer");
    if (!f) return;
    GfxStorage *b = gfx_storage_get(buf);
    if (!b) { gfx_rec_fail("gfx_frame_fill_buffer: invalid storage buffer %d", buf); return; }
    if (offset % 4 || bytes % 4 || offset > b->size || bytes > b->size - offset) {
        gfx_rec_fail("gfx_frame_fill_buffer: bytes [%llu, %llu) are outside storage buffer %d (%llu bytes) "
                     "or not multiples of 4", (unsigned long long)offset, (unsigned long long)(offset + bytes), buf,
                     (unsigned long long)b->size);
        return;
    }
    if (bytes == 0) return;
    vkCmdFillBuffer(f->cmd, b->buf, offset, bytes, value);
    gfx_compute_barrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
}

void gfx_frame_dispatch(int pipe, const int *bufs, const void *push, u32 nx, u32 ny, u32 nz) {
    GfxFrame *f = gfx_rec("gfx_frame_dispatch");
    if (!f) return;
    if (gx.recording) gx.frame_rendered = true;
    GfxCompute *c = gfx_compute_get(pipe);
    if (!c) { gfx_rec_fail("gfx_frame_dispatch: invalid compute pipeline %d", pipe); return; }
    GfxStorage *sb[GFX_COMPUTE_MAX_BUFFERS];
    for (int i = 0; i < c->nbuffers; i++) {
        sb[i] = bufs ? gfx_storage_get(bufs[i]) : NULL;
        if (!sb[i]) {
            gfx_rec_fail("gfx_frame_dispatch: invalid storage buffer %d (bufs[%d])", bufs ? bufs[i] : 0, i);
            return;
        }
    }
    const u32 *maxn = gx.props.limits.maxComputeWorkGroupCount;
    if (nx > maxn[0] || ny > maxn[1] || nz > maxn[2]) {
        gfx_rec_fail("gfx_frame_dispatch: %u x %u x %u workgroups exceed this GPU's limit of %u x %u x %u",
                     nx, ny, nz, maxn[0], maxn[1], maxn[2]);
        return;
    }
    if (nx == 0 || ny == 0 || nz == 0) return;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!gfx_frame_globals_chunk(f) || !(set = gfx_compute_set(f, c, sb))) {
        gfx_rec_fail("gfx_frame_dispatch: %s", gx.err);
        return;
    }
    u32 off = (u32)f->gchunk_off;
    if (f->bound_pipe != c->pipeline) {
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
        f->bound_pipe = c->pipeline;
        f->bound_set = VK_NULL_HANDLE;   /* the new pipeline's layout may differ: rebind */
    }
    if (f->bound_set != set || f->bound_offset != off) {
        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1, &set, 1, &off);
        f->bound_set = set;
        f->bound_offset = off;
    }
    if (c->push_bytes) {
        static const u8 zeros[128];
        vkCmdPushConstants(f->cmd, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, c->push_bytes, push ? push : zeros);
    }
    vkCmdDispatch(f->cmd, nx, ny, nz);
    gfx_compute_barrier(f->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
}

/* Checks of the buffer <-> target copies; false (error reported) if invalid. */
static bool gfx_copy_args(const char *fn, int buf, u64 offset, int target, GfxStorage **b, GfxTexture **t) {
    *b = gfx_storage_get(buf);
    *t = gfx_target_get(target);
    if (!*b || !*t) {
        gfx_rec_fail("%s: invalid %s %d", fn, *b ? "target" : "storage buffer", *b ? target : buf);
        return false;
    }
    u64 need = (u64)(*t)->w * (u64)(*t)->h * 4;
    if (offset % 4 || offset > (*b)->size || need > (*b)->size - offset) {
        gfx_rec_fail("%s: storage buffer %d (%llu bytes) has no room for %dx%d RGBA8 pixels at offset %llu%s", fn, buf,
                     (unsigned long long)(*b)->size, (*t)->w, (*t)->h, (unsigned long long)offset,
                     offset % 4 ? " (the offset must be a multiple of 4)" : "");
        return false;
    }
    return true;
}

/* Before a copy to/from a target: SHADER_READ_ONLY -> `layout`, after earlier
   color writes, shader reads, and copies of the image. */
static void gfx_target_copy_begin(VkCommandBuffer cmd, const GfxTexture *t, VkImageLayout layout) {
    gfx_image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layout,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT
                                                                     : VK_ACCESS_TRANSFER_READ_BIT);
}

/* After a copy to/from a target: back to SHADER_READ_ONLY, visible to later sampling,
   copies and render passes; the buffer side gets the usual compute barrier. */
static void gfx_target_copy_end(VkCommandBuffer cmd, const GfxTexture *t, VkImageLayout layout) {
    bool to_image = layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcAccessMask = to_image ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ib.oldLayout = layout;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->image;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = to_image ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 1, &ib);
}

static VkBufferImageCopy gfx_target_region(const GfxTexture *t, u64 offset) {
    VkBufferImageCopy r;
    memset(&r, 0, sizeof r);
    r.bufferOffset = offset;   /* rows tightly packed: bufferRowLength = bufferImageHeight = 0 */
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = (u32)t->w;
    r.imageExtent.height = (u32)t->h;
    r.imageExtent.depth = 1;
    return r;
}

void gfx_frame_buffer_to_target(int buf, u64 offset, int target) {
    GfxFrame *f = gfx_rec("gfx_frame_buffer_to_target");
    GfxStorage *b;
    GfxTexture *t;
    if (!f || !gfx_copy_args("gfx_frame_buffer_to_target", buf, offset, target, &b, &t)) return;
    if (gx.recording) gx.frame_rendered = true;
    VkBufferImageCopy region = gfx_target_region(t, offset);
    gfx_target_copy_begin(f->cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(f->cmd, b->buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    gfx_target_copy_end(f->cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

void gfx_frame_target_to_buffer(int target, int buf, u64 offset) {
    GfxFrame *f = gfx_rec("gfx_frame_target_to_buffer");
    GfxStorage *b;
    GfxTexture *t;
    if (!f || !gfx_copy_args("gfx_frame_target_to_buffer", buf, offset, target, &b, &t)) return;
    VkBufferImageCopy region = gfx_target_region(t, offset);
    gfx_target_copy_begin(f->cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(f->cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buf, 1, &region);
    gfx_target_copy_end(f->cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

bool gfx_batch_begin(void) {
    if (!gx.device || gx.fatal[0]) {
        gfx_warn("gfx_batch_begin: %s", gx.device ? gx.fatal : "not initialized");
        return false;
    }
    if (gx.batching || gx.recording) {
        gfx_warn("gfx_batch_begin: %s", gx.batching ? "a batch is already open" : "inside a frame (end it first)");
        return false;
    }
    GfxFrame *f = &gx.batch;
    gfx_rec_reset(f);   /* the previous batch was waited for in gfx_batch_end */
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!gfx_vk(vkBeginCommandBuffer(f->cmd, &bi), "vkBeginCommandBuffer")) {
        gfx_warn("gfx_batch_begin: %s", gx.err);
        return false;
    }
    gx.batching = true;
    gx.batch_errors = 0;
    gx.batch_err[0] = 0;
    return true;
}

bool gfx_batch_end(char *err, int errcap) {
    if (!gx.batching) {
        gfx_fail("gfx_batch_end without gfx_batch_begin");
        gfx_copy_err(err, errcap);
        return false;
    }
    GfxFrame *f = &gx.batch;
    gx.batching = false;
    bool ok = gfx_vk(vkEndCommandBuffer(f->cmd), "vkEndCommandBuffer") &&
              gfx_vk(vkResetFences(gx.device, 1, &f->fence), "vkResetFences");
    if (ok) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f->cmd;
        ok = gfx_vk(vkQueueSubmit(gx.queue, 1, &si, f->fence), "vkQueueSubmit");
    }
    if (ok) f->serial = ++gx.submitted;
    gfx_resolve_pending();
    if (ok && (ok = gfx_vk(vkWaitForFences(gx.device, 1, &f->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))) {
        gx.completed = f->serial;
        gfx_collect_garbage();
    }
    if (ok && gx.batch_errors > 0) {
        if (gx.batch_errors == 1) gfx_fail("%s", gx.batch_err);
        else gfx_fail("%s (and %d more errors)", gx.batch_err, gx.batch_errors - 1);
        ok = false;
    }
    if (!ok) gfx_copy_err(err, errcap);
    return ok;
}
