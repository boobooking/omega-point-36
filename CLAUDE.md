# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A ZMK **user config** (`zmk-config`), forked from Ergohaven's. It is almost all devicetree keymaps,
Kconfig fragments and a build matrix, with one exception: `module/` is a small Zephyr module carried
by this repository, the only compiled code here. No local toolchain is checked in; firmware is
produced by GitHub Actions.

**Only the Omega Point 36 (`op36`) with the RU/EN keymap is built**, and `config/op36_ruen.keymap`
is the only file that receives real work. `config/` still holds the upstream keymaps for velvet_v3,
velvet_v3_ui, k03, imperial44 and the trackballs, plus the non-RU `op36.keymap`, but no `build.yaml`
entry references any of them, so they are never compiled. Treat them as reference material, not as
code that has to keep working.

**There is no ZMK fork in this build.** `config/west.yml` imports `ergohaven/ergohaven-zmk`, whose
own manifest pins **`zmkfirmware/zmk` at tag `v0.3.0`** — stock upstream, confirmed in the CI log
(`HEAD is now at edf5c081 chore(main): release 0.3.0`). So every ZMK source path quoted in this file
is upstream v0.3.0, not somebody's patch.

`ergohaven-zmk` is not a fork either (`fork: false`, no parent). It is a ZMK **module** supplying the
`ergohaven` board, the shields, and a reusable workflow whose one real extension is the `keymap:`
build-matrix key. `scroll-snap.dtsi` and `input/processors/sensor_rotation.dtsi` come from third
parties its manifest pulls in (`kot149/zmk-scroll-snap`, `hsgw/zmk-feature-sensor_rotation`), not
from ergohaven.

A repo named `ergohaven/zmk` does exist and is **not used by anything here**: it carries zero commits
of its own, sits 217 behind upstream, and was last touched in April 2025. Don't reach for it.

Whether to leave: there is little to leave. v0.3.0 is upstream's **latest release tag** — `main` is
189 commits past it with no v0.4 — so staying costs no version debt. What ergohaven supplies is
hardware description for hardware you own: the op36 shield is eight files totalling under 6 KB, and
vendoring it would mean owning those and losing upstream's fixes to them.

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

`./tests/run.sh` first runs two checks that need no simulator — `tests/check-en-letters.py`, that
layer 8 has not drifted from `en`, and the host-compiled tests under `tests/resync-state/`, which
exercise the layout resync module's logic that the simulator cannot reach for want of BLE — and then
builds `config/op36_ruen.keymap` for ZMK's `native_posix_64`
board — the firmware becomes an ordinary Linux binary whose key matrix is a mock
scanner replaying timed events — and diffs the resulting log against a snapshot.
It runs in `zmkfirmware/zmk-build-arm:stable`; the first run clones ZMK and its
west dependencies into `$WS` (default `$TMPDIR/zmk-sim-ws`, ~1.5 GB), later runs
reuse it. **It clones `zmkfirmware/zmk` at tag `v0.3.0`, the same revision the
firmware ships** — it used to clone `ergohaven/zmk@main`, which was 28 commits
*behind* v0.3.0, so the simulator was quietly answering questions about a
different ZMK than the one on the board. If the pin in `ergohaven-zmk`'s
manifest ever moves, move `tests/run.sh` with it and delete `$WS` so the clone
is redone. See `tests/README.md` for the shape of a case.

**Reach for this before theorizing about any hold-tap, layer, combo or macro
timing question.** The log carries not just the HID output but the decision
itself — `ht_decide: 16 decided hold-interrupt (balanced decision moment
other-key-up)` — so questions that were answered here by three successive wrong
guesses are answered by one run. Cases mount `config/` into the container and
`#include` the real keymap, so they test the file that ships.

Three gotchas worth knowing before writing a case:

- **Start with a lead-in press and a pause of 200 ms or more.** At t=0 the
  "last tapped" timestamp is zero, so `require-prior-idle-ms` sees the very first
  key as a quick tap and collapses every hold-tap to its tap. The lead-in has to
  land on a key that actually emits something — the cases use position 1, since
  position 0 is `&none`.
- Positions map straight onto the matrix: position N is `RC(N/10, N%10)`, so
  thumbs 30-35 are simply row 3. No translation layer is needed.
- **Space the events the way a person would, not the way a machine could.** It is
  tempting to write 30 ms between every press, and for hold-taps and rolls that
  is exactly right. But anything gated by a *timeout* — a sticky key's
  `release-after-ms`, a macro's `wait-ms` — is only exercised if the case waits
  as long as a human does. `tests/herdr-prefix` pressed the command key 150 ms
  after the prefix, passed, and shipped a firmware where the feature did not work
  at all: in real use you read herdr's prompt first, and by then the sticky had
  expired. Ask what the user is doing between two events and put that number in.

**It cannot model the split.** One node, no peripheral half, no BLE — so anything
caused by event ordering between the halves is out of reach, and so is host
behavior (layout switching, Spotlight, the Latin fallback for Cmd shortcuts).
`tests/cmd-space-reordered` illustrates that class of bug by hand-ordering the
events; it does not reproduce the hardware faithfully, and the difference is
visible — the simulator emits a `j` there where the real keyboard emits none.

## Flashing and the split

The op36 shield's `Kconfig.defconfig` in `ergohaven-zmk` gives `ZMK_SPLIT_ROLE_CENTRAL` to
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
  workflow extension resolving to `-DKEYMAP_FILE=`) overrides it, which is how `op36_ruen` is built
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

**Layer 8 (`en_letters`) is a copy of the `en` layer** — rows 0-2 verbatim, plus `en`'s thumb row
with the layout switch position replaced by `&none`. Change one and you must change the other in the
same commit;
`tests/check-en-letters.py` fails the run and names the drifting position if you forget. See
"Modifiers on `ru` raise the English letters over it" for why the copy exists.

When editing programmatically, **write a renderer and first prove it reproduces the file byte for
byte with no substitutions applied**, then apply changes. That check caught the thumb-row rule on the
first attempt; without it the diff would have been full of spurious realignment.

## Layers

Ten layers, and the index order matters: `en`=0, `ru`=1, `ru_ext`=2, `sym_en`=3, `sym_ru`=4,
`nav`=5, `numbers_en`=6, `numbers_ru`=7, `adj`=8, `en_letters`=9. The other (unbuilt) `_ruen` keymaps
use a different order, so never copy a `&mo N` across files.

**No binding in this file spells a layer number, though.** `&mo`, `&lt` and `&to` take a plain
devicetree cell and ZMK has no phandle to a layer — a layer *is* its position in the keymap node — so
an index is unavoidable where the behavior is bound. The `#define L_*` block at the top names every
one of them, and the bindings read `&ltt L_NAV BACKSPACE`. Reordering the layers is therefore one
edit in that block rather than a hunt for literals, which is what it is there for; keep the block in
the same order as the nodes below it so the two cannot disagree silently.

The one layer index the block cannot reach is `CONFIG_ZMK_LAYOUT_RESYNC_ALT_LAYER` in
`module/Kconfig`: Kconfig does not see these defines. It defaults to 1, which is `L_RU`, and has to
be moved by hand if `ru` ever moves.

