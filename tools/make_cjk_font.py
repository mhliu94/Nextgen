#!/usr/bin/env python3
"""Rebuild web/static/fonts/ktrader-cjk.woff2, the last-resort Chinese font of both web UIs.

The font is a subset of Noto Sans CJK SC holding the CJK characters (U+2E80 and up) that appear
in the web assets and in the server's own strings, plus all CJK punctuation and full-width
forms, so the Chinese interface renders on machines without a Chinese system font. Hinting and
OpenType layout features are dropped: neither matters for horizontal UI text and together they
would double the file. Run it after adding or changing Chinese text:

    tools/make_cjk_font.py [--source /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc]

Needs fontTools with WOFF2 support (pip install fonttools brotli). Prints the character count
and fails if any character is missing from the source font.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

from fontTools import subset
from fontTools.ttLib import TTCollection, TTFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "web" / "static" / "fonts" / "ktrader-cjk.woff2"
FAMILY = "Noto Sans CJK SC"
SCANNED = [
    (ROOT / "web" / "static", ("*.js", "*.html", "*.css")),
    (ROOT / "src", ("*.cpp", "*.h", "*.inc")),
]
# CJK symbols and punctuation, full-width forms: always included for text from data and users.
ALWAYS = "".join(map(chr, range(0x3000, 0x3040))) + "".join(map(chr, range(0xFF01, 0xFF61)))


def characters() -> str:
    found: set[str] = set()
    for directory, patterns in SCANNED:
        for pattern in patterns:
            for path in directory.rglob(pattern):
                found.update(ch for ch in path.read_text(encoding="utf-8") if ord(ch) >= 0x2E80)
    return "".join(sorted(found | set(ALWAYS)))


def source_font(path: pathlib.Path) -> TTFont:
    if path.suffix.lower() in (".ttc", ".otc"):
        for font in TTCollection(str(path)).fonts:
            if font["name"].getDebugName(1) == FAMILY:
                return font
        sys.exit(f"{path} has no {FAMILY} face")
    return TTFont(str(path))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    args = parser.parse_args()

    text = characters()
    font = source_font(args.source)
    missing = [ch for ch in text if ord(ch) not in font.getBestCmap()]
    if missing:
        sys.exit(f"characters missing from {args.source}: {''.join(missing)}")
    options = subset.Options()
    options.flavor = "woff2"
    options.hinting = False
    options.layout_features = []
    subsetter = subset.Subsetter(options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    font.flavor = "woff2"
    font.save(str(args.output))
    print(f"{args.output}: {len(text)} characters, {args.output.stat().st_size:,} bytes")


if __name__ == "__main__":
    main()
