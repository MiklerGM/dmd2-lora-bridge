#include "e220.h"

#include <stdio.h>

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "e220"

/* Pins come from the build environment: the WROOM and the C3 have different GPIO maps,
 * and the C3 has no GPIO 16/17/22/23 at all. See platformio.ini. */
#define E220_UART UART_NUM_1

/* The part's rated transmit power in dBm, read from its product-information block. */
static int8_t s_rated_dbm = -1;
#ifndef PIN_E220_TX
#define PIN_E220_TX 17
#endif
#ifndef PIN_E220_RX
#define PIN_E220_RX 16
#endif
#ifndef PIN_E220_M0
#define PIN_E220_M0 21
#endif
#ifndef PIN_E220_M1
#define PIN_E220_M1 22
#endif
#ifndef PIN_E220_AUX
#define PIN_E220_AUX 23
#endif
#define PIN_TX ((gpio_num_t)PIN_E220_TX)
#define PIN_RX ((gpio_num_t)PIN_E220_RX)
#define PIN_M0 ((gpio_num_t)PIN_E220_M0)
#define PIN_M1 ((gpio_num_t)PIN_E220_M1)
#define PIN_AUX ((gpio_num_t)PIN_E220_AUX)

#define UART_BUF 1024
#define UART_LOCK_WAIT_MS 2000
#define AUX_POLL_MS 10
#define MODE_SWITCH_WAIT_MS 2000
/* LBT may legally defer a transmission for two seconds before the module sends anyway. The
 * completion wait also has to cover the packet's airtime and scheduler/module margin. */
#define TX_COMPLETE_WAIT_MS 5000

/*
 * One port, three users: the receive loop, the transmit task and configuration. Without this
 * lock a config write lands while the receive loop is blocked in a read, so the module's reply
 * is swallowed as if it were an air packet — and worse, a payload can be transmitted while
 * M0/M1 still hold the module in configuration mode.
 */
static SemaphoreHandle_t s_uart_lock;

static bool uart_lock(void) {
    return xSemaphoreTake(s_uart_lock, pdMS_TO_TICKS(UART_LOCK_WAIT_MS)) == pdTRUE;
}

static void uart_unlock(void) { xSemaphoreGive(s_uart_lock); }

static bool wait_aux(uint32_t timeout_ms);

/* E220 mode table (differs from the E22!): M1 M0 = 1 1 is configuration. */
static bool e220_set_mode(int m0, int m1, const char *name) {
    /* EBYTE only samples a mode change while AUX is stably high. In particular, a TX can have
     * drained the ESP UART while the module is still finishing the RF packet; toggling M0/M1
     * then makes the change deferred, and a blind delay can send C0 into transparent mode. */
    if (!wait_aux(MODE_SWITCH_WAIT_MS)) {
        ESP_LOGW(TAG, "cannot enter %s mode: AUX stayed busy before switch", name);
        return false;
    }
    gpio_set_level(PIN_M0, m0);
    gpio_set_level(PIN_M1, m1);
    /* 20 ms is deliberately above the documented 1-2 ms mode-settle interval. Re-check AUX
     * afterwards as well: leaving configuration mode can hold it low while parameters load. */
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!wait_aux(MODE_SWITCH_WAIT_MS)) {
        ESP_LOGW(TAG, "cannot enter %s mode: AUX stayed busy after switch", name);
        return false;
    }
    return true;
}

static bool e220_mode_normal(void) { return e220_set_mode(0, 0, "normal"); }
#ifdef E22_MODULE
/* The E22's mode table is not the E220's: configuration is mode 2, M0 = 0 and M1 = 1. */
static bool e220_mode_config(void) { return e220_set_mode(0, 1, "configuration"); }
#else
static bool e220_mode_config(void) { return e220_set_mode(1, 1, "configuration"); }
#endif

