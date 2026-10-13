#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the shared firmware asks of the platform under it. ESP-IDF on the ESP32 boards, Zephyr
 * on the nRF52840 ones.
 *
 * Each backend supplies the same names, selected by the build - no runtime choice and no table
 * of function pointers, for the same reason radio.h has none. Pins are plain numbers: a GPIO
 * number on an ESP32, port * 32 + pin on an nRF52.
 *
 *   int64_t port_uptime_us(void)                monotonic, since boot
 *   void    port_delay_us(uint32_t us)          busy wait, for the radio's sub-millisecond timing
 *   void    port_sleep_ms(uint32_t ms)          blocks the caller; others run meanwhile
 *   void    port_yield(void)
 *
 *   port_mutex_t port_mutex_create(void)        does not return without one
 *   bool    port_mutex_take(port_mutex_t, uint32_t timeout_ms)
 *   void    port_mutex_lock(port_mutex_t)       waits for as long as it takes
 *   void    port_mutex_give(port_mutex_t)
 *
 *   port_queue_t port_queue_create(size_t count, size_t item_size)   does not return without one
 *   bool    port_queue_send(port_queue_t, const void *item)          never waits
 *   bool    port_queue_receive(port_queue_t, void *item, uint32_t timeout_ms)
 *
 *   bool    port_task_create(void (*fn)(void *), const char *name, size_t stack_bytes, int prio)
 *                                               prio as FreeRTOS counts it: higher runs first
 *
 *   int     port_wdt_watch(void)                restart the board if the calling task stops
 *   void    port_wdt_feed(int watch)            feeding this for 30 s
 *   void    port_wdt_unwatch(int watch)
 *
 *   void    port_pin_output(int pin)            no pulls
 *   void    port_pin_input(int pin)             no pulls
 *   void    port_pin_set(int pin, int level)    the physical level, whatever the board calls active
 *   int     port_pin_get(int pin)
 *   bool    port_button_down(void)              the board's one button, held
 *
 *   void    port_radio_spi_init(int nss, int sck, int mosi, int miso)
 *                                               mode 0, 2 MHz, chip select driven by the bus; a
 *                                               platform that takes the bus from its devicetree
 *                                               ignores the pins
 *   bool    port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
 *                                               full duplex; whether the bus completed it
 *
 *   void    port_restart(uint32_t note)         restarts, leaving the note for the next start
 *   uint32_t port_restart_note(void)            that note once, then 0; 0 after a power loss
 *   const char *port_reset_cause(void)          poweron brownout panic int_wdt task_wdt wdt pin
 *                                               sw other - in ESP-IDF's words where it has one
 *
 *   void    port_console_open(void)             the host's side of the console, where read
 *   int     port_console_getc(uint32_t timeout_ms)                   the next byte, or -1
 *   PORT_CONSOLE_NO_RESET                       defined where opening the console cannot restart
 *                                               the board, so the bench asks for its banner
 *
 *   LOGI(tag, fmt, ...), LOGW(...), LOGE(...)   one line each
 */
#ifdef __ZEPHYR__
#include "port_zephyr.h"
#else
#include "port_esp.h"
#endif
