/* all.c - the unity build: every module of FractalStream, in dependency order.
 * src/main.c and tests/test_main.c include this file. */
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

/* language: lexer -> parser -> checker -> interpreter / SPIR-V */
#include "ast.h"
#include "lex.c"
#include "builtins.c"
#include "parse.c"
#include "check.c"
#include "interp.c"
#include "spirv.c"
#include "codegen.c"
#include "program.c"
/* graphics, platform, UI */
#include "gfx.c"
#include "platform.c"
#include "ui.c"
#include "editor.c"
/* application */
#include "app.c"
#include "cli.c"
