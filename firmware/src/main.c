/*
 * DMD2 LoRa bridge on an ESP32: storage and the board, then the bridge itself (bridge.c), which
 * is the same on every board.
 */
#include "bridge.h"
#include "led.h"
#include "port.h"

#include "nvs_flash.h"

#define TAG "bridge"

static void erase_nvs(void) { ESP_ERROR_CHECK(nvs_flash_erase()); }

void app_main(void) {
    const char *reset = bridge_reset_name();
    LOGI(TAG, "started after: %s", reset);
    /* Watched through the slow part of start-up as well: radio_init alone can wait seconds per
     * module it tries, and a start that hangs there never reaches Bluetooth. */
    const int watch = port_wdt_watch();
    /* Before storage and the radio, so a board waiting on either still shows it is alive. */
    led_init();
    gpio_config_t pair_pin = {
        .pin_bit_mask = (1ULL << PIN_PAIR_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&pair_pin);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        bridge_erase_when_asked(esp_err_to_name(err), watch, erase_nvs);
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#ifdef PIN_BOARD_POWERON
    /*
     * One pin powers the board's peripherals. On a T-Deck that is the keyboard, the trackball,
     * the backlight, the card slot and the audio chain - not the radio, which has its own
     * supply - but it has to go up early all the same: bringing that much load up late browns
     * the rail out and resets the chip.
     */
    gpio_config_t board_power = {
        .pin_bit_mask = (1ULL << PIN_BOARD_POWERON),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&board_power);
    gpio_set_level((gpio_num_t)PIN_BOARD_POWERON, 1);
#endif

#ifdef BOARD_SPI_IDLE_HIGH
    /*
     * Chip selects of the other devices sharing the radio's SPI bus, parked high so that only
     * the radio ever answers. Undriven they float, and a neighbour replies alongside it.
     *
     * That failure does not look like a bus problem, which is what makes it worth a comment:
     * on a T-Deck every command came back 0xAA, which is not the all-ones of a floating line
     * but real bytes from the wrong chip, and the low nibble even decoded as a believable chip
     * mode. The radio looked present and confused rather than drowned out.
     */
    static const int idle_high[] = {BOARD_SPI_IDLE_HIGH};
    for (size_t i = 0; i < sizeof(idle_high) / sizeof(idle_high[0]); i++) {
        gpio_config_t cs = {
            .pin_bit_mask = (1ULL << idle_high[i]),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&cs);
        gpio_set_level((gpio_num_t)idle_high[i], 1);
    }
#endif

#if defined(PIN_BOARD_POWERON) || defined(BOARD_SPI_IDLE_HIGH)
    vTaskDelay(pdMS_TO_TICKS(100));   /* let the rail settle before the radio is spoken to */
#endif

    bridge_start(reset, watch);
    /* app_main returns and its task ends: a watched task that ends without leaving trips the
     * watchdog. */
    port_wdt_unwatch(watch);
}
