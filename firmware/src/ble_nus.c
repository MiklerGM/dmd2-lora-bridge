#include "ble_nus.h"
#include "led.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Provided by the NimBLE port; the ESP-IDF examples forward-declare it the same way. */
void ble_store_config_init(void);
/* The public all-record clear can fail after removing OUR_SEC from RAM but before persisting
 * that deletion. Retrying this pinned port's persistence step is what makes an empty readback
 * durable rather than merely an empty RAM cache. */
int ble_store_config_persist_our_secs(void);
void ble_hs_lock_nested(void);
void ble_hs_unlock_nested(void);

#define TAG "ble"

/* 6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_RX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_TX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define NUS_LINE_MAX 512
/* The app asks for a status every 30 s, so a live phone is never quiet for long. A sleeping one
 * can be, which is why this is minutes rather than three missed polls: dropping a link the phone
 * will simply rebuild costs little, but not this often. */
#define SILENT_LINK_S 300
/* How long the kick above gets to work before only a restart is left. */
#define STUCK_RESTART_S 60
#define DEVICE_NAME_PREFIX "LoRaBridge-for-DMD2"
#define DEVICE_ID_HEX_LEN 8
#define DEVICE_NAME_LEN ((sizeof(DEVICE_NAME_PREFIX) - 1) + 1 + DEVICE_ID_HEX_LEN)

/* A complete local name in a legacy scan response gets 29 payload bytes: one byte each for
 * AD length/type leaves 29 of the 31-byte packet. Fail at compile time if the prefix grows far
 * enough that NimBLE would have to truncate the stable identity suffix. */
_Static_assert(DEVICE_NAME_LEN <= 29, "BLE device name must fit a legacy scan response");

static ble_nus_line_cb s_on_line;
static ble_nus_revoke_cb s_on_revoke;
static ble_nus_revoke_cb s_on_bonds_cleared;
static const char *s_name;
/*
 * Every bridge advertised the same name, so on a group ride one rider's phone could grab
 * another rider's bridge - and since only one central may connect, the other rider would be
 * left with no radio at all. The name carries the last four bytes of this board's BT MAC: eight
 * hex digits keep collision risk negligible for a ride while the shared prefix keeps every
 * bridge discoverable.
 */
static char s_name_buf[DEVICE_NAME_LEN + 1];
static atomic_uint_least16_t s_conn = ATOMIC_VAR_INIT(BLE_HS_CONN_HANDLE_NONE);
/* Even values are stable snapshots; odd means the GAP callback is changing s_conn. Only the
 * NimBLE host callback writes these two atomics, so this tiny sequence lock is sufficient. */
static atomic_uint_least32_t s_conn_generation = ATOMIC_VAR_INIT(0);
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static struct ble_npl_event s_forget_event;
static struct ble_npl_event s_kick_event;

/* When the phone last wrote anything, in seconds since boot; reset on every connection. */
static atomic_uint_least32_t s_heard_s = ATOMIC_VAR_INIT(0);

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

typedef struct {
    uint16_t conn;
    uint32_t generation;
} disconnect_target_t;

/* Written only by the NimBLE host task. A physical clear during an unfinished pairing first
 * tears that connection down; clearing in its DISCONNECT event prevents a late pairing step
 * from storing a replacement key after the clear. */
static bool s_clear_after_disconnect;
static disconnect_target_t s_clear_target;
static bool s_current_revoked;
/* A failed NVS persistence can leave the RAM owner list empty while the durable key is unknown.
 * Until a later physical/authenticated clear succeeds, that is not an unclaimed bridge. */
static bool s_store_indeterminate;
static bool s_clear_in_progress;
/* A restart clears NimBLE's controller resolving list after the durable store is empty. Do not
 * accept a replacement owner in the short interval before main performs that restart. */
static bool s_restart_pending;

static void revoke_current_session(void) {
    if (s_current_revoked) return;
    s_current_revoked = true;
    if (s_on_revoke) s_on_revoke();
}

