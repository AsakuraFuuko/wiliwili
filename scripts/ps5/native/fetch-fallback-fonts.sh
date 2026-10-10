#!/usr/bin/env bash
# Fetch the Noto fallback faces needed by the native title (PPSA99233).
#
# Why: the title ships no system fonts. Only the bundled CJK/Latin face is used, so
# Korean, Arabic, Thai and the Indic scripts render as tofu boxes - in the UI, in B站
# subtitles and in danmaku. nanovg/fontstash supports per-glyph fallback lists
# (nvgAddFallbackFont), so the runtime work is "load these faces + hang them off the
# base fonts"; this script only produces the assets.
#
# Status (2026-10-10): assets verified good and present in the image
# (`res-check: critical=11 missing=0` with them listed), but the in-app registration
# could not be made to take effect: `nvgCreateFont` reports success for the bundled
# switch_font.ttf while stdio `fopen` on the very same path fails from the app's
# clean-room libc, and every new face stayed FONT_INVALID. Resolve that (see
# run-continuation/ps5-native-handoff.md, fonts section) before enabling the faces.
#
# Usage: bash scripts/ps5/native/fetch-fallback-fonts.sh [output-dir]
set -euo pipefail

out=${1:-$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../resources/font" && pwd)}
mkdir -p "$out"
base="https://raw.githubusercontent.com/googlefonts/noto-fonts/main"

fetch() {
    local name=$1 url="$base/hinted/ttf/$1/$1-Regular.ttf" tmp
    tmp=$(mktemp)
    curl -sSL --max-time 60 -o "$tmp" "$url"
    if [ "$(head -c4 "$tmp" | xxd -p)" != "00010000" ]; then
        echo "!! $name: not a glyf TTF (did the upstream path change?)" >&2
        rm -f "$tmp"
        return 1
    fi
    cp "$tmp" "$out/$2"
    rm -f "$tmp"
    echo "$2  $(du -h "$out/$2" | cut -f1)"
}

# Straight from the Noto classic repo (static hinted TTFs).
fetch NotoSansDevanagari noto-sans-devanagari.ttf
fetch NotoSansArabic     noto-sans-arabic.ttf
fetch NotoSansThai       noto-sans-thai.ttf
fetch NotoSansMyanmar    noto-sans-myanmar.ttf
fetch NotoSansTelugu     noto-sans-telugu.ttf
fetch NotoSansTamil      noto-sans-tamil.ttf
fetch NotoSansSinhala    noto-sans-sinhala.ttf
fetch NotoSansHebrew     noto-sans-hebrew.ttf
fetch NotoSansGeorgian   noto-sans-georgian.ttf
fetch NotoSansArmenian   noto-sans-armenian.ttf

# Korean: the classic repo has no KR face; noto-cjk's TrueType VF is 10 MB, so pin the
# regular instance and subset to Hangul + Latin + punctuation (~2.7 MB).
kr=https://github.com/notofonts/noto-cjk/raw/main/Sans/Variable/TTF/Subset/NotoSansKR-VF.ttf
tmp=$(mktemp)
curl -sSL --max-time 120 -o "$tmp" "$kr"
[ "$(head -c4 "$tmp" | xxd -p)" = "00010000" ] || { echo "!! Korean face unavailable" >&2; exit 1; }
python3 - "$tmp" "$out/noto-sans-kr.ttf" <<'PY'
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

src, dst = sys.argv[1], sys.argv[2]
inst = instancer.instantiateVariableFont(TTFont(src, lazy=True), {"wght": 400}, inplace=False)
tmp_inst = dst + ".inst.ttf"
inst.save(tmp_inst)
opts = subset.Options(layout_features=["*"], name_IDs=["*"], notdef_outline=True, hinting=True)
s = subset.Subsetter(options=opts)
s.populate(unicodes=subset.parse_unicodes(
    "U+0020-007E,U+00A0-00FF,U+2000-206F,U+3000-303F,U+AC00-D7A3,U+1100-11FF,U+3130-318F,U+FF00-FFEF"))
f = subset.load_font(tmp_inst, opts)
s.subset(f)
subset.save_font(f, dst, opts)
import os
os.remove(tmp_inst)
print(f"noto-sans-kr.ttf  {os.path.getsize(dst)//1024}KB")
PY
rm -f "$tmp"
echo "==> done: $out"
