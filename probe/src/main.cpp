/*
 * PHY probe for the DMD air interface. NOT the bridge, and not a radio backend for it.
 *
 * EBYTE hides the LoRa PHY behind an "air data rate" index, so the SF/BW/sync an E220 actually
 * uses is not derivable from public documentation - see docs/PHY-notes.md. This walks the short
 * list of plausible combinations, listens on each, and reports what it hears.
 *
 * It cannot be validated without a transmitter: point it at a configured E220 (or a NOR7E) and
 * the answer arrives. With nothing on the air it can still prove the board, the radio and the
 * pin map, and show the noise floor on the channel.
 */
#include <Arduino.h>
#include <RadioLib.h>

#include "dmd_sniff.h"

/*
 * Heltec WiFi LoRa 32: the SX1262 is wired on the board, not by hand. V3 and V4 share this
 * map, but Heltec has moved pins between revisions before, so check the schematic of the board
 * actually in hand before trusting a silent receiver.
 */
#ifndef LORA_NSS
#define LORA_NSS 8
#define LORA_SCK 9
#define LORA_MOSI 10
#define LORA_MISO 11
#define LORA_RST 12
#define LORA_BUSY 13
#define LORA_DIO1 14
#endif

/*
 * V4 puts a front-end module between the SX1262 and the antenna, and it comes up dead. This is
 * the failure that would waste a day: SPI answers, begin() succeeds, every candidate reports a
 * clean zero, and the radio was switched off the whole time. A silent probe and a deaf one look
 * identical.
 *
 * Pins read off the HTIT-WB32LAF V4.3 schematic, where the module pin table gives
 * PA_CSD -> GPIO2, PA_CTX -> GPIO5, VFEM_Ctrl -> GPIO7. That reading is confirmed by
 * LoRa_NSS -> GPIO8 in the same table, which is known independently. The part is a KCT8103L.
 *
 * For receive: power the front end, release its shutdown, and hold the path in receive. CTX
 * high would select the transmit path, which is not what a receive-only probe wants pointed at
 * its LNA. PA_CPS only selects transmit power and is left alone.
 *
 * Heltec has moved these between revisions, so they are build flags and the values in use are
 * printed at boot. -DFEM_VCTRL_PIN=-1 disables the whole block, which is correct for a V3.
 */
#ifndef FEM_VCTRL_PIN
#define FEM_VCTRL_PIN 7   /* VFEM_Ctrl - supplies the FEM */
#endif
#ifndef FEM_CSD_PIN
#define FEM_CSD_PIN 2     /* PA_CSD - chip shutdown, high to enable */
#endif
#ifndef FEM_CTX_PIN
#define FEM_CTX_PIN 5     /* PA_CTX - high selects TX, low selects the LNA */
#endif

/* Channel 18 of the DMD profile: 850.125 + channel. Overridable so the probe can follow a
 * beacon that has been moved, which is how the obfuscation key gets tested against the channel
 * number it is suspected of being derived from. */
#ifndef DMD_CHANNEL
#define DMD_CHANNEL 18
#endif
static const float FREQ_MHZ = 850.125f + (float)DMD_CHANNEL;

/* Long enough to catch a position broadcast, short enough to walk the grid while watching. */
#ifndef DWELL_MS
#define DWELL_MS 20000
#endif

/*
 * Exhaustive rather than a shortlist, because the shortlist was tried and failed.
 *
 * PHY-notes.md inferred SF9/BW125 or SF10/BW250 from the -129 dBm typical sensitivity. Both,
 * with both sync words, were swept against ~117 real E220 transmissions on 2026-09-16 while a
 * second E220 sitting beside this board decoded every one of them. Zero packets, and - the
 * informative part - zero header errors, so the receiver never even reached preamble/sync lock.
 * That rules out the header mode as the culprit and leaves SF, BW, sync and IQ polarity.
 *
 * So the grid is now every combination the transmitter can physically produce. LLCC68 is not a
 * full SX1262: its maximum spreading factor depends on the bandwidth, which is what bounds this
 * to 18 SF/BW pairs rather than the whole matrix.
 *
 * Coding rate is still not a dimension - in explicit header mode the receiver recovers it from
 * the packet. IQ polarity is, because an inversion mismatch produces exactly the silence above.
 */
