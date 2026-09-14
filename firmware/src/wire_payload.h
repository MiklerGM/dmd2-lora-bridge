#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DMD sends standard base64 without padding or line wrapping; chat is the largest current
 * packet at 200 wire characters (a maximum group packet is 187). */
#define WIRE_PAYLOAD_MAX 200

bool wire_payload_valid(const uint8_t *payload, size_t len);
