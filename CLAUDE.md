# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A ZMK **user config** (`zmk-config`), forked from Ergohaven's. There is no application code, no test
suite, and no local toolchain checked in — the entire repo is devicetree keymaps, Kconfig fragments,
and a build matrix. Firmware is produced by GitHub Actions.

**Only the Omega Point 36 (`op36`) with the RU/EN keymap is built**, and `config/op36_ruen.keymap`
is the only file that receives real work. `config/` still holds the upstream keymaps for velvet_v3,
velvet_v3_ui, k03, imperial44 and the trackballs, plus the non-RU `op36.keymap`, but no `build.yaml`
entry references any of them, so they are never compiled. Treat them as reference material, not as
code that has to keep working.

Everything builds against the **`ergohaven/ergohaven-zmk`** ZMK fork (`config/west.yml`), which
supplies the `ergohaven` board, all shields, and pieces the upstream ZMK tree does not have:
`scroll-snap.dtsi`, `input/processors/sensor_rotation.dtsi`, the `zmk,input-processor-temp-layer`
compatible, and the `keymap:` build-matrix key. When something referenced by a keymap isn't in this
repo, it lives in that fork.

## Building

Push (or open a PR, or dispatch manually) → `.github/workflows/build.yml` delegates to
`ergohaven/ergohaven-zmk/.github/workflows/build-user-config.yml@main`, which builds every entry in
`build.yaml` and uploads a `firmware` artifact of `.uf2` files. There is no lint or test step, and
**that CI run is the only compile check that exists** — nothing here validates a keymap locally.

The CI log is a genuinely useful instrument, not just a pass/fail. The workflow dumps the fully
resolved `zephyr/.config` and the built devicetree, so questions like "is this Kconfig symbol
actually on?" or "did this keycode fold to the right value?" are answered from the log rather than
from Kconfig defaults or memory:

```sh
gh run view <run-id> --log | grep -oE "CONFIG_ZMK_STUDIO[A-Z_]*=[^ ]*" | sort -u
```

Always confirm the grep works by checking a symbol you know is set (`CONFIG_ZMK_STUDIO=y`) — the
workflow filters out `# ... is not set` lines, so an empty result and a broken pipeline look alike.

To reproduce the main entry locally:

```sh
west init -l config && west update && west zephyr-export
west build -s zmk/app -d build/op36_left -b ergohaven -S studio-rpc-usb-uart -- \
  -DZMK_CONFIG="$PWD/config" -DSHIELD=op36_left \
  -DKEYMAP_FILE="$PWD/config/op36_ruen.keymap" -DCONFIG_ZMK_STUDIO=y
```

`west init -l` clones `zmk/`, `zephyr/`, `modules/`, `bootloader/` and `tools/` into the repo root
and none of them are gitignored — build outside the repo, or just push and let CI do it.

## Flashing and the split

