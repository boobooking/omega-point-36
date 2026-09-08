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
