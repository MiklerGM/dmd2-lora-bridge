/* port.h on Zephyr: the parts that are more than a call through. */
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#define TAG "port"

/* ---- log ------------------------------------------------------------------ */

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/*
 * Through Zephyr's own log, deferred, so these lines and the host's never interleave and a
 * caller never waits on the USB console - which, with a terminal open and not reading, would
 * otherwise sleep inside a log call. The bench scripts look only at the text after the tag.
 */
void port_log(char level, const char *tag, const char *fmt, ...) {
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    switch (level) {
        case 'E': LOG_ERR("%s: %s", tag, text); break;
        case 'W': LOG_WRN("%s: %s", tag, text); break;
        default: LOG_INF("%s: %s", tag, text); break;
    }
}

/* ---- kernel objects -------------------------------------------------------- */

port_mutex_t port_mutex_create(void) {
    struct k_mutex *m = k_malloc(sizeof(*m));
    if (!m) k_panic();
    k_mutex_init(m);
    return m;
}

port_queue_t port_queue_create(size_t count, size_t item_size) {
    struct k_msgq *q = k_malloc(sizeof(*q));
    char *ring = k_malloc(count * item_size);
    if (!q || !ring) k_panic();
    k_msgq_init(q, ring, item_size, count);
    return q;
}

struct task_start {
    void (*fn)(void *);
};

static void task_entry(void *start, void *unused1, void *unused2) {
    (void)unused1;
    (void)unused2;
    ((struct task_start *)start)->fn(NULL);
}

/*
 * The bridge's priorities are FreeRTOS's, counting up; Zephyr counts down, so 1..6 become 14..9,
 * all preemptible. The Bluetooth host's own threads are cooperative and run ahead of every one of
 * them, and its line output (ble_nus.c, 8) runs ahead of the radio's busy wait.
 */
bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio) {
    struct k_thread *thread = k_malloc(sizeof(*thread));
    struct task_start *start = k_malloc(sizeof(*start));
    k_thread_stack_t *stack_mem = k_thread_stack_alloc(stack, 0);
    if (!thread || !start || !stack_mem) return false;
    start->fn = fn;
    k_tid_t tid = k_thread_create(thread, stack_mem, stack, task_entry, start, NULL, NULL,
                                  K_PRIO_PREEMPT(15 - prio), 0, K_NO_WAIT);
    k_thread_name_set(tid, name);
    return true;
}

/* ---- watchdog -------------------------------------------------------------- */

/* What the watchdog and a fatal error leave for the next start when they restart the board, so
 * that start can say task_wdt or panic, as an ESP32 does, rather than a plain sw. */
#define NOTE_TASK_WDT 0x5E70B001u
#define NOTE_PANIC 0x5E70B002u

static void wdt_expired(int channel, void *user_data) {
    (void)channel;
    (void)user_data;
    port_restart(NOTE_TASK_WDT);
}

/*
 * Kernel timers only, no hardware fallback yet: the nRF52's hardware watchdog cannot be stopped
 * once started, and whether the UF2 bootloader feeds it while a new image is being copied is
 * not known here. A hang with interrupts off therefore goes unnoticed until that is settled.
 */
int port_wdt_watch(void) {
    static bool ready;
    if (!ready) {
        ready = task_wdt_init(NULL) == 0;
    }
    const int channel = ready ? task_wdt_add(30 * 1000, wdt_expired, NULL) : -1;
    if (channel < 0) LOGW(TAG, "task watchdog unavailable (%d)", channel);
    return channel;
}

void port_wdt_feed(int watch) {
    if (watch >= 0) task_wdt_feed(watch);
}

void port_wdt_unwatch(int watch) {
    if (watch >= 0) task_wdt_delete(watch);
}

/* ---- pins ------------------------------------------------------------------ */

