# Layout Resync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep `op36_ruen`'s language layer in step with the host it is talking to, across host sleep and across BLE profile switches.

**Architecture:** A Zephyr module carried by this repository, in three layers. A pure state machine holds the per-profile language memory and decides what to do. An adapter core translates Bluetooth and ZMK events into it, reconciles after pairing and applies the result; every platform call it needs is behind a struct of function pointers. A ZMK binding fills that struct in and serialises the callbacks, and has no logic of its own. The first two layers have no Zephyr types and are unit tested on the host; only the binding is untestable here, which is why it is kept trivial.

**Tech Stack:** C11, Zephyr, ZMK v0.3.0, Kconfig, CMake. Host tests compiled with `cc`.

**Spec:** `docs/superpowers/specs/2026-09-08-layout-resync-design.md`

## Global Constraints

- ZMK is **v0.3.0**, pinned through `ergohaven-zmk`. Only public API: `zmk_keymap_layer_activate`, `zmk_keymap_layer_deactivate`, `zmk_keymap_layer_active`, `zmk_ble_profile_index`, `zmk_endpoints_selected`, `ZMK_LISTENER`/`ZMK_SUBSCRIPTION`, Zephyr's `BT_CONN_CB_DEFINE`.
- **Never call `zmk_keymap_layer_to()`.** It deactivates every layer (`keymap.c:214`) and would drop a held momentary layer — including `en_letters` under a held modifier.
- **Never send a keycode.** The host sets its own layout on wake; the firmware only has to agree.
- `ZMK_LAYOUT_RESYNC` **must** depend on `ZMK_BLE` **and** `(!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)`. Without both, `op36_right` (no `keymap.c`, no `ble.c` — role) and `settings_reset` (no `ble.c` — `CONFIG_ZMK_BLE=n`) fail to link.
- Nothing is written to flash.
- The repository is column-aligned in the keymap editor's format; **this plan touches no `.keymap` file**, so no re-rendering is involved.
- Commit messages: conventional commits, imperative, present tense, no attribution.
- **Do not push.** Tim pushes.

---

## File Structure

| file | responsibility |
|---|---|
| `zephyr/module.yml` | makes this repository a Zephyr module; the build workflow already looks for it |
| `module/CMakeLists.txt` | compiles the module sources when `ZMK_LAYOUT_RESYNC` is on |
| `module/Kconfig` | the four symbols and the build guard |
| `module/src/resync_state.h` | the pure state machine's types and API — no Zephyr headers |
| `module/src/resync_state.c` | the pure state machine |
| `module/src/resync_adapter.h` | the adapter core's API and its platform struct |
| `module/src/resync_adapter.c` | event translation, reconciliation and application — no Zephyr |
| `module/src/layout_resync.c` | ZMK binding: fills the platform struct in, routes callbacks, holds the mutex |
| `tests/resync-state/test_resync_state.c` | host-compiled tests for the state machine |
| `tests/resync-state/test_resync_adapter.c` | host-compiled tests for the adapter core |
| `tests/run.sh` | runs the host test alongside `check-en-letters.py` |

---

### Task 1: The state machine, with its tests

The whole of the decision logic, testable on the host with no Zephyr. Every bug review found in the
spec was about stored state rather than which branch ran, so the tests assert the resulting state as
well as the returned action.

**Files:**
- Create: `module/src/resync_state.h`
- Create: `module/src/resync_state.c`
- Test: `tests/resync-state/test_resync_state.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct resync_state`, `struct resync_event`, `enum resync_action`, `enum resync_event_kind`, `resync_init()`, `resync_handle()`, `resync_link_up()`. Task 3's adapter calls `resync_init`, `resync_handle` and `resync_link_up`.

- [ ] **Step 1: Write the header**

Create `module/src/resync_state.h`:

```c
/*
 * Pure decision logic for layout resync. No Zephyr types appear here, so this
 * compiles and is tested on the host. See
 * docs/superpowers/specs/2026-09-08-layout-resync-design.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RESYNC_MAX_PROFILES 8

enum resync_action {
    /* Leave the keymap alone. */
    RESYNC_ACTION_NONE = 0,
    /* Activate the alternate language layer. */
    RESYNC_ACTION_SET_ALT,
    /* Deactivate it, returning to the default language. */
    RESYNC_ACTION_CLEAR_ALT,
};

enum resync_event_kind {
    /* A host profile's link went down or came up. From the connection
       callbacks, which see every profile and not only the active one. */
    RESYNC_EV_LINK_DOWN,
    RESYNC_EV_LINK_UP,
    /* The active profile index is now `profile`. May repeat with no change. */
    RESYNC_EV_ACTIVE_PROFILE,
    /* The peer behind `profile` changed: pairing, or &bt BT_CLR. */
    RESYNC_EV_PROFILE_CLEARED,
    /* BLE became, or stopped being, the selected transport. */
    RESYNC_EV_BLE_SELECTED,
    RESYNC_EV_BLE_DESELECTED,
};

struct resync_event {
    enum resync_event_kind kind;
    /* Meaningful for LINK_DOWN, LINK_UP, ACTIVE_PROFILE, PROFILE_CLEARED. */
    uint8_t profile;
    /* Whether the alternate layer is active right now, as the caller sees it.
       Passed in so the state machine never has to ask the keymap. */
    bool alt_now;
    /* Milliseconds since boot, taken when the event happened and not when it
       is processed. */
    int64_t at;
    /* Whether BLE is the selected transport at this instant. Carried on every
       event because ZMK selects a transport before announcing it, so a callback
       can arrive while a remembered copy still says BLE. */
    bool ble_now;
};

struct resync_profile {
    /* The language remembered for this profile, and whether it was ever
       established. lang_known guards against adopting another profile's
       language for one we merely selected. */
    bool lang_alt;
    bool lang_known;
    bool link_up;
    /* Sticky: an outage longer than the threshold happened and has not been
       acted on. Set only when a link comes back up, cleared only when a reset
       is applied. */
    bool needs_reset;
    /* Whether a disconnect was ever observed for this profile. Time since boot
       is not an outage: without this the first connection after a minute of
       advertising measures a minute and arms a reset that never happened. */
    bool ever_down;
    int64_t link_down_at;
};

struct resync_state {
    struct resync_profile profiles[RESYNC_MAX_PROFILES];
    uint8_t profile_count;
    uint8_t active;
    /* Profile whose language the current layer state represents, or -1. */
    int owner;
    bool ble_selected;
    /* Bit N set means profile N may take the reset branch. */
    uint32_t reset_mask;
    int32_t threshold_ms;
    /* The outage measured by the call that is running right now, or -1 if that
       call measured nothing. Cleared on entry to every resync_handle, so a
       repeated notification cannot re-report an older measurement — possibly
       another profile's. Exposed only so the adapter can log it: the threshold
       is a guess and this is the data for tuning it. */
    int64_t last_outage_ms;
};

void resync_init(struct resync_state *s, uint8_t profile_count, uint32_t reset_mask,
                 int32_t threshold_ms);

enum resync_action resync_handle(struct resync_state *s, const struct resync_event *ev);

/* Whether the machine believes this profile's link is up. The adapter uses it
   to compare against reality after pairing, where a callback can be missed. */
bool resync_link_up(const struct resync_state *s, uint8_t profile);
```

- [ ] **Step 2: Write the failing tests**

Create `tests/resync-state/test_resync_state.c`. Every case is a sequence the spec names.

