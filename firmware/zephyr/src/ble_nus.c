/*
 * ble_nus.h on Zephyr's Bluetooth host: the same service, the same owner rules and the same line
 * handling as the NimBLE version in ../../src/ble_nus.c, which is where the reasons for each are
 * written down. What is said here is where Zephyr differs.
 */
#include "ble_nus.h"
#include "bridge.h"
#include "led.h"
#include "port.h"
#include "store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#define TAG "ble"

#define NUS_LINE_MAX 512
#define SILENT_LINK_S 300
#define STUCK_RESTART_S 60
#define DEVICE_NAME_PREFIX "LoRaBridge-for-DMD2"
#define DEVICE_ID_HEX_LEN 8
#define DEVICE_NAME_LEN ((sizeof(DEVICE_NAME_PREFIX) - 1) + 1 + DEVICE_ID_HEX_LEN)
_Static_assert(DEVICE_NAME_LEN <= 29, "BLE device name must fit a legacy scan response");

/* 6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E */
#define NUS_UUID(n) BT_UUID_128_ENCODE(0x6e400000 | (n), 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
static const struct bt_uuid_128 s_uuid_svc = BT_UUID_INIT_128(NUS_UUID(1));
static const struct bt_uuid_128 s_uuid_rx = BT_UUID_INIT_128(NUS_UUID(2));
static const struct bt_uuid_128 s_uuid_tx = BT_UUID_INIT_128(NUS_UUID(3));

static ble_nus_line_cb s_on_line;
static ble_nus_revoke_cb s_on_revoke;
static ble_nus_revoke_cb s_on_bonds_cleared;
static char s_name[DEVICE_NAME_LEN + 1];

/* When the phone last wrote anything, in seconds since boot; reset on every connection. */
static atomic_uint_least32_t s_heard_s = ATOMIC_VAR_INIT(0);

static uint32_t uptime_s(void) { return (uint32_t)(port_uptime_us() / 1000000); }

/* ---- the connection ---------------------------------------------------------- */

/*
 * The one central served, with a reference held while it is current. The generation tells a
 * new connection from an old one even when the host hands out the same connection object again,
 * and every snapshot holds its own reference, so an object in use is never recycled under it.
 */
static struct k_spinlock s_conn_lock;
static struct bt_conn *s_conn;
static uint32_t s_conn_generation;

typedef struct {
    struct bt_conn *conn;
    uint32_t generation;
} conn_target_t;

static bool s_current_revoked;
/* A clear that could not be verified: nobody new until one is. */
static bool s_store_indeterminate;
/* The store and what the host loaded from it disagreed at start, or the store could not be read:
 * a phone the host holds keys for may connect, nobody else may pair. */
static bool s_owner_unverified;
static bool s_clear_in_progress;
static bool s_restart_pending;
/* A clear asked for while pairing was still in flight finishes at that connection's end. */
static bool s_clear_after_disconnect;
static bool s_clear_physical;
static uint32_t s_clear_generation;
/* Kept by hand: the host has no getter, and the supervisor has to know. */
static atomic_bool s_advertising = ATOMIC_VAR_INIT(false);

static void revoke_current_session(void) {
    if (s_current_revoked) return;
    s_current_revoked = true;
    if (s_on_revoke) s_on_revoke();
}

static void connection_set(struct bt_conn *conn) {
    k_spinlock_key_t key = k_spin_lock(&s_conn_lock);
    s_conn = bt_conn_ref(conn);
    s_conn_generation++;
    k_spin_unlock(&s_conn_lock, key);
    s_current_revoked = false;
}

static bool connection_clear_if(struct bt_conn *conn) {
    struct bt_conn *old = NULL;
    k_spinlock_key_t key = k_spin_lock(&s_conn_lock);
    if (s_conn && s_conn == conn) {
        old = s_conn;
        s_conn = NULL;
        s_conn_generation++;
    }
    k_spin_unlock(&s_conn_lock, key);
    if (old) bt_conn_unref(old);
    return old != NULL;
}

