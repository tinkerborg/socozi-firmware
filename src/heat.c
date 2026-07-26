#include "debug.h"
#include "gpio.h"
#include "heat.h"
#include "motion.h"
#include "timing.h"

/* The factory firmware sets a countdown of 60 and decrements it once per
 * minute, clearing the heat flag when it runs out. See docs/firmware-spec.md.
 */
#define HEAT_TIMEOUT_MS (60u * 60u * 1000u)

static int      on;
static uint32_t on_since_ms;

int heat_is_on(void)
{
    return on;
}

void heat_off(void)
{
    on = 0;
    dbg.heat_on = 0;
    pin_write(PIN_HEATER, 0);
}

void heat_button(void)
{
    /* Power budget: never start a heater while a motor is drawing current.
     * The factory firmware refuses the press outright rather than queueing it.
     */
    if (motion_active() != MOTION_NONE) {
        return;
    }

    if (on) {
        heat_off();
        return;
    }

    on          = 1;
    on_since_ms = ms_ticks;
    dbg.heat_on = 1;
}

void heat_update(void)
{
    if (!on) {
        pin_write(PIN_HEATER, 0);
        return;
    }

    if ((ms_ticks - on_since_ms) >= HEAT_TIMEOUT_MS) {
        heat_off();
        return;
    }

    dbg.heat_ms = ms_ticks - on_since_ms;

    /* Cut the element while a motor runs, but keep the flag set so it resumes
     * by itself, a pause, not a cancel.
     */
    pin_write(PIN_HEATER, (motion_active() == MOTION_NONE) ? 1 : 0);
}
