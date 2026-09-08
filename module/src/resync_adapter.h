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