```c
/* Host-compiled tests for the layout resync state machine. Build and run:
     cc -std=c11 -Wall -Wextra -o /tmp/resync_test \
        module/src/resync_state.c tests/resync-state/test_resync_state.c && /tmp/resync_test
   tests/run.sh does this before any simulator case. */

#include <stdio.h>
#include <string.h>

#include "../../module/src/resync_state.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define MAC 0
#define IPAD 1
#define THRESHOLD 10000

static struct resync_state st;
/* What the transport looks like to the caller. Every event carries it, so these
   tests set it the way the adapter would. */
static bool ble_now = true;

static void setup(void) {
    ble_now = true;
    resync_init(&st, 3, 0xFFFFFFFF, THRESHOLD);
}

static enum resync_action ev(enum resync_event_kind kind, uint8_t profile, bool alt_now,
                             int64_t at) {
    struct resync_event e = {
        .kind = kind, .profile = profile, .alt_now = alt_now, .at = at, .ble_now = ble_now};
    return resync_handle(&st, &e);
}

/* Both hosts connected; arriving at one restores its own language and the
   threshold is never consulted. */
static void test_switch_between_connected_hosts(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 10);
    /* User goes to ru on the Mac, then switches to the iPad. */
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 100) == RESYNC_ACTION_CLEAR_ALT,
          "the iPad is unknown, so it gets the default and not the Mac's ru");
    CHECK(st.profiles[MAC].lang_alt, "the Mac's ru must have been saved on the way out");
    /* Back to the Mac: its ru comes back. */
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 200) == RESYNC_ACTION_SET_ALT,
          "returning to the Mac restores ru");
}

/* Review counterexample: switching away must not rewrite link_down_at. */
static void test_switch_does_not_rewrite_outage(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_LINK_DOWN, MAC, true, 1000);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 2000);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 3000);
    CHECK(ev(RESYNC_EV_LINK_UP, MAC, false, 61000) == RESYNC_ACTION_CLEAR_ALT,
          "the outage is 60s measured from the real disconnect, so the Mac resets");
}

/* Review counterexample: a profile selected but never connected must not
   inherit the language that was on screen. */
static void test_selected_but_never_connected_keeps_its_memory(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_LINK_DOWN, MAC, true, 10);
    /* The Mac remembers ru. Go to the iPad, which is on en. */
    ev(RESYNC_EV_LINK_UP, IPAD, false, 20);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 30);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 40); /* Mac still down: no decision */
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, false, 50);
    CHECK(st.profiles[MAC].lang_alt, "the Mac's ru must survive being selected while down");
}

/* A one second blink, selected a minute later, must not reset. */
static void test_blink_then_selected_later(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 1000);
    ev(RESYNC_EV_LINK_UP, MAC, false, 2000);
    CHECK(!st.profiles[MAC].needs_reset, "a 1s outage is under the threshold");
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 62000) == RESYNC_ACTION_SET_ALT,
          "arriving a minute later restores ru, it does not reset");
}

/* A long outage followed by a short one before returning: the verdict sticks. */
static void test_needs_reset_is_sticky(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 100);
    ev(RESYNC_EV_LINK_UP, MAC, false, 100 + 20000);
    CHECK(st.profiles[MAC].needs_reset, "20s sets the verdict");
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 30000);
    ev(RESYNC_EV_LINK_UP, MAC, false, 30500);
    CHECK(st.profiles[MAC].needs_reset, "a later 500ms outage must not clear it");
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 40000) == RESYNC_ACTION_CLEAR_ALT,
          "the sleep is acted on when we come back");
}

/* The owner never lost ownership, so its stored language may be older than the
   user's latest choice. Restoring would drag them back. */
static void test_short_outage_under_owner_changes_nothing(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 0); /* owner, on en */
    /* User switches to ru. A one second hiccup follows. */
    ev(RESYNC_EV_LINK_DOWN, MAC, true, 1000);
    CHECK(ev(RESYNC_EV_LINK_UP, MAC, true, 1500) == RESYNC_ACTION_NONE,
          "still the owner with no verdict: do nothing at all");
    CHECK(st.profiles[MAC].lang_alt, "and the snapshot on disconnect kept ru");
}

/* USB takes the layer away and gives it back; the Mac's memory must not learn
   the language chosen while USB was driving. */
static void test_usb_round_trip(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0); /* Mac owns ru */
    ble_now = false;
    ev(RESYNC_EV_BLE_DESELECTED, 0, true, 100);
    CHECK(st.owner < 0, "nobody owns the layer while USB drives it");
    /* The user types on USB and switches to en. No BLE event describes it. */
    ble_now = true;
    CHECK(ev(RESYNC_EV_BLE_SELECTED, 0, false, 5000) == RESYNC_ACTION_SET_ALT,
          "returning to BLE restores the Mac's ru");
    CHECK(st.profiles[MAC].lang_alt, "and the USB-era en was never recorded for the Mac");
}

/* Observing the transport late does the deselect transition itself, so the
   layer cannot be attributed to the wrong profile afterwards. */
static void test_late_transport_observation_releases_ownership(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0); /* Mac owns ru */

    /* USB is live but no event has said so yet; the next one carries it. */
    ble_now = false;
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 100) == RESYNC_ACTION_NONE,
          "nothing is applied while USB drives the layer");
    CHECK(st.owner < 0, "and nothing takes ownership");
    CHECK(st.profiles[MAC].lang_alt, "the Mac's ru was saved on the way out");
    CHECK(!st.profiles[IPAD].lang_known, "the iPad learned nothing that was not its own");
}

/* The boundary itself, pinned so it cannot drift. */
static void test_threshold_boundary(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 1);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 1000);
    ev(RESYNC_EV_LINK_UP, MAC, false, 1000 + THRESHOLD);
    CHECK(st.profiles[MAC].needs_reset, "exactly the threshold counts as a reset");

    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 1);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 1000);
    ev(RESYNC_EV_LINK_UP, MAC, false, 1000 + THRESHOLD - 1);
    CHECK(!st.profiles[MAC].needs_reset, "one millisecond under does not");
}

/* A profile outside the mask is restore-only however long it was away. */
static void test_restore_only_profile(void) {
    /* Not setup(): this case needs its own mask. Reset the transport by hand,
       or it inherits whatever the previous case left behind. */
    ble_now = true;
    resync_init(&st, 3, 1u << MAC, THRESHOLD); /* only the Mac may reset */
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 10);
    ev(RESYNC_EV_LINK_DOWN, IPAD, false, 100);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 100 + 600000);
    CHECK(!st.profiles[IPAD].needs_reset, "ten minutes away must not arm a reset here");
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, false, 700000) == RESYNC_ACTION_SET_ALT,
          "its language is still restored");
}

/* An unknown profile must not inherit the language on screen. */
static void test_unknown_profile_takes_the_default(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 0);
    /* The Mac is on ru. The iPad has never been seen. */
    ev(RESYNC_EV_LINK_UP, IPAD, true, 10);
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 20) == RESYNC_ACTION_CLEAR_ALT,
          "the first visit to a host gives the default language");
    CHECK(!st.profiles[IPAD].lang_alt, "and that is what gets remembered for it");
}

/* Uptime is not an outage. */
static void test_first_connection_is_not_an_outage(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 0);
    /* The keyboard advertised for a minute before the host showed up. */
    ev(RESYNC_EV_LINK_UP, MAC, false, 60000);
    CHECK(!st.profiles[MAC].needs_reset, "never having been connected is not a long outage");
    CHECK(st.last_outage_ms < 0, "and there is no outage to report");
}

/* A repeated notification must not re-report a measurement, least of all one
   belonging to a different profile. */
static void test_repeat_link_up_reports_no_outage(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 0);

    ev(RESYNC_EV_LINK_DOWN, MAC, false, 1000);
    ev(RESYNC_EV_LINK_UP, MAC, false, 2000);
    CHECK(st.last_outage_ms == 1000, "the Mac's own outage is measured");

    ev(RESYNC_EV_LINK_DOWN, IPAD, false, 3000);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 63000);
    CHECK(st.last_outage_ms == 60000, "so is the iPad's");

    ev(RESYNC_EV_LINK_UP, MAC, false, 64000);
    CHECK(st.last_outage_ms < 0,
          "a repeat with nothing to measure must not inherit the iPad's minute");
}

/* The adapter may report the same transition twice. */
static void test_repeat_notification_is_harmless(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 100);
    ev(RESYNC_EV_LINK_UP, MAC, false, 100 + 20000);
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 30000) == RESYNC_ACTION_CLEAR_ALT,
          "the reset runs once");
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 30001) == RESYNC_ACTION_NONE,
          "and the repeat does nothing");
}

/* Re-pairing puts a different machine behind the index. */
static void test_profile_cleared_drops_history(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10);
    ev(RESYNC_EV_LINK_DOWN, MAC, false, 100);
    ev(RESYNC_EV_LINK_UP, MAC, false, 100 + 20000);
    ev(RESYNC_EV_PROFILE_CLEARED, MAC, false, 25000);
    CHECK(!st.profiles[MAC].needs_reset, "the verdict belonged to the old peer");
    CHECK(!st.profiles[MAC].lang_known, "so did the language");
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"switch_between_connected_hosts", test_switch_between_connected_hosts},
        {"switch_does_not_rewrite_outage", test_switch_does_not_rewrite_outage},
        {"selected_but_never_connected_keeps_its_memory",
         test_selected_but_never_connected_keeps_its_memory},
        {"blink_then_selected_later", test_blink_then_selected_later},
        {"needs_reset_is_sticky", test_needs_reset_is_sticky},
        {"short_outage_under_owner_changes_nothing", test_short_outage_under_owner_changes_nothing},
        {"usb_round_trip", test_usb_round_trip},
        {"threshold_boundary", test_threshold_boundary},
        {"late_transport_observation_releases_ownership",
         test_late_transport_observation_releases_ownership},
        {"restore_only_profile", test_restore_only_profile},
        {"unknown_profile_takes_the_default", test_unknown_profile_takes_the_default},
        {"first_connection_is_not_an_outage", test_first_connection_is_not_an_outage},
        {"repeat_link_up_reports_no_outage", test_repeat_link_up_reports_no_outage},
        {"repeat_notification_is_harmless", test_repeat_notification_is_harmless},
        {"profile_cleared_drops_history", test_profile_cleared_drops_history},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("%s\n", cases[i].name);
        cases[i].fn();
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run:

```bash
cc -std=c11 -Wall -Wextra -o /tmp/resync_test \
   module/src/resync_state.c tests/resync-state/test_resync_state.c && /tmp/resync_test
