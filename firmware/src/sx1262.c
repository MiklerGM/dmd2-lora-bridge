/* Compiled only into the build that selects this backend; see radio.h. */
#ifdef RADIO_BACKEND_SX1262

#include "radio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "sx1262"

/* ---- chip constants ------------------------------------------------------ */

#define CMD_SET_SLEEP            0x84
#define CMD_SET_STANDBY          0x80
#define CMD_SET_TX               0x83
#define CMD_SET_RX               0x82
#define CMD_SET_RF_FREQUENCY     0x86
#define CMD_SET_PACKET_TYPE      0x8A
#define CMD_SET_TX_PARAMS        0x8E
#define CMD_SET_PA_CONFIG        0x95
#define CMD_SET_BUFFER_BASE      0x8F
#define CMD_SET_MODULATION       0x8B
#define CMD_SET_PACKET_PARAMS    0x8C
#define CMD_SET_DIO_IRQ_PARAMS   0x08
#define CMD_GET_IRQ_STATUS       0x12
#define CMD_CLEAR_IRQ_STATUS     0x02
#define CMD_SET_DIO2_RF_SWITCH   0x9D
#define CMD_SET_DIO3_TCXO        0x97
#define CMD_CALIBRATE            0x89
#define CMD_CALIBRATE_IMAGE      0x98
#define CMD_SET_REGULATOR_MODE   0x96
#define CMD_WRITE_REGISTER       0x0D
#define CMD_READ_REGISTER        0x1D
#define CMD_WRITE_BUFFER         0x0E
#define CMD_READ_BUFFER          0x1E
#define CMD_GET_RX_BUFFER_STATUS 0x13

#define REG_LORA_SYNC_MSB 0x0740
#define REG_LORA_SYNC_LSB 0x0741
#define REG_OCP           0x08E7

#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)

#define PACKET_TYPE_LORA 0x01
#define STANDBY_RC       0x00

/*
 * The measured E220 PHY. Not parameters to tune - every one of them was established on hardware
 * and changing any breaks demodulability with the group. See docs/PHY-notes.md.
 */
#define PHY_SF        11
#define PHY_BW_CODE   0x06   /* 500 kHz */
#define PHY_CR_CODE   0x01   /* 4/5 */
#define PHY_LDRO      0x01   /* forced on: the chip's own rule would leave it off here */
#define PHY_PREAMBLE  8
#define PHY_SYNC_MSB  0x54   /* sync word 0x55 as the chip stores it: high nibble | 0x04 */
#define PHY_SYNC_LSB  0x54   /* low nibble << 4 | 0x04 */

/*
 * EBYTE's own frame, which the module's MCU adds and strips. A bare SX1262 has no such MCU, so
 * the backend does it - and above the seam nobody can tell the difference.
 */
#define EBYTE_HEADER_LEN 8
#define EBYTE_XOR        0x12

/* Deliberately conservative and not derived from a measurement, because there is no measurement:
 * the front end adds unknown gain. These are chip-output dBm, lowest sensible values, and they
 * are why this backend exports no RADIO_TX_DBM. */
static const int8_t POWER_DBM[4] = {10, 6, 2, -2};

/* ---- bus ----------------------------------------------------------------- */

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static radio_request_t s_applied;
static bool s_have_applied;
static bool s_in_rx;

#define LOCK_WAIT_MS 2000

static bool bus_lock(void) {
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_WAIT_MS)) == pdTRUE;
}
static void bus_unlock(void) { xSemaphoreGive(s_lock); }

/*
 * Everything here is timed against esp_timer, never against ticks.
 *
 * CONFIG_FREERTOS_HZ is 100 on this build, so a tick is 10 ms and pdMS_TO_TICKS() of anything
 * under 10 rounds to zero - vTaskDelay(0) yields rather than waiting. Every sub-10 ms delay in
 * this driver was therefore not happening at all, which is what made the chip look absent: BUSY
 * is high for a few hundred microseconds after most commands, and a "100 ms" timeout built from
 * pdMS_TO_TICKS(1) expired in well under a millisecond.
 */
static void delay_us(uint32_t us) { esp_rom_delay_us(us); }

/* The chip raises BUSY while it digests a command; talking over it is ignored silently. */
static bool wait_busy(uint32_t timeout_ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level((gpio_num_t)PIN_SX_BUSY) != 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "BUSY stuck high for %ums", (unsigned)timeout_ms);
            return false;
        }
        delay_us(50);
    }
    return true;
}

static bool xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!wait_busy(100)) return false;
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(s_spi, &t) == ESP_OK;
}

