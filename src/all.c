/* all.c - the unity build: every module of FractalStream, in dependency order.
 * src/main.c and tests/test_main.c include this file.
 * (__has_include guards are temporary while modules are being written in parallel.) */
#include "base.c"

#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#undef STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION

/* language */
#include "ast.h"
#include "lex.c"
#include "builtins.c"
#include "parse.c"
#include "check.c"
#include "interp.c"
#if __has_include("spirv.c")
#include "spirv.c"
#endif
#if __has_include("codegen.c")
#include "codegen.c"
#endif
#include "program.c"
/* graphics */
#if __has_include("gfx.c")
#include "gfx.c"
#endif
/* platform + UI */
#if __has_include("platform.c")
#include "platform.c"
#endif
#if __has_include("ui.c")
#include "ui.c"
#endif
#if __has_include("editor.c")
#include "editor.c"
#endif
/* application */
#if __has_include("app.c") && __has_include("editor.c")
#include "app.c"
#endif
#if __has_include("cli.c")
#include "cli.c"
#endif
