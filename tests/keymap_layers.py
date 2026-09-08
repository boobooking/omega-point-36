#!/usr/bin/env python3
"""Parse the layer blocks out of the keymap.

Shared by the checks that guard the two hand-kept layer copies: en_letters
against en, and numbers_ru against numbers_en. Layers are found by node name,
never by index, so renumbering them does not reach this file.
"""
import re
from pathlib import Path

KEYMAP = Path(__file__).resolve().parent.parent / "config" / "op36_ruen.keymap"


def split_row(line):
    """Tokens of one rendered row; the format separates them by two spaces."""
    return re.split(r" {2,}", line.strip())


def layers(text):
    """Map layer name -> (three letter rows, six thumb tokens)."""
    out = {}
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() != "bindings = <":
            continue
        name = lines[i - 1].strip().rstrip("{").strip()
        rows = [split_row(l) for l in lines[i + 1:i + 4]]
        thumbs = split_row(lines[i + 4])
        out[name] = (rows, thumbs)
    return out


def require(found, *names):
    """Exit with a message naming whichever layers are missing."""
    missing = [n for n in names if n not in found]
    if missing:
        raise SystemExit(f"no {' or '.join(missing)} layer in {KEYMAP.name}")
