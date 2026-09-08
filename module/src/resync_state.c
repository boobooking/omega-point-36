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
