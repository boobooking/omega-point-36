#!/usr/bin/env python3
"""Fail if layer 8 (en_letters) has drifted away from the en layer.

en_letters is a deliberate copy: it is raised over ru whenever a modifier or the
herdr prefix needs Latin scancodes, and it has to carry real bindings because
&trans there would fall straight back through to ru. The one difference is
position 32, which is &trans so the layout switch reads ru and keeps its true
direction. If the switch ever moves again, this exception moves with it.

Nothing in the devicetree enforces that copy, so this does. It runs from
tests/run.sh before any case is built.
"""
import re
import sys
from pathlib import Path

KEYMAP = Path(__file__).resolve().parent.parent / "config" / "op36_ruen.keymap"

# Thumb index (0..5, i.e. positions 30..35) carrying the layout switch, and so
# the one position where en_letters must be &trans rather than a copy of en.
SWITCH_THUMB = 2


def layers(text):
    """Map layer name -> (three letter rows, six thumb tokens)."""
    out = {}
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() != "bindings = <":
            continue
        name = lines[i - 1].strip().rstrip("{").strip()
        rows = [re.split(r" {2,}", l.strip()) for l in lines[i + 1:i + 4]]
        thumbs = re.split(r" {2,}", lines[i + 4].strip())
        out[name] = (rows, thumbs)
    return out


def main():
    found = layers(KEYMAP.read_text())
    missing = [n for n in ("en", "en_letters") if n not in found]
    if missing:
        sys.exit(f"check-en-letters: no {' or '.join(missing)} layer in {KEYMAP.name}")

    en_rows, en_thumbs = found["en"]
    cp_rows, cp_thumbs = found["en_letters"]

    problems = []
    for r, (a, b) in enumerate(zip(en_rows, cp_rows)):
        for c, (x, y) in enumerate(zip(a, b)):
            if x != y:
                problems.append(f"position {r * 10 + c}: en has {x!r}, en_letters has {y!r}")

    for i, (x, y) in enumerate(zip(en_thumbs, cp_thumbs)):
        want = "&trans" if i == SWITCH_THUMB else x
        if y != want:
            problems.append(f"position {30 + i}: en_letters has {y!r}, expected {want!r}")

    if problems:
        print("en_letters has drifted from en:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            f"\nen_letters is a copy of en with &trans at position "
            f"{30 + SWITCH_THUMB}, where the layout switch lives. Mirror the\n"
            "change into both layers, or the English letters under a modifier\n"
            "on ru stop matching the ones you actually type.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
