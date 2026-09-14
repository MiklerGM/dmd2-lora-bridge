#include "wire_payload.h"

bool wire_payload_valid(const uint8_t *payload, size_t len) {
    if (len == 0 || len > WIRE_PAYLOAD_MAX || len % 4 == 1) return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = payload[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/') continue;
        return false;
    }
    return true;
}
