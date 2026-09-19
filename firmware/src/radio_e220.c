/* Compiled only into the build that selects one of these backends; see radio.h. */
#ifdef RADIO_BACKEND_E220

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

size_t radio_receive(uint8_t *out, size_t out_len, uint32_t idle_gap_ms, uint32_t first_wait_ms) {
    return e220_read_packet(out, out_len, idle_gap_ms, first_wait_ms);
}

size_t radio_identity(char *out, size_t out_len) { return e220_read_firmware(out, out_len); }

bool radio_tx_dbm(int8_t out[4]) { return e220_tx_dbm(out); }

/* Whichever of the two parts answered at init. */
const char *radio_name(void) { return e220_part_name(); }

#endif /* RADIO_BACKEND_E220 */
