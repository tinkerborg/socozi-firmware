#include "debug.h"
#include "enhancements.h"
#include "gpio.h"
#include "heat.h"
#include "motion.h"
#include "settings.h"
#include "timing.h"

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

/* The bar graph's life: fills from the bottom, sits at the level, then slides
 * off the top.
 *
 * `armed` says whether the four motion buttons currently mean levels or still
 * mean motors, and it ends with BAR_HOLD, not with the animation. The slide out
 * is the bar getting out of the way, not part of the choice, so the buttons go
 * back to the motors the moment the wait is over.
 */
enum { BAR_OFF, BAR_IN, BAR_HOLD, BAR_OUT };

static uint8_t  bar_phase;
static uint8_t  bar_step;
static uint32_t bar_step_ms;

/* How long BAR_HOLD lasts: the long wait after a hold, a shorter one after each
 * pick, shorter again for the readout a plain tap puts up.
 */
static uint32_t bar_ms;
static uint32_t bar_span;

static int armed;

int heat_level(void)
{
    return level;
}

int heat_armed(void)
{
    return armed;
}

/* Every lamp from the bottom up to `n`. */
static uint8_t bar_filled(uint8_t n)
{
    return (uint8_t)((1u << n) - 1u);
}

uint8_t heat_bar_mask(void)
{
    switch (bar_phase) {
    case BAR_IN:
        return bar_filled(bar_step);

    case BAR_HOLD:
        return bar_filled(level);

    case BAR_OUT:
        /* The whole bar shoved up by `bar_step` lamps, and whatever runs off
         * the top is simply gone. One expression for the entire slide, and it
         * gives a single-lamp bar a dot that travels up and leaves, which is
         * the same gesture at level one as at level four.
         */
        return (uint8_t)((bar_filled(level) << bar_step)
                         & bar_filled(HEAT_LEVEL_MAX));

    default:
        return 0;
    }
}

static void bar_start(uint32_t span)
{
    bar_phase   = BAR_IN;
    bar_step    = 1;
    bar_step_ms = ms_ticks;
    bar_span    = span;
}

static void bar_dismiss(void)
{
    if (bar_phase == BAR_OFF || bar_phase == BAR_OUT) {
        return;
    }

    bar_phase   = BAR_OUT;
    bar_step    = 1;
    bar_step_ms = ms_ticks;
    armed       = 0;
}

void heat_bar_cancel(void)
{
    /* Only the readout. A menu is not dismissed by pressing one of the things
     * it is offering.
     */
    if (!armed) {
        bar_dismiss();
    }
}

/* True while the bar is on screen, whether it is still arriving or sitting. */
static int bar_showing(void)
{
    return bar_phase == BAR_IN || bar_phase == BAR_HOLD;
}

void heat_bar_defer(uint32_t ms)
{
    uint32_t elapsed, left;

    if (!bar_showing()) {
        return;
    }

    /* BAR_IN does not run on this clock, so the span it is extended to is the
     * one that starts when the fill lands.
     */
    if (bar_phase == BAR_IN) {
        if (bar_span < ms) {
            bar_span = ms;
        }
        return;
    }

    elapsed = ms_ticks - bar_ms;
    left    = (elapsed < bar_span) ? bar_span - elapsed : 0;

    if (left < ms) {
        bar_ms   = ms_ticks;
        bar_span = ms;
    }
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

    armed = 1;

    /* A bar already on screen stays on screen. Replaying the entrance under a
     * finger that is still down reads as the chair having lost its place, and
     * the readout and the menu are the same bar showing the same level; only
     * what the buttons mean has changed.
     */
    if (bar_showing()) {
        bar_ms   = ms_ticks;
        bar_span = HEAT_ARM_MS;
    } else {
        bar_start(HEAT_ARM_MS);
    }
}

void heat_select_level(uint8_t want)
{
    if (!armed || want == 0 || want > HEAT_LEVEL_MAX) {
        return;
    }

    level = want;
    settings_set_heat_level(want);

    /* Snap to the new level rather than replaying the fill. The animation is an
     * entrance; once the bar is up, a pick should land under your finger.
     *
     * Stay armed, on the shorter span. You have just changed the heat and the
     * obvious next thought is whether you wanted that much, so the buttons keep
     * meaning levels for a moment longer, and every press renews it.
     */
    bar_phase = BAR_HOLD;
    bar_ms    = ms_ticks;
    bar_span  = HEAT_ARM_REPICK_MS;

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
    armed     = 0;
    bar_phase = BAR_OFF;
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
    if (armed) {
        bar_dismiss();
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

    armed = 0;
    bar_start(HEAT_BAR_MS);
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
    /* Blink while the buttons are handed over, quickly enough to read as
     * "waiting for you" rather than as a status light. It stops the moment they
     * go back to the motors, so blinking means "still changeable".
     */
    if (armed) {
        return (((ms_ticks - bar_ms) / HEAT_ARM_BLINK_MS) % 2u) == 0u;
    }
#endif

    return 1;
}

void heat_update(void)
{
    int want;

#if ENH_HEAT_LEVELS
    /* Walk the bar through its phases. The buttons go back to the motors when
     * the wait ends, which is where BAR_HOLD hands over to BAR_OUT; the slide
     * after that is only the bar leaving.
     */
    switch (bar_phase) {
    case BAR_IN:
        if ((ms_ticks - bar_step_ms) >= HEAT_BAR_STEP_MS) {
            bar_step_ms = ms_ticks;

            if (++bar_step >= level) {
                bar_phase = BAR_HOLD;
                bar_ms    = ms_ticks;
            }
        }
        break;

    case BAR_HOLD:
        /* Nothing more pressed, so whatever the bar is showing stands. */
        if ((ms_ticks - bar_ms) >= bar_span) {
            bar_dismiss();
        }
        break;

    case BAR_OUT: {
        /* Nothing left after the next shift means this frame is the last lit
         * one, and it gets the longer dwell.
         */
        int last = ((bar_filled(level) << (bar_step + 1u))
                    & bar_filled(HEAT_LEVEL_MAX)) == 0;

        if ((ms_ticks - bar_step_ms) >= (last ? HEAT_BAR_EXIT_MS
                                              : HEAT_BAR_STEP_MS)) {
            bar_step_ms = ms_ticks;

            /* Once the shift reaches the width of the bar there is nothing
             * left on it, so stop rather than clock out an empty frame.
             */
            if (++bar_step >= HEAT_LEVEL_MAX) {
                bar_phase = BAR_OFF;
            }
        }
        break;
    }

    default:
        break;
    }
#endif

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
