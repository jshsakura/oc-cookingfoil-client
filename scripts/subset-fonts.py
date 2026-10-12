"""Subset the bundled fonts.

Usage:
  uvx --from fonttools python scripts/subset-fonts.py ko <Pretendard public/static dir>
  uvx --from fonttools python scripts/subset-fonts.py ja <Pretendard JP public/static dir>
  uvx --from fonttools python scripts/subset-fonts.py zh <NotoSansSC[wght].ttf>

ko keeps KS X 1001 Hangul, jamo, Latin and punctuation; ja and zh keep the
level 1 kanji/hanzi plus every character the translations use
(scripts/font-chars.py). Rarer glyphs fall back to the console's shared fonts
at runtime. The subsets are renamed: Pretendard is OFL with the reserved name
"Pretendard", and renaming the Noto subset keeps the naming consistent.
"""
import subprocess
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

root = Path(__file__).resolve().parents[1]
mode, source = sys.argv[1], Path(sys.argv[2])
unicodes = subprocess.run([sys.executable, "-I", str(root / "scripts/font-chars.py"), mode],
                          capture_output=True, text=True, check=True).stdout.strip()
suffix = {"ko": "", "ja": "JP", "zh": "SC"}[mode]


def load(weight):
    if mode == "ko":
        return TTFont(source / f"Pretendard-{weight}.otf")
    if mode == "ja":
        return TTFont(source / f"PretendardJP-{weight}.otf")
    from fontTools.varLib import instancer
    return instancer.instantiateVariableFont(TTFont(source), {"wght": 400 if weight == "Regular" else 700})


for weight in ("Regular", "Bold"):
    options = subset.Options()
    options.layout_features = ["kern", "liga", "calt", "ccmp", "mark", "mkmk"]
    options.hinting = False
    options.desubroutinize = True
    font = load(weight)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[int(u[2:], 16) for u in unicodes.split(",")])
    subsetter.subset(font)
    family = "CookingFoil Sans" + (" " + suffix if suffix else "")
    full = f"{family} {weight}"
    postscript = f"CookingFoilSans{suffix}-{weight}"
    for record in font["name"].names:
        if record.nameID in (1, 16):
            record.string = family
        elif record.nameID in (3, 4):
            record.string = full
        elif record.nameID == 6:
            record.string = postscript
    if "CFF " in font:
        cff = font["CFF "].cff
        cff.fontNames = [postscript]
        top = cff.topDictIndex[0]
        top.FullName, top.FamilyName = full, family
    font.save(root / "host/fonts" / f"{postscript}{'.ttf' if mode == 'zh' else '.otf'}")
