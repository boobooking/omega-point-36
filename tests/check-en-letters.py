#!/usr/bin/env python3
"""Fail if en_letters has drifted away from the en layer.

en_letters is a deliberate copy: it is raised over ru whenever a modifier or the
herdr prefix needs Latin scancodes, and it has to carry real bindings because
&trans there would fall straight back through to ru. The copy has no exceptions
— all 36 positions must match en.

There used to be one. Position 32 was &none while its tap carried the layout
switch, because under a held modifier neither of that key's two jobs could be
right: a copy of en would run "go to ru" from ru, and &trans would reach sym_ru,
whose symbols switch the layout through the &en wrapper. The switch moved to the
31+34 combo and the tap became Escape, which is safe under a modifier, so the
exception went with it.

Nothing in the devicetree enforces that copy, so this does. It runs from
tests/run.sh before any case is built.
"""
import sys

from keymap_layers import KEYMAP, layers, require


def main():
    found = layers(KEYMAP.read_text())
    require(found, "en", "en_letters")

    en_rows, en_thumbs = found["en"]
    cp_rows, cp_thumbs = found["en_letters"]

    problems = []
    for r, (a, b) in enumerate(zip(en_rows, cp_rows)):
        for c, (x, y) in enumerate(zip(a, b)):
            if x != y:
                problems.append(f"position {r * 10 + c}: en has {x!r}, en_letters has {y!r}")

    for i, (x, y) in enumerate(zip(en_thumbs, cp_thumbs)):
        if x != y:
            problems.append(f"position {30 + i}: en has {x!r}, en_letters has {y!r}")

    if problems:
        print("en_letters has drifted from en:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            "\nen_letters is a copy of en, every position. Mirror the change into both\n"
            "layers, or the English letters under a modifier on ru stop matching the\n"
            "ones you actually type.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
