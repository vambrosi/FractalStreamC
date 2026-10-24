# Building FractalStreamC

FractalStreamC is a single C11 program (a "unity build": `src/main.c` includes every
module through `src/all.c`). Its only dependencies are vendored in `third_party/`
(GLFW, the Vulkan headers, stb, fonts). Vulkan itself is loaded at run time, so
nothing links against a Vulkan library: the program needs a Vulkan *driver* to run,
but no Vulkan SDK to build.

| platform | compiler | command | output |
|---|---|---|---|
| Linux | gcc or clang | `./build.sh` | `build/fractalstreamc` |
| Linux (download) | gcc or clang | `tools/make_linux_release.sh` | `build/FractalStreamC-linux-x86_64.tar.gz` |
| macOS | Xcode command line tools | `./build.sh app` | `build/FractalStreamC.app`, `build/FractalStreamC-macos-arm64.zip` |
| Windows | Visual Studio (MSVC) | `build.bat` | `build\fractalstreamc.exe` |
| Windows | MSYS2 (MinGW gcc) | `./build.sh` | `build/fractalstreamc.exe` |
| Linux -> Windows | MinGW cross compiler | `./build.sh windows` | `build/win/fractalstreamc.exe` |

Other `build.sh` modes: `debug` (sanitizers, `build/fractalstreamc_debug`) and
`test [filter]` (builds and runs the test suite, `build/tests`).
`build.bat` has `debug` and `test [filter]` too.

## Downloads (nothing to build)

The [releases](https://github.com/vambrosi/FractalStreamC/releases/latest) have ready-made downloads (made as described
in "Making a release" below); the README's "Download" has the details.

- **Mac** (macOS 12 or newer): `FractalStreamC-macos-arm64.zip` for Apple silicon (M1, M2, ...),
  `FractalStreamC-macos-x86_64.zip` for Intel processors (Apple menu > About This Mac says which).
  Unzip it, move FractalStreamC to Applications and open it. The app is signed, but not notarized by
  Apple, so the first time macOS says it could not verify it: on macOS 15 and newer click Done, then
  System Settings > Privacy & Security > Open Anyway (and once more Open Anyway, and your password); on
  older versions right-click the app > Open > Open. Or install it from Terminal, which avoids the
  question, as files downloaded by `curl` are not marked as coming from the internet (`uname -m` picks
  the file for the Mac; no developer tools needed):

      curl -fL -o /tmp/FractalStreamC.zip "https://github.com/vambrosi/FractalStreamC/releases/latest/download/FractalStreamC-macos-$(uname -m).zip" && rm -rf /Applications/FractalStreamC.app && ditto -x -k /tmp/FractalStreamC.zip /Applications && open /Applications/FractalStreamC.app

- **Linux** (x86_64, glibc 2.35 or newer: Ubuntu 22.04, Debian 12, Fedora 36 and later):
  `tar -xzf FractalStreamC-linux-x86_64.tar.gz`, then run `FractalStreamC-linux-x86_64/fractalstreamc`.
  It needs a Vulkan driver (see Linux below: `sudo apt install libvulkan1 mesa-vulkan-drivers`).

## Linux

Build dependencies (Debian/Ubuntu names) are a C compiler and the X11 headers GLFW needs:

    sudo apt install build-essential libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev

To run, a Vulkan driver: `libvulkan1` (the loader) plus the GPU's driver, e.g.
`mesa-vulkan-drivers` (Intel, AMD, and `llvmpipe`/lavapipe, a software renderer that
works everywhere, slowly) or NVIDIA's driver. `vulkaninfo --summary` (package
`vulkan-tools`) shows what is installed.

GLFW is built for X11 only; on Wayland desktops the program runs through XWayland.

Then:

    ./build.sh            # build/fractalstreamc
    ./build.sh test       # run all tests

The download, a folder that runs wherever it is unpacked:

    tools/make_linux_release.sh   # build/FractalStreamC-linux-x86_64.tar.gz