void e220_init(void) {
    s_uart_lock = xSemaphoreCreateMutex();
    configASSERT(s_uart_lock != NULL);
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_M0) | (1ULL << PIN_M1),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    gpio_config_t aux = {
        .pin_bit_mask = (1ULL << PIN_AUX),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&aux);

    const uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(E220_UART, UART_BUF, UART_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(E220_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(E220_UART, PIN_TX, PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#ifdef E22_MODE_HUNT
    /*
     * Bench: EBYTE's mode table is not the same across this family, and the manual to hand is for
     * a different package than the part on the bench. Rather than trust either, walk all four
     * M0/M1 combinations, ask each for its register block, and print whatever comes back. The
     * combination that answers with a well-formed C1 frame is configuration mode on this part.
     */
    for (int m1 = 0; m1 <= 1; m1++) {
        for (int m0 = 0; m0 <= 1; m0++) {
            char name[16];
            snprintf(name, sizeof(name), "M0=%d M1=%d", m0, m1);
            e220_set_mode(m0, m1, name);
            vTaskDelay(pdMS_TO_TICKS(100));
            uint8_t req[3] = {0xC1, 0x00, E220_REG_COUNT};
            uint8_t resp[24] = {0};
            uart_flush_input(E220_UART);
            uart_write_bytes(E220_UART, (const char *)req, sizeof(req));
            uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(500));
            int n = uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(600));
            char hex[80]; int k = 0;
            for (int i = 0; i < n && i < 20 && k + 3 < (int)sizeof(hex); i++)
                k += snprintf(hex + k, sizeof(hex) - k, "%02X ", resp[i]);
            if (n <= 0) snprintf(hex, sizeof(hex), "(нет ответа)");
            ESP_LOGI(TAG, "hunt %s -> %d bytes: %s", name, n, hex);
        }
    }
#endif
    /*
     * Ask the module what it is. Registers 0x80-0x86 are a read-only product-information block:
     * documented for the E22, undocumented for the E220, and answered by both. The fourth of the
     * seven bytes is the part's rated power in plain dBm - 0x1E on a 30 dBm module, and that is
     * what turns "which variant is fitted" from a build flag into a question with an answer.
     *
     * Costs one config-mode round trip at boot. A module that does not answer leaves this unset,
     * and the status line then reports no transmit power at all rather than a guess.
     */
    e220_mode_config();
    {
        uint8_t req[3] = {0xC1, 0x80, 0x07};
        uint8_t resp[3 + 7] = {0};
        uart_flush_input(E220_UART);
        uart_write_bytes(E220_UART, (const char *)req, sizeof(req));
        uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(500));
        int n = uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(600));
        if (n == (int)sizeof(resp) && resp[0] == 0xC1) {
            s_rated_dbm = (int8_t)resp[6];
            ESP_LOGI(TAG, "module reports %d dBm (PID %02X %02X %02X %02X %02X %02X %02X)",
                     s_rated_dbm, resp[3], resp[4], resp[5], resp[6], resp[7], resp[8], resp[9]);
        } else {
            ESP_LOGW(TAG, "module did not answer the product-information read (%d bytes)", n);
        }
    }
    if (!e220_mode_normal()) ESP_LOGW(TAG, "normal mode not confirmed during init");
    ESP_LOGI(TAG, "uart up at 9600 8N1 (tx=%d rx=%d m0=%d m1=%d aux=%d)",
             PIN_E220_TX, PIN_E220_RX, PIN_E220_M0, PIN_E220_M1, PIN_E220_AUX);
}

/* Waits for AUX to go high, which the module uses to signal that it is idle again. */
static bool wait_aux(uint32_t timeout_ms) {
    /* AUX being high at one instant is not enough: it can fall just after the ESP UART drains
     * while the module starts RF work. Keep the whole deadline available for a stable-high
     * sample instead of failing on that first edge. */
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const TickType_t settle = pdMS_TO_TICKS(AUX_POLL_MS);
    for (;;) {
        if (gpio_get_level(PIN_AUX) != 0) {
            vTaskDelay(settle);
            TickType_t elapsed = xTaskGetTickCount() - started;
            if (gpio_get_level(PIN_AUX) != 0 && elapsed <= timeout) return true;
        }
        if (xTaskGetTickCount() - started >= timeout) return false;
        /* The shipped configs use a 100 Hz FreeRTOS tick; 5 ms converts to zero ticks and
         * would turn both the timeout and the stability delay into a busy-loop. */
        vTaskDelay(settle);
    }
}