/* Holds a reference the caller gives back with target_release, whether or not this succeeded. */
static bool connection_snapshot(conn_target_t *out) {
    k_spinlock_key_t key = k_spin_lock(&s_conn_lock);
    out->conn = s_conn ? bt_conn_ref(s_conn) : NULL;
    out->generation = s_conn_generation;
    k_spin_unlock(&s_conn_lock, key);
    return out->conn != NULL;
}

static void target_release(conn_target_t *target) {
    if (target->conn) bt_conn_unref(target->conn);
    target->conn = NULL;
}

static bool connection_is_current(const conn_target_t *target) {
    k_spinlock_key_t key = k_spin_lock(&s_conn_lock);
    const bool current = s_conn && s_conn == target->conn &&
                         s_conn_generation == target->generation;
    k_spin_unlock(&s_conn_lock, key);
    return current;
}

static void disconnect_if_current(const conn_target_t *target) {
    if (connection_is_current(target)) {
        bt_conn_disconnect(target->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---- owners ------------------------------------------------------------------ */

struct bond_census {
    const bt_addr_le_t *peer;
    int count;
    bool found;
};

static void bond_visit(const struct bt_bond_info *info, void *user_data) {
    struct bond_census *census = user_data;
    census->count++;
    if (census->peer && bt_addr_le_eq(&info->addr, census->peer)) census->found = true;
}

/* The bonds the host holds in RAM; what the flash holds is store_scan_bonds' business. */
static struct bond_census bonds_of(const bt_addr_le_t *peer) {
    struct bond_census census = {.peer = peer};
    bt_foreach_bond(BT_ID_DEFAULT, bond_visit, &census);
    return census;
}

/* True when a bond already exists, or may, and this is not it. By the time this is asked the
 * host has resolved a phone's private address against the bonds it holds, so the address
 * compared is the phone's identity. */
static bool is_stranger(struct bt_conn *conn) {
    if (s_clear_in_progress || s_restart_pending) {
        LOGW(TAG, "bond reset in progress; refusing new connection");
        return true;
    }
    if (s_store_indeterminate) {
        LOGW(TAG, "bond store outcome unknown; refusing new connection");
        return true;
    }
    const struct bond_census census = bonds_of(bt_conn_get_dst(conn));
    if (census.found) return false;
    if (census.count > 0) return true;
    if (s_owner_unverified) {
        LOGW(TAG, "bond store unverified; refusing a phone it holds no keys for");
        return true;
    }
    return false;   /* nobody has claimed this bridge yet */
}

/*
 * No pairing at all once the bridge is claimed, or may be. A stranger is already gone by the
 * time it could ask, so this is the owner's own phone pairing over its bond, which is what
 * NimBLE's repeat-pairing event refuses on the ESP32 builds: Just Works authenticates no human,
 * and a peer presenting the owner's address must not be able to replace the owner's key.
 */
static enum bt_security_err on_pairing_accept(struct bt_conn *conn,
                                              const struct bt_conn_pairing_feat *feat) {
    (void)conn;
    (void)feat;
    if (s_clear_in_progress || s_restart_pending || s_store_indeterminate ||
        s_owner_unverified || bonds_of(NULL).count > 0) {
        LOGW(TAG, "repeat pairing refused; use authenticated FORGET or hold the button 3 s");
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }
    return BT_SECURITY_ERR_SUCCESS;
}

/* Only pairing_accept: with no passkey callbacks the host pairs as NoInputNoOutput, Just Works,
 * as the bridge has no display or keypad to compare anything on. */
static struct bt_conn_auth_cb s_auth = {
    .pairing_accept = on_pairing_accept,
};

static void on_pairing_complete(struct bt_conn *conn, bool bonded) {
    (void)conn;
    LOGI(TAG, "paired%s", bonded ? " and bonded" : ", not bonded");
}

static void on_pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    (void)conn;
    LOGW(TAG, "pairing failed (%d)", (int)reason);
}

static struct bt_conn_auth_info_cb s_auth_info = {
    .pairing_complete = on_pairing_complete,
    .pairing_failed = on_pairing_failed,
};

/* ---- lines in ------------------------------------------------------------------ */

static char s_line[NUS_LINE_MAX];
static size_t s_line_len;
static bool s_discarding;

static void feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_discarding) {
                s_discarding = false;
                s_line_len = 0;
            } else if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                if (s_on_line) s_on_line(s_line, s_line_len);
                s_line_len = 0;
                /* FORGET can revoke this session inside the callback. Do not parse a second
                 * command that shared the same GATT write after that security boundary. */
                if (s_current_revoked) return;
            }
        } else if (s_discarding) {
            /* skip */
        } else if (s_line_len < NUS_LINE_MAX - 1) {
            s_line[s_line_len++] = c;
        } else {
            LOGW(TAG, "line overflow, discarding to end of line");
            s_discarding = true;
            s_line_len = 0;
        }
    }
}

