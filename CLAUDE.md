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

Reading the **devicetree** out of that log needs one more step: every line carries a job name and
timestamp in front, so anchored patterns silently match nothing. Strip the prefix and pick the one
build you care about first — several jobs share the log:

```sh
gh run view <run-id> --log > /tmp/ci.log
grep "op36_left" /tmp/ci.log | sed -E 's/^.*[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+Z //' > /tmp/dt.txt
sed -n '/: en_mod {/,/};/p' /tmp/dt.txt
```

This is how the forward references were confirmed on real firmware rather than in the simulator:
`hml_ru` comes out as `bindings = < &en_mod >, < &kp >`, and `hyper_en` as
`< &macro_press &kp 0x700e0 &kp 0x700e1 &kp 0x700e2 &kp 0x700e3 >` — usages 0xE0-0xE3, the four
modifiers.

To reproduce the main entry locally:

```sh
west init -l config && west update && west zephyr-export
west build -s zmk/app -d build/op36_left -b ergohaven -S studio-rpc-usb-uart -- \
  -DZMK_CONFIG="$PWD/config" -DSHIELD=op36_left \
  -DKEYMAP_FILE="$PWD/config/op36_ruen.keymap" -DCONFIG_ZMK_STUDIO=y
```

`west init -l` clones `zmk/`, `zephyr/`, `modules/`, `bootloader/` and `tools/` into the repo root
and none of them are gitignored — build outside the repo, or just push and let CI do it.

## Simulating the keymap

`./tests/run.sh` builds `config/op36_ruen.keymap` for ZMK's `native_posix_64`
board — the firmware becomes an ordinary Linux binary whose key matrix is a mock
scanner replaying timed events — and diffs the resulting log against a snapshot.
It runs in `zmkfirmware/zmk-build-arm:stable`; the first run clones ZMK and its
west dependencies into `$WS` (default `$TMPDIR/zmk-sim-ws`, ~1.5 GB), later runs
reuse it. See `tests/README.md` for the shape of a case.

**Reach for this before theorizing about any hold-tap, layer, combo or macro
timing question.** The log carries not just the HID output but the decision
itself — `ht_decide: 16 decided hold-interrupt (balanced decision moment
other-key-up)` — so questions that were answered here by three successive wrong
guesses are answered by one run. Cases mount `config/` into the container and
`#include` the real keymap, so they test the file that ships.

Two gotchas worth knowing before writing a case:

- **Start with a lead-in press and a pause of 200 ms or more.** At t=0 the
  "last tapped" timestamp is zero, so `require-prior-idle-ms` sees the very first
  key as a quick tap and collapses every hold-tap to its tap. The lead-in has to
  land on a key that actually emits something — the cases use position 1, since
  position 0 is `&none`.
- Positions map straight onto the matrix: position N is `RC(N/10, N%10)`, so
  thumbs 30-35 are simply row 3. No translation layer is needed.

**It cannot model the split.** One node, no peripheral half, no BLE — so anything
caused by event ordering between the halves is out of reach, and so is host
behavior (Caps Lock switching, Spotlight, the Latin fallback for Cmd shortcuts).
`tests/cmd-space-reordered` illustrates that class of bug by hand-ordering the
events; it does not reproduce the hardware faithfully, and the difference is
visible — the simulator emits a `j` there where the real keyboard emits none.

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

Eight layers, and the index order matters: `en`=0, `ru`=1, `ru_ext`=2, `sym_en`=3, `sym_ru`=4,
`nav`=5, `numbers`=6, `adj`=7. The other (unbuilt) `_ruen` keymaps use a different order, so never
copy a `&mo N` across files.

```
en ──Space─────> numbers ──Bspc───┐
 │                                ├──> adj
 ├──Bspc───────> nav     ──Space──┘
 └──Esc/Enter──> sym_en
ru behaves identically, except Esc/Enter reach sym_ru
ru ──hold pos 23 or 26──> ru_ext        (the seven letters that did not fit)
nav has &to 0 / &to 1 for language resync
```

Positions are 0-9 / 10-19 / 20-29 for the three rows, then 30-32 (left thumbs) and 33-35 (right).

