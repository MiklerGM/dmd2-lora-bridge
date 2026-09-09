#include "ble_nus.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "driver/gpio.h"

/* Provided by the NimBLE port; the ESP-IDF examples forward-declare it the same way. */
void ble_store_config_init(void);

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

static ble_nus_line_cb s_on_line;
static const char *s_name;
/*
 * Every bridge advertised the same name, so on a group ride one rider's phone could grab
 * another rider's bridge - and since only one central may connect, the other rider would be
 * left with no radio at all. The name now carries the last two bytes of this board's MAC so
 * each bridge is individually addressable, while the shared prefix keeps them all discoverable.
 */
static char s_name_buf[32];
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;

/* Reassembly: BLE writes arrive in MTU-sized chunks, so lines must be stitched back. */
static char s_line[NUS_LINE_MAX];
static size_t s_line_len;

/*
 * One line at a time on the wire. ble_nus_send_line runs from three tasks - the NimBLE host
 * for command replies, the radio task for received packets, the pair button for FORGET - and a
 * line reaches the phone as several notifications. Without this, two of them interleave and
 * the phone reassembles one unparseable line out of both.
 */
static SemaphoreHandle_t s_send_lock;
#define SEND_LOCK_WAIT_MS 500
/* Once a line has overflowed, everything up to the next newline is rubbish too. Without this
 * the tail of an oversized line gets dispatched as if it were a command of its own. */
static bool s_discarding;

static void advertise(void);

/* Boot with this pin pulled low to forget the paired phone - the way back from a lost or
 * replaced handset, which would otherwise leave the bridge permanently unusable. */
#ifndef PIN_BOND_RESET
#define PIN_BOND_RESET 0
#endif

/** True when a bond already exists and this is not it. */
static bool is_stranger(uint16_t conn_handle) {
    ble_addr_t bonded[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    if (ble_store_util_bonded_peers(bonded, &count, sizeof(bonded) / sizeof(bonded[0])) != 0) {
        return false;
    }
    if (count == 0) return false;   /* nobody has claimed this bridge yet */

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
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
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    struct os_mbuf *om = ctxt->om;
    while (om) {
        feed(om->om_data, om->om_len);
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
         * WRITE_ENC is the whole point: commands - CFG, TX, ECHO - are only accepted over an
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
                    ble_gap_terminate(event->connect.conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                s_conn = event->connect.conn_handle;
                s_line_len = 0;
                s_discarding = false;
                /* MTU is negotiated by the central: a full packet is ~270 base64
                 * characters, so the phone must request more than the 23-byte default.
                 * We only advertise a large preferred MTU (see sdkconfig.defaults). */
                ESP_LOGI(TAG, "connected");
            } else {
                advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected");
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                ESP_LOGI(TAG, "encryption on");
            } else {
                /*
                 * The stored key did not work, so it is worthless: the phone has forgotten us
                 * and will not offer a matching one again. Holding on to it means every future
                 * connection tries to encrypt with a key nobody else has, no pairing is ever
                 * started, and the link simply dies of supervision timeout - which is exactly
                 * the loop this produced. Drop it and let the next connection pair cleanly.
                 */
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                ESP_LOGW(TAG, "encryption FAILED - stale key dropped, will pair afresh");
            }
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* The phone forgot us but we still hold its key: drop the stale bond and let it
             * pair again, rather than refusing forever. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
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
    /* A 128-bit UUID is 18 bytes and flags are 3; together with a 15-char name that is 38,
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

void ble_nus_init(const char *device_name, ble_nus_line_cb on_line) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name_buf, sizeof(s_name_buf), "%s-%02X%02X", device_name, mac[4], mac[5]);
    s_name = s_name_buf;
    s_on_line = on_line;

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

    gpio_config_t boot_pin = {
        .pin_bit_mask = (1ULL << PIN_BOND_RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_pin);
    if (gpio_get_level((gpio_num_t)PIN_BOND_RESET) == 0) {
        ble_store_clear();
        ESP_LOGW(TAG, "boot pin held low - paired phones forgotten");
    }

    s_send_lock = xSemaphoreCreateMutex();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(s_name));
    ESP_LOGI(TAG, "advertising as %s", s_name);

    nimble_port_freertos_init(host_task);
}

bool ble_nus_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

void ble_nus_forget_bonds(void) {
    ble_store_clear();
    ESP_LOGW(TAG, "pairings cleared on request");
    if (ble_nus_connected()) {
        /* Drop the link too: the peer we were just talking to is no longer trusted. */
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_nus_send_line(const char *line) {
    if (!ble_nus_connected()) return;
    if (s_send_lock && xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(SEND_LOCK_WAIT_MS)) != pdTRUE) {
        /* Half a second is far longer than a line takes; something is wedged, and dropping
         * this line is better than queueing behind it for ever. */
        ESP_LOGW(TAG, "outbound busy - line dropped");
        return;
    }

    /* A notification carries at most ATT_MTU-3 bytes and the stack silently truncates the
     * rest. Lines here reach ~250 characters, and if the MTU request failed we are still on
     * the 23-byte default, so fragmenting is not optional. The receiver reassembles on the
     * newline, which is why the terminator must go out in the final fragment. */
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    size_t len = strlen(line);
    size_t sent = 0;
    bool terminated = false;
    bool torn = false;

    while (sent < len) {
        size_t take = len - sent;
        if (take > chunk) take = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line + sent, take);
        if (!om) {
            torn = (sent > 0);
            break;
        }
        sent += take;
        if (sent == len && take < chunk) {
            os_mbuf_append(om, "\n", 1);
            terminated = true;
        }
        if (ble_gatts_notify_custom(s_conn, s_tx_handle, om) != 0) {
            torn = true;
            terminated = false;   /* whatever went out, the phone has no end of line yet */
            break;
        }
    }

    if (!terminated) {
        /* Either the terminator did not fit in the final fragment, or the line was cut short
         * partway. Both need the newline: without it the phone glues the next line onto the
         * tail of this one and parses neither. */
        struct os_mbuf *om = ble_hs_mbuf_from_flat("\n", 1);
        if (om) ble_gatts_notify_custom(s_conn, s_tx_handle, om);
    }
    if (torn) ESP_LOGW(TAG, "outbound line cut short");
    if (s_send_lock) xSemaphoreGive(s_send_lock);
}