/* On the host's receive thread. The phone writes at most a fragment per write, so a prepared
 * (long) write never arrives: the host refuses those with no prepare queue configured. */
static ssize_t on_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                     uint16_t len, uint16_t offset, uint8_t flags) {
    (void)attr;
    (void)offset;
    (void)flags;
    conn_target_t current;
    const bool ours = connection_snapshot(&current) && current.conn == conn;
    target_release(&current);
    if (!ours) {
        /* Termination is cleanup, not authorization: a refused stranger must remain powerless
         * even if the controller has not processed (or refused) our disconnect yet. */
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    if (s_current_revoked) return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    atomic_store(&s_heard_s, uptime_s());
    feed(buf, len);
    return len;
}

/*
 * Writes need an encrypted link, which means a peer that has bonded; notifications are left
 * open, because everything they carry also goes out over the air in clear.
 */
BT_GATT_SERVICE_DEFINE(s_nus,
    BT_GATT_PRIMARY_SERVICE(&s_uuid_svc),
    BT_GATT_CHARACTERISTIC(&s_uuid_rx.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, on_rx, NULL),
    BT_GATT_CHARACTERISTIC(&s_uuid_tx.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* The TX characteristic's value, the attribute a notification names. */
#define NUS_TX_ATTR (&s_nus.attrs[4])

/* ---- advertising and connection events ---------------------------------------- */

static const struct bt_data s_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, NUS_UUID(1)),
};
/* The name in the scan response: a 128-bit UUID and flags leave too little of the
 * advertisement for it, and the phone scans by UUID. */
static struct bt_data s_sd[1];

/* Every 100-150 ms rather than as fast as allowed: this board runs on a battery, and a phone
 * scanning for it still finds it within a second. */
static void advertise(void) {
    if (s_restart_pending) return;
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, s_ad, ARRAY_SIZE(s_ad), s_sd,
                              ARRAY_SIZE(s_sd));
    if (err == 0 || err == -EALREADY) {
        atomic_store(&s_advertising, true);
    } else {
        LOGE(TAG, "adv_start failed: %d", err);
    }
}

static void forget(bool physical);

