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
