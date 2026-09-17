/* Compiled only into the build that selects this backend; see radio.h. */
#ifdef RADIO_BACKEND_SX1262

#include "radio.h"

#include <stdio.h>
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
#define REG_TX_MODULATION 0x0889
#define REG_TX_CLAMP      0x08D8
#define REG_IQ_CONFIG     0x0736
#define REG_FREQ_ERROR    0x076B
#define CMD_SET_RXTX_FALLBACK 0x93

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

/* Deliberately conservative and not derived from a measurement of this board, because there is
 * none: the front end adds unknown gain. This is why the backend exports no RADIO_TX_DBM.
 *
 * Not a linear dBm knob either. Writing the wanted dBm straight into SetTxParams and leaving
 * SetPaConfig at the +22 dBm pair does not produce that power - RadioLib carries a measured
 * table (radiolib-org/power-tests) precisely because the naive mapping is wrong, and on this
 * bench the naive version reached an E220 zero times out of 27 where the table reached it. */
#ifdef POWER_SWEEP
/* Bench sweep: a deliberately wide span, because the question it answers is whether the front
 * end is in compression - which shows up as the weak settings decoding better than the strong
 * ones, and cannot be seen at all across the 8 dB the shipping table covers. */
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {1, 2, 22},   /* ~10 dBm */
    {1, 1, 22},   /* ~4 dBm  */
    {4, 1, 6},    /* ~-2 dBm */
    {2, 2, -5},   /* ~-9 dBm */
};
#else
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {1, 2, 22},   /* ~10 dBm */
    {3, 1, 21},   /* ~6 dBm */
    {1, 1, 22},   /* ~4 dBm */
    {2, 1, 14},   /* ~2 dBm */
};
#endif

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
    bool ok = spi_device_polling_transmit(s_spi, &t) == ESP_OK;
    /*
     * BUSY has to be low again before the next command, and it rises a moment *after* NSS does -
     * so sampling it only on the way in samples the gap, and the chip silently discards whatever
     * is sent next. Two polling transfers here are microseconds apart, well inside that window.
     *
     * This is why a register read taken third in a burst returned 0x20: not register content but
     * a perfectly formed status byte, STBY_RC with no command status. Reads and writes that
     * followed a heavy command landed, the ones that followed another register access raced, and
     * a dropped reg_write is indistinguishable from an applied one at every level this driver
     * can see - which is how the transmit errata came to be configured by lottery.
     */
    if (ok) {
        delay_us(1);
        ok = wait_busy(100);
    }
#ifdef SX_SPI_TRACE
    /* Bench only: the same view RadioLib's RLB_SPI debug gives, so the two can be diffed. */
    if (tx[0] != CMD_GET_IRQ_STATUS && tx[0] != 0xC0) {
        char line[200];
        size_t k = 0, show = len > 28 ? 28 : len;
        for (size_t i = 0; i < show && k + 4 < sizeof(line); i++)
            k += (size_t)snprintf(line + k, sizeof(line) - k, "%02X ", tx[i]);
        ESP_LOGI(TAG, "SPI %s%s", line, len > show ? "..." : "");
    }
#endif
    return ok;
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

static bool reg_read(uint16_t addr, uint8_t *out, size_t n) {
    uint8_t tx[8] = {0}, rx[8] = {0};
    if (n + 4 > sizeof(tx)) return false;
    tx[0] = CMD_READ_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    if (!xfer(tx, rx, n + 4)) return false;
    memcpy(out, &rx[4], n);   /* opcode, address high, address low, one status byte */
    return true;
}

/*
 * SX1262 errata 15.1, "Modulation Quality with 500 kHz LoRa Bandwidth". At BW500 bit 2 of the
 * TxModulation register must be CLEARED before transmitting; at every other bandwidth it must
 * be set. Skipping it does not fail anything locally - the chip reports TxDone exactly as
 * usual - it just degrades the modulation enough that nothing on the other end decodes. That is
 * precisely what it did here: 49 transmissions reported ok, an E220 half a metre away heard
 * none of them, and the same pair worked when RadioLib (which applies this) drove the radio.
 */
static bool fix_modulation_quality(void) {
    /* Constant, not a read-modify-write: BW500 is the only bandwidth this backend uses, 0x00 is
     * what the working reference puts on the wire, and a read that quietly returns a status byte
     * would otherwise write that status byte straight into a modem register. */
    uint8_t v = 0x00;
    return reg_write(REG_TX_MODULATION, &v, 1);
}

