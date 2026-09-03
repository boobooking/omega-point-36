# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A ZMK **user config** (`zmk-config`), forked from Ergohaven's. There is no application code, no test
suite, and no local toolchain checked in — the entire repo is devicetree keymaps, Kconfig fragments,
and a build matrix. Firmware is produced by GitHub Actions.

**Only the Omega Point 36 (`op36`) with the RU/EN keymap is built.** `config/` still holds the
upstream keymaps for velvet_v3, velvet_v3_ui, k03, imperial44, and the trackballs, but no
`build.yaml` entry references them, so they are never compiled. Treat them as reference material,
not as code that has to keep working. `git log` before the build-matrix trim shows how they were
wired up if one ever needs restoring.

Everything builds against the **`ergohaven/ergohaven-zmk`** ZMK fork (`config/west.yml`), which
supplies the `ergohaven` board, all shields, and pieces the upstream ZMK tree does not have:
`scroll-snap.dtsi`, `input/processors/sensor_rotation.dtsi`, the `zmk,input-processor-temp-layer`
compatible, and the `keymap:` build-matrix key. When something referenced by a keymap isn't in this
repo, it lives in that fork.

## Building

Push (or open a PR, or dispatch manually) → `.github/workflows/build.yml` delegates to
`ergohaven/ergohaven-zmk/.github/workflows/build-user-config.yml@main`, which builds every entry in
`build.yaml` and uploads a `firmware` artifact of `.uf2` files. There is no lint or test step; a
broken keymap surfaces as a failed matrix job, and that CI run is the only compile check that
exists — nothing in this repo validates a keymap locally.

To reproduce the main entry locally:

```sh
west init -l config
west update
west zephyr-export
west build -s zmk/app -d build/op36_left -b ergohaven -- \
  -DZMK_CONFIG="$PWD/config" -DSHIELD=op36_left \
  -DKEYMAP_FILE="$PWD/config/op36_ruen.keymap" -DCONFIG_ZMK_STUDIO=y
```

(plus `-S studio-rpc-usb-uart` on the `west build` line for the Studio snippet). Note that
`west init -l` clones `zmk/`, `zephyr/`, `modules/`, `bootloader/`, and `tools/` into the repo root
and none of them are in `.gitignore` — build outside the repo or clean up afterwards. In practice
pushing and letting CI build is the cheaper path.

## Flashing and the split

The op36 shield's `Kconfig.defconfig` in the fork gives `ZMK_SPLIT_ROLE_CENTRAL` to
`SHIELD_OP36_LEFT`, so the **left half is central and holds the whole keymap**. The right half is a
peripheral: it scans its own matrix and ships key positions over BLE, and the central resolves what
they mean. Consequences:

- **Keymap changes only require reflashing the left half.** Layers, combos, macros, home-row mods —
  none of it exists on the right half. There is no and cannot be a "ruen" build of the right half.
- `settings_reset-ergohaven-zmk.uf2` is a utility: flash it to both halves, then flash the normal
  firmware back, to clear stored settings and BLE pairings.

**If keys appear broken, suspect the split connection before the keymap.** A disconnected right half
presents as "modifiers don't work" / "that key does nothing" rather than as an obviously dead half,
because the failing keys are simply the ones that live on it. Power-cycle both halves first; that
has already resolved one session's worth of apparent keymap bugs.

## How build.yaml maps to config/

`build.yaml` is the source of truth for what gets built. Every entry has `board: ergohaven`; the
`shield` selects the keyboard and role. A file is only compiled if some matrix entry pulls it in.

- **`config/<shield>.conf`** — Kconfig fragment, matched by shield name. `op36_left` and `op36_right`
  therefore share `op36.conf`.
- **`config/<keymap>.keymap`** — the keymap. Defaults to the base name for the split halves; the
  `keymap:` matrix key (an ergohaven fork extension, resolved to `-DKEYMAP_FILE=`) overrides it,
  which is how `op36_ruen` gets built instead of `op36`.
- **`config/<keymap>.json`** — physical layout consumed by the keymap editor, matched to the
  **keymap** filename, not the shield. The `*_ruen.json` files are symlinks to their base `.json`;
  keep them symlinks. The entry order must match the binding order in the keymap, and `sensors[]`
  must match the number of `sensor-bindings`.

Shield-name vocabulary: `_left`/`_right` are the split halves, `_qube` is the wireless dongle
(renamed from "dongle" historically), `<kb>_left_qube` is the half paired to a dongle, and
`settings_reset` wipes stored settings. Dongle builds add `qube dongle_screen` as extra shields and
use `_qube.conf` (`CONFIG_ZMK_SLEEP=n` plus `CONFIG_ZMK_POINTING=y`). No dongle is in use here.

Adding a variant means adding matrix entries **and** the matching `.conf` / `.keymap` / `.json`;
a Studio-unlockable build also needs `snippet: studio-rpc-usb-uart`,
`cmake-args: -DCONFIG_ZMK_STUDIO=y`, and an explicit `artifact-name` when two entries would
otherwise collide.

## Keymap conventions

**`op36.keymap` is dead weight but still present.** Only `op36_ruen.keymap` is built. The two are
independent copies, not includes — do not assume a change to one reaches the other.

