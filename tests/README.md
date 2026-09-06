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
faithfully. Host behavior (Caps Lock language switching, Spotlight, the Latin
fallback for Cmd shortcuts) is equally out of scope.

## Some of the cases

- **`cmd-space-in-order`** — right Cmd (`J`, position 16) pressed, then left Space
  (position 31). Cmd reaches the host immediately via `hold-while-undecided`, and
  the chord comes out as `0xE7` + `0x2C`. This is the configuration working.
- **`cmd-space-reordered`** — the same chord with Space arriving first, which is
  what BLE latency on the peripheral half does to the ordering. Space claims the
  single undecided-hold-tap slot, a bare space is emitted, `J` is captured and
  replayed too late, and `require-prior-idle-ms` then resolves it to a tap. Note
  the `Unable to release keycode` line: with `hold-while-undecided` set, ZMK
  releases a modifier it never pressed when quick-tap wins.
- **`numbers-layer`** — holds Space (position 31) past the tapping term and checks
  what `numbers` resolves to: `1` on position 26, Prev Win as `0x38` with
  `implicit_mods 0x0B`, Term as `0x1F` with `0x0F`, the plain `&kp LGUI` on
  position 13 held over a digit, and Space still tapping afterwards.
- **`adj-both-orders`** — the two routes into `adj`. Space first raises `numbers`
  and Backspace lifts it to layer 6; Backspace first raises `nav` and Space does
  the same. The `layer N position: …` lines are the point: position 5 must
  resolve on layer 6 either way.