`en_letters` is a copy of `en` raised over `ru` whenever a modifier or the herdr prefix needs Latin
scancodes — see below, including why it must be kept in sync by hand and what checks that.

`numbers_en` and `numbers_ru` are the second such pair, and they sit next to each other on purpose —
see "Two numbers layers" below for what differs between them and why one layer cannot serve both
languages.

```
en ──hold pos 35──> adj                 (tap gives Tab)
 ├──Space────────> numbers_en           (numbers_ru from ru)
 ├──Bspc─────────> nav
 └──Esc/Enter────> sym_en
ru behaves identically, except Esc/Enter reach sym_ru
ru ──hold pos 23 or 26──> ru_ext        (the seven letters that did not fit)
en <──tap pos 32──> ru                  (layer_ru / layer_en, and the host follows)
nav has &to 0 / &to 1 for language resync
```

Positions are 0-9 / 10-19 / 20-29 for the three rows, then 30-32 (left thumbs) and 33-35 (right).

`en` carries Colemak-DH with `Q`, `Z` and `J` pulled off the pinky columns into the grid. `ru` is
ЙЦУКЕН squeezed onto the same 26 keys: `ц`, `щ`, `ф` and `э` are dropped and `я` moves from the
bottom-left corner up to position 10. The layers are independent — each sends the scancodes its own
host layout expects — so the asymmetry costs nothing.

**Positions 0, 9, 20 and 29 are `&none` on every layer.** Both letter layers gave them up, and the
four higher layers never used them.

**Letters move, positions do not**: the mods and the positional hold-trigger lists are all addressed
by position, so a layout change touches none of them. The `ru` rework is the proof — the layout
switch survived it untouched, and every move it has made since (top row, then a thumb pair, then a
single thumb key) was for reasons that had nothing to do with letters.

**Home row mods** sit on `en`, `ru` and — as plain `&kp` — the right hand of `nav` and the left hand
of `numbers`: Alt on A/O, Shift on R/I, Ctrl on S/E, **Cmd on T/N**. `hml`/`hmr` carry
`hold-trigger-key-positions` listing the whole opposite hand **plus that hand's three thumbs** —
`hml*` and `hyl*` list `5..29` and `33 34 35`, `hmr*` and `hyr*` list `0..24` and `30 31 32` — so a
mod engages when the next key is on the other hand or on one of the other half's thumbs. The own
half's thumbs are deliberately absent; see the `require-prior-idle-ms` table below for what putting
them back costs. On `ru`, position 17 is the exception: it uses `hmr_pfx`, same lists, different
hold binding, for herdr's prefix. Those lists are position-based —
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

Position 2 used to share the layout-switch combo, which has since moved to the thumbs; the key is
purely Hyper now. `tests/hyper` still presses 2+3 to record that they simply type `f` and `p`.

### Modifiers on `ru` raise the English letters over it

`ru` is positional ЙЦУКЕН, so the scancode under a physical key differs from Colemak-DH: position 4
sends `B` on `en` and `T` on `ru`. Shortcuts are matched on the scancode — through the Latin fallback
for Cmd, and straight through the virtual keycode for anything a hotkey daemon registers — so
`Cmd+B` fired from the same finger worked before the Colemak-DH rework and stopped afterwards.

So on `ru`, **holding Cmd, Ctrl, Alt or Hyper raises `en_letters` (`&mo 8`)** for as long as the
modifier is down, which puts the letters back where `en` has them. `hml_ru`/`hmr_ru` and
`hyl_ru`/`hyr_ru` are the `hml`/`hmr` and `hyl`/`hyr` pairs with the hold binding swapped for the
`en_mod` / `hyper_en` macros; flavour, guards and trigger lists are untouched. Verified end to end in
`tests/ru-mod-switch`.

