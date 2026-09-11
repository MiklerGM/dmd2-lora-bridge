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
        int rs = radio.readData(buf, n);

        if (rs == RADIOLIB_ERR_CRC_MISMATCH) {
            /* Nearly a success: preamble, sync word and header all decoded, so this candidate
             * is on the right PHY and only the payload is damaged - or the CRC setting differs.
             * Worth far more attention than silence. */
            crc_err++;
            Serial.printf("   CRC MISMATCH, %u bytes, RSSI %.1f dBm - PHY looks RIGHT\n",
                          (unsigned)n, radio.getRSSI());
            dump(buf, n);
        } else if (rs == RADIOLIB_ERR_NONE) {
            heard++;
            Serial.printf("   packet, %u bytes, RSSI %.1f dBm, SNR %.1f dB\n",
                          (unsigned)n, radio.getRSSI(), radio.getSNR());
            dump(buf, n);
            dmd_sniff_t s = dmd_sniff(buf, n);
            if (s.found) {
                Serial.printf("   *** DMD PAYLOAD *** base64 run of %u at offset %u",
                              (unsigned)s.run_len, (unsigned)s.run_start);
                if (s.offset != s.run_start)
                    Serial.printf(", \"0g\" %u bytes into it - the module adds framing",
                                  (unsigned)(s.offset - s.run_start));
                Serial.println();
            }
        } else {
            Serial.printf("   read error %d\n", rs);
        }
        radio.startReceive();
    }

    Serial.printf("   done: %u packets, %u CRC mismatches, noise floor %.1f dBm\n",
                  (unsigned)heard, (unsigned)crc_err, radio.getRSSI());
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\nDMD PHY probe - receive only, transmits nothing");
    Serial.printf("pins: NSS %d SCK %d MOSI %d MISO %d RST %d BUSY %d DIO1 %d\n",
                  LORA_NSS, LORA_SCK, LORA_MOSI, LORA_MISO, LORA_RST, LORA_BUSY, LORA_DIO1);
    spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
}

void loop() {
    for (size_t i = 0; i < GRID_N; i++) listen(GRID[i]);
}