/*
 * Errata 15.2: the PA clamp is too eager, and into a mismatched antenna it can swallow the
 * output entirely. Receive never notices, which is what makes it hard to find: the chip reports
 * a clean TxDone over a transmission of exactly the right length that nothing ever hears.
 */
static bool fix_pa_clamping(void) {
    /* The value RadioLib leaves here on this exact board, taken from its SPI trace rather than
     * from a read-modify-write: reading this register on this driver returns 0x20 where
     * RadioLib reads 0xC0, and the working reference is the one worth copying. */
    uint8_t v = 0xDE;
    return reg_write(REG_TX_CLAMP, &v, 1);
}

/* Errata 15.4, standard (non-inverted) IQ. Same source: the reference writes 0x0D here. */
static bool fix_iq_polarity(void) {
    uint8_t v = 0x0D;
    return reg_write(REG_IQ_CONFIG, &v, 1);
}

/*
 * 140 mA. SetPaConfig resets this register, so it has to be written after it - and the value the
 * reference writes, 0x18, is the SX1261's 60 mA, where the SX1262's high-power PA is specified at
 * 0x38. That is a datasheet argument and it is the only one: a first run put the change at +11
 * points, and a second run of the same design put it at -7. Combined, 36/288 against 45/288,
 * p = 0.28. The effect does not exist at any size this bench can see, and the first result was
 * read as a finding when it was noise. Correct by the part number, not by measurement.
 */
static bool set_ocp(void) {
    uint8_t v = 0x38;
    return reg_write(REG_OCP, &v, 1);
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
    uint8_t tx[3 + 256] = {0}, rx[260] = {0};   /* DMA rounds the receive up to 4 */
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
    /* The supply has to be up before any control pin is driven, and VFEM_Ctrl only enables an
     * LDO whose output then ramps through 10 uF. Driving CSD in the same breath violates the
     * documented power-on sequence on every boot. */
    gpio_set_level((gpio_num_t)PIN_FEM_VCTRL, on);
    if (on) delay_us(10000);
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

#ifdef PREAMBLE_SWEEP
/*
 * Bench: the E220's own preamble length was never measured - the probe received it with 8
 * symbols configured, but that setting governs transmission, and a receiver decodes a longer
 * preamble than it expects. If the module's receiver sniffs the channel rather than listening
 * continuously, too short a preamble is caught only when it happens to be awake, which reads as
 * a fixed low success rate that more power cannot move. That is exactly the 20% plateau this
 * backend hits across 10 dB. Steps ride on power_level so the beacon's payload already labels
 * them.
 */
static const uint16_t SWEEP_PREAMBLE[4] = {8, 16, 32, 64};
static uint16_t s_preamble = PHY_PREAMBLE;
#endif

static bool set_packet_params(uint8_t payload_len) {
#ifdef PREAMBLE_SWEEP
    const uint16_t pre = s_preamble;
#else
    const uint16_t pre = PHY_PREAMBLE;
#endif
    uint8_t a[6] = {
        (uint8_t)(pre >> 8), (uint8_t)pre,
        0x00,          /* explicit header */
        payload_len,
        0x01,          /* CRC on */
        0x00,          /* IQ normal */
    };
    return cmd(CMD_SET_PACKET_PARAMS, a, 6);
}

static bool set_power(uint8_t level) {
    /* SX1262 PA, then the drive level. The OCP default suits this configuration. */
    const uint8_t i = level & 0x03;
    uint8_t pa[4] = {POWER_PA[i].duty, POWER_PA[i].hp_max, 0x00 /* SX1262 */, 0x01};
    if (!cmd(CMD_SET_PA_CONFIG, pa, 4)) return false;
    uint8_t tx[2] = {(uint8_t)POWER_PA[i].pa_val, 0x04 /* 200 us ramp */};
    return cmd(CMD_SET_TX_PARAMS, tx, 2);
}

static bool configure(const radio_request_t *req) {
    uint8_t reg = 0x01;                      /* DC-DC, as the Heltec board is wired */
    uint8_t tcxo[4] = {0x02,                 /* 1.8 V */
                       0x00, 0x01, 0x40};    /* 5 ms startup */
    uint8_t pkt = PACKET_TYPE_LORA;
    uint8_t sync[2] = {PHY_SYNC_MSB, PHY_SYNC_LSB};
    uint8_t base[2] = {0x00, 0x00};
    uint8_t rfsw = 0x01;
    uint8_t fallback = 0x20;   /* fall back to STDBY_RC, not the reset default of FS */
    uint8_t calib = 0x7F;
    uint8_t image[2] = {0xD7, 0xDB};   /* 863-870 MHz (datasheet table 9-2) */
    uint8_t irq[8] = {0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};

    /* Named steps, because "apply failed" on a bus that answers is otherwise a guess. */
#define STEP(what, expr) do { if (!(expr)) { ESP_LOGW(TAG, "configure: %s failed", what); \
                                             return false; } } while (0)
    STEP("standby", set_standby());
    STEP("regulator", cmd(CMD_SET_REGULATOR_MODE, &reg, 1));
    STEP("tcxo", cmd(CMD_SET_DIO3_TCXO, tcxo, 4));
    /* DIO2 drives the antenna switch on this board. Without this the switch stays on the receive
     * path, so transmit raises TxDone and puts nothing on the air - which is exactly how this
     * backend behaved for its first 130 transmissions. */

    STEP("calibrate", cmd(CMD_CALIBRATE, &calib, 1));
    delay_us(5000);
    /* Image rejection is calibrated per band and is not covered by the calibrate-all above.
     * 0x6B/0x6F is the 863-870 MHz pair from the datasheet's table. */
    STEP("calibrate image", cmd(CMD_CALIBRATE_IMAGE, image, 2));
    /*
     * After the calibrations, not before, which is where the working reference puts it and where
     * this driver did not. On a V4.3 this pin is not a convenience: DIO2 drives the front end's
     * CPS, and CPS is what selects the amplifier over the bypass path. The board fits it no pull
     * resistor, so if this write is lost every transmission leaves through the bypass, about
     * 28 dB down - and only transmission suffers, because CPS is a don't-care while receiving.
     */
    STEP("dio2 rf switch", cmd(CMD_SET_DIO2_RF_SWITCH, &rfsw, 1));
    STEP("packet type", cmd(CMD_SET_PACKET_TYPE, &pkt, 1));
    STEP("frequency", set_frequency(req->channel));
    STEP("modulation", set_modulation());
    STEP("errata 15.1", fix_modulation_quality());
    STEP("errata 15.2", fix_pa_clamping());
    STEP("errata 15.4", fix_iq_polarity());
    STEP("fallback mode", cmd(CMD_SET_RXTX_FALLBACK, &fallback, 1));
    STEP("packet params", set_packet_params(255));
    STEP("sync word", reg_write(REG_LORA_SYNC_MSB, sync, 2));
    STEP("buffer base", cmd(CMD_SET_BUFFER_BASE, base, 2));