struct phy_span {
    float bw;
    uint8_t sf_min, sf_max;
};
static const phy_span SPANS[] = {
    {125.0f, 5, 9},
    {250.0f, 5, 10},
    {500.0f, 5, 11},
};
static const size_t SPANS_N = sizeof(SPANS) / sizeof(SPANS[0]);

/* 0x12 is the LoRa private sync word and 0x34 the public one. A vendor-private third value
 * would not be found by this sweep; that is the first thing to widen if 72 comes back empty. */
static const uint8_t SYNCS[] = {0x12, 0x34};

static SPIClass spi(HSPI);
static SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, spi);

static volatile bool s_irq = false;
static void IRAM_ATTR on_irq() { s_irq = true; }

/*
 * Everything the chip can latch, not just what drives DIO1.
 *
 * The probe acts only on the DIO1 interrupt, so a demodulator that is working perfectly while
 * the interrupt path is not - wrong pin, an IRQ the mask never enabled - is indistinguishable
 * from a PHY that does not match. RSSI cannot tell those apart either: it is measured before
 * and independently of demodulation, which is why a healthy-looking signal level proved the
 * analogue chain and nothing above it.
 *
 * PREAMBLE_DETECTED is the one that settles it. It fires when the correlator locks onto a
 * chirp train, before any header or CRC, so it answers "does this demodulator see anything at
 * all" without needing a packet to survive all the way to readData.
 *
 * The chip latches only what the IRQ mask enables, so the mask has to be widened or the flag is
 * never set to be read. startReceive's third argument keeps DIO1 driving RX_DONE alone - pulling
 * the pin on every preamble would fire the packet callback with nothing to read and turn the
 * dwell loop into a spin.
 *
 * Two flag vocabularies meet here, which is easy to get wrong: startReceive takes RadioLib's
 * portable bit *positions*, while getIrqFlags returns the SX126x IrqStatus register raw.
 */
#define IRQ_LATCH_EVERYTHING ( \
    (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED) | (1UL << RADIOLIB_IRQ_SYNC_WORD_VALID) | \
    (1UL << RADIOLIB_IRQ_HEADER_VALID) | (1UL << RADIOLIB_IRQ_HEADER_ERR) | \
    (1UL << RADIOLIB_IRQ_RX_DONE) | (1UL << RADIOLIB_IRQ_CRC_ERR) | \
    (1UL << RADIOLIB_IRQ_TIMEOUT))

static void arm_receive() {
    radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, IRQ_LATCH_EVERYTHING,
                       RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
}

static void print_irq(uint16_t f) {
    if (!f) { Serial.print("none"); return; }
    if (f & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) Serial.print("PREAMBLE ");
    if (f & RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID)   Serial.print("SYNC ");
    if (f & RADIOLIB_SX126X_IRQ_HEADER_VALID)      Serial.print("HDR_OK ");
    if (f & RADIOLIB_SX126X_IRQ_HEADER_ERR)        Serial.print("HDR_ERR ");
    if (f & RADIOLIB_SX126X_IRQ_RX_DONE)           Serial.print("RX_DONE ");
    if (f & RADIOLIB_SX126X_IRQ_CRC_ERR)           Serial.print("CRC_ERR ");
    if (f & RADIOLIB_SX126X_IRQ_TIMEOUT)           Serial.print("TIMEOUT ");
    Serial.printf("(0x%04X)", f);
}

static void dump(const uint8_t *buf, size_t n) {
    Serial.print("    hex   ");
    for (size_t i = 0; i < n; i++) Serial.printf("%02X ", buf[i]);
    Serial.print("\n    ascii ");
    for (size_t i = 0; i < n; i++) Serial.printf("%c", (buf[i] >= 0x20 && buf[i] < 0x7F) ? buf[i] : '.');
    Serial.println();
}

/*
 * implicit_len 0 means explicit header. Anything else switches to implicit and fixes the
 * payload length, which the receiver must then know in advance - 38 is what the beacon puts on
 * the air. If the module wraps the payload in framing of its own the real length is larger,
 * but reading the first 38 bytes still lets dmd_sniff find the marker at whatever offset it
 * lands, so a wrong guess here degrades rather than blinds.
 */
