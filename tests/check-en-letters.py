#!/usr/bin/env python3
"""Fail if en_letters has drifted away from the en layer.

en_letters is a deliberate copy: it is raised over ru whenever a modifier or the
herdr prefix needs Latin scancodes, and it has to carry real bindings because
&trans there would fall straight back through to ru. The one difference is
position 32, which is &none. That key now carries two jobs — hold for the symbol
layer, tap to switch language — and under a modifier neither can be right: a copy
of en would run "go to ru" from ru, and &trans would reach sym_ru, whose symbols
tap Caps Lock through the &en wrapper. Refusing the press is the only safe
answer. If the switch ever moves again, this exception moves with it.

Nothing in the devicetree enforces that copy, so this does. It runs from
tests/run.sh before any case is built.
"""
import sys

from keymap_layers import KEYMAP, layers, require

# Thumb index (0..5, i.e. positions 30..35) carrying the layout switch, and so
# the one position where en_letters must be &trans rather than a copy of en.
SWITCH_THUMB = 2
SWITCH_THUMB_BINDING = "&none"


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
        want = SWITCH_THUMB_BINDING if i == SWITCH_THUMB else x
        if y != want:
            problems.append(f"position {30 + i}: en_letters has {y!r}, expected {want!r}")

    if problems:
        print("en_letters has drifted from en:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            f"\nen_letters is a copy of en with {SWITCH_THUMB_BINDING} at position "
            f"{30 + SWITCH_THUMB}, where the layout switch lives. Mirror the\n"
            "change into both layers, or the English letters under a modifier\n"
            "on ru stop matching the ones you actually type.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