```

Expected: FAIL — `module/src/resync_state.c` does not exist, so the compile fails.

- [ ] **Step 4: Write the implementation**

Create `module/src/resync_state.c`:

```c
#include "resync_state.h"

static void save_lang(struct resync_state *s, uint8_t p, bool alt) {
    s->profiles[p].lang_alt = alt;
    s->profiles[p].lang_known = true;
}

/* Apply a decision for the active profile, if one is due. Called from every
   event that could have made one due; doing nothing is the common answer. It
   needs nothing from the event itself: what is on screen only matters when a
   language is being saved, which the callers do. */
static enum resync_action decide(struct resync_state *s) {
    if (!s->ble_selected) {
        return RESYNC_ACTION_NONE;
    }

    uint8_t p = s->active;
    if (p >= s->profile_count || !s->profiles[p].link_up) {
        return RESYNC_ACTION_NONE;
    }

    /* Never lost ownership and nothing happened to the host: the layer already
       is this profile's language, and the stored copy may be older than the
       user's latest choice. */
    if (s->owner == (int)p && !s->profiles[p].needs_reset) {
        return RESYNC_ACTION_NONE;
    }

    s->owner = (int)p;

    if (s->profiles[p].needs_reset) {
        s->profiles[p].needs_reset = false;
        save_lang(s, p, false);
        return RESYNC_ACTION_CLEAR_ALT;
    }

    if (!s->profiles[p].lang_known) {
        /* Nothing is known about this host, so the default language stands.
           Adopting what is on screen would hand it the *previous* host's
           language, which is how a first visit to the iPad used to inherit the
           Mac's ru. */
        save_lang(s, p, false);
        return RESYNC_ACTION_CLEAR_ALT;
    }

    return s->profiles[p].lang_alt ? RESYNC_ACTION_SET_ALT : RESYNC_ACTION_CLEAR_ALT;
}

bool resync_link_up(const struct resync_state *s, uint8_t profile) {
    return profile < s->profile_count && s->profiles[profile].link_up;
}

void resync_init(struct resync_state *s, uint8_t profile_count, uint32_t reset_mask,
                 int32_t threshold_ms) {
    for (unsigned i = 0; i < RESYNC_MAX_PROFILES; i++) {
        s->profiles[i].lang_alt = false;
        s->profiles[i].lang_known = false;
        s->profiles[i].link_up = false;
        s->profiles[i].needs_reset = false;
        s->profiles[i].ever_down = false;
        s->profiles[i].link_down_at = 0;
    }
    s->profile_count = profile_count > RESYNC_MAX_PROFILES ? RESYNC_MAX_PROFILES : profile_count;
    s->active = 0;
    s->owner = -1;
    s->ble_selected = false;
    s->reset_mask = reset_mask;
    s->threshold_ms = threshold_ms;
    s->last_outage_ms = -1;
}

enum resync_action resync_handle(struct resync_state *s, const struct resync_event *ev) {
    uint8_t p = ev->profile;

    /* Only this call's own measurement may be reported. */
    s->last_outage_ms = -1;

    /* Reconcile the transport first. Deciding and then refusing to apply would
       leave the layer behind the state: ownership would move, a language would
       be remembered and needs_reset cleared, with nothing written to the
       keymap. decide() returns before it mutates anything when BLE is not
       selected, so this has to be right before it runs. */
    if (ev->ble_now != s->ble_selected) {
        if (!ev->ble_now && s->owner >= 0) {
            /* Leaving BLE: the layer is still the owner's, so save it. */
            save_lang(s, (uint8_t)s->owner, ev->alt_now);
            s->owner = -1;
        }
        s->ble_selected = ev->ble_now;
    }

    switch (ev->kind) {
    case RESYNC_EV_LINK_DOWN:
        if (p >= s->profile_count) {
            return RESYNC_ACTION_NONE;
        }
        if (s->profiles[p].link_up) {
            s->profiles[p].link_up = false;
            s->profiles[p].ever_down = true;
            s->profiles[p].link_down_at = ev->at;
        }
        /* Snapshot the owner's language before the link is gone: it may change
           before we hear about this profile again. */
        if (s->owner == (int)p) {
            save_lang(s, p, ev->alt_now);
        }
        return RESYNC_ACTION_NONE;

    case RESYNC_EV_LINK_UP:
        if (p >= s->profile_count) {
            return RESYNC_ACTION_NONE;
        }
        if (!s->profiles[p].link_up) {
            if (s->profiles[p].ever_down) {
                int64_t outage = ev->at - s->profiles[p].link_down_at;
                s->last_outage_ms = outage;
                if (outage >= s->threshold_ms && (s->reset_mask & (1u << p))) {
                    /* Decided here, not at decision time, so it measures the
                       outage and not the time since. Sticky: only ever set. */
                    s->profiles[p].needs_reset = true;
                }
            } else {
                /* First time this profile has ever connected: there is no
                   outage to measure, only uptime. */
                s->last_outage_ms = -1;
            }
            s->profiles[p].link_up = true;
        }
        return decide(s);

    case RESYNC_EV_ACTIVE_PROFILE:
        if (p >= s->profile_count) {
            return RESYNC_ACTION_NONE;
        }
        if (p != s->active) {
            if (s->owner == (int)s->active) {
                save_lang(s, s->active, ev->alt_now);
            }
            s->owner = -1;
            s->active = p;
        }
        return decide(s);

    case RESYNC_EV_PROFILE_CLEARED:
        if (p >= s->profile_count) {
            return RESYNC_ACTION_NONE;
        }
        s->profiles[p].lang_alt = false;
        s->profiles[p].lang_known = false;
        s->profiles[p].link_up = false;
        s->profiles[p].needs_reset = false;
        s->profiles[p].ever_down = false;
        s->profiles[p].link_down_at = 0;
        if (s->owner == (int)p) {
            s->owner = -1;
        }
        return RESYNC_ACTION_NONE;

    case RESYNC_EV_BLE_DESELECTED:
        /* The reconcile above has already saved and released ownership if this
           is the first event to observe the change. Arriving second is normal
           and must do nothing. */
        return RESYNC_ACTION_NONE;

    case RESYNC_EV_BLE_SELECTED:
        return decide(s);
    }

