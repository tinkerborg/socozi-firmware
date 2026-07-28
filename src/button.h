/* Handset button codes to events.
 *
 * The handset reports one code at a time, so "which button" and "for how long"
 * is all the information there is. This turns that into discrete events, so
 * nothing downstream has to keep its own edge or hold timers.
 *
 * Events are emitted for every button. Callers ignore the ones they don't care
 * about, which is most of them: only POWER and HEAT use taps and holds.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

#include "enhancements.h"

/* Hold POWER this long to trigger the all-off-and-flatten macro.
 *
 * The factory threshold is 20 counts of its action 0xF0, whose rate is set by a
 * compiler-generated division we never decoded. Observed on the chair as about
 * two seconds, which is what this reproduces.
 */
#define BUTTON_HOLD_MS 2000

#if ENH_HEAT_LEVELS
/* HEAT's hold is shorter than POWER's, and deliberately so. POWER's hold starts
 * the chair moving, so it wants to be hard to do by accident; HEAT's only opens
 * something you can ignore, and it is a thing you do often.
 */
#define BUTTON_HEAT_HOLD_MS 1250
#endif

/* Two taps inside this window are a double tap. Long enough for a deliberate
 * one, short enough that two separate intentional presses don't trip it.
 */
#define BUTTON_DOUBLE_MS 400

enum {
    BTN_NONE = 0,
    BTN_PRESS,          /* went down */
    BTN_TAP,            /* released before the hold threshold */
    BTN_DOUBLE_TAP,     /* released, and the second tap inside the window */
    BTN_HOLD,           /* still down, past the threshold; fires once */
    BTN_HOLD_RELEASE,   /* released, having held */
};

struct button_event {
    uint8_t kind;       /* BTN_* */
    uint8_t button;     /* HS_*, the button the event refers to */
};

/* Feed the current button code once per loop. At most one event per call.
 *
 * On a release the event carries the button that *was* down, not HS_NONE.
 */
struct button_event button_update(uint8_t button);

/* True from BTN_HOLD until the release that ends it. Lets a caller drive
 * something for as long as the button stays down without tracking it again.
 */
int button_hold_active(void);

#endif /* BUTTON_H */
