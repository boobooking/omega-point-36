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
