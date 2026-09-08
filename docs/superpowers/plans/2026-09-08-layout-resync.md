# Layout Resync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every BLE profile its own remembered input language, and restore it when you come back to that host.

**Architecture:** A Zephyr module carried by this repository, in three layers. A pure state machine holds the per-profile language memory and decides what to do. An adapter core translates Bluetooth and ZMK events into it, reconciles after pairing and applies the result; every platform call it needs is behind a struct of function pointers. A ZMK binding fills that struct in and serialises the callbacks, and has no logic of its own. The first two layers have no Zephyr types and are unit tested on the host; only the binding is untestable here, which is why it is kept trivial.

**Tech Stack:** C11, Zephyr, ZMK v0.3.0, Kconfig, CMake. Host tests compiled with `cc`.

**Spec:** `docs/superpowers/specs/2026-09-08-layout-resync-design.md`

## This is a migration, not a green field

**The module already exists in the repository**, built and passing CI at commit `2747874`, in the
shape the design had before the lock-screen measurement: it reset a host's language after an outage
longer than a threshold. The measurement removed that idea — after unlocking, both platforms restore
the language that was active before locking — so the reset branch, the threshold, the sticky verdict
and the outage clock all come out.

That makes the ordering different from a from-scratch build. The state machine, the adapter, the
tests, the Kconfig and `config/op36.conf` are **one change**: land them separately and the tree does
not compile, because the adapter calls `resync_init` with an argument that no longer exists and the
tests read fields that are gone. Task 1 is therefore a single coherent step, and the code blocks in
it are the content each file must end up with, not new files to create.

## Global Constraints

- ZMK is **v0.3.0**, pinned through `ergohaven-zmk`. Only public API: `zmk_keymap_layer_activate`, `zmk_keymap_layer_deactivate`, `zmk_keymap_layer_active`, `zmk_ble_profile_index`, `zmk_ble_profile_address`, `zmk_ble_profile_is_connected`, `zmk_endpoints_selected`, `ZMK_LISTENER`/`ZMK_SUBSCRIPTION`, Zephyr's `BT_CONN_CB_DEFINE`.
- **Never call `zmk_keymap_layer_to()`.** It deactivates every layer (`keymap.c:214`) and would drop a held momentary layer — including `en_letters` under a held modifier.
- **Never send a keycode.** The module only moves the firmware's own layer.
- **The module never resets a language, only restores one.** See the spec: the lock screen forces ASCII for the password field, but the session restores what it had, so resetting would be wrong for everything after the unlock.
- `ZMK_LAYOUT_RESYNC` **must** depend on `ZMK_BLE` **and** `(!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)`. Verified on CI run 34214748643.
- Nothing is written to flash.
- **This plan touches no `.keymap` file**, so no column re-rendering is involved.
- Commit messages: conventional commits, imperative, present tense, no attribution.
- **Do not push.** Tim pushes.

## File Structure

All of these exist already; the table says what each is for and which change.

| file | responsibility | changes |
|---|---|---|
| `zephyr/module.yml` | makes this repository a Zephyr module | no |
| `module/CMakeLists.txt` | compiles the module sources | no |
| `module/Kconfig` | the symbols and the build guard | **yes** — the threshold goes |
| `module/src/resync_state.h` | the pure state machine's types and API | **yes** |
| `module/src/resync_state.c` | the pure state machine | **yes** |
| `module/src/resync_adapter.h` | the adapter core's API and platform struct | **yes** |
| `module/src/resync_adapter.c` | event translation, reconciliation, application | **yes** |
| `module/src/layout_resync.c` | ZMK binding | **yes** — the outage logging goes |
| `tests/resync-state/test_resync_state.c` | host tests for the state machine | **yes** |
| `tests/resync-state/test_resync_adapter.c` | host tests for the adapter core | **yes** |
| `config/op36.conf` | Kconfig fragment | **yes** — a dead assignment must go |
| `tests/run.sh` | runs both host tests before the simulator | no |

---

### Task 1: Remove the reset machinery

One commit. The pieces cannot land separately: the adapter calls `resync_init` with a threshold the
state machine no longer takes, the tests read `needs_reset` and `ever_down` which no longer exist, and
`config/op36.conf` assigns a Kconfig symbol that is about to be deleted — which stops the Zephyr
configuration outright with `Aborting due to Kconfig warnings`, not merely warns.

**Files:**
- Modify: `module/src/resync_state.h`, `module/src/resync_state.c`
- Modify: `module/src/resync_adapter.h`, `module/src/resync_adapter.c`
- Modify: `module/src/layout_resync.c`, `module/Kconfig`, `config/op36.conf`
- Test: `tests/resync-state/test_resync_state.c`, `tests/resync-state/test_resync_adapter.c`