**Layer order in `op36_ruen` is en / ru / sym_en / sym_ru / nav / adj.** The other (unbuilt) `_ruen`
keymaps use en / ru / nav / sym_en / sym_ru / adj, so `&mo 4` means different things in different
files. Re-read the layer order before touching a `&mo N`, `&to N`, or a `layers = <N>`.

**Every keymap starts with `#include "keys_ru.h"`**, including ones with no Cyrillic bindings. It is
a generated Unicode-licensed header of `RU_*` HID usages; treat it as vendored and don't hand-edit
it.

**Home-row mods** (`hml`/`hmr`) carry `hold-trigger-key-positions` listing the whole opposite hand,
so a right-hand mod only engages when the next key is on the left half, and vice versa. Those
position lists are tied to the physical layout — changing key count or ordering invalidates them.
Positions are 0-9 / 10-19 / 20-29 for the three rows, then 30-32 (left thumbs) and 33-35 (right
thumbs).

**Combos** are bounded by `config/op36.conf`: `CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO=3` and
`CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY=7`.

A combo's `layers = <N>` is checked against `zmk_keymap_highest_layer_active()` (see
`app/src/combo.c` in the fork), **not** against "is that layer active anywhere in the stack". A
combo scoped to the `ru` layer therefore does not fire while `nav` is held on top of it.

`cmben`/`cmbru` carry `require-prior-idle-ms = <150>` because they sit on positions 2+3 — `E`+`R` in
Latin, `У`+`К` in Cyrillic — both same-hand rolls that occur in ordinary words. `kha`/`hrdsgn`
deliberately do **not** have that guard: Х and Ъ are needed mid-word ("плохо", "объект"), and the
guard would suppress exactly those cases.

## The RU/EN dual-layout system

`op36_ruen.keymap` keeps a firmware layer (`en` = 0, `ru` = 1) in sync with the host's input
language. **The host must have "switch languages using Caps Lock" enabled** — this works on macOS
and iPadOS, which are the target platforms.

The critical constraint: Caps Lock is a **toggle**, not a selector. There is no "set the host to EN"
on these platforms (Windows' `Ctrl+Shift+1`/`Ctrl+Shift+2`, which this keymap used to send, does
nothing on iPadOS). Everything below follows from that.

- `os_lang` sends `&kp CAPS` and nothing else. It is the single macro for "flip the host language";
  `to_en`/`to_ru` no longer exist, because with a toggle they would be the same action.
- `layer_en` / `layer_ru` are `&to 0 &os_lang` / `&to 1 &os_lang`. They are correct **only when
  invoked from the layer they are leaving**, and the only callers are the `cmben`/`cmbru` combos,
  which are layer-scoped (`layers = <1>` and `layers = <0>`) so each fires only from its own side.
  Do not bind these to anything unconditional — pressing "go to EN" while already on EN flips the
  host and desynchronizes it from the firmware.
- **nav positions 20 and 29 are `&to 0` / `&to 1`** — firmware-only resync, deliberately sending no
  keystroke. If the host and the firmware disagree (the user switched language some other way),
  press the one matching the host's actual layout.
- The `en` one-param macro types a single key in EN and returns to RU, for glyphs that don't exist
  on the Cyrillic layout. It survives toggle semantics because it is balanced — flip, type, flip
  back — which holds as long as the host's language cycle has exactly two stops.
- `wait-ms = <50>` on the `en` macro is a guess at how long the host needs to apply the language
  switch, not a measured value. Raise it if `sym_ru` emits the wrong glyph, lower it if it feels
  sluggish. Macro `wait-ms` is only charged for real bindings, not for control bindings like
  `&macro_press` (see `app/src/behaviors/behavior_macro.c`), and `&macro_wait_time <ms>` can set it
  inline if a pause is ever needed at one point only.
- Cyrillic keys are `RU_CYRILLIC_*` from `keys_ru.h`, i.e. the EN scancode that lands on that letter
  once the host is in RU.

Two alternatives to `CAPS` exist if it ever needs replacing. `Ctrl+Space` was confirmed on the
iPad to be a clean two-stop toggle — the emoji keyboard stays out of the cycle even when added as a
third keyboard. `&kp GLOBE` (Consumer usage `0x029D`, the Apple Globe key) is available as a keycode
in both upstream ZMK and the fork, but was never tested on the device. Both are "next layout", never
"select layout N", so the toggle constraint above applies to them equally.

**This keymap is macOS/iPadOS-specific.** On Windows, `CAPS` is an ordinary Caps Lock and the whole
scheme breaks; supporting both would need a separate keymap and `build.yaml` entry.

Known gap: **`RU_CYRILLIC_IO` (ё) is not bound anywhere.** The `ru` layer holds 30 letters and
`kha`/`hrdsgn` add Х and Ъ via combos, for 32 of 33. nav position 25 is free (`&none`) if it ever
needs a home.

## Dormant config (not built)

Kept for reference only; nothing here is compiled. Trackball keymaps declare their own
`default_transform` inline, so one `trackball` shield serves three physically different devices, with
per-variant tuning in `build.yaml` `cmake-args`; pointer behavior is layered through
`&trackball_listener` child nodes keyed by `layers = <N>`. `velvet_v3_ui*` raises a Mouse layer on
pointer motion via `zmk,input-processor-temp-layer`. `k03` binds six encoders through a shared
`enc_vol` (`zmk,behavior-sensor-rotate`) and `imperial44` two through `&inc_dec_kp`; both need
`CONFIG_EC11=y` and `CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y`.
