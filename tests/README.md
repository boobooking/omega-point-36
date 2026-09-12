# Keymap simulator tests

`config/op36_ruen.keymap` built for ZMK's `native_posix_64` board, which replaces
the key matrix with a mock scanner that replays timed press/release events. The
firmware runs as an ordinary Linux binary and logs both the HID output and the
internal hold-tap decisions, so timing-dependent behavior can be checked without
flashing anything.

```sh
./tests/run.sh                        # every case
./tests/run.sh cmd-space-in-order     # one case
ACCEPT=1 ./tests/run.sh <case>        # rewrite the snapshot after an intended change
```

Everything runs in `zmkfirmware/zmk-build-arm:stable`; the first run clones ZMK
and its west dependencies into `$WS` (default `$TMPDIR/zmk-sim-ws`, ~1.5 GB).
The clone is `zmkfirmware/zmk` at tag **v0.3.0**, matching what the firmware
ships; keep the two in step or these cases stop describing the real board.

## What a case looks like

Four files per directory:

- **`native_posix_64.keymap`** — widens the board's mock `&kscan` to 4x10, adds a
  transform, lists the events, and then `#include`s the real keymap from `/cfg`.
  The repo's `config/` is mounted there, so cases test the file that ships, not a
  copy of it.
- **`native_posix_64.conf`** — the parts of `config/op36.conf` that affect keymap
  behavior (currently the combo limits).
- **`events.patterns`** — sed filters selecting the log lines to compare.
- **`keycode_events.snapshot`** — expected output; the test is a diff against it.

Positions map straight onto the matrix: op36 position N is `RC(N/10, N%10)`, so
the thumbs 30-35 are simply row 3. `ZMK_MOCK_PRESS(row, col, msec)` presses and
then waits, so chord timing is exact.

**Start each case with a lead-in press and a pause of 200 ms or more.** At t=0
the "last tapped" timestamp is zero, so `require-prior-idle-ms` treats the very
first key as a quick tap and resolves every hold-tap to its tap. The lead-in also
matches how the chord is really used: after a pause in typing.

## What it cannot test

There is one node — **no split, no peripheral half, no BLE.** Anything caused by
event ordering between the halves is invisible here, which is exactly the class
of bug that `cmd-space-reordered` exists to illustrate rather than reproduce
faithfully. Host behavior (input-source switching, Spotlight, the Latin
fallback for Cmd shortcuts) is equally out of scope.

## The cases

Every case starts with a lead-in press on position 1 and a 400 ms pause, so the
counts below exclude the `0x1A` that produces. Position 0 cannot serve as the
lead-in: it is `&none` on `en`.

- **`cmd-space-in-order`** — right Cmd (position 16) pressed, then left Space
  (position 31). Releasing Space decides position 16 as `hold-interrupt`, so
  `0xE7` goes down before the captured Space is replayed and the chord comes out
  as `0xE7` + `0x2C`. This is the configuration working.
- **`cmd-space-reordered`** — the same chord with Space arriving first, which is
  what BLE latency on the peripheral half does to the ordering. Space claims the
  single undecided-hold-tap slot, a bare space is emitted, position 16 is
  captured and replayed too late, and `is_quick_tap` resolves it to a tap: `0x2C`
  then `0x11`.
- **`chord-repeat-fast`** — that chord five times over, the gaps between chords
  shrinking to 30 ms. Counts how many repeats keep their modifier: at the current
  100 ms guard four do and the fifth falls inside the window, so the snapshot
  ends in a bare `0x11`. This is the case to re-run before touching
  `require-prior-idle-ms`.
- **`typing-roll`** — `"at "` typed as a roll, Space going down while `t` is still
  held. Both letters carry a home row mod on this layout, `a` Alt and `t` Cmd, so
  the roll runs through two hold-taps and must still come out `0x04 0x17 0x2C`
  with no modifier. This is what stops a narrower guard, or thumbs put back into
  the own-hand lists, from turning ordinary typing into Spotlight. The other case
  to re-run before touching `require-prior-idle-ms`.
- **`cmd-enter-crosshand`** — Enter (position 33, right half) chorded with the
  LEFT Cmd (position 13), four times with shrinking gaps. All four give
  `0xE3` + `0x28`: the opposite-hand rule satisfied.
- **`cmd-enter-twice`** — right Cmd (position 16) with Enter, both on the right
  half. A same-hand chord, which `hmr`'s hold-trigger list refuses by forcing a
  tap, so the output is `0x11` + `0x28` — the letter and Enter, no modifier. Kept as
  documentation of that rule, not as a bug.
- **`cmd-enter-reordered-twice`** — the same pair with Enter arriving first. Same
  keycodes as `cmd-enter-twice` but a different route to them: the hold is lost
  to `quick-tap` rather than refused positionally, and `j` lands after Enter
  instead of before. Within each of the two cases the second attempt reproduces
  the first line for line, which is how the keymap was shown to be deterministic
  where the hardware is not.
