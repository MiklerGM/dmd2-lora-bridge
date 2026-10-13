#pragma once
/*
 * port.h on Zephyr, for the nRF52840 boards: the kernel's clock, mutexes, queues and threads;
 * pins as port * 32 + pin; the radio's bus and chip select from the board's devicetree.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

void port_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define LOGI(tag, ...) port_log('I', tag, __VA_ARGS__)
#define LOGW(tag, ...) port_log('W', tag, __VA_ARGS__)
#define LOGE(tag, ...) port_log('E', tag, __VA_ARGS__)

static inline int64_t port_uptime_us(void) {
    return (int64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}
static inline void port_delay_us(uint32_t us) { k_busy_wait(us); }
static inline void port_sleep_ms(uint32_t ms) { k_msleep((int32_t)ms); }
static inline void port_yield(void) { k_yield(); }

typedef struct k_mutex *port_mutex_t;
port_mutex_t port_mutex_create(void);
static inline bool port_mutex_take(port_mutex_t m, uint32_t timeout_ms) {
    return k_mutex_lock(m, K_MSEC(timeout_ms)) == 0;
}
static inline void port_mutex_lock(port_mutex_t m) { (void)k_mutex_lock(m, K_FOREVER); }
static inline void port_mutex_give(port_mutex_t m) { (void)k_mutex_unlock(m); }

typedef struct k_msgq *port_queue_t;
port_queue_t port_queue_create(size_t count, size_t item_size);
static inline bool port_queue_send(port_queue_t q, const void *item) {
    return k_msgq_put(q, item, K_NO_WAIT) == 0;
}
static inline bool port_queue_receive(port_queue_t q, void *item, uint32_t timeout_ms) {
    return k_msgq_get(q, item, K_MSEC(timeout_ms)) == 0;
}

bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio);

int port_wdt_watch(void);
void port_wdt_feed(int watch);
void port_wdt_unwatch(int watch);

void port_pin_output(int pin);
void port_pin_input(int pin);
void port_pin_set(int pin, int level);
int port_pin_get(int pin);
bool port_button_down(void);

void port_radio_spi_init(int nss, int sck, int mosi, int miso);
bool port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

void port_restart(uint32_t note);
uint32_t port_restart_note(void);
const char *port_reset_cause(void);

void port_console_open(void);
int port_console_getc(uint32_t timeout_ms);

/* Opening the console does not restart this board - its USB would go with it - so every build
 * reads the console for the bench's BANNER request (bridge.c). */
#define PORT_CONSOLE_NO_RESET 1