static void connection_set(uint16_t conn) {
    atomic_fetch_add(&s_conn_generation, 1);  /* odd: update in progress */
    atomic_store(&s_conn, conn);
    atomic_fetch_add(&s_conn_generation, 1);  /* even: stable */
    s_current_revoked = false;
}

static bool connection_clear_if(uint16_t conn) {
    /* GAP callbacks are serialized on the NimBLE host task; external tasks only read. */
    if (atomic_load(&s_conn) != conn) return false;
    atomic_fetch_add(&s_conn_generation, 1);
    atomic_store(&s_conn, BLE_HS_CONN_HANDLE_NONE);
    atomic_fetch_add(&s_conn_generation, 1);
    return true;
}

static bool connection_snapshot(disconnect_target_t *out) {
    for (;;) {
        uint32_t before = (uint32_t)atomic_load(&s_conn_generation);
        if (before & 1u) {
            taskYIELD();
            continue;
        }
        uint16_t conn = (uint16_t)atomic_load(&s_conn);
        uint32_t after = (uint32_t)atomic_load(&s_conn_generation);
        if (before != after) continue;
        out->conn = conn;
        out->generation = after;
        return conn != BLE_HS_CONN_HANDLE_NONE;
    }
}

static bool connection_is_current(const disconnect_target_t *target) {
    disconnect_target_t current;
    return connection_snapshot(&current) && current.generation == target->generation &&
           current.conn == target->conn;
}

/* Reassembly: BLE writes arrive in MTU-sized chunks, so lines must be stitched back. */
static char s_line[NUS_LINE_MAX];
static size_t s_line_len;

/*
 * One line at a time on the wire. ble_nus_send_line runs from three tasks - the NimBLE host
 * for command replies, the radio task for received packets, the pair button for FORGOTTEN -
 * and a line reaches the phone as several notifications. Without this, two of them interleave
 * and the phone reassembles one unparseable line out of both.
 */
static SemaphoreHandle_t s_send_lock;
#define SEND_LOCK_WAIT_MS 500
/* Once a line has overflowed, everything up to the next newline is rubbish too. Without this
 * the tail of an oversized line gets dispatched as if it were a command of its own. */
static bool s_discarding;

static void advertise(void);
static void forget_from_button(struct ble_npl_event *event);
static void kick(struct ble_npl_event *event);

/** True when a bond already exists and this is not it. */
static bool is_stranger(uint16_t conn_handle) {
    if (s_clear_in_progress || s_restart_pending) {
        ESP_LOGW(TAG, "bond reset in progress; refusing new connection");
        return true;
    }
    if (s_store_indeterminate) {
        ESP_LOGW(TAG, "bond store outcome unknown; refusing new connection");
        return true;
    }
    ble_addr_t bonded[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    int rc = ble_store_util_bonded_peers(bonded, &count,
                                         sizeof(bonded) / sizeof(bonded[0]));
    if (rc != 0) {
        /* An unreadable store is not proof that nobody owns the bridge. Failing open here
         * would turn a transient NVS error into permission for a new phone to take it. */
        ESP_LOGW(TAG, "cannot enumerate bonded peers (%d); refusing connection", rc);
        return true;
    }
    if (count == 0) return false;   /* nobody has claimed this bridge yet */

    struct ble_gap_conn_desc desc;
    rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "cannot identify peer (%d); refusing connection", rc);
        return true;
    }
    for (int i = 0; i < count; i++) {
        if (memcmp(&bonded[i], &desc.peer_id_addr, sizeof(ble_addr_t)) == 0) return false;
    }
    return true;
}

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
            ESP_LOGW(TAG, "line overflow, discarding to end of line");
            s_discarding = true;
            s_line_len = 0;
        }
    }
}

