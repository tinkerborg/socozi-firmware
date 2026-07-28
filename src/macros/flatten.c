#include "macros/flatten.h"

#include "debug.h"
#include "enhancements.h"
#include "handset.h"
#include "motion.h"
#include "power.h"
#include "timing.h"

/* Half on, half off, so a full cycle is twice this. */
#define FLATTEN_BLINK_MS 500

static int held;

#if ENH_POWER_DOUBLE_TAP
static int unattended;
#else
enum { unattended = 0 };
#endif

/* What the factory does when the macro ends: everything off, gate off, and
 * pneumatics_shutdown() inside power_comfort_off() starts the vent.
 */
static void finish(void)
{
    power_comfort_off();

    if (power_is_on()) {
        power_toggle();
    }
}

void flatten_hold_start(void)
{
    held = 1;
    motion_request(MOTION_FLATTEN);
}

void flatten_hold_end(void)
{
    held = 0;
    finish();
}

void flatten_tap_start(void)
{
#if ENH_POWER_DOUBLE_TAP
    unattended = 1;
    dbg.auto_moves++;
    motion_request(MOTION_FLATTEN);
#endif
}

uint32_t flatten_update(uint8_t button)
{
    if (held) {
        return MOTION_FLATTEN;
    }

#if ENH_POWER_DOUBLE_TAP
    if (unattended) {
        if (button != HS_NONE) {
            unattended = 0;         /* cancelled, leave everything as it is */
        } else if (motion_active() == MOTION_NONE) {
            /* Ended on its own, at the stops or on the ceiling. The unattended
             * equivalent of releasing a held POWER.
             */
            unattended = 0;
            finish();
        } else {
            return MOTION_FLATTEN;
        }
    }
#else
    (void)button;
#endif

    return MOTION_NONE;
}

int flatten_led_power(void)
{
    if (!unattended) {
        return -1;
    }

    return (int)((ms_ticks / FLATTEN_BLINK_MS) & 1);
}