**Interfaces:**
- Produces: `resync_init(s, profile_count)`, `resync_handle()`, `resync_link_up()`, `resync_adapter_init(a, plat, at)`, `resync_adapter_on_link()`, `resync_adapter_on_profile_changed()`, `resync_adapter_on_endpoint_changed()`. Every one of these loses a parameter or disappears from the old shape, which is why nothing compiles until all of them move together.

- [ ] **Step 1: Rewrite the state machine header**

`module/src/resync_state.h` becomes:

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
};

struct resync_state {
    struct resync_profile profiles[RESYNC_MAX_PROFILES];
    uint8_t profile_count;
    uint8_t active;
    /* Profile whose language the current layer state represents, or -1. */
    int owner;
    bool ble_selected;
};

void resync_init(struct resync_state *s, uint8_t profile_count);

enum resync_action resync_handle(struct resync_state *s, const struct resync_event *ev);

/* Whether the machine believes this profile's link is up. The adapter uses it
   to compare against reality after pairing, where a callback can be missed. */
bool resync_link_up(const struct resync_state *s, uint8_t profile);
```

- [ ] **Step 2: Rewrite the state machine**

`module/src/resync_state.c` becomes:

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

    /* Never lost ownership: the layer already is this profile's language, and
       the stored copy may be older than the user's latest choice. */
    if (s->owner == (int)p) {
        return RESYNC_ACTION_NONE;
    }

    s->owner = (int)p;

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

void resync_init(struct resync_state *s, uint8_t profile_count) {
    for (unsigned i = 0; i < RESYNC_MAX_PROFILES; i++) {
        s->profiles[i].lang_alt = false;
        s->profiles[i].lang_known = false;
        s->profiles[i].link_up = false;
    }
    s->profile_count = profile_count > RESYNC_MAX_PROFILES ? RESYNC_MAX_PROFILES : profile_count;
    s->active = 0;
    s->owner = -1;
    s->ble_selected = false;
}

enum resync_action resync_handle(struct resync_state *s, const struct resync_event *ev) {
    uint8_t p = ev->profile;

    /* Reconcile the transport first. Deciding and then refusing to apply would
       leave the layer behind the state: ownership would move, a language would
       be remembered, with nothing written to the keymap. decide() returns
       before it mutates anything when BLE is not selected, so this has to be
       right before it runs. */
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
        s->profiles[p].link_up = false;
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
        s->profiles[p].link_up = true;
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

- [ ] **Step 3: Rewrite its tests**

`tests/resync-state/test_resync_state.c` becomes:

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

static struct resync_state st;
/* What the transport looks like to the caller. Every event carries it, so these
   tests set it the way the adapter would. */
static bool ble_now = true;

static void setup(void) {
    ble_now = true;
    resync_init(&st, 3);
}

static enum resync_action ev(enum resync_event_kind kind, uint8_t profile, bool alt_now,
                             int64_t at) {
    struct resync_event e = {
        .kind = kind, .profile = profile, .alt_now = alt_now, .at = at, .ble_now = ble_now};
    return resync_handle(&st, &e);
}

/* Both hosts connected; arriving at one restores its own language and the
   nothing to decide there. */
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

/* The adapter may report the same transition twice. */
static void test_repeat_notification_is_harmless(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_LINK_UP, IPAD, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);   /* the Mac is left on ru */
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10); /* the iPad learns en */

    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, false, 20) == RESYNC_ACTION_SET_ALT,
          "coming back to the Mac restores ru, once");
    CHECK(ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 21) == RESYNC_ACTION_NONE,
          "and the repeat does nothing, because the Mac still owns the layer");
}

/* Re-pairing puts a different machine behind the index. */
static void test_profile_cleared_drops_history(void) {
    setup();
    ev(RESYNC_EV_BLE_SELECTED, 0, false, 0);
    ev(RESYNC_EV_LINK_UP, MAC, false, 0);
    ev(RESYNC_EV_ACTIVE_PROFILE, MAC, true, 0);  /* the Mac is remembered on ru */
    ev(RESYNC_EV_ACTIVE_PROFILE, IPAD, true, 10);
    CHECK(st.profiles[MAC].lang_known, "the Mac's language was recorded");

    ev(RESYNC_EV_PROFILE_CLEARED, MAC, false, 25000);
    CHECK(!st.profiles[MAC].lang_known, "and it belonged to the old peer, so it goes");
    CHECK(!st.profiles[MAC].link_up, "the link state goes with it");
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"switch_between_connected_hosts", test_switch_between_connected_hosts},
        {"selected_but_never_connected_keeps_its_memory",
         test_selected_but_never_connected_keeps_its_memory},
        {"short_outage_under_owner_changes_nothing", test_short_outage_under_owner_changes_nothing},
        {"usb_round_trip", test_usb_round_trip},
        {"late_transport_observation_releases_ownership",
         test_late_transport_observation_releases_ownership},
        {"unknown_profile_takes_the_default", test_unknown_profile_takes_the_default},
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

- [ ] **Step 4: Run the state machine tests**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -o /tmp/resync_test \
   module/src/resync_state.c tests/resync-state/test_resync_state.c && /tmp/resync_test
```

