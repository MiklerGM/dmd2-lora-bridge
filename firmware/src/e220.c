#include "e220.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "e220"

#define E220_UART UART_NUM_1
#define PIN_TX GPIO_NUM_17
#define PIN_RX GPIO_NUM_16
#define PIN_M0 GPIO_NUM_21
#define PIN_M1 GPIO_NUM_22
#define PIN_AUX GPIO_NUM_23

#define UART_BUF 1024
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
    ESP_LOGI(TAG, "uart up at 9600 8N1");
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
    uint8_t reg[8] = {0};
    reg[0] = (uint8_t)(cfg->address >> 8);
    reg[1] = (uint8_t)(cfg->address & 0xFF);
    /* REG0: baud 9600 (index 3) << 5 | parity 8N1 (0) << 3 | air rate */
    reg[2] = (uint8_t)((3 << 5) | (0 << 3) | (cfg->air_rate & 0x07));
    /* REG1: sub-packet << 6 | power. Ambient-RSSI bit left clear. */
    reg[3] = (uint8_t)(((cfg->packet_size & 0x03) << 6) | (cfg->power & 0x03));
    reg[4] = cfg->channel;
    /* REG3: bit7 RSSI byte off, bit6 fixed-mode (0 = transparent), bit4 LBT, WOR cycle 0 */
    reg[5] = (uint8_t)((cfg->transparent ? 0 : (1 << 6)) | (cfg->lbt ? (1 << 4) : 0));
    reg[6] = (uint8_t)(cfg->key >> 8);
    reg[7] = (uint8_t)(cfg->key & 0xFF);

    uint8_t frame[3 + sizeof(reg)];
    frame[0] = CMD_HEAD_WRITE;
    frame[1] = 0x00; /* start register */
    frame[2] = sizeof(reg);
    memcpy(&frame[3], reg, sizeof(reg));

    e220_mode_config();
    uart_flush_input(E220_UART);
    uart_write_bytes(E220_UART, (const char *)frame, sizeof(frame));
    wait_aux(500);

    uint8_t resp[16] = {0};
    int n = uart_read_bytes(E220_UART, resp, sizeof(resp), pdMS_TO_TICKS(500));
    e220_mode_normal();

    /* The module echoes the block back with C1 as the head. */
    bool ok = (n >= 3) && (resp[0] == CMD_HEAD_READ || resp[0] == CMD_HEAD_WRITE);
    ESP_LOGI(TAG, "apply ch=%u rate=%u power=%u lbt=%d -> %s (%d bytes back)",
             cfg->channel, cfg->air_rate, cfg->power, (int)cfg->lbt, ok ? "ok" : "FAILED", n);
    return ok;
}

size_t e220_read_firmware(char *out, size_t out_len) {
    /* The stock DMD firmware answers AT+FWCODE=?; a distributor module may not. Harmless
     * either way — an unanswered probe just yields nothing. */
    const char *probe = "AT+FWCODE=?\r\n";
    e220_mode_config();
    uart_flush_input(E220_UART);
    uart_write_bytes(E220_UART, probe, strlen(probe));
    int n = uart_read_bytes(E220_UART, (uint8_t *)out, out_len - 1, pdMS_TO_TICKS(300));
    e220_mode_normal();
    if (n <= 0) return 0;
    out[n] = '\0';
    return (size_t)n;
}

void e220_write(const uint8_t *data, size_t len) {
    uart_write_bytes(E220_UART, (const char *)data, len);
    uart_wait_tx_done(E220_UART, pdMS_TO_TICKS(1000));
}

size_t e220_read_packet(uint8_t *out, size_t out_len, uint32_t idle_gap_ms) {
    size_t used = 0;
    /* Block until the first byte, then keep reading until the line goes quiet. That idle gap
     * is the packet boundary — the same rule the module itself uses to close a transmission. */
    int n = uart_read_bytes(E220_UART, out, 1, portMAX_DELAY);
    if (n <= 0) return 0;
    used = 1;
    while (used < out_len) {
        n = uart_read_bytes(E220_UART, out + used, out_len - used, pdMS_TO_TICKS(idle_gap_ms));
        if (n <= 0) break;
        used += (size_t)n;
    }
    return used;
}