static void listen(float freq, float bw, uint8_t sf, uint8_t sync, bool invert_iq,
                   uint8_t implicit_len = 0, bool crc_on = true, uint8_t cr = 5,
                   bool force_ldro = false) {
    Serial.printf("\n== SF%-2u BW%-3.0f sync0x%02X IQ%s hdr%s crc%s on %.3f MHz, %d s ==\n",
                  sf, bw, sync, invert_iq ? "inv" : "nor",
                  implicit_len ? "imp" : "exp", crc_on ? "on" : "off", freq, DWELL_MS / 1000);

    /* Recovered from the header in explicit mode, so it only matters when implicit_len is set
     * - and then it matters absolutely. */
    int st = radio.begin(freq, bw, sf, cr, sync, 10 /* dBm, receive only */, 8 /* preamble */);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   begin failed: %d - wrong pins or no radio\n", st);
        return;
    }
    /* Both reset by begin(), so both go after it. A header-mode mismatch is exactly what
     * produces a stream of header errors on an otherwise correct PHY: the receiver decodes a
     * header out of payload bytes. CRC has to be swept for the same reason - expecting one the
     * transmitter never sent fails the packet after the header decodes cleanly. */
    st = implicit_len ? radio.implicitHeader(implicit_len) : radio.explicitHeader();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   header mode failed: %d\n", st);
        return;
    }
    st = radio.setCRC(crc_on ? 2 : 0);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   setCRC failed: %d\n", st);
        return;
    }
    /* Also reset by begin(). autoLDRO() follows Semtech's 16.38 ms symbol rule; forcing it on
     * covers implementations that use the "SF11 or SF12" shortcut instead. */
    st = force_ldro ? radio.forceLDRO(true) : radio.autoLDRO();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   LDRO failed: %d\n", st);
        return;
    }
    /* After begin(), which resets the modem config. A mismatch here never reaches header
     * decode - the chirps simply do not correlate - so it must be swept, not assumed. */
    st = radio.invertIQ(invert_iq);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   invertIQ failed: %d\n", st);
        return;
    }

    radio.setPacketReceivedAction(on_irq);
    s_irq = false;
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    arm_receive();

    uint32_t end = millis() + DWELL_MS;
    uint32_t heard = 0, crc_err = 0;
    uint16_t seen = 0;          /* every flag the chip latched during this dwell */
    uint32_t preambles = 0, hdr_errs = 0;
    bool dio1_fired = false;    /* whether the interrupt path ever delivered one */
    while ((int32_t)(end - millis()) > 0) {
        /* Polled, not waited on. This is the whole point: a latched flag with no interrupt
         * means the demodulator worked and the path to this code did not. */
        uint16_t now = radio.getIrqFlags();
        seen |= now;

        /*
         * Counted, not OR-ed, and this is what makes the sweep a measurement.
         *
         * The SX126x preamble detector is promiscuous: it triggers on chirp-like energy rather
         * than on a matching slope, so a single transmitter sets it at almost every SF. One
         * latched bit therefore says only "something is on this channel". A count separates
         * them - against a beacon of known rate, the right SF/BW should report a detection per
         * transmission while a wrong one reports the occasional false trigger.
         *
         * Clearing only these bits leaves the rest latched, and clearing IrqStatus does not
         * disturb the receiver state machine.
         */
        uint16_t consume = now & (RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED |
                                  RADIOLIB_SX126X_IRQ_HEADER_ERR);
        if (consume) {
            if (now & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) preambles++;
            if (now & RADIOLIB_SX126X_IRQ_HEADER_ERR) hdr_errs++;
            radio.clearIrqFlags(consume);
        }
        if (!s_irq) { delay(5); continue; }
        dio1_fired = true;
        s_irq = false;

        uint8_t buf[256];
        size_t n = radio.getPacketLength();
        if (n > sizeof(buf)) n = sizeof(buf);

        /*
         * Read the latched flags before readData, which clears them. RadioLib collapses two
         * different events into RADIOLIB_ERR_CRC_MISMATCH - a payload CRC error, and a header
         * error with no valid header - and they mean opposite things here: the first says the
         * PHY is right, the second says it is not. The raw flags separate them.
         */
        uint32_t irq = radio.getIrqFlags();
        int rs = radio.readData(buf, n);

        if (rs == RADIOLIB_ERR_CRC_MISMATCH) {
            if (irq & RADIOLIB_SX126X_IRQ_HEADER_ERR && !(irq & RADIOLIB_SX126X_IRQ_HEADER_VALID)) {
                Serial.printf("   header error, RSSI %.1f dBm - energy here, but not this PHY\n",
                              radio.getRSSI());
            } else {
                /* HEADER_VALID makes this more informative than silence, but a single bad-CRC
                 * observation is still only a clue. Repeat it and capture a valid payload before
                 * treating the candidate as confirmed. */
                crc_err++;
                Serial.printf("   CRC mismatch, %u bytes, RSSI %.1f dBm - payload failed CRC; in implicit\n                 mode there is no header, so this is not evidence of one\n",
                              (unsigned)n, radio.getRSSI());
                dump(buf, n);
            }
        } else if (rs == RADIOLIB_ERR_NONE && n == 0) {
            /* A zero-length read is not a packet: there is nothing to check a CRC against and
             * nothing to recognise, so counting it as one manufactures hits. A 1024-candidate
             * sweep produced 23 of these and every one looked like a validated reception. */
            Serial.println("   empty RX_DONE, 0 bytes - not a packet");
        } else if (rs == RADIOLIB_ERR_NONE) {
            heard++;
            Serial.printf("   packet, %u bytes, RSSI %.1f dBm, SNR %.1f dB\n",
                          (unsigned)n, radio.getRSSI(), radio.getSNR());
            dump(buf, n);
            dmd_sniff_t s = dmd_sniff(buf, n);
            if (s.found) {
                Serial.printf("   *** DMD HEADER CANDIDATE *** v%u ttl%u %s, base64 run of %u",
                              s.version, s.ttl, s.type ? "chat" : "group", (unsigned)s.run_len);
                if (s.offset != s.run_start)
                    Serial.printf(", payload %u bytes into the run - the module adds framing",
                                  (unsigned)(s.offset - s.run_start));
                Serial.println();
            }
        } else {
            Serial.printf("   read error %d\n", rs);
        }
        arm_receive();
    }

    /* getRSSI() with no argument is the RSSI of the last packet - stale or meaningless on a
     * quiet candidate. The instantaneous reading is the one that describes the channel. */
    Serial.printf("   done: %u packets, %u CRC mismatches, noise floor %.1f dBm\n",
                  (unsigned)heard, (unsigned)crc_err, radio.getRSSI(false));

    /* The line this build exists for. "irq none" on every candidate means the correlator never
     * locked and the sweep is honest; anything latched here with dio1=no means the demodulator
     * was working and the interrupt path was swallowing it. */
    Serial.printf("   preambles %-4lu header-errors %-4lu   irq ",
                  (unsigned long)preambles, (unsigned long)hdr_errs);
    print_irq(seen);
    Serial.printf(", dio1=%s\n", dio1_fired ? "yes" : "no");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\nDMD PHY probe - receive only, transmits nothing");
    Serial.printf("pins: NSS %d SCK %d MOSI %d MISO %d RST %d BUSY %d DIO1 %d\n",
                  LORA_NSS, LORA_SCK, LORA_MOSI, LORA_MISO, LORA_RST, LORA_BUSY, LORA_DIO1);
    spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