Expected: PASS — eight cases, then `all checks passed`, no warnings.

- [ ] **Step 5: Rewrite the adapter core header**

`module/src/resync_adapter.h` becomes:

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

/* Every entry point takes the time the event happened, read by the caller on
   entry to its callback rather than after the binding's mutex. Nothing measures
   durations now, but keeping the timestamp honest costs nothing. */
void resync_adapter_init(struct resync_adapter *a, const struct resync_platform *plat, int64_t at);

/* A host link went up or down. Called from the Bluetooth connection callbacks,
   which is the only place an outage may be measured from. */
void resync_adapter_on_link(struct resync_adapter *a, uint8_t profile, bool up, int64_t at);

/* The active profile changed, or a profile's peer was written. */
void resync_adapter_on_profile_changed(struct resync_adapter *a, uint8_t index, int64_t at);

/* ZMK selected a different transport. */
void resync_adapter_on_endpoint_changed(struct resync_adapter *a, int64_t at);
```

- [ ] **Step 6: Rewrite the adapter core**

`module/src/resync_adapter.c` becomes:

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

void resync_adapter_init(struct resync_adapter *a, const struct resync_platform *plat, int64_t at) {
    a->plat = plat;
    resync_init(&a->state, plat->profile_count);

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
```

- [ ] **Step 7: Rewrite its tests**

`tests/resync-state/test_resync_adapter.c` becomes:

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
    resync_adapter_init(&ad, &plat, fake_now);
}

/* Reconciling must never fabricate a link transition for a profile whose peer
   did not change. A stale snapshot saying "connected" after a real disconnect
   had been recorded used to be papered over with a synthetic LINK_UP — after
   which the real reconnect finds the link already up and never restores. */
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

    /* Its memory is intact and the real reconnect is what marks it up again.
       The assertion above is the one with teeth: under the racy version the
       fabricated LINK_UP marked the link up while it was really down. */
    CHECK(ad.state.profiles[MAC].lang_alt, "the Mac still remembers ru");
    fake_now = 61000;
    resync_adapter_on_link(&ad, MAC, true, fake_now);
    CHECK(resync_link_up(&ad.state, MAC), "and the real reconnect brings it up");
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
    CHECK(!ad.state.profiles[MAC].lang_alt,
          "the old peer's ru is gone; the new one starts on the default");
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

- [ ] **Step 8: Run the adapter tests**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -o /tmp/resync_adapter_test \
   module/src/resync_state.c module/src/resync_adapter.c \
   tests/resync-state/test_resync_adapter.c && /tmp/resync_adapter_test
```

Expected: PASS — five cases, then `all checks passed`, no warnings.

- [ ] **Step 9: Drop the threshold from the Kconfig**

`module/Kconfig` becomes:

```kconfig
menuconfig ZMK_LAYOUT_RESYNC
    bool "Keep the language layer in step with the connected host"
    default y
    depends on ZMK_BLE
    depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL
    help
      Remembers which language each BLE profile was left on and restores it
      when you come back to that host. It never resets a language: after
      unlocking, both macOS and iPadOS restore whatever was active before
      locking, so resetting would be wrong for the whole session.

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

endif # ZMK_LAYOUT_RESYNC
```

- [ ] **Step 10: Remove the dead assignment from `config/op36.conf`**

Delete these four lines, comment included. The symbol no longer exists, and Zephyr stops on an
assignment to a symbol it does not know:

```
# Layout resync. Restoring a language is safe on any host; resetting one is only
# safe on a host known to reset its own layout after sleep, and neither has been
# checked yet. Task 5 turns on the bit for the Mac.
CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES=0x0
```

Nothing replaces them. `CONFIG_ZMK_LAYOUT_RESYNC` is `default y` and its dependencies are what CI
already put under test; the alternate layer defaults to 1, which is `ru`.

- [ ] **Step 11: Rewrite the binding**

`module/src/layout_resync.c` becomes:

```c
/*
 * Binds the layout resync adapter to ZMK. There is no logic here: the platform
 * struct wraps the ZMK calls, the callbacks route into resync_adapter, and one
 * mutex serialises them. See
 * docs/superpowers/specs/2026-09-08-layout-resync-design.md.
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
/* SYS_INIT lives here, not in kernel.h. */
#include <zephyr/init.h>
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

