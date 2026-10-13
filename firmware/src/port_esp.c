/* port.h on ESP-IDF: the parts that are more than a call through. */
#include "port.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/queue.h"

#define TAG "bridge"

void port_yield(void) { taskYIELD(); }

void port_mutex_lock(port_mutex_t m) { xSemaphoreTake(m, portMAX_DELAY); }

port_queue_t port_queue_create(size_t count, size_t item_size) {
    QueueHandle_t q = xQueueCreate(count, item_size);
    configASSERT(q != NULL);
    return q;
}

bool port_queue_send(port_queue_t q, const void *item) {
    return xQueueSend((QueueHandle_t)q, item, 0) == pdTRUE;
}

bool port_queue_receive(port_queue_t q, void *item, uint32_t timeout_ms) {
    return xQueueReceive((QueueHandle_t)q, item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio) {
    return xTaskCreate(fn, name, stack, NULL, prio, NULL) == pdPASS;
}

/*
 * A task that stops feeding for CONFIG_ESP_TASK_WDT_TIMEOUT_S reboots the board
 * (CONFIG_ESP_TASK_WDT_PANIC). ESP-IDF's watchdog knows its tasks by handle, so the number
 * handed back here is only for the other platforms' sake.
 */
int port_wdt_watch(void) {
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "task watchdog unavailable: %s", esp_err_to_name(err));
    return 0;
}

void port_wdt_feed(int watch) {
    (void)watch;
    esp_task_wdt_reset();
}

void port_wdt_unwatch(int watch) {
    (void)watch;
    esp_task_wdt_delete(NULL);
}

#ifndef PIN_PAIR_BUTTON
#error "PIN_PAIR_BUTTON must name this board's runtime BOOT button"
#endif

/* Pulled up and configured by main; BOOT is held when it reads low. */
bool port_button_down(void) { return gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON) == 0; }

/* RTC memory survives a software restart but not a power loss. */
static RTC_NOINIT_ATTR uint32_t s_restart_note;

void port_restart(uint32_t note) {
    s_restart_note = note;
    esp_restart();
}

uint32_t port_restart_note(void) {
    const uint32_t note = s_restart_note;
    s_restart_note = 0;
    return note;
}

const char *port_reset_cause(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_EXT: return "pin";
        case ESP_RST_SW: return "sw";
        default: return "other";
    }
}

#ifdef BENCH_CMD
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif

/*
 * Read the port the host is actually on, chosen by what the chip has rather than by where the
 * console happens to be pointed. On the S3 that is the native USB the board enumerates as
 * /dev/ttyACM*; the console there defaults to UART0 with USB only as a *secondary* output,
 * which is why logging always worked while stdin sat on a pin nothing is wired to. Reading the
 * peripheral directly sidesteps that entirely, and needs no sdkconfig to be right.
 */
void port_console_open(void) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb);
#else
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
#endif
}

int port_console_getc(uint32_t timeout_ms) {
    uint8_t c;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(timeout_ms));
#else
    int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(timeout_ms));
#endif
    return n > 0 ? c : -1;
}
#endif