#if FEM_VCTRL_PIN >= 0
    pinMode(FEM_VCTRL_PIN, OUTPUT);
    digitalWrite(FEM_VCTRL_PIN, HIGH);   /* power it */
    pinMode(FEM_CSD_PIN, OUTPUT);
    digitalWrite(FEM_CSD_PIN, HIGH);     /* release shutdown */
    pinMode(FEM_CTX_PIN, OUTPUT);
    digitalWrite(FEM_CTX_PIN, LOW);      /* receive path through the LNA */
    delay(10);
    Serial.printf("FEM on: VFEM_Ctrl GPIO%d high, PA_CSD GPIO%d high, PA_CTX GPIO%d low (RX)\n",
                  FEM_VCTRL_PIN, FEM_CSD_PIN, FEM_CTX_PIN);
#else
    Serial.println("no FEM configured - correct for V3, deaf on V4");
#endif
}

#if TRANSMIT
/*
 * The other direction, and the only oracle in this project with no false positives.
 *
 * Everything so far has been us interpreting bytes we received. This inverts it: build the frame
 * the E220 is believed to expect, put it on the air, and let a real E220 in transparent mode
 * judge. It either prints the marker on its UART or it does not. There is nothing to interpret.
 *
 * Frame, measured in docs/PHY-notes.md:
 *     2C  CH  A0  01  00  00  00  LEN  |  payload XOR 0x12
 *
 * The FEM is the trap here. The probe pins PA_CTX low for receive, and leaving it there
 * transmits into the LNA - the same "deaf probe" failure, in the transmit direction, with every
 * attempt coming back silent. It has to be driven high for TX and back down afterwards.
 *
 * Power stays low deliberately: the receiving E220 is on the same bench, and an overloaded
 * receiver fails exactly like a wrong frame.
 */
