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

/* Channel 18 of the DMD profile. The one number we are certain of. */
static const float FREQ_MHZ = 868.125f;

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

static void dump(const uint8_t *buf, size_t n) {
    Serial.print("    hex   ");
    for (size_t i = 0; i < n; i++) Serial.printf("%02X ", buf[i]);
    Serial.print("\n    ascii ");
    for (size_t i = 0; i < n; i++) Serial.printf("%c", (buf[i] >= 0x20 && buf[i] < 0x7F) ? buf[i] : '.');
    Serial.println();
}

static void listen(float freq, float bw, uint8_t sf, uint8_t sync, bool invert_iq) {
    Serial.printf("\n== SF%-2u BW%-3.0f sync0x%02X IQ%s on %.3f MHz, %d s ==\n",
                  sf, bw, sync, invert_iq ? "inv" : "nor", freq, DWELL_MS / 1000);

    int st = radio.begin(freq, bw, sf, 5 /* CR 4/5, recovered from the header anyway */,
                         sync, 10 /* dBm, receive only */, 8 /* preamble */);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   begin failed: %d - wrong pins or no radio\n", st);
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
    radio.startReceive();

    uint32_t end = millis() + DWELL_MS;
    uint32_t heard = 0, crc_err = 0;
    while ((int32_t)(end - millis()) > 0) {
        if (!s_irq) { delay(5); continue; }
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
                Serial.printf("   CRC mismatch, %u bytes, RSSI %.1f dBm - valid LoRa header; repeat/capture\n",
                              (unsigned)n, radio.getRSSI());
                dump(buf, n);
            }
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
        radio.startReceive();
    }

    /* getRSSI() with no argument is the RSSI of the last packet - stale or meaningless on a
     * quiet candidate. The instantaneous reading is the one that describes the channel. */
    Serial.printf("   done: %u packets, %u CRC mismatches, noise floor %.1f dBm\n",
                  (unsigned)heard, (unsigned)crc_err, radio.getRSSI(false));
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

#if CONTROL_SWEEP
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