It builds the program and packs `FractalStreamC-linux-<arch>/` with `fractalstreamc`, `examples/`,
`docs/`, `licenses/` and a `README.txt`. The program finds `examples/` and `docs/` next to itself (its
own location, not the working directory). It runs on systems with the glibc it was built with or a
newer one, so releases are built on an old distribution (Ubuntu 22.04, glibc 2.35, in the release
workflow). Vulkan and the X11 libraries are loaded when it starts, so it links against nothing else.

## macOS

Requirements: the Xcode command line tools (`xcode-select --install`). That is all `./build.sh app`
needs: it takes MoltenVK, the Vulkan implementation on top of Metal, from the Vulkan SDK or Homebrew if
either is installed, and otherwise downloads the official MoltenVK release. The plain program
(`./build.sh`, `build/fractalstreamc`) loads an installed MoltenVK at run time: `brew install molten-vk`,
or the LunarG Vulkan SDK.

    ./build.sh app        # build/FractalStreamC.app and build/FractalStreamC-macos-<arch>.zip
    open build/FractalStreamC.app
    ./build.sh            # build/fractalstreamc (MoltenVK from Homebrew or the SDK at run time)
    ./build.sh test       # the tests (the GPU ones use MoltenVK too)

`./build.sh app` runs `tools/make_macos_app.sh`, which makes a self-contained app:

- `Contents/MacOS/fractalstreamc`, `Contents/Frameworks/libMoltenVK.dylib`, and in
  `Contents/Resources/` the `examples/`, `docs/` and `licenses/` folders. The program finds them from
  its own location: opened from the Finder, its working directory is `/`.
- MoltenVK comes from `$MOLTENVK` (the dylib, or a folder containing it), else the LunarG Vulkan SDK
  (`$VULKAN_SDK/lib`, `~/VulkanSDK/*/macOS/lib`), else Homebrew (`/opt/homebrew/lib`, `/usr/local/lib`),
  else the official release pinned in the script (version and SHA-256), downloaded into `build/`.
  `MOLTENVK=download` takes that release directly (the release workflow does). The dylib must contain
  the program's processor architecture and use nothing but macOS's own libraries; both are checked.
- `Info.plist`: bundle id `io.github.vambrosi.FractalStreamC`, Retina (`NSHighResolutionCapable`), and
  `LSMinimumSystemVersion` the newest of the minimums of the program (`MACOSX_DEPLOYMENT_TARGET`,
  11.0 unless set; `build.sh` sets it on macOS) and of MoltenVK (12.0 for MoltenVK 1.4). The names
  differ from the Haskell FractalStream's (`FractalStream.app`, `org.haskell.FractalStream`), so both
  can be installed.
- The app is signed ad hoc (`codesign --force --deep --sign -`), which Apple silicon requires, but not
  notarized (that needs a paid Apple developer account). It opens without questions on the Mac that
  built it; on others see "Downloads" above.
- The zip is made with `ditto -c -k --keepParent`, which keeps the bundle (and its signature) intact.
  Its name says the processor: `FractalStreamC-macos-arm64.zip` or `FractalStreamC-macos-x86_64.zip`.

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

    build.bat             :: build\fractalstreamc.exe
    build.bat test        :: run the tests

**MSYS2**: in the "UCRT64" shell, `pacman -S mingw-w64-ucrt-x86_64-gcc`, then `./build.sh`.

**Cross-compiling from Linux**: `sudo apt install gcc-mingw-w64-x86-64`, then
`./build.sh windows`. This build must stay free of warnings.

## Run-time options (environment variables)

