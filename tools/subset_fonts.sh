#!/bin/sh
# Make the DejaVu subsets in third_party/fonts from the full DejaVu 2.37 fonts (fontTools'
# pyftsubset), then embed them: python3 tools/embed.py ... (see third_party/fonts/fonts_embedded.c).
#   tools/subset_fonts.sh [DIR with DejaVuSans.ttf and DejaVuSansMono.ttf]
# Kept: Latin (with Extended-A/B and Additional), Greek, basic Cyrillic, punctuation, super- and
# subscripts, letterlike symbols, arrows, mathematical operators and symbols, geometric shapes,
# miscellaneous symbols (★ ☆ ...), dingbats (✓ ✗ ...). No hinting, no layout features.
set -e
SRC=${1:-/usr/share/fonts/truetype/dejavu}
OUT=third_party/fonts
RANGES="U+0020-007E,U+00A0-024F,U+02B0-02FF,U+0370-03FF,U+0400-045F,U+1E00-1EFF,U+2000-2071,U+2074-209C,U+2100-214F,U+2190-23FF,U+25A0-26FF,U+2700-27BF,U+27C0-27FF,U+2900-29FF,U+FFFD"
for f in DejaVuSans DejaVuSansMono; do
  pyftsubset "$SRC/$f.ttf" --unicodes="$RANGES" --no-hinting --layout-features='' \
    --drop-tables+=kern --output-file="$OUT/$f-subset.ttf"
done
python3 tools/embed.py third_party/fonts/fonts_embedded.c font_mono_ttf=third_party/fonts/DejaVuSansMono-subset.ttf \
  font_sans_ttf=third_party/fonts/DejaVuSans-subset.ttf font_icons_ttf=third_party/fonts/MaterialIcons-Regular.ttf