The op36 shield's `Kconfig.defconfig` in the fork gives `ZMK_SPLIT_ROLE_CENTRAL` to
`SHIELD_OP36_LEFT`, so the **left half is central and holds the whole keymap** (confirmed in the
build's own `.config`). The right half is a peripheral: it scans its matrix and ships key positions
over BLE, and the central resolves what they mean.

- **Keymap changes only require reflashing the left half.** There is no and cannot be a "ruen"
  build of the right half.
- `settings_reset-ergohaven-zmk.uf2` clears stored settings and BLE pairings: flash it to both
  halves, then flash the normal firmware back.

**If keys appear broken, suspect the split connection before the keymap.** A disconnected right half
presents as "modifiers don't work" or "that key does nothing" rather than as an obviously dead half,
because the failing keys are simply the ones living on it. Power-cycling both halves has already
resolved one session's worth of apparent keymap bugs — do that first, every time.

## How build.yaml maps to config/

`build.yaml` is the source of truth for what gets built. Every entry has `board: ergohaven`; the
`shield` selects keyboard and role. A file is only compiled if some matrix entry pulls it in.

- **`config/<shield>.conf`** — Kconfig fragment matched by shield name, so `op36_left` and
  `op36_right` share `op36.conf`.
- **`config/<keymap>.keymap`** — defaults to the base name; the `keymap:` matrix key (an ergohaven
  fork extension resolving to `-DKEYMAP_FILE=`) overrides it, which is how `op36_ruen` is built
  instead of `op36`.
- **`config/<keymap>.json`** — physical layout for the keymap editor, matched to the **keymap**
  filename, not the shield. The `*_ruen.json` files are symlinks to their base `.json`; keep them
  symlinks.

Shield vocabulary: `_left`/`_right` are the halves, `_qube` is the wireless dongle (renamed from
"dongle" historically), `settings_reset` wipes stored settings. No dongle is in use here.

## Editing the keymap file

`op36_ruen.keymap` is column-aligned in the keymap editor's format, and **the thumb row participates
in the column widths** — column 3's width is set by `&kp BACKSPACE`, not by the letter rows. Naive
re-rendering silently breaks alignment. The layout algorithm is:

- `w[c]` = longest token in column `c` across rows 0-2, additionally maxed with thumb tokens for
  columns 3, 4, 5 and 6 (thumb tokens 0, 1, 4, 5 respectively).
- Columns start at 2 and advance by `w[c] + 2`, except between the halves: after column 4's slot
  come the two middle thumb tokens (separated by 4 spaces, not 2), and column 5 starts after them.

When editing programmatically, **write a renderer and first prove it reproduces the file byte for
byte with no substitutions applied**, then apply changes. That check caught the thumb-row rule on the
first attempt; without it the diff would have been full of spurious realignment.

## Layers

Six layers, and the index order matters: `en`=0, `ru`=1, `sym_en`=2, `sym_ru`=3, `nav`=4, `adj`=5.
The other (unbuilt) `_ruen` keymaps use a different order, so never copy a `&mo N` across files.

```
en ──Space/Bspc──> nav ──Bspc──> adj
 │                                ▲
 └───Esc/Enter───> sym_en ─Space──┘
ru ──Esc/Enter───> sym_ru ─Space──┘   (ru reaches nav the same way as en)
nav has &to 0 / &to 1 for language resync
```

Positions are 0-9 / 10-19 / 20-29 for the three rows, then 30-32 (left thumbs) and 33-35 (right).

**Home row mods** sit on `en`, `ru` and (as plain `&kp`) the right hand of `nav`:
Alt on A/`;`, Shift on S/L, Ctrl on D/K, **Cmd on F/J**. `hml`/`hmr` carry
`hold-trigger-key-positions` listing the whole opposite hand **plus all six thumb positions**, so a
mod engages when the next key is on the other hand or on any thumb. Those lists are position-based —
changing key count or ordering invalidates them, but moving a mod between positions inside the same
half does not. See "Positional hold-tap" below for why the thumbs are in both lists.

On `nav` the right-hand mods are deliberately **plain `&kp RGUI/RCTRL/RSHFT/LALT`, not hold-taps**:
the arrows moved to the left hand, so there is nothing to tap, and `require-prior-idle-ms` would
make a hold-tap resolve as a useless tap right after typing.

The symbol layers carry only two mods, both on the left: Shift on `_` and Cmd on `$`.

## The thumb row

Four actions, one pair per layer target, each pair giving you one free hand:

| | left | right | leads to |
|---|---|---|---|
| **nav** | 31 Space | 34 Backspace | `nav` |
| **symbols** | 32 Esc | 33 Enter | `sym_en` from `en`, `sym_ru` from `ru` |

Positions 30 and 35 are deliberately unused.

**The rule: base layers (`en`, `ru`) hold the real bindings; every higher layer is `&trans`.** There
are exactly two exceptions, both mandatory because the target must differ: position 31 on the symbol
layers and position 34 on `nav` are `&lt 5 …` into `adj`.

This rule is load-bearing, not cosmetic. Because each layer now has two entrances, a thumb position
is no longer guaranteed to be the held key, and an `&none` left over from when it was **silently
kills that key**. Two such dead keys already shipped this way. After any thumb-row change, resolve
every position against every reachable layer stack — `en`/`ru` alone, each with `nav` (entered both
ways), each with its symbol layer (entered both ways), and `adj` by both routes — accounting for
which key is held in each case.

## Timing and hold-tap behaviors

`&mt` and `&lt` are both re-tuned at the top of the file; `hml`, `hmr` and `hml_en` are defined in
the behaviors block.

**`&lt` must be overridden.** ZMK ships it as `flavor = "tap-preferred"`, `tapping-term-ms = 200`,
no `quick-tap-ms`, and tap-preferred only reaches the layer once the term expires — a fast
opposite-hand press would beat the layer to it. This config uses `balanced` plus
`quick-tap-ms = 200`, the latter so tap-then-hold repeats Space and Backspace instead of switching
layers.

Space and Enter running through a hold-tap is the main ergonomic risk in this keymap: with
`balanced`, pressing and releasing the next key before releasing the thumb hands the layer the win.
`tapping-term-ms` is the knob if that shows up in real typing.

`hml_en` is `hml` with `bindings = <&kp>, <&en>` instead of `<&kp>, <&kp>`. A hold-tap passes its
first parameter to the hold binding and the second to the tap binding, so `&hml_en LGUI DLLR` gives
Cmd on hold and `$` through the language-switching macro on tap. It exists because `$` cannot be
reached with a plain `&kp` on the Cyrillic layout. Note that it references `&en` from the
`behaviors` block while the macro is defined further down in `macros` — devicetree resolves labels
after parsing the whole tree, so referring forward like this is fine (verified in the built
devicetree, where the node comes out as `bindings = < &kp >, < &en >`).

## Positional hold-tap: how it actually decides

All from `app/src/behaviors/behavior_hold_tap.c` in the fork. This was the source of a real bug —
same-hand `Cmd+Space` was impossible by construction, and the workaround people find ("hold the
modifier, wait, then press") hides it, so the symptom reads as "the timing is too slow".

- **`decide_positional_hold()` only consults the list if another key was pressed before the
  decision.** It returns early when `position_of_first_other_key_pressed == -1`. A hold resolved by
  the timer alone therefore **bypasses the positional check entirely** — which is exactly why
  holding and waiting appears to work while the fast chord does not.
- **A position not in the list forces a tap**, it does not merely decline the hold. So a modifier
  chorded with a key outside its list can never work, at any speed.
- **The thumbs must be in both lists.** The list's purpose is to stop same-hand letter rolls from
  raising a modifier; thumbs never take part in letter rolls, so restricting them buys nothing and
  makes every same-hand modifier+thumb chord (`Cmd+Space`, `Cmd+Backspace`, `Shift+Space`)
  impossible.
- **What guards mid-typing false triggers is `require-prior-idle-ms`, not the position list.**
  `is_quick_tap()` resolves the hold-tap as a tap immediately when *any* key was tapped within the
  window (150 ms here), which is what keeps "if " and "of " from raising Cmd. Raise that value if
  false triggers appear; do not narrow the position lists.
- **Flavor semantics:** `balanced` decides hold on the other key's **release**, `hold-preferred` on
  its **press**, `tap-preferred` on the **timer only**. With `hold-trigger-on-release` the position
  is recorded on the other key's release rather than its press.
- **Only one hold-tap may be undecided at a time.** `on_hold_tap_binding_pressed()` returns
  immediately when `undecided_hold_tap != NULL`, so a second hold-tap pressed during that window is
  dropped whole — no modifier, no tap, nothing. With hold-taps on both the home row and the thumbs,
  pressing the thumb even slightly first swallows the modifier entirely and yields a bare Space.
  Press the modifier first; this is not tunable.
- **`hold-while-undecided` is set on `hml`, `hmr` and `hml_en`.** It presses the hold binding at
  `HT_KEY_DOWN` before any flavor decision and returns, so the modifier reaches the host the instant
  the key goes down. Do **not** add `-linger`: without it the modifier is released before the tap is
  sent, with it the tap would come out modified (`Cmd+j` instead of `j`).
- The usual objection to `hold-while-undecided` — a modifier flickering on every home-row letter —
  does not apply here, because `is_quick_tap()` is evaluated *before* `HT_KEY_DOWN`. When it fires
  the status is no longer `UNDECIDED`, so `decide_hold_tap()` returns at its first guard and the
  modifier is never pressed. With `require-prior-idle-ms = 150` that covers ordinary typing, and the
  instant modifier only appears after a pause — exactly the deliberate-chord case. The flip side:
  a chord started within 150 ms of the last keystroke still gets no modifier at all.

### An open question

`Cmd+Space` with right-hand Cmd (`J`, position 16) and left-hand Space (position 31) has been
reported as frequently producing a bare space — **no `j` and no modifier**, which rules out both an
ordinary tap resolution and a positional rejection, since either would emit `j`. The only path in
the source that produces nothing at all is the `undecided_hold_tap != NULL` early return.

A plausible cause specific to this chord: `J` is on the **peripheral** half and its events cross BLE,
while Space is on the central and is handled locally, so Space can win the race and claim the
undecided slot even when `J` was pressed first. If that is what is happening, no timing parameter
fixes it. The discriminating test is the same chord with left-hand Cmd (`F`), where both keys are on
the central half: if that is reliable while the right-hand version is not, the split latency is the
cause.

## The RU/EN dual-layout system

`op36_ruen.keymap` keeps a firmware layer (`en`=0, `ru`=1) in sync with the host's input language.
**The host must have "switch languages using Caps Lock" enabled** — confirmed working on both macOS
and iPadOS, which are the target platforms.

The critical constraint: Caps Lock is a **toggle**, not a selector. There is no "set the host to EN"
on these platforms — Windows' `Ctrl+Shift+1`/`Ctrl+Shift+2`, which this keymap used to send, does
nothing on iPadOS. Everything below follows from that.

- `os_lang` sends `&kp CAPS` and nothing else. `to_en`/`to_ru` no longer exist, because under a
  toggle they would be the same action.
- `layer_en` / `layer_ru` are `&to 0 &os_lang` / `&to 1 &os_lang`, correct **only when invoked from
  the layer they are leaving**. The only callers are the `cmben`/`cmbru` combos, which are
  layer-scoped so each fires from its own side. Never bind these to anything unconditional —
  pressing "go to EN" while already on EN flips the host and desynchronizes it.
- **`nav` positions 6 and 7 are `&to 0` / `&to 1`** — firmware-only resync, deliberately sending no
  keystroke. If host and firmware disagree, press the one matching the host's actual layout.
- The `en` one-param macro types a single key in EN and returns, for glyphs absent from Cyrillic. It
  survives toggle semantics because it is balanced — flip, type, flip back — which holds as long as
  the host's language cycle has exactly two stops (verified: the emoji keyboard stays out of it).
- `wait-ms = <50>` on the `en` macro is a guess at how long the host needs, not a measured value.
  Fourteen keys on `sym_ru` now route through it, so a wrong glyph there means raising it.

**Cmd shortcuts do not need the `en` wrapper.** Verified on device: `Cmd+Shift+[` works with the
Russian layout active, because macOS and iPadOS fall back to the Latin equivalent when matching
command shortcuts. Bind them directly as `&kp LG(LS(LBKT))`. Splitting `nav` per language would have
been pointless anyway — the firmware sends a scancode, and `[` has no Cyrillic scancode to send
instead.

**Glyphs with no Cyrillic equivalent**, i.e. the ones that must go through `&en` on `sym_ru`:
`[ ] ' | { } $ ~ ` ` — `keys_ru.h` has no `RU_LBKT`, `RU_RBKT`, `RU_SQT`, `RU_PIPE`, `RU_LBRC`,
`RU_RBRC`, `RU_DLLR`, `RU_TILDE` or `RU_GRAVE`. Everything else has an `RU_*` form; check the header
before assuming.

`keys_ru.h` itself is a generated Unicode-licensed header — vendored, don't hand-edit. Every keymap
in the repo includes it, even ones with no Cyrillic bindings.

## Combos

Bounded by `config/op36.conf`: `CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO=3`,
`CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY=7`.

A combo's `layers = <N>` is checked against `zmk_keymap_highest_layer_active()` (see
`app/src/combo.c` in the fork), **not** against "is that layer active anywhere in the stack". A
combo scoped to `ru` therefore does not fire while `nav` is held on top of it.

`cmben`/`cmbru` (layout switch) carry `require-prior-idle-ms = <150>` because they sit on positions
2+3 — `E`+`R` in Latin, `У`+`К` in Cyrillic, both same-hand rolls occurring in ordinary words.
`kha`/`hrdsgn` deliberately do **not** have that guard: Х and Ъ are needed mid-word ("плохо",
"объект"), and it would suppress exactly those cases.

## ZMK internals worth not re-deriving

All checked against the fork's source or the build's own output:

- **Macro `wait-ms` is only charged for real bindings**, not for control bindings like
  `&macro_press` (`app/src/behaviors/behavior_macro.c`). `&macro_wait_time <ms>` sets it inline when
  a pause is needed at one point only.
- **`&out OUT_BLE` / `OUT_USB` is a preference, not a switch.** `get_selected_transport()` in
  `app/src/endpoints.c` uses it only when both transports are ready and otherwise takes whichever
  one is. You cannot strand yourself without output. The choice persists to flash.
- **`&bt BT_CLR` clears only the active profile's bond** (`zmk_ble_clear_bonds`), then re-advertises.
  It does **not** touch the split pairing — host profiles live in `profiles[]`, the peripheral
  address in `peripheral_addrs[]`. `BT_CLR_ALL` (not bound here) clears everything.
- **Studio locking is on** with stock values: `ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC=600` and
  `ZMK_STUDIO_LOCK_ON_DISCONNECT=y`. `&studio_unlock` is the physical unlock that stops a host from
  silently rewriting the keymap.
- The firmware declares **no backlight of any kind** — ZMK's HID layer has no backlight or
  brightness usage at all, `ZMK_HID_INDICATORS` is off, and the device identifies as VID `0x1D50` /
  "ZMK Project", not as Apple hardware. An iPad "Keyboard Brightness" setting comes from a Smart
  Connector accessory, not from this keyboard.

## Keeping this file current

Write new findings here as they are worked out, in the same turn, without being asked. The hard part
of this repo is not the devicetree — it is the accumulated knowledge about ZMK internals and host
behavior, each item of which cost a source dive or an on-device test. Record the evidence trail
(which source file, which check) so the next instance can re-verify rather than trust prose.

## Known gaps

Deliberate, pending later work — do not "fix" them unprompted:

- **Digits 0-9 are not bound anywhere.** They left `nav` when the arrows moved in.
- **Home, End, Insert, Delete, PageUp, PageDown, PrintScreen** are likewise unbound.
- **`RU_CYRILLIC_IO` (ё) is not bound.** The `ru` layer holds 30 letters and the `kha`/`hrdsgn`
  combos add Х and Ъ, for 32 of 33.
- `adj` is stripped to `&bootloader`, the four `&bt BT_SEL`, `&bt BT_CLR`, both `&out` and
  `&studio_unlock`; everything else on it is `&none` by intent.

## Dormant config (not built)

Reference only. Trackball keymaps declare their own `default_transform` inline, so one `trackball`
shield serves three devices with per-variant tuning in `cmake-args`. `velvet_v3_ui*` raises a Mouse
layer on pointer motion via `zmk,input-processor-temp-layer`. `k03` binds six encoders through a
shared `enc_vol` and `imperial44` two through `&inc_dec_kp`; both need `CONFIG_EC11=y` and
`CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y`.
