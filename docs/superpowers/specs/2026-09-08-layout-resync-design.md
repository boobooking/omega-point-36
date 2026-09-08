# Layout resync: keeping the firmware layer with the host it is talking to

Design for a small ZMK module that keeps `op36_ruen`'s language layer in step with the host, across
host sleep and across BLE profile switches. It is the first C in this repository.

## The problem

`op36_ruen` holds the current input language as a firmware layer — `en` is layer 0, `ru` is layer 1 —
and keeps it in step with the host by convention: every action that changes one changes the other.
The system is open loop; the firmware issues a Caps Lock toggle and never learns the result. Two
failures follow from that and neither is reachable from the keymap.

**Host sleep.** The Mac wakes with the layout reset to ABC, but the firmware is wherever it was. Type
the login password and the ЙЦУКЕН scancodes arrive as scrambled Latin. The firmware already recovers
after a *deep* sleep — `activity.c` calls `sys_poweroff()` and saves nothing, so waking is a reset and
layer state is not persisted — but only after `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT` (600000 ms here), and
`activity.c` gates that on `!is_usb_power_present()`. Close the lid and reopen it two minutes later,
or work on the cable, and the firmware never resets.

**Two hosts at once.** The firmware has one language state; each host has its own. Switch from the
Mac to the iPad, change the language there, come back, and the firmware is describing the wrong
device. The state belongs per profile, and today it is global.

## Why this cannot be done in the keymap

No behaviour in ZMK v0.3.0 observes connection or activity state; the listeners for those events are
all C (`backlight.c`, `battery.c`, `ble.c`, `endpoints.c`, `rgb_underglow.c`). There is no keymap
hook, and no Kconfig ties layers to either signal. A module is the smallest thing that can do this.

Closing the loop the other way — having the firmware *read* the host's layout — was investigated and
is not available: with the host on Russian, `HIDCapsLockLEDOn` reads `No` for the op36's own HID
service, so macOS does not mirror the input source into any LED. See CLAUDE.md, "Every way the layer
and the host can drift apart".

## The signals, and why the obvious one is not enough

`zmk_ble_active_profile_changed` looks like the whole answer and is not. Three properties of
`app/src/ble.c` rule it out as the only input:

- **It is raised from four places, not three.** `set_profile_address()` (line 119) raises it too, so
  pairing and `&bt BT_CLR` produce it as well as profile selection and connect/disconnect.
- **It is a snapshot, not a transition.** `raise_profile_changed_event()` reads `active_profile` when
  the work item runs, and `K_WORK_DEFINE` gives all four call sites **one** work item. A disconnect
  followed quickly by a connect submits twice, runs once, and the intervening state is never seen.
- **It only ever concerns the active profile.** `connected()` and `disconnected()` gate on
  `is_conn_active_profile(conn)`, which compares against `profiles[active_profile]`. A host that drops
  while we are on another profile is invisible — exactly the measurement this design needs.

So the module registers **its own** `BT_CONN_CB_DEFINE` and uses the ZMK event only to learn that the
active index changed:

- Connection callbacks filter `info.role == BT_CONN_ROLE_PERIPHERAL`, which is how `ble.c` itself
  excludes the split peripheral link, and map the peer to a profile with `zmk_ble_profile_index()`
  (public in `zmk/ble.h`). That yields accurate per-profile connect and disconnect times for **all**
  profiles.
- `zmk_ble_active_profile_changed` tells us the active profile may have changed; the module compares
  against its own record rather than trusting the event to describe a transition.

## Design

