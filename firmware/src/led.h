#pragma once
#include <stdbool.h>

/*
 * Service indicator for the bridge itself, not an interface for the rider: the board lives in
 * a top box or under the seat, while the phone stays the only source of semantically reliable
 * status. So this reports what the phone cannot - whether the board is alive, whether it can
 * see a phone at all, and whether the radio module answers - and stays dark when all is well.
 *
 * Built only when PIN_LED is defined. No board is assumed to have one: on an unknown board a
 * guessed pin is either dark or, worse, drives something else, so the default is no indicator.
 */
void led_init(void);

/*
 * What the indicator should be doing, worst state first. Split out as a pure function so the
 * one rule that matters - a fault is never masked by traffic or by a missing phone - is
 * checkable on the host rather than only by staring at a board.
 */
typedef enum {
    LED_BOND_CLEARED,   /* confirmation of an action the user just took */
    LED_MODULE_FAULT,   /* the radio cannot be configured; nothing reaches the air */
    LED_ADVERTISING,    /* alive, no phone yet */
    LED_ACTIVITY,       /* healthy, bytes just moved */
    LED_IDLE,           /* healthy and quiet: dark */
} led_state_t;

static inline led_state_t led_pick(bool bond_cleared, bool module_ok, bool linked, bool activity) {
    if (bond_cleared) return LED_BOND_CLEARED;
    if (!module_ok) return LED_MODULE_FAULT;
    if (!linked) return LED_ADVERTISING;
    if (activity) return LED_ACTIVITY;
    return LED_IDLE;
}

/* Latest known state of the radio module. Pushed rather than polled because main.c owns it. */
void led_set_module_ok(bool ok);

/*
 * Bytes moved between us and the module. Deliberately NOT called "packet sent": the driver
 * hands bytes to the E220 over UART and a receive is bytes coming back, neither of which says
 * anything went out over the air or arrived anywhere. Proof of that is the RX->DMD2 counter on
 * the receiving phone.
 */
void led_activity(void);

/* Bonds were cleared. Both reset paths are otherwise only observable over serial. */
void led_bond_cleared(void);
