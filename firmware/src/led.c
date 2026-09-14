#include "led.h"

#include "ble_nus.h"

#ifdef PIN_LED

#include <stdatomic.h>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * Priority, highest first. One indicator cannot carry four independent facts at once, so the
 * pattern is whichever state below is worst, not a blend of them:
 *
 *   1. bonds just cleared   three quick flashes, once
 *   2. module not answering double flash every 3 s
 *   3. no phone connected   single flash every 3 s
 *   4. healthy              dark, with a brief flash on radio activity
 *
 * Advertising is deliberately slow rather than the 5 Hz an idle device is often given: a fast
 * blink reads as an alarm, and "no phone yet" is the normal state of a board that was just
 * switched on.
 *
 * ponytail: a module that dies while healthy is only noticed at the next probe, so the fault
 * pattern can lag by up to RADIO_PROBE_MS (30 s). Shortening that is a main.c decision about
 * how often to disturb the UART, not something this file should paper over.
 */

static atomic_bool s_module_ok = ATOMIC_VAR_INIT(true);
static atomic_bool s_bond_cleared = ATOMIC_VAR_INIT(false);
static atomic_bool s_activity = ATOMIC_VAR_INIT(false);

static void flash(int on_ms, int off_ms) {
    gpio_set_level((gpio_num_t)PIN_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    gpio_set_level((gpio_num_t)PIN_LED, 0);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void led_task(void *arg) {
    (void)arg;
    for (;;) {
        /* Consume one-shot events atomically. An event masked by a more important state is
         * deliberately dropped rather than flashing later, after it has become stale. */
        bool bond_cleared = atomic_exchange(&s_bond_cleared, false);
        bool activity = atomic_exchange(&s_activity, false);
        led_state_t state = led_pick(bond_cleared, atomic_load(&s_module_ok),
                                     ble_nus_connected(), activity);
        switch (state) {
        case LED_BOND_CLEARED:
            for (int i = 0; i < 3; i++) flash(80, 120);
            break;
        case LED_MODULE_FAULT:
            flash(60, 150);
            flash(60, 2600);
            break;
        case LED_ADVERTISING:
            flash(60, 2900);
            break;
        case LED_ACTIVITY:
            flash(20, 60);
            break;
        case LED_IDLE:
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }
}

void led_init(void) {
    gpio_config_t led = {
        .pin_bit_mask = (1ULL << PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
    gpio_set_level((gpio_num_t)PIN_LED, 0);
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
}

void led_set_module_ok(bool ok) {
    atomic_store(&s_module_ok, ok);
    if (!ok) atomic_store(&s_activity, false);
}
void led_activity(void) { atomic_store(&s_activity, true); }
void led_bond_cleared(void) { atomic_store(&s_bond_cleared, true); }

#else  /* no indicator on this board */

void led_init(void) {}
void led_set_module_ok(bool ok) { (void)ok; }
void led_activity(void) {}
void led_bond_cleared(void) {}

#endif