/* Zephyr says of bt_addr_le_t: "Not packed, so no sizeof()". BT_ADDR_LE_SIZE is
   the documented length, and the type byte and the six address bytes are both
   byte aligned, so the first BT_ADDR_LE_SIZE bytes are the address whatever the
   compiler does with trailing padding. */
BUILD_ASSERT(BT_ADDR_LE_SIZE == RESYNC_PEER_LEN,
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
   would attribute the wait to the event. Nothing measures durations now, but
   an honest timestamp costs nothing. */
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
    k_mutex_unlock(&lock);
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
    resync_adapter_init(&adapter, &platform, k_uptime_get());

    LOG_INF("resync: %d profiles, alternate layer %d", ZMK_BLE_PROFILE_COUNT, ALT_LAYER);
    return 0;
}

SYS_INIT(layout_resync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 12: Run the whole suite**

Run: `./tests/run.sh`
Expected: no `FAILED` lines. Both C tests run before any simulator case.

- [ ] **Step 13: Commit**

```bash
git add module/src tests/resync-state module/Kconfig config/op36.conf
git commit -m "refactor: restore languages per profile without ever resetting one"
```

- [ ] **Step 14: Build it**

Ask for a push, then confirm the run is `success` on all three matrix entries:

```bash
gh run list --limit 1
gh run view <run-id> --json jobs --jq '.jobs[] | "\(.conclusion)  \(.name)"'
```

If the configuration step fails with `Aborting due to Kconfig warnings`, Step 10 was missed.

---

### Task 2: Verify on the devices and document it

What is left to prove is that the memory is per host, that it survives a sleep, and that nothing
moves the layer when it should not.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Two hosts, two languages**

Flash the left half with Task 1's build.

The obvious check — one host on `ru`, switch away, come back, type — **proves nothing**: it passes
without the module too, because the global `ru` simply stays on. The hosts have to disagree.

1. Mac: switch to `ru`, type a Cyrillic word. Host and firmware agree.
2. Switch to the iPad's profile. Set it to `en`, type a Latin word. Same there.
3. Back to the Mac, type. Expected: Cyrillic, with no manual switch.
4. To the iPad, type. Expected: Latin, with no manual switch.
5. Repeat 3 and 4 once more, so a single lucky state cannot be mistaken for the feature working.

- [ ] **Step 2: The language survives a sleep**

This is the case that replaced the reset branch. **Do not touch the layer by hand during this check** —
`nav` 6 or 7 would move the firmware without moving ownership and confound the result. The manual
path is Step 3, separately.

1. Mac on `ru`, confirmed by typing.
2. Lock the screen and wait until the link **is observed** to have dropped. A minute of waiting is
   not evidence: watch the keyboard disappear from the Bluetooth list, or read it out of the log if
   USB logging is on. Without that observation the check proves nothing, because a link that never
   dropped means no event ever reached the module.
3. Unlock **with Touch ID, a watch, or the built-in keyboard** — anything that does not put a
   character through the op36. Typing the password on it would move the firmware layer and confound
   the result; the manual path is Step 3, on purpose separate.
4. Type in an application, on the op36.

Expected: Cyrillic, with the firmware agreeing. Repeat once on the iPad.

- [ ] **Step 3: The manual path for the password, in full**

The password field takes ASCII while the firmware is on `ru`. The manual answer is two presses, not
one, and the second is the part that is easy to forget:

1. At the lock screen, press `nav` 6 (`&to 0`). The firmware is now `en`; nothing was sent to the
   host.
2. Type the password. Expected: it works.
3. Unlock. The session restores `ru`, but **the firmware is still `en`** — ownership never moved, so
   nothing restored it.
4. Press `nav` 7 (`&to 1`).

Expected: Cyrillic again, both sides agreeing.

Check the consequence of skipping step 4 once, so the failure is recognisable: with the firmware left
on `en`, switch to the iPad and back. The Mac's memory will have taken that `en` — the layer was its
to save when ownership moved — and the Mac now restores `en`. Fix it by setting `ru` again and
switching away and back, which re-saves it.

- [ ] **Step 4: Out of range and back**

Walk away until the link drops, come back, type. Expected: the language the host was left on, on both
sides, with no manual switch. Same path as Step 2 without the lock screen in it.

- [ ] **Step 5: Update CLAUDE.md**

The module's section exists from an earlier commit and has **two** stale paragraphs, not one. Replace
both:

- the opening one, which says the module "returns to the default language" after an outage past
  `CONFIG_ZMK_LAYOUT_RESYNC_DISCONNECT_MS`;
- the one beginning "**`CONFIG_ZMK_LAYOUT_RESYNC_RESET_PROFILES` is a bitmask**".

Replacing only the second leaves the first promising a reset that no longer happens. The replacement
for both:

```markdown
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
```

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: record that the module restores rather than resets"
```
