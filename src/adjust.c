#include "adjust.h"

#include "timing.h"

/* The bar's life: fills from the bottom, sits at the level, then slides off the
 * top.
 *
 * `armed` ends with BAR_HOLD, not with the animation. The slide out is the bar
 * getting out of the way, not part of the choice, so the buttons go back to the
 * motors the moment the wait is over.
 */
enum { BAR_OFF, BAR_IN, BAR_HOLD, BAR_OUT };

static uint8_t  phase;
static uint8_t  step;
static uint32_t step_ms;

/* How long BAR_HOLD lasts: the long wait after a hold, a shorter one after each
 * pick, shorter again for the readout a plain tap puts up.
 */
static uint32_t hold_ms;
static uint32_t hold_span;

static uint8_t owner;
static uint8_t level;
static int     armed;

/* Every lamp from the bottom up to `n`. */
static uint8_t filled(uint8_t n)
{
    return (uint8_t)((1u << n) - 1u);
}

static int showing(void)
{
    return phase == BAR_IN || phase == BAR_HOLD;
}

static void start(uint8_t who, uint8_t want, uint32_t span)
{
    owner = who;
    level = want;

    /* A bar already on screen stays on screen. Replaying the entrance under a
     * finger that is still down reads as the chair having lost its place, and a
     * readout and a menu are the same bar showing the same level; only what the
     * buttons mean has changed.
     */
    if (showing()) {
        hold_ms   = ms_ticks;
        hold_span = span;
        return;
    }

    phase     = BAR_IN;
    step      = 1;
    step_ms   = ms_ticks;
    hold_span = span;
}

static void slide_out(void)
{
    if (phase == BAR_OFF || phase == BAR_OUT) {
        armed = 0;
        return;
    }

    phase   = BAR_OUT;
    step    = 1;
    step_ms = ms_ticks;
    armed   = 0;
}

void adjust_show(uint8_t who, uint8_t want)
{
    start(who, want, ADJUST_READOUT_MS);
    armed = 0;
}

void adjust_open(uint8_t who, uint8_t want)
{
    start(who, want, ADJUST_ARM_MS);
    armed = 1;
}

void adjust_pick(uint8_t want)
{
    if (!armed || want == 0 || want > ADJUST_LEVEL_MAX) {
        return;
    }

    level = want;

    /* Snap to the new level rather than replaying the fill. The animation is an
     * entrance; once the bar is up, a pick should land under your finger.
     */
    phase     = BAR_HOLD;
    hold_ms   = ms_ticks;
    hold_span = ADJUST_REPICK_MS;
}

void adjust_accept(void)
{
    if (armed) {
        slide_out();
    }
}

void adjust_dismiss(void)
{
    slide_out();
}

void adjust_close(uint8_t who)
{
    if (owner != who) {
        return;
    }

    phase = BAR_OFF;
    armed = 0;
    owner = ADJUST_NOBODY;
}

void adjust_defer(uint32_t ms)
{
    uint32_t elapsed, left;

    if (!showing()) {
        return;
    }

    /* BAR_IN does not run on this clock, so the span it is extended to is the
     * one that starts when the fill lands.
     */
    if (phase == BAR_IN) {
        if (hold_span < ms) {
            hold_span = ms;
        }
        return;
    }

    elapsed = ms_ticks - hold_ms;
    left    = (elapsed < hold_span) ? hold_span - elapsed : 0;

    if (left < ms) {
        hold_ms   = ms_ticks;
        hold_span = ms;
    }
}

int adjust_armed(void)
{
    return armed;
}

uint8_t adjust_owner(void)
{
    return owner;
}

int adjust_is(uint8_t who)
{
    return owner == who && showing();
}

uint8_t adjust_bar_mask(void)
{
    switch (phase) {
    case BAR_IN:
        return filled(step);

    case BAR_HOLD:
        return filled(level);

    case BAR_OUT:
        /* The whole bar shoved up by `step` lamps, and whatever runs off the
         * top is simply gone. One expression for the entire slide, and it gives
         * a single-lamp bar a dot that travels up and leaves, the same gesture
         * at level one as at level four.
         */
        return (uint8_t)((filled(level) << step) & filled(ADJUST_LEVEL_MAX));

    default:
        return 0;
    }
}

int adjust_blink(void)
{
    return (((ms_ticks - hold_ms) / ADJUST_BLINK_MS) % 2u) == 0u;
}

void adjust_update(void)
{
    switch (phase) {
    case BAR_IN:
        if ((ms_ticks - step_ms) >= ADJUST_BAR_STEP_MS) {
            step_ms = ms_ticks;

            if (++step >= level) {
                phase   = BAR_HOLD;
                hold_ms = ms_ticks;
            }
        }
        break;

    case BAR_HOLD:
        /* Nothing more pressed, so whatever the bar is showing stands. */
        if ((ms_ticks - hold_ms) >= hold_span) {
            slide_out();
        }
        break;

    case BAR_OUT: {
        /* Nothing left after the next shift means this frame is the last lit
         * one, and it gets the longer dwell.
         */
        int last = ((filled(level) << (step + 1u))
                    & filled(ADJUST_LEVEL_MAX)) == 0;

        if ((ms_ticks - step_ms) >= (last ? ADJUST_BAR_EXIT_MS
                                          : ADJUST_BAR_STEP_MS)) {
            step_ms = ms_ticks;

            /* Once the shift reaches the width of the bar there is nothing left
             * on it, so stop rather than clock out an empty frame.
             */
            if (++step >= ADJUST_LEVEL_MAX) {
                phase = BAR_OFF;
                owner = ADJUST_NOBODY;
            }
        }
        break;
    }

    default:
        break;
    }
}
