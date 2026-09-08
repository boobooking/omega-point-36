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