e220_state_t e220_apply(const e220_config_t *cfg) {
    uint8_t frame[E220_FRAME_LEN];
    e220_build_frame(cfg, frame);

    if (!uart_lock()) {
        ESP_LOGW(TAG, "apply: uart busy");
        return E220_BUSY;
    }
    if (!e220_mode_config()) {
        uart_unlock();
        return E220_NO_ANSWER;
    }
    uart_flush_input(E220_UART);
    int written = uart_write_bytes(E220_UART, (const char *)frame, sizeof(frame));
    bool drained = uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(500)) == ESP_OK;
    bool idle = wait_aux(500);

    uint8_t resp[3 + E220_REG_COUNT] = {0};
    int n = (written == (int)sizeof(frame) && drained && idle)
                ? uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(500))
                : 0;
    bool normal = e220_mode_normal();
    uart_unlock();

    /*
     * The module answers a write with the full block it now holds: C1, start address, length,
     * then the parameters. Accepting "three bytes starting with C0/C1" would call a rejected
     * or truncated write a success and leave the radio on the old channel. As with verify,
     * compare only the fields this driver controls: reserved bits may differ and the key is not
     * reliably readable on this family. UART bits are controlled and must match because normal
     * mode otherwise cannot carry the data plane.
     */
    bool answered = e220_reply_valid(resp, n > 0 ? (size_t)n : 0);
    int mismatch = answered ? e220_first_mismatch(&resp[3], cfg) : -1;
    e220_state_t state = !answered || !normal ? E220_NO_ANSWER
                         : mismatch < 0 ? E220_OK : E220_MISMATCH;
    if (state != E220_OK) {
        /* First contact with a real module fails for dull reasons - swapped TX/RX, wrong
         * M0/M1, a module that is not an E220 - and every one of them looks identical unless
         * the raw reply is visible. Print what actually came back, and what was expected. */
        char got[3 * sizeof(resp) + 1] = {0};
        char want[3 * E220_REG_COUNT + 1] = {0};
        for (int i = 0; i < n && i < (int)sizeof(resp); i++) sprintf(got + 3 * i, "%02X ", resp[i]);
        for (int i = 0; i < E220_REG_COUNT; i++) sprintf(want + 3 * i, "%02X ", frame[3 + i]);
        ESP_LOGW(TAG, "apply: reply %d bytes [%s] - expected C0/C1 00 08 [%s]", n, got, want);
        if (!idle) ESP_LOGW(TAG, "apply: AUX stayed low");
        if (state == E220_NO_ANSWER) {
            ESP_LOGW(TAG, "apply: nothing at all - check TX/RX not swapped, M0/M1 wiring, 3V3");
        }
    }
    ESP_LOGI(TAG, "apply ch=%u rate=%u power=%u lbt=%d -> %s (%d bytes back)",
             cfg->channel, cfg->air_rate, cfg->power, (int)cfg->lbt,
             state == E220_OK ? "ok" : state == E220_MISMATCH ? "MISMATCH" : "NO ANSWER", n);
    return state;
}