static bool cmd(uint8_t opcode, const uint8_t *args, size_t n) {
    uint8_t buf[16];
    if (n + 1 > sizeof(buf)) return false;
    buf[0] = opcode;
    if (n) memcpy(&buf[1], args, n);
    return xfer(buf, NULL, n + 1);
}

static bool cmd_read(uint8_t opcode, uint8_t *out, size_t n) {
    uint8_t tx[16] = {0}, rx[16] = {0};
    if (n + 2 > sizeof(tx)) return false;
    tx[0] = opcode;
    if (!xfer(tx, rx, n + 2)) return false;
    memcpy(out, &rx[2], n);   /* opcode byte, then one status byte, then the payload */
    return true;
}

static bool reg_write(uint16_t addr, const uint8_t *data, size_t n) {
    uint8_t buf[8];
    if (n + 3 > sizeof(buf)) return false;
    buf[0] = CMD_WRITE_REGISTER;
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)addr;
    memcpy(&buf[3], data, n);
    return xfer(buf, NULL, n + 3);
}

static bool buffer_write(const uint8_t *data, size_t n) {
    uint8_t buf[2 + 256];
    if (n > 255) return false;
    buf[0] = CMD_WRITE_BUFFER;
    buf[1] = 0x00;   /* offset */
    memcpy(&buf[2], data, n);
    return xfer(buf, NULL, n + 2);
}

static bool buffer_read(uint8_t offset, uint8_t *out, size_t n) {
    uint8_t tx[3 + 256] = {0}, rx[3 + 256] = {0};
    if (n > 255) return false;
    tx[0] = CMD_READ_BUFFER;
    tx[1] = offset;
    if (!xfer(tx, rx, n + 3)) return false;
    memcpy(out, &rx[3], n);
    return true;
}

/* ---- front end ----------------------------------------------------------- */

static void fem_power(bool on) {
#if PIN_FEM_VCTRL >= 0
    gpio_set_level((gpio_num_t)PIN_FEM_VCTRL, on);
    gpio_set_level((gpio_num_t)PIN_FEM_CSD, on);
#else
    (void)on;
#endif
}

static void fem_transmit(bool tx) {
#if PIN_FEM_VCTRL >= 0
    gpio_set_level((gpio_num_t)PIN_FEM_CTX, tx);
    delay_us(1000);
#else
    (void)tx;
#endif
}

/* ---- configuration ------------------------------------------------------- */

static bool set_standby(void) {
    uint8_t a = STANDBY_RC;
    s_in_rx = false;
    return cmd(CMD_SET_STANDBY, &a, 1);
}

static bool set_frequency(uint8_t channel) {
    /* 850.125 MHz + channel, in the chip's 2^25/32 MHz steps. */
    uint64_t hz = 850125000ull + (uint64_t)channel * 1000000ull;
    uint32_t frf = (uint32_t)((hz << 25) / 32000000ull);
    uint8_t a[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    return cmd(CMD_SET_RF_FREQUENCY, a, 4);
}

static bool set_modulation(void) {
    uint8_t a[4] = {PHY_SF, PHY_BW_CODE, PHY_CR_CODE, PHY_LDRO};
    return cmd(CMD_SET_MODULATION, a, 4);
}

static bool set_packet_params(uint8_t payload_len) {
    uint8_t a[6] = {
        (uint8_t)(PHY_PREAMBLE >> 8), (uint8_t)PHY_PREAMBLE,
        0x00,          /* explicit header */
        payload_len,
        0x01,          /* CRC on */
        0x00,          /* IQ normal */
    };
    return cmd(CMD_SET_PACKET_PARAMS, a, 6);
}

static bool set_power(uint8_t level) {
    /* SX1262 high-power PA, then the output level. The OCP default suits this configuration. */
    uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
    if (!cmd(CMD_SET_PA_CONFIG, pa, 4)) return false;
    uint8_t tx[2] = {(uint8_t)POWER_DBM[level & 0x03], 0x04 /* 200 us ramp */};
    return cmd(CMD_SET_TX_PARAMS, tx, 2);
}

static bool configure(const radio_request_t *req) {
    uint8_t reg = 0x01;                      /* DC-DC, as the Heltec board is wired */
    uint8_t tcxo[4] = {0x02,                 /* 1.8 V */
                       0x00, 0x01, 0x40};    /* 5 ms startup */
    uint8_t pkt = PACKET_TYPE_LORA;
    uint8_t sync[2] = {PHY_SYNC_MSB, PHY_SYNC_LSB};
    uint8_t base[2] = {0x00, 0x00};
    uint8_t calib = 0x7F;
    uint8_t irq[8] = {0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};

    /* Named steps, because "apply failed" on a bus that answers is otherwise a guess. */
#define STEP(what, expr) do { if (!(expr)) { ESP_LOGW(TAG, "configure: %s failed", what); \
                                             return false; } } while (0)
    STEP("standby", set_standby());
    STEP("regulator", cmd(CMD_SET_REGULATOR_MODE, &reg, 1));
    STEP("tcxo", cmd(CMD_SET_DIO3_TCXO, tcxo, 4));
    STEP("calibrate", cmd(CMD_CALIBRATE, &calib, 1));
    delay_us(5000);
    STEP("packet type", cmd(CMD_SET_PACKET_TYPE, &pkt, 1));
    STEP("frequency", set_frequency(req->channel));
    STEP("modulation", set_modulation());
    STEP("packet params", set_packet_params(255));
    STEP("sync word", reg_write(REG_LORA_SYNC_MSB, sync, 2));
    STEP("buffer base", cmd(CMD_SET_BUFFER_BASE, base, 2));
    STEP("power", set_power(req->power_level));
    /* Everything latched, only RxDone and TxDone on DIO1. */
    STEP("irq params", cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8));