static void on_connected(struct bt_conn *conn, uint8_t err) {
    /* A connectable advertisement ends with the connection it made, or with its failure. */
    atomic_store(&s_advertising, false);
    if (err) {
        LOGW(TAG, "connection failed (%u)", err);
        advertise();
        return;
    }
    if (is_stranger(conn)) {
        LOGW(TAG, "rejecting unpaired peer; bridge is already claimed");
        int rc = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (rc != 0) LOGE(TAG, "could not terminate refused peer (%d); writes stay denied", rc);
        return;
    }
    connection_set(conn);
    atomic_store(&s_heard_s, uptime_s());
    s_line_len = 0;
    s_discarding = false;
    LOGI(TAG, "connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    LOGI(TAG, "disconnected (0x%02x)", reason);
    k_spinlock_key_t key = k_spin_lock(&s_conn_lock);
    const bool clear_for_button = s_clear_after_disconnect && s_conn == conn &&
                                  s_conn_generation == s_clear_generation;
    k_spin_unlock(&s_conn_lock, key);
    const bool was_current = connection_clear_if(conn);
    /* Work accepted by a BLE session belongs to that session; a normal link loss fences its
     * queued TX just as FORGET does. */
    if (was_current) revoke_current_session();
    if (clear_for_button && was_current) {
        s_clear_after_disconnect = false;
        forget(s_clear_physical);
    }
}

/* Advertising again waits for the connection object to be free: with one connection allowed,
 * a connectable advertisement started before then has nothing to accept a phone with. */
static void on_recycled(void) { advertise(); }

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err) {
    (void)conn;
    if (err == BT_SECURITY_ERR_SUCCESS) {
        LOGI(TAG, "encryption on (level %d)", (int)level);
    } else {
        /* Not proof that the key is stale, and never a reason to delete the owner's bond:
         * repair is deliberately physical or authorised - FORGET, or the button. */
        LOGW(TAG, "encryption failed (%d) at level %d; bond kept", (int)err, (int)level);
    }
}

BT_CONN_CB_DEFINE(s_conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
    .security_changed = on_security_changed,
};

static void on_mtu(struct bt_conn *conn, uint16_t tx, uint16_t rx) {
    (void)conn;
    (void)rx;
    LOGI(TAG, "mtu now %u", tx);
}

static struct bt_gatt_cb s_gatt_callbacks = {
    .att_mtu_updated = on_mtu,
};

/* ---- forgetting ------------------------------------------------------------------ */

/* One clear at a time, whether the phone asked or the button did; s_scan is used under it, and
 * at start before anything else can be. */
static K_MUTEX_DEFINE(s_forget_lock);
static struct store_bonds s_scan;

/*
 * The durable half only: every record a bond keeps, deleted, and the store read back - past the
 * settings layer, see store_scan_bonds - as holding no owner. The keys the live link encrypts with
 * stay in RAM until the restart that follows, since the host offers no way to drop one without
 * also dropping the link, and that is what lets FORGOTTEN travel over the link it ends.
 */
static bool clear_bond_store(void) {
    const int found = store_scan_bonds(&s_scan);
    /* Nothing is deleted from a store that could not be read whole: which of its records are an
     * owner's is not known, and the button's erase is the way out of that. */
    bool owners_deleted = found == 0;
    for (int i = 0; found == 0 && i < s_scan.count; i++) {
        const int rc = settings_delete(s_scan.names[i]);
        if (rc != 0) {
            LOGW(TAG, "could not delete %s (%d)", s_scan.names[i], rc);
            if (strncmp(s_scan.names[i], "bt/keys/", 8) == 0) owners_deleted = false;
        }
    }
    const int read = store_scan_bonds(&s_scan);
    if (owners_deleted && read == 0 && s_scan.owners == 0) {
        s_store_indeterminate = false;
        s_owner_unverified = false;
        return true;
    }
    LOGE(TAG, "bond clear unverified (scan %d, read back %d, owners left %d)", found, read,
         read == 0 ? s_scan.owners : -1);
    s_store_indeterminate = true;
    return false;
}

/* Complete the destructive half only after any in-flight pairing is gone. A non-null target
 * means this is an already-bonded session that can receive the success/failure result. */
