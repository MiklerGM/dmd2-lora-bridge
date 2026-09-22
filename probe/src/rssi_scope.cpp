/* Passive T-Deck RSSI scope. The pilot, not the sample rate, decides whether chirps resolve. */
#include <Arduino.h>
#include <RadioLib.h>
#include <esp_timer.h>
#include <errno.h>

static SPIClass spi(HSPI);
static Module module(9, 45, 17, 13, spi);
static SX1262 radio(&module);
static constexpr uint32_t PERIOD_US = 100, DURATION_US = 1000000;
static constexpr size_t MAX_SAMPLES = DURATION_US / PERIOD_US;
static uint8_t rssi[MAX_SAMPLES];
static uint32_t time_us[MAX_SAMPLES];
static int16_t radio_state = RADIOLIB_ERR_UNKNOWN;
static int offset_khz;
static struct {
    bool valid;
    uint32_t seq;
    int offset;
    int16_t status;
    int64_t start;
    uint32_t elapsed;
    size_t n;
    uint32_t skipped, read_max;
    uint64_t read_sum;
} last_capture;

/* HWCDC writes may be short; its disconnected FIFO also discards older bytes. */
static bool serial_all(const char *data, size_t size) {
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (size) {
        if (esp_timer_get_time() >= deadline) return false;
        if (!Serial.isConnected()) { delay(1); continue; }
        size_t written = Serial.write((const uint8_t *)data, min(size, (size_t)64));
        if (!written) { delay(1); continue; }
        Serial.flush();
        if (!Serial.isConnected()) return false;  // flush timed out: this record is incomplete.
        data += written;
        size -= written;
        deadline = esp_timer_get_time() + 2000000;
    }
    return true;
}

static bool serial_format(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return n >= 0 && (size_t)n < sizeof(text) && serial_all(text, (size_t)n);
}

static bool parse_capture(const char *line, uint32_t &seq, int &offset) {
    if (strncmp(line, "CAP seq=", 8) != 0) return false;
    const char *p = line + 8;
    if (*p < '0' || *p > '9') return false;
    char *end;
    errno = 0;
    long number = strtol(p, &end, 10);
    if (errno || number < 0 || number > 99999999) return false;
    seq = (uint32_t)number;
    offset = 0;
    p = end;
    if (strncmp(p, " offset=", 8) == 0) {
        p += 8;
        errno = 0;
        number = strtol(p, &end, 10);
        if (errno || end == p || number < -100 || number > 100) return false;
        offset = (int)number;
        p = end;
    }
    return *p == '\0';
}

static bool parse_dump(const char *line, uint32_t &seq) {
    if (strncmp(line, "DUMP seq=", 9) != 0 || line[9] < '0' || line[9] > '9') return false;
    char *end;
    errno = 0;
    long number = strtol(line + 9, &end, 10);
    if (errno || *end || number < 0 || number > 99999999) return false;
    seq = (uint32_t)number;
    return true;
}

/* One command-contract check: reject malformed captures rather than silently retuning. */
static bool command_self_check() {
    uint32_t seq;
    int offset;
    return parse_capture("CAP seq=99999999 offset=-100", seq, offset) &&
           seq == 99999999 && offset == -100 &&
           parse_capture("CAP seq=0", seq, offset) && seq == 0 && offset == 0 &&
           !parse_capture("CAP seq=100000000", seq, offset) &&
           !parse_capture("CAP seq=1 offset=101", seq, offset) &&
           !parse_capture("CAP seq=1 offset=2 extra", seq, offset) &&
           parse_dump("DUMP seq=42", seq) && seq == 42 &&
           !parse_dump("DUMP seq=42 offset=0", seq);
}

static void ready() {
    serial_format("{\"type\":\"ready\",\"status\":%d,\"board\":\"tdeck\","
                  "\"freq_mhz\":868.125,\"offset_khz\":%d,\"bw_khz\":39.0,"
                  "\"br_kbps\":19.2,\"tcxo_v\":1.8,\"period_us\":%lu,"
                  "\"duration_us\":%lu,\"uptime_us\":%llu}\n",
                  radio_state, offset_khz, (unsigned long)PERIOD_US,
                  (unsigned long)DURATION_US, (unsigned long long)esp_timer_get_time());
}

static bool hex_bytes(const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    static const char hex[] = "0123456789abcdef";
    char out[512];
    while (size) {
        size_t n = min(size, sizeof(out) / 2);
        for (size_t i = 0; i < n; ++i) {
            out[2 * i] = hex[bytes[i] >> 4];
            out[2 * i + 1] = hex[bytes[i] & 15];
        }
        if (!serial_all(out, n * 2)) return false;
        bytes += n;
        size -= n;
    }
    return true;
}