#undef STEP
    return true;
}

static bool enter_rx(void) {
    uint8_t clear[2] = {0xFF, 0xFF};
    uint8_t rx[3] = {0xFF, 0xFF, 0xFF};   /* continuous */
    if (!cmd(CMD_CLEAR_IRQ_STATUS, clear, 2)) return false;
    fem_transmit(false);
    if (!cmd(CMD_SET_RX, rx, 3)) return false;
    s_in_rx = true;
    return true;
}

/* ---- radio.h ------------------------------------------------------------- */

void radio_init(void) {
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SX_RST)
#if PIN_FEM_VCTRL >= 0
                        | (1ULL << PIN_FEM_VCTRL) | (1ULL << PIN_FEM_CSD) | (1ULL << PIN_FEM_CTX)
#endif
        ,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = (1ULL << PIN_SX_BUSY) | (1ULL << PIN_SX_DIO1),
                        .mode = GPIO_MODE_INPUT};
    gpio_config(&in);

    fem_power(true);
    fem_transmit(false);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SX_MOSI,
        .miso_io_num = PIN_SX_MISO,
        .sclk_io_num = PIN_SX_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_SX_NSS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    gpio_set_level((gpio_num_t)PIN_SX_RST, 0);
    delay_us(2000);
    gpio_set_level((gpio_num_t)PIN_SX_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "spi up (nss=%d sck=%d mosi=%d miso=%d busy=%d dio1=%d), FEM %s",
             PIN_SX_NSS, PIN_SX_SCK, PIN_SX_MOSI, PIN_SX_MISO, PIN_SX_BUSY, PIN_SX_DIO1,
             PIN_FEM_VCTRL >= 0 ? "powered, RX path" : "not fitted");
}

radio_state_t radio_apply(const radio_request_t *req) {
    if (!bus_lock()) return RADIO_BUSY;
    bool ok = configure(req) && enter_rx();
    if (ok) {
        s_applied = *req;
        s_have_applied = true;
    }
    bus_unlock();
    ESP_LOGI(TAG, "apply ch=%u power_level=%u lbt=%d -> %s",
             req->channel, req->power_level, (int)req->lbt, ok ? "ok" : "FAILED");
    /* No answer is the only failure this bus reports: an SX1262 that does not respond leaves
     * BUSY high, and every command above returns false on that. */
    return ok ? RADIO_OK : RADIO_NO_ANSWER;
}

radio_state_t radio_verify(const radio_request_t *req) {
    if (!bus_lock()) return RADIO_BUSY;
    uint8_t sync[2] = {0, 0};
    uint8_t tx[6] = {CMD_READ_REGISTER, (uint8_t)(REG_LORA_SYNC_MSB >> 8),
                     (uint8_t)REG_LORA_SYNC_MSB, 0x00, 0x00, 0x00};
    uint8_t rx[6] = {0};
    bool answered = xfer(tx, rx, 6);
    sync[0] = rx[4];
    sync[1] = rx[5];
    bus_unlock();

    if (!answered) return RADIO_NO_ANSWER;
    /* The sync word is the one register that proves configuration rather than mere presence:
     * a chip that reset itself comes back with the 0x1424 default, not ours. */
    if (sync[0] != PHY_SYNC_MSB || sync[1] != PHY_SYNC_LSB) {
        ESP_LOGW(TAG, "verify: sync word is %02X%02X, expected %02X%02X",
                 sync[0], sync[1], PHY_SYNC_MSB, PHY_SYNC_LSB);
        return RADIO_MISMATCH;
    }
    if (!s_have_applied || s_applied.channel != req->channel ||
        s_applied.power_level != req->power_level || s_applied.lbt != req->lbt) {
        return RADIO_MISMATCH;
    }
    return RADIO_OK;
}

