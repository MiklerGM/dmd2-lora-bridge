/* Host test for the indicator's priority rule. Intervals, GPIO and anything needing a board
 * are deliberately not tested here - those are checked by looking at a real one. */
#include <assert.h>
#include <stdio.h>

#include "../src/led.h"

int main(void) {
    /* The rule worth protecting: a fault outranks everything except the user's own action, so
     * traffic or a missing phone can never make a dead radio look fine. */
    assert(led_pick(false, false, true, true) == LED_MODULE_FAULT);
    assert(led_pick(false, false, false, false) == LED_MODULE_FAULT);
    assert(led_pick(true, false, true, true) == LED_BOND_CLEARED);

    /* A board with no phone says so rather than showing the dark "all well". */
    assert(led_pick(false, true, false, true) == LED_ADVERTISING);

    /* Healthy: dark unless something just moved. */
    assert(led_pick(false, true, true, true) == LED_ACTIVITY);
    assert(led_pick(false, true, true, false) == LED_IDLE);

    printf("led priority                 ok\n");
    return 0;
}
