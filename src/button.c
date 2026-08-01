#include "button.h"
#include "handset.h"
#include "timing.h"

static uint8_t  prev;
static uint32_t down_ms;
static int      hold_done;

/* Double-tap tracking. The window is measured release to release, and is only
 * honoured when both taps are the same button.
 */
static uint32_t tap_ms;
static uint8_t  tap_button;
static int      tapped;

int button_hold_active(void)
{
    return hold_done;
}

/* How long this button has to be down to count as held.
 *
 * The one place in here that knows a button apart. Keeping it to a single
 * function is the compromise: the alternative is a threshold argument threaded
 * through from control.c, which would put timing back in the caller that this
 * module exists to take it away from.
 */
static uint32_t hold_ms_for(uint8_t button)
{
    switch (button) {
#if ENH_HEAT_LEVELS
    case HS_HEAT:
#endif
#if ENH_MASSAGE_LEVELS
    case HS_MASSAGE:
#endif
#if ENH_PRESET
    /* Only meaningful while the preset window is open, where a hold clears the
     * slot. Outside it nothing listens for these, and a motion carries on for
     * as long as the button is down either way.
     */
    case HS_RECLINE_UP:
    case HS_RECLINE_DOWN:
    case HS_HEADREST_UP:
    case HS_HEADREST_DOWN:
#endif
#if BUTTONS_BORROWED
        return BUTTON_SHORT_HOLD_MS;
#endif

    default:
        return BUTTON_HOLD_MS;
    }
}

struct button_event button_update(uint8_t button)
{
    struct button_event ev = { BTN_NONE, button };

    if (button != prev) {
        if (button != HS_NONE) {
            /* A press, including one that replaces another button without
             * passing through HS_NONE.
             */
            down_ms   = ms_ticks;
            hold_done = 0;
            ev.kind   = BTN_PRESS;
        } else if (hold_done) {
            ev.kind   = BTN_HOLD_RELEASE;
            ev.button = prev;
        } else if (tapped && prev == tap_button &&
                   (ms_ticks - tap_ms) <= BUTTON_DOUBLE_MS) {
            tapped    = 0;
            ev.kind   = BTN_DOUBLE_TAP;
            ev.button = prev;
        } else {
            tapped     = 1;
            tap_ms     = ms_ticks;
            tap_button = prev;
            ev.kind    = BTN_TAP;
            ev.button  = prev;
        }

        prev = button;
        return ev;
    }

    if (button != HS_NONE && !hold_done &&
        (ms_ticks - down_ms) >= hold_ms_for(button)) {
        hold_done = 1;
        ev.kind   = BTN_HOLD;
    }

    return ev;
}