    return RESYNC_ACTION_NONE;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:

```bash
cc -std=c11 -Wall -Wextra -o /tmp/resync_test \
   module/src/resync_state.c tests/resync-state/test_resync_state.c && /tmp/resync_test
```

Expected: PASS — every case listed, then `all checks passed`. No compiler warnings.

- [ ] **Step 6: Wire the test into the suite**

Modify `tests/run.sh`. Find this block, added when `check-en-letters.py` arrived:

```sh
# en_letters is a hand-kept copy of en; nothing in the devicetree enforces it.
python3 "$REPO/tests/check-en-letters.py"
```

Replace it with:

```sh
# en_letters is a hand-kept copy of en; nothing in the devicetree enforces it.
python3 "$REPO/tests/check-en-letters.py"

# The resync state machine is plain C with no Zephyr in it, so it runs here
# rather than in the simulator, which has no BLE to exercise it with.
cc -std=c11 -Wall -Wextra -o "${TMPDIR:-/tmp}/resync_test" \
    "$REPO/module/src/resync_state.c" "$REPO/tests/resync-state/test_resync_state.c"
"${TMPDIR:-/tmp}/resync_test" > /dev/null
```

- [ ] **Step 7: Run the whole suite**

Run: `./tests/run.sh`
Expected: no `FAILED` lines. The C test runs silently before any simulator case; break it deliberately once (change a `CHECK` to something false) to confirm the runner stops, then restore it.

- [ ] **Step 8: Commit**

```bash
git add module/src/resync_state.h module/src/resync_state.c \
        tests/resync-state/test_resync_state.c tests/run.sh
git commit -m "feat: add the layout resync state machine"
```

---

### Task 2: Module scaffolding that builds on every matrix entry

The state machine exists but nothing compiles it into the firmware. This task adds the Zephyr module
and its Kconfig guard, with a stub for the adapter, and puts the guard under test: the symbol keeps
its `default y`, so it must resolve to `y` on `op36_left` and to `n` on the other two by its
dependencies alone.

**Files:**
- Create: `zephyr/module.yml`
- Create: `module/CMakeLists.txt`
- Create: `module/Kconfig`

**Interfaces:**
- Consumes: `module/src/resync_state.c` from Task 1.
- Produces: `CONFIG_ZMK_LAYOUT_RESYNC`, `CONFIG_ZMK_LAYOUT_RESYNC_ALT_LAYER`, `CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS`, `CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES`. Task 3's source is added to the build by `module/CMakeLists.txt`.

- [ ] **Step 1: Declare the module**

Create `zephyr/module.yml`:

```yaml
# Makes this repository a Zephyr module. .github/workflows/build-user-config.yml
# looks for exactly this file and, finding it, passes
# -DZMK_EXTRA_MODULES=<repo root> to the build.
build:
  cmake: module
  kconfig: module/Kconfig
```

- [ ] **Step 2: Write the Kconfig**

Create `module/Kconfig`:

```kconfig
menuconfig ZMK_LAYOUT_RESYNC
    bool "Keep the language layer in step with the connected host"
    default y
    depends on ZMK_BLE
    depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL
    help
      Remembers which language each BLE profile was left on and restores it
      when that host comes back, and returns to the default language when a
      host has been away long enough to have slept and reset its own layout.

      Both dependencies are load bearing. app/CMakeLists.txt builds keymap.c
      and ble.c only for a central or non-split target, so op36_right has
      neither; settings_reset is not split and does build keymap.c, but sets
      CONFIG_ZMK_BLE=n and so has no ble.c. Dropping either dependency breaks
      the link on one of them.

if ZMK_LAYOUT_RESYNC

config ZMK_LAYOUT_RESYNC_ALT_LAYER
    int "Layer holding the alternate language"
    default 1
    help
      The layer that is raised for the second language. On op36_ruen this is
      1, the ru layer. The design assumes exactly two languages.

config ZMK_LAYOUT_RESYNC_DISCONNECT_MS
    int "Outage above which the host is assumed to have reset"
    default 10000
    help
      A link that comes back after longer than this is treated as a host that
      slept and reset its own layout. Shorter outages are radio hiccups and
      restore whatever language the profile was left on. A starting guess:
      every decision is logged with the measured outage so it can be tuned.

config ZMK_LAYOUT_RESYNC_RESET_PROFILES
    hex "Bitmask of profiles allowed to reset"
    default 0xFFFFFFFF
    help
      Bit N set means profile N may take the reset branch. A profile outside
      the mask is restore-only: its language is remembered and restored, but a
      long outage never clears it to the default. Clear the bit for any host
      that is known, or not yet known, to preserve its own layout across sleep.

endif # ZMK_LAYOUT_RESYNC
```

- [ ] **Step 3: Write the CMakeLists**

Create `module/CMakeLists.txt`:

```cmake
if(CONFIG_ZMK_LAYOUT_RESYNC)
  zephyr_library_named(layout_resync)
  # app adds its own headers with target_include_directories(app PRIVATE include),
  # and a separately named library does not inherit that. Without this line
  # <zmk/...> resolves only by accident, depending on what other modules set.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/resync_state.c)
  zephyr_library_sources(src/layout_resync.c)
endif()
```

- [ ] **Step 4: Create the adapter as a stub so the build has something to compile**

Create `module/src/layout_resync.c` with only enough to link. Task 4 replaces it with the real binding.

```c
/* ZMK binding for the layout resync module. Task 4 implements this. */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(layout_resync, CONFIG_ZMK_LOG_LEVEL);

static int layout_resync_init(void) {
    LOG_DBG("layout resync present");
    return 0;
}

SYS_INIT(layout_resync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 5: Leave the default in place**

Do **not** add `CONFIG_ZMK_LAYOUT_RESYNC=n` to `config/op36.conf`. With the feature switched off the
module is never compiled, so the Kconfig dependencies are never evaluated and the guard is not tested
at all — deleting it entirely would pass just the same. The symbol is `default y`, so leaving
`op36.conf` alone is what puts the guard under test: it must resolve to `y` on `op36_left` and to `n`
on the other two, by its dependencies alone.

The Task 2 stub is a no-op `SYS_INIT`, so a `y` here links and does nothing.

- [ ] **Step 6: Verify the simulator suite is unaffected**

Run: `./tests/run.sh`
Expected: no `FAILED` lines. The simulator does not receive `ZMK_EXTRA_MODULES`, so this proves only that nothing was broken — the module itself is not exercised here.

- [ ] **Step 7: Commit and let CI build it**

```bash
git add zephyr/module.yml module/CMakeLists.txt module/Kconfig \
        module/src/layout_resync.c config/op36.conf
git commit -m "build: carry a Zephyr module for layout resync"
```

Then ask Tim to push, and check the run:

```bash
gh run list --limit 1
gh run view <run-id> --json conclusion --jq .conclusion
```

Expected: `success` for all three matrix entries. Then check the guard actually resolved as intended,
because a green build alone does not show it. The workflow dumps each build's fully resolved
`zephyr/.config`, and every line carries a job name, so scope the grep per build:

```bash
gh run view <run-id> --log > /tmp/ci.log
for job in op36_left op36_right settings_reset; do
  printf '%s: ' "$job"
  grep "$job" /tmp/ci.log | grep -oE "CONFIG_ZMK_LAYOUT_RESYNC=[ny]" | sort -u | head -1
  printf '\n'
done
```

Expected: `y` for `op36_left`, and **nothing** for `op36_right` and `settings_reset` — the workflow
filters out `# ... is not set` lines, so an unset symbol prints empty. An empty result and a broken
pipeline look identical, so give each build a **positive** control, a symbol that must be there:

```bash
grep op36_left /tmp/ci.log | grep -oE "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y" | sort -u
grep op36_right /tmp/ci.log | grep -oE "CONFIG_ZMK_SPLIT=y" | sort -u
grep settings_reset /tmp/ci.log | grep -oE "CONFIG_ZMK_SETTINGS_RESET_ON_START=y" | sort -u
```

All three must print their symbol. Only then does an empty `ZMK_LAYOUT_RESYNC` result mean anything.
**This is the gate for the guard.** If
`ZMK_LAYOUT_RESYNC` shows up anywhere but `op36_left`, or if a build fails to link, the dependencies
are wrong and must be fixed before Task 3.

---

### Task 3: The adapter core, with its tests

Three of the four code defects review found lived in the adapter, so it does not stay untestable. The
part that decides *what to tell the state machine* — reconciling after pairing, translating
callbacks, guarding on the transport — is separated from the ZMK and Zephyr calls behind a small
platform struct, and tested on the host like the state machine. Task 4 is then a thin binding with no
logic in it.

**Files:**
- Create: `module/src/resync_adapter.h`
- Create: `module/src/resync_adapter.c`
- Test: `tests/resync-state/test_resync_adapter.c`
- Modify: `tests/run.sh`

**Interfaces:**
- Consumes: `resync_init()`, `resync_handle()`, `resync_link_up()`, `struct resync_event`, `enum resync_action` from Task 1.
- Produces: `struct resync_platform`, `resync_adapter_init()`, `resync_adapter_on_link()`, `resync_adapter_on_profile_changed()`, `resync_adapter_on_endpoint_changed()`. Task 4 calls exactly these four.

- [ ] **Step 1: Write the core header**

Create `module/src/resync_adapter.h`:

```c
/*
 * The adapter's logic, with every ZMK and Zephyr call behind a platform struct
 * so it can be tested on the host. Task 4's layout_resync.c fills the struct in
 * and does nothing else.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "resync_state.h"

/* A BLE address is 7 bytes: one type byte and six of address. Compared here as
   opaque bytes so no Bluetooth headers are needed. */
#define RESYNC_PEER_LEN 7

struct resync_platform {
    /* Is the alternate language layer active right now. */
    bool (*alt_active)(void);
    /* Activate or deactivate it. */
    void (*set_alt)(bool on);
    /* Is BLE the transport ZMK has selected *at this instant*. */
    bool (*ble_selected)(void);
    /* Is this profile's host link up, per ZMK. */
    bool (*profile_connected)(uint8_t profile);
    /* This profile's peer address, RESYNC_PEER_LEN bytes, or NULL. */
    const uint8_t *(*profile_peer)(uint8_t profile);
    uint8_t profile_count;
};

struct resync_adapter {
    const struct resync_platform *plat;
    struct resync_state state;
    uint8_t known_peer[RESYNC_MAX_PROFILES][RESYNC_PEER_LEN];
    bool peer_known[RESYNC_MAX_PROFILES];
};

/* Every entry point takes the time the event happened. The caller reads the
   clock on entry to its callback, so a wait on the binding's mutex cannot be
   counted as part of an outage — which near the threshold would change the
   decision. */
void resync_adapter_init(struct resync_adapter *a, const struct resync_platform *plat,
                         uint32_t reset_mask, int32_t threshold_ms, int64_t at);

/* A host link went up or down. Called from the Bluetooth connection callbacks,
   which is the only place an outage may be measured from. */
void resync_adapter_on_link(struct resync_adapter *a, uint8_t profile, bool up, int64_t at);

/* The active profile changed, or a profile's peer was written. */
void resync_adapter_on_profile_changed(struct resync_adapter *a, uint8_t index, int64_t at);

/* ZMK selected a different transport. */
void resync_adapter_on_endpoint_changed(struct resync_adapter *a, int64_t at);

/* The outage the last call measured, or -1. Diagnostics only. */
int64_t resync_adapter_last_outage(const struct resync_adapter *a);
```

- [ ] **Step 2: Write the failing tests**

Create `tests/resync-state/test_resync_adapter.c`. Each case is a defect review reproduced.

```c
/* Host tests for the adapter core, with a fake platform. Build and run:
     cc -std=c11 -Wall -Wextra -Werror -o /tmp/resync_adapter_test \
        module/src/resync_state.c module/src/resync_adapter.c \
        tests/resync-state/test_resync_adapter.c && /tmp/resync_adapter_test */

#include <stdio.h>
#include <string.h>

#include "../../module/src/resync_adapter.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define MAC 0
#define IPAD 1
#define THRESHOLD 10000

/* The fake host. */
static bool fake_alt;
static int64_t fake_now;
static bool fake_ble;
static bool fake_connected[RESYNC_MAX_PROFILES];
static uint8_t fake_peer[RESYNC_MAX_PROFILES][RESYNC_PEER_LEN];
static bool fake_has_peer[RESYNC_MAX_PROFILES];
static int set_alt_calls;

static bool p_alt_active(void) { return fake_alt; }
static void p_set_alt(bool on) {
    fake_alt = on;
    set_alt_calls++;
}
static bool p_ble_selected(void) { return fake_ble; }
static bool p_profile_connected(uint8_t i) { return fake_connected[i]; }
static const uint8_t *p_profile_peer(uint8_t i) { return fake_has_peer[i] ? fake_peer[i] : NULL; }

static const struct resync_platform plat = {
    .alt_active = p_alt_active,
    .set_alt = p_set_alt,
    .ble_selected = p_ble_selected,
    .profile_connected = p_profile_connected,
    .profile_peer = p_profile_peer,
    .profile_count = 3,
};

static struct resync_adapter ad;

static void setup(void) {
    fake_alt = false;
    fake_now = 0;
    fake_ble = true;
    set_alt_calls = 0;
    memset(fake_connected, 0, sizeof(fake_connected));
    memset(fake_peer, 0, sizeof(fake_peer));
    memset(fake_has_peer, 0, sizeof(fake_has_peer));
    for (uint8_t i = 0; i < 3; i++) {
        fake_has_peer[i] = true;
        fake_peer[i][0] = (uint8_t)(0x10 + i);
    }
    resync_adapter_init(&ad, &plat, 0xFFFFFFFF, THRESHOLD, fake_now);
}

/* Reconciling must never fabricate a link transition for a profile whose peer
   did not change. Doing so used to swallow a real outage: a stale snapshot said
   "connected", a real disconnect had just been recorded, and the difference was
   papered over with a synthetic LINK_UP that reset link_down_at. */
static void test_reconcile_does_not_fabricate_transitions(void) {
    setup();
    fake_connected[MAC] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    fake_alt = true; /* user goes to ru on the Mac */

    /* A real disconnect, then a profile-changed event arrives while ZMK still
       reports the old connection state. */
    fake_now = 1000;
    resync_adapter_on_link(&ad, MAC, false, fake_now);
    fake_connected[MAC] = true; /* stale snapshot */
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);

    CHECK(!resync_link_up(&ad.state, MAC), "the real disconnect must stand");

    /* The real reconnect, a minute later, must still be measured. */
    fake_now = 61000;
    fake_connected[MAC] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    CHECK(resync_adapter_last_outage(&ad) == 60000, "the full outage is measured, not 0");
}

/* A peer that changed means a different machine behind the index: its history
   goes, and its link state is taken from ZMK because the connect callback for
   the new pairing may have been dropped. */
static void test_new_peer_clears_history_and_adopts_link(void) {
    setup();
    fake_connected[MAC] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    fake_alt = true;
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    fake_now = 1000;
    resync_adapter_on_link(&ad, MAC, false, fake_now);

    /* Paired with something else; ZMK already has it connected. */
    fake_peer[MAC][0] = 0x99;
    fake_connected[MAC] = true;
    fake_now = 2000;
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);

    CHECK(resync_link_up(&ad.state, MAC), "the missed connect is picked up from ZMK");
    CHECK(!ad.state.profiles[MAC].needs_reset, "the old peer's verdict is gone");
    CHECK(!ad.state.profiles[MAC].ever_down, "and so is its outage history");
}

/* ZMK sets the transport before announcing it, so a connection callback can
   arrive while our own copy still says BLE. The layer must not move then. */
static void test_no_layer_change_once_usb_is_selected(void) {
    setup();
    fake_connected[MAC] = true;
    fake_connected[IPAD] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    resync_adapter_on_link(&ad, IPAD, true, fake_now);
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    fake_alt = true;
    resync_adapter_on_profile_changed(&ad, IPAD, fake_now); /* iPad learns en */
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);  /* Mac restored to ru */
    CHECK(fake_alt, "the Mac is on ru");

    /* USB is chosen. ZMK has switched already but has not told us yet. */
    fake_ble = false;
    set_alt_calls = 0;
    resync_adapter_on_link(&ad, IPAD, false, fake_now);
    resync_adapter_on_link(&ad, IPAD, true, fake_now);
    resync_adapter_on_profile_changed(&ad, IPAD, fake_now);
    CHECK(set_alt_calls == 0, "nothing may touch the layer while USB is selected");
}

/* An action that cannot be applied must not be decided. ZMK selects the
   transport before announcing it, so events arrive in that window with USB
   already live; if the machine advanced anyway, ownership would move with
   nothing written and the next save would attribute one host's language to
   another. */
static void test_blocked_window_does_not_advance_state(void) {
    setup();
    fake_connected[MAC] = true;
    fake_connected[IPAD] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    resync_adapter_on_link(&ad, IPAD, true, fake_now);
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    fake_alt = true;
    resync_adapter_on_profile_changed(&ad, IPAD, fake_now);
    fake_alt = false;
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    CHECK(fake_alt, "the Mac is on ru and the iPad remembers en");

    /* USB is live; the announcement has not arrived. */
    fake_ble = false;
    set_alt_calls = 0;
    resync_adapter_on_profile_changed(&ad, IPAD, fake_now);
    CHECK(set_alt_calls == 0, "nothing is applied");
    CHECK(ad.state.owner < 0, "and nothing takes ownership either");

    resync_adapter_on_endpoint_changed(&ad, fake_now); /* the late notification */
    CHECK(ad.state.profiles[MAC].lang_alt, "the Mac keeps its ru");
    CHECK(!ad.state.profiles[IPAD].lang_alt, "and the iPad does not inherit it");
}

/* A reset that could not be applied must still be owed. */
static void test_blocked_reset_survives_the_window(void) {
    setup();
    fake_connected[MAC] = true;
    resync_adapter_on_link(&ad, MAC, true, 0);
    resync_adapter_on_profile_changed(&ad, MAC, 0);
    fake_alt = true;
    resync_adapter_on_profile_changed(&ad, MAC, 0);

    /* Away long enough to reset, but USB is live when it comes back. */
    resync_adapter_on_link(&ad, MAC, false, 1000);
    fake_ble = false;
    resync_adapter_on_link(&ad, MAC, true, 100000);
    CHECK(ad.state.profiles[MAC].needs_reset, "the verdict is still owed");

    fake_ble = true;
    set_alt_calls = 0;
    resync_adapter_on_endpoint_changed(&ad, 101000);
    CHECK(!fake_alt, "and is applied when BLE comes back");
    CHECK(set_alt_calls == 1, "exactly once");
}

/* The USB round trip, through the adapter rather than the bare machine. */
static void test_usb_round_trip(void) {
    setup();
    fake_connected[MAC] = true;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);
    fake_alt = true;
    resync_adapter_on_profile_changed(&ad, MAC, fake_now);

    fake_ble = false;
    resync_adapter_on_endpoint_changed(&ad, fake_now);
    fake_alt = false; /* the user types on USB in en */
    fake_ble = true;
    resync_adapter_on_endpoint_changed(&ad, fake_now);
    CHECK(fake_alt, "coming back to BLE restores the Mac's ru");
    CHECK(ad.state.profiles[MAC].lang_alt, "and the USB language was never recorded for it");
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"reconcile_does_not_fabricate_transitions", test_reconcile_does_not_fabricate_transitions},
        {"new_peer_clears_history_and_adopts_link", test_new_peer_clears_history_and_adopts_link},
        {"no_layer_change_once_usb_is_selected", test_no_layer_change_once_usb_is_selected},
        {"blocked_window_does_not_advance_state", test_blocked_window_does_not_advance_state},
        {"blocked_reset_survives_the_window", test_blocked_reset_survives_the_window},
        {"usb_round_trip", test_usb_round_trip},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("%s\n", cases[i].name);
        cases[i].fn();
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -o /tmp/resync_adapter_test \
   module/src/resync_state.c module/src/resync_adapter.c \
   tests/resync-state/test_resync_adapter.c && /tmp/resync_adapter_test
```

Expected: FAIL — `module/src/resync_adapter.c` does not exist.

- [ ] **Step 4: Write the core**

Create `module/src/resync_adapter.c`:

```c
#include <string.h>

#include "resync_adapter.h"

/* Build the event, hand it over and apply what comes back. The transport is
   read here, at the moment of applying, and never from a remembered copy: ZMK
   sets the selected transport before it announces the change, so a connection
   callback can arrive while our own copy still says BLE. Reading it late is
   what keeps a BLE decision off a USB session. */
static void feed(struct resync_adapter *a, enum resync_event_kind kind, uint8_t profile,
                 int64_t at) {
    struct resync_event ev = {
        .kind = kind,
        .profile = profile,
        .alt_now = a->plat->alt_active(),
        .at = at,
        .ble_now = a->plat->ble_selected(),
    };

    enum resync_action action = resync_handle(&a->state, &ev);

    /* No guard here on purpose. The transport is carried into the machine and
       consulted before it decides, so an action can only come back when it is
       allowed to be applied. Checking afterwards and refusing would leave the
       state ahead of the layer. */
    if (action != RESYNC_ACTION_NONE) {
        a->plat->set_alt(action == RESYNC_ACTION_SET_ALT);
    }
}

void resync_adapter_init(struct resync_adapter *a, const struct resync_platform *plat,
                         uint32_t reset_mask, int32_t threshold_ms, int64_t at) {
    a->plat = plat;
    resync_init(&a->state, plat->profile_count, reset_mask, threshold_ms);

    for (uint8_t i = 0; i < RESYNC_MAX_PROFILES; i++) {
        a->peer_known[i] = false;
        memset(a->known_peer[i], 0, RESYNC_PEER_LEN);
    }

    for (uint8_t i = 0; i < plat->profile_count; i++) {
        const uint8_t *peer = plat->profile_peer(i);
        if (peer) {
            memcpy(a->known_peer[i], peer, RESYNC_PEER_LEN);
            a->peer_known[i] = true;
        }
    }

    if (plat->ble_selected()) {
        feed(a, RESYNC_EV_BLE_SELECTED, 0, at);
    }
}

void resync_adapter_on_link(struct resync_adapter *a, uint8_t profile, bool up, int64_t at) {
    feed(a, up ? RESYNC_EV_LINK_UP : RESYNC_EV_LINK_DOWN, profile, at);
}

void resync_adapter_on_profile_changed(struct resync_adapter *a, uint8_t index, int64_t at) {
    /* Only a profile whose peer changed is reconciled. Comparing every profile
       against its remembered link state races the connection callbacks: a
       snapshot taken a moment ago can contradict a disconnect that has just
       been recorded, and "correcting" it fabricates a transition that resets
       the outage clock and swallows the real outage. A changed peer is the one
       case where a callback is known to have been unusable, because at pairing
       the connect fires before the address is stored and the profile cannot be
       named yet. */
    for (uint8_t i = 0; i < a->plat->profile_count; i++) {
        const uint8_t *peer = a->plat->profile_peer(i);
        if (!peer) {
            continue;
        }

        bool changed = !a->peer_known[i] || memcmp(a->known_peer[i], peer, RESYNC_PEER_LEN) != 0;
        if (!changed) {
            continue;
        }

        memcpy(a->known_peer[i], peer, RESYNC_PEER_LEN);
        a->peer_known[i] = true;

        /* A different machine is behind this index now. */
        feed(a, RESYNC_EV_PROFILE_CLEARED, i, at);

        /* Its history is gone, so adopting ZMK's view of the link cannot
           swallow an outage: there is none to swallow. */
        if (a->plat->profile_connected(i)) {
            feed(a, RESYNC_EV_LINK_UP, i, at);
        }
    }

    feed(a, RESYNC_EV_ACTIVE_PROFILE, index, at);
}

void resync_adapter_on_endpoint_changed(struct resync_adapter *a, int64_t at) {
    feed(a, a->plat->ble_selected() ? RESYNC_EV_BLE_SELECTED : RESYNC_EV_BLE_DESELECTED, 0, at);
}

int64_t resync_adapter_last_outage(const struct resync_adapter *a) { return a->state.last_outage_ms; }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run the command from Step 3.
Expected: PASS — six cases, then `all checks passed`, with no warnings under `-Werror`.

- [ ] **Step 6: Wire it into the suite and put the state machine test under -Werror too**

Modify `tests/run.sh`. Replace the block added in Task 1 with:

```sh
# en_letters is a hand-kept copy of en; nothing in the devicetree enforces it.
python3 "$REPO/tests/check-en-letters.py"

# The resync state machine and adapter core are plain C with no Zephyr in them,
# so they run here rather than in the simulator, which has no BLE to exercise
# them with.
cc -std=c11 -Wall -Wextra -Werror -o "${TMPDIR:-/tmp}/resync_test" \
    "$REPO/module/src/resync_state.c" "$REPO/tests/resync-state/test_resync_state.c"
"${TMPDIR:-/tmp}/resync_test" > /dev/null

cc -std=c11 -Wall -Wextra -Werror -o "${TMPDIR:-/tmp}/resync_adapter_test" \
    "$REPO/module/src/resync_state.c" "$REPO/module/src/resync_adapter.c" \
    "$REPO/tests/resync-state/test_resync_adapter.c"
"${TMPDIR:-/tmp}/resync_adapter_test" > /dev/null
```

- [ ] **Step 7: Run the whole suite**

Run: `./tests/run.sh`
Expected: no `FAILED` lines. Break one `CHECK` in each C test in turn to confirm the runner stops on
both, then restore them.

- [ ] **Step 8: Commit**

```bash
git add module/src/resync_adapter.h module/src/resync_adapter.c \
        tests/resync-state/test_resync_adapter.c tests/run.sh
git commit -m "feat: add the layout resync adapter core"
```

---

### Task 4: Bind the adapter to ZMK

Everything with logic in it is now behind Task 3. This file only fills in the platform struct, routes
Bluetooth and ZMK events into the four entry points, and serialises them.

**Files:**
- Modify: `module/src/layout_resync.c` (replace the Task 2 stub entirely)
- Modify: `module/CMakeLists.txt`
- Modify: `config/op36.conf`

**Interfaces:**
- Consumes: the four `resync_adapter_*` functions and `struct resync_platform` from Task 3.
- Produces: nothing.

- [ ] **Step 1: Add the new source to the build**

Modify `module/CMakeLists.txt`, adding one line beside the others:

```cmake
  zephyr_library_sources(src/resync_adapter.c)
```

- [ ] **Step 2: Write the binding**

Replace the contents of `module/src/layout_resync.c`:

```c
/*
 * Binds the layout resync adapter to ZMK. There is no logic here: the platform
 * struct wraps the ZMK calls, the callbacks route into resync_adapter, and one
 * mutex serialises them. See
 * docs/superpowers/specs/2026-09-08-layout-resync-design.md.
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>

#include "resync_adapter.h"

LOG_MODULE_REGISTER(layout_resync, CONFIG_ZMK_LOG_LEVEL);

#define ALT_LAYER ((zmk_keymap_layer_id_t)CONFIG_ZMK_LAYOUT_RESYNC_ALT_LAYER)

BUILD_ASSERT(sizeof(bt_addr_le_t) == RESYNC_PEER_LEN,
             "the adapter compares peers as RESYNC_PEER_LEN opaque bytes");

static struct resync_adapter adapter;
/* Recursive for the owning thread in Zephyr, so a nested entry cannot deadlock. */
static struct k_mutex lock;

static bool plat_alt_active(void) { return zmk_keymap_layer_active(ALT_LAYER); }

static void plat_set_alt(bool on) {
    LOG_INF("resync: %s the alternate language layer", on ? "raising" : "dropping");
    if (on) {
        zmk_keymap_layer_activate(ALT_LAYER);
    } else {
        zmk_keymap_layer_deactivate(ALT_LAYER);
    }
}

static bool plat_ble_selected(void) {
    return zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE;
}

static bool plat_profile_connected(uint8_t profile) { return zmk_ble_profile_is_connected(profile); }

static const uint8_t *plat_profile_peer(uint8_t profile) {
    return (const uint8_t *)zmk_ble_profile_address(profile);
}

static const struct resync_platform platform = {
    .alt_active = plat_alt_active,
    .set_alt = plat_set_alt,
    .ble_selected = plat_ble_selected,
    .profile_connected = plat_profile_connected,
    .profile_peer = plat_profile_peer,
    .profile_count = ZMK_BLE_PROFILE_COUNT,
};

/* The role filter is the one ble.c uses to ignore the split peripheral link.
   The clock is read here, on entry, and passed down: taking it after the mutex
   would fold a wait into the measured outage, and near the threshold that
   changes the decision. */
static void resync_connected(struct bt_conn *conn, uint8_t err) {
    int64_t at = k_uptime_get();
    struct bt_conn_info info;

    if (err || bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        /* Pairing: the address is not stored yet, so this connection cannot be
           named. resync_adapter_on_profile_changed picks it up when
           set_profile_address raises its event. */
        return;
    }

    k_mutex_lock(&lock, K_FOREVER);
    resync_adapter_on_link(&adapter, (uint8_t)profile, true, at);
    /* Read under the same lock: last_outage_ms is shared, and another event
       clears it or replaces it with a different profile's measurement. */
    int64_t outage = resync_adapter_last_outage(&adapter);
    k_mutex_unlock(&lock);

    if (outage >= 0) {
        /* The threshold is a guess; this is the data for tuning it. */
        LOG_INF("resync: profile %d back after %lld ms (threshold %d)", profile, (long long)outage,
                CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS);
    }
}

static void resync_disconnected(struct bt_conn *conn, uint8_t reason) {
    int64_t at = k_uptime_get();
    struct bt_conn_info info;

    ARG_UNUSED(reason);

    if (bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return;
    }

    k_mutex_lock(&lock, K_FOREVER);
    resync_adapter_on_link(&adapter, (uint8_t)profile, false, at);
    k_mutex_unlock(&lock);
}

BT_CONN_CB_DEFINE(resync_conn_callbacks) = {
    .connected = resync_connected,
    .disconnected = resync_disconnected,
};

static int resync_event_listener(const zmk_event_t *eh) {
    int64_t at = k_uptime_get();

    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
    if (profile_ev) {
        k_mutex_lock(&lock, K_FOREVER);
        resync_adapter_on_profile_changed(&adapter, profile_ev->index, at);
        k_mutex_unlock(&lock);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_endpoint_changed(eh)) {
        k_mutex_lock(&lock, K_FOREVER);
        resync_adapter_on_endpoint_changed(&adapter, at);
        k_mutex_unlock(&lock);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layout_resync, resync_event_listener);
ZMK_SUBSCRIPTION(layout_resync, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(layout_resync, zmk_endpoint_changed);

static int layout_resync_init(void) {
    k_mutex_init(&lock);
    resync_adapter_init(&adapter, &platform, CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES,
                        CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS, k_uptime_get());

    LOG_INF("resync: %d profiles, threshold %d ms, reset mask 0x%x", ZMK_BLE_PROFILE_COUNT,
            CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS, CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES);
    return 0;
}

SYS_INIT(layout_resync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 3: Keep the reset branch off everywhere for now**

Modify `config/op36.conf`, appending:

```
# Layout resync. Restoring a language is safe on any host; resetting one is only
# safe on a host known to reset its own layout after sleep, and neither has been
# checked yet. Task 5 turns on the bit for the Mac.
CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES=0x0
```

`CONFIG_ZMK_LAYOUT_RESYNC` itself is left alone: it is `default y` and its dependencies are what Task
2 put under test.

- [ ] **Step 4: Verify the simulator suite still passes**

Run: `./tests/run.sh`
Expected: no `FAILED` lines, both C tests included.

- [ ] **Step 5: Commit and build**

```bash
git add module/src/layout_resync.c module/CMakeLists.txt config/op36.conf
git commit -m "feat: keep the language layer with the host it is talking to"
```

Then ask for a push, and once it lands confirm the run is `success`. If it fails, read the failing
entry before changing anything:

```bash
gh run view <run-id> --log | grep -iE "error|undefined reference" | head -20
```

- [ ] **Step 6: Flash and confirm the restore half works**

Flash the left half only.

The obvious check — Mac on `ru`, go to the iPad, come back, type — **proves nothing**: it passes on
today's firmware too, because the global `ru` simply stays on. The two hosts have to be on
*different* languages, each consistent with its own host, and both have to be typed on.

1. Mac: switch to `ru` and type a Cyrillic word to confirm host and firmware agree.
2. Switch to the iPad's profile. Switch it to `en` and type a Latin word, confirming the same there.
3. Switch back to the Mac and type. Expected: Cyrillic, with no manual switch.
4. Switch to the iPad and type. Expected: Latin, with no manual switch.
5. Repeat 3 and 4 once more, so a single lucky state cannot be mistaken for the feature working.

Nothing should reset at any point: the mask is `0x0`. If a reset happens, the mask is not being read
as intended.

### Task 5: Turn the reset branch on for the Mac

Restoring is safe on any host. Resetting is only safe on a host known to reset its own layout, which
so far means the Mac. This task establishes which profile that is and enables exactly that bit.

**Files:**
- Modify: `config/op36.conf`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: `CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES` from Task 2.
- Produces: nothing.

- [ ] **Step 1: Find out which profile index the Mac occupies**

The keymap binds `&bt BT_SEL 0`, `1` and `2` on `adj` and says nothing about which host is which. Ask
Tim, or read it off the device: select a profile, check which machine the keyboard types into.

Record the answer in this step before continuing. Do not guess.

- [ ] **Step 2: Enable the bit for the Mac only**

Modify `config/op36.conf`. Replace the `CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES=0x0` line that Task 4
added:

| Mac is profile | mask value |
|---|---|
| 0 | `0x1` |
| 1 | `0x2` |
| 2 | `0x4` |

```
# Only the Mac's bit. It is known to reset its own layout on wake. Every other
# profile stays restore-only until checked the same way — in particular the
# iPad, where a reset would create a desync rather than fix one if iPadOS turns
# out to preserve the layout across sleep.
CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES=0x2
```

The value above assumes the Mac is profile 1. Use the table if Step 1 said otherwise.

- [ ] **Step 3: Commit and build**

```bash
git add config/op36.conf
git commit -m "feat: let the Mac's profile reset the language after a sleep"
```

Ask Tim to push and confirm CI is `success`.

- [ ] **Step 4: Flash and verify the reset half on the Mac**

Flash the left half. Then:

1. On the Mac, switch to `ru` and type a Cyrillic word to confirm both sides agree.
2. Close the lid, wait longer than `CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS`, reopen it.
3. Type the password.

Expected: Latin characters. Before this change the keyboard was still on `ru` and the password came
out scrambled.

4. Unlock, open an application and type again.

Expected: still Latin, and the firmware agrees with the host — the spec calls for the whole chain,
because the lock screen can force ASCII independently of what the session restores, so a correct
password proves less than it looks.

Then the short case. The steps above end with the Mac on `en`, so **set `ru` again and confirm both
sides agree** before testing anything — otherwise the check starts from a state it was not meant to.
Close and reopen the lid **inside** the threshold, then type.

Keep two expectations apart here. That the firmware did **not** reset is the thing this step tests,
and it must hold. What language the *host* comes back on is not controlled by us: a short sleep that
still reset the layout is a limitation the spec already accepts, so a mismatch there is a known gap
and not a failure of this change. Record which of the two you saw.

Finally, confirm the iPad is unaffected: switch to it, type, and see its own language restored rather
than reset.

- [ ] **Step 5: Check the iPad, and only then decide about its bit**

With the log available (`CONFIG_ZMK_USB_LOGGING` if it is not already on), on the iPad:

1. Switch to Russian and confirm in Notes that Cyrillic comes out.
2. Lock it, wait long enough that the log shows an outage over the threshold, unlock.
3. Type one letter in Notes.

If it comes back Latin, iPadOS resets its own layout and the iPad's bit can be added to the mask in a
follow-up commit. If it comes back Cyrillic, leave the bit clear — the module must not reset there.

Record the answer in CLAUDE.md either way.

- [ ] **Step 6: Update CLAUDE.md**

Two statements are now false and one section is missing.

In "What this repository is", the sentence "There is no application code, no test suite, and no local
toolchain checked in" must acknowledge the module. Replace with:

```markdown
A ZMK **user config** (`zmk-config`), forked from Ergohaven's. It is almost all devicetree keymaps,
Kconfig fragments and a build matrix, with one exception: `module/` is a small Zephyr module carried
by this repository, the only compiled code here. Firmware is produced by GitHub Actions.
```

Under "Simulating the keymap", extend the line describing what `tests/run.sh` does first:

```markdown
`./tests/run.sh` first runs two checks that need no simulator — `tests/check-en-letters.py`, that
layer 8 has not drifted from `en`, and the host-compiled `tests/resync-state/`, which exercises the
layout resync state machine that the simulator cannot reach for want of BLE — and then builds
`config/op36_ruen.keymap` for ZMK's `native_posix_64`
```

Add a new section after "The RU/EN dual-layout system", before "## ru_ext":

```markdown
## The layout resync module

`module/` is a Zephyr module this repository carries, enabled by `zephyr/module.yml`, which the build
workflow finds and turns into `-DZMK_EXTRA_MODULES`. It keeps the language layer with the host it is
talking to: each BLE profile's language is remembered and restored when you come back to it, and a
host that was away longer than `CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS` is assumed to have slept and
reset its own layout, so the firmware returns to the default language too.

`module/src/resync_state.c` holds the decision logic and `module/src/resync_adapter.c` the event
translation and reconciliation; neither has a Zephyr type in it, and both are tested by
`tests/resync-state/`, which `tests/run.sh` runs. `module/src/layout_resync.c` is only the binding —
it fills in a struct of platform calls and serialises the callbacks — because that layer cannot be
tested here, the simulator having no BLE.

**`CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES` is a bitmask and defaults to nothing in `op36.conf`.**
Restoring a language is safe on any host; resetting it is only safe on a host known to reset its own
layout on wake. Set a bit only after checking that host, and record the result here.

The design and everything checked while arriving at it are in
`docs/superpowers/specs/2026-09-08-layout-resync-design.md`. The three facts it turns on, all in
`app/src/ble.c`: `zmk_ble_prof_select()` does not disconnect the outgoing host, both connection
callbacks ignore anything that is not `BT_CONN_ROLE_PERIPHERAL`, and `zmk_ble_active_profile_changed`
coalesces through one work item and only ever describes the active profile — which is why the module
registers its own connection callbacks instead of relying on it.
```

- [ ] **Step 7: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: record the layout resync module and what it needs to be true"
```