static int on_rx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    disconnect_target_t current;
    if (!connection_snapshot(&current) || current.conn != conn) {
        /* Termination is cleanup, not authorization: a refused stranger must remain powerless
         * even if the controller has not processed (or refused) our disconnect yet. */
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    /* Bond removal revokes the existing encrypted session immediately. FORGOTTEN gets the
     * restart window for delivery, but that is not permission for more TX or CFG commands. */
    if (s_current_revoked) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    atomic_store(&s_heard_s, uptime_s());
    struct os_mbuf *om = ctxt->om;
    while (om) {
        feed(om->om_data, om->om_len);
        if (s_current_revoked) break;
        om = SLIST_NEXT(om, om_next);
    }
    return 0;
}

static int on_tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0; /* notify-only */
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &UUID_RX.u,
        .access_cb = on_rx,
        /*
         * WRITE_ENC is the whole point: commands - CFG, TX, FORGET - are only accepted over an
         * encrypted link, which means a peer that has bonded. Notifications are deliberately
         * left open, because everything they carry also goes out over the air in clear, so
         * locking them would cost compatibility for no privacy gained.
         */
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &UUID_TX.u,
        .access_cb = on_tx_access,
        .val_handle = &s_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int on_gap(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                if (is_stranger(event->connect.conn_handle)) {
                    ESP_LOGW(TAG, "rejecting unpaired peer; bridge is already claimed");
                    int rc = ble_gap_terminate(event->connect.conn_handle,
                                               BLE_ERR_REM_USER_CONN_TERM);
                    if (rc != 0) {
                        ESP_LOGE(TAG, "could not terminate refused peer (%d); writes stay denied", rc);
                    }
                    break;
                }
                /* The generation distinguishes a genuinely new connection even if the
                 * controller immediately reuses the same 16-bit handle. */
                connection_set(event->connect.conn_handle);
                atomic_store(&s_heard_s, uptime_s());
                s_line_len = 0;
                s_discarding = false;
                /* MTU is negotiated by the central: the largest current wire packet is 200
                 * base64 characters, so the phone must request more than the 23-byte default.
                 * We only advertise a large preferred MTU (see sdkconfig.defaults). */
                ESP_LOGI(TAG, "connected");
            } else {
                advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGI(TAG, "disconnected");
            bool clear_for_button = s_clear_after_disconnect &&
                                    event->disconnect.conn.conn_handle == s_clear_target.conn &&
                                    connection_is_current(&s_clear_target);
            /* A late event for a refused/old peer must not erase a newer live handle. */
            bool was_current = connection_clear_if(event->disconnect.conn.conn_handle);
            /* Work accepted by a BLE session belongs to that session. A normal link loss must
             * fence its queued TX just as explicitly as FORGET does; otherwise the queue can
             * continue transmitting for seconds after its owner has gone away. */
            if (was_current) revoke_current_session();
            if (clear_for_button && was_current) {
                s_clear_after_disconnect = false;
                ble_nus_forget_bonds();
            }
            /* A refused stranger was never installed as s_conn, but its connection still
             * stopped advertising. Restart once it is gone unless another peer is live. */
            if (was_current || atomic_load(&s_conn) == BLE_HS_CONN_HANDLE_NONE) advertise();
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                ESP_LOGI(TAG, "encryption on");
            } else {
                /* A non-zero event is not proof that the key is stale. NimBLE can report a
                 * timeout even when the connection descriptor already says encrypted; deleting
                 * the owner's bond here would reopen a claimed bridge after a transient error.
                 * Repair is deliberately physical/authorised: FORGET or the runtime BOOT hold. */
                struct ble_gap_conn_desc desc;
                int find_rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
                if (find_rc != 0) {
                    ESP_LOGW(TAG, "encryption event failed (%d), peer lookup failed (%d); "
                                  "bond kept", event->enc_change.status, find_rc);
                } else {
                    ESP_LOGW(TAG, "encryption event failed (%d), encrypted=%u bonded=%u; bond kept",
                             event->enc_change.status, (unsigned)desc.sec_state.encrypted,
                             (unsigned)desc.sec_state.bonded);
                }
            }
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* Just Works authenticates no human. Automatically replacing an existing key here
             * lets a peer that presents the owner's identity address erase the real owner and
             * claim the board. Only encrypted FORGET or the physical runtime button may do so. */
            ESP_LOGW(TAG, "repeat pairing refused; use authenticated FORGET or hold BOOT 3 s");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu now %d", event->mtu.value);
            break;
        default:
            break;
    }
    return 0;
}

