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
 * conn= is the module, not the Bluetooth link: 0 means the E220 is not answering and nothing
 * this bridge is handed will reach the air.
 *
 * The payload the module carries is already base64 ASCII, because that is what DMD writes to
 * its module's UART. So nothing here encodes or decodes anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_nus.h"
#include "driver/gpio.h"
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
#define MODULE_RETRY_MS 5000
#define MODULE_PROBE_MS 30000

/*
 * Weakest power index this firmware will accept (0 is the strongest step, 3 the weakest; what
 * each means in dBm depends on the module variant). The phone owns the setting the rider
 * actually turns - see Prefs.powerIndex - and this is the floor underneath it, because the BLE
 * command surface is unauthenticated and anything in range can send a CFG.
 *
 * It is a build flag rather than a constant so a bridge can be built for a licence that allows
 * more: -DMIN_POWER_INDEX=0. Whatever it is set to, the bridge reports the power it actually
 * runs and the phone accepts a weaker one than it asked for, so lowering this ceiling never
 * deadlocks the two sides against each other.
 */
#ifndef MIN_POWER_INDEX
#define MIN_POWER_INDEX 3
#endif

/*
 * Hold this pin (the BOOT button on most boards) for a few seconds to hand the bridge to a
 * different phone. It is deliberately a button and not a timeout: the bike spends much of its
 * life parked and unattended, so "open up when the trusted phone is not around" would mean
 * "open up to whoever waits for the owner to leave". A button needs someone standing there.
 */
#ifndef PIN_PAIR_BUTTON
#define PIN_PAIR_BUTTON 0
#endif
#define PAIR_HOLD_MS 3000

static e220_config_t s_cfg;
static char s_fw[48] = "";
static QueueHandle_t s_tx_queue; /* char* owned by the queue */

/*
 * Echo (simulation) mode: a transmit is looped straight back as a receive instead of going
 * to the radio. It lets the whole chain - phone, BLE, framing, codec, injection into DMD2 -
 * be exercised on a bare ESP32 with no LoRa module attached.
 *
 * It is only ever switched on by an explicit ECHO 1. It used to turn itself on whenever the
 * module failed to answer, which meant a broken radio and a bench board were indistinguishable:
 * the bridge went on replying OK to every command and the phone showed a healthy green link
 * while not one packet reached the air. A simulator that starts itself is a liar.
 */
static bool s_echo = false;

/* Whether the module acknowledged the last configuration. Reported as conn=. */
static bool s_module_ok = false;

/*
 * A module that consistently reads its registers back as something other than what it accepted
 * would otherwise sit in a loop: verify says drifted, conn goes to 0, the phone re-sends its
 * configuration, the module writes its flash, and thirty seconds later the same thing again -
 * which is the flash wear the read-only probe exists to avoid, only slower. After a few of
 * those, believe the module rather than the comparison and go on using presence alone.
 */
#define MISMATCH_TOLERANCE 3
static int s_mismatch_streak = 0;
static bool s_trust_readback = true;

/* ---- command handling ---------------------------------------------------- */