#ifndef TX_MARKER
#define TX_MARKER "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#endif
#ifndef TX_DBM
#define TX_DBM 2
#endif
#define EBYTE_XOR 0x12

static void fem_transmit(bool on) {
#if FEM_VCTRL_PIN >= 0
    digitalWrite(FEM_CTX_PIN, on ? HIGH : LOW);
    delay(2);
#else
    (void)on;
#endif
}

void loop() {
    static bool ready = false;
    if (!ready) {
        int st = radio.begin(FREQ_MHZ, 500.0f, 11, 5, 0x55, TX_DBM, 8);
        if (st != RADIOLIB_ERR_NONE) { Serial.printf("begin failed: %d\n", st); delay(2000); return; }
        radio.explicitHeader();
        radio.setCRC(2);
        radio.forceLDRO(true);
        Serial.printf("\nTX on %.3f MHz SF11 BW500 sync0x55 LDRO on, %d dBm\n", FREQ_MHZ, TX_DBM);
        Serial.printf("marker: \"%s\" (%u bytes)\n", TX_MARKER, (unsigned)(sizeof(TX_MARKER) - 1));
        ready = true;
    }

    const size_t len = sizeof(TX_MARKER) - 1;
    uint8_t frame[8 + len];
    frame[0] = 0x2C;
    frame[1] = (uint8_t)DMD_CHANNEL;
    frame[2] = 0xA0;
    frame[3] = 0x01;
    frame[4] = frame[5] = frame[6] = 0x00;
    frame[7] = (uint8_t)len;
#ifdef POWER_SWEEP
    /* The same four steps the firmware backend sweeps, spelled into the payload the same way, so
     * the two transmitters can be compared point by point instead of plateau to single point. */
    static const int8_t sweep_dbm[4] = {10, 4, -2, -9};
    static uint8_t lvl = 0;
    lvl = (uint8_t)((lvl + 1) & 3);
    radio.setOutputPower(sweep_dbm[lvl]);
    for (size_t i = 0; i < len; i++) frame[8 + i] = (uint8_t)('A' + lvl) ^ EBYTE_XOR;
#else
    for (size_t i = 0; i < len; i++) frame[8 + i] = (uint8_t)TX_MARKER[i] ^ EBYTE_XOR;
#endif

    fem_transmit(true);
    int st = radio.transmit(frame, sizeof(frame));
    fem_transmit(false);

    Serial.printf("tx %u bytes -> %s\n", (unsigned)sizeof(frame),
                  st == RADIOLIB_ERR_NONE ? "ok" : "FAILED");
    delay(2000);
}
#elif LISTEN_SOLVED
/*
 * Park on the answer and dump everything that arrives. Not a sweep - the PHY is settled, and
 * what is being read now is the layer above it: 38 bytes go into the module and 46 come out of
 * the air, with the CRC passing, so the difference is EBYTE's own framing or its CRYPT
 * transform. Vary the beacon payload between runs and compare the hex.
 */
void loop() {
    listen(FREQ_MHZ, 500.0f, 11, 0x55, false, 0, true, 5, true);
}
#elif NIBBLE5
/*
 * Confirming a pattern, not searching. In the 256-value sweep at SF11/BW500, fourteen windows
 * produced header errors and twelve of them were sync words ending in nibble 5 - across every
 * high nibble. RadioLib puts the low nibble of the argument into the SECOND sync symbol and the
 * high nibble into the first, so the reading is that the second symbol must be 5 while the
 * first is not being discriminated.
 *
 * Counts were 1-3 per window, which is noise on its own; twelve out of fourteen sharing a
 * nibble is not. So: the sixteen values with that nibble, at three times the dwell, looking for
 * a header error rate that stands out or - the real prize - HEADER_VALID.
 */