static void output_capture(uint32_t seq) {
    // End any record truncated by a previous USB disconnect before retrying its exact bytes.
    if (!serial_format("\n")) return;
    if (!last_capture.valid || last_capture.seq != seq) {
        serial_format("{\"type\":\"error\",\"seq\":%lu,\"reason\":\"no_matching_capture\"}\n",
                      (unsigned long)seq);
        return;
    }
    const auto &saved = last_capture;
    size_t n = saved.n;
    uint32_t dt_min = UINT32_MAX, dt_max = 0;
    for (size_t i = 1; i < n; ++i) {
        uint32_t dt = time_us[i] - time_us[i - 1];
        dt_min = min(dt_min, dt);
        dt_max = max(dt_max, dt);
    }
    // ESP32-S3 is little endian. Full offsets preserve long scheduling gaps without clipping.
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "timestamp encoding");
    if (!serial_format("{\"type\":\"capture\",\"seq\":%lu,\"offset_khz\":%d,"
                  "\"status\":%d,\"start_us\":%llu,\"elapsed_us\":%lu,\"n_samples\":%u,"
                  "\"period_us\":%lu,\"skipped_slots\":%lu,\"dt_min_us\":%lu,"
                  "\"dt_max_us\":%lu,\"read_mean_us\":%.2f,\"read_max_us\":%lu,"
                  "\"rssi_hex\":\"",
                  (unsigned long)seq, saved.offset, saved.status, (unsigned long long)saved.start,
                  (unsigned long)saved.elapsed, (unsigned)n, (unsigned long)PERIOD_US,
                  (unsigned long)saved.skipped, (unsigned long)(n > 1 ? dt_min : 0),
                  (unsigned long)dt_max, n ? (double)saved.read_sum / n : 0.0,
                  (unsigned long)saved.read_max)) return;
    if (!hex_bytes(rssi, n)) return;  // raw GetRssiInst bytes: dBm = -byte / 2.
    if (!serial_format("\",\"t_us_le32_hex\":\"")) return;
    if (!hex_bytes(time_us, n * sizeof(time_us[0]))) return;  // midpoint from start_us.
    serial_format("\"}\n");
}

static void capture(uint32_t seq, int offset) {
    if (radio_state == RADIOLIB_ERR_NONE) radio_state = radio.standby();
    if (radio_state == RADIOLIB_ERR_NONE)
        radio_state = radio.setFrequency(868.125f + offset / 1000.0f);
    if (radio_state == RADIOLIB_ERR_NONE) radio_state = radio.startReceive();
    if (radio_state != RADIOLIB_ERR_NONE) {
        serial_format("{\"type\":\"error\",\"seq\":%lu,\"status\":%d}\n",
                      (unsigned long)seq, radio_state);
        return;
    }
    offset_khz = offset;
    delay(2);  // SetRx RSSI settling; no packet handler ever leaves this continuous RX.
    if (!serial_format("{\"type\":\"armed\",\"seq\":%lu,\"offset_khz\":%d}\n",
                       (unsigned long)seq, offset)) return;

    size_t n = 0;
    uint32_t skipped = 0, read_max = 0;
    uint64_t read_sum = 0;
    const int64_t start = esp_timer_get_time();
    int64_t next = start;
    last_capture.valid = false;
    while (n < MAX_SAMPLES && next < start + DURATION_US) {
        while (esp_timer_get_time() < next) {}
        int64_t before = esp_timer_get_time();
        if (before >= start + DURATION_US) break;
        // Same GetRssiInst command as getRSSI(false), but retain transport/status failures.
        // verify=false skips the extra GetStatus transaction; the returned status is parsed.
        radio_state = module.SPIreadStream(RADIOLIB_SX126X_CMD_GET_RSSI_INST,
                                           &rssi[n], 1, true, false);
        int64_t after = esp_timer_get_time();
        if (radio_state != RADIOLIB_ERR_NONE) break;
        uint32_t cost = (uint32_t)(after - before);
        read_sum += cost;
        read_max = max(read_max, cost);
        time_us[n++] = (uint32_t)((before + after) / 2 - start);
        next += PERIOD_US;
        while (next <= after) { next += PERIOD_US; ++skipped; }
    }
    last_capture = {true, seq, offset, radio_state, start,
                    (uint32_t)(esp_timer_get_time() - start), n, skipped, read_max, read_sum};
    output_capture(seq);
}

void setup() {
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    for (int cs : {12, 39, 9}) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
    delay(100);
    Serial.begin(115200);
    Serial.setTxBufferSize(1024);
    Serial.setTxTimeoutMs(1000);
    delay(2000);
    if (!command_self_check()) {
        serial_format("{\"type\":\"error\",\"reason\":\"command_self_check\"}\n");
        while (true) delay(1000);
    }
    spi.begin(40, 38, 41, 9);
    radio_state = radio.beginFSK(868.125f, 19.2f, 5.0f, 39.0f, 0, 16, 1.8f);
    uint8_t sync[] = {0x97, 0x35, 0xb1, 0x6a, 0xe3, 0x48, 0x0d, 0xfc};
    if (radio_state == RADIOLIB_ERR_NONE) radio_state = radio.setSyncWord(sync, sizeof(sync));
    if (radio_state == RADIOLIB_ERR_NONE) radio_state = radio.startReceive();
    delay(2);
    ready();
}

void loop() {
    static char line[96];
    static size_t used;
    static bool overflow;
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (used < sizeof(line) - 1) line[used++] = ch;
            else overflow = true;
            continue;
        }
        line[used] = '\0';
        uint32_t seq;
        int offset;
        if (!overflow && (!strcmp(line, "PING") || !strcmp(line, "STATUS"))) ready();
        else if (!overflow && parse_capture(line, seq, offset)) capture(seq, offset);
        else if (!overflow && parse_dump(line, seq)) output_capture(seq);
        else serial_format("{\"type\":\"error\",\"reason\":\"invalid_command\"}\n");
        used = 0;
        overflow = false;
    }
    delay(1);
}