Per profile, in RAM, `ZMK_BLE_PROFILE_COUNT` entries (`CONFIG_BT_MAX_PAIRED` minus
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS`, so 5 on this build):

- `lang_alt` — the remembered language for this profile
- `disconnected_at` — when this profile's host link went down, from the connection callback only

Plus one global: `owner`, the profile index whose language the current layer state represents, or
none.

### Ownership, and the two bugs it fixes

The layer state is global; a profile's language is not. Conflating them produces two concrete
failures, both found in review of the first draft:

- The Mac dropped a minute ago. You switch to the iPad and straight back. If switching away rewrote
  the Mac's timestamp, its minute-long absence would measure as two seconds. **Fixed by taking
  `disconnected_at` only from the connection callback**, never from profile switching.
- The iPad is on `en`. You select the Mac profile, which is disconnected and remembers `ru`, and
  leave before it connects. The layer never became the Mac's, yet a naive save would write `en` into
  the Mac's slot. **Fixed by `owner`**: the language is saved for a profile only while that profile
  owns the layer state.

`owner` is set when a decision is applied for a profile, and cleared when we move to a profile for
which no decision has been made. Saving happens only when `owner` is about to change and is set.

### Deciding

When a profile becomes both active and connected:

| condition | meaning | action |
|---|---|---|
| link never went down since we last owned it | the host stayed awake | restore `lang_alt` |
| link came back, gap ≥ threshold | away long enough to have slept and reset | clear to the default language |
| link came back, gap < threshold | radio hiccup, or an immediate reconnect after a switch | restore `lang_alt` |

`gap` is `now - disconnected_at`. This rests on one assumption worth stating plainly: **a sleeping
host drops the BLE connection.** A host that slept while holding the link open would look like one
that stayed awake, and its stale language would be restored — no worse than today.

Two cases the threshold cannot separate, accepted rather than solved: a short sleep that still reset
the layout is missed, and a long radio loss from an awake host causes a false reset.

### Applying the change

**Never `zmk_keymap_layer_to()`.** It loops `zmk_keymap_layer_deactivate()` over every layer before
activating the target (`keymap.c:214`), so firing it while a momentary layer is held would drop that
layer with its key still down — including `en_letters` under a held modifier, which is precisely the
mechanism the keymap relies on to type Latin shortcuts on `ru`. `nav`, `numbers`, `adj` and the
symbol layers are exposed the same way.

The module therefore touches only the language layer:
`zmk_keymap_layer_activate(ALT)` / `zmk_keymap_layer_deactivate(ALT)`. Layer 0 is the default and
`set_layer_state` refuses to deactivate it, so "go to `en`" *is* deactivating the alternate layer.
Reading the current language is `zmk_keymap_layer_active(ALT)`, which is unaffected by whatever
momentary layer sits on top.

No keystroke is ever sent. On wake the host sets its own layout; the firmware only has to agree.

### Only while BLE is the selected transport

BLE profiles and their connections keep existing while USB is the chosen endpoint —
`get_selected_transport()` returns the stored preference whenever both are ready. Without a guard, a
background BLE reconnect could change the layer under a user typing over USB, and a language chosen
while on USB could be recorded into a BLE profile.

The module acts only when the selected endpoint transport is BLE. Fixing the layout for USB is out of
scope; not corrupting it is not.

### Why nothing is written to flash

Deep sleep resets the board, which loses this state — and that is correct, because a deep sleep means
a long absence and the host will have reset too. Persisting it would survive exactly the case where
it is most likely to be wrong.

## Configuration

| symbol | meaning | default |
|---|---|---|
| `ZMK_LAYOUT_RESYNC` | enable the module | `y` |
| `ZMK_LAYOUT_RESYNC_ALT_LAYER` | the layer holding the alternate language | `1` (`ru`) |
| `ZMK_LAYOUT_RESYNC_DISCONNECT_MS` | reconnect gap above which the host is assumed to have reset | `10000` |

`ZMK_LAYOUT_RESYNC` **must** depend on `ZMK_BLE` and on the central or non-split role.
`app/CMakeLists.txt:47` builds `keymap.c` and `ble.c` only under
`(NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL`; without the guard the module fails to link
on `op36_right` and `settings_reset`, which is two of the three matrix entries.

The design assumes exactly two languages: `ALT_LAYER` exists, differs from the default, and is read
through `zmk_keymap_layer_active()`. A third language would break the Caps Lock macros in the keymap
long before it troubled this module, so it is not generalised here.

10000 is a starting guess, not a measurement. Every decision logs the profile, the disconnect and
connect moments, the measured gap and the branch taken, so the value can be tuned from observation.

## Build integration

`.github/workflows/build-user-config.yml` already does:

```sh
if [ -e zephyr/module.yml ]; then
    export zmk_load_arg=" -DZMK_EXTRA_MODULES='${GITHUB_WORKSPACE}'"
```

so adding `zephyr/module.yml` at the repository root is enough — no workflow change, no fork, and
`config/west.yml` is untouched. The module body lives in a subdirectory to keep the root readable:

```
zephyr/module.yml        build: { cmake: module, kconfig: module/Kconfig }
module/CMakeLists.txt
module/Kconfig
module/src/layout_resync.c
module/src/decide.h      pure decision function, shared with the test
```

Everything it uses is public API: `zmk_keymap_layer_activate`, `zmk_keymap_layer_deactivate`,
`zmk_keymap_layer_active`, `zmk_ble_profile_index`, `zmk_endpoints_selected`, Zephyr's
`BT_CONN_CB_DEFINE`, and the `ZMK_LISTENER` / `ZMK_SUBSCRIPTION` macros.

## Out of scope

Named so they are not mistaken for oversights:

- **Correcting the layout on USB.** No BLE profile is in play and the keyboard never deep-sleeps on
  the cable, so the password case remains broken there. The module only guarantees it will not make
  USB worse. `usb_conn_state_changed` is the analogous signal if it is ever worth doing.
- **The host changing layout while awake** — menu bar, another application, a foreign shortcut. Still
  undetectable; cause 2 in CLAUDE.md's table, and it needs feedback the platform does not give.
- **The first connection to a profile after boot.** No memory exists yet, so the default language
  stands. Right answer, but not a decision.
- **Profile clearing and re-pairing.** `&bt BT_CLR` and pairing both raise the profile-changed event
  through `set_profile_address()`. The module must not crash or act oddly, but restoring a language
  for a profile whose peer just changed is meaningless; that profile's memory is simply dropped.
- **Pressing the layout switch while disconnected.** The keymap will happily toggle the layer and send
  a Caps Lock nobody receives. The module records whatever state results; it does not try to undo it.

**iPadOS behaviour after sleep is unverified, and this is a real risk rather than a free one.** The
first draft claimed the cost was "one manual switch, same as today". That was wrong: if iPadOS
preserves the Russian layout across sleep, then today the iPad comes back *consistent*, and the
reconnect branch would **introduce** a desync that does not exist now. Before enabling the reset
branch for the iPad's profile, check it — switch the iPad to Russian, lock it, unlock, and type one
letter in Notes. If it comes back Russian, that profile should get restore-only behaviour and no
reset.

## Verification

**The device is the only place the BLE transitions and the OS behaviour can be seen.**
`native_posix_64` has no BLE, no second host and no profiles.

**The decision logic, however, is testable and will be tested.** It is extracted into a pure function
over (per-profile state, event, timestamp) returning an action, with no Zephyr types in its
signature. A host-compiled test drives it through the sequences that matter, including the two that
review found in the first draft:

- Mac drops, switch to iPad and back, Mac reconnects — the gap must still measure the full minute.
- iPad on `en`, select the disconnected Mac which remembers `ru`, leave before it connects — the
  Mac's memory must be untouched.
- Switch between two connected hosts — restore each side's language, threshold never consulted.
- Reconnect below and above the threshold.
- Profile cleared while active.

That test runs from `tests/run.sh` alongside `check-en-letters.py`, so it costs nothing to keep.

Device checks that remain: lid close and reopen inside and outside the threshold; Mac to iPad and
back inside a minute; a walk out of range and back; the full password-screen chain — lock screen,
unlock, then typing in an application, since the lock screen may force ASCII independently of what
the session restores.

The existing suite and `check-en-letters.py` must stay green throughout: the module changes no keymap
behaviour.

## Risks

- **The threshold is a guess.** Tunable without code changes, and every decision is logged.
- **A host that sleeps without dropping the link** is indistinguishable from one that stayed awake;
  its stale language would be restored. No worse than today.
- **First C in the repository.** CLAUDE.md's claim that there is no ZMK fork stays true — this is a
  module — but the statement that the repo holds no application code stops being true and must be
  updated.
- **Two of three matrix entries do not build `keymap.c` or `ble.c`.** The Kconfig guard is not tidiness;
  without it CI breaks.
