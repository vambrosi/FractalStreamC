# Building FractalStream

FractalStream is a single C11 program (a "unity build": `src/main.c` includes every
module through `src/all.c`). Its only dependencies are vendored in `third_party/`
(GLFW, the Vulkan headers, stb, fonts). Vulkan itself is loaded at run time, so
nothing links against a Vulkan library: the program needs a Vulkan *driver* to run,
but no Vulkan SDK to build.

| platform | compiler | command | output |
|---|---|---|---|
| Linux | gcc or clang | `./build.sh` | `build/fractalstream` |
| macOS | Xcode command line tools | `./build.sh app` | `build/FractalStream.app` |
| Windows | Visual Studio (MSVC) | `build.bat` | `build\fractalstream.exe` |
| Windows | MSYS2 (MinGW gcc) | `./build.sh` | `build/fractalstream.exe` |
| Linux -> Windows | MinGW cross compiler | `./build.sh windows` | `build/win/fractalstream.exe` |

Other `build.sh` modes: `debug` (sanitizers, `build/fractalstream_debug`) and
`test [filter]` (builds and runs the test suite, `build/tests`).
`build.bat` has `debug` and `test [filter]` too.

## Linux

Build dependencies (Debian/Ubuntu names) are a C compiler and the X11 headers GLFW needs:

    sudo apt install build-essential libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev

To run, a Vulkan driver: `libvulkan1` (the loader) plus the GPU's driver, e.g.
`mesa-vulkan-drivers` (Intel, AMD, and `llvmpipe`/lavapipe, a software renderer that
works everywhere, slowly) or NVIDIA's driver. `vulkaninfo --summary` (package
`vulkan-tools`) shows what is installed.

GLFW is built for X11 only; on Wayland desktops the program runs through XWayland.

Then:

    ./build.sh            # build/fractalstream
    ./build.sh test       # run all tests

## macOS

Requirements: the Xcode command line tools (`xcode-select --install`) and MoltenVK,
the Vulkan implementation on top of Metal:

    brew install molten-vk

Then:

    ./build.sh            # build/fractalstream, uses the Homebrew MoltenVK at run time
    ./build.sh app        # build/FractalStream.app with MoltenVK bundled inside

`./build.sh app` runs `tools/make_macos_app.sh`, which takes MoltenVK from `$MOLTENVK`
(the dylib or a directory containing it), else Homebrew, else the LunarG Vulkan SDK
(`$VULKAN_SDK`), else downloads a pinned MoltenVK release into `build/moltenvk/`.
The bundle is self-contained (`Contents/Frameworks/libMoltenVK.dylib`) and ad-hoc
signed, so it can be copied to `/Applications`.

Where the program looks for Vulkan on macOS, in order: `libMoltenVK.dylib` next to the
executable, `../Frameworks/libMoltenVK.dylib` and `../Frameworks/libvulkan.1.dylib`
(inside the .app), then `libvulkan.1.dylib` and `libMoltenVK.dylib` on the default
library path, in `/usr/local/lib` and in `/opt/homebrew/lib`. If a Vulkan loader
(`libvulkan.1.dylib`) is found but cannot find MoltenVK, point the program at MoltenVK
directly: `FS_VULKAN_LIBRARY=/opt/homebrew/lib/libMoltenVK.dylib`.

## Windows

Vulkan comes with the graphics driver (`vulkan-1.dll`), so recent Intel, AMD and NVIDIA
drivers are all that is needed to run.

**Visual Studio** (2019 16.8 or newer, for C11): open a "x64 Native Tools Command Prompt
for VS", then

    build.bat             :: build\fractalstream.exe
    build.bat test        :: run the tests

**MSYS2**: in the "UCRT64" shell, `pacman -S mingw-w64-ucrt-x86_64-gcc`, then `./build.sh`.

**Cross-compiling from Linux**: `sudo apt install gcc-mingw-w64-x86-64`, then
`./build.sh windows`. This build must stay free of warnings.

## Run-time options (environment variables)

| variable | effect |
|---|---|
| `FS_GPU=<text>` | use the GPU whose name contains `<text>` (case-insensitive), e.g. `FS_GPU=intel` |
| `FS_VULKAN_LIBRARY=<path>` | load this Vulkan library (or MoltenVK dylib) before the default ones |
| `FS_VULKAN_VALIDATION=1` | enable the Vulkan validation layer and print its messages |
| `FS_DEBUG=1` | print what the app decides at startup (the GPU; how deep zooms keep their arithmetic exact) |
| `MVK_CONFIG_FAST_MATH_ENABLED` | (macOS) set to `2` by the program unless already set: MoltenVK's fast maths, except in the shaders of deep zooms, which declare IEEE floats (SPIR-V float controls) and then need no per-operation workaround, the slow part of double precision before |

Without `FS_GPU` a discrete GPU is preferred, then an integrated one, then anything
else (e.g. lavapipe).

## Testing graphics

The renderer tests (`./build.sh test gfx`) run headless on any Vulkan device, and are
reported as skipped when there is none. They enable the validation layer when it is
installed and fail on any validation error. On Linux:

    sudo apt install mesa-vulkan-drivers vulkan-validationlayers xvfb imagemagick
    xvfb-run -a ./build.sh test gfx     # includes the windowed test (swapchain, resize)
    tools/gfx_demo.sh                   # windowed demo; under Xvfb saves build/gfx_demo.png

Synchronization checking (slower) is enabled with
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.

## Fixed shaders

The renderer's own shaders are GLSL files in `src/shaders/` (test shaders in
`tests/shaders/`). Their SPIR-V is checked in as C arrays (`src/shaders_spv.h`,
`tests/test_gfx_shaders.h`), so building never needs a shader compiler. After editing
a shader, regenerate the arrays with

    tools/gen_shaders.sh     # needs glslangValidator and spirv-val (apt: glslang-tools spirv-tools)

The shaders written by users are compiled to SPIR-V by FractalStream itself.