- **`enter-repeat-under-cmd`** — position 16 held down while Enter is tapped five
  times. Both keys are on the right half, so the same refusal applies: position 16
  resolves to a bare `0x11` and the five `0x28` arrive unmodified.
- **`numbers-layer`** — holds Space (position 31) past the tapping term to raise
  `numbers_en`, then checks what that layer resolves to: `0x1E` on position 26, Prev
  Win as `0x38` with `implicit_mods 0x0B`, Term as `0x1F` with `0x0F`, the plain
  `&kp LGUI` on position 13 held over a digit, and Space still tapping afterwards.
  The tail taps position 11 on its own, the layer's Shift: a plain `&kp LSHFT`
  now that Escape lives on the thumb cluster, so it goes down and up as `0xE1`
  with no keycode of its own.
- **`adj-key`** — `adj` hangs off position 30 as `&ltadj L_ADJ 0`, a hold-tap
  whose tap is `&none`. Held past the tapping term, position 6 must resolve on
  `adj` as `bluetooth`; tapped, the key emits nothing at all and `adj` never
  comes up. That second half is the guard, not a formality: `adj` carries
  `&bootloader` and the BT profile keys on the right hand now, which is where a
  letter rolled after the left thumb lands. Position 35, the thumb `adj` left
  behind, must do nothing at all. The tail holds Space and then Backspace 300 ms
  apart: that still gives `numbers` over `nav`, and does not switch the
  language, because a combo needs both presses inside `timeout-ms`.
- **`thumb-switch`** — the layout switch, back on the `cmbru`/`cmben` combo.
  Thumbs 31+34 struck together emit one Globe and flip the layer, so position 1
  reads `0x14` (й) and then `0x1A` (W) again. Position 32, the key the switch
  vacated, taps Escape (`0x29`) and nothing else. Tapped far apart the same two
  thumbs are still Space (`0x2C`) and Backspace (`0x2A`): outside `timeout-ms`
  the combo must not fire.
- **`hyper`** — holds `F` (position 2) past the term and chords `M`, then taps
  `F`, then holds `U` (position 7) and chords `G`. Each hold must emit all four
  modifiers as separate `0xE0`-`0xE3` keycodes that are still down when the
  chorded key is sent; that is what fails if Hyper is ever rewritten as
  `&kp LS(LC(LA(LGUI)))`. It ends by pressing 2+3, which the layout switch has
  vacated, to record that they now simply type `f` and `p`.
- **`ru-symbols`** — proves the keymap side of the symbol layers is right: from
  `ru`, Esc and Enter both raise **layer 4**, and positions 5, 26 and 27 send
  `LS(0x24)`, `LS(0x38)` and `0x38` — `?`, `,` and `.` as a Russian host reads
  them. Position 26 is the `ru_ext` entrance on `ru`, so `sym_ru` has to win over
  it. Written to separate a keymap fault from a host-language desync; those
  scancodes on an *English* host read `&`, `?` and `/`.
- **`switch-under-mod`** — the switch combo struck while a modifier is held on
  `ru`. It must not fire: the modifier raises `en_letters`, and a combo's
  `layers` is checked against the highest active layer alone, so neither
  direction is armed there. No Globe may appear inside the hold, and `ru` must
  still be active afterwards. Position 32 under the same modifier taps Escape,
  which is the safe answer that key could not give while it carried the switch.
- **`ru-mod-switch`** — the reason `hml_ru`/`hyl_ru` exist. On `ru`, right Cmd
  over position 4 must resolve on layer 0 and send `0x05` (B), not `0x17`; right
  Alt and left Hyper must do the same; right **Shift** must not, so position 1
  stays `0x14` (й) rather than `0x1A` (W). Between and after every hold, position
  1 has to be back on layer 1 — that is the `&to 1` restore.
- **`hyper-ru`** — the same three checks on `ru`, where positions 2 and 7 are
  `у` and `ш`: the hold emits `0xE0`-`0xE3` around `0x0B`, the tap gives `0x08`,
  and both `cmbru` (entering) and `cmben` (leaving) fire from under the hold-tap.
  Positions 6+7 and 7+8 once held the kha/hrdsgn combos, so this also records
  that nothing competes for that key any more.
- **`ru-ext`** — switches to `ru` with the `cmbru` combo, then reaches all seven
  letters of `ru_ext` through both entrances: holding position 23 for `щ э х ъ`
  and position 26 for `ц ё ф`. Both decide `hold-timer (tap-preferred …)`, and
  `ф` lands on position 10, where `ru_ext` has to beat a home row mod.
- **`ru-ext-rolls`** — the counterpart: `мо` and `ть ` rolled at typing speed.
  Neither may raise `ru_ext`, so the snapshot has no `mo_pressed` line at all and
  every letter survives. This is the case that fails if `lt_ext` is ever changed
  to the file's `balanced` flavour.