static void finish_forget(const conn_target_t *notify_target, bool physical) {
    s_clear_in_progress = true;
    const bool cleared = clear_bond_store();
    s_clear_in_progress = false;
    if (!cleared && physical) {
        /*
         * The button is the owner's own hand on the board, and when the records cannot be read
         * back, only an empty partition can be shown to hold no owner. Restarting at once: the
         * host's view of the store is gone with it, and must not be written into.
         */
        LOGW(TAG, "bond store unreadable - erasing all settings and restarting");
        store_erase();
        port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BOND);
    }
    if (!cleared) {
        LOGE(TAG, "could not verify an empty bond store; keeping any recovery link");
        if (notify_target && connection_is_current(notify_target)) {
            ble_nus_send_line("ERR forget incomplete");
        }
        return;
    }
    LOGW(TAG, "pairings cleared");
    led_bond_cleared();
    revoke_current_session();
    s_restart_pending = true;
    if (s_on_bonds_cleared) s_on_bonds_cleared();

    if (!notify_target || !connection_is_current(notify_target)) return;
    /* The success event is also the phone's permission to delete its system bond; the restart
     * that follows is the eventual disconnect. */
    if (!ble_nus_send_line("FORGOTTEN")) {
        LOGE(TAG, "pairings cleared but FORGOTTEN could not be queued; restart will disconnect");
    }
}

