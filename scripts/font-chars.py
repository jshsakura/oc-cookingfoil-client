# Unicode lists for the bundled font subsets.
#   ko (default): KS X 1001 Hangul, jamo, Latin, Cyrillic, punctuation and
#       every character the Latin and Cyrillic translations use (Pretendard).
#   ja: kana, JIS X 0208 level 1 kanji and every character the Japanese
#       translations use (Pretendard JP).
#   zh: GB 2312 level 1 hanzi and every character the simplified Chinese
#       translations use (Noto Sans SC).
#   tc: Big5 level 1 hanzi and every character the traditional Chinese
#       translations use (Noto Sans TC).
import re
import sys
from pathlib import Path

mode = sys.argv[1] if len(sys.argv) > 1 else "ko"
cps = set()


def table(codec, leads, trails=range(0xA1, 0xFF)):
    for lead in leads:
        for trail in trails:
            try:
                cps.add(ord(bytes([lead, trail]).decode(codec)))
            except UnicodeDecodeError:
                pass


def translations(code, low=0x2E80, high=0x10FFFF):
    # Rows in source/i18n/lang_<code>.cpp: {"English key", "text"}
    root = Path(__file__).resolve().parents[1]
    row = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')
    path = root / f"source/i18n/lang_{code}.cpp"
    if not path.exists():
        return
    for match in row.finditer(path.read_text()):
        cps.update(ord(c) for c in match.group(2) if low <= ord(c) <= high)


punctuation = [(0x3000, 0x303F), (0xFF01, 0xFF5E), (0xFF61, 0xFF9F), (0x30FB, 0x30FC)]
if mode == "ja":
    table("euc_jp", range(0xB0, 0xD0))
    translations("ja")
    ranges = punctuation + [(0x3040, 0x30FF), (0x31F0, 0x31FF)]
elif mode == "zh":
    table("gb2312", range(0xB0, 0xD8))
    translations("zh")
    ranges = punctuation
elif mode == "tc":
    # Big5 level 1 runs A440 to C67E, trail bytes 40-7E and A1-FE.
    table("big5", range(0xA4, 0xC6), [*range(0x40, 0x7F), *range(0xA1, 0xFF)])
    table("big5", [0xC6], range(0x40, 0x7F))
    translations("zh-Hant")
    ranges = punctuation
else:
    for code in ("es", "fr", "de", "it", "pt", "nl", "ru"):
        translations(code, 0x20, 0x2E7F)
    table("euc-kr", range(0xB0, 0xC9))
    ranges = [(0x20, 0x7E), (0xA0, 0x17F), (0x300, 0x36F), (0x400, 0x45F), (0x2010, 0x2027), (0x2030, 0x203A),
              (0x20A9, 0x20AC), (0x2122, 0x2122), (0x2190, 0x2199), (0x2460, 0x2473), (0x25A0, 0x25CF),
              (0x2605, 0x2606), (0x3000, 0x3011), (0x3131, 0x318E), (0xFF01, 0xFF5E)]
for a, b in ranges:
    cps.update(range(a, b + 1))
print(",".join(f"U+{c:04X}" for c in sorted(cps)))
print(len(cps), file=sys.stderr)
