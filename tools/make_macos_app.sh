#!/bin/sh
# make_macos_app.sh - build/FractalStream.app and its download, build/FractalStream-macos-<arch>.zip
# (run by `./build.sh app` on a Mac, after it builds the program).
#
#   tools/make_macos_app.sh <path to the fractalstream binary>
#
# Bundle layout:
#   FractalStream.app/Contents/Info.plist
#   FractalStream.app/Contents/MacOS/fractalstream            the program
#   FractalStream.app/Contents/Frameworks/libMoltenVK.dylib   Vulkan on Metal; src/gfx.c loads it from ../Frameworks
#                                                            (next to the program) before any other Vulkan library
#   FractalStream.app/Contents/Resources/examples/            the examples (the Open dialog's "Examples")
#   FractalStream.app/Contents/Resources/docs/                the language reference (F2)
#   FractalStream.app/Contents/Resources/licenses/            the program's license, MoltenVK's and the code compiled in
# The program finds all of it relative to its own file (fs_exe_dir), never to the working directory, which is /
# when the app is opened from the Finder.
#
# MoltenVK (libMoltenVK.dylib, with code for the program's processor) is the first found of:
#   1. $MOLTENVK: the dylib itself or a folder with it inside; MOLTENVK=download goes straight to 4.
#   2. the LunarG Vulkan SDK: $VULKAN_SDK/lib, then ~/VulkanSDK/<version>/macOS/lib
#   3. Homebrew (brew install molten-vk): /opt/homebrew/lib, /usr/local/lib
#   4. the official MoltenVK release pinned below, downloaded into build/ and checked against its SHA-256
# It must need nothing but macOS itself (checked): the app has to run on Macs without the SDK or Homebrew.
#
# The app is signed ad hoc (codesign -s -, no Apple developer account), as Apple Silicon requires. A Mac opens
# it without questions when it was built there or downloaded with curl; downloaded with a web browser, macOS asks
# for a confirmation the first time (README.md, "Download and run"). LSMinimumSystemVersion is the newest of the
# minimum macOS versions of the program (MACOSX_DEPLOYMENT_TARGET, see build.sh) and of MoltenVK.
set -e
MOLTENVK_RELEASE=v1.4.2   # https://github.com/KhronosGroup/MoltenVK/releases (universal; needs macOS 12)
MOLTENVK_SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e   # of its MoltenVK-macos.tar

die() { echo "make_macos_app.sh: $*" >&2; exit 1; }

BIN=${1:?usage: tools/make_macos_app.sh <fractalstream binary>}
[ -f "$BIN" ] || die "$BIN not found"
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")   # (absolute, before the cd)
cd "$(dirname "$0")/.."
for tool in codesign ditto otool; do
  command -v $tool >/dev/null 2>&1 || die "$tool not found: this runs on a Mac, with Xcode's command line tools"
done

# The architectures in a Mach-O file ("arm64", "x86_64", "x86_64 arm64"); empty if lipo can't tell.
# (The unquoted echo trims the spaces lipo leaves around them.)
# shellcheck disable=SC2046,SC2005
archs() { echo $(lipo -archs "$1" 2>/dev/null); }
BIN_ARCHS=$(archs "$BIN")

# Is $1 a MoltenVK with code for every architecture of the program?
fits() {
  [ -f "$1" ] || return 1
  have=$(archs "$1")
  [ -n "$have" ] || return 0
  for a in $BIN_ARCHS; do
    case " $have " in *" $a "*) ;; *) return 1 ;; esac
  done
  return 0
}

# The first libMoltenVK.dylib in a folder (the macOS one, if the folder has several)
find_dylib() {
  f=$(find "$1/" -name libMoltenVK.dylib -path '*macOS*' 2>/dev/null | head -n 1)
  [ -n "$f" ] || f=$(find "$1/" -name libMoltenVK.dylib 2>/dev/null | head -n 1)
  echo "$f"
}

DYLIB=""
if [ -n "$MOLTENVK" ] && [ "$MOLTENVK" != download ]; then
  if [ -d "$MOLTENVK" ]; then F=$(find_dylib "$MOLTENVK"); else F=$MOLTENVK; fi
  [ -n "$F" ] && [ -f "$F" ] || die "no libMoltenVK.dylib at MOLTENVK=$MOLTENVK"
  fits "$F" || die "$F has no code for this program's processor ($BIN_ARCHS; it has $(archs "$F"))"
  DYLIB=$F
