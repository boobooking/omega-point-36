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

## The signal

`zmk_ble_active_profile_changed` carries `uint8_t index` and is raised in exactly the three places
this design needs, and nowhere else:

- `zmk_ble_prof_select()` raises it when the active profile index actually changes, and returns early
  when it does not. It does **not** disconnect the outgoing host — it only calls
  `update_advertising()`, which manages advertising alone.
- `connected()` and `disconnected()` raise it for the active profile.

Both connection callbacks return early unless `info.role == BT_CONN_ROLE_PERIPHERAL`, and then gate
on `is_conn_active_profile(conn)`. The split peripheral link — where this half is the BLE central —
never reaches the event. That is the correctness property the whole design rests on: the module hears
about the host and only the host.

Because a profile switch leaves the outgoing connection up, "you switched devices" and "the host went
away" arrive as *different* events, and can be told apart without guessing from elapsed time.

## Design

Per profile, in RAM, `ZMK_BLE_PROFILE_COUNT` entries (`CONFIG_BT_MAX_PAIRED` minus
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS`, so 5 on this build):

- `alt_active` — was the alternate language layer active when we last left this profile
- `left_at` — `k_uptime_get()` when we left it

The module also tracks the previous profile index and the previous connection state, so each event
can be classified.

On every `zmk_ble_active_profile_changed`:

1. **Record.** If the index changed, or the profile went from connected to disconnected, store
   `alt_active` and `left_at` for the profile we are leaving.
2. **Decide**, when the now-active profile is connected. The discriminator is whether the link was
   already up when we arrived, because **host sleep drops the BLE link** — so a live connection is
   itself evidence that the host never went away:

| trigger | meaning | action |
|---|---|---|
| the profile is already connected when it becomes active | you moved to a host that has been awake the whole time | restore that profile's `alt_active` |
| the profile *becomes* connected, gap ≥ threshold | it was away long enough to have slept and reset | `zmk_keymap_layer_to(zmk_keymap_layer_default())` |
| the profile *becomes* connected, gap < threshold | radio hiccup, or you switched to it and it reconnected at once | restore that profile's `alt_active` |

`gap` is `now - left_at` for that profile: how long since we last had it. That covers switching to a
profile that happens to be disconnected as well, with no extra rule — it lands on the second or third
row according to how long we have been away from it.

"Restore" means `zmk_keymap_layer_to()` of either the alternate layer or the default one. Returning
to `zmk_keymap_layer_default()` rather than a literal 0 keeps the code correct if the default layer
ever moves, and avoids a magic number.

No keystroke is ever sent. On wake the host sets its own layout; the firmware only has to agree with
it. Sending Caps Lock here would desynchronise in the other direction.

The threshold is consulted **only** when a link comes up that was down. Switching between two hosts
that are both awake never reaches it, so "away on the iPad for three minutes" and "lid closed for
thirty seconds" are decided by different rules and never compete for one number.

This rests on one assumption worth stating plainly: **a sleeping host drops the BLE connection.** If
some host were to sleep while holding the link open, its wake would look like a plain profile switch
and the stale language would be restored. That is the same failure the user already lives with today,
so the assumption failing costs nothing new.

### Why nothing is written to flash

Deep sleep resets the board, which loses this state — and that is correct, because a deep sleep means
a long absence and the host will have reset too. Persisting it would survive exactly the case where
it is most likely to be wrong. CLAUDE.md already records this reasoning for layer state generally.

## Configuration

| symbol | meaning | default |
|---|---|---|
| `ZMK_LAYOUT_RESYNC` | enable the module | `y` |
| `ZMK_LAYOUT_RESYNC_ALT_LAYER` | the layer whose state is remembered per profile | `1` (`ru`) |
| `ZMK_LAYOUT_RESYNC_DISCONNECT_MS` | reconnect gap above which the host is assumed to have reset | `10000` |

10000 is a starting guess, not a measurement. The module logs the measured gap on every reconnect so
the value can be tuned from observation.

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
```

Everything it uses is public API: `zmk_keymap_layer_to`, `zmk_keymap_layer_default`,
`zmk_keymap_layer_active`, `zmk_ble_active_profile_is_connected`, and the
`ZMK_LISTENER` / `ZMK_SUBSCRIPTION` macros.

## Out of scope

Named so they are not mistaken for oversights:

- **USB.** No BLE profile exists in that mode and the keyboard never deep-sleeps on the cable, so the
  password case remains broken there. `usb_conn_state_changed` is the analogous signal if it is ever
  worth doing.
- **The host changing layout while awake** — menu bar, another application, a foreign shortcut. Still
  undetectable; this is cause 2 in CLAUDE.md's table and needs feedback the platform does not give.
- **The first connection to a profile after boot.** There is no memory yet, so the default layer
  stands. That is the right answer, but it is worth knowing it is not a decision.
- **iPadOS behaviour after sleep is unverified.** If iPadOS preserves the Russian layout across sleep,
  the reconnect branch will force `en` there wrongly. The cost is one manual switch, which is what
  already happens today, so this does not block. Scoping the behaviour per profile is the fix if it
  turns out to matter.

## Verification

**The simulator cannot exercise any of this.** `native_posix_64` is a single node with no BLE, so
there is no connection to drop and no profile to switch. This is the first code in the repository
that `./tests/run.sh` does not cover, and it should be treated as such rather than assumed safe
because the suite is green.

What stands in for it:

- The decision logic is a handful of lines with no timing subtlety, small enough to read whole.
- Every reconnect logs the measured gap and the branch taken, so device behaviour is observable
  rather than inferred.
- Device checks: lid close and reopen inside the threshold; lid close for longer; Mac to iPad and
  back inside a minute; a walk out of range and back.
- `tests/check-en-letters.py` and the existing suite still guard everything the module does not touch,
  and must stay green — the module changes no keymap behaviour.

## Risks

- **The threshold is a guess.** Tunable without code changes, and the log gives the data.
- **A host that sleeps without dropping the link** would be indistinguishable from one that stayed
  awake, and its stale language would be restored. No worse than today's behaviour.
- **First C in the repository.** CLAUDE.md's opening claim that there is no fork stays true — this is
  a module, not a patch to ZMK — but the statement that the repo holds no application code stops
  being true and must be updated.
