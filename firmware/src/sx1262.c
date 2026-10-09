/* Compiled only into the build that selects this backend; see radio.h. */
#ifdef RADIO_BACKEND_SX1262

#include "radio.h"
#include "ebyte_frame.h"

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

/* So radio_identity() names the PHY actually compiled in, rather than the one it used to be. */
#define STR_(x) #x
#define STR(x) STR_(x)

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
#define CMD_GET_PACKET_STATUS    0x14

#define REG_LORA_SYNC_MSB 0x0740
#define REG_LORA_SYNC_LSB 0x0741
#define REG_OCP           0x08E7
#define REG_TX_MODULATION 0x0889
#define REG_TX_CLAMP      0x08D8
#define REG_IQ_CONFIG     0x0736
#define REG_FREQ_ERROR    0x076B
#define CMD_SET_RXTX_FALLBACK 0x93

#define IRQ_CLEAR_HI 0xFF
#define IRQ_CLEAR_LO 0xFF

#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_PREAMBLE     (1u << 2)
#define IRQ_HEADER_VALID (1u << 4)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)

#define PACKET_TYPE_LORA 0x01
#define STANDBY_RC       0x00

/*
 * The measured E220 PHY. Not parameters to tune - every one of them was established on hardware
 * and changing any breaks demodulability with the group. See docs/PHY-notes.md.
 */
#ifdef PHY_ALT_RATE
/*
 * Bench: the same family at EBYTE air rate 0 rather than 2. Semtech's erratum 15.1 says plainly
 * that "some sensitivity degradation may be observed on any LoRa device, when receiving signals
 * transmitted by the SX1261/2 with a LoRa BW of 500 kHz" - which is this project's symptom in the
 * vendor's own words, and is bandwidth-specific. Moving both ends off 500 kHz tests exactly that.
 *
 * 0.3 kbps is SF12/BW125 by the LoRa rate identity: 12 * 125000 * 0.8 / 4096 = 293 bps. At a
 * 32.8 ms symbol LDRO is genuinely required, which it already is.
 */
#define PHY_SF        12
#define PHY_BW_CODE   0x04   /* 125 kHz */
#define PHY_BW_KHZ    125
#else
#define PHY_SF        11
#define PHY_BW_CODE   0x06   /* 500 kHz */
#define PHY_BW_KHZ    500
#endif
/*
 * Coding rate. 4/5 because that is what this project recorded for the module - but the note
 * that recorded it also argued the value does not matter, since the explicit header carries it
 * to the receiver. That is true of whether a packet decodes and false of how well it survives:
 * 4/8 is twice the redundancy, and the datasheet is explicit that the gain shows up in
 * interference. The channel an E220 sits in here has a floor around -80 dBm, which is some
 * thirty decibels above thermal, so interference is the regime.
 *
 * Overridable so that can be measured rather than assumed. 0x01 = 4/5 … 0x04 = 4/8.
 */
#ifndef PHY_CR_CODE
#define PHY_CR_CODE   0x01
#endif
/*
 * Forced on: the chip's own rule would leave it off here, and every reference driver does leave
 * it off at SF11/BW500, where the symbol is 4.096 ms against the 16 ms threshold. It is on
 * because an E220's transmissions decode with the payload CRC passing against a receiver that
 * has it on, and a mismatch garbles the payload rather than the header.
 *
 * Overridable for the bench, because that argument is about what a module TRANSMITS and this
 * setting also governs what its receiver expects. Our own receiver is forced into the same
 * non-standard mode, so a link between two of our boards says nothing about it either way.
 */
#ifndef PHY_LDRO
#define PHY_LDRO      0x01
#endif
#define PHY_PREAMBLE  8
/*
 * The two sync symbols an EBYTE module puts on the air are 40 and 36, read off the air with an
 * RTL-SDR on 2026-10-06 (bench/sdr_lora.py). The chip makes each from the top five bits of its
 * register byte, times four - 0x54 gives 40, 0x48 gives 36, the low three bits do nothing. The
 * 0x54 0x54 written before sent 40 and 40: EBYTE receivers took it about one time in four, and
 * our own receiver, finding two identical symbols, locked on the second one time in five and
 * read the data a symbol late (PHY-notes 2026-09-25). Interleaved on one bench, an E22 received
 * 7 of 30 packets with the old pair and 30 of 30 with this one; across rooms an E220, the NOR7E's
 * module, 5 of 40 and 38 of 40.
 */
#define PHY_SYNC_MSB  0x54
#define PHY_SYNC_LSB  0x48

/*
 * EBYTE's own frame, which the module's MCU adds and strips. A bare SX1262 has no such MCU, so
 * the backend does it - and above the seam nobody can tell the difference.
 */
#define EBYTE_HEADER_LEN 8
#define EBYTE_XOR        0x12

/* What each level sets the chip to, and what leaves the antenna for it (TX_DBM, reported to the
 * phone as tx_dbm).
 *
 * Not a linear dBm knob. Writing the wanted dBm straight into SetTxParams and leaving
 * SetPaConfig at the +22 dBm pair does not produce that power - RadioLib carries a measured
 * table (radiolib-org/power-tests) precisely because the naive mapping is wrong, and on this
 * bench the naive version reached an E220 zero times out of 27 where the table reached it. */
