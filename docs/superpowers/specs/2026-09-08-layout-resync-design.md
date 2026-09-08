# Layout resync: giving each host its own language

Design for a small ZMK module that gives every BLE profile its own remembered input language and
restores it when you come back to that host. It is the first C in this repository.

## The problem

`op36_ruen` holds the current input language as a firmware layer — `en` is layer 0, `ru` is layer 1 —
and keeps it in step with the host by convention: every action that changes one changes the other.
The system is open loop; the firmware issues a Caps Lock toggle and never learns the result. Two
failures follow from that and neither is reachable from the keymap.

**Two hosts at once.** The firmware has one language state; each host has its own. Switch from the
Mac to the iPad, change the language there, come back, and the firmware is describing the wrong
device. The state belongs per profile, and today it is global. That is what this module fixes.

**The lock screen is not fixed, and cannot be.** This design began as an attempt at a second problem:
the password field takes ASCII while the firmware is still on `ru`, so the password arrives scrambled.
Making the firmware follow that looked easy — a host that has been away has surely reset — until the
behaviour was measured on both platforms. **The lock screen forces ASCII only for the password field;
after unlocking, the session restores the language that was active before locking.** So a firmware
that reset on reconnect would type the password correctly and then be wrong for the whole session
afterwards, which is a worse trade: the desync would live where the work happens rather than in one
field. The transition at unlock is invisible — no disconnect, no event — so it cannot be followed.

The module therefore does not reset anything. Restoring the language a host was left on is correct
both for a plain profile switch *and* after a sleep, because the session restores the same thing on
its side. The password field stays as it is; `nav` positions 6 and 7 remain the manual answer, and
they send no keystroke, so using one at a lock screen is safe.

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
  it runs, and `K_WORK_DEFINE` gives the three deferred call sites — `set_profile_address()`,
  `connected()`, `disconnected()` — **one** work item. (`zmk_ble_prof_select()` calls it directly, at
  line 303.) A disconnect followed quickly by a connect submits twice, runs once, and the intervening
  state is never seen.
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

- `lang_alt` — the remembered language for this profile, and `lang_known`, whether it was ever
  established
- `link_up` — whether this profile's host link is up

Plus one global: `owner`, the profile index whose language the current layer state represents, or
none.

There is no timing here at all. An earlier draft measured how long a host had been away in order to
decide whether it had slept and reset; with the reset branch gone, nothing consumes a duration, and
the threshold, the sticky verdict and the outage clock went with it.

### Ownership, and the bug it fixes

The layer state is global; a profile's language is not. Conflating them produces a concrete failure,
found in review of the first draft: the iPad is on `en`; you select the Mac profile, which is
disconnected and remembers `ru`, and leave before it connects. The layer never became the Mac's, yet
a naive save would write `en` into the Mac's slot. **Fixed by `owner`**: the language is saved for a
profile only while that profile owns the layer state.

`owner` is set when a decision is applied for a profile, and cleared when we move to a profile for
which no decision has been made or when BLE stops being the selected transport. Saving requires an
`owner` to be set, and happens at the two moments listed under "When the language is saved" — an
ownership change and the owner's own disconnect.

### Deciding

When a profile becomes both active and connected, or when BLE becomes the selected transport again:

| condition | meaning | action |
|---|---|---|
| it is still the `owner` | nothing happened to it; the layer already *is* its language | **nothing** |
| it is known | you have arrived at a host that was left on a language | restore `lang_alt` |
| it is not known | first time at this host | the default language stands |

The first row matters more than it looks. If a profile never lost ownership, its stored `lang_alt`
may be older than the user's latest choice — they may have switched language since — so restoring
from memory would drag them back. Doing nothing is both correct and cheaper.

The third row is why an unknown profile does not adopt what is on screen: what is on screen belongs
to the host you just left.

### When the language is saved

`lang_alt` is written for a profile at exactly two moments, both while that profile is the `owner`:

- when its link goes down, and
- when ownership is about to move elsewhere — another profile, or away from BLE entirely.

Saving only on ownership change is not enough: a profile can own the layer, have the user change
language under it, and then lose the link briefly without any ownership change. Without the
save-on-disconnect the memory would still hold the older language.

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

### Transport changes are their own signal

BLE profiles and their connections keep existing while USB is the chosen endpoint —
`get_selected_transport()` returns the stored preference whenever both are ready. Guarding only the
*application* of a layer change is not enough, because ownership can go stale with no BLE event at
all:

> BLE with the Mac on `ru` → switch to USB → change the language to `en` → switch back to BLE. The
> connection never dropped and the active index never changed, so neither the connection callbacks nor
> `zmk_ble_active_profile_changed` need fire. The layer is left holding a USB-era `en`, which could
> then be saved as the Mac's language.

So the module also subscribes to `zmk_endpoint_changed`:

- **leaving BLE** — save the current owner's language, then clear `owner`. Nobody owns the layer while
  USB drives it.
- **returning to BLE** — decide for the selected profile if it is connected, exactly as on arrival.

BLE connection history keeps being recorded throughout, including while USB is selected; only the
*application* of a layer change is gated on BLE being the selected transport. **No language memory is
kept for USB** — the module's job there is to not corrupt the BLE profiles, not to manage USB.

### The adapter has to be idempotent

The pure state machine is testable; the code that feeds it is not, and that is where the remaining
risk sits. Two rules it must obey, to be stated as requirements rather than discovered later:

- **State update, layer application and the `owner` change are one step.** A connection callback and
  a ZMK event can describe the same transition, and they do not arrive in a guaranteed order — the
  event is a coalescing snapshot, the callback is immediate. Applying a layer without moving `owner`,
  or the reverse, leaves the two disagreeing.