`en` carries Colemak-DH with `Q`, `Z` and `J` pulled off the pinky columns into the grid. `ru` is
ЙЦУКЕН squeezed onto the same 26 keys: `ц`, `щ`, `ф` and `э` are dropped and `я` moves from the
bottom-left corner up to position 10. The layers are independent — each sends the scancodes its own
host layout expects — so the asymmetry costs nothing.

**Positions 0, 9, 20 and 29 are `&none` on every layer.** Both letter layers gave them up, and the
four higher layers never used them.

**Letters move, positions do not**: the mods, the positional hold-trigger lists and the combos are
all addressed by position, so a layout change touches none of them. The `ru` rework is the proof —
`cmben` still lands on `У`+`К` without the combo being edited.

**Home row mods** sit on `en`, `ru` and — as plain `&kp` — the right hand of `nav` and the left hand
of `numbers`: Alt on A/O, Shift on R/I, Ctrl on S/E, **Cmd on T/N**. `hml`/`hmr` carry
`hold-trigger-key-positions` listing the whole opposite hand **plus all six thumb positions**, so a
mod engages when the next key is on the other hand or on any thumb. Those lists are position-based —
changing key count or ordering invalidates them, but moving a mod between positions inside the same
half does not. See "Positional hold-tap" below for why the thumbs are in both lists.

On `nav` the right-hand mods are deliberately **plain `&kp RGUI/RCTRL/RSHFT/LALT`, not hold-taps**:
the arrows moved to the left hand, so there is nothing to tap, and `require-prior-idle-ms` would
make a hold-tap resolve as a useless tap right after typing. `numbers` carries its left-hand mods
the same way and for the same reason — the digits are on the right hand, and there is no letter
under the mod worth tapping.

**Hyper sits on the top row of both letter layers**, positions 2 and 7 — `F`/`U` on `en`, `у`/`ш` on
`ru` — through `hyl`/`hyr`. Those are `hml`/`hmr` with the hold binding swapped, same flavour, same
guards, same hand-scoped trigger lists; only the row differs, which is a reminder that those lists
describe hands, not rows.

**The hold binding has to be the `&hyper` macro, and `&kp LS(LC(LA(LGUI)))` will not do.** That form
registers `LGUI` as an *explicit* modifier, which persists, but Shift/Ctrl/Alt only as *implicit*
ones — and `hid_listener_keycode_pressed` calls `zmk_hid_implicit_modifiers_press(ev->implicit_modifiers)`
on every keypress, so the next key overwrites the implicit set with its own (usually zero). Hyper
would reach the host as a bare Cmd. Four separate `&kp` modifier keycodes each go through
`zmk_hid_register_mod` (`zmk_hid_keyboard_press` routes the whole `LEFTCONTROL..RIGHT_GUI` range
there) and accumulate until released. `tests/hyper` shows all four as `0xE0`-`0xE3` still down while
the chorded key is sent.

`&hyl 0 F` looks odd: the `0` is the hold parameter, unused because the macro takes none, but the
hold-tap schema includes `two_param.yaml` and demands two cells regardless.

Position 2 also carries the layout-switch combo on both layers — `cmbru` on `en`, `cmben` on `ru`.
Combos are resolved before behaviors, so the combo still wins on a 2+3 press: `tests/hyper` and
`tests/hyper-ru` each end with the switch firing and emitting only its Caps Lock, no letters.

### Modifiers on `ru` drop to `en` for the duration of the hold

`ru` is positional ЙЦУКЕН, so the scancode under a physical key differs from Colemak-DH: position 4
sends `B` on `en` and `T` on `ru`. Shortcuts are matched on the scancode — through the Latin fallback
for Cmd, and straight through the virtual keycode for anything a hotkey daemon registers — so
`Cmd+B` fired from the same finger worked before the Colemak-DH rework and stopped afterwards.