static void advertise(void) {
    if (s_restart_pending) return;
    /* A 128-bit UUID is 18 bytes and flags are 3; adding the 30-byte complete-name field is
     * past the 31-byte legacy advertising limit. NimBLE then rejects the whole set and the
     * device advertises nothing the phone can match, since the phone scans by UUID. So the
     * UUID goes in the advertisement and the name in the scan response. */
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name = (uint8_t *)s_name;
    rsp_fields.name_len = strlen(s_name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) ESP_LOGW(TAG, "adv_rsp_set_fields failed: %d", rc);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, on_gap, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start failed: %d", rc);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_nus_init(ble_nus_line_cb on_line, ble_nus_revoke_cb on_revoke,
                  ble_nus_revoke_cb on_bonds_cleared) {
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(s_name_buf, sizeof(s_name_buf), "%s-%02X%02X%02X%02X", DEVICE_NAME_PREFIX,
             mac[2], mac[3], mac[4], mac[5]);
    s_name = s_name_buf;
    s_on_line = on_line;
    s_on_revoke = on_revoke;
    s_on_bonds_cleared = on_bonds_cleared;

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;

    /*
     * Just Works pairing: the bridge has no display or keypad, so there is nothing to compare.
     * That gives encryption without man-in-the-middle protection, and on its own would let
     * anyone pair - so the first phone to bond claims the bridge and the rest are refused.
     */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    s_send_lock = xSemaphoreCreateMutex();
    configASSERT(s_send_lock != NULL);
    ble_npl_event_init(&s_forget_event, forget_from_button, NULL);
    ble_npl_event_init(&s_kick_event, kick, NULL);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(s_name));
    ESP_LOGI(TAG, "advertising as %s", s_name);

    nimble_port_freertos_init(host_task);
}

bool ble_nus_connected(void) {
    disconnect_target_t target;
    return connection_snapshot(&target);
}