static void forget(bool physical) {
    k_mutex_lock(&s_forget_lock, K_FOREVER);
    conn_target_t current;
    if (!connection_snapshot(&current)) {
        finish_forget(NULL, physical);
        k_mutex_unlock(&s_forget_lock);
        return;
    }
    const bool encrypted = bt_conn_get_security(current.conn) >= BT_SECURITY_L2;
    const bool bonded = bonds_of(bt_conn_get_dst(current.conn)).found;
    if (encrypted && bonded) {
        /* Pairing is complete, so no later security step can recreate the key. */
        finish_forget(&current, physical);
        target_release(&current);
        k_mutex_unlock(&s_forget_lock);
        return;
    }
    /* Encrypted but not yet bonded: key distribution may still be running, and clearing now
     * could have the owner stored again after FORGOTTEN. Stop the pairing first; its disconnect
     * event performs the clear, with no success claimed across the dead link. */
    LOGW(TAG, "bond reset requested during incomplete pairing; disconnecting first");
    s_clear_after_disconnect = true;
    s_clear_physical = physical;
    revoke_current_session();
    s_clear_generation = current.generation;
    const int rc = bt_conn_disconnect(current.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (rc == -ENOTCONN) {
        s_clear_after_disconnect = false;
        connection_clear_if(current.conn);
        finish_forget(NULL, physical);
    } else if (rc != 0) {
        /* Keep both revocation and the pending clear: a natural disconnect can still finish
         * it. Another button press may retry if the controller stays wedged. */
        LOGE(TAG, "cannot stop in-flight pairing for bond reset (%d); clear pending", rc);
    }
    target_release(&current);
    k_mutex_unlock(&s_forget_lock);
}

void ble_nus_forget_bonds(void) { forget(false); }

static void forget_from_button(struct k_work *work) {
    (void)work;
    forget(true);
}
static K_WORK_DEFINE(s_forget_work, forget_from_button);

bool ble_nus_request_forget_bonds(void) {
    /* Queued at most once while pending, so a held button stays one event. */
    return k_work_submit(&s_forget_work) >= 0;
}

/* ---- supervision ---------------------------------------------------------------- */

/* Drop a link the phone has gone silent on, or start advertising again if nothing is; either
 * way the host's own events take it from there. */
static void kick(struct k_work *work) {
    (void)work;
    conn_target_t current;
    if (connection_snapshot(&current)) {
        LOGW(TAG, "phone silent for %u s on a live link - dropping it",
             (unsigned)(uptime_s() - atomic_load(&s_heard_s)));
        disconnect_if_current(&current);
    } else if (!atomic_load(&s_advertising)) {
        LOGW(TAG, "neither connected nor advertising - advertising again");
        advertise();
    }
    target_release(&current);
}
static K_WORK_DEFINE(s_kick_work, kick);

bool ble_nus_supervise(void) {
    static uint32_t stuck_since;   /* 0: nothing wrong */
    if (s_restart_pending || s_clear_in_progress) {
        stuck_since = 0;
        return false;
    }
    const uint32_t now = uptime_s();
    const bool stuck = ble_nus_connected() ? now - atomic_load(&s_heard_s) > SILENT_LINK_S
                                           : !atomic_load(&s_advertising);
    if (!stuck) {
        stuck_since = 0;
        return false;
    }
    if (stuck_since == 0) {
        stuck_since = now ? now : 1;
        k_work_submit(&s_kick_work);
        return false;
    }
    if (now - stuck_since < STUCK_RESTART_S) return false;
    LOGE(TAG, "bluetooth still stuck after %u s", (unsigned)(now - stuck_since));
    return true;
}

/* ---- lines out -------------------------------------------------------------------- */

bool ble_nus_connected(void) {
    conn_target_t target;
    const bool connected = connection_snapshot(&target);
    target_release(&target);
    return connected;
}

/*
 * Lines go out from a thread of their own. The host allocates a notification without waiting
 * when asked from its receive thread or the system workqueue - where every command reply and
 * FORGOTTEN would be written - so a long line at the default MTU, 26 notifications for a
 * PING 500, ran out of buffers at the tenth and tore. From here the allocation waits for the
 * controller to hand buffers back, and the queue keeps lines whole and in order.
 */
struct out_line {
    uint32_t generation;   /* the connection the line was written for */
    uint16_t len;
    char text[NUS_LINE_MAX];
};

K_MSGQ_DEFINE(s_out, sizeof(struct out_line), 8, 4);

bool ble_nus_send_line(const char *line) {
    /*
     * Not subscribed yet is not a broken link: the host refuses a notification nobody asked for,
     * where NimBLE's custom notify sent it anyway. Treated as a torn line, that refusal would
     * drop every fresh connection the moment a packet arrived before the phone subscribed.
     */
    conn_target_t target;
    const bool live = connection_snapshot(&target) &&
                      bt_gatt_is_subscribed(target.conn, NUS_TX_ATTR, BT_GATT_CCC_NOTIFY);
    struct out_line out = {.generation = target.generation};
    target_release(&target);
    if (!live) return false;
    const size_t len = strlen(line);
    if (len > sizeof(out.text)) {
        LOGW(TAG, "outbound line of %u bytes dropped", (unsigned)len);
        return false;
    }
    memcpy(out.text, line, len);
    out.len = (uint16_t)len;
    if (k_msgq_put(&s_out, &out, K_NO_WAIT) != 0) {
        LOGW(TAG, "outbound busy - line dropped");
        return false;
    }
    return true;
}

static int notify(struct bt_conn *conn, const void *data, size_t len) {
    return bt_gatt_notify(conn, NUS_TX_ATTR, data, (uint16_t)len);
}

static void send_now(const struct out_line *line) {
    conn_target_t target;
    if (!connection_snapshot(&target) || target.generation != line->generation) {
        target_release(&target);
        return;   /* written for a connection that has ended: never the next phone's */
    }
    /* Unsubscribed since it was queued: refused at the first fragment, which would read as a
     * torn line and cost the link. */
    if (!bt_gatt_is_subscribed(target.conn, NUS_TX_ATTR, BT_GATT_CCC_NOTIFY)) {
        target_release(&target);
        return;
    }

    /* A notification carries at most ATT_MTU-3 bytes; the receiver reassembles on the newline,
     * which is why the terminator must go out in the final fragment. */
    const uint16_t mtu = bt_gatt_get_mtu(target.conn);
    const size_t chunk = mtu > 3 ? (size_t)(mtu - 3) : 20;
    static char frag[NUS_LINE_MAX + 1];

    size_t sent = 0;
    bool terminated = false;
    bool torn = false;

    while (sent < line->len) {
        if (!connection_is_current(&target)) {
            torn = (sent > 0);
            break;
        }
        size_t take = line->len - sent;
        if (take > chunk) take = chunk;
        memcpy(frag, line->text + sent, take);
        size_t out = take;
        sent += take;
        if (sent == line->len && take < chunk) {
            frag[out++] = '\n';
            terminated = true;
        }
        if (notify(target.conn, frag, out) != 0) {
            torn = true;
            terminated = false;   /* whatever went out, the phone has no end of line yet */
            break;
        }
    }

    bool resync = false;
    if (!terminated && connection_is_current(&target)) {
        if (notify(target.conn, "\n", 1) != 0) {
            /* Anything already delivered leaves the phone holding the front of a line with no
             * end to it; dropping the link is the only honest recovery. */
            resync = (sent > 0);
        } else {
            terminated = true;
        }
    }
    if (torn) LOGW(TAG, "outbound line cut short");
    if (resync) {
        LOGW(TAG, "no delimiter after a torn line - dropping the link to resynchronise");
        disconnect_if_current(&target);
    }
    target_release(&target);
}

static void out_thread(void *unused1, void *unused2, void *unused3) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    static struct out_line line;
    for (;;) {
        k_msgq_get(&s_out, &line, K_FOREVER);
        send_now(&line);
    }
}