e220_state_t e220_verify(const e220_config_t *cfg) {
    uint8_t want[E220_FRAME_LEN];
    e220_build_frame(cfg, want);
    uint8_t req[3];
    e220_build_read_frame(req);

    if (!uart_lock()) return E220_BUSY;
    if (!e220_mode_config()) {
        uart_unlock();
        return E220_NO_ANSWER;
    }
    uart_flush_input(E220_UART);
    int written = uart_write_bytes(E220_UART, (const char *)req, sizeof(req));
    bool drained = uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(500)) == ESP_OK;
    bool idle = wait_aux(500);

    uint8_t resp[3 + E220_REG_COUNT] = {0};
    int n = (written == (int)sizeof(req) && drained && idle)
                ? uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(500))
                : 0;
    bool normal = e220_mode_normal();
    uart_unlock();

    /* Accept either head byte, the same way e220_apply does: which one a given module answers
     * with is not what is being checked here, and being strict about it would turn a healthy
     * module into a permanent mismatch. */
    if (!normal || !e220_reply_valid(resp, n > 0 ? (size_t)n : 0)) {
        return E220_NO_ANSWER;
    }
    /*
     * Only the registers that read back, and within those only the bits this driver chose.
     * The key is write-only on this family and answers zero; the reserved bits came from the
     * stock template rather than from a decision. Comparing either would call a healthy module
     * a mismatch, and a mismatch costs a re-configuration - which is a flash write.
     */
    int mismatch = e220_first_mismatch(&resp[3], cfg);
    if (mismatch >= 0) {
        uint8_t mask = E220_READBACK_MASK[mismatch];
        /* Say which register and what came back: a persistent mismatch keeps the radio down,
         * and this line is what makes it fixable rather than merely visible. */
        ESP_LOGW(TAG, "verify: register %d is %02X, expected %02X (mask %02X)",
                 mismatch, resp[3 + mismatch], want[3 + mismatch], mask);
        return E220_MISMATCH;
    }
    return E220_OK;
}

/*
 * What each power level is worth, from the part itself. Both families index REG1 bits 1-0 the
 * same way and EBYTE publishes one table per rated power, so the rated power is the whole
 * answer. A module that never said, or said something not in this table, gets no claim made
 * about it - an unknown transmit power has to look unknown, because a guessed number invites
 * trust it has not earned.
 */
bool e220_tx_dbm(int8_t out[4]) {
    static const int8_t t30[4] = {30, 27, 24, 21};
    static const int8_t t22[4] = {22, 17, 13, 10};
    const int8_t *t = s_rated_dbm == 30 ? t30 : (s_rated_dbm == 22 ? t22 : NULL);
#if defined(E220_VARIANT_T30) || defined(E22_VARIANT_M30)
    if (!t) t = t30;   /* the build was told, and the module did not answer */
#elif defined(E220_VARIANT_T22) || defined(E22_VARIANT_T22)
    if (!t) t = t22;
#endif
    if (!t) return false;
    memcpy(out, t, 4);
    return true;
}

size_t e220_read_firmware(char *out, size_t out_len) {
    /* The stock DMD firmware answers AT+FWCODE=?; a distributor module may not. Harmless
     * either way — an unanswered probe just yields nothing. */
    const char *probe = "AT+FWCODE=?\r\n";
    if (!uart_lock()) return 0;
    if (!e220_mode_config()) {
        uart_unlock();
        return 0;
    }
    uart_flush_input(E220_UART);
    uart_write_bytes(E220_UART, probe, strlen(probe));
    int n = uart_read_bytes(E220_UART, (uint8_t *)out, out_len - 1, pdMS_TO_TICKS(300));
    bool normal = e220_mode_normal();
    uart_unlock();
    if (!normal || n <= 0) return 0;

    /*
     * The reply carries CR/LF. It ends up as the value of fw= in a space-separated status
     * line, so anything that is not printable has to go or the phone sees the line split in
     * two - and a space has to go too, or a module answering "v1 online=1" would inject
     * tokens into a status the phone then believes. The phone rejects duplicates for exactly
     * this reason; this side simply does not produce them.
     */
    size_t kept = 0;
    for (int i = 0; i < n; i++) {
        char c = out[i];
        if (c == ' ') c = '_';
        if (c > 0x20 && c < 0x7F) out[kept++] = c;
    }
    out[kept] = '\0';
    return kept;
}

