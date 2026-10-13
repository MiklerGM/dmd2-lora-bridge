#pragma once
/*
 * port.h on ESP-IDF. Inline wrappers, so the ESP32 builds make exactly the calls they made before
 * the seam existed - and test_sx1262.c, which fakes ESP-IDF underneath them, still checks the
 * driver as those boards run it. What is more than a call through is in port_esp.c.
 */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)

static inline int64_t port_uptime_us(void) { return esp_timer_get_time(); }
static inline void port_delay_us(uint32_t us) { esp_rom_delay_us(us); }
/* Whole ticks, as pdMS_TO_TICKS always gave: at this build's 100 Hz, anything under 10 ms is a
 * yield rather than a wait. */
static inline void port_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
void port_yield(void);

typedef SemaphoreHandle_t port_mutex_t;

static inline port_mutex_t port_mutex_create(void) {
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    configASSERT(m != NULL);
    return m;
}
static inline bool port_mutex_take(port_mutex_t m, uint32_t timeout_ms) {
    return xSemaphoreTake(m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void port_mutex_lock(port_mutex_t m);
static inline void port_mutex_give(port_mutex_t m) { xSemaphoreGive(m); }

/* A QueueHandle_t, kept opaque so this header needs nothing past what the radio driver uses. */
typedef void *port_queue_t;
port_queue_t port_queue_create(size_t count, size_t item_size);
bool port_queue_send(port_queue_t q, const void *item);
bool port_queue_receive(port_queue_t q, void *item, uint32_t timeout_ms);

bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio);

int port_wdt_watch(void);
void port_wdt_feed(int watch);
void port_wdt_unwatch(int watch);

bool port_button_down(void);

void port_restart(uint32_t note);
uint32_t port_restart_note(void);
const char *port_reset_cause(void);

void port_console_open(void);
int port_console_getc(uint32_t timeout_ms);

static inline void port_pin_output(int pin) {
    gpio_config_t c = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&c);
}
static inline void port_pin_input(int pin) {
    gpio_config_t c = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT};
    gpio_config(&c);
}
static inline void port_pin_set(int pin, int level) { gpio_set_level((gpio_num_t)pin, level); }
static inline int port_pin_get(int pin) { return gpio_get_level((gpio_num_t)pin); }

/* One bus and one radio, driven from the one file that includes radio.h's SX1262 backend. */
static inline spi_device_handle_t *port_radio_spi_dev(void) {
    static spi_device_handle_t dev;
    return &dev;
}

static inline void port_radio_spi_init(int nss, int sck, int mosi, int miso) {
    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = nss,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, port_radio_spi_dev()));
}

static inline bool port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(*port_radio_spi_dev(), &t) == ESP_OK;
}