void loop() {
    for (unsigned hi = 0; hi < 16; hi++) {
        listen(FREQ_MHZ, 500.0f, 11, (uint8_t)((hi << 4) | 0x05), false, 0, true, 5, true);
    }
    Serial.println("\n=== nibble-5 pass complete ===");
}
#elif SYNC_SF11
/*
 * Sync word at the SF/BW that finally matches.
 *
 * SF11/BW500 reports 59-79 preamble detections per 20 s against a 1 Hz beacon - three or four
 * per packet, which is what a real slope match looks like - and ZERO header errors. That pair
 * is the sync-mismatch signature exactly: the correlator locks on the preamble, the sync
 * symbols fail, and the packet engine returns to preamble search without ever attempting a
 * header. The earlier 256-value sweep was run at SF7/BW125, which is the slope alias, so it
 * tested nothing.
 *
 * LDRO is not swept here. The LoRa header block is always sent at reduced rate regardless, so
 * once sync is right the header decodes either way - HEADER_VALID or a header error is the
 * signal to watch for, and LDRO only matters afterwards, for the payload.
 */
void loop() {
    for (unsigned sync = 0; sync < 256; sync++) {
        listen(FREQ_MHZ, 500.0f, 11, (uint8_t)sync, false, 0, true, 5, false);
    }
    Serial.println("\n=== sync sweep at SF11/BW500 complete ===");
}
#elif CONFIRM_SF11
/*
 * The candidate the sweep metric was actively hiding.
 *
 * Chirp rate is BW^2/2^SF, and SF7/BW125, SF9/BW250 and SF11/BW500 all sit at 122.1 kHz/ms. An
 * SF11/BW500 chirp crossing a BW125 passband has the same slope as a real SF7/BW125 chirp, so
 * the correlator locks, re-arms and locks again about every ten symbols across a 226 ms burst -
 * roughly twenty false detections per packet. That is where "432 preamble detections at
 * SF7/BW125" came from. The true match detects its preamble ONCE per packet, so it scored ~20
 * and was filed under noise.
 *
 * SF11/BW500 at CR 4/5 is 2148 bps, which is what EBYTE labels "2.4k" - and the FCC test report
 * for this family measures a 20 dB bandwidth of 548 kHz at the default configuration, where
 * BW125 would read about 130 kHz.
 *
 * LDRO is swept because the two conventions disagree exactly here: Semtech enables it above a
 * 16.38 ms symbol (SF11/BW500 is 4.096 ms, so off), while the common shortcut is "on for SF11
 * and SF12". RadioLib follows Semtech, so all 1024 earlier candidates ran it off. A mismatch
 * drops the two LSBs of every payload symbol while leaving preamble, sync and header intact -
 * which is precisely "locks, decodes a header, then consistent garbage".
 */
void loop() {
    for (uint8_t sync : {0x12, 0x34}) {
        for (int ldro = 0; ldro < 2; ldro++) {
            Serial.printf("\n--- SF11/BW500 CR4/5 explicit CRC-on sync0x%02X LDRO %s ---",
                          sync, ldro ? "forced on" : "auto (off)");
            listen(FREQ_MHZ, 500.0f, 11, sync, false, 0, true, 5, ldro != 0);
        }
    }
    Serial.println("\n=== SF11/BW500 confirmation pass complete ===");
}
#elif CR_SWEEP
/*
 * The one gap the 1024-candidate sweep left open.
 *
 * In explicit header mode the receiver reads the coding rate out of the header, so it never
 * needed sweeping. In implicit mode there is no header and the CR must be set correctly or
 * nothing decodes - and every implicit run so far used 4/5. The implicit half of that sweep was
 * therefore testing one CR out of four, and its CRC-off rows could not tell a CR mismatch from
 * anything else because nothing was validating the result.
 *
 * CRC stays on here for exactly that reason: it is what separates a decode from 38 bytes of
 * noise. Sync is only two values, because implicit-mode decodes appeared at some sixty
 * different sync words, which says the SX126x enforces it loosely rather than strictly.
 */