#ifdef PREAMBLE_SWEEP
    s_preamble = SWEEP_PREAMBLE[req->power_level & 0x03];
    STEP("power", set_power(1));   /* fixed ~4 dBm: the sweep varies preamble, not power */
#else
    STEP("power", set_power(req->power_level));
#endif
    STEP("ocp", set_ocp());
    /* Everything latched, only RxDone and TxDone on DIO1. */
    STEP("irq params", cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8));
    {
        uint8_t sy[2] = {0, 0}, md = 0, cl = 0, oc = 0;
        reg_read(REG_LORA_SYNC_MSB, sy, 2);
        reg_read(REG_TX_MODULATION, &md, 1);
        reg_read(REG_TX_CLAMP, &cl, 1);
        reg_read(REG_OCP, &oc, 1);
        ESP_LOGI(TAG, "regs sync=%02x%02x txmod=%02x clamp=%02x ocp=%02x", sy[0], sy[1], md, cl, oc);
    }
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

#if PIN_FEM_VCTRL >= 0
    /*
     * Which board is this? V4.3 pulls CSD up to the front end's supply through 10k; V4.2 does
     * not. It matters because the two revisions wire the front end differently - on V4.3 DIO2
     * drives CPS and GPIO5 drives CTX, while on V4.2 DIO2 drives CTX and CPS hangs on GPIO46,
     * which nothing here touches. A floating CPS would enable the amplifier at random from one
     * packet to the next, which is the shape of the transmit defect being chased.
     */
    {
        gpio_config_t probe = {.pin_bit_mask = (1ULL << PIN_FEM_CSD),
                               .mode = GPIO_MODE_INPUT};
        gpio_config(&probe);
        delay_us(1000);
        int csd = gpio_get_level((gpio_num_t)PIN_FEM_CSD);
        ESP_LOGI(TAG, "front end: CSD idles %s -> %s", csd ? "high" : "low",
                 csd ? "V4.3, KCT8103L (CPS on DIO2)" : "V4.2, GC1109 (CPS on GPIO46, undriven)");
        gpio_config(&out);
    }
#endif
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
    uint8_t rx[8] = {0};   /* 6 used; DMA rounds a receive up to a 4-byte boundary */
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

