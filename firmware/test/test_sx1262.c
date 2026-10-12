/*
 * The SX1262 backend against a fake chip at the far end of its SPI bus: what radio_verify calls
 * healthy has to be what the chip was last told, whatever the driver believes.
 *
 * sx1262.c is compiled whole, on just enough of ESP-IDF to build (the Makefile supplies its
 * headers as empty files). The fake keeps only what the radio would: its registers, its
 * frequency, its mode, and how its last command went - told, as the real chip tells it, on the
 * transfer after.
 */
#define RADIO_BACKEND_SX1262

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- just enough ESP-IDF ---------------------------------------------------- */

typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef void *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num,
                 max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef void *SemaphoreHandle_t;

#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)
#define configASSERT(x) assert(x)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

static void log_nothing(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) log_nothing(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_nothing(tag, __VA_ARGS__)

static int gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static int gpio_set_level(gpio_num_t pin, int level) { (void)pin; (void)level; return ESP_OK; }
static int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }   /* BUSY is never high */
static void esp_rom_delay_us(uint32_t us) { (void)us; }
/* A millisecond per look at the clock, so every deadline in the driver arrives. */
static int64_t esp_timer_get_time(void) { static int64_t now; return now += 1000; }
static void vTaskDelay(int ticks) { (void)ticks; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static int xSemaphoreTake(SemaphoreHandle_t s, int ticks) { (void)s; (void)ticks; return pdTRUE; }
static int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static int spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma) {
    (void)host; (void)bus; (void)dma;
    return ESP_OK;
}
static int spi_bus_add_device(int host, const spi_device_interface_config_t *dev,
                              spi_device_handle_t *out) {
    (void)host; (void)dev;
    *out = (spi_device_handle_t)1;
    return ESP_OK;
}
static int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t);

#include "../src/sx1262.c"

/* ---- the chip ---------------------------------------------------------------- */

#define MODE_STANDBY 2
#define MODE_RX 5

static struct {
    uint8_t regs[0x10000];
    uint8_t frequency[4];   /* the last SetRfFrequency it took */
    uint8_t mode;
    uint8_t status;         /* how the last command went, told on the next transfer */
    uint8_t drop;           /* a command the bus fails, once */
    uint8_t refuse;         /* a command the chip refuses, once */
} chip = {.mode = MODE_STANDBY};

static int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t) {
    (void)spi;
    const uint8_t *tx = t->tx_buffer;
    uint8_t *rx = t->rx_buffer;
    const size_t n = t->length / 8;
    if (chip.drop && tx[0] == chip.drop) {
        chip.drop = 0;
        return -1;
    }
    /* The status byte goes out while this command is still coming in, so it tells of the last
     * one - as the datasheet has it, and as a driver has to read it. */
    if (rx) {
        memset(rx, 0, n);
        if (n > 1) rx[1] = (uint8_t)(chip.mode << 4 | chip.status << 1);
    }
    chip.status = 0;
    if (chip.refuse && tx[0] == chip.refuse) {
        chip.refuse = 0;
        chip.status = 5;    /* failure to execute: nothing changes */
        return ESP_OK;
    }
    switch (tx[0]) {
        case CMD_WRITE_REGISTER: memcpy(&chip.regs[tx[1] << 8 | tx[2]], &tx[3], n - 3); break;
        case CMD_READ_REGISTER: memcpy(&rx[4], &chip.regs[tx[1] << 8 | tx[2]], n - 4); break;
        case CMD_SET_RF_FREQUENCY: memcpy(chip.frequency, &tx[1], 4); break;
        case CMD_SET_STANDBY:
        case CMD_SET_TX: chip.mode = MODE_STANDBY; break;   /* a packet goes out at once */
        case CMD_SET_RX: chip.mode = MODE_RX; break;
        case CMD_GET_IRQ_STATUS: rx[3] = IRQ_TX_DONE; break;
    }
    return ESP_OK;
}

int main(void) {
    const radio_request_t a = {.channel = 18, .power_level = 3, .lbt = false};
    const radio_request_t b = {.channel = 19, .power_level = 0, .lbt = false};
    radio_init();
    assert(radio_apply(&a) == RADIO_OK && radio_verify(&a) == RADIO_OK);

    /* The frequency lands and a later command does not, lost on the bus or refused by the chip -
     * which says so only on the transfer after it. The chip has left A, and B was never
     * completed, so neither may be confirmed until a whole apply succeeds. */
    static const struct { uint8_t *how, opcode; } partway[] = {
        {&chip.drop, CMD_SET_MODULATION}, {&chip.refuse, CMD_SET_TX_PARAMS}};
    for (size_t i = 0; i < sizeof(partway) / sizeof(partway[0]); i++) {
        uint8_t on_a[4];
        memcpy(on_a, chip.frequency, sizeof(on_a));
        *partway[i].how = partway[i].opcode;
        assert(radio_apply(&b) != RADIO_OK);
        assert(memcmp(chip.frequency, on_a, sizeof(on_a)) != 0);
        assert(radio_verify(&a) != RADIO_OK && radio_verify(&b) != RADIO_OK);
        assert(radio_apply(&a) == RADIO_OK && radio_verify(&a) == RADIO_OK);
    }
    printf("sx1262: an apply that stops partway confirms neither request\n");

    /* The packet goes out and the return to receive does not, lost or refused: a configured
     * radio that hears nothing is not a healthy one, until it is listening again. */
    uint8_t *const back[] = {&chip.drop, &chip.refuse};
    for (size_t i = 0; i < sizeof(back) / sizeof(back[0]); i++) {
        *back[i] = CMD_SET_RX;
        assert(radio_send((const uint8_t *)"00000001AAAA", 12));
        assert(chip.mode != MODE_RX && radio_verify(&a) != RADIO_OK);
        uint8_t buf[16];
        radio_receive(buf, sizeof(buf), 0, 1);   /* tries RX again on its way in */
        assert(chip.mode == MODE_RX && radio_verify(&a) == RADIO_OK);
    }
    printf("sx1262: a receiver that did not come back after sending is not reported healthy\n");
    return 0;
}
