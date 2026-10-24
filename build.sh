#!/bin/sh
# FractalStreamC build script (Linux, macOS, MSYS2/MinGW, and Linux->Windows cross builds).
#
#   ./build.sh            optimized build            -> build/fractalstreamc
#   ./build.sh debug      debug + sanitizers         -> build/fractalstreamc_debug
#   ./build.sh dev        optimized, another name    -> build/fractalstreamc_dev
#   ./build.sh test       build and run test suite   -> build/tests
#   ./build.sh windows    cross-compile with MinGW   -> build/win/fractalstreamc.exe
#   ./build.sh app        macOS app with MoltenVK    -> build/FractalStreamC.app, build/FractalStreamC-macos-<arch>.zip
#                         (tools/make_macos_app.sh; the Linux download: tools/make_linux_release.sh)
#
# Environment: CC (compiler), MOLTENVK (where MoltenVK is, for `app`: see tools/make_macos_app.sh),
# MACOSX_DEPLOYMENT_TARGET (the oldest macOS the program is built for, 11.0 if not set).
set -e
cd "$(dirname "$0")"
MODE=${1:-release}
OS=$(uname -s)
case "$MODE" in windows) OS=Windows ;; esac
case "$OS" in MINGW*|MSYS*|CYGWIN*) OS=Windows ;; esac
if [ "$MODE" = app ] && [ "$OS" != Darwin ]; then
  echo "./build.sh app makes the macOS app: run it on a Mac (the Linux download: tools/make_linux_release.sh)" >&2
  exit 1
fi
# macOS: without this the compiler targets the macOS it runs on, and the app would not start on older ones
[ "$OS" = Darwin ] && export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"

if [ "$OS" = Windows ] && [ "$MODE" = windows ]; then
  CC=${CC:-x86_64-w64-mingw32-gcc}; AR=${AR:-x86_64-w64-mingw32-ar}
else
  CC=${CC:-cc}; AR=${AR:-ar}
fi

OUT=build
[ "$MODE" = windows ] && OUT=build/win
mkdir -p "$OUT/glfw"

GLFW_COMMON="context init input monitor platform vulkan window egl_context osmesa_context null_init null_monitor null_window null_joystick"
case "$OS" in
  Linux)   GLFW_DEF="-D_GLFW_X11 -D_DEFAULT_SOURCE"
           GLFW_SRC="$GLFW_COMMON posix_module posix_time posix_thread posix_poll x11_init x11_monitor x11_window xkb_unicode glx_context linux_joystick"
           LIBS="-lm -ldl -lpthread" ;;
  Darwin)  GLFW_DEF="-D_GLFW_COCOA"
           GLFW_SRC="$GLFW_COMMON cocoa_time posix_module posix_thread cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_window.m nsgl_context.m"
           LIBS="-framework Cocoa -framework IOKit -framework CoreFoundation -framework QuartzCore -lm" ;;
  Windows) GLFW_DEF="-D_GLFW_WIN32"
           GLFW_SRC="$GLFW_COMMON win32_module win32_time win32_thread win32_init win32_joystick win32_monitor win32_window wgl_context"
           LIBS="-lgdi32 -luser32 -lshell32 -lm" ;;
  *)       echo "unsupported OS: $OS"; exit 1 ;;
esac

# GLFW is compiled once per object and cached in $OUT/glfw (compiled again when the compiler or its flags change).
GLFW_BUILD="$CC -O2 $GLFW_DEF${MACOSX_DEPLOYMENT_TARGET:+ (macOS $MACOSX_DEPLOYMENT_TARGET)}"
if [ "$(cat "$OUT/glfw/build.txt" 2>/dev/null)" != "$GLFW_BUILD" ]; then
  rm -f "$OUT"/glfw/*.o
  echo "$GLFW_BUILD" > "$OUT/glfw/build.txt"
fi
OBJS=""
for f in $GLFW_SRC; do
  case "$f" in *.m) src="third_party/glfw/src/$f"; obj="$OUT/glfw/${f%.m}.o" ;;
               *)   src="third_party/glfw/src/$f.c"; obj="$OUT/glfw/$f.o" ;; esac
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    $CC -O2 $GLFW_DEF -c "$src" -o "$obj"
  fi
  OBJS="$OBJS $obj"
done

INC="-Ithird_party/vulkan -Ithird_party/glfw/include -Ithird_party/stb"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -Wno-sign-compare"
case "$MODE" in
  release|app|windows) FLAGS="-O2 -std=c11 $WARN"; EXE=fractalstreamc ;;
  dev)     FLAGS="-O2 -std=c11 $WARN"; EXE=fractalstreamc_dev ;;
  debug)   FLAGS="-O0 -g -std=c11 $WARN -fsanitize=address,undefined"; EXE=fractalstreamc_debug ;;
  test)    FLAGS="-O1 -g -std=c11 $WARN"; EXE=tests ;;
  *)       echo "unknown mode: $MODE"; exit 1 ;;
esac
[ "$OS" = Linux ] && FLAGS="$FLAGS -D_GNU_SOURCE"
[ "$OS" = Windows ] && { EXE="$EXE.exe"; FLAGS="$FLAGS -D__USE_MINGW_ANSI_STDIO=1"; }

MAIN=src/main.c
[ "$MODE" = test ] && MAIN=tests/test_main.c
$CC $FLAGS $INC "$MAIN" $OBJS -o "$OUT/$EXE" $LIBS
echo "built $OUT/$EXE"

if [ "$MODE" = test ]; then
  shift
  "$OUT/$EXE" "$@"
fi
if [ "$MODE" = app ]; then
  sh tools/make_macos_app.sh "$OUT/$EXE"
fi