static const struct device *const s_gpio[] = {
    DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

static const struct device *pin_port(int pin) { return s_gpio[pin / 32]; }

void port_pin_output(int pin) { gpio_pin_configure(pin_port(pin), pin % 32, GPIO_OUTPUT); }
void port_pin_input(int pin) { gpio_pin_configure(pin_port(pin), pin % 32, GPIO_INPUT); }
/* Raw: the level on the wire, as the ESP32 builds mean it, not the devicetree's active sense. */
void port_pin_set(int pin, int level) { gpio_pin_set_raw(pin_port(pin), pin % 32, level); }
int port_pin_get(int pin) { return gpio_pin_get_raw(pin_port(pin), pin % 32); }

static const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Logical, so the devicetree's polarity and pull apply; configured on first use. */
bool port_button_down(void) {
    static bool ready;
    if (!ready) {
        ready = gpio_pin_configure_dt(&s_button, GPIO_INPUT) == 0;
    }
    return ready && gpio_pin_get_dt(&s_button) == 1;
}

/* ---- the radio's bus -------------------------------------------------------- */

static struct spi_dt_spec s_spi =
    SPI_DT_SPEC_GET(DT_ALIAS(lora0), SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_CONTROLLER);

/* The devicetree names the bus and the chip select; the clock is held at the ESP32 builds' 2 MHz,
 * so the two drivers time the chip alike. The pin arguments are theirs. */
void port_radio_spi_init(int nss, int sck, int mosi, int miso) {
    (void)nss;
    (void)sck;
    (void)mosi;
    (void)miso;
    if (!spi_is_ready_dt(&s_spi)) k_panic();
    s_spi.config.frequency = 2 * 1000 * 1000;
}

bool port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    const struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
    const struct spi_buf rx_buf = {.buf = rx, .len = len};
    const struct spi_buf_set txs = {.buffers = &tx_buf, .count = 1};
    const struct spi_buf_set rxs = {.buffers = &rx_buf, .count = 1};
    return spi_transceive_dt(&s_spi, &txs, &rxs) == 0;
}

/* ---- reset and restart -------------------------------------------------------- */

/*
 * Survives a software reset - RAM is not cleared - but not a power loss. Whether the UF2
 * bootloader, which runs on every reset, leaves this word alone is to be confirmed on the board;
 * if it does not, the cost is reset=sw where ble_stuck or bond_reset was meant.
 */
static __noinit uint32_t s_restart_note;

void port_restart(uint32_t note) {
    s_restart_note = note;
    sys_reboot(SYS_REBOOT_COLD);
}

/*
 * In place of Zephyr's, which halts: a halted bridge is off the air until someone re-plugs it,
 * and the kernel's watchdog, the only one running, halts with it. A restart is back in seconds.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    (void)reason;
    (void)esf;
    port_restart(NOTE_PANIC);
}

/* Read once, then cleared, so a note never outlives the restart it was left for. */
static uint32_t note_once(void) {
    static bool read;
    static uint32_t note;
    if (!read) {
        read = true;
        note = s_restart_note;
        s_restart_note = 0;
    }
    return note;
}

uint32_t port_restart_note(void) { return note_once(); }

/* The nRF52's reset reasons, in ESP-IDF's words where there is one. A brownout looks like a
 * power-on here: the chip keeps no separate flag for it. */
const char *port_reset_cause(void) {
    static const char *cause;
    if (cause) return cause;
    uint32_t flags = 0;
    if (hwinfo_get_reset_cause(&flags) != 0) flags = 0;
    (void)hwinfo_clear_reset_cause();   /* they accumulate across resets until cleared */
    if (flags & RESET_WATCHDOG) {
        cause = "wdt";
    } else if (flags & RESET_CPU_LOCKUP) {
        cause = "panic";
    } else if (flags & RESET_SOFTWARE) {
        const uint32_t note = note_once();
        cause = note == NOTE_TASK_WDT ? "task_wdt" : note == NOTE_PANIC ? "panic" : "sw";
    } else if (flags & RESET_PIN) {
        cause = "pin";
    } else if (flags == 0) {
        cause = "poweron";
    } else {
        cause = "other";
    }
    return cause;
}

/* ---- the bench's console --------------------------------------------------- */

static const struct device *const s_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

void port_console_open(void) {}

/* The console is the board's own USB, as on an ESP32-S3; polled, since only the bench reads it. */
int port_console_getc(uint32_t timeout_ms) {
    const int64_t until = k_uptime_get() + timeout_ms;
    do {
        unsigned char c;
        if (uart_poll_in(s_console, &c) == 0) return c;
        k_msleep(10);
    } while (k_uptime_get() < until);
    return -1;
}
