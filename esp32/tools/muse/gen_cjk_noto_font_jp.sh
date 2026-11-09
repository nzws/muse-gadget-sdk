#!/bin/sh
# Regenerates components/muse/fonts/muse_font_cjk_noto_16.c from
# Noto Sans JP Medium (not SC): JIS X 0208 coverage for Japanese text,
# anti-aliased at 16 px, 4 bpp. Adapted from gen_cjk_noto_font.sh.
#
# The codepoint list is JIS X 0208 (decoded via EUC-JP) intersected
# with the font's cmap: lv_font_conv 1.5.3 aborts if a requested
# codepoint is missing from the font. Noto Sans JP lacks exactly one
# JIS X 0208 character, U+2252 (nearly-equal sign), which therefore
# shows as a box. Needs curl, python3 with fontTools, and npx.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_noto_16.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/noto.otf" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Medium.otf"
KANJI=$(python3 - "$TMP/noto.otf" <<'EOF'
import sys
from fontTools.ttLib import TTFont
cmap = TTFont(sys.argv[1]).getBestCmap()
cps = set()
for hi in range(0xA1, 0xFF):
    for lo in range(0xA1, 0xFF):
        try:
            cp = ord(bytes([hi, lo]).decode("euc_jp"))
        except (UnicodeDecodeError, ValueError):
            continue
        if cp in cmap:
            cps.add(cp)
print(",".join(hex(c) for c in sorted(cps)))
EOF
)
npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_cjk_noto_16 --no-compress \
    -r 0x2000-0x206F -r 0x3000-0x30FF -r 0xFF00-0xFFEF -r "$KANJI" \
    -o "$TMP/font.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
