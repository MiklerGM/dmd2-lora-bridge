/*
 * DMD2 LoRa bridge — ESP32 firmware.
 *
 * A deliberately dumb pipe: BLE on one side, an EBYTE E220 on the other. It never parses a
 * DMD packet. The phone owns the codec, the group id and all session state; see
 * docs/ARCHITECTURE.md section 2.
 *
 * Line protocol over BLE (Nordic UART Service), newline delimited:
 *
 *   phone -> bridge      bridge -> phone
 *   ------------------   -------------------------------------
 *   TX <base64>          RX <base64>        one packet off the air
 *   CFG k=v k=v ...      OK / ERR <reason>
 *   STATUS               ST conn=1 ch=18 rate=2 power=3 lbt=0 fw=...
 *
 * The payload the module carries is already base64 ASCII, because that is what DMD writes to
 * its module's UART. So nothing here encodes or decodes anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_nus.h"
#include "e220.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "bridge"
#define PACKET_MAX 256
#define OUT_LINE_MAX 512
#define IDLE_GAP_MS 50
#define RX_POLL_MS 100

static e220_config_t s_cfg;
static char s_fw[48] = "";
static QueueHandle_t s_tx_queue; /* char* owned by the queue */

/* ---- command handling ---------------------------------------------------- */

static void send_status(void) {
    char out[160];
    snprintf(out, sizeof(out),
             "ST conn=1 ch=%u freq=%.3f rate=%u power=%u packet=%u trans=%u lbt=%u addr=%u fw=%s",
             s_cfg.channel, 850.125 + (double)s_cfg.channel, s_cfg.air_rate, s_cfg.power,
             s_cfg.packet_size, (unsigned)s_cfg.transparent, (unsigned)s_cfg.lbt,
             s_cfg.address, s_fw[0] ? s_fw : "?");
    ble_nus_send_line(out);
}

static bool kv_int(const char *args, const char *key, long *out) {
    size_t klen = strlen(key);
    for (const char *p = args; *p; p++) {
        if ((p == args || p[-1] == ' ') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            *out = strtol(p + klen + 1, NULL, 10);
            return true;
        }
    }
    return false;
}

/* Range must be checked on the parsed value, not after narrowing: ch=274 truncates to 18,
 * which then passes a check against 80 and silently configures the wrong channel. */
static bool in_range(long v, long lo, long hi) { return v >= lo && v <= hi; }

static void handle_cfg(const char *args) {
    e220_config_t next = s_cfg;
    long v;
    if (kv_int(args, "addr", &v)) {
        if (!in_range(v, 0, 0xFFFF)) goto bad_range;
        next.address = (uint16_t)v;
    }
    if (kv_int(args, "ch", &v)) {
        if (!in_range(v, 0, 80)) goto bad_range;
        next.channel = (uint8_t)v;
    }
    if (kv_int(args, "rate", &v)) {
        if (!in_range(v, 0, 7)) goto bad_range;
        next.air_rate = (uint8_t)v;
    }
    if (kv_int(args, "power", &v)) {
        if (!in_range(v, 0, 3)) goto bad_range;
        next.power = (uint8_t)v;
    }
    if (kv_int(args, "packet", &v)) {
        if (!in_range(v, 0, 3)) goto bad_range;
        next.packet_size = (uint8_t)v;
    }
    if (kv_int(args, "key", &v)) {
        if (!in_range(v, 0, 0xFFFF)) goto bad_range;
        next.key = (uint16_t)v;
    }
    if (kv_int(args, "trans", &v)) next.transparent = (v != 0);
    if (kv_int(args, "lbt", &v)) next.lbt = (v != 0);

    if (e220_apply(&next)) {
        s_cfg = next;
        ble_nus_send_line("OK");
        send_status();
    } else {
        ble_nus_send_line("ERR module did not acknowledge");
    }
    return;

bad_range:
    ble_nus_send_line("ERR range");
}

static void handle_tx(const char *b64) {
    size_t len = strlen(b64);
    if (len == 0 || len > PACKET_MAX) {
        ble_nus_send_line("ERR tx length");
        return;
    }
    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, b64, len + 1);
    if (xQueueSend(s_tx_queue, &copy, 0) != pdTRUE) {
        free(copy);
        ble_nus_send_line("ERR tx queue full");
    }
}

static void on_ble_line(const char *line, size_t len) {
    (void)len;
    if (strncmp(line, "TX ", 3) == 0) {
        handle_tx(line + 3);
    } else if (strncmp(line, "CFG", 3) == 0) {
        handle_cfg(line + 3);
    } else if (strcmp(line, "STATUS") == 0) {
        send_status();
    } else {
        ble_nus_send_line("ERR unknown command");
    }
}

/* ---- radio tasks --------------------------------------------------------- */

/* Serialising transmission in one task keeps two BLE writes from interleaving on the UART,
 * which would merge them into a single corrupt air packet. */
static void tx_task(void *arg) {
    (void)arg;
    char *payload;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &payload, portMAX_DELAY) == pdTRUE) {
            size_t len = strlen(payload);
            /* No terminator: the module closes the packet on the UART idle gap, so a stray
             * newline here would travel as part of the payload and corrupt it for everyone. */
            e220_write((const uint8_t *)payload, len);
            ESP_LOGI(TAG, "tx %u bytes", (unsigned)len);
            free(payload);
            /* e220_write already blocked until the module returned to idle (AUX high), so no
             * fixed settling delay is needed here; pacing is the phone's job. */
            taskYIELD();
        }
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[PACKET_MAX + 1];
    static char line[OUT_LINE_MAX];
    for (;;) {
        /* A bounded wait, so configuration can take the port between packets. */
        size_t n = e220_read_packet(buf, PACKET_MAX, IDLE_GAP_MS, RX_POLL_MS);
        if (n == 0) continue;
        buf[n] = '\0';
        ESP_LOGI(TAG, "rx %u bytes", (unsigned)n);
        if (!ble_nus_connected()) continue;
        snprintf(line, sizeof(line), "RX %s", (const char *)buf);
        ble_nus_send_line(line);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_cfg = E220_GROUP_CONFIG;
    s_tx_queue = xQueueCreate(8, sizeof(char *));

    e220_init();
    e220_read_firmware(s_fw, sizeof(s_fw));
    if (!e220_apply(&s_cfg)) {
        ESP_LOGW(TAG, "module did not acknowledge the initial config");
    }

    ble_nus_init("DMD-LoRa-Bridge", on_ble_line);

    xTaskCreate(tx_task, "lora_tx", 4096, NULL, 6, NULL);
    xTaskCreate(rx_task, "lora_rx", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "bridge up: ch=%u %.3f MHz, power index %u",
             s_cfg.channel, 850.125 + (double)s_cfg.channel, s_cfg.power);
}