So on `ru`, **holding Cmd, Ctrl, Alt or Hyper runs `&to 0` first and `&to 1` on release**, which puts
the letters back where `en` has them for as long as the modifier is down. `hml_ru`/`hmr_ru` and
`hyl_ru`/`hyr_ru` are the `hml`/`hmr` and `hyl`/`hyr` pairs with the hold binding swapped for the
`en_mod` / `hyper_en` macros; flavour, guards and trigger lists are untouched. Verified end to end in
`tests/ru-mod-switch`.

**Shift is deliberately excluded**, on positions 11 and 18, and must stay that way. It is what types
capital Cyrillic: switching would make Shift plus position 1 emit `W` instead of `Й` and break
Russian input outright. Nothing is lost, because `Cmd+Shift+…` still resolves on `en` — Cmd does the
switching and Shift only contributes a modifier bit.

Two things this rests on, both checked in `app/src/keymap.c`:

- `set_layer_state` refuses to deactivate the default layer ("Default layer should *always* remain
  active"), so `&to 1` leaves both 0 and 1 active with 1 on top — which is why `&trans` on `ru` falls
  through to `en` at all — and `&to 0` from `ru` cleanly drops back to just `en`.
- **`&to` does not touch flash.** Every `settings_save_one` in that file belongs to a Studio keymap
  edit, not to layer activation, so paying `&to` on each modifier press costs nothing.

`&to 1` on the way out is absolute rather than a toggle, so the layer ends up right however the hold
ended. The one failure mode left: if the release never runs — the peripheral half dropping mid-chord,
say — the firmware stays on `en` under a Russian host. `nav` positions 6 and 7 are the resync.

The symbol layers carry only two mods, both on the left: Shift on `_` and Cmd on `$`.

## The thumb row

Four keys, and which one you hold picks the layer. Each leaves one hand free:

| held | leads to | free hand |
|---|---|---|
| 31 Space | `numbers` | right, where the digits are |
| 34 Backspace | `nav` | left, where the arrows are |
| 32 Esc / 33 Enter | `sym_en` from `en`, `sym_ru` from `ru` | the other one |
| **31 and 34 together, in either order** | `adj` | — |

Positions 30 and 35 are deliberately unused.

`adj` is reached by holding both Space and Backspace, and **the order must not matter**. That is why
`numbers` and `nav` each carry the other thumb: on `numbers`, position 34 is `&lt 7 BACKSPACE`; on
`nav`, position 31 is `&lt 7 SPACE`. Space first goes `en → numbers → adj`, Backspace first goes
`en → nav → adj`, and both arrive at the same layer — `tests/adj-both-orders` logs
`mo_pressed: position 34 layer 7` on one route and `mo_pressed: position 31 layer 7` on the other.
The symbol layers deliberately lead nowhere.

The chain of two `&lt` works because the second thumb's press is **captured, not delivered**:
`position_state_changed_listener` returns `ZMK_EV_EVENT_CAPTURED` while the first hold-tap is
undecided, so `on_hold_tap_binding_pressed` — and its "another hold-tap is undecided" early return —
is never reached (`app/src/behaviors/behavior_hold_tap.c`). `decide_balanced` ignores
`HT_OTHER_KEY_DOWN`, so each step resolves on its own `tapping-term-ms` timer and the captured event
replays against the layer that just came up. Consequence: **both thumbs must stay down**. Releasing
the second one early decides the first as `hold-interrupt` and replays the second as a tap, giving a
Backspace or a Space instead of `adj`.

**The rule: base layers (`en`, `ru`) hold the real bindings; every higher layer is `&trans`.** There
are exactly two exceptions, both mandatory because the target must differ: position 34 on `numbers`
and position 31 on `nav` are `&lt 7 …` into `adj`.

This rule is load-bearing, not cosmetic. Because a layer can be entered by more than one route, a
thumb position is no longer guaranteed to be the held key, and an `&none` left over from when it was
**silently kills that key**. Two such dead keys already shipped this way. After any thumb-row change,
resolve every position against every reachable layer stack — `en`/`ru` alone, each with `numbers`,
`nav` or its symbol layer, and `adj` by both routes — accounting for which key is held in each case.

One stack is reachable and worth knowing about: on `sym_en`/`sym_ru` position 31 is `&trans` and
falls through to the base `&lt 6 SPACE`, so holding Esc and then Space raises `numbers` on top of the
symbol layer (6 beats 3). Harmless, and `&none` is not an alternative there — it would kill the space
tap.

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
- **`hold-while-undecided` was tried and removed** — do not add it back without new evidence. It
  presses the hold binding at `HT_KEY_DOWN` before any decision, which sounds like what a sluggish
  chord needs, but the simulator showed it changes nothing for a modifier-plus-thumb chord: the tap
  is replayed *after* the hold-tap resolves either way, so the modifier already precedes it in the
  HID stream. What it did add was an unbalanced release — when `is_quick_tap()` wins, the modifier
  was never pressed yet `release_binding()` still releases it, which the log shows as
  `kp_released: ... 0xE7` followed by `Unable to release keycode`. Both lines disappear from
  `tests/cmd-space-reordered` when the option is off.

### Chords across the split, and why hold-tap timing is the wrong suspect

A modifier chorded with a thumb key was unreliable, and the decisive evidence was an **asymmetry
between the halves**, not any timing value:

| modifier | other key | result |
|---|---|---|
| left Cmd, position 13 (central) | right Backspace (peripheral) | mostly works |
| right Cmd, position 16 (peripheral) | left Space/Enter (central) | essentially never works |

The failure emitted **nothing at all** — neither a modifier nor the letter that position taps. That
rules out an ordinary tap resolution and a positional rejection alike, since both emit the tap. The only path in the source
producing nothing is the `undecided_hold_tap != NULL` early return.

The cause is event **ordering**, not hold-tap timing: the peripheral half's key events cross BLE
while the central's are handled locally, so a central key can reach the keymap first even when the
peripheral key was physically pressed first — and then it claims the single undecided-hold-tap slot
and the modifier is dropped whole.

`CONFIG_BT_PERIPHERAL_PREF_LATENCY` looks like the lever — ZMK ships it at **30**, letting the split
peripheral skip up to thirty connection intervals before it has to be heard — and it was set to 0
for a while on that reasoning. **It did not help, and it has been reverted.** The chord kept failing
on that firmware, and the real cause was `require-prior-idle-ms` in the keymap. All it bought was
battery drain: `BT_PERIPHERAL_PREF_*` applies to whichever link the board is a peripheral on, so
zero stopped the right half sleeping on the split link *and* the left half sleeping on the host
link. Note also that only `config/op36.conf` is merged for both halves (the CI log shows it twice),
so this cannot be scoped to one half without a half-specific conf whose merge behavior is unverified.

The asymmetry that pointed here was real, but it was explained by the ordering of the chord rather
than by any delay long enough to matter.

### require-prior-idle-ms is the knob that loses modifiers on repeats

`is_quick_tap()` compares against the last tap of **any** key, not a neighbouring one, so repeating a
chord suppresses its own modifier: the previous keystroke is still inside the window when the next
modifier goes down, and the hold-tap resolves to a tap. Measured with `tests/chord-repeat-fast`
(right Cmd + left Space, the chord that prompted this):

| `require-prior-idle-ms` | chord repeats surviving | `"of "` typed as a roll |
|---|---|---|
| 150 | 3 of 5 | `o f space` |
| 100 | 4 of 5 | `o f space` (opposite-hand lists) / **`o Cmd space`** (thumbs in own-hand lists) |
| 50 | 5 of 5 | same as 100 |

Those counts are `tests/chord-repeat-fast`'s five chords, read off the `0xE7` and `0x0D` lines of
its snapshot with the value set to each of the three settings in turn. A lost modifier shows up as
`16 decided tap (balanced decision moment quick-tap)` followed by a bare `0x0D`.

The middle column is why the value was lowered to 100; the right column is why the thumb positions
were taken back out of the own-hand lists at the same time. Leaving both changes in would have made
typing "of" open Spotlight — a worse bug than the one being fixed. **Change one of these two without
re-running `typing-roll` and `chord-repeat-fast` and you will trade one bug for the other.**

The combo guards (`cmben`/`cmbru`) keep their own `require-prior-idle-ms = 150`; they protect against
a different accident and were deliberately left alone.

**Outcome: reduced, not eliminated, and accepted.** At 100 the symptom is still reachable if the
chord is repeated fast enough — it is inherent to the mechanism, since any guard window at all will
swallow a modifier pressed inside it. Tim's call was that the residue does not matter because the
gesture is not repeated at that rate in real use. Do not reopen this expecting a clean fix; the
remaining options all cost typing safety, and the table above is what they cost.

### Same-hand chords are refused by design

Position 33 (Enter) and 34 (Backspace) are on the **right** half, 30-32 (Space, Esc) on the left. So
right-hand Cmd (position 16) with Enter is a same-hand chord and the positional lists turn it into a
bare tap of that position — see `tests/cmd-enter-twice`. Enter and Backspace must be chorded with the **left** modifiers,
Space and Esc with the right ones. This is the opposite-hand rule working, not a defect.

### Still unexplained

The alternating failure turned out to be the `require-prior-idle-ms` window above — the previous
keystroke (the earlier Space, or the Esc dismissing Spotlight) sat inside it — and shrinking the
window reduced it. One detail never matched, though: the simulator emits `j` on a failed chord,
while the real keyboard emits neither `j` nor a space. Whatever accounts for that gap is still
unknown, and it lives below the keymap, since the keymap's own trace is byte-identical across
repeated attempts: in both `tests/cmd-enter-twice` and `tests/cmd-enter-reordered-twice` the second
attempt reproduces the first line for line. The two cases do differ **from each other** — in-order
refuses the hold positionally (`decided tap … other-key-up`, `j` before Enter) while reordered loses
it to the guard window (`decided tap … quick-tap`, `j` after Enter) — which is the split's reordering
showing up in the decision itself.

**When a chord misbehaves, check which half each key is on before touching any timing parameter,
and run it through `./tests/run.sh` before theorizing.** Three successive hypotheses about
`require-prior-idle-ms`, positional lists and tapping terms were all wrong here; the asymmetry
between hands is what identified the cause, and a simulator run would have shown the decision
directly. The simulator also revealed what actually happens to the losing key: it is not dropped
but **captured** by the winner's undecided hold-tap and replayed afterwards — by which time the
other key has been sent and `store_last_tapped` updated, so `require-prior-idle-ms` resolves the
modifier to a tap. The split reorders the chord; `require-prior-idle-ms` then makes the loss
permanent.

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

**The digit row is layout-independent by construction**, not by that fallback: `keys_ru.h` defines
`RU_N0`..`RU_N9` as the very same HID usages as `N0`..`N9`, because ЙЦУКЕН leaves the number row
alone. So `numbers` needs no `&en` wrapper anywhere, and neither do its digit-based shortcuts —
`&kp LA(LC(LG(LS(N2))))` (Term) and `&kp LA(LC(LG(LS(N1))))` (qTerm) send the same thing on either
layout. Of the four shortcuts on that layer only Prev Win, `&kp LC(LG(LS(FSLH)))`, leans on the
Latin fallback: `RU_FSLH` is `LS(BACKSLASH)`, so the bare FSLH scancode yields `.` in Cyrillic and
only the Cmd in the chord rescues it. Prev App is `&kp LG(TAB)` and carries no character at all.

**Glyphs with no Cyrillic equivalent**, i.e. the ones that must go through `&en` on `sym_ru`:
`[ ] ' | { } $ ~ ` ` — `keys_ru.h` has no `RU_LBKT`, `RU_RBKT`, `RU_SQT`, `RU_PIPE`, `RU_LBRC`,
`RU_RBRC`, `RU_DLLR`, `RU_TILDE` or `RU_GRAVE`. Everything else has an `RU_*` form; check the header
before assuming.

`keys_ru.h` itself is a generated Unicode-licensed header — vendored, don't hand-edit. Every keymap
in the repo includes it, even ones with no Cyrillic bindings.

## ru_ext, and why it is not a plain `&lt`

Seven Cyrillic letters do not fit on 26 keys, so `ru_ext` (layer 2) holds them and is reachable
**only from `ru`**, by holding one of two letter keys — positions 23 and 26, which are `V` and `M` in
QWERTY terms and `м` and `ь` on this layer:

```
_  ц  _  _  ё  |  _  _  щ  э  _
ф  _  _  _  _  |  _  _  х  _  _
_  _  _  _  _  |  _  ъ  _  _  _
```

Two entrances, one per half, and the letters divide the same way: holding 23 (left) frees the right
hand for `щ э х ъ`, holding 26 (right) frees the left for `ц ё ф`. That is also why `ъ` may sit on
position 26 — you never reach it from the entrance it lives on.

**These keys must not use this file's `&lt`.** That behavior is re-tuned to `balanced`, which decides
hold as soon as the next key is released; on a letter key that is fatal. Rolling `м` into `о` would
raise the layer and resolve `о` against `ru_ext`'s `&none`, losing **both** letters, and `быть ` would
lose its `ь`. `lt_ext` keeps ZMK's stock `tap-preferred`, where only the `tapping-term-ms` timer
raises the layer, so a roll always taps (`tests/ru-ext-rolls` proves both shapes). There is
deliberately **no** `require-prior-idle-ms` and no positional list: those would suppress the trigger
right after a keystroke, which is precisely when `ъ` and `х` are wanted. `tapping-term-ms` is the
only knob here.

## Combos

Bounded by `config/op36.conf`: `CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO=3`,
`CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY=7`.

A combo's `layers = <N>` is checked against `zmk_keymap_highest_layer_active()` (see
`app/src/combo.c` in the fork), **not** against "is that layer active anywhere in the stack". A
combo scoped to `ru` therefore does not fire while `nav` is held on top of it.

`cmben`/`cmbru` (layout switch) carry `require-prior-idle-ms = <150>` because they sit on positions
2+3. On `ru` that is `У`+`К`, a same-hand roll in ordinary words ("рука", "наука"), which is what the
guard is for. On `en` those positions are `F`+`P` under Colemak-DH, where no such roll exists, so the
guard now earns its keep on the Cyrillic side only — keep it anyway, it costs nothing and `en` is not
finished changing.
Those two are the only combos left. `kha` (Х) and `hrdsgn` (Ъ) were deleted when `ru_ext` arrived —
see below — but the reason they never carried a prior-idle guard still governs that layer: Х and Ъ
are wanted mid-word ("плохо", "объект"), so anything that suppresses a trigger right after a
keystroke suppresses exactly the cases you need.

## ZMK internals worth not re-deriving

All checked against the fork's source or the build's own output:

- **Implicit modifiers are a single global that the next keypress overwrites.** `hid.c` keeps one
  `static zmk_mod_flags_t implicit_modifiers`, and `hid_listener_keycode_pressed` assigns the
  incoming event's set to it unconditionally. Explicit modifiers, registered when a modifier keycode
  is pressed on its own, accumulate instead. So `LS(LC(LA(X)))` is right for a one-shot chord like
  `&kp LA(LC(LG(LS(N2))))` on `numbers`, and wrong for anything meant to be *held* across other
  keys. The released path even carries a comment admitting the tracking is approximate.
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

- **`;` `,` `.` `'` are not on the base layer.** The Colemak-DH rework took their positions; all
  four live on `sym_en`, at positions 24, 26, 27 and 16.
- **Home, End, Insert, Delete, PageUp, PageDown, PrintScreen** are likewise unbound.
- `adj` is stripped to `&bootloader`, the four `&bt BT_SEL`, `&bt BT_CLR`, both `&out` and
  `&studio_unlock`; everything else on it is `&none` by intent.

## Dormant config (not built)

Reference only. Trackball keymaps declare their own `default_transform` inline, so one `trackball`
shield serves three devices with per-variant tuning in `cmake-args`. `velvet_v3_ui*` raises a Mouse
layer on pointer motion via `zmk,input-processor-temp-layer`. `k03` binds six encoders through a
shared `enc_vol` and `imperial44` two through `&inc_dec_kp`; both need `CONFIG_EC11=y` and
`CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y`.