elif [ -z "$MOLTENVK" ]; then
  if [ -n "$VULKAN_SDK" ] && fits "$VULKAN_SDK/lib/libMoltenVK.dylib"; then DYLIB=$VULKAN_SDK/lib/libMoltenVK.dylib; fi
  if [ -z "$DYLIB" ]; then
    for d in "$HOME"/VulkanSDK/*/macOS/lib; do   # (in the order of their names: the last is the newest)
      if fits "$d/libMoltenVK.dylib"; then DYLIB=$d/libMoltenVK.dylib; fi
    done
  fi
  for d in /opt/homebrew/lib /usr/local/lib; do
    if [ -z "$DYLIB" ] && fits "$d/libMoltenVK.dylib"; then DYLIB=$d/libMoltenVK.dylib; fi
  done
fi
if [ -z "$DYLIB" ]; then
  DL=build/moltenvk-$MOLTENVK_RELEASE
  if [ -z "$(find_dylib "$DL")" ]; then
    URL=https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_RELEASE/MoltenVK-macos.tar
    echo "downloading MoltenVK $MOLTENVK_RELEASE: $URL"
    rm -rf "$DL"
    mkdir -p "$DL"
    curl -fL --retry 3 -o "$DL/MoltenVK-macos.tar" "$URL"
    SUM=$(shasum -a 256 "$DL/MoltenVK-macos.tar" | cut -d ' ' -f 1)
    if [ "$SUM" != "$MOLTENVK_SHA256" ]; then
      rm -rf "$DL"
      die "the download's SHA-256 is $SUM, not the expected $MOLTENVK_SHA256"
    fi
    tar -xf "$DL/MoltenVK-macos.tar" -C "$DL"
    rm -f "$DL/MoltenVK-macos.tar"
  fi
  F=$(find_dylib "$DL")
  [ -n "$F" ] || die "no libMoltenVK.dylib in the MoltenVK release ($DL)"
  fits "$F" || die "$F has no code for this program's processor ($BIN_ARCHS; it has $(archs "$F"))"
  DYLIB=$F
fi
echo "MoltenVK: $DYLIB"

# MoltenVK may use only macOS's own libraries (/usr/lib, /System): one built against other Homebrew libraries,
# say, would not load on Macs without them.
IDS=$(otool -D "$DYLIB" | awk '!/:$/ { printf " %s", $0 }')   # (its own install names: " id1 id2")
for dep in $(otool -L "$DYLIB" | awk '/^[[:space:]]/ { print $1 }' | sort -u); do
  case "$dep" in /usr/lib/*|/System/Library/*) continue ;; esac
  case "$IDS " in *" $dep "*) continue ;; esac
  die "$DYLIB needs $dep, which other Macs don't have; use another MoltenVK (MOLTENVK=download: the official one)"
done

# The oldest macOS a Mach-O file runs on (the newest of its architectures' minimums); empty if unknown.
min_macos() {
  a=$(archs "$1")
  { if [ -n "$a" ]; then for x in $a; do otool -arch "$x" -l "$1"; done; else otool -l "$1"; fi; } 2>/dev/null |
    awk '$1 == "cmd" { k = $2 }
         k == "LC_BUILD_VERSION" && $1 == "minos" { print $2 }
         k == "LC_VERSION_MIN_MACOSX" && $1 == "version" { print $2 }' |
    sort -t . -k 1,1n -k 2,2n -k 3,3n | tail -n 1
}
MIN_BIN=$(min_macos "$BIN")
MIN_MVK=$(min_macos "$DYLIB")
MINOS=$(printf '%s\n%s\n' "${MIN_BIN:-${MACOSX_DEPLOYMENT_TARGET:-11.0}}" "$MIN_MVK" | grep . |
        sort -t . -k 1,1n -k 2,2n -k 3,3n | tail -n 1)

VERSION=$(sed -n 's/^#define FS_VERSION "\(.*\)"/\1/p' src/base.h)
EXE=$(basename "$BIN")
APP=build/FractalStream.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/docs" \
         "$APP/Contents/Resources/licenses"
cp "$BIN" "$APP/Contents/MacOS/$EXE"
cp -L "$DYLIB" "$APP/Contents/Frameworks/libMoltenVK.dylib"
cp -R examples "$APP/Contents/Resources/examples"
cp docs/*.md "$APP/Contents/Resources/docs/"
R=$APP/Contents/Resources/licenses
cp LICENSE "$R/FractalStream.txt"
cp third_party/moltenvk/LICENSE "$R/MoltenVK.txt"
cp third_party/glfw/LICENSE.md "$R/GLFW.md"
cp third_party/vulkan/LICENSE.md "$R/Vulkan-Headers.md"
cp third_party/fonts/LICENSE-DejaVu.txt "$R/DejaVu-fonts.txt"
cp third_party/fonts/LICENSE-MaterialIcons.txt "$R/Material-Icons.txt"
find "$APP" -name '.*' -type f -exec rm -f {} +   # (.DS_Store and the like)
chmod 755 "$APP/Contents/MacOS/$EXE" "$APP/Contents/Frameworks/libMoltenVK.dylib"   # (Homebrew's dylib is read-only)
chmod -R u=rwX,go=rX "$APP"   # (755 for code and folders, 644 for the rest, whatever the umask)

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>FractalStream</string>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundleIdentifier</key><string>org.fractalstream.FractalStream</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>FractalStream</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.education</string>
  <key>LSMinimumSystemVersion</key><string>$MINOS</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>NSDesktopFolderUsageDescription</key><string>FractalStream opens and saves scripts and pictures where you ask it to.</string>
  <key>NSDocumentsFolderUsageDescription</key><string>FractalStream opens and saves scripts and pictures where you ask it to.</string>
  <key>NSDownloadsFolderUsageDescription</key><string>FractalStream opens and saves scripts and pictures where you ask it to.</string>
</dict>
</plist>
PLIST
if command -v plutil >/dev/null 2>&1; then plutil -lint "$APP/Contents/Info.plist"; fi

# Sign last: any change to the bundle afterwards breaks the signature ("FractalStream is damaged").
xattr -cr "$APP" 2>/dev/null || true   # (extended attributes of copied files, e.g. quarantine, upset codesign)
codesign --force --sign - "$APP/Contents/Frameworks/libMoltenVK.dylib"
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

case "$BIN_ARCHS" in
  "")    ARCH=$(uname -m) ;;
  *" "*) ARCH=universal ;;
  *)     ARCH=$BIN_ARCHS ;;
esac
ZIP=build/FractalStream-macos-$ARCH.zip
rm -f "$ZIP"
ditto -c -k --norsrc --keepParent "$APP" "$ZIP"   # (ditto keeps the bundle as it is; signature included)
echo "built $APP: FractalStream $VERSION for $ARCH, macOS $MINOS or newer"
echo "zipped $ZIP"
