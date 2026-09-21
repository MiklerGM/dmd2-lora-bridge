/* Command-driven function substitution on one board. E220 UART is the oracle. */
#include <Arduino.h>
#include <RadioLib.h>
#include <esp_log.h>

extern "C" {
#include "radio.h"
#include "radio_bench.h"
}

static SPIClass spi(HSPI);
static SX1262 reference = new Module(8, 14, 12, 13, spi);
static unsigned mode = 4;

extern "C" bool sx_bench_spi(const uint8_t *tx, uint8_t *rx, size_t len) {
    spi.beginTransaction(RADIOLIB_DEFAULT_SPI_SETTINGS);
    digitalWrite(8, LOW);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = spi.transfer(tx[i]);
        if (rx) rx[i] = b;
    }
    digitalWrite(8, HIGH);
    spi.endTransaction();
    return true;
}

static int serial_log(const char *fmt, va_list args) {
    char text[384];
    int n = vsnprintf(text, sizeof(text), fmt, args);
    Serial.print(text);
    return n;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    esp_log_set_vprintf(serial_log);
    spi.begin(9, 11, 10, 8);
    pinMode(8, OUTPUT);
    digitalWrite(8, HIGH);
    radio_init();
    Serial.println("bisect ready: MODE 0=RL/invalid-counter 1=RL-init/our-TX 2=our-init/RL-TX 3=ours/invalid-counter 4=RL-xor 5=ours-xor 6=RL-xor/pre20 7=ours-xor/pre20 8=RL-xor/long-interleaver; Arduino SPI throughout");
}

void loop() {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (line.length() < 160) line += c;
            continue;
        }
        if (line.startsWith("MODE ")) {
            if (line.length() == 6 && line[5] >= '0' && line[5] <= '8') {
                mode = line[5] - '0';
                Serial.printf("MODEOK %u\n", mode);
            } else Serial.println("TXERR invalid mode");
            line = "";
            continue;
        }
        bench_tx_t req;
        bench_result_t parsed = bench_parse_tx(line.c_str(), &req);
        line = "";
        if (parsed != BENCH_OK || req.power >= 0 || req.preamble >= 0 || req.sync >= 0 || req.hdr >= 0) {
            Serial.println("TXERR this comparison accepts only seq and len");
            continue;
        }
        size_t len = req.len < 0 ? 38 : req.len;
        char payload[201];
        snprintf(payload, sizeof(payload), "%08lu", (unsigned long)req.seq);
        memset(payload + 8, 'A', len - 8);
        payload[len] = 0;
        // Modes 0/3 deliberately retain the disproved counter hypothesis as a negative control.
        uint8_t frame[208] = {0x2C, 18, (uint8_t)(0xA0 | (req.seq & 15)),
                             (uint8_t)((req.seq & 15) ^ 1), 0, 0, 0, (uint8_t)len};
        for (size_t i = 0; i < len; ++i) frame[8 + i] = payload[i] ^ 0x12;
        if (mode >= 4 || mode == 1 || mode == 2) {
            uint8_t check = 0;
            for (size_t i = 0; i < len; ++i) check ^= payload[i];
            frame[2] = 0xA0 ^ check;
            frame[3] = 0x01 ^ check;
        }
        req.hdr = (frame[2] << 8) | frame[3];
        uint32_t started = millis();

        // Reset and initialize equally on every attempt, including the untouched reference.
        digitalWrite(5, LOW);
        const uint16_t preamble = (mode == 6 || mode == 7) ? 20 : 8;
        bool ok = reference.begin(868.125f, 500.0f, 11, 5, 0x55, 2, preamble) == RADIOLIB_ERR_NONE;
        ok = ok && reference.explicitHeader() == RADIOLIB_ERR_NONE;
        ok = ok && reference.setCRC(2) == RADIOLIB_ERR_NONE;
        ok = ok && reference.forceLDRO(true) == RADIOLIB_ERR_NONE;
        if (ok && mode == 8) ok = reference.setCodingRate(5, true) == RADIOLIB_ERR_NONE;
        if (ok && (mode == 2 || mode == 3 || mode == 5 || mode == 7)) {
            radio_request_t cfg = {18, 3, false};
            ok = radio_apply(&cfg) == RADIO_OK;
        }
        // Our TX polls latched IRQ flags; RadioLib normally enables them in startTransmit.
        if (ok && mode == 1) ok = reference.setIrqFlags(0xFFFF) == RADIOLIB_ERR_NONE;
        if (ok && (mode & 1)) {
            req.preamble = preamble;
            uint32_t air;
            ok = radio_bench_tx(&req, (const uint8_t *)payload, len, &air);
        } else if (ok) {
            digitalWrite(5, HIGH);
            delay(2);
            ok = reference.transmit(frame, len + 8) == RADIOLIB_ERR_NONE;
            digitalWrite(5, LOW);
        }
        delay(2);
        Serial.printf("%s seq=%lu mode=%u elapsed=%lu\n", ok ? "TXOK" : "TXFAIL", (unsigned long)req.seq, mode,
                      (unsigned long)(millis() - started));
        Serial.flush();
    }
    delay(1);
}
