#!/bin/sh
# gfx_demo.sh - build and run the windowed renderer smoke test (tests/gfx_demo.c).
#
#   tools/gfx_demo.sh [seconds]      (default 3)
#
# Builds build/gfx_demo from tests/gfx_demo.c (renderer + GLFW only, validation on).
# With a display it just runs it. On Linux without a display it runs it under Xvfb
# and saves a screenshot of the window to build/gfx_demo.png (needs xvfb-run and
# ImageMagick's `import`). Exit status is nonzero on errors or validation errors.
set -e
cd "$(dirname "$0")/.."
SECS=${1:-3}
CC=${CC:-cc}

# build.sh compiles (and caches) the GLFW objects before anything else; the app
# itself may fail to build while it is being worked on, which does not matter here.
./build.sh >/dev/null 2>&1 || true
ls build/glfw/*.o >/dev/null 2>&1 || { echo "gfx_demo.sh: GLFW objects missing (run ./build.sh)"; exit 1; }

case "$(uname -s)" in
  Linux)  DEF="-D_GNU_SOURCE"; LIBS="-lm -ldl -lpthread" ;;
  Darwin) DEF=""; LIBS="-framework Cocoa -framework IOKit -framework CoreFoundation -framework QuartzCore -lm" ;;
  *)      echo "gfx_demo.sh: unsupported OS (on Windows build tests/gfx_demo.c by hand)"; exit 1 ;;
esac
$CC -O1 -g -std=c11 $DEF -DGFX_DEMO_MAIN -Ithird_party/vulkan -Ithird_party/glfw/include -Ithird_party/stb \
    tests/gfx_demo.c build/glfw/*.o -o build/gfx_demo $LIBS
echo "built build/gfx_demo"

if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ] || [ "$(uname -s)" = Darwin ]; then
  exec build/gfx_demo "$SECS"
fi
SHOT_AT=$(awk "BEGIN { print $SECS * 0.85 }")
rm -f build/gfx_demo.png
xvfb-run -a -s "-screen 0 1024x768x24" sh -c \
  "build/gfx_demo $SECS & pid=\$!; sleep $SHOT_AT; import -window root build/gfx_demo.png; wait \$pid"
echo "screenshot: build/gfx_demo.png"
