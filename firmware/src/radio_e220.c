/* Compiled only into the build that selects one of these backends; see radio.h. */
#ifdef RADIO_BACKEND_E220

#include <string.h>

#include "radio.h"

/*
 * EBYTE's UART modules behind the seam - E220 and E22 both, because they are one driver. An
 * adapter and nothing else: e220.c is untouched, since the driver was already proven on
 * hardware and rewriting working register code to fit a new signature is how proven things
 * stop being proven.
 */

static radio_state_t from_e220(e220_state_t s) {
    switch (s) {
        case E220_OK: return RADIO_OK;
        case E220_MISMATCH: return RADIO_MISMATCH;
        case E220_BUSY: return RADIO_BUSY;
        case E220_NO_ANSWER: break;
    }
    return RADIO_NO_ANSWER;
}

void radio_init(void) { e220_init(); }

radio_state_t radio_apply(const radio_request_t *req) {
    e220_config_t hw = e220_config_for(req);
    return from_e220(e220_apply(&hw));
}

radio_state_t radio_verify(const radio_request_t *req) {
    e220_config_t hw = e220_config_for(req);
    return from_e220(e220_verify(&hw));
}

bool radio_send(const uint8_t *data, size_t len) { return e220_write(data, len); }

static int s_last_rssi;
static bool s_have_rssi;

/*
 * The module appends one RSSI byte to every packet (REG3 bit 7, e220_config_for), so read one
 * byte more than the caller asked for and keep it apart. The caller's overflow sentinel still
 * works: a read that fills the larger buffer is a payload of at least out_len, which is returned
 * as out_len for the caller to reject. Only reached once the registers read back as ours, bit 7
 * included, so the last byte really is the RSSI.
 */
size_t radio_receive(uint8_t *out, size_t out_len, uint32_t idle_gap_ms, uint32_t first_wait_ms) {
    static uint8_t raw[256 + 1];
    const size_t cap = out_len + 1 <= sizeof(raw) ? out_len + 1 : sizeof(raw);
    size_t n = e220_read_packet(raw, cap, idle_gap_ms, first_wait_ms);
    s_have_rssi = false;
    if (n == cap) {
        memcpy(out, raw, out_len);
        return out_len;
    }
    if (n < 2) return 0;   /* nothing, or a lone byte with no payload */
    s_last_rssi = -(256 - (int)raw[n - 1]);   /* dBm, per the manual */
    s_have_rssi = true;
    memcpy(out, raw, n - 1);
    return n - 1;
}

bool radio_last_rssi(int *dbm) {
    if (s_have_rssi) *dbm = s_last_rssi;
    return s_have_rssi;
}

size_t radio_identity(char *out, size_t out_len) { return e220_read_firmware(out, out_len); }

bool radio_tx_dbm(int8_t out[4]) { return e220_tx_dbm(out); }

/* Whichever of the two parts answered at init. */
const char *radio_name(void) { return e220_part_name(); }

#ifdef BENCH_CMD
#include "esp_log.h"
#include "esp_timer.h"

#include "radio_bench.h"

bool radio_bench_tx(const bench_tx_t *req, const uint8_t *payload, size_t len, uint32_t *air_us) {
    /*
     * The module owns its own PHY, so preamble symbols and sync registers are not ours to set.
     * Refusing is the point: a bench that silently ignored them would report a sweep over
     * settings that never changed, which is the failure mode this whole interface exists to
     * remove.
     */
    if (req->preamble >= 0 || req->sync >= 0 || req->hdr >= 0 || req->foff != BENCH_FOFF_NONE ||
        req->ldo >= 0 || req->txmod >= 0 || req->cal >= 0 || req->pa >= 0) {
        ESP_LOGW("e220", "preamble, sync, the frame header and the frequency are the module's own");
        return false;
    }
    if (req->power >= 0) {
        e220_config_t hw = e220_config_for(&(radio_request_t){
            .channel = RADIO_DEFAULT_CHANNEL, .power_level = (uint8_t)req->power, .lbt = false});
        if (e220_apply(&hw) != E220_OK) return false;
    }
    const int64_t t0 = esp_timer_get_time();
    const bool ok = e220_write(payload, len);
    if (air_us) *air_us = (uint32_t)(esp_timer_get_time() - t0);
    return ok;
}
#endif

#endif /* RADIO_BACKEND_E220 */
