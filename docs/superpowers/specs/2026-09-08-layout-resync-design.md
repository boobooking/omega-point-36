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

- `lang_alt` — the remembered language for this profile
- `link_down_at` — when this profile's host link went down, from the connection callback only
- `needs_reset` — sticky: an outage longer than the threshold has happened and has not been acted on

Plus one global: `owner`, the profile index whose language the current layer state represents, or
none.

`needs_reset` is a **verdict, not a duration**. It is decided at the moment the link comes back, from
`link_up_at - link_down_at`, and it is sticky: once a long outage has set it, a later short outage
must not clear it. Two bugs follow from getting this wrong, and both were found in review:

- Measuring `now - link_down_at` at decision time counts the time the link was *up* again. An
  inactive profile that blinked for a second and was selected a minute later would measure a
  minute-long outage and reset for no reason.
- Clearing the verdict on any reconnect loses a genuine sleep that happened to be followed by a brief
  hiccup before you came back.

### Ownership, and the two bugs it fixes

The layer state is global; a profile's language is not. Conflating them produces two concrete
failures, both found in review of the first draft:

- The Mac dropped a minute ago. You switch to the iPad and straight back. If switching away rewrote
  the Mac's timestamp, its minute-long absence would measure as two seconds. **Fixed by taking
  `link_down_at` only from the connection callback**, never from profile switching.
- The iPad is on `en`. You select the Mac profile, which is disconnected and remembers `ru`, and
  leave before it connects. The layer never became the Mac's, yet a naive save would write `en` into
  the Mac's slot. **Fixed by `owner`**: the language is saved for a profile only while that profile
  owns the layer state.

`owner` is set when a decision is applied for a profile, and cleared when we move to a profile for
which no decision has been made or when BLE stops being the selected transport. Saving requires an
`owner` to be set, and happens at the two moments listed under "When the language is saved" — an
ownership change and the owner's own disconnect.

### Deciding

When a profile becomes both active and connected, or when BLE becomes the selected transport again:

| condition | meaning | action |
|---|---|---|
| it is still the `owner` and `needs_reset` is clear | nothing happened to it; the layer already *is* its language | **nothing** |
| `needs_reset` set | it was away long enough to have slept and reset | clear to the default language, clear the flag, take ownership |
| otherwise | arriving at a host that stayed awake, or a hiccup | restore `lang_alt`, take ownership |

The first row matters more than it looks. If a profile never lost ownership, its stored `lang_alt`
may be older than the user's latest choice — they may have switched language since — so restoring
from memory would drag them back. Doing nothing is both correct and cheaper.

This rests on one assumption worth stating plainly: **a sleeping host drops the BLE connection.** A
host that slept while holding the link open would look like one that stayed awake, and its stale
language would be restored — no worse than today.

Two cases the threshold cannot separate, accepted rather than solved: a short sleep that still reset
the layout is missed, and a long radio loss from an awake host causes a false reset.

### When the language is saved

`lang_alt` is written for a profile at exactly two moments, both while that profile is the `owner`:

- when its link goes down, and
- when ownership is about to move elsewhere — another profile, or away from BLE entirely.

Saving only on ownership change is not enough: a profile can own the layer, have the user change
language under it, and then suffer a short outage without any ownership change. Without the
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

### Why nothing is written to flash

Deep sleep resets the board, which loses this state — and that is correct, because a deep sleep means
a long absence and the host will have reset too. Persisting it would survive exactly the case where
it is most likely to be wrong.

## Configuration

| symbol | meaning | default |
|---|---|---|
| `ZMK_LAYOUT_RESYNC` | enable the module | `y` |
| `ZMK_LAYOUT_RESYNC_ALT_LAYER` | the layer holding the alternate language | `1` (`ru`) |
| `ZMK_LAYOUT_RESYNC_DISCONNECT_MS` | outage above which the host is assumed to have reset | `10000` |

**There is nothing to configure per host.** Both target hosts reset their own layout on wake, macOS
and iPadOS alike, measured on the devices. So the reset branch is right on every profile.

An earlier draft carried a `RESET_PROFILES` bitmask so one host could be restore-only, against the
possibility that iPadOS preserved its layout. The measurement removed the reason, and the mask was
deleted rather than left with a permissive default, because it keyed on the **profile index** — which
changes when a device is re-paired, so a mask set today would point at the wrong host tomorrow. If a
host ever appears that does preserve its layout, key that behaviour on the **peer address**, which
the module already stores and compares in order to notice re-pairing.

`ZMK_LAYOUT_RESYNC` **must** depend on `ZMK_BLE` **and** on the central or non-split role. The two
excluded matrix entries fail for different reasons, and only both conditions together cover them:

- `op36_right` is a split peripheral, so `app/CMakeLists.txt:47`
  (`(NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL`) builds neither `keymap.c` nor `ble.c`.
- `settings_reset` is not split at all, so it *does* build `keymap.c`; what it lacks is `ble.c`,
  because its shield conf sets `CONFIG_ZMK_BLE=n`.

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

**iPadOS behaviour after sleep was the one thing that could have made this design wrong, and it was
measured rather than assumed.** An earlier draft claimed the risk was free — "one manual switch, same
as today" — which was untrue: had iPadOS preserved the Russian layout, the iPad would come back
consistent today and the reconnect branch would have *introduced* a desync. It does not preserve it.
Both hosts reset, so the branch is correct on both and no per-host configuration exists.

## Verification

**The device is the only place the BLE transitions and the OS behaviour can be seen.**
`native_posix_64` has no BLE, no second host and no profiles.

**The decision logic, however, is testable and will be tested.** It is extracted into a pure
**state-transition** function — (state, event, timestamp) → (new state, action) — with no Zephyr types
in its signature. Testing branch selection alone would miss the whole class of bugs review found,
which are about *what gets stored and who owns it*, so the tests assert the resulting state and not
just the action.

Sequences, each one a bug review found or a rule the design turns on:

- Mac drops, you switch to the iPad and back, Mac reconnects — the outage must measure the real
  minute, and switching must not rewrite `link_down_at`.
- iPad on `en`; select the disconnected Mac, which remembers `ru`; leave before it connects — the
  Mac's `lang_alt` must be untouched.
- A profile blinks for a second, then is selected a minute later — no reset: the verdict is decided at
  reconnect, not from `now`.
- A long outage followed by a short one before you return — `needs_reset` must survive the short one.
- A profile owns the layer, the user changes language, a short outage follows with no profile change —
  the restore must not drag the old language back.
- BLE/Mac on `ru` → USB → language changed to `en` → back to BLE — the Mac's memory must not acquire
  the USB-era language.
- Switch between two connected hosts — each side's language restored, threshold never consulted.
- An outage of exactly `ZMK_LAYOUT_RESYNC_DISCONNECT_MS` — the boundary, pinned so it cannot drift.
- A long outage on a **restore-only** profile — the language comes back, the reset never fires.
- A repeat notification after a reset has already been applied — nothing happens the second time.
- Profile cleared or re-paired while active — the whole history for that profile goes, `needs_reset`
  included, since the peer behind the index is now a different machine.

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
- **Two of three matrix entries lack what the module links against**, for different reasons —
  `op36_right` by role, `settings_reset` by `CONFIG_ZMK_BLE=n`. The Kconfig guard is not tidiness;
  without it CI breaks.