/* Above the bridge's own threads, so a transmission's busy wait does not hold a reply back for
 * its whole time on air; the host's threads are cooperative and run first anyway. */
K_THREAD_DEFINE(s_out_tid, 2048, out_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);

/* ---- start ---------------------------------------------------------------------- */

void ble_nus_init(ble_nus_line_cb on_line, ble_nus_revoke_cb on_revoke,
                  ble_nus_revoke_cb on_bonds_cleared) {
    s_on_line = on_line;
    s_on_revoke = on_revoke;
    s_on_bonds_cleared = on_bonds_cleared;

    int err = bt_enable(NULL);
    if (err != 0) {
        LOGE(TAG, "bluetooth would not start (%d)", err);
        abort();
    }
    bt_conn_auth_cb_register(&s_auth);
    bt_conn_auth_info_cb_register(&s_auth_info);
    bt_gatt_cb_register(&s_gatt_callbacks);
    /* The identity and the bonds, which the host reads from settings only once it is up - and
     * only from a store read whole first. main has already refused to start over one it could
     * not read; should this read fail all the same, restarting leaves it for main to find. */
    int load_err;
    err = store_load_bonds(&s_scan, &load_err);
    if (err != 0) {
        LOGE(TAG, "bond store unreadable (%d) - restarting rather than loading it", err);
        port_restart(0);
    }
    if (load_err != 0) LOGE(TAG, "bluetooth settings did not load (%d)", load_err);

    /*
     * An owner on flash that the host did not load would otherwise read as no owner at all, and
     * the next phone to pair would take the bridge. So unless the two agree exactly, nobody new
     * pairs until the button clears the store - while a phone the host does hold keys for still
     * connects.
     */
    const int loaded = bonds_of(NULL).count;
    if (s_scan.owners != loaded) {
        s_owner_unverified = true;
        LOGE(TAG, "bond store unverified (%d stored, %d loaded): no new phone until the button "
             "is held 3 s", s_scan.owners, loaded);
    }

    /*
     * Named after the last four bytes of this board's identity address, as an ESP32 bridge is
     * after its MAC, so two bridges on one ride never share a name. The host keeps the address
     * least significant byte first.
     */
    bt_addr_le_t id[CONFIG_BT_ID_MAX];
    size_t count = ARRAY_SIZE(id);
    bt_id_get(id, &count);
    const uint8_t *a = count ? id[0].a.val : (const uint8_t[6]){0};
    snprintf(s_name, sizeof(s_name), "%s-%02X%02X%02X%02X", DEVICE_NAME_PREFIX, a[3], a[2], a[1],
             a[0]);
    err = bt_set_name(s_name);
    if (err != 0) LOGE(TAG, "name not set (%d)", err);
    s_sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, s_name, strlen(s_name));
    LOGI(TAG, "advertising as %s", s_name);
    advertise();
}
