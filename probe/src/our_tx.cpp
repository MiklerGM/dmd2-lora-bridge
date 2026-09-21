/*
 * The bridge's own SX1262 driver, compiled into the probe's environment.
 *
 * Everything the two transmitters say to the chip now matches: the same modulation, sync,
 * packet parameters, errata writes, PA settings and frame, verified against RadioLib's own SPI
 * trace. The reference reaches an E220 from this board and the bridge does not, so what is left
 * is not what the driver says but where it runs - ESP-IDF at 160 MHz with a scheduler, several
 * tasks and a Bluetooth stack, against a bare Arduino sketch at 240 MHz.
 *
 * This splits that in half. Same driver, the reference's environment. If it works here, the
 * environment is the culprit; if it fails here, our code is, and neither answer is a guess.
 */
#include <Arduino.h>
#include <esp_log.h>
#include <stdarg.h>

extern "C" {
#include "radio.h"
}

/* Under Arduino the IDF log goes to UART0 while Serial is the native USB, so the driver's own
 * output - including its SPI trace - lands on a pin nothing is wired to. Send it to Serial. */
static int log_to_serial(const char *fmt, va_list args) {
    char buf[320];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    Serial.print(buf);
    return n;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    esp_log_set_vprintf(log_to_serial);
    Serial.println("\nbridge driver inside the probe's environment");
    radio_init();
    radio_request_t req = {};
    req.channel = 18;
    req.power_level = 3;
    req.lbt = false;
    radio_state_t st = radio_apply(&req);
    Serial.printf("apply -> %d (0 = ok)\n", (int)st);
    char id[48] = "";
    radio_identity(id, sizeof(id));
    Serial.printf("radio: %s, cpu %lu MHz\n", id, (unsigned long)(getCpuFrequencyMhz()));
}

void loop() {
    static uint32_t n = 0;
    char payload[39];
    snprintf(payload, sizeof(payload), "%08lu", (unsigned long)(++n));
    memset(payload + 8, 'A', 30);
    payload[38] = '\0';
    bool ok = radio_send((const uint8_t *)payload, 38);
    Serial.printf("tx %lu -> %s\n", (unsigned long)n, ok ? "ok" : "FAILED");
    delay(2000);
}