#ifdef PREAMBLE_SWEEP
    /* Take the step from the payload itself rather than from the applied power level: the beacon
     * queues, the transmit task drains, and a one-item lag between them silently shifts every
     * label by one step. Reading it here makes the label and the setting the same fact. */
    if (len && data[0] >= 'A' && data[0] <= 'D') s_preamble = SWEEP_PREAMBLE[data[0] - 'A'];
#endif
    uint8_t clear[2] = {0xFF, 0xFF};
    uint8_t txbase[2] = {0x00, 0x00};
    uint8_t go[3] = {0x00, 0x00, 0x00};   /* no timeout */
    bool ok = set_standby() && set_packet_params((uint8_t)total) &&
              cmd(CMD_SET_BUFFER_BASE, txbase, 2) &&
              buffer_write(frame, total) && cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
    if (ok) {
        uint8_t noerr[2] = {0x00, 0x00};
        cmd(0x07 /* ClearDeviceErrors */, noerr, 2);
#ifdef TCXO_SETTLE_TEST
        /*
         * STDBY_RC powers the TCXO down, so every SetTx restarts it and waits only the 5 ms in
         * SetDIO3AsTcxoCtrl. The chip calls it ready when the oscillator runs, not when it is on
         * frequency - which is why XOSC_START_ERR stays clean while the carrier is still walking.
         * At SF11/BW500 a drift over ~60 kHz/s smears the correlation peak out of its bin, and a
         * TCXO in its first milliseconds moves far faster than that. Receive never sees it: there
         * the chip sits in RX for seconds with the oscillator already up.
         *
         * Raise the oscillator early and let it settle. The step comes from the payload letter,
         * so the label and the setting cannot drift apart.
         */
        static const uint16_t SETTLE_MS[4] = {0, 10, 40, 100};
        uint16_t settle = (len && data[0] >= 'A' && data[0] <= 'D') ? SETTLE_MS[data[0] - 'A'] : 0;
        if (settle) {
            uint8_t xosc = 0x01;   /* STDBY_XOSC */
            cmd(CMD_SET_STANDBY, &xosc, 1);
            delay_us((uint32_t)settle * 1000u);
        }
#endif
        /*
         * Errata 15.1 belongs here, not in configure(): SetPacketParams above puts bit 2 of
         * REG_TX_MODULATION back, and at BW500 transmitting with it set produces a signal of
         * exactly the right length and timing that no receiver can demodulate. Applying it once
         * at startup looks identical in a register dump taken at startup and is useless by the
         * time a packet goes out.
         */
        fix_modulation_quality();
        fem_transmit(true);
        int64_t t_start = esp_timer_get_time();
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
        uint8_t st = 0, err[2] = {0, 0};
        cmd_read(0xC0 /* GetStatus */, &st, 1);
        cmd_read(0x17 /* GetDeviceErrors */, err, 2);
        ESP_LOGI(TAG, "send: %u bytes, air %lld us, status 0x%02x, errors 0x%04x", (unsigned)total,
                 esp_timer_get_time() - t_start, st, (unsigned)((err[0] << 8) | err[1]));
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
#ifdef SX_FREQ_ERROR
        /*
         * What the modem had to pull the incoming carrier by to lock onto it - so, measured
         * against a transmitter we trust, the error of this board's own reference.
         *
         * Receiving cleanly proves nothing about our frequency: a LoRa receiver tracks the signal
         * and tolerates a quarter of the bandwidth, 125 kHz here. Transmitting is where an error
         * has to be inside the *other* end's window, and nothing says an E220's window is as wide
         * as the theory allows. That would be transmit-only, per-packet, indifferent to power,
         * identical across two drivers sharing one board, and absent on a module with its own
         * reference - which is the whole shape of what we are chasing.
         */
        {
            uint8_t fe[3] = {0};
            if (reg_read(REG_FREQ_ERROR, fe, 3)) {
                uint32_t raw = ((uint32_t)fe[0] << 16) | ((uint32_t)fe[1] << 8) | fe[2];
                raw &= 0x0FFFFF;
                int32_t sign = 1;
                if (raw & 0x80000) { raw = (~raw + 1) & 0xFFFFF; sign = -1; }
                /* 1.55 * raw / (1600 / bw_khz), bw = 500 kHz -> 0.484 Hz per count */
                int32_t hz = sign * (int32_t)((raw * 155u) / 320u);
                ESP_LOGI(TAG, "freq error %+ld Hz (raw %lu)", (long)hz, (unsigned long)raw);
            }
        }
#endif
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