static void disconnect_if_current(const disconnect_target_t *target) {
    if (connection_is_current(target)) {
        ble_gap_terminate(target->conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* ble_store_clear mutates RAM before persisting each record. If OUR_SEC persistence fails, a
 * plain retry sees empty RAM and can falsely succeed while the old owner remains in NVS. Retry
 * that exact persistence step, then read back the RAM fact that controls current ownership. */
static bool clear_bond_store(void) {
    int clear_rc = ble_store_clear();
    /* Always run this, including after a nominally successful retry: an earlier failed clear
     * may already have emptied RAM, in which case ble_store_clear has nothing left to persist. */
    /* A GATT command may already run under the host mutex; the nested variant is valid both
     * there and for the physical-reset event when the port invokes it without that lock. */
    ble_hs_lock_nested();
    int persist_rc = ble_store_config_persist_our_secs();
    ble_hs_unlock_nested();
    ble_addr_t bonded[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    int read_rc = ble_store_util_bonded_peers(bonded, &count,
                                              sizeof(bonded) / sizeof(bonded[0]));
    if (persist_rc == 0 && read_rc == 0 && count == 0) {
        s_store_indeterminate = false;
        if (clear_rc != 0) {
            ESP_LOGW(TAG, "bond owner cleared; non-owner store clear stopped with %d", clear_rc);
        }
        return true;
    }
    ESP_LOGE(TAG, "bond clear incomplete (clear=%d owner-persist=%d read=%d owners=%d)",
             clear_rc, persist_rc, read_rc, count);
    s_store_indeterminate = true;
    return false;
}

/* Complete the destructive half only after any in-flight pairing is gone. A non-null target
 * means this is an already-bonded session that can receive the success/failure result. */
static void finish_forget(const disconnect_target_t *notify_target) {
    s_clear_in_progress = true;
    bool cleared = clear_bond_store();
    s_clear_in_progress = false;
    if (!cleared) {
        /* The current encrypted session remains the only reliable recovery path when a
         * multi-record clear was partial. Do not claim the old key was kept; ask for a retry. */
        ESP_LOGE(TAG, "could not verify an empty bond store; keeping any recovery link");
        if (notify_target && connection_is_current(notify_target)) {
            ble_nus_send_line("ERR forget incomplete");
        }
        return;
    }
    ESP_LOGW(TAG, "pairings cleared");
    led_bond_cleared();
    revoke_current_session();
    s_restart_pending = true;
    if (s_on_bonds_cleared) s_on_bonds_cleared();

    if (!notify_target || !connection_is_current(notify_target)) return;

    /* The success event is also the phone's permission to delete its system bond. A successful
     * notify call only queues an unacknowledged notification, so do not tear the connection
     * down on a guessed 300 ms timer. The common four-second restart path is the eventual
     * disconnect and gives the controller several intervals to deliver it. The revoked session
     * cannot issue more commands while it waits. */
    if (!ble_nus_send_line("FORGOTTEN")) {
        ESP_LOGE(TAG, "pairings cleared but FORGOTTEN could not be queued; restart will disconnect");
    }
}

void ble_nus_forget_bonds(void) {
    disconnect_target_t current;
    if (!connection_snapshot(&current)) {
        finish_forget(NULL);
        return;
    }

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(current.conn, &desc);
    if (rc == 0 && desc.sec_state.encrypted && desc.sec_state.bonded) {
        /* Pairing is complete, so no later security step can recreate the key. Keep the live
         * link just long enough to deliver FORGOTTEN and let Android clear its side too. */
        finish_forget(&current);
        return;
    }

    /* WRITE_ENC can admit FORGET after encryption starts but before key distribution has
     * finished. Clearing now would race the remaining pairing procedure, which could persist
     * the owner again after FORGOTTEN. Stop that procedure first; its matching DISCONNECT event
     * performs the durable clear, with no success notification claimed across the dead link. */
    ESP_LOGW(TAG, "bond reset requested during incomplete pairing; disconnecting first");
    s_clear_after_disconnect = true;
    revoke_current_session();
    /* Revocation invalidates pending radio work before the pairing handshake is stopped. It
     * does not change the connection target itself, so the original generation remains valid. */
    s_clear_target = current;
    rc = ble_gap_terminate(current.conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        return;
    }
    if (rc == BLE_HS_ENOTCONN) {
        s_clear_after_disconnect = false;
        connection_clear_if(current.conn);
        finish_forget(NULL);
        return;
    }
    /* Keep both revocation and the pending clear: a natural DISCONNECT can still finish the
     * physical reset safely. Another button event may retry if the controller stays wedged. */
    ESP_LOGE(TAG, "cannot stop in-flight pairing for bond reset (%d); clear pending", rc);
}

/* On the host task, for ble_nus_supervise: drop a link the phone has gone silent on, or start
 * advertising again if nothing is. Either way the stack's own events take it from there. */
static void kick(struct ble_npl_event *event) {
    (void)event;
    disconnect_target_t current;
    if (connection_snapshot(&current)) {
        ESP_LOGW(TAG, "phone silent for %u s on a live link - dropping it",
                 (unsigned)(uptime_s() - atomic_load(&s_heard_s)));
        disconnect_if_current(&current);
    } else if (!ble_gap_adv_active()) {
        ESP_LOGW(TAG, "neither connected nor advertising - advertising again");
        advertise();
    }
}

bool ble_nus_supervise(void) {
    static uint32_t stuck_since;   /* 0: nothing wrong */
    /* Both flags are written by the host task; a stale read only delays this by a second. */
    if (s_restart_pending || s_clear_in_progress) {
        stuck_since = 0;
        return false;
    }
    const uint32_t now = uptime_s();
    const bool stuck = ble_nus_connected() ? now - atomic_load(&s_heard_s) > SILENT_LINK_S
                                           : !ble_gap_adv_active();
    if (!stuck) {
        stuck_since = 0;
        return false;
    }
    if (stuck_since == 0) {
        stuck_since = now ? now : 1;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_kick_event);
        return false;
    }
    if (now - stuck_since < STUCK_RESTART_S) return false;
    ESP_LOGE(TAG, "bluetooth still stuck after %u s", (unsigned)(now - stuck_since));
    return true;
}

static void forget_from_button(struct ble_npl_event *event) {
    (void)event;
    ble_nus_forget_bonds();
}

bool ble_nus_request_forget_bonds(void) {
    struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
    if (!queue) {
        ESP_LOGE(TAG, "cannot schedule physical bond reset: host queue unavailable");
        return false;
    }
    /* The NPL queue itself ignores a duplicate put, so a held-button action stays one event. */
    ble_npl_eventq_put(queue, &s_forget_event);
    return true;
}

bool ble_nus_send_line(const char *line) {
    if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(SEND_LOCK_WAIT_MS)) != pdTRUE) {
        /* Half a second is far longer than a line takes; something is wedged, and dropping
         * this line is better than queueing behind it for ever. */
        ESP_LOGW(TAG, "outbound busy - line dropped");
        return false;
    }
    /* Keep one handle for the whole fragmented line. A disconnect/reconnect in the middle
     * must not send the tail of an old peer's line to the new peer. */
    disconnect_target_t target;
    if (!connection_snapshot(&target)) {
        xSemaphoreGive(s_send_lock);
        return false;
    }
    uint16_t conn = target.conn;

    /* A notification carries at most ATT_MTU-3 bytes and the stack silently truncates the
     * rest. Lines here can reach hundreds of characters, and if MTU negotiation failed we are on
     * the 23-byte default, so fragmenting is not optional. The receiver reassembles on the
     * newline, which is why the terminator must go out in the final fragment. */
    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    size_t len = strlen(line);
    size_t sent = 0;
    bool terminated = false;
    bool torn = false;

    while (sent < len) {
        if (!connection_is_current(&target)) {
            torn = (sent > 0);
            break;
        }
        size_t take = len - sent;
        if (take > chunk) take = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line + sent, take);
        if (!om) {
            torn = (sent > 0);
            break;
        }
        sent += take;
        if (sent == len && take < chunk) {
            /* An append that fails leaves the line unterminated, so it must not be recorded
             * as terminated - the standalone newline below is then the only delimiter. */
            terminated = (os_mbuf_append(om, "\n", 1) == 0);
        }
        if (ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) {
            torn = true;
            terminated = false;   /* whatever went out, the phone has no end of line yet */
            break;
        }
    }

    bool resync = false;
    if (!terminated && connection_is_current(&target)) {
        /* Either the terminator did not fit in the final fragment, or the line was cut short
         * partway. Both need the newline: without it the phone glues the next line onto the
         * tail of this one and parses neither. */
        struct os_mbuf *om = ble_hs_mbuf_from_flat("\n", 1);
        if (!om || ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) {
            /* Whatever exhausted the buffers a moment ago has just done it again, which is
             * the likely case rather than the unlucky one. If fragments of this line already
             * reached the phone there is now no way to close it, so the only honest recovery
             * is to drop the link: the phone clears its reassembly buffer on reconnect. */
            /* Anything already delivered leaves the phone holding the front of a line with
             * no end to it - including a line whose length happened to be an exact multiple
             * of the fragment size, which needs a delimiter of its own without ever being
             * torn. What matters is that bytes went out, not how they ended. */
            resync = (sent > 0);
        } else {
            terminated = true;
        }
    }
    bool queued_whole = !torn && sent == len && terminated;
    if (torn) ESP_LOGW(TAG, "outbound line cut short");
    xSemaphoreGive(s_send_lock);
    if (resync) {
        ESP_LOGW(TAG, "no delimiter after a torn line - dropping the link to resynchronise");
        disconnect_if_current(&target);
    }
    return queued_whole;
}