- **A repeat notification must be harmless.** The same transition may be reported twice, or reported
  when nothing changed. Deciding again for a profile that already owns the layer with no pending
  verdict must do nothing, which the first row of the decision table already gives — but it has to
  hold for the adapter's own re-entry, not only for the automaton.

### Nothing is written to flash, and that is a limitation rather than a virtue

The memory lives in RAM, so a deep sleep loses it. `activity.c` powers the board off after
`CONFIG_ZMK_IDLE_SLEEP_TIMEOUT` — 600000 ms here — when it is not on USB, and waking is a reset, so
the module comes back knowing nothing. The first arrival at a profile then takes the default language
while the host, which restores what it had, may well be on the other one.

An earlier draft called this correct on the grounds that the host would have reset too. **That was
the reset hypothesis, and the measurement disproved it.** It is now simply an accepted gap, the same
one as the first connection after boot: no memory, so the default stands.

Persisting to flash would close it, and is deliberately out of scope here rather than justified away.
It would mean writing on every language change — `settings_save_one` on a path the keymap currently
touches only for Studio edits — and deciding what a stored language means after the peer behind a
profile has changed. Neither is hard; both are a separate piece of work.

## Configuration

| symbol | meaning | default |
|---|---|---|
| `ZMK_LAYOUT_RESYNC` | enable the module | `y` |
| `ZMK_LAYOUT_RESYNC_ALT_LAYER` | the layer holding the alternate language | `1` (`ru`) |

Nothing else. There is no threshold because there is no reset, and nothing per host because every
host is treated the same: remember what it was left on, put it back on arrival.

`ZMK_LAYOUT_RESYNC` **must** depend on `ZMK_BLE` **and** on the central or non-split role. The two
excluded matrix entries fail for different reasons, and only both conditions together cover them:

- `op36_right` is a split peripheral, so `app/CMakeLists.txt:47`
  (`(NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL`) builds neither `keymap.c` nor `ble.c`.
- `settings_reset` is not split at all, so it *does* build `keymap.c`; what it lacks is `ble.c`,
  because its shield conf sets `CONFIG_ZMK_BLE=n`.

Confirmed on CI run 34214748643, where `CONFIG_ZMK_LAYOUT_RESYNC=y` appears for `op36_left` alone
while the two positive controls prove the grep was working.

The design assumes exactly two languages: `ALT_LAYER` exists, differs from the default, and is read
through `zmk_keymap_layer_active()`. A third language would break the Caps Lock macros in the keymap
long before it troubled this module, so it is not generalised here.

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

**The password field.** It takes ASCII while the firmware is on `ru`, and this module does not fix
that. It cannot: the firmware would have to be `en` for the lock screen and back on the session's
language a moment later, and the unlock carries no disconnect and no event. Measured on both
platforms — lock with Russian active, unlock, type, and Cyrillic comes back — so the session restores
what it had, and a firmware that reset on reconnect would be wrong for the entire session instead of
one field. `nav` positions 6 and 7 remain the manual answer and send no keystroke, so either is safe
to press at a lock screen.

## Verification

**The device is the only place the BLE transitions and the OS behaviour can be seen.**
`native_posix_64` has no BLE, no second host and no profiles.

**The decision logic, however, is testable and will be tested.** It is extracted into a pure
**state-transition** function — (state, event, timestamp) → (new state, action) — with no Zephyr types
in its signature. Testing branch selection alone would miss the whole class of bugs review found,
which are about *what gets stored and who owns it*, so the tests assert the resulting state and not
just the action.

Sequences, each one a bug review found or a rule the design turns on:

- The iPad on `en`; select the disconnected Mac, which remembers `ru`; leave before it connects — the
  Mac's `lang_alt` must be untouched.
- Switch between two connected hosts — each side's language restored.
- A profile owns the layer, the user changes language, a link blip follows with no profile change —
  the restore must not drag the old language back.
- BLE/Mac on `ru` → USB → language changed to `en` → back to BLE — the Mac's memory must not acquire
  the USB-era language, and its `ru` must come back.
- The transport observed late, in the window before ZMK announces it — nothing applied, nothing owned,
  and no language attributed to the wrong profile.
- A stale connection snapshot must not fabricate a link transition, or a real reconnect stops
  triggering the restore.
- A repeat notification after a restore — nothing happens the second time.
- An unknown profile — the default stands, and the previous host's language is not adopted.
- Profile cleared or re-paired while active — the whole history for that profile goes, since the peer
  behind the index is now a different machine.

That test runs from `tests/run.sh` alongside `check-en-letters.py`, so it costs nothing to keep.

Device checks that remain: the two hosts held on **different** languages and typed on after each
switch, repeated, since a single lucky state proves nothing and a shared language proves less; a walk
out of range and back; and lock, unlock, then typing in an application — which must come back on the
language the session restores, with the firmware agreeing, because that pairing is the whole point of
dropping the reset branch.

The existing suite and `check-en-letters.py` must stay green throughout: the module changes no keymap
behaviour.

## Risks

- **A host that changes language while awake** — menu bar, another application, a foreign shortcut —
  is still invisible, and the module will restore a stale language the next time you come back to it.
  Cause 2 in CLAUDE.md's table; it needs feedback the platform does not give.
- **First C in the repository.** CLAUDE.md's claim that there is no ZMK fork stays true — this is a
  module — but the statement that the repo holds no application code stops being true and must be
  updated.
- **Two of three matrix entries lack what the module links against**, for different reasons —
  `op36_right` by role, `settings_reset` by `CONFIG_ZMK_BLE=n`. The Kconfig guard is not tidiness;
  without it CI breaks.
