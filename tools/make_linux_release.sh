#!/bin/sh
# make_linux_release.sh - build FractalStreamC and pack build/FractalStreamC-linux-<arch>.tar.gz (a release download).
#
#   tools/make_linux_release.sh
#
# The archive holds one folder, FractalStreamC-linux-<arch>/ (arch: x86_64, aarch64):
#   fractalstreamc    the program
#   examples/         the example scripts (the Open dialog's "Examples"; the templates of New)
#   docs/             the language reference (F2 in the app)
#   LICENSE           the program's license
#   licenses/         licenses of the third-party code compiled into the program
#   README.txt        how to run it
# The program finds examples/ and docs/ next to itself, wherever the folder is moved.
#
# The program runs on Linux systems with the glibc it was built with or a newer one, so build releases on an old
# distribution: the release workflow (.github/workflows/release.yml) builds in Ubuntu 22.04 (glibc 2.35).
# Vulkan (libvulkan.so.1) and the X11 libraries are loaded when the program starts, so they are not needed here.
set -e
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo "make_linux_release.sh: this packs the Linux build; run it on Linux" >&2; exit 1; }

ARCH=$(uname -m)
NAME=FractalStreamC-linux-$ARCH
VERSION=$(sed -n 's/^#define FS_VERSION "\(.*\)"/\1/p' src/base.h)
STAGE=build/release

./build.sh
rm -rf "$STAGE"
mkdir -p "$STAGE/$NAME/docs" "$STAGE/$NAME/licenses"
DIR=$STAGE/$NAME
cp build/fractalstreamc "$DIR/fractalstreamc"
cp -R examples "$DIR/examples"
cp docs/*.md "$DIR/docs/"
cp LICENSE "$DIR/LICENSE"
cp third_party/glfw/LICENSE.md "$DIR/licenses/GLFW.md"
cp third_party/vulkan/LICENSE.md "$DIR/licenses/Vulkan-Headers.md"
cp third_party/fonts/LICENSE-DejaVu.txt "$DIR/licenses/DejaVu-fonts.txt"
cp third_party/fonts/LICENSE-MaterialIcons.txt "$DIR/licenses/Material-Icons.txt"
find "$DIR" -name '.*' -type f -exec rm -f {} +   # (editor and desktop leftovers, e.g. .DS_Store)
chmod -R u=rwX,go=rX "$DIR"   # (755 for the program and folders, 644 for the rest, whatever the umask)

cat > "$DIR/README.txt" <<EOF
FractalStreamC $VERSION for Linux ($ARCH)

Interactive pictures of the complex plane: write a short script, explore it on the GPU.

Run it from this folder:

    ./fractalstreamc                           the script editor, with an example to start from
    ./fractalstreamc "examples/0 - Basics/0 - Mandelbrot set.fstream"
                                               open a script
    ./fractalstreamc --help                    the command line (checks, renders to PNG files, ...)

In the app, F1 lists the keys and F2 shows the language reference; Ctrl+O opens a script
(the examples are a click away), Ctrl+N starts a new one from a template.

FractalStreamC draws with Vulkan, which needs a Vulkan driver. Most desktops have one. If the
program says "no Vulkan library found", install it:

    Ubuntu, Debian, Mint:   sudo apt install libvulkan1 mesa-vulkan-drivers
    Fedora:                 sudo dnf install vulkan-loader mesa-vulkan-drivers

(NVIDIA's own driver brings its Vulkan driver with it.) The window uses X11; on Wayland
desktops it runs through XWayland, which they include.

The folder can live anywhere; keep examples/ and docs/ next to the program. To start it by
name from any terminal: ln -s "\$PWD/fractalstreamc" ~/.local/bin/fractalstreamc

FractalStreamC is free software (BSD 3-Clause, see LICENSE). Third-party code compiled into the
program, and its licenses: licenses/.
EOF
if [ -n "$GITHUB_REPOSITORY" ]; then   # (set when GitHub Actions builds the release)
  printf '\nNewer versions, the source code and the documentation: %s/%s\n' \
    "${GITHUB_SERVER_URL:-https://github.com}" "$GITHUB_REPOSITORY" >> "$DIR/README.txt"
fi

tar -czf "build/$NAME.tar.gz" -C "$STAGE" "$NAME"
echo "packed build/$NAME.tar.gz"
