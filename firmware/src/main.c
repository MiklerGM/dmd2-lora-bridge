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
 *   STATUS               ST conn=1 ch=18 rate=2 power=3 lbt=0 echo=0 fw=...
 *   ECHO 0|1             OK                 simulate a radio by looping TX back as RX
 *   PING <n>             PONG <n chars>     transport test: forces a long, fragmented line
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

/*
 * Weakest power index the firmware will accept (0=30, 1=27, 2=24, 3=21 dBm). The BLE command
 * surface is unauthenticated - anything in range can talk to it - so the regulatory limit is
 * enforced on the device, not left to a well-behaved client. Raise deliberately, and only to
 * what is licensed where the bridge is used.
 */
#define MIN_POWER_INDEX 3

static e220_config_t s_cfg;
static char s_fw[48] = "";
static QueueHandle_t s_tx_queue; /* char* owned by the queue */

/*
 * Echo (simulation) mode: a transmit is looped straight back as a receive instead of going
 * to the radio. It lets the whole chain - phone, BLE, framing, codec, injection into DMD2 -
 * be exercised on a bare ESP32 with no LoRa module attached. Enabled automatically when no
 * module answers at boot, which is exactly the bench case.
 */
static bool s_echo = false;

/* ---- command handling ---------------------------------------------------- */

static void send_status(void) {
    char out[160];
    snprintf(out, sizeof(out),
             "ST conn=1 ch=%u freq=%.3f rate=%u power=%u packet=%u trans=%u lbt=%u addr=%u "
             "echo=%u fw=%s",
             s_cfg.channel, 850.125 + (double)s_cfg.channel, s_cfg.air_rate, s_cfg.power,
             s_cfg.packet_size, (unsigned)s_cfg.transparent, (unsigned)s_cfg.lbt,
             s_cfg.address, (unsigned)s_echo, s_fw[0] ? s_fw : "?");
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
        /* Index 0 is 30 dBm. Anyone in Bluetooth range can send CFG, so the limit has to live
         * here as well as on the phone - a clamp only one side enforces is not a clamp. */
        if (v < MIN_POWER_INDEX) {
            ESP_LOGW(TAG, "power index %ld requested, clamped to %d", v, MIN_POWER_INDEX);
            v = MIN_POWER_INDEX;
        }
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

    if (s_echo) {
        // Nothing to configure; a simulated radio accepts the settings so the phone can
        // confirm them and the rest of the chain behaves exactly as it will with hardware.
        s_cfg = next;
        ble_nus_send_line("OK");
        send_status();
        return;
    }
    if (e220_apply(&next)) {
        s_cfg = next;
        if (s_echo) {
            // A real module answered, so stop pretending to be one - otherwise every
            // transmission keeps looping back to the phone and never reaches the air.
            s_echo = false;
            ESP_LOGI(TAG, "module answered - echo (simulation) mode OFF");
        }
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
    if (s_echo) {
        /* Bounce it back verbatim. The phone is responsible for making a looped packet look
         * like it came from a peer; the bridge stays a dumb pipe even when simulating. */
        static char line[OUT_LINE_MAX];
        snprintf(line, sizeof(line), "RX %s", b64);
        ble_nus_send_line(line);
        ESP_LOGI(TAG, "echo %u bytes: %.12s...", (unsigned)len, b64);
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

/* Replies with a line of n filler characters. Lines here can reach ~250 bytes, well past a
 * default 23-byte MTU, so this is the quickest way to prove fragmentation and reassembly. */
static void handle_ping(const char *arg) {
    long n = strtol(arg, NULL, 10);
    if (n < 1) n = 1;
    if (n > OUT_LINE_MAX - 16) n = OUT_LINE_MAX - 16;
    static char line[OUT_LINE_MAX];
    int at = snprintf(line, sizeof(line), "PONG ");
    for (long i = 0; i < n && at < (int)sizeof(line) - 1; i++) line[at++] = 'A' + (char)(i % 26);
    line[at] = '\0';
    ble_nus_send_line(line);
}

static void on_ble_line(const char *line, size_t len) {
    (void)len;
    if (strncmp(line, "TX ", 3) == 0) {
        handle_tx(line + 3);
    } else if (strncmp(line, "CFG", 3) == 0) {
        handle_cfg(line + 3);
    } else if (strcmp(line, "STATUS") == 0) {
        send_status();
    } else if (strncmp(line, "ECHO ", 5) == 0) {
        s_echo = (strtol(line + 5, NULL, 10) != 0);
        ESP_LOGI(TAG, "echo mode %s", s_echo ? "on" : "off");
        ble_nus_send_line("OK");
        send_status();
    } else if (strncmp(line, "PING ", 5) == 0) {
        handle_ping(line + 5);
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
            /* A DMD packet starts 0xD2 0x03, which is "0g" in base64 - a wrong or foreign
             * payload is obvious at a glance. */
            ESP_LOGI(TAG, "tx %u bytes: %.12s...", (unsigned)len, payload);
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
        ESP_LOGI(TAG, "rx %u bytes: %.12s...", (unsigned)n, (const char *)buf);
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
    if (s_cfg.power < MIN_POWER_INDEX) s_cfg.power = MIN_POWER_INDEX;
    s_tx_queue = xQueueCreate(8, sizeof(char *));

    e220_init();
    e220_read_firmware(s_fw, sizeof(s_fw));
    if (!e220_apply(&s_cfg)) {
        /* Almost certainly no module wired up. Simulate one so the rest of the chain can be
         * tested on a bare board; a real module will answer and this stays off. */
        s_echo = true;
        ESP_LOGW(TAG, "no module acknowledged - echo (simulation) mode ON");
    }

    ble_nus_init("DMD-LoRa-Bridge", on_ble_line);

    xTaskCreate(tx_task, "lora_tx", 4096, NULL, 6, NULL);
    xTaskCreate(rx_task, "lora_rx", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "bridge up: ch=%u %.3f MHz, power index %u, echo=%d",
             s_cfg.channel, 850.125 + (double)s_cfg.channel, s_cfg.power, (int)s_echo);
}