bool radio_send(const uint8_t *data, size_t len) {
    if (!RADIO_TX_ENABLED) return false;
    if (len == 0 || len > 255 - EBYTE_HEADER_LEN) return false;
    if (!bus_lock()) return false;

    /* The frame an E220 expects, built here because there is no module MCU to build it. */
    uint8_t frame[EBYTE_HEADER_LEN + 255];
    frame[0] = 0x2C;
    frame[1] = s_have_applied ? s_applied.channel : RADIO_DEFAULT_CHANNEL;
    frame[2] = 0xA0;
    frame[3] = 0x01;
    frame[4] = frame[5] = frame[6] = 0x00;
    frame[7] = (uint8_t)len;
    for (size_t i = 0; i < len; i++) frame[EBYTE_HEADER_LEN + i] = data[i] ^ EBYTE_XOR;
    size_t total = EBYTE_HEADER_LEN + len;

    uint8_t clear[2] = {0xFF, 0xFF};
    uint8_t go[3] = {0x00, 0x00, 0x00};   /* no timeout */
    bool ok = set_standby() && set_packet_params((uint8_t)total) &&
              buffer_write(frame, total) && cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
    if (ok) {
        fem_transmit(true);
        ok = cmd(CMD_SET_TX, go, 3);
        /* At SF11/BW500 a 46-byte frame is about a quarter of a second on the air. */
        int64_t deadline = esp_timer_get_time() + 3000 * 1000;
        while (ok) {
            uint8_t irq[2] = {0, 0};
            if (!cmd_read(CMD_GET_IRQ_STATUS, irq, 2)) { ok = false; break; }
            uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
            if (flags & IRQ_TX_DONE) break;
            if (flags & IRQ_TIMEOUT) { ok = false; break; }
            if (esp_timer_get_time() > deadline) { ok = false; break; }
            delay_us(2000);
        }
        fem_transmit(false);
    }
    if (!ok) ESP_LOGW(TAG, "send: %u bytes did not complete", (unsigned)total);
    /* Back to listening whatever happened: a radio parked in standby hears nothing and says
     * nothing about it. */
    enter_rx();
    bus_unlock();
    return ok;
}

size_t radio_receive(uint8_t *out, size_t out_len, uint32_t idle_gap_ms, uint32_t first_wait_ms) {
    (void)idle_gap_ms;   /* a UART packet boundary; SPI hands over whole packets */
    if (!bus_lock()) return 0;
    if (!s_in_rx) enter_rx();

    size_t produced = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)first_wait_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq[2] = {0, 0};
        if (!cmd_read(CMD_GET_IRQ_STATUS, irq, 2)) break;
        uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
        if (!(flags & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR))) {
            delay_us(2000);
            continue;
        }
        uint8_t clear[2] = {0xFF, 0xFF};
        if (flags & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
            cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
            break;
        }
        uint8_t st[2] = {0, 0};
        if (!cmd_read(CMD_GET_RX_BUFFER_STATUS, st, 2)) break;
        size_t n = st[0];
        uint8_t frame[EBYTE_HEADER_LEN + 255];
        if (n > sizeof(frame)) n = sizeof(frame);
        if (n && buffer_read(st[1], frame, n)) {
            /* Strip EBYTE's frame and undo the obfuscation, so the caller sees exactly the
             * bytes a module would have put on its UART. A frame that is not one is dropped
             * rather than passed up half-understood. */
            if (n > EBYTE_HEADER_LEN && frame[0] == 0x2C && frame[7] == n - EBYTE_HEADER_LEN) {
                size_t payload = n - EBYTE_HEADER_LEN;
                if (payload > out_len) payload = out_len;
                for (size_t i = 0; i < payload; i++) out[i] = frame[EBYTE_HEADER_LEN + i] ^ EBYTE_XOR;
                produced = payload;
            } else {
                ESP_LOGW(TAG, "rx: %u bytes but not an EBYTE frame", (unsigned)n);
            }
        }
        cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
        break;
    }
    bus_unlock();
    return produced;
}

size_t radio_identity(char *out, size_t out_len) {
    /* The chip has no firmware string to report. Naming the PHY is more use in a status line
     * than a blank, and it is space-free as the contract requires. */
    const char *id = "SX1262-SF11BW500";
    size_t n = strlen(id);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, id, n);
    out[n] = '\0';
    return n;
}

#endif /* RADIO_BACKEND_SX1262 */
