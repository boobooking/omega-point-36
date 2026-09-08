# Layout Resync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep `op36_ruen`'s language layer in step with the host it is talking to, across host sleep and across BLE profile switches.

**Architecture:** A Zephyr module carried by this repository. A pure state machine holds the per-profile language memory and decides what to do; a thin ZMK adapter feeds it Bluetooth connection callbacks, active-profile events and endpoint changes, and applies the one layer change it returns. The decision logic has no Zephyr types and is unit tested on the host; the adapter is not testable here and is kept as small as possible.

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
| `module/src/layout_resync.c` | ZMK adapter: connection callbacks, event subscriptions, layer application |
| `tests/resync-state/test_resync_state.c` | host-compiled tests for the state machine |
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
    /* Milliseconds since boot. */
    int64_t at;
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
    /* The most recent outage measured on a link coming back up, or -1 before
       the first one. Exposed only so the adapter can log it: the threshold is
       a starting guess and this is the data for tuning it. */
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

static void setup(void) { resync_init(&st, 3, 0xFFFFFFFF, THRESHOLD); }

static enum resync_action ev(enum resync_event_kind kind, uint8_t profile, bool alt_now,
                             int64_t at) {
    struct resync_event e = {.kind = kind, .profile = profile, .alt_now = alt_now, .at = at};
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
    ev(RESYNC_EV_BLE_DESELECTED, 0, true, 100);
    CHECK(st.owner < 0, "nobody owns the layer while USB drives it");
    /* The user types on USB and switches to en. No BLE event describes it. */
    CHECK(ev(RESYNC_EV_BLE_SELECTED, 0, false, 5000) == RESYNC_ACTION_SET_ALT,
          "returning to BLE restores the Mac's ru");
    CHECK(st.profiles[MAC].lang_alt, "and the USB-era en was never recorded for the Mac");
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
        {"restore_only_profile", test_restore_only_profile},
        {"unknown_profile_takes_the_default", test_unknown_profile_takes_the_default},
        {"first_connection_is_not_an_outage", test_first_connection_is_not_an_outage},
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
        if (s->owner >= 0) {
            save_lang(s, (uint8_t)s->owner, ev->alt_now);
        }
        s->owner = -1;
        s->ble_selected = false;
        return RESYNC_ACTION_NONE;

    case RESYNC_EV_BLE_SELECTED:
        s->ble_selected = true;
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
and its Kconfig guard, and proves the guard is right by leaving the module *off* — so a green CI here
means the scaffolding cannot break the two entries that must not have it.

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

Create `module/src/layout_resync.c` with only enough to link. Task 3 fills it in.

```c
/* ZMK adapter for the layout resync state machine. Task 3 implements this. */

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
filters out `# ... is not set` lines, so an unset symbol prints empty. Confirm the grep is working by
checking a symbol known to be set, as CLAUDE.md advises:

```bash
grep op36_right /tmp/ci.log | grep -oE "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=[ny]" | sort -u
```

That must also print nothing, while `op36_left` prints `y`. **This is the gate for the guard.** If
`ZMK_LAYOUT_RESYNC` shows up anywhere but `op36_left`, or if a build fails to link, the dependencies
are wrong and must be fixed before Task 3.

---

### Task 3: The ZMK adapter

Feeds the state machine and applies its decisions. This is the part the host tests cannot reach, so it
stays small and obeys the two rules the spec sets out: one step for state, layer and ownership; and a
repeat notification does nothing.

**Files:**
- Modify: `module/src/layout_resync.c` (replace the Task 2 stub entirely)
- Modify: `config/op36.conf`

**Interfaces:**
- Consumes: `resync_init()`, `resync_handle()`, `struct resync_event`, `enum resync_action` from Task 1.
- Produces: nothing other tasks consume.

- [ ] **Step 1: Write the adapter**

Replace the contents of `module/src/layout_resync.c`:

```c
/*
 * ZMK adapter for the layout resync state machine.
 *
 * The decision logic lives in resync_state.c and is tested on the host. This
 * file only turns Bluetooth and ZMK events into resync_event values and applies
 * the single action that comes back. See
 * docs/superpowers/specs/2026-09-08-layout-resync-design.md.
 *
 * Two rules from the spec govern everything here:
 *   - state update, layer application and the ownership change are one step,
 *     because a connection callback and a ZMK event can describe the same
 *     transition and arrive in either order;
 *   - a repeated notification must do nothing, which the state machine
 *     guarantees as long as it is told about every event exactly as it happens.
 */

#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>

#include "resync_state.h"

LOG_MODULE_REGISTER(layout_resync, CONFIG_ZMK_LOG_LEVEL);

#define ALT_LAYER ((zmk_keymap_layer_id_t)CONFIG_ZMK_LAYOUT_RESYNC_ALT_LAYER)

static struct resync_state state;
static struct k_mutex lock;
/* Our copy of each profile's peer, so a change can be noticed. */
static bt_addr_le_t known_peer[ZMK_BLE_PROFILE_COUNT];

static bool alt_is_active(void) { return zmk_keymap_layer_active(ALT_LAYER); }

static bool ble_is_selected(void) {
    return zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE;
}

/* Reading the layer, deciding and applying the result are one critical section.
   Split them and another handler can slip in between: saving a stale language
   for the new owner, or selecting USB after the decision was taken but before
   the layer moves, so a BLE decision lands on a USB session. Zephyr's k_mutex is
   recursive for the owning thread, so a layer change that somehow re-entered
   this path cannot deadlock. */
static void feed(enum resync_event_kind kind, uint8_t profile) {
    enum resync_action action;
    int64_t outage;
    uint8_t active;

    k_mutex_lock(&lock, K_FOREVER);

    struct resync_event ev = {
        .kind = kind,
        .profile = profile,
        .alt_now = alt_is_active(),
        .at = k_uptime_get(),
    };

    action = resync_handle(&state, &ev);
    outage = state.last_outage_ms;
    active = state.active;

    switch (action) {
    case RESYNC_ACTION_SET_ALT:
        zmk_keymap_layer_activate(ALT_LAYER);
        break;
    case RESYNC_ACTION_CLEAR_ALT:
        zmk_keymap_layer_deactivate(ALT_LAYER);
        break;
    case RESYNC_ACTION_NONE:
        break;
    }

    k_mutex_unlock(&lock);

    /* Logging is outside the lock: it is diagnostics, not state. */
    if (kind == RESYNC_EV_LINK_UP && outage >= 0) {
        /* The threshold is a guess; this is the data for tuning it. */
        LOG_INF("resync: profile %d back after %lld ms (threshold %d)", profile, (long long)outage,
                CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS);
    }
    if (action == RESYNC_ACTION_SET_ALT) {
        LOG_INF("resync: profile %d -> alternate language", active);
    } else if (action == RESYNC_ACTION_CLEAR_ALT) {
        LOG_INF("resync: profile %d -> default language", active);
    }
}

/* Bring the machine back in line with reality. Needed because a callback can be
   missed: at pairing, connected() fires before set_profile_address() stores the
   peer, so zmk_ble_profile_index() cannot name the profile yet and the link-up
   is dropped. This also notices a profile whose peer changed — pairing over an
   old one, or &bt BT_CLR — whose remembered language belongs to a different
   machine and must go. */
static void sync_profiles(void) {
    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        bt_addr_le_t *peer = zmk_ble_profile_address(i);

        if (peer && bt_addr_le_cmp(peer, &known_peer[i]) != 0) {
            bt_addr_le_copy(&known_peer[i], peer);
            LOG_INF("resync: profile %d has a new peer, dropping its history", i);
            feed(RESYNC_EV_PROFILE_CLEARED, i);
        }

        bool up_now = zmk_ble_profile_is_connected(i);

        k_mutex_lock(&lock, K_FOREVER);
        bool up_known = resync_link_up(&state, i);
        k_mutex_unlock(&lock);

        if (up_now != up_known) {
            LOG_DBG("resync: profile %d link state corrected to %d", i, up_now);
            feed(up_now ? RESYNC_EV_LINK_UP : RESYNC_EV_LINK_DOWN, i);
        }
    }
}

/* Our own connection callbacks, because ZMK's profile-changed event only ever
   describes the active profile and coalesces through a shared work item. The
   role filter is the same one ble.c uses to ignore the split peripheral link. */
static void resync_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err || bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return;
    }

    LOG_DBG("resync: profile %d link up", profile);
    feed(RESYNC_EV_LINK_UP, (uint8_t)profile);
}

static void resync_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct bt_conn_info info;

    ARG_UNUSED(reason);

    if (bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return;
    }

    LOG_DBG("resync: profile %d link down", profile);
    feed(RESYNC_EV_LINK_DOWN, (uint8_t)profile);
}

BT_CONN_CB_DEFINE(resync_conn_callbacks) = {
    .connected = resync_connected,
    .disconnected = resync_disconnected,
};

static int resync_event_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
    if (profile_ev) {
        /* This event is also raised by set_profile_address(), so pairing and
           BT_CLR arrive here. Reconcile first: peers may have changed and a
           link-up may have been dropped. */
        sync_profiles();
        feed(RESYNC_EV_ACTIVE_PROFILE, profile_ev->index);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_endpoint_changed *endpoint_ev = as_zmk_endpoint_changed(eh);
    if (endpoint_ev) {
        feed(ble_is_selected() ? RESYNC_EV_BLE_SELECTED : RESYNC_EV_BLE_DESELECTED, 0);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layout_resync, resync_event_listener);
ZMK_SUBSCRIPTION(layout_resync, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(layout_resync, zmk_endpoint_changed);

static int layout_resync_init(void) {
    k_mutex_init(&lock);
    resync_init(&state, ZMK_BLE_PROFILE_COUNT, CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES,
                CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS);

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        bt_addr_le_t *peer = zmk_ble_profile_address(i);
        if (peer) {
            bt_addr_le_copy(&known_peer[i], peer);
        }
    }

    if (ble_is_selected()) {
        feed(RESYNC_EV_BLE_SELECTED, 0);
    }

    LOG_INF("resync: %d profiles, threshold %d ms, reset mask 0x%x", ZMK_BLE_PROFILE_COUNT,
            CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS, CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES);
    return 0;
}

SYS_INIT(layout_resync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 2: Enable the module, with the reset branch off everywhere**

Modify `config/op36.conf`. Replace the two lines Task 2 added with:

```
# Layout resync. The reset branch is off for every profile until each host's
# behaviour after sleep is known: see the spec's "Out of scope". Restoring is
# safe everywhere, resetting is not.
CONFIG_ZMK_LAYOUT_RESYNC=y
CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES=0x0
```

- [ ] **Step 3: Verify the simulator suite still passes**

Run: `./tests/run.sh`
Expected: no `FAILED` lines. Again this proves only that nothing else broke.

- [ ] **Step 4: Commit and build**

```bash
git add module/src/layout_resync.c config/op36.conf
git commit -m "feat: keep the language layer with the host it is talking to"
```

Ask Tim to push, then:

```bash
gh run list --limit 1
gh run view <run-id> --json conclusion --jq .conclusion
```

Expected: `success`. If it fails, read the log for the failing entry before changing anything:

```bash
gh run view <run-id> --log | grep -iE "error|undefined reference" | head -20
```

- [ ] **Step 5: Flash and confirm the restore half works**

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

---

### Task 4: Turn the reset branch on for the Mac

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

Modify `config/op36.conf`. With the Mac on profile N, replace the mask line:

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

Then confirm the short case is untouched: close and reopen the lid **inside** the threshold, type,
and check `ru` is still there. And confirm the iPad is unaffected by this change: switch to it, type,
and see its own language still restored rather than reset.

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

`module/src/resync_state.c` holds all the decision logic with no Zephyr types in it and is tested by
`tests/resync-state/`, run by `tests/run.sh`. `module/src/layout_resync.c` is the adapter and cannot
be tested here — the simulator has no BLE — so it is deliberately thin.

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