bool e220_write(const uint8_t *data, size_t len) {
    if (!uart_lock()) {
        ESP_LOGW(TAG, "write: uart busy, packet dropped");
        return false;
    }
    /* State says normal mode was confirmed, but AUX can be low because an asynchronous RF RX
     * is still being delivered. Do not write into a busy module and hope the UART buffers it. */
    if (!wait_aux(MODE_SWITCH_WAIT_MS)) {
        ESP_LOGW(TAG, "write: AUX stayed busy before transmission");
        uart_unlock();
        return false;
    }
    int written = uart_write_bytes(E220_UART, (const char *)data, len);
    bool drained = uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(1000)) == ESP_OK;
    /* A short write is a truncated air packet, which the group decodes as corruption rather
     * than as a shorter roster. Report it as a failure instead of logging a transmission. */
    bool idle = wait_aux(TX_COMPLETE_WAIT_MS);
    bool ok = (written == (int)len) && drained && idle;
    if (!ok) {
        ESP_LOGW(TAG, "write: %d of %u bytes accepted, drained=%d aux_idle=%d", written,
                 (unsigned)len, (int)drained, (int)idle);
    }
    /* Draining the ESP32 UART only means the bytes reached the module, not that the module
     * has finished sending them over the air. At 2.4 kbps a full packet takes a few hundred
     * ms. Hold the lock until AUX goes high, or a later config could flip M0/M1 mid-packet
     * and truncate the transmission. */
    uart_unlock();
    return ok;
}

size_t e220_read_packet(uint8_t *out, size_t out_len, uint32_t idle_gap_ms,
                        uint32_t first_wait_ms) {
    if (!uart_lock()) return 0;

    /* Wait a bounded time for the first byte, then keep reading until the line goes quiet.
     * That idle gap is the packet boundary — the same rule the module itself uses to close a
     * transmission. Bounding the first wait is what lets configuration get the port. */
    int n = uart_read_bytes(E220_UART, out, 1, pdMS_TO_TICKS(first_wait_ms));
    if (n <= 0) {
#ifdef E220_AMBIENT_RSSI
        /*
         * Bench instrument, and the only one that answers "is anything on the air at all".
         * With REG1 bit 5 set the module replies to C0 C1 C2 C3 <addr> <len> with the channel's
         * current noise floor even in normal mode, so energy can be seen without a packet ever
         * decoding - which is the question when a transmitter reports success and nothing hears
         * it. dBm = -(256 - value), per the E220 manual.
         *
         * Rate-limited, and with no input flush: this branch is only reached when the read above
         * found an empty buffer, so there is nothing to discard, and an unthrottled poll spends
         * enough of every cycle talking to the module to cost real packets. Both of those were
         * learned the hard way - the first version of this instrument reported the noise floor
         * beautifully and drove reception to zero.
         */
        static int64_t next_poll;
        int64_t now = esp_timer_get_time();
        if (now >= next_poll) {
        next_poll = now + E220_AMBIENT_RSSI * 1000;
        static const uint8_t req[6] = {0xC0, 0xC1, 0xC2, 0xC3, 0x00, 0x01};
        uint8_t resp[4] = {0};
        if (uart_write_bytes(E220_UART, (const char *)req, sizeof(req)) == (int)sizeof(req) &&
            uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(200)) == ESP_OK) {
            int got = uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(200));
            if (got == (int)sizeof(resp) && resp[0] == 0xC1)
                ESP_LOGI(TAG, "noise %d dBm", -(256 - (int)resp[3]));
        }
        }
#endif
        uart_unlock();
        return 0;
    }
    size_t used = 1;
    while (used < out_len) {
        n = uart_read_bytes(E220_UART, out + used, out_len - used, pdMS_TO_TICKS(idle_gap_ms));
        if (n <= 0) break;
        used += (size_t)n;
    }
    if (used == out_len) {
        /* The caller deliberately gives us one byte beyond its protocol limit. If even that
         * fills, drain to the same idle boundary so the tail cannot masquerade as a second
         * packet on the next call. */
        uint8_t discard[32];
        do {
            n = uart_read_bytes(E220_UART, discard, sizeof(discard), pdMS_TO_TICKS(idle_gap_ms));
        } while (n > 0);
    }
    uart_unlock();
    return used;
}