#if PIN_FEM_VCTRL >= 0
/* Heltec V4.3: the KCT8103L adds 13 dB to a drive of up to 13 dBm and less above it - 11 dB at
 * 17, 7 at 21, where the output tops out near 28 dBm (Heltec's table, as in Meshtastic's
 * LoRaFEMInterface.cpp and RNode's Boards.h). Max at that ceiling, Low at about the EU limit
 * for 868.0-868.6 MHz, 25 mW. Every row is RadioLib's measured setting for that chip output
 * (paOptimizedTable in its SX1262.cpp, radiolib-org/power-tests). */
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {1, 6, 22},   /* chip 17 dBm -> 28 dBm */
    {2, 2, 21},   /* chip 11 dBm -> 24 dBm */
    {3, 1, 21},   /* chip  6 dBm -> 19 dBm */
    {1, 1, 14},   /* chip  1 dBm -> 14 dBm */
};
static const int8_t TX_DBM[4] = {28, 24, 19, 14};
#else
/* No front end - the T-Deck, or a Heltec V3: the chip's own output, from its +22 dBm ceiling down
 * to the same 14 dBm Low as the Heltec. RadioLib's measured settings, as above. */
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {4, 7, 22},   /* 22 dBm */
    {3, 6, 22},   /* 20 dBm */
    {1, 6, 22},   /* 17 dBm */
    {1, 4, 20},   /* 14 dBm */
};
static const int8_t TX_DBM[4] = {22, 20, 17, 14};
#endif

/* ---- bus ----------------------------------------------------------------- */

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static radio_request_t s_applied;
static bool s_have_applied;
static bool s_in_rx;
#ifdef BENCH_CMD
/* Set for one packet by radio_bench_tx and consulted by radio_send; -1 means "leave alone". */
static int s_once_preamble = -1, s_once_sync = -1;
/* Bench: the configured preamble length while receiving (and for any later packet without its own
 * preamble= token), set by RXPRE. -1: PHY_PREAMBLE. */
static int s_rx_preamble = -1;
/* Bench: receive with an implicit header of this many bytes and no CRC, set by IMPL; 0: the normal
 * explicit header. */
static int s_rx_implicit;
#endif
#ifdef RX_STAGES
static unsigned s_stage_preamble, s_stage_header, s_stage_rx_done, s_stage_hdr_err, s_stage_crc_err;
static unsigned s_stage_timeout;
static int64_t s_stage_next;
static int64_t s_t_preamble, s_t_header;
#endif

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
    /*
     * Bench only: the same view RadioLib's RLB_SPI debug gives, so the two can be diffed.
     *
     * Both directions, because the question this answers is usually "is the chip the one
     * replying". Sending alone cannot tell a live radio from an empty bus: MISO reads all ones
     * with nothing driving it and all zeroes with the chip held in reset, and either looks like
     * a plausible status byte until you see it next to what went out.
     */
#ifdef SX_SPI_TRACE_ALL
    if (1) {
#else
    if (tx[0] != CMD_GET_IRQ_STATUS && tx[0] != 0xC0) {
#endif
        char out[120], in[120];
        size_t ko = 0, ki = 0, show = len > 20 ? 20 : len;
        for (size_t i = 0; i < show && ko + 4 < sizeof(out); i++) {
            ko += (size_t)snprintf(out + ko, sizeof(out) - ko, "%02X ", tx[i]);
            ki += (size_t)snprintf(in + ki, sizeof(in) - ki, "%02X ", rx ? rx[i] : 0);
        }
        ESP_LOGI(TAG, "SPI ->%s%s  <-%s%s", out, len > show ? "..." : "",
                 rx ? in : "(discarded)", rx && len > show ? "..." : "");
    }
#endif
    return ok;
}

/*
 * Set when the chip reported a command error, cleared at the start of configure(). A transfer
 * that the SPI driver completed says the bus worked, not that the radio accepted what it was
 * told - and publishing configured=1 on the strength of the former is how a rejected frequency
 * or power would pass unnoticed.
 */
static bool s_cmd_error;

