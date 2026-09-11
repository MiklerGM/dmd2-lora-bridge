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
 * V4 puts a front-end module (GC1109, KCT8103L from V4.3.1) between the SX1262 and the antenna,
 * and it comes up disabled. This is the failure that would waste a day: SPI answers, begin()
 * succeeds, every candidate reports a clean zero, and the radio was simply switched off the
 * whole time. A silent probe and a deaf one look identical.
 *
 * FEM_EN is held high to power the front end; DIO2 then selects the path, low for the LNA in
 * receive. Revisions have moved this pin, so it is a build flag and the value in use is printed
 * at boot - check it against the schematic of the board in hand rather than trusting this.
 * Build with -DFEM_EN_PIN=-1 for a V3, which has no FEM at all.
 */
#ifndef FEM_EN_PIN
#define FEM_EN_PIN 2
#endif

/* Channel 18 of the DMD profile. The one number we are certain of. */
static const float FREQ_MHZ = 868.125f;

/* Long enough to catch a position broadcast, short enough to walk the grid while watching. */
#ifndef DWELL_MS
#define DWELL_MS 20000
#endif

/*
 * Ordered by the reasoning in PHY-notes.md: SF9/BW125 is the leading guess because it matches
 * both the module's -129 dBm typical sensitivity and the widely reported EBYTE default; 0x12 is
 * the usual LoRa private sync word. Coding rate is deliberately not a dimension - in explicit
 * header mode the receiver recovers it from the packet, so a mismatch blocks nothing.
 */
struct candidate {
    float bw;
    uint8_t sf;
    uint8_t sync;
    const char *name;
};
static const candidate GRID[] = {
    {125.0f, 9, 0x12, "SF9  BW125 sync0x12"},
    {250.0f, 10, 0x12, "SF10 BW250 sync0x12"},
    {125.0f, 9, 0x34, "SF9  BW125 sync0x34"},
    {250.0f, 10, 0x34, "SF10 BW250 sync0x34"},
};
static const size_t GRID_N = sizeof(GRID) / sizeof(GRID[0]);

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

static void listen(const candidate &c) {
    Serial.printf("\n== %s on %.3f MHz, %d s ==\n", c.name, FREQ_MHZ, DWELL_MS / 1000);

    int st = radio.begin(FREQ_MHZ, c.bw, c.sf, 5 /* CR 4/5, recovered from the header anyway */,
                         c.sync, 10 /* dBm, receive only */, 8 /* preamble */);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("   begin failed: %d - wrong pins or no radio\n", st);
        return;
    }
    /* DIO2 drives the front end's path selection: low selects the LNA for receive. Harmless on
     * a board with no FEM, required on one that has it. */
    radio.setDio2AsRfSwitch(true);
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
                /* Header decoded, so preamble and sync word matched: this candidate is on the
                 * right PHY and only the payload or the CRC setting differs. The most
                 * informative result short of success. */
                crc_err++;
                Serial.printf("   CRC MISMATCH, %u bytes, RSSI %.1f dBm - PHY looks RIGHT\n",
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
                Serial.printf("   *** DMD PAYLOAD *** v%u ttl%u %s, base64 run of %u",
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

#if FEM_EN_PIN >= 0
    pinMode(FEM_EN_PIN, OUTPUT);
    digitalWrite(FEM_EN_PIN, HIGH);
    Serial.printf("FEM enable on GPIO %d, held high\n", FEM_EN_PIN);
#else
    Serial.println("no FEM configured - correct for V3, wrong for V4");
#endif
}

void loop() {
    for (size_t i = 0; i < GRID_N; i++) listen(GRID[i]);
}