| variable | effect |
|---|---|
| `FS_GPU=<text>` | use the GPU whose name or driver contains `<text>` (case-insensitive), e.g. `FS_GPU=intel`, `FS_GPU=kosmic` |
| `FS_VULKAN_LIBRARY=<path>` | load this Vulkan library (or MoltenVK dylib) before the default ones |
| `FS_VULKAN_VALIDATION=1` | enable the Vulkan validation layer and print its messages |
| `FS_DEBUG=1` | print what the app decides at startup (the GPUs and their drivers, the one used; how deep zooms keep their arithmetic exact) |
| `MVK_CONFIG_FAST_MATH_ENABLED` | (macOS) set to `2` by the program unless already set: MoltenVK's fast maths, except in the shaders of deep zooms, which declare IEEE floats (SPIR-V float controls) and then need no per-operation workaround, the slow part of double precision before |
| `MVK_CONFIG_LOG_LEVEL` | (macOS) set to `1` by the program unless already set: MoltenVK prints only its errors; `3` adds its informational messages |

Without `FS_GPU` a discrete GPU is preferred, then an integrated one, then anything
else (e.g. lavapipe); for the same GPU through two drivers (a Vulkan SDK on macOS has MoltenVK and
KosmicKrisp), MoltenVK, the one the macOS app bundles.

## Testing graphics

The renderer tests (`./build.sh test gfx`) run headless on any Vulkan device, and are
reported as skipped when there is none. They enable the validation layer when it is
installed and fail on any validation error. On Linux:

    sudo apt install mesa-vulkan-drivers vulkan-validationlayers xvfb imagemagick
    xvfb-run -a ./build.sh test gfx     # includes the windowed test (swapchain, resize)
    tools/gfx_demo.sh                   # windowed demo; under Xvfb saves build/gfx_demo.png

Synchronization checking (slower) is enabled with
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.

Drivers differ in what they order for you. MoltenVK 1.3 and later (with Metal argument
buffers and residency sets, macOS 15+) orders the work of a command buffer by pipeline
barriers only and skips render-pass (subpass) dependencies, which the validation layer
accepts. A missing barrier there shows as magenta or stale squares in a view that is being
redrawn; to tell such a problem from others, compare with `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`
(MoltenVK's older binding, which Metal orders by itself) or with `FS_GPU=kosmic` (KosmicKrisp).

## Fixed shaders

The renderer's own shaders are GLSL files in `src/shaders/` (test shaders in
`tests/shaders/`). Their SPIR-V is checked in as C arrays (`src/shaders_spv.h`,
`tests/test_gfx_shaders.h`), so building never needs a shader compiler. After editing
a shader, regenerate the arrays with

    tools/gen_shaders.sh     # needs glslangValidator and spirv-val (apt: glslang-tools spirv-tools)

The shaders written by users are compiled to SPIR-V by FractalStreamC itself.

## Making a release

A release is the set of downloads above, published on GitHub by the workflow
`.github/workflows/release.yml` when a version tag is pushed:

1. Set the version in `src/base.h` (`#define FS_VERSION "0.2.0"`) and commit it.
2. Tag that commit `v` + the version, and push both:

       git tag v0.2.0
       git push origin main v0.2.0

3. On GitHub, Actions > Release shows the run (about 10 minutes). It builds and tests the Linux
   program in Ubuntu 22.04, builds the Mac app on an Apple silicon Mac (`macos-15`) and on an Intel
   one (`macos-15-intel`), tries each download as a user would (unpacked in another folder), and
   then creates the release of the tag with the three files. A tag that doesn't match `FS_VERSION` stops
   it before publishing. A tag with a hyphen (`v0.3.0-beta`) makes a pre-release.

To build the downloads without publishing anything: Actions > Release > Run workflow; the files are
then under that run's Artifacts. The file names don't change between releases, so links ending in
`releases/latest/download/<file>` (as in the README) always give the newest.

Notes for later:

- The Mac builds use the MoltenVK release pinned in `tools/make_macos_app.sh`; to move to a newer one,
  change `MOLTENVK_RELEASE` and `MOLTENVK_SHA256` (`shasum -a 256 MoltenVK-macos.tar`) together.
- `macos-15-intel` is GitHub's last Intel image (to be retired in 2027). Without it, the Intel app can be
  built on Apple silicon: `CC="cc -arch x86_64" MOLTENVK=download ./build.sh app` (the official MoltenVK
  has both architectures).
