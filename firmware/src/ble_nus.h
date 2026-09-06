#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Nordic UART Service peripheral: one central (the phone), newline-delimited lines. */

typedef void (*ble_nus_line_cb)(const char *line, size_t len);

void ble_nus_init(const char *device_name, ble_nus_line_cb on_line);
bool ble_nus_connected(void);
void ble_nus_send_line(const char *line);