void loop() {
    for (uint8_t cr = 5; cr <= 8; cr++) {
        for (uint8_t sync : {0x12, 0x34}) {
            Serial.printf("\n--- coding rate 4/%u ---", cr);
            listen(FREQ_MHZ, 125.0f, 7, sync, false, 38, true, cr);
        }
    }
    Serial.println("\n=== CR sweep complete: 4 coding rates x 2 sync words ===");
}
#elif NARROW_SF7
/*
 * SF7/BW125 with normal IQ, measured rather than guessed: against a 1 Hz beacon it reported 432
 * and 420 preamble detections at the two sync words, where the next candidate managed 123 and
 * everything else 32 or fewer - and it was the only configuration to reach header decode at all.
 * Not a level artefact either: SF9/BW250 saw the transmitter 20-35 dB stronger and still counted
 * a third as many.
 *
 * Preamble detection happens before the sync word is checked, which is why both sync words
 * scored alike and why sync is still unknown. So it is swept here in full, together with the
 * two things that can fail *after* a correct PHY locks: header mode and CRC.
 *
 * 0x12 arrives 18 candidates in, so the conventional answer shows up in the first few minutes.
 */
void loop() {
    for (unsigned sync = 0; sync < 256; sync++) {
        for (int mode = 0; mode < 4; mode++) {
            listen(FREQ_MHZ, 125.0f, 7, (uint8_t)sync, false,
                   (mode & 1) ? 38 : 0, (mode & 2) != 0);
        }
    }
    Serial.println("\n=== narrow sweep complete: 256 sync words x 4 header/CRC modes ===");
}
#elif CONTROL_SWEEP
/*
 * Positive control, and the reason it exists: this probe has never decoded a LoRa packet from
 * anything. FEM on, begin() clean and a drifting noise floor prove the radio is powered and
 * listening - not that the demodulator reports what it hears. Until something is decoded, a bug
 * in the receive path is indistinguishable from 72 honest misses, and the E220 sweep cannot be
 * written down as a result.
 *
 * Ambient LoRaWAN is the cheapest known-good transmitter available: EU868 uplinks sit on
 * 868.1/868.3/868.5 MHz at BW125 with the public sync word and normal IQ. Anywhere with
 * habitation nearby there is usually something on them.
 *
 * Switch the E220 bridge OFF while this runs. Channel 18 is 868.125 MHz, which falls inside the
 * 868.1 BW125 passband, so leaving it on jams the very channel being listened to.
 */
void loop() {
    unsigned n = 0;

    /*
     * Meshtastic first, because it is the likelier hit: its nodes beacon position, telemetry
     * and nodeinfo on their own schedule, while a LoRaWAN gateway mostly listens and only the
     * end devices - which no map shows - actually transmit. Sync word 0x2B is the value
     * PHY-notes.md already records as the one that makes us NOT Meshtastic-compatible.
     *
     * LongFast is the near-universal default preset. The exact EU centre frequency is from
     * memory and worth checking against the Meshtastic region table before trusting a miss.
     */
    for (uint8_t sf = 7; sf <= 12; sf++) { listen(869.525f, 250.0f, sf, 0x2B, false); n++; }

    /* LoRaWAN EU868 uplinks: public sync word, normal IQ, BW125. */
    static const float LORAWAN[] = {868.1f, 868.3f, 868.5f};
    for (size_t c = 0; c < sizeof(LORAWAN) / sizeof(LORAWAN[0]); c++)
        for (uint8_t sf = 7; sf <= 12; sf++)
            { listen(LORAWAN[c], 125.0f, sf, 0x34, false); n++; }

    Serial.printf("\n=== control sweep complete: %u candidates ===\n", n);
}
#else
void loop() {
    unsigned n = 0;
    for (size_t p = 0; p < SPANS_N; p++)
        for (uint8_t sf = SPANS[p].sf_min; sf <= SPANS[p].sf_max; sf++)
            for (size_t s = 0; s < sizeof(SYNCS); s++)
                for (int iq = 0; iq < 2; iq++)
                    { listen(FREQ_MHZ, SPANS[p].bw, sf, SYNCS[s], iq != 0); n++; }
    Serial.printf("\n=== sweep complete: %u candidates ===\n", n);
}
#endif
