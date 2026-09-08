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
