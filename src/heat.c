#include "debug.h"
#include "enhancements.h"
#include "gpio.h"
#include "heat.h"
#include "motion.h"
#include "settings.h"
#include "timing.h"

#if ENH_HEAT_LEVELS
#include "adjust.h"
#endif

/* The factory firmware sets a countdown of 60 and decrements it once per
 * minute, clearing the heat flag when it runs out. See docs/firmware-spec.md.
 *
 * Wall clock at every level: a safety bound, not a dose. Level one gets the
 * same 60 minutes as level four, not four times longer.
 */
#define HEAT_TIMEOUT_MS (60u * 60u * 1000u)

static int      on;
static uint32_t on_since_ms;

#if ENH_HEAT_LEVELS

/* Percent of HEAT_DUTY_PERIOD_MS the element is energised, indexed by level.
 * Level 4 is 100%, which is continuous, and so reference behaviour.
 */
static const uint8_t duty_pct[HEAT_LEVEL_MAX + 1] = { 0, 25, 50, 75, 100 };

static uint8_t level;

int heat_level(void)
{
    return level;
}

/* The level to come back to. Survives switching off, which is the whole point:
 * a tap should return you to the heat you were using, and only a hold should
 * have to say anything about level. With ENH_SETTINGS_PERSIST it survives a
 * reset too, which is why it is asked for rather than held here.
 *
 * Anything outside the range means the store had nothing usable to say, so the
 * chair falls back to a working level rather than refusing to heat.
 */
static uint8_t remembered(void)
{
    uint8_t v = settings_heat_level();

    return (v >= 1 && v <= HEAT_LEVEL_MAX) ? v : HEAT_LEVEL_DEFAULT;
}

/* Switch on at the level last used, without disturbing an existing run. */
static void switch_on(void)
{
    if (on) {
        return;
    }

    on          = 1;
    on_since_ms = ms_ticks;
    level       = remembered();

    dbg.heat_on    = 1;
    dbg.heat_level = level;
}

void heat_arm(void)
{
    /* Power budget, same as a press: no heater while a motor is drawing. */
    if (motion_active() != MOTION_NONE) {
        return;
    }

    switch_on();
    adjust_open(ADJUST_HEAT, level);
}

void heat_set_level(uint8_t want)
{
    if (want == 0 || want > HEAT_LEVEL_MAX) {
        return;
    }

    level = want;
    settings_set_heat_level(want);
    dbg.heat_level = want;
}

#endif /* ENH_HEAT_LEVELS */

int heat_is_on(void)
{
    return on;
}

void heat_off(void)
{
    on = 0;
#if ENH_HEAT_LEVELS
    level = 0;
    dbg.heat_level = 0;

    /* Straight off, no slide. There is nothing left to get out of the way of,
     * and a bar animating after the heat has gone reads as the heat still
     * doing something.
     */
    adjust_close(ADJUST_HEAT);
#endif
    dbg.heat_on = 0;
    pin_write(PIN_HEATER, 0);
}

void heat_press(void)
{
#if ENH_HEAT_LEVELS
    /* Done choosing: take the level that is showing and get on with heating.
     * Ahead of the power budget check, because this starts nothing, it only
     * stops asking.
     */
    if (adjust_armed() && adjust_owner() == ADJUST_HEAT) {
        adjust_accept();
        return;
    }
#endif

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

#if ENH_HEAT_LEVELS
    /* Straight back to the level you were using. No question asked, because a
     * tap is not asking one: the bar comes up only to say what you got, the
     * four buttons keep driving the motors, and anything else you press takes
     * it down. Holding HEAT is how you ask to choose.
     */
    switch_on();
    adjust_show(ADJUST_HEAT, level);
#else
    on          = 1;
    on_since_ms = ms_ticks;
    dbg.heat_on = 1;
#endif
}

int heat_led(void)
{
    if (!on) {
        return 0;
    }

#if ENH_HEAT_LEVELS
    /* Blink while the buttons are borrowed, quickly enough to read as "waiting
     * for you" rather than as a status light. It stops the moment they go back
     * to the motors, so blinking means "still changeable".
     */
    if (adjust_armed() && adjust_owner() == ADJUST_HEAT) {
        return adjust_blink();
    }
#endif

    return 1;
}

void heat_update(void)
{
    int want;

    if (!on) {
        pin_write(PIN_HEATER, 0);
        return;
    }

    if ((ms_ticks - on_since_ms) >= HEAT_TIMEOUT_MS) {
        heat_off();
        return;
    }

    dbg.heat_ms = ms_ticks - on_since_ms;

#if ENH_HEAT_LEVELS
    /* Phased from the moment heat came on, so the cycle always opens with the
     * element energised rather than in a gap.
     */
    {
        uint32_t phase = (ms_ticks - on_since_ms) % HEAT_DUTY_PERIOD_MS;
        uint32_t span  = HEAT_DUTY_PERIOD_MS * duty_pct[level] / 100u;

        want = phase < span;
    }
#else
    want = 1;
#endif

    /* Cut the element while a motor runs, but keep the flag set so it resumes
     * by itself, a pause, not a cancel. The duty cycle keeps running
     * underneath, so a motion does not re-phase it.
     */
    pin_write(PIN_HEATER, (want && motion_active() == MOTION_NONE) ? 1 : 0);
}
