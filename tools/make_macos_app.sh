#!/bin/sh
# make_macos_app.sh - package build/FractalStream.app (run by `./build.sh app` on macOS).
#
#   tools/make_macos_app.sh <path to the fractalstream binary>
#
# Bundle layout:
#   FractalStream.app/Contents/Info.plist
#   FractalStream.app/Contents/MacOS/<binary>
#   FractalStream.app/Contents/Frameworks/libMoltenVK.dylib      Vulkan on Metal; src/gfx.c loads it
#                                                                from here before any system library
#   FractalStream.app/Contents/Frameworks/MoltenVK_icd.json      (only needed by a Vulkan loader)
#   FractalStream.app/Contents/Frameworks/LICENSE-MoltenVK.txt   MoltenVK is Apache-2.0
#
# MoltenVK comes from the first of:
#   1. $MOLTENVK      libMoltenVK.dylib itself, or a directory containing it (searched)
#   2. Homebrew       brew install molten-vk
#   3. $VULKAN_SDK    the LunarG Vulkan SDK (.../macOS)
#   4. a download of the pinned release below into build/moltenvk/ (curl)
#
# NOTE: written on Linux and not tested on a Mac yet; plain POSIX sh + macOS tools.
set -e
MOLTENVK_RELEASE=v1.4.2   # https://github.com/KhronosGroup/MoltenVK/releases

BIN=${1:?usage: tools/make_macos_app.sh <fractalstream binary>}
[ -f "$BIN" ] || { echo "make_macos_app.sh: $BIN not found" >&2; exit 1; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")   # absolute, before cd
cd "$(dirname "$0")/.."
APP=build/FractalStream.app

# First libMoltenVK.dylib under a directory (the trailing slash follows symlinked dirs).
find_dylib() { find "$1/" -name libMoltenVK.dylib 2>/dev/null | head -n 1; }

DYLIB=""
if [ -n "$MOLTENVK" ]; then
  if [ -f "$MOLTENVK" ]; then DYLIB=$MOLTENVK; else DYLIB=$(find_dylib "$MOLTENVK"); fi
  [ -n "$DYLIB" ] || { echo "make_macos_app.sh: no libMoltenVK.dylib in MOLTENVK=$MOLTENVK" >&2; exit 1; }
fi
if [ -z "$DYLIB" ] && command -v brew >/dev/null 2>&1; then
  PREFIX=$(brew --prefix molten-vk 2>/dev/null || true)
  if [ -n "$PREFIX" ] && [ -d "$PREFIX" ]; then DYLIB=$(find_dylib "$PREFIX"); fi
fi
if [ -z "$DYLIB" ] && [ -n "$VULKAN_SDK" ] && [ -d "$VULKAN_SDK" ]; then
  DYLIB=$(find_dylib "$VULKAN_SDK")
fi
if [ -z "$DYLIB" ]; then
  DL=build/moltenvk
  if [ -z "$(find_dylib "$DL")" ]; then
    URL=https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_RELEASE/MoltenVK-macos.tar
    echo "downloading $URL"
    mkdir -p "$DL"
    curl -fL --retry 2 -o "$DL/MoltenVK-macos.tar" "$URL"
    tar -xf "$DL/MoltenVK-macos.tar" -C "$DL"
    rm -f "$DL/MoltenVK-macos.tar"
  fi
  DYLIB=$(find_dylib "$DL")
fi
[ -n "$DYLIB" ] || { echo "make_macos_app.sh: libMoltenVK.dylib not found" >&2; exit 1; }
echo "MoltenVK: $DYLIB"

# A file named like $1 near the dylib: in its directory or up to 4 parents
# (release tarball: MoltenVK/LICENSE and .../dylib/macOS/MoltenVK_icd.json).
find_near() {
  d=$(dirname "$DYLIB")
  for i in 1 2 3 4 5; do
    f=$(find "$d/" -maxdepth 4 -name "$1" 2>/dev/null | grep -i moltenvk | head -n 1)
    [ -n "$f" ] || f=$(find "$d/" -maxdepth 1 -name "$1" 2>/dev/null | head -n 1)
    if [ -n "$f" ]; then echo "$f"; return 0; fi
    d=$(dirname "$d")
  done
  return 0
}
ICD=$(find_near MoltenVK_icd.json)
LIC=$(find_near 'LICENSE*')

VERSION=$(sed -n 's/^#define FS_VERSION "\(.*\)"/\1/p' src/base.h)
EXE=$(basename "$BIN")
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/$EXE"
cp -L "$DYLIB" "$APP/Contents/Frameworks/libMoltenVK.dylib"
if [ -n "$ICD" ]; then cp -L "$ICD" "$APP/Contents/Frameworks/MoltenVK_icd.json"; fi
if [ -n "$LIC" ]; then
  cp -L "$LIC" "$APP/Contents/Frameworks/LICENSE-MoltenVK.txt"
else
  echo "warning: MoltenVK license file not found; writing a pointer to it instead" >&2
  echo "MoltenVK is licensed under the Apache License 2.0: https://github.com/KhronosGroup/MoltenVK/blob/main/LICENSE" \
    > "$APP/Contents/Frameworks/LICENSE-MoltenVK.txt"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>FractalStream</string>
  <key>CFBundleDisplayName</key><string>FractalStream</string>
  <key>CFBundleIdentifier</key><string>org.fractalstream.FractalStream</string>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

# Ad-hoc signature so the bundle runs on Apple Silicon after files were copied in.
if command -v codesign >/dev/null 2>&1; then
  codesign --force --sign - "$APP/Contents/Frameworks/libMoltenVK.dylib"
  codesign --force --sign - "$APP"
fi
echo "built $APP"
