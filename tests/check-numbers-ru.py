#!/usr/bin/env python3
"""Fail if numbers_ru has drifted away from numbers_en.

numbers_ru is a copy of numbers_en that exists for one reason: Shift+digit is
decided by the host layout, and "Русская" puts different glyphs there than ABC.
Seven digits therefore carry a mod-morph that reaches the English symbol through
Option, which is where "Русская" keeps it. Everything else on the layer — the
left-hand modifiers, the shortcuts, the &none — has to stay a literal copy, or
the numbers layer quietly behaves differently depending on the language you
came from, which is the bug this pair was built to remove.

Nothing in the devicetree enforces that copy, so this does. It runs from
tests/run.sh before any case is built.
"""
import sys

from keymap_layers import KEYMAP, layers, require

# Position -> digit, for the seven keys whose Shift form differs between ABC and
# "Русская". 1, 9 and 0 are absent on purpose: Shift+1/9/0 already gives ! ( )
# on both layouts, so those keys stay plain and must match their copy exactly.
MORPHED = {6: "7", 7: "8", 16: "4", 17: "5", 18: "6", 27: "2", 28: "3"}


def main():
    found = layers(KEYMAP.read_text())
    require(found, "numbers_en", "numbers_ru")

    en_rows, en_thumbs = found["numbers_en"]
    ru_rows, ru_thumbs = found["numbers_ru"]

    problems = []
    for r, (a, b) in enumerate(zip(en_rows, ru_rows)):
        for c, (x, y) in enumerate(zip(a, b)):
            pos = r * 10 + c
            if pos in MORPHED:
                digit = MORPHED[pos]
                for layer, tok, want in (("numbers_en", x, f"&kp N{digit}"),
                                         ("numbers_ru", y, f"&d{digit}")):
                    if tok != want:
                        problems.append(
                            f"position {pos}: {layer} has {tok!r}, expected {want!r}")
            elif x != y:
                problems.append(
                    f"position {pos}: numbers_en has {x!r}, numbers_ru has {y!r}")

    for i, (x, y) in enumerate(zip(en_thumbs, ru_thumbs)):
        if x != y:
            problems.append(
                f"position {30 + i}: numbers_en has {x!r}, numbers_ru has {y!r}")

    if problems:
        print("numbers_ru has drifted from numbers_en:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            "\nnumbers_ru is a copy of numbers_en in which exactly the seven "
            "digits\nwhose Shift form differs between ABC and \"Русская\" carry "
            "&d2..&d8.\nMirror the change into both layers, or Shift+digit stops "
            "meaning the\nsame thing depending on which language you came from.",
            file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