static bool cmd(uint8_t opcode, const uint8_t *args, size_t n) {
    uint8_t buf[16], st[16] = {0};
    if (n + 1 > sizeof(buf)) return false;
    buf[0] = opcode;
    if (n) memcpy(&buf[1], args, n);
    if (!xfer(buf, st, n + 1)) return false;
    /* Status bits 3:1: 3 command timeout, 4 processing error, 5 failure to execute. */
    const uint8_t status = (uint8_t)((st[n ? 1 : 0] >> 1) & 0x07);
    if (status >= 3 && status <= 5) {
        ESP_LOGW(TAG, "command %02X rejected, status %u", opcode, status);
        s_cmd_error = true;
    }
    return true;
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
 * usual. The workaround is required, but the old zero-reception result did not establish it
 * as the cause: later register captures verified the relevant bit, and the EBYTE frame header
 * also needed correction. See docs/PHY-notes.md for the controlled measurements.
 */

static bool fix_modulation_quality(void) {
    /*
     * Errata 15.1 is not "clear bit 2" - it is "clear it at BW500, set it at every other
     * bandwidth". Writing 0x00 unconditionally was right for the shipping PHY and wrong for the
     * one build whose entire purpose is to leave 500 kHz, which would then have carried the
     * BW500 workaround into BW125 and measured something other than what it claimed.
     *
     * Constant rather than a read-modify-write on purpose: a read that quietly returns a status
     * byte would otherwise write that status byte into a modem register.
     */
    uint8_t v = (PHY_BW_CODE == 0x06) ? 0x00 : 0x04;
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

/* Bench only, and zero everywhere else: see bench_tx_t::foff. */
static int32_t s_freq_offset_hz;

static bool set_frequency(uint8_t channel) {
    /* 850.125 MHz + channel, in the chip's 2^25/32 MHz steps. */
    uint64_t hz = (uint64_t)((int64_t)(850125000ll + (int64_t)channel * 1000000ll) +
                             s_freq_offset_hz);
    uint32_t frf = (uint32_t)((hz << 25) / 32000000ull);
    uint8_t a[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    return cmd(CMD_SET_RF_FREQUENCY, a, 4);
}

static bool set_modulation(void) {
    uint8_t a[4] = {PHY_SF, PHY_BW_CODE, PHY_CR_CODE, PHY_LDRO};
    return cmd(CMD_SET_MODULATION, a, 4);
}

static bool set_packet_params(uint8_t payload_len) {
#ifdef BENCH_CMD
    const uint16_t pre = s_once_preamble >= 0 ? (uint16_t)s_once_preamble
                         : s_rx_preamble >= 0 ? (uint16_t)s_rx_preamble : PHY_PREAMBLE;
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
    /* Ramp time. 0x04 is 200 us; the field runs to 0x07, 3.4 ms. A gentler ramp is a cleaner
     * spectrum over exactly the part of the packet where this link loses its headers. */
#ifndef TX_RAMP_CODE
#define TX_RAMP_CODE 0x04
#endif
    uint8_t tx[2] = {(uint8_t)POWER_PA[i].pa_val, TX_RAMP_CODE};
    return cmd(CMD_SET_TX_PARAMS, tx, 2);
}

static bool clear_irq(void) {
    uint8_t c[2] = {IRQ_CLEAR_HI, IRQ_CLEAR_LO};
    return cmd(CMD_CLEAR_IRQ_STATUS, c, 2);
}

static bool configure(const radio_request_t *req) {
    uint8_t reg = 0x01;                      /* DC-DC, as the Heltec board is wired */
    /* The voltage comes from SX_TCXO_MV rather than a literal, which is what that macro always
     * claimed to control and never did - -DSX_TCXO_MV=1700 used to change nothing at all, in a
     * project where half the open questions are about carrier drift. */
    uint8_t tcxo[4] = {SX_TCXO_CODE,
                       0x00, 0x01, 0x40};    /* 5 ms startup */
    uint8_t pkt = PACKET_TYPE_LORA;
    uint8_t sync[2] = {PHY_SYNC_MSB, PHY_SYNC_LSB};
    uint8_t base[2] = {0x00, 0x00};
    uint8_t rfsw = 0x01;
    uint8_t fallback = 0x20;   /* fall back to STDBY_RC, not the reset default of FS */
    uint8_t calib = 0x7F;
    /*
     * Image rejection is calibrated per band, and the band follows the channel the phone asked
     * for - channels 0..80 span 850.125 to 930.125 MHz, so a fixed 863-870 pair would calibrate
     * the wrong band everywhere outside channels 13..19. Datasheet table 9-2 has no row between
     * 870 and 902 MHz, so the halfway point picks the nearer of the two.
     */
    const bool upper_band = req->channel >= 36;   /* 850.125 + 36 = 886.125 MHz */
    uint8_t image[2] = {upper_band ? 0xE1 : 0xD7, upper_band ? 0xE9 : 0xDB};
    uint8_t irq[8] = {0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};

    /* Named steps, because "apply failed" on a bus that answers is otherwise a guess. */
#define STEP(what, expr) do { if (!(expr)) { ESP_LOGW(TAG, "configure: %s failed", what); \
                                             return false; } } while (0)
    STEP("standby", set_standby());
    /*
     * Cleared after the first command, not before it. The status byte a command returns reports
     * what the chip was doing *before* it, so the first one describes a chip this sequence has
     * not touched yet - on a T-Deck, one that had been running someone else's firmware until a
     * moment ago. Counting that against us failed every configuration on an otherwise healthy
     * radio. Nothing is lost by skipping it: whether the standby above worked is reported by
     * the status byte of the command after it, which is checked.
     */
    s_cmd_error = false;
    STEP("regulator", cmd(CMD_SET_REGULATOR_MODE, &reg, 1));
    STEP("tcxo", cmd(CMD_SET_DIO3_TCXO, tcxo, 4));
    STEP("calibrate", cmd(CMD_CALIBRATE, &calib, 1));
    delay_us(5000);
    /* Not covered by the calibrate-all above. An earlier comment here claimed 0x6B/0x6F was the
     * 863-870 row; that is the 470-510 one, and the code was briefly wrong to match. */
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
    STEP("power", set_power(req->power_level));
    STEP("ocp", set_ocp());
    /* Everything latched, only RxDone and TxDone on DIO1. */
    STEP("irq params", cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8));
    /* Every step above reported a completed transfer; this asks whether the chip also accepted
     * them, so configured=1 means the radio agreed rather than that the bus worked. */
    STEP("no command was rejected", !s_cmd_error);
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
    uint8_t clear[2] = {IRQ_CLEAR_HI, IRQ_CLEAR_LO};
    uint8_t rx[3] = {0xFF, 0xFF, 0xFF};   /* continuous */
    /*
     * Put the payload length back to the maximum. SetPacketParams' length field is "the size to
     * transmit or the largest the receiver will accept", so transmitting leaves the receiver
     * capped at whatever was just sent - a 46-byte frame out means anything longer coming back
     * is refused. Two bench radios sending the same size never notice; a group whose packets
     * grow with the number of riders would, and only after a transmission.
     */
#ifdef BENCH_CMD
    if (s_rx_implicit > 0) {
        /*
         * Bench: an implicit-header receiver has no header to reject, so it hands over every
         * packet it synchronises on - including the ones the explicit receiver drops with a
         * header error - with the transmitter's own header nibbles as the first bytes. With CRC
         * off the payload comes back as received, errors and all.
         */
        const uint16_t pre = s_rx_preamble >= 0 ? (uint16_t)s_rx_preamble : PHY_PREAMBLE;
        uint8_t a[6] = {(uint8_t)(pre >> 8), (uint8_t)pre, 0x01 /* implicit */,
                        (uint8_t)s_rx_implicit, 0x00 /* CRC off */, 0x00 /* IQ normal */};
        if (!cmd(CMD_SET_PACKET_PARAMS, a, 6)) return false;
    } else if (!set_packet_params(255)) {
        return false;
    }
#else
    if (!set_packet_params(255)) return false;
#endif
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
     * Which board is this? V4.3 pulls CSD up through 10k, V4.2 does not - and it matters, because
     * the revisions wire the front end differently: on V4.3 DIO2 drives CPS and GPIO5 drives CTX,
     * while on V4.2 DIO2 drives CTX and CPS hangs on GPIO46, which nothing here touches. A
     * floating control input would enable the amplifier at random from packet to packet.
     *
     * The pull-up references Vfem, not the main rail, so the supply has to be up before the
     * question means anything. Reading it first - as the first version of this did - answers
     * "V4.2" on every board, including one with 4.3 printed on it.
     */
    gpio_set_level((gpio_num_t)PIN_FEM_VCTRL, 1);
    delay_us(10000);
    {
        gpio_config_t probe = {.pin_bit_mask = (1ULL << PIN_FEM_CSD),
                               .mode = GPIO_MODE_INPUT};
        gpio_config(&probe);
        delay_us(1000);
        int csd = gpio_get_level((gpio_num_t)PIN_FEM_CSD);
        ESP_LOGI(TAG, "front end: with Vfem up, CSD idles %s -> %s", csd ? "high" : "low",
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
    /*
     * This backend does not listen before it talks, so accepting lbt=1 would have the phone
     * shown configured=1 lbt=1 over a transmitter that never checks the channel. Refusing is the
     * honest answer until it is implemented.
     */
    if (req->lbt) {
        ESP_LOGW(TAG, "listen-before-talk requested; this backend does not implement it");
        return RADIO_MISMATCH;
    }
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

#ifdef BENCH_CMD
/* Set for one packet by radio_bench_tx; -1 means derive the header from the payload. */
static int s_once_hdr = -1;
/* Set for one packet by radio_bench_tx; -1 means untouched. See bench_tx_t. */
static int s_once_ldo = -1, s_once_txmod = -1, s_once_cal = -1, s_once_pa = -1;
/* What the chip said just before SetTx, logged once the packet is out. */
static bool s_seen_reg_ok, s_seen_cal_rejected;
static uint8_t s_seen_txmod;
static uint16_t s_seen_errors;
#endif

bool radio_send(const uint8_t *data, size_t len) {
    if (!RADIO_TX_ENABLED) return false;
    if (len == 0 || len > 255 - EBYTE_HEADER_LEN) return false;
    if (!bus_lock()) return false;

    /* The frame an E220 expects, built here because there is no module MCU to build it. */
    uint8_t frame[EBYTE_HEADER_LEN + 255];
    ebyte_frame_encode(frame, s_have_applied ? s_applied.channel : RADIO_DEFAULT_CHANNEL,
                       data, len);
#ifdef BENCH_CMD
    if (s_once_hdr >= 0) {
        frame[2] = (uint8_t)(s_once_hdr >> 8);
        frame[3] = (uint8_t)s_once_hdr;
    }
#endif
    size_t total = EBYTE_HEADER_LEN + len;

#ifdef BENCH_CMD
    if (s_once_sync >= 0) {
        uint8_t sw[2] = {(uint8_t)(s_once_sync >> 8), (uint8_t)s_once_sync};
        set_standby();
        reg_write(REG_LORA_SYNC_MSB, sw, 2);
    }
#endif
    uint8_t clear[2] = {IRQ_CLEAR_HI, IRQ_CLEAR_LO};
    uint8_t txbase[2] = {0x00, 0x00};
    uint8_t go[3] = {0x00, 0x00, 0x00};   /* no timeout */
    bool ok = set_standby() && set_packet_params((uint8_t)total) &&
              cmd(CMD_SET_BUFFER_BASE, txbase, 2) &&
              buffer_write(frame, total) && cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
    if (ok) {
        uint8_t noerr[2] = {0x00, 0x00};
        cmd(0x07 /* ClearDeviceErrors */, noerr, 2);
#ifdef BENCH_CMD
        /* Configure calibrates before SetRfFrequency, as Semtech's reference does: this redoes
         * RC, PLL and ADC at the frequency actually in use. Not the image, which is receive-side
         * and band-specific. The control waits the same 5 ms without calibrating. */
        if (s_once_cal >= 0) {
            if (s_once_cal) {
                const bool before = s_cmd_error;
                uint8_t mask = 0x3F;
                s_cmd_error = false;
                cmd(CMD_CALIBRATE, &mask, 1);
                s_seen_cal_rejected = s_cmd_error;
                s_cmd_error = before;
            }
            delay_us(5000);
            uint8_t err[2] = {0, 0};
            cmd_read(0x17 /* GetDeviceErrors */, err, 2);
            s_seen_errors = (uint16_t)((err[0] << 8) | err[1]);
        }
#endif
        /*
         * Errata 15.1 belongs here, not in configure(): SetPacketParams above puts bit 2 of
         * REG_TX_MODULATION back, so applying it once at startup looks right in a register dump
         * and is gone by the time a packet goes out. An earlier note here said a packet sent
         * with the bit set is undecodable; measured, it is not (PHY-notes, series test B).
         */
        fix_modulation_quality();
#ifdef BENCH_CMD
        /* Still in STDBY_RC from the chain above, which SetRegulatorMode requires. After the
         * erratum write, so txmod=1 is what goes out rather than being overwritten. */
        if (s_once_ldo >= 0) {
            uint8_t reg = s_once_ldo ? 0x00 : 0x01;
            s_seen_reg_ok = cmd(CMD_SET_REGULATOR_MODE, &reg, 1);
        }
        if (s_once_pa >= 0) {
            /* The pair pinned at Semtech's validated (4, 7) and only the drive level moving; the
             * over-current limit has to follow SetPaConfig. */
            static const int8_t PA_DBM[4] = {22, 14, 6, -2};
            uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
            uint8_t tx[2] = {(uint8_t)PA_DBM[s_once_pa], TX_RAMP_CODE};
            cmd(CMD_SET_PA_CONFIG, pa, 4);
            cmd(CMD_SET_TX_PARAMS, tx, 2);
            set_ocp();
        }
        if (s_once_txmod >= 0) {
            uint8_t m = 0;
            reg_read(REG_TX_MODULATION, &m, 1);
            m = s_once_txmod ? (uint8_t)(m | 0x04) : (uint8_t)(m & ~0x04);
            reg_write(REG_TX_MODULATION, &m, 1);
            reg_read(REG_TX_MODULATION, &s_seen_txmod, 1);
        }
#endif
        fem_transmit(true);
        int64_t t_start = esp_timer_get_time();
        ok = cmd(CMD_SET_TX, go, 3);
        /*
         * Derived from the compiled PHY, not a flat three seconds: at SF12/BW125 a hundred-byte
         * payload is over four seconds on the air, and a fixed deadline would abort it and look
         * like a failed bandwidth experiment rather than a too-short timer.
         */
        const uint32_t sym_us = ((1u << PHY_SF) * 1000u) / PHY_BW_KHZ;
        const uint32_t symbols = PHY_PREAMBLE + 20u +
                                 (8u * (uint32_t)total * 5u) / (4u * (PHY_SF - 2u));
        int64_t deadline = esp_timer_get_time() + (int64_t)symbols * sym_us + 500 * 1000;
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
        /* GetStatus has no payload: the status byte is the one clocked out beside the opcode,
         * so the shared cmd_read() - which starts at rx[2] - reads past it. */
        uint8_t st = 0, err[2] = {0, 0};
        {
            uint8_t sx[2] = {0xC0, 0x00}, sr[2] = {0, 0};
            if (xfer(sx, sr, 2)) st = sr[1];
        }
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
static void log_freq_error(const char *what) {
    uint8_t fe[3] = {0};
    if (reg_read(REG_FREQ_ERROR, fe, 3)) {
        uint32_t raw = ((uint32_t)fe[0] << 16) | ((uint32_t)fe[1] << 8) | fe[2];
        raw &= 0x0FFFFF;
        int32_t sign = 1;
        if (raw & 0x80000) { raw = (~raw + 1) & 0xFFFFF; sign = -1; }
        /* 1.55 * raw / (1600 / bw_khz), bw = 500 kHz -> 0.484 Hz per count */
        int32_t hz = sign * (int32_t)((raw * 155u) / 320u);
        ESP_LOGI(TAG, "%s %+ld Hz (raw %lu)", what, (long)hz, (unsigned long)raw);
    }
}
#endif

/* The last packet handed up, as the modem measured it: see radio_last_rssi. */
static int s_last_rssi;
static bool s_have_rssi;

size_t radio_receive(uint8_t *out, size_t out_len, uint32_t idle_gap_ms, uint32_t first_wait_ms) {
    (void)idle_gap_ms;   /* a UART packet boundary; SPI hands over whole packets */
    s_have_rssi = false;
    if (!bus_lock()) return 0;
    if (!s_in_rx) enter_rx();

    size_t produced = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)first_wait_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq[2] = {0, 0};
        if (!cmd_read(CMD_GET_IRQ_STATUS, irq, 2)) break;
        uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
#ifdef RX_STAGES
        /*
         * Where a reception dies, rather than only whether it lived.
         *
         * The modem latches each stage separately and the IRQ mask here is 0xFFFF, so all of them
         * are already being recorded - this loop simply threw the distinction away. Counting them
         * turns "it fails at acquisition" from an inference into a measurement: no preamble at all
         * is one problem, a preamble that never yields a valid header is a different one, and a
         * valid header followed by a CRC error is a third.
         *
         * Preamble and header-valid are consumed as they are seen, because they latch early and
         * would otherwise be counted again on every poll of the same packet.
         */
        {
            /*
             * When each stage happened, not only how often.
             *
             * Header-valid to rx-done is the payload's time on the air and nothing else, so it
             * counts payload symbols directly: at SF11/BW500 a 46-byte frame is 258 ms at
             * CR 4/5, 303 at 4/6, 348 at 4/7 and 393 at 4/8. That reads a transmitter's coding
             * rate off the air without asking it. Preamble-detected to header-valid does the
             * same for preamble length. Both were argued about for a day; this poll is 10 ms
             * and the steps to tell apart are 45.
             */
            const int64_t t_now = esp_timer_get_time();
            if (flags & IRQ_PREAMBLE) {
                s_t_preamble = t_now;
                s_t_header = 0;
            }
            if ((flags & IRQ_HEADER_VALID) && s_t_header == 0) s_t_header = t_now;
            if (flags & IRQ_RX_DONE) {
                ESP_LOGI(TAG, "timing: preamble->header %lld ms, header->done %lld ms",
                         (s_t_header && s_t_preamble) ? (s_t_header - s_t_preamble) / 1000 : -1,
                         s_t_header ? (t_now - s_t_header) / 1000 : -1);
                s_t_preamble = 0;
                s_t_header = 0;
            }
        }
        if (flags & (IRQ_PREAMBLE | IRQ_HEADER_VALID)) {
            uint8_t eat[2] = {(uint8_t)((flags & (IRQ_PREAMBLE | IRQ_HEADER_VALID)) >> 8),
                              (uint8_t)(flags & (IRQ_PREAMBLE | IRQ_HEADER_VALID))};
            if (flags & IRQ_PREAMBLE) s_stage_preamble++;
            if (flags & IRQ_HEADER_VALID) s_stage_header++;
            cmd(CMD_CLEAR_IRQ_STATUS, eat, 2);
        }
        if (flags & IRQ_HEADER_ERR) s_stage_hdr_err++;
        if (flags & IRQ_CRC_ERR) s_stage_crc_err++;
        if (flags & IRQ_TIMEOUT) s_stage_timeout++;
        if (flags & IRQ_RX_DONE) s_stage_rx_done++;
        {
            int64_t now = esp_timer_get_time();
            if (now >= s_stage_next) {
                s_stage_next = now + 30 * 1000 * 1000;
                ESP_LOGI(TAG, "stages: preamble %u -> header %u -> rx %u | hdr_err %u crc_err %u timeout %u",
                         s_stage_preamble, s_stage_header, s_stage_rx_done,
                         s_stage_hdr_err, s_stage_crc_err, s_stage_timeout);
            }
        }
#endif
        if ((flags & IRQ_TIMEOUT) && !(flags & IRQ_RX_DONE)) {
            /* Symbol timeout exits RX even when SetRx uses the continuous timeout value.
             * The software flag must follow the chip, otherwise this receiver stays deaf. */
            s_in_rx = false;
#ifdef RX_STAGES
            s_t_preamble = s_t_header = 0;
#endif
            if (!enter_rx()) break;
            continue;
        }
        if (!(flags & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR))) {
            /*
             * Blocking, not delay_us. This poll runs for the whole of first_wait_ms and the
             * task loops straight back into it, so a busy wait here never lets the scheduler
             * run anything else: the idle task on this core is starved and the watchdog fires
             * every five seconds forever. That is worth more than tidy logs - a core spinning
             * flat out draws current and makes noise a few centimetres from the receiver, and
             * this firmware is being compared against ones that do not do it.
             *
             * One tick, because the tick is 100 Hz and anything under it rounds to no delay at
             * all. Granularity costs nothing: the modem latches these flags until they are
             * cleared, so a slower poll reads the same events a little later.
             */
            vTaskDelay(1);
            continue;
        }
        uint8_t clear[2] = {IRQ_CLEAR_HI, IRQ_CLEAR_LO};
        if (flags & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
#ifdef RX_STAGES
            const long long since_preamble =
                s_t_preamble ? (esp_timer_get_time() - s_t_preamble) / 1000 : -1;
            s_t_preamble = s_t_header = 0;
            /*
             * The same measurements for a reception that died. Measured 2026-09-24: at a header
             * error the packet status and RegFreqError still hold the previous delivered packet's
             * values, so only the time since the preamble belongs to this reception.
             */
            {
                uint8_t ps[3] = {0, 0, 0};
                if (cmd_read(CMD_GET_PACKET_STATUS, ps, 3)) {
                    const int snr4 = (int)(int8_t)ps[1];
                    const int mag = snr4 < 0 ? -snr4 : snr4;
                    ESP_LOGI(TAG, "rx fail %s rssi %d dBm, snr %s%d.%02d dB, signal rssi %d dBm, "
                             "preamble->fail %lld ms", (flags & IRQ_HEADER_ERR) ? "hdr" : "crc",
                             -(int)ps[0] / 2, snr4 < 0 ? "-" : "", mag / 4, mag % 4 * 25,
                             -(int)ps[2] / 2, since_preamble);
                }
            }
#ifdef SX_FREQ_ERROR
            log_freq_error("fail freq error");
#endif
#endif
            cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
            break;
        }
        uint8_t st[2] = {0, 0};
        if (!cmd_read(CMD_GET_RX_BUFFER_STATUS, st, 2)) break;
        size_t n = st[0];
        uint8_t frame[EBYTE_HEADER_LEN + 255];
        if (n > sizeof(frame)) n = sizeof(frame);
        if (n && buffer_read(st[1], frame, n)) {
            /* Measured by the modem for every packet it demodulates: RSSI in half-dBm, SNR in
             * quarter-dB and signed, then the signal's own RSSI after despreading. */
            uint8_t ps[3] = {0, 0, 0};
            const bool have_ps = cmd_read(CMD_GET_PACKET_STATUS, ps, 3);
#ifdef RX_STAGES
            /*
             * How good the signal was, per packet, rather than how often a rare event fired.
             *
             * Counting HEADER_ERR answers "how many times did something fail", which is a
             * handful of events over a run and cannot separate two transmitters that are both
             * mostly working. The modem measures every packet it demodulates: RSSI says how
             * loud it was. SNR saturates near 8 dB on this bench, so equal readings at these
             * strong levels do not establish equal modulation quality. It becomes informative
             * closer to the receiver's sensitivity limit.
             *
             * Scaling is the datasheet's: RSSI in half-dBm, SNR in quarter-dB and signed.
             */
            {
                if (have_ps) {
                    const int rssi = -(int)ps[0] / 2;
                    const int snr4 = (int)(int8_t)ps[1];
                    const int sig = -(int)ps[2] / 2;
                    /* The sign printed separately: snr4 / 4 is 0 for -0.75, which used to come
                     * out as "0.75" - exactly the range this instrument is read in near the
                     * sensitivity limit. */
                    const int mag = snr4 < 0 ? -snr4 : snr4;
                    ESP_LOGI(TAG, "rx quality rssi %d dBm, snr %s%d.%02d dB, signal rssi %d dBm",
                             rssi, snr4 < 0 ? "-" : "", mag / 4, mag % 4 * 25, sig);
                }
            }
            /*
             * The header as this receiver decoded it: 0x0749 bits 6:4 hold the coding rate and
             * 0x076B bit 4 the CRC flag (RadioLib's getLoRaRxHeaderInfo). Our frames match a
             * module's byte for byte, but those two fields of its PHY header were never read.
             */
            {
                uint8_t cr = 0, crc = 0;
                if (reg_read(0x0749, &cr, 1) && reg_read(0x076B, &crc, 1))
                    ESP_LOGI(TAG, "rx header regs 0749=%02X 076B=%02X", cr, crc);
            }
            /*
             * The header exactly as it came off the air. This backend builds its own EBYTE frame
             * because there is no module MCU to build one, and the only way to know whether it
             * matches what a module really sends is to hold the two side by side. The check
             * below looks at two bytes of the eight; a module's firmware need not be so
             * forgiving, so the other six are worth seeing rather than assuming.
             */
            ESP_LOGI(TAG, "rx frame %u bytes, header %02X %02X %02X %02X %02X %02X %02X %02X",
                     (unsigned)n, frame[0], frame[1], frame[2], frame[3],
                     frame[4], frame[5], frame[6], frame[7]);
#ifdef RX_RAW_FRAME
            char hex[2 * 255 + 1];
            for (size_t i = 0; i < n; ++i) snprintf(hex + 2 * i, 3, "%02X", frame[i]);
            ESP_LOGI(TAG, "rx raw: %s", hex);
#endif
#endif
            /* Strip EBYTE's frame and undo the obfuscation, so the caller sees exactly the
             * bytes a module would have put on its UART. A frame that is not one is dropped
             * rather than passed up half-understood. */
            if (n > EBYTE_HEADER_LEN && frame[0] == 0x2C && frame[7] == n - EBYTE_HEADER_LEN) {
                size_t payload = n - EBYTE_HEADER_LEN;
                if (payload > out_len) payload = out_len;
                for (size_t i = 0; i < payload; i++) out[i] = frame[EBYTE_HEADER_LEN + i] ^ EBYTE_XOR;
                produced = payload;
                if (have_ps) {
                    s_last_rssi = -(int)ps[0] / 2 - SX_RX_GAIN_DB;
                    s_have_rssi = true;
                }
            } else {
                ESP_LOGW(TAG, "rx: %u bytes but not an EBYTE frame", (unsigned)n);
            }
        }
#ifdef SX_FREQ_ERROR
        log_freq_error("freq error");
#endif
        cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
        break;
    }
    bus_unlock();
    return produced;
}

const char *radio_name(void) { return "sx1262"; }

/* At the antenna: what the front end adds on receive is taken off, see SX_RX_GAIN_DB. */
bool radio_last_rssi(int *dbm) {
    if (s_have_rssi) *dbm = s_last_rssi;
    return s_have_rssi;
}

bool radio_tx_dbm(int8_t out[4]) {
    memcpy(out, TX_DBM, sizeof(TX_DBM));
    return true;
}

#ifdef BENCH_CMD
#include "radio_bench.h"

bool radio_bench_tx(const bench_tx_t *req, const uint8_t *payload, size_t len, uint32_t *air_us) {
    /*
     * Power goes through the configured request so the change is visible in the status line and
     * survives into radio_verify, rather than being poked behind the bridge's back. Preamble and
     * sync are per-packet and revert on their own, because they are properties of one
     * transmission rather than of the radio's configuration.
     */
    if (req->power >= 0) {
        radio_request_t want = s_have_applied ? s_applied : (radio_request_t){
            .channel = RADIO_DEFAULT_CHANNEL, .power_level = RADIO_DEFAULT_POWER_LEVEL,
            .lbt = false};
        want.power_level = (uint8_t)req->power;
        if (radio_apply(&want) != RADIO_OK) return false;
    }
    s_once_preamble = req->preamble;
    s_once_sync = req->sync;
    s_once_hdr = req->hdr;
    s_once_ldo = req->ldo;
    s_once_txmod = req->txmod;
    s_once_cal = req->cal;
    s_once_pa = req->pa;
    /*
     * Frequency is the one setting radio_send does not touch, so it is moved here and moved
     * back afterwards rather than being made a per-packet argument. SetRfFrequency holds until
     * something changes it, and nothing between here and SetTx does.
     */
    const uint8_t chan = s_have_applied ? s_applied.channel : RADIO_DEFAULT_CHANNEL;
    if (req->foff != BENCH_FOFF_NONE && bus_lock()) {
        s_freq_offset_hz = (int32_t)req->foff * 1000;
        set_standby();
        set_frequency(chan);
        bus_unlock();
    }
    const int64_t t0 = esp_timer_get_time();
    const bool ok = radio_send(payload, len);
    if (air_us) *air_us = (uint32_t)(esp_timer_get_time() - t0);
    s_once_preamble = -1;
    s_once_sync = -1;
    s_once_hdr = -1;
    s_once_ldo = s_once_txmod = s_once_cal = s_once_pa = -1;
    if (req->pa >= 0 && bus_lock()) {
        /* Back to the configured level's own PA pair and drive. */
        set_standby();
        set_power(s_have_applied ? s_applied.power_level : RADIO_DEFAULT_POWER_LEVEL);
        set_ocp();
        enter_rx();
        bus_unlock();
        ESP_LOGI(TAG, "txpa seq=%lu pa=%d", (unsigned long)req->seq, req->pa);
    }
    if (req->cal >= 0) {
        ESP_LOGI(TAG, "txcal seq=%lu cal=%d rejected=%d errors=%04X", (unsigned long)req->seq,
                 req->cal, req->cal ? (int)s_seen_cal_rejected : 0, s_seen_errors);
    }
    /* The regulator has no readback, so its command status is the evidence; the modulation bit
     * is read back. Logged after the packet so nothing sits between the write and SetTx. */
    if (req->ldo >= 0 || req->txmod >= 0) {
        ESP_LOGI(TAG, "txcfg seq=%lu ldo=%d reg_cmd=%s txmod=%d 0889=%02X", (unsigned long)req->seq,
                 req->ldo, req->ldo >= 0 ? (s_seen_reg_ok ? "ok" : "FAILED") : "-", req->txmod,
                 req->txmod >= 0 ? s_seen_txmod : 0);
    }
    /* Back to DC-DC, which the next packet and receive assume. ldo=0 pays this too. */
    if (req->ldo >= 0 && bus_lock()) {
        uint8_t reg = 0x01;
        set_standby();
        cmd(CMD_SET_REGULATOR_MODE, &reg, 1);
        enter_rx();
        bus_unlock();
    }
    if (req->foff != BENCH_FOFF_NONE && bus_lock()) {
        s_freq_offset_hz = 0;
        set_standby();
        set_frequency(chan);
        enter_rx();
        bus_unlock();
    }
    /* Put the sync word back, since the next packet is not this one's. */
    if (req->sync >= 0) {
        uint8_t sw[2] = {PHY_SYNC_MSB, PHY_SYNC_LSB};
        if (bus_lock()) {
            set_standby();
            reg_write(REG_LORA_SYNC_MSB, sw, 2);
            enter_rx();
            bus_unlock();
        }
    }
    return ok;
}
#endif

#ifdef BENCH_CMD
bool radio_bench_rx_preamble(int symbols) {
    /* What a receiver is told to expect: SetPacketParams carries it for reception as well as for
     * transmission, and every test so far moved only the transmitted one. */
    if (symbols < 6 || symbols > 65535 || !bus_lock()) return false;
    s_rx_preamble = symbols;
    const bool ok = set_standby() && set_packet_params(255) && enter_rx();
    bus_unlock();
    return ok;
}

bool radio_bench_reg_read(uint16_t addr, uint8_t *out, size_t n) {
    if (n == 0 || n > 32 || !bus_lock()) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < n; i += 4)      /* reg_read takes at most four at a time */
        ok = reg_read((uint16_t)(addr + i), out + i, n - i < 4 ? n - i : 4);
    bus_unlock();
    return ok;
}

bool radio_bench_rx_implicit(int len) {
    if (len < 0 || len > 255 || !bus_lock()) return false;
    s_rx_implicit = len;
    const bool ok = set_standby() && enter_rx();
    bus_unlock();
    return ok;
}

bool radio_bench_tune(int32_t hz) {
    /* The same bound as foff, in Hz: past a quarter of the bandwidth nothing is expected to lock. */
    if (hz < -125000 || hz > 125000 || !bus_lock()) return false;
    s_freq_offset_hz = hz;
    const uint8_t chan = s_have_applied ? s_applied.channel : RADIO_DEFAULT_CHANNEL;
    const bool ok = set_standby() && set_frequency(chan) && enter_rx();
    bus_unlock();
    return ok;
}
#endif

size_t radio_identity(char *out, size_t out_len) {
    /* The chip has no firmware string to report. Naming the PHY is more use in a status line
     * than a blank, and it is space-free as the contract requires. */
    const char *id = "SX1262-SF" STR(PHY_SF) "BW" STR(PHY_BW_KHZ);
    size_t n = strlen(id);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, id, n);
    out[n] = '\0';
    return n;
}

#endif /* RADIO_BACKEND_SX1262 */