**`en_letters` is a hand-kept copy of `en`** — rows 0-2 verbatim, plus `en`'s thumb row with position
32 replaced by **`&none`**. Under a held modifier neither of that key's two jobs can be right: a copy
of `en`'s binding would tap "go to ru" while already on `ru`, and a `&trans` would fall through to
`ru` and reach `sym_ru`, whose `\` goes through the `&en` wrapper and switches the layout mid-chord.
Refusing the press is the only safe answer, and it costs `Cmd` plus the left thumb reaching a symbol
layer at all. Enter on position 33 still does, and still reaches `sym_en` with the Latin symbols a
shortcut needs. It has to hold real bindings: `&trans` on a layer sitting *over* `ru` falls
straight back through to `ru`, which is the thing being overridden. Position 30 is the exception
precisely because falling through is what is wanted there — see below.

**Editing `en` means editing `en_letters` in the same commit.** Nothing in the devicetree enforces
the copy, so `tests/check-en-letters.py` does; `tests/run.sh` runs it before building any case and
names the position that drifted. Trust that check rather than remembering.

**Shift is deliberately excluded**, on positions 11 and 18, and must stay that way. It is what types
capital Cyrillic: switching would make Shift plus position 1 emit `W` instead of `Й` and break
Russian input outright. Nothing is lost, because `Cmd+Shift+…` still resolves on `en` — Cmd does the
raising and Shift only contributes a modifier bit.

**This used to switch the base layer instead, and that is why an empty marker layer existed.** The
old `en_mod` ran `&to 0` on press and `&to 1` on release, which made `en` the base for the duration —
so the keymap's own state claimed the language was English while the user was still on `ru`. Position
30 is direction-specific (`&layer_en` on `ru`, `&layer_ru` on `en`), so it read that lying state and
ran "go to ru" *from* ru: a host switch with no firmware movement, and a desynchronised host. The fix
then was a layer of 35 `&trans` and one `&none` at position 30, raised alongside `&to 0`, whose only
job was to refuse the press. Earlier still, when the switch was a combo, the same marker layer served
a different purpose — `combo.c:164` tests `combo_active_on_layer(combo, zmk_keymap_highest_layer_active())`,
one layer and not the whole stack, so `&to 0` flipped which combo was armed and parking the stack on
layer 8 disarmed both.

Raising a layer instead of switching the base removes the cause rather than the symptom. The base
never stops being `ru`, so the switch key always reads the true language: pressed mid-chord it now
**does** the switch, correctly, and the firmware lands on `en` with the host agreeing — where the
marker layer could only refuse. `tests/switch-under-mod` pins that, and the whole change moved no
HID output at all, only which layer resolved each key.

Two things worth not re-deriving, both in `app/src/keymap.c`:

- `set_layer_state` refuses to deactivate the default layer ("Default layer should *always* remain
  active"), so `&to 1` leaves both 0 and 1 active with 1 on top — which is why `&trans` on `ru` falls
  through to `en` at all.
- **`&to` does not touch flash.** Every `settings_save_one` in that file belongs to a Studio keymap
  edit, not to layer activation. It no longer runs on every modifier press, but the explicit layout
  switch and the `nav` resync keys still use it.

`&mo` unwinds with its own key, so a modifier hold can no longer strand the firmware on `en` the way
`&to 0` without its `&to 1` could. A lost release still leaves the layer up, but the symptom is the
same either way and `nav` positions 6 and 7 remain the resync.

The symbol layers carry only two mods, both on the left: Shift on `_` and Cmd on `$`.

### The right Ctrl keeps `en` for one key after the chord, for herdr's prefix

herdr takes a prefix — **Ctrl+B** — and then a command key: prefix then `W` opens Spaces, prefix
then `Shift+R` renames one. `prefix` in `~/.config/herdr/config.toml` is the only place it
lives, and Ctrl+B is herdr's own default. It sat on Ctrl+Space until the language switch briefly
claimed that chord; the switch has since moved on, so Ctrl+Space is free again if the prefix is ever
wanted back there. On
`ru` the prefix itself arrives fine, because the right Ctrl raises `en_letters` for its own duration,
but the **command key** used to arrive as Cyrillic: the modifier hold dropped `en_letters` the
instant Ctrl was released, which is before the command key is pressed. Position 1 sent `Q` (`й`)
instead of `W`, and herdr matched nothing.

So position 17, the right Ctrl, uses **`hmr_pfx`** — `hmr_ru` with the hold binding swapped for
`en_mod_prefix`, which is `en_mod` except that its release **arms `sken`** instead of dropping the
layer. `sken` holds `en_letters` across exactly one key press, then it falls on its own.
`tests/herdr-prefix` verifies that much, and only that much: the command key leaves the firmware as
`0x1A` (`W`) rather than `0x14` (`Q`).

**That is half of the fix, and on its own it changes nothing the user can see.** A keymap layer picks
the HID usage; the **host** turns that usage into a character, using whichever input source is
active. With "Русская" active, `0x1A` decodes to `ц` — checked with `UCKeyTranslate` over
`com.apple.keylayout.Russian`, which likewise gives `0x15` → `к` and `0x16` → `ы`. herdr is a
terminal application reading crossterm key events, so the character is what it matches on. The layer
therefore moved the failure from `й` to `ц`, equally unmatched, which is why the prefix went on
looking broken across two firmware fixes that were each correct in themselves.

**The host half is herdr's own `experimental.switch_ascii_input_source_in_prefix`**, off by default
and now `true` in `~/.config/herdr/config.toml`. It puts the host on an ASCII-capable layout while
prefix mode is open and restores the previous input source when it closes; its comment names the CJK
IME, which is the same problem arriving from the other direction. Being herdr's own switch it is
closed-loop — it saves what it replaces — so it cannot desynchronise the language layer the way a
firmware-side switch tap would. Confirmed working on the device with both halves in place.

**Both halves are required and neither works alone.** The host half alone leaves the firmware sending
ЙЦУКЕН-positional scancodes into an ASCII layout: position 11 sends `ы` (`0x16`), which is `s` under
ABC, so `prefix+r` would reach the wrong command. The firmware half alone sends `0x15` into
"Русская" and gets `к`. Together the scancode is `0x15` and the layout is ABC, so herdr reads `r`.

**Do not try to close this from the keymap alone.** The firmware cannot know when prefix mode ends —
`Esc`, the prefix again, or any unmatched key all leave it — so there is no moment at which it could
put the layout back, and every shape of that idea desynchronises the host on an abandoned prefix.

**Releasing the modifier is the only moment this can be done, and that is forced.** The window
between "Ctrl+Space has been sent" and "the command key is pressed" contains exactly one firmware
event: the Ctrl release.

**`sken` carries `release-after-ms = <600000>`, and the stock 1000 is far too short.** herdr puts a
prompt on screen and you read it before choosing, so the gap between the prefix and the command key
is seconds, not milliseconds. At 1000 the sticky expired first and the command key arrived as
Cyrillic — the prefix appeared to work, herdr showed it was waiting, and nothing matched.

**herdr has no prefix timeout at all**, so neither should this. `ClientShellMode::Prefix` in
`src/client/shell/input.rs` (`herdrdev/herdr`) is a key-driven state machine with no `Instant` and no
`Duration`: the prefix key again, `Esc`, a matching binding, or **any other key** all just leave the
mode. That last branch is why an expired sticky looked the way it did — the Cyrillic scancode was not
recognised but still dismissed the prompt. Ten minutes is not a considered interval, it is "long
enough that the firmware never decides before you do". This is
the failure that reached the device, and `tests/herdr-prefix` missed it because the case pressed the
command key 150 ms after the prefix. It now waits 1500 ms, which is what a person does. A long window
costs nothing in normal use: any key press consumes the sticky, so the only exposure is arming the
prefix and then abandoning it, which costs one Latin character.

Two things were checked before settling on this and are not worth re-deriving:

- **`sken` is the stock `&sl` in all but one property.** That only became true once modifiers stopped
  switching the base layer. While `en` was reached by turning `ru` *off*, no sticky layer could
  express it — `keymap.c` pins the default layer at 0 (`_zmk_keymap_layer_default`, line 27), keeps it
  always active (line 177), refuses to deactivate it (line 145) and makes it the floor of the lookup
  (lines 186, 715), so layer 0 can never be raised above `ru`. The sticky then had to wrap a macro
  that switched `ru` off and back on, which `behavior_sticky_key.c` allows because it invokes an
  arbitrary bound behavior as press/release (lines 104-135). With `en_letters` sitting above `ru`
  there is a real layer to raise, and the wrapped behavior is plain `&mo`.
- **`ignore-modifiers` is required**, and the stock `&sl` does not have it (only `&sk` does). Without
  it the Shift of `Shift+R` consumes the sticky before `R` arrives.
- **`lazy` does not fix the ordering.** It defers the wrapped behavior to the next key, which sounds
  like exactly what is needed, but a lazy sticky presses around the **keycode** event, while the
  layer for the next key is chosen earlier, at the **position** event. That is also why the stock
  `&sl` is not lazy.

A combo on the prefix chord was rejected: combos need both keys inside `timeout-ms` (default 50),
and the prefix is typed by holding Ctrl first and pressing the other key afterwards, so it would
never fire.

**`en_mod_prefix` cannot tell which key was pressed during the hold**, so it arms after any chord
made with the right Ctrl, not only the prefix. That costs nothing here because the right Ctrl is
used for the prefix and nothing else — Tim confirmed it. Do not copy `hmr_pfx` onto a modifier that
has other uses without re-reading this: the next key after every chord would resolve on `en`.

**Shift must not follow the prefix within 100 ms.** `is_quick_tap()` compares against `last_tapped`,
which `keycode_state_changed_listener` updates on any non-modifier **press** — including the prefix's
own second key. Measured with a cut-down case: at a 60 ms gap the log shows
`ht_decide: 18 decided tap (balanced decision moment quick-tap)`, so Shift types `i` (0x0C), that
`i` discharges the sticky, and the command key then arrives as Cyrillic (0x16, `ы`, instead of 0x15,
`R`). Releasing Space and then Ctrl takes a human longer than 100 ms, so this does not bite in
practice, but it is the failure shape to recognise. It is the ordinary home-row guard, not something
this feature introduced; `require-prior-idle-ms` is the knob and raising it makes this worse, not
better.

## The thumb row

Five keys, and which one you hold picks the layer. Each leaves one hand free:

| held | leads to | free hand |
|---|---|---|
| 35 Tab | `adj` | left, and `adj` lives on the right hand |
| 31 Space | `numbers_en` from `en`, `numbers_ru` from `ru` | right, where the digits are |
| 34 Backspace | `nav` | left, where the arrows are |
| 32 / 33 Enter | `sym_en` from `en`, `sym_ru` from `ru` | the other one |
| **tap 32** | the layout switch, not a layer | — |

Position 30 is a plain `&kp ESC` and reaches no layer at all. Every other thumb carries something;
none is spare.

**Position 32 does two jobs**, hold for the symbol layer and tap for the layout switch, through
`ltru` on `en` and `lten` on `ru`. `&ltt` cannot express it: its tap binding is `&kp` and the switch
is a macro, so these are the same hold-tap with the tap swapped, one per direction. The second
parameter is written `0` and ignored, because the macro takes none and the schema demands two cells —
the same shape as `&hyl 0 F`.

They carry **no `quick-tap-ms`**, deliberately. That property exists so tap-then-hold repeats the
tapped key, and repeating a language switch is never wanted — it would switch twice instead of
raising the layer. Balanced is also the safer flavour here rather than merely the consistent one: a
roll resolves as a hold, so it yields the symbol layer, where tap-preferred would yield an accidental
language switch.

Read any "position 30" below that talks about the layout switch as history: the switch has lived on
2+3, then 31+34, then 30, and now 32.

**Position 35 is `&lt 7 TAB`, deliberately the stock tap-preferred `&lt` and not `&ltt`.** Every other
thumb is balanced, but `adj` carries `&bootloader` and `&bt BT_CLR`, and Tab is frequent: with
balanced, a fast Tab-then-letter roll would raise `adj` and drop that letter onto one of them.
Tap-preferred waits for the tapping term, so a roll always taps. `tests/adj-key` pins both halves —
held 250 ms it reaches layer 7, tapped it emits `0x2B` and layer 7 never comes up. Tab left `nav`
(position 14, now `&none`) when it arrived here.

`adj` is a plain `&lt L_ADJ TAB` on position 35 — no chain, no combo. It was on the Space+Backspace hold
until a tap on those two became the layout switch, which made the gesture ambiguous by construction:
a ZMK combo fires on simultaneous press and cannot tell a tap from the start of a hold. It then sat
on position 30 until that position became the layout switch itself, and moved to 35, the last free
thumb. `adj` is rare, so it gets the least reachable key. `tests/adj-key` pins both halves: position
35 raises `adj`, and holding Space then Backspace yields the numbers layer over `nav` and never
`adj`.

**The rule: base layers (`en`, `ru`) hold the real bindings; every higher layer is `&trans`.** It now
holds with **no exceptions at all** — every one of the six thumb positions is `&trans` on all seven
layers above `ru`. The two `&lt 7 …` exceptions that used to sit on `nav` and the numbers layer went
away with the old `adj` route.

This rule is load-bearing, not cosmetic. A thumb position is not guaranteed to be the held key, and
an `&none` left over from when it was **silently kills that key**. Two such dead keys already shipped
this way. After any thumb-row change, resolve every position against every reachable layer stack —
`en`/`ru` alone, each with `adj`, its numbers layer, `nav` or its symbol layer — accounting for which
key is held in each case.

One stack is reachable and worth knowing about: on `sym_en`/`sym_ru` position 31 is `&trans` and
falls through to the base Space, so holding Esc and then Space raises the numbers layer on top of the
symbol layer — 6 over 3 from `en`, 7 over 4 from `ru`. Harmless, and `&none` is not an alternative
there — it would kill the space tap.

## Timing and hold-tap behaviors

`&lt` gets one narrow override at the top of the file; every other hold-tap is a named behavior in
the behaviors block.

**The deviation is named, not the default.** ZMK ships `&lt` as `flavor = "tap-preferred"`,
`tapping-term-ms = 200`, no `quick-tap-ms`, and that is exactly right for a layer hanging off a
letter key: tap-preferred reaches the layer only when the term expires, so a roll always taps. The
`ru_ext` entrances use it as shipped. The thumbs want the opposite — with tap-preferred a fast
opposite-hand press would beat the layer to it — so they use **`&ltt`**, which is the same behavior
with `flavor = "balanced"`.

The one property `&lt` does have overridden is `quick-tap-ms = <200>`, so tap-then-hold repeats the
key instead of raising a layer. `&ltt` carries it too.

This file used to re-tune `&lt` itself to `balanced` and give `ru_ext` the named exception, which is
backwards: `&lt N X` then silently meant something other than what the ZMK docs say, and that cost a
detour. `&mt` was re-tuned in the same block and **never used by any binding** — it is gone.

Space and Enter running through a hold-tap is the main ergonomic risk in this keymap: with
`balanced`, pressing and releasing the next key before releasing the thumb hands the layer the win.
`tapping-term-ms` is the knob if that shows up in real typing.

`hml_en` was `hml` with `bindings = <&kp>, <&en>`, carrying `$` through the language-switching macro
on tap because `$` was thought unreachable with a plain `&kp` on the Cyrillic layout. Option+4 gives
it directly, so position 13 is an ordinary `&hml LGUI RU_DLLR` and the behavior is **gone**, having
had exactly one user. Forward references from `behaviors` into `macros` are still fine, though —
devicetree resolves labels after parsing the whole tree, which `hml_ru` and the `hyper_en` pair still
rely on.

## Positional hold-tap: how it actually decides

All from `app/src/behaviors/behavior_hold_tap.c` in upstream ZMK v0.3.0. This was the source of a real bug —
same-hand `Cmd+Space` was impossible by construction, and the workaround people find ("hold the
modifier, wait, then press") hides it, so the symptom reads as "the timing is too slow".

- **`decide_positional_hold()` only consults the list if another key was pressed before the
  decision.** It returns early when `position_of_first_other_key_pressed == -1`. A hold resolved by
  the timer alone therefore **bypasses the positional check entirely** — which is exactly why
  holding and waiting appears to work while the fast chord does not.
- **A position not in the list forces a tap**, it does not merely decline the hold. So a modifier
  chorded with a key outside its list can never work, at any speed.
- **Putting the thumbs in both lists was tried and reversed.** The argument for it is real: the
  list's purpose is to stop same-hand letter rolls from raising a modifier, thumbs never take part in
  letter rolls, and leaving them out makes every same-hand modifier+thumb chord (`Cmd+Space`,
  `Cmd+Backspace`, `Shift+Space`) impossible. It was still reversed, because with the thumbs in the
  own-hand lists typing "of" opened Spotlight — see the `require-prior-idle-ms` table below. Each mod
  therefore lists only the opposite half's three thumbs, and same-hand modifier+thumb chords stay
  impossible on purpose.
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

The layout switch used to carry its own `require-prior-idle-ms = 150` for a different accident — a
fast space-then-backspace correction firing the combo mid-word. That guard went with the combo: a
dedicated key cannot be hit by a correction, so there is nothing left to suppress.

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
**The switch sends two keys, `GLOBE` and `Hyper+F13`**, because no single one serves both platforms.
Each host acts on the one it understands and ignores the other, so neither sees a double switch.

- **iPadOS honours `GLOBE`** (consumer `0x29D`) and offers no alternative: its input-source shortcuts
  are fixed, so an obscure chord is impossible there.
- **macOS ignores `GLOBE`** but binds any chord, under Settings → Keyboard → Keyboard Shortcuts →
  Input Sources. `Hyper+F13` is bound there.

That macOS ignores `GLOBE` is **not** a descriptor problem. The report descriptor read back off the
connected keyboard — `ioreg -c IOHIDDevice -r -l`, then decode the `ReportDescriptor` bytes —
advertises the consumer page with Usage Max `0xFFF` over a 16-bit field, so `0x29D` fits and macOS
parses the collection. Apple's "Press 🌐 key to" setting, `AppleFnUsageType = 1` in
`com.apple.HIToolbox`, does not answer to that consumer usage from a third-party keyboard; it appears
wired to Apple's own Fn. Do not spend another firmware on `GLOBE` for macOS without new evidence.

**`Hyper+F13` is chosen for being unpressable.** This keyboard has no F13, so the chord cannot be
struck by accident, and nothing on either platform claims it. That matters because the switch is
open-loop: one stray press desynchronises the firmware from the host until you notice it. Ctrl+Space
was tried and rejected for exactly that — it works on both platforms and is far too easy to hit.

**To bind it on macOS you cannot type it.** Open the shortcut recorder and press the keyboard's own
switch key: `os_lang` sends the chord and the recorder captures it. The keyboard configures the host
for itself.

Neither key carries a hold threshold, so `os_lang` needs no `tap-ms`.

**The host's Russian layout is the Mac-traditional "Русская", and the keymap is targeted at it.**
`keys_ru.h` is generated for the Windows ЙЦУКЕН, which macOS calls "Русская — ПК"
(`com.apple.keylayout.RussianWin`); the host actually runs plain "Русская"
(`com.apple.keylayout.Russian`). The two place every *letter* identically and disagree only on
punctuation, which is why `ru` typed correctly for a long time while `sym_ru` was wrong throughout —
the reported symptom was `,` coming out as `?` and `.` as `/`. `op36_ruen.keymap` therefore
redefines the affected `RU_*` macros immediately after the includes, and the *base* macro is the one
redefined so the aliases follow it (`RU_DOT` is `(RU_PERIOD)`, `RU_FSLH` is `(RU_SLASH)`, and so on).

| binding | glyph | PC ЙЦУКЕН (keys_ru.h) | "Русская" (in use) |
|---|---|---|---|
| `RU_PERCENT` | `%` | `LS(N5)` | `LS(N4)` |
| `RU_COLON` | `:` | `LS(N6)` | `LS(N5)` |
| `RU_COMMA` | `,` | `LS(SLASH)` | `LS(N6)` |
| `RU_PERIOD` | `.` | `SLASH` | `LS(N7)` |
| `RU_SEMICOLON` | `;` | `LS(N4)` | `LS(N8)` |
| `RU_SLASH` | `/` | `LS(BSLH)` | `FSLH` |
| `RU_QUESTION` | `?` | `LS(N7)` | `LS(FSLH)` |
| `RU_CYRILLIC_IO` | `ё` | `GRAVE` | `BSLH` |

`RU_LPAR`, `RU_RPAR`, `RU_UNDER`, `RU_DQT`, `RU_MINUS`, `RU_EQUAL` and `RU_PLUS` land the same on
both, and so does the whole digit row, so `numbers` is untouched. **`ё` is the only letter that
moves** — every other Cyrillic key, including the ones on punctuation positions (`х ъ э ж б ю`), is
identical between the variants. That is exactly why the bug read as "`sym_ru` broke" and hid for so
long.

**"Русская" has no key for `\` — and that is the only one.** An earlier sweep concluded the same of
`*`, but it had covered only the plain and shifted states; `*` is Option+8, and `RU_ASTERISK` is
redefined to `LA(N8)` accordingly. `RU_BACKSLASH` stays `#undef`'d, so a future `&kp RU_BSLH` fails
the devicetree build instead of quietly emitting the wrong glyph — the key that would carry `\`
carries `ё` here, which is why `RU_CYRILLIC_IO` is `BSLH`. **Sweep the Option and Option+Shift states
too before concluding a glyph is absent**; that omission hid ten reachable glyphs behind the `&en`
wrapper for as long as the wrapper existed.

If the host is ever switched to "Русская — ПК", both override blocks come out — the punctuation
corrections and the ten Option-layer definitions alike, since the PC layout puts those glyphs
elsewhere again — and `\` goes back to a plain `&kp`.

Verified by translating each binding through the layout's own data with `UCKeyTranslate` over
`TISCreateInputSourceList`, for both `com.apple.keylayout.Russian` and `…RussianWin`, rather than
off a published chart, and confirmed on real firmware afterwards: the built devicetree in run
34121271222 carries `,` as `&kp 0x2070023` (`LS(N6)`), `.` as `0x2070024` (`LS(N7)`) and `ё` as
`0x70031` (`BSLH`). `defaults read com.apple.HIToolbox AppleEnabledInputSources` says which
sources are actually enabled — it also confirms the input-source cycle has exactly two keyboard
layouts, which is what the `en` macro's balance depends on.

**The `&en` wrapper is asynchronous, and at typing speed it reorders.** With `wait-ms = <50>`, a key
pressed within about 50 ms of an `&en` key gets its own scancode emitted *before* the macro's, and
worse, it is typed while the host is still flipped to ABC. `tests/ru-symbols` showed this directly back when `*`
went through the wrapper: rolled at 30 ms, `*` landed after the following `/`, and the `:` after
that came out as `%`. Only `\` is left on the wrapper, and the case still spaces it 200 ms from its
neighbours; the hazard is real but needs a roll no one performs on a symbol layer. Raising `wait-ms`
widens the window rather than closing it.

The critical constraint: the switch is **relative**, not a selector — both keys select the next or
previous input source, which with exactly two enabled layouts is a toggle. There is no "set the host to EN"
on these platforms — Windows' `Ctrl+Shift+1`/`Ctrl+Shift+2`, which this keymap used to send, does
nothing on iPadOS. Everything below follows from that.

- `os_lang` sends `&kp GLOBE` and `&kp LC(LA(LG(LS(F13))))`, and nothing else. `to_en`/`to_ru` no longer exist, because under a
  toggle they would be the same action.
- `layer_en` / `layer_ru` are `&to 0 &os_lang` / `&to 1 &os_lang`, correct **only when invoked from
  the layer they are leaving**. They are bound directly at position 32 — `&layer_en` on `ru`,
  `&layer_ru` on `en` — so the ordinary top-down layer lookup picks the right direction. Never bind
  these to anything unconditional, and never let a layer make the base stop reflecting the user's
  language while the switch key can still be pressed: reading a base that lies is what made "go to RU"
  fire from RU and desynchronize the host. `en_letters` sits *over* `ru` rather than replacing it
  precisely so the base never lies.
- **`nav` positions 6 and 7 are `&to 0` / `&to 1`** — firmware-only resync, deliberately sending no
  keystroke. If host and firmware disagree, press the one matching the host's actual layout.
- The `en` one-param macro types a single key in EN and returns, for glyphs absent from Cyrillic. It
  survives toggle semantics because it is balanced — flip, type, flip back — which holds as long as
  the host's language cycle has exactly two stops (verified: the emoji keyboard stays out of it).
- **`os_lang` carries no `tap-ms`, and must not grow one.** ZMK's default is 30, which is right
  because a plain chord has no hold threshold. Caps Lock does: macOS applies `CapsLockDelay`, 75 ms on this
  host, visible in the internal keyboard's HID service properties
  (`ioreg -r -c IOHIDEventService -l`). A tap under it is dropped without a word, so the firmware's
  layer moved while the host stayed put — and that failure was frequent enough in daily use to be
  the reason the switch key changed at all. Raising the tap to 100 narrowed the window and never
  closed it; a threshold you have to out-wait is not a thing to tune, it is a thing to leave.
- **A dropped switch inside `&en` desynchronises the host.** The macro is balanced — flip, type,
  flip back — so if the first flip is lost and the second lands, the glyph comes out Cyrillic *and*
  the host ends up on the other layout while the firmware never moved. The balance that makes `&en`
  safe under toggle semantics is exactly what makes a single lost tap permanent.
- `wait-ms = <50>` on the `en` macro is a guess at how long the host needs, not a measured value.
  One key on `sym_ru` routes through it now — `\` at position 28 — so a wrong glyph there means
  raising it. Raising it also widens the reordering window described above. It used to be eleven,
  which is what made this value worth worrying about.

**Cmd shortcuts do not need the `en` wrapper.** Verified on device: `Cmd+Shift+[` works with the
Russian layout active, because macOS and iPadOS fall back to the Latin equivalent when matching
command shortcuts. Bind them directly as `&kp LG(LS(LBKT))`. Splitting `nav` per language would have
been pointless anyway — the firmware sends a scancode, and `[` has no Cyrillic scancode to send
instead.

**The digit row is layout-independent unshifted**, not by that fallback: `keys_ru.h` defines
`RU_N0`..`RU_N9` as the very same HID usages as `N0`..`N9`, because ЙЦУКЕН leaves the number row
alone. So no digit needs an `&en` wrapper, and neither do the digit-based shortcuts —
`&kp LA(LC(LG(LS(N2))))` (Term) and `&kp LA(LC(LG(LS(N1))))` (qTerm) send the same thing on either
layout. Prev Win, `&kp LC(LG(LS(FSLH)))`, does not lean on the Latin fallback either: "Русская"
keeps `/` on the slash key exactly where ABC has it, which is also why `RU_SLASH` is redefined to a
bare `FSLH` above. (Under the PC ЙЦУКЕН it would have: there `RU_FSLH` is `LS(BACKSLASH)` and the
bare FSLH scancode yields `.`, leaving only the Cmd in the chord to rescue it.) Prev App is
`&kp LG(TAB)` and carries no character at all.

### Two numbers layers

**Shifted, the digit row is not layout-independent at all**, and that is the whole reason
`numbers_ru` exists. `Shift+2` is `@` under ABC and `"` under "Русская"; `Shift+4` is `$` and `%`;
`Shift+5` is `%` and `:`. One layer cannot serve both, because the firmware sends a usage and the
host picks the glyph — the same fact that governs the herdr prefix above.

The English row is the standard here, and "Русская" turns out to carry all of it, one row over:

| | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 0 |
|---|---|---|---|---|---|---|---|---|---|---|
| Shift | `!` | `"` | `№` | `%` | `:` | `,` | `.` | `;` | `(` | `)` |
| Option | `!` | `@` | `#` | `$` | `%` | `^` | `&` | `*` | `{` | `}` |

So `numbers_ru` needs **no host switching at all**: digits 2 through 8 carry a mod-morph
(`&d2`..`&d8`) whose Shift form is `&kp LA(N<d>)`, and Option is where the symbol already lives.
Digits **1, 9 and 0 stay plain `&kp`** — `Shift+1/9/0` already gives `! ( )` on both layouts, so
morphing them would only add a way to be wrong. Read out of the layout's own data with
`UCKeyTranslate`, dead-key processing left on; none of the seven is a dead key.

`keep-mods` is deliberately absent from the morphs. Without it ZMK sets `masked_mods = mods`
(`behavior_mod_morph.c`), so the Shift that selected the morph is masked while it is pressed and the
host sees a clean `Option+digit` rather than `Shift+Option+digit`. `tests/numbers-ru-symbols` pins
the whole shape: unshifted the key is a bare `0x1F`, shifted it is `0x1F` with `implicit_mods 0x04`,
and positions 26 and 8 stay bare under the same Shift.

**`numbers_ru` is a hand-kept copy of `numbers_en`** in everything but those seven keys — the
left-hand modifiers, the four shortcuts and the `&none` all have to match, or the layer quietly
behaves differently depending on the language you came from, which is the bug the pair removes.
`tests/check-numbers-ru.py` enforces that and names the drifting position; it and
`tests/check-en-letters.py` share `tests/keymap_layers.py`, which finds layers by node name and so
is untouched by renumbering.

Only `ru` position 31 selects it (`&ltt L_NUM_RU SPACE`); `en` and `en_letters` keep `L_NUM_EN`.
`en_letters` is right to stay on the English one: it is only ever raised under a held modifier, where
macOS takes the Latin fallback anyway.

**One glyph goes through `&en` on `sym_ru`: `\`.** The other ten that used to — `[ ] ' | { } $ ~ ` *`
— are reached directly now, because "Русская" carries them under Option even though `keys_ru.h` has
no macro for nine of them. The keymap defines those nine itself, next to the `RU_*` overrides:

| | | | |
|---|---|---|---|
| `{` `LA(N9)` | `}` `LA(N0)` | `*` `LA(N8)` | `$` `LA(N4)` |
| `[` `LS(GRAVE)` | `]` `GRAVE` | `~` `LA(M)` | `` ` `` `LA(LS(N0))` |
| `'` `LA(LS(N9))` | `\|` `LA(LS(N1))` | | |

Each was read out of the layout's own data with `UCKeyTranslate`, dead-key processing left on — none
of the ten is a dead key — and the identity of each physical key was confirmed against ABC rather
than assumed, which is how `~` on Option+M and the bracket pair on the `` ` `` key were found.
`tests/ru-symbols` pins all ten: Option shows up as `implicit_mods 0x04`, Option+Shift as `0x06`, and
`]` carries none at all. **The header is not the authority on what a layout can reach** — it is
generated for the PC ЙЦУКЕН. Check the layout.

`keys_ru.h` itself is a generated Unicode-licensed header — vendored, don't hand-edit. Every keymap
in the repo includes it, even ones with no Cyrillic bindings.

### Every way the layer and the host can drift apart

Worked out in full after intermittent desync was reported on the old combo switch. **The system is
open-loop**: the firmware issues a *relative* command (select the next or previous source) and never
learns whether it landed. Two consequences follow and neither is fixable from the keymap — any lost
switch is a permanent desync, and any host-side change is undetectable.

| # | Cause | Status |
|---|---|---|
| 1 | Firmware switched, host never saw the switch — dropped HID report, or a tap under the host's hold threshold | **removed**: a plain chord has no hold threshold, unlike Caps Lock |
| 2 | Host switched by itself — menu bar, another shortcut, a secure-input field | **irreducible** |
| 3 | Switch key pressed while a modifier is held on `ru`, running the wrong direction | fixed: modifiers raise `en_letters` instead of switching the base, so the direction is never wrong |
| 4 | Unintended switch — a fast space-then-backspace firing the old combo | fixed: the switch is a dedicated key |
| 5 | Switch missed entirely — the old combo spanned both halves and needed both events inside `timeout-ms` = 50 while the peripheral's crossed BLE | fixed: the switch key is on the central half |
| 6 | A modifier hold's release never runs, so `&to 1` never restores `ru` | reduced only; recovery is one keystroke |
| 7 | Boot — layer 0 is the hardcoded default and layer state is not persisted, so every reflash and every battery pull starts on `en` regardless of the host | **irreducible**; the module's memory is RAM-only and shares this fate after a deep sleep |
| 8 | A third keyboard layout enabled on the host — the switch then cycles through three, and the binary model breaks silently | avoid; currently ABC + Russian only |
| 9 | Typing inside the window between `&to` and the host acting on the switch | one character, self-correcting |
| 10 | A switch lost inside the `&en` macro, which flips twice per keypress | reduced with cause 1, and now reaches one key: `\` |
| 11 | Two hosts on different languages — one firmware layer describing whichever profile you last switched to | fixed by the layout resync module, below |

Two things were checked and **excluded** as causes: macOS's "automatically switch to a document's
input source" is off here (`TextInputGlobalPropertyPerContextInput = 0` in `com.apple.HIToolbox`), and
the behavior queue cannot drop the `os_lang` half of `layer_en`/`layer_ru` — `ZMK_BEHAVIORS_QUEUE_SIZE`
is 64 and those macros queue two items.

**Closing the loop is not available on a stock host, and both routes were checked rather than
assumed.** There is no feedback: with the host on Russian, `HIDCapsLockLEDOn` reads `No` for the
op36's own HID service and for the internal keyboard, so macOS does not mirror the input source into
the Caps Lock LED for a keyboard *layout* the way it does for some input *methods*. And there is no
absolute setter: macOS and iPadOS offer only "select previous source" and "select next source", both
relative; Windows' `Ctrl+Shift+1`/`2` does nothing on iPadOS, which is why this keymap stopped sending
it. ZMK does read HID indicators (`app/src/hid_indicators.c`) but **no behavior exposes them to a
keymap**, so using them at all would mean custom C in a module.

That leaves two complete solutions, both rejected because **iPadOS is a required platform**: mirror
the host's input source into an LED from a host-side daemon and have the firmware follow it; or
install a custom `.keylayout` that maps this keyboard's Colemak-DH scancodes to ЙЦУКЕН positions,
which removes the firmware's language state entirely. Neither works on iPad. Do not re-derive these —
what is left is reducing the mechanical causes and keeping recovery cheap, and `nav` positions 6 and
7 are that recovery.

Cause 11 is the one that turned out to be fixable in firmware after all, because it needs no
knowledge of the host's layout — only of which host you are talking to, which BLE does tell the
keyboard. That is the layout resync module. Nothing else in this table moved because of it.

**The switch key went Caps Lock → `GLOBE` → Ctrl+Space → `GLOBE` plus `Hyper+F13`**, and the whole
detour is worth not repeating: `GLOBE` works on iPadOS and is ignored by macOS, which left the Mac
with no language switch at all for one firmware; Ctrl+Space then worked on both and was rejected for
being trivially easy to hit by accident. The lesson generalises — **testing a host setting is not testing the
path from this keyboard.** Setting `AppleFnUsageType` and pressing the MacBook's own Globe proved
the setting and nothing else, because the internal keyboard does not travel over BLE and Apple may
treat its own hardware apart. Whatever the switch key is, the fallback when it fails is `nav` 6 and
7, which move the firmware without needing the host at all.

## The layout resync module

**It was switched off once, to isolate a fault, and is on again.** Switching BLE profiles had left
the iPad unable to type letters on either language layer, and the module was the newest thing
touching layer state. It was the wrong suspect, and the reason to doubt it was available before the
firmware was ever flashed: its whole reach is `zmk_keymap_layer_activate` and `..._deactivate` on
layer 1, and letters work on both layer 0 and layer 1, so it has no way to kill them on both.
`config/op36.conf` sets nothing for the module now; it runs on its Kconfig default. **Three separate iPad faults in one day were all transient and none was
the keymap:**

| symptom | cleared by |
|---|---|
| Enter at position 33 dead, `nav` hold at 34 dead; both fine on the Mac | a BLE profile switch |
| no letters on either language layer after a profile switch | — (prompted this setting) |
| Spotlight accepted no input at all, while a text editor on the same iPad took everything | restarting the iPad |

Nothing host-dependent exists in the firmware to explain any of them:
`CONFIG_ZMK_HID_INDICATORS` is off, so the keyboard reads nothing back from either host, and layer
raising is decided entirely on the keyboard. The Spotlight one is the clearest — it refused input
even when opened by a swipe, with the keyboard not involved in opening it at all.

**So on iPadOS, exhaust the host before touching the keymap: reconnect the profile, then restart the
iPad.** That is the same instinct as "suspect the split connection before the keymap", extended to
the host's own state. Chasing any of the three into the devicetree would have found nothing — and
the second one cost this module a firmware in exile before the pattern was visible.

Everything below describes the module as written, for when it comes back.

`module/` is a Zephyr module this repository carries, enabled by `zephyr/module.yml`, which the build
workflow finds and turns into `-DZMK_EXTRA_MODULES`. It gives every BLE profile its own remembered
input language and restores it when you come back to that host. That is the whole of it.

Three layers. `module/src/resync_state.c` decides what should happen, `module/src/resync_adapter.c`
translates events and reconciles after pairing, and neither has a Zephyr type in it — both are tested
by `tests/resync-state/`, which `tests/run.sh` runs. `module/src/layout_resync.c` is only the binding:
it fills in a struct of platform calls and serialises the callbacks. That last layer cannot be tested
here, the simulator having no BLE, which is exactly why it holds no logic.

**The module never resets a language, only restores one.** The lock screen forces ASCII for the
password field, but after unlocking, both macOS and iPadOS restore whatever language was active
before locking — measured on both. A firmware that reset on reconnect would therefore have fixed the
password field and been wrong for the whole session afterwards, which is the worse trade. Restoring
what a host was left on is right for a plain profile switch and after a sleep alike.

The password field is left as it is. The manual answer is `nav` 6 before typing it and **`nav` 7
after unlocking** — both presses, because the first moves the firmware without moving ownership, so
nothing puts it back. Skip the second and the next profile switch saves that `en` as the Mac's
language.

Two gaps are accepted rather than solved. A host that changes language while awake is invisible. And
the memory is in RAM, so a deep sleep — `activity.c` powers the board off after
`CONFIG_ZMK_IDLE_SLEEP_TIMEOUT`, 600000 ms, when not on USB — loses it, and the first arrival at a
profile afterwards takes the default language while the host restores its own. Persisting to flash
would close that and is deliberately out of scope.

An earlier draft reset after an outage longer than a threshold, and carried a `RESET_PROFILES`
bitmask so one host could opt out. Both went when the measurement came in. The mask was deleted
rather than defaulted to "all" because it keyed on the **profile index**, which changes when a device
is re-paired — a trap that would have pointed at the wrong host later. If that behaviour is ever
needed, key it on the **peer address**, which the module already stores and compares in order to
notice re-pairing.

The only configuration is `CONFIG_ZMK_LAYOUT_RESYNC_ALT_LAYER`, which is the layer holding the second
language and defaults to 1. `op36.conf` sets nothing for the module at all.

The Kconfig guard is not tidiness. `app/CMakeLists.txt:47` builds `keymap.c` and `ble.c` only for a
central or non-split target, so `op36_right` has neither; `settings_reset` is not split and does build
`keymap.c`, but sets `CONFIG_ZMK_BLE=n` and so has no `ble.c`. Confirmed on CI run 34235280103, where
`CONFIG_ZMK_LAYOUT_RESYNC=y` appears for `op36_left` alone and each build carries its own positive
control — an empty grep result and a broken pipeline look identical otherwise.

The design and everything checked while arriving at it are in
`docs/superpowers/specs/2026-09-08-layout-resync-design.md`. The three facts it turns on, all in
`app/src/ble.c`: `zmk_ble_prof_select()` does not disconnect the outgoing host, both connection
callbacks ignore anything that is not `BT_CONN_ROLE_PERIPHERAL`, and `zmk_ble_active_profile_changed`
coalesces through one work item and only ever describes the active profile — which is why the module
registers its own connection callbacks instead of relying on it.

## ru_ext, and why it is not a plain `&lt`

Seven Cyrillic letters do not fit on 26 keys, so `ru_ext` (layer 2) holds them and is reachable
**only from `ru`**, by holding one of two letter keys — positions 23 and 26, which are `V` and `M` in
QWERTY terms and `м` and `ь` on this layer:

```
_  ц  _  _  ё  |  _  _  щ  э  _
ф  _  _  _  _  |  _  _  _  х  _
_  _  _  _  _  |  _  ъ  _  _  _
```

Two entrances, one per half, and the letters divide the same way: holding 23 (left) frees the right
hand for `щ э х ъ`, holding 26 (right) frees the left for `ц ё ф`. That is also why `ъ` may sit on
position 26 — you never reach it from the entrance it lives on.

**These keys use the stock `&lt`, and must not be switched to `&ltt`.** `&ltt` is `balanced`, which
decides hold as soon as the next key is released; on a letter key that is fatal. Rolling `м` into
`о` would raise the layer and resolve `о` against `ru_ext`'s `&none`, losing **both** letters, and
`быть ` would lose its `ь`. Stock tap-preferred raises the layer only on the `tapping-term-ms`
timer, so a roll always taps (`tests/ru-ext-rolls` proves both shapes). There is deliberately **no**
`require-prior-idle-ms` and no positional list: those would suppress the trigger right after a
keystroke, which is precisely when `ъ` and `х` are wanted. `tapping-term-ms` is the only knob here.

## Combos

**There are none left.** The layout switch was the last pair (`cmben`/`cmbru` on positions 31+34)
and it moved to a single key on position 30; `kha` (Х) and `hrdsgn` (Ъ) went when `ru_ext` arrived.
`config/op36.conf` still carries `CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO=3` and
`CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY=7`; they now bound nothing and are left only so that adding a
combo back does not need the limits rediscovered.

Two facts worth keeping if a combo is ever added again:

- A combo's `layers = <N>` is checked against `zmk_keymap_highest_layer_active()` (see
  `app/src/combo.c`), **not** against "is that layer active anywhere in the stack". A combo scoped to
  `ru` therefore does not fire while `nav` is held on top of it.
- A combo fires on **press**, so holding its keys switches exactly like tapping them. There is no way
  to tell the two apart in ZMK, which is what pushed `adj` off the Space+Backspace gesture in the
  first place.

The reason the deleted Cyrillic combos never carried a prior-idle guard still governs `ru_ext`: Х and
Ъ are wanted mid-word ("плохо", "объект"), so anything that suppresses a trigger right after a
keystroke suppresses exactly the cases you need.

## ZMK internals worth not re-deriving

All checked against upstream ZMK v0.3.0 or the build's own output:

- **Implicit modifiers are a single global that the next keypress overwrites.** `hid.c` keeps one
  `static zmk_mod_flags_t implicit_modifiers`, and `hid_listener_keycode_pressed` assigns the
  incoming event's set to it unconditionally. Explicit modifiers, registered when a modifier keycode
  is pressed on its own, accumulate instead. So `LS(LC(LA(X)))` is right for a one-shot chord like
  `&kp LA(LC(LG(LS(N2))))` on the numbers layers, and wrong for anything meant to be *held* across other
  keys. The released path even carries a comment admitting the tracking is approximate.
- **Macro `wait-ms` is only charged for real bindings**, not for control bindings like
  `&macro_press` (`app/src/behaviors/behavior_macro.c`). `&macro_wait_time <ms>` sets it inline when
  a pause is needed at one point only.
- **`&out OUT_BLE` / `OUT_USB` is a preference, not a switch.** `get_selected_transport()` in
  `app/src/endpoints.c` uses it only when both transports are ready and otherwise takes whichever
  one is — readiness being `zmk_usb_is_hid_ready()` and `zmk_ble_active_profile_is_connected()`. You
  cannot strand yourself without output.
- **USB output is real on this board, and the preference is sticky.** `CONFIG_ZMK_USB=y` in the
  op36_left build and `zephyr_udc0: &usbd { status = "okay" }` on the `ergohaven` board, so the left
  half does HID over the cable while still holding the split over BLE. `preferred_transport`
  initialises to `ZMK_TRANSPORT_USB` but is written to `endpoints/preferred` in settings and restored
  at boot, so **one past press of `&out OUT_BLE` keeps the board on radio forever, cable or not** —
  which reads as "this keyboard is BLE-only". `&out OUT_USB` is the undo.
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
- **Layer/host desync is reduced, not eliminated.** Two causes are irreducible without host-side
  software — the host switching by itself, and every boot starting on `en` — and iPadOS support rules
  out the two solutions that would close the loop. See "Every way the layer and the host can drift
  apart" for the full accounting and what is still worth trying.
- **The switch key has no guard.** Position 30 fires the layout switch on press, unlike the combo it
  replaced, which needed two keys and a 150 ms prior-idle window. That is the point — it can no
  longer be missed — but it also cannot be un-pressed. If brushing the outer left thumb turns out to
  switch layouts in real use, the fix is to make it a hold rather than to reinstate a combo.
- `adj` is stripped to `&bootloader`, **two** `&bt BT_SEL` (0-1), `&bt BT_CLR`, both `&out` and
  `&studio_unlock`; everything else on it is `&none` by intent. Two profiles is two hosts, the Mac
  and the iPad. Profiles 2 and 3 have no key — ZMK still keeps their bonds, there is just no way to
  select them from the keymap, and adding one back is a binding on any of the free positions.
  **The profile keys sit on the left hand, at positions 1 and 2**, because `adj` is held with the
  right thumb and the left hand is the free one; the rest of the layer has not followed them yet.
  `&bt BT_CLR` sits on the bottom row, away from the home row it used to share with the outputs,
  because it is the destructive one.

## Dormant config (not built)

Reference only. Trackball keymaps declare their own `default_transform` inline, so one `trackball`
shield serves three devices with per-variant tuning in `cmake-args`. `velvet_v3_ui*` raises a Mouse
layer on pointer motion via `zmk,input-processor-temp-layer`. `k03` binds six encoders through a
shared `enc_vol` and `imperial44` two through `&inc_dec_kp`; both need `CONFIG_EC11=y` and
`CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y`.
