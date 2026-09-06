#include "ble_nus.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

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

#define LINE_MAX 512

static ble_nus_line_cb s_on_line;
static const char *s_name;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;

/* Reassembly: BLE writes arrive in MTU-sized chunks, so lines must be stitched back. */
static char s_line[LINE_MAX];
static size_t s_line_len;

static void advertise(void);

static void feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                if (s_on_line) s_on_line(s_line, s_line_len);
                s_line_len = 0;
            }
        } else if (s_line_len < LINE_MAX - 1) {
            s_line[s_line_len++] = c;
        } else {
            ESP_LOGW(TAG, "line overflow, dropping");
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
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
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
                s_conn = event->connect.conn_handle;
                s_line_len = 0;
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
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu now %d", event->mtu.value);
            break;
        default:
            break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, on_gap, NULL);
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
    s_name = device_name;
    s_on_line = on_line;

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(device_name));

    nimble_port_freertos_init(host_task);
}

bool ble_nus_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

void ble_nus_send_line(const char *line) {
    if (!ble_nus_connected()) return;
    size_t len = strlen(line);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, len);
    if (!om) return;
    /* Append the delimiter in the same buffer so a line is never split across notifications
     * in a way that loses its terminator. */
    os_mbuf_append(om, "\n", 1);
    ble_gatts_notify_custom(s_conn, s_tx_handle, om);
}
