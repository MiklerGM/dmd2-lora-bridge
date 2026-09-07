#include "e220.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "e220"

/* Pins come from the build environment: the WROOM and the C3 have different GPIO maps,
 * and the C3 has no GPIO 16/17/22/23 at all. See platformio.ini. */
#define E220_UART UART_NUM_1
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
#define CMD_HEAD_WRITE 0xC0
#define CMD_HEAD_READ 0xC1

/* E220 mode table (differs from the E22!): M1 M0 = 1 1 is configuration. */
static void e220_mode_normal(void) {
    gpio_set_level(PIN_M0, 0);
    gpio_set_level(PIN_M1, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void e220_mode_config(void) {
    gpio_set_level(PIN_M0, 1);
    gpio_set_level(PIN_M1, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

void e220_init(void) {
    s_uart_lock = xSemaphoreCreateMutex();
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
    e220_mode_normal();
    ESP_LOGI(TAG, "uart up at 9600 8N1 (tx=%d rx=%d m0=%d m1=%d aux=%d)",
             PIN_E220_TX, PIN_E220_RX, PIN_E220_M0, PIN_E220_M1, PIN_E220_AUX);
}

/* Waits for AUX to go high, which the module uses to signal "idle, command accepted". */
static void wait_aux(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (gpio_get_level(PIN_AUX) == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
}

bool e220_apply(const e220_config_t *cfg) {
    uint8_t frame[E220_FRAME_LEN];
    e220_build_frame(cfg, frame);

    if (!uart_lock()) {
        ESP_LOGW(TAG, "apply: uart busy");
        return false;
    }
    e220_mode_config();
    uart_flush_input(E220_UART);
    uart_write_bytes(E220_UART, (const char *)frame, sizeof(frame));
    wait_aux(500);

    uint8_t resp[16] = {0};
    int n = uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(500));
    e220_mode_normal();
    uart_unlock();

    /* The module answers a write with the same block, headed C1. */
    bool ok = (n >= 3) && (resp[0] == CMD_HEAD_READ || resp[0] == CMD_HEAD_WRITE);
    ESP_LOGI(TAG, "apply ch=%u rate=%u power=%u lbt=%d -> %s (%d bytes back)",
             cfg->channel, cfg->air_rate, cfg->power, (int)cfg->lbt, ok ? "ok" : "FAILED", n);
    return ok;
}

size_t e220_read_firmware(char *out, size_t out_len) {
    /* The stock DMD firmware answers AT+FWCODE=?; a distributor module may not. Harmless
     * either way — an unanswered probe just yields nothing. */
    const char *probe = "AT+FWCODE=?\r\n";
    if (!uart_lock()) return 0;
    e220_mode_config();
    uart_flush_input(E220_UART);
    uart_write_bytes(E220_UART, probe, strlen(probe));
    int n = uart_read_bytes(E220_UART, (uint8_t *)out, out_len - 1, pdMS_TO_TICKS(300));
    e220_mode_normal();
    uart_unlock();
    if (n <= 0) return 0;

    /* The reply carries CR/LF. It ends up inside a newline-delimited status line, so anything
     * that is not printable has to go or the phone sees the line split in two. */
    size_t kept = 0;
    for (int i = 0; i < n; i++) {
        char c = out[i];
        if (c >= 0x20 && c < 0x7F) out[kept++] = c;
    }
    out[kept] = '\0';
    return kept;
}

void e220_write(const uint8_t *data, size_t len) {
    if (!uart_lock()) {
        ESP_LOGW(TAG, "write: uart busy, packet dropped");
        return;
    }
    uart_write_bytes(E220_UART, (const char *)data, len);
    uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(1000));
    /* Draining the ESP32 UART only means the bytes reached the module, not that the module
     * has finished sending them over the air. At 2.4 kbps a full packet takes a few hundred
     * ms. Hold the lock until AUX goes high, or a later config could flip M0/M1 mid-packet
     * and truncate the transmission. */
    wait_aux(2000);
    uart_unlock();
}

size_t e220_read_packet(uint8_t *out, size_t out_len, uint32_t idle_gap_ms,
                        uint32_t first_wait_ms) {
    if (!uart_lock()) return 0;

    /* Wait a bounded time for the first byte, then keep reading until the line goes quiet.
     * That idle gap is the packet boundary — the same rule the module itself uses to close a
     * transmission. Bounding the first wait is what lets configuration get the port. */
    int n = uart_read_bytes(E220_UART, out, 1, pdMS_TO_TICKS(first_wait_ms));
    if (n <= 0) {
        uart_unlock();
        return 0;
    }
    size_t used = 1;
    while (used < out_len) {
        n = uart_read_bytes(E220_UART, out + used, out_len - used, pdMS_TO_TICKS(idle_gap_ms));
        if (n <= 0) break;
        used += (size_t)n;
    }
    uart_unlock();
    return used;
}