static void send_status(void) {
    char out[160];
    snprintf(out, sizeof(out),
             "ST conn=%u ch=%u freq=%.3f rate=%u power=%u packet=%u trans=%u lbt=%u addr=%u "
             "echo=%u fw=%s",
             (unsigned)s_module_ok,
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

    /* Always ask the module, even while simulating: it is the only way conn= can ever go
     * back to 1 once a module is plugged in, and the old early return made that unreachable. */
    s_module_ok = e220_apply(&next);
    if (s_module_ok || s_echo) {
        /* A simulated radio accepts the settings so the phone can confirm them and the rest
         * of the chain behaves exactly as it will with hardware. */
        s_cfg = next;
        ble_nus_send_line(s_module_ok ? "OK" : "OK simulated");
    } else {
        ble_nus_send_line("ERR module did not acknowledge");
    }
    /* Either way: the status carries conn=, so a refusal is visible rather than just a line
     * the phone logs and forgets. */
    send_status();
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
    } else if (strcmp(line, "FORGET") == 0) {
        /* Both sides have to forget together. If only the phone dropped its bond, this bridge
         * would keep the old key, take the returning phone for a stranger and refuse it -
         * leaving the pair unusable until someone held the boot pin down. */
        ble_nus_send_line("OK");
        ble_nus_forget_bonds();
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

static void pair_button_task(void *arg) {
    (void)arg;
    const TickType_t poll = pdMS_TO_TICKS(100);
    int held_ms = 0;
    bool fired = false;
    for (;;) {
        if (gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON) == 0) {
            held_ms += 100;
            if (held_ms >= PAIR_HOLD_MS && !fired) {
                fired = true;   /* once per press, not once per poll */
                ESP_LOGW(TAG, "pair button held - forgetting the paired phone");
                /*
                 * Warn the phone first. If only this side forgets, the phone keeps offering a
                 * key we no longer hold, encryption fails and the pair drops into a loop that
                 * accepting the pairing dialog cannot break.
                 */
                if (ble_nus_connected()) {
                    ble_nus_send_line("FORGET");
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
                ble_nus_forget_bonds();
            }
        } else {
            held_ms = 0;
            fired = false;
        }
        vTaskDelay(poll);
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[PACKET_MAX + 1];
    static char line[OUT_LINE_MAX];
    TickType_t last_probe = xTaskGetTickCount();
    for (;;) {
        /*
         * Ask the module what it is holding, on a timer, whichever way it is currently believed
         * to be. A conn= that goes to 1 once and stays there for ever is the same lie as a
         * simulation that switches itself on: a module unplugged, browned out or reset since it
         * last worked answers nothing, and both the phone and DMD2 have to hear about that
         * rather than go on transmitting into a dead port.
         *
         * It reads (C1) and never writes. Re-applying the block would have answered the same
         * question, but C0 writes the module's flash - 2880 times a day at this interval - and
         * it would race a configuration change: a frame built from the old settings, applied
         * after the new ones landed, puts the radio back on the wrong channel while the status
         * says otherwise. Reading cannot do that. When the registers have drifted, conn goes to
         * 0 and the phone re-sends its configuration, which keeps one writer rather than two.
         *
         * Timed against the clock rather than counted in loop passes: those run far faster
         * than the poll interval whenever packets are actually arriving.
         */
        TickType_t due = pdMS_TO_TICKS(s_module_ok ? MODULE_PROBE_MS : MODULE_RETRY_MS);
        if (!s_echo && xTaskGetTickCount() - last_probe >= due) {
            last_probe = xTaskGetTickCount();
            e220_state_t st = e220_verify(&s_cfg);
            /* Busy means the port was in use, which says nothing about the module. */
            if (st != E220_BUSY) {
                if (st == E220_MISMATCH) {
                    if (++s_mismatch_streak >= MISMATCH_TOLERANCE && s_trust_readback) {
                        s_trust_readback = false;
                        ESP_LOGW(TAG, "module keeps reading back registers it accepted; "
                                      "checking presence only from now on");
                    }
                } else {
                    s_mismatch_streak = 0;
                }
                bool ok = (st == E220_OK) || (st == E220_MISMATCH && !s_trust_readback);
                if (ok != s_module_ok) {
                    s_module_ok = ok;
                    ESP_LOGW(TAG, "module %s", ok ? "answered - RF is up"
                             : (st == E220_MISMATCH ? "holds different registers - RF is down"
                                                    : "stopped answering - RF is down"));
                    send_status();
                }
            }
        }
        if (!s_module_ok && !s_echo) {
            /* Nothing to read from a module that is not there; wait for the next probe. */
            vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
            continue;
        }
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
    s_module_ok = e220_apply(&s_cfg);
    if (!s_module_ok) {
        /* Almost certainly no module wired up. Report it as conn=0 and keep trying - do not
         * quietly stand in for the radio, or a bench board and a broken one look the same. */
        ESP_LOGW(TAG, "no module acknowledged - conn=0, retrying; send ECHO 1 to simulate one");
    }

    ble_nus_init("DMD-LoRa-Bridge", on_ble_line);

    gpio_config_t pair_pin = {
        .pin_bit_mask = (1ULL << PIN_PAIR_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&pair_pin);
    xTaskCreate(pair_button_task, "pair_btn", 2560, NULL, 3, NULL);

    xTaskCreate(tx_task, "lora_tx", 4096, NULL, 6, NULL);
    xTaskCreate(rx_task, "lora_rx", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "bridge up: ch=%u %.3f MHz, power index %u, module=%d",
             s_cfg.channel, 850.125 + (double)s_cfg.channel, s_cfg.power, (int)s_module_ok);
}
