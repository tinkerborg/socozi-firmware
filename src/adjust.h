/* The level adjuster: one bar graph, one borrowed set of buttons.
 *
 * The handset has no spare buttons and reports one code at a time, so a level
 * has to be expressed by borrowing buttons that already mean something else.
 * This owns that borrowing, and the bar graph that says it is happening.
 *
 * Two functions want it — heat and massage intensity — and they cannot both
 * have it, because there is one set of motion buttons and one bar. So the
 * adjuster has an *owner*, and opening it hands ownership over.
 *
 * The lifecycle, which is the same whichever function is asking:
 *
 *   adjust_show()    a plain tap turned something on. The bar comes up as a
 *                    readout, saying what level you got. The four motion
 *                    buttons still drive the motors, and any other button takes
 *                    the bar down.
 *   adjust_open()    a hold asked to choose. The same bar, now a menu: the four
 *                    motion buttons mean levels 1..4 for as long as it is up,
 *                    and pressing one of the things a menu offers does not
 *                    dismiss it.
 *   adjust_pick()    a level was chosen. Applies at once and reopens a shorter
 *                    window, renewed by each pick, so a level can be felt and
 *                    then corrected.
 *   adjust_accept()  the owner's own button says "that one". Closes the menu
 *                    without switching anything off.
 *
 * The bar animates in from the bottom and slides off the top, and this owns
 * that too. See enhancements-spec.md §2.3.
 */

#ifndef ADJUST_H
#define ADJUST_H

#include <stdint.h>

#include "enhancements.h"

/* Four levels, because there are four motion buttons and four lamps to show
 * them on. Not a coincidence, and not something to change on one side alone.
 */
#define ADJUST_LEVEL_MAX 4

/* Who currently owns the buttons and the bar. */
enum {
    ADJUST_NOBODY = 0,
    ADJUST_HEAT,
    ADJUST_MASSAGE,
};

/* Three spans, because the three waits are for different things.
 *
 * ADJUST_ARM_MS is you deciding what level you want, having asked to be asked.
 * ADJUST_REPICK_MS starts from a level you have just felt, so it only has to be
 * long enough to change your mind. ADJUST_READOUT_MS is not a wait at all, just
 * how long the bar stays up after a plain tap to tell you what you got.
 */
#define ADJUST_ARM_MS     8000u
#define ADJUST_READOUT_MS 2000u

/* "That landed", as a flash. One definition, used by every gesture that
 * records something and moves nothing: picking a level here, and saving a
 * preset in macros/preset.c. They should not be distinguishable, because they
 * mean the same thing.
 *
 * A pick ends the window there and then and flashes the level back, accepting
 * nothing in between: leaving it open to be corrected meant sitting through a
 * lit bar that was still taking input, and the wait read as the chair not
 * having understood.
 */
#define ADJUST_CONFIRM_FLASHES 3
#define ADJUST_CONFIRM_MS      250u

/* How fast the owner's own lamp blinks to say the buttons are borrowed. */
#define ADJUST_BLINK_MS 250u

/* One lamp of the bar's fill-in and slide-out animations, and the longer dwell
 * on whichever frame is the last one with a lamp still lit.
 *
 * How long the top lamp stays lit on the way out depends on how wide the bar
 * is: a level four bar reaches it early and keeps it, a level one bar is a
 * single dot that touches it for one step and is gone, which the eye misses
 * because it is already tracking the dot off the edge. The dwell gives every
 * level the same parting.
 */
#define ADJUST_BAR_STEP_MS 80u
#define ADJUST_BAR_EXIT_MS 200u

/* Put the bar up as a readout. The buttons keep meaning motors. */
void adjust_show(uint8_t owner, uint8_t level);

/* Put the bar up as a menu and borrow the buttons for ADJUST_ARM_MS. Takes
 * ownership from whoever had it.
 */
void adjust_open(uint8_t owner, uint8_t level);

/* A level button was pressed. Takes the level, ends the window immediately, and
 * flashes the bar back as confirmation. The caller passes the level on to
 * whatever it means; this only knows how many lamps to light.
 */
void adjust_pick(uint8_t level);

/* The owner's own button, closing the menu on whatever is showing. Not an off
 * switch: there is a question on the table and this answers it.
 */
void adjust_accept(void);

/* Some other button. Takes the bar down early, readout or menu alike — anything
 * that is not a level and not the owner's button means the user has moved on.
 */
void adjust_dismiss(void);

/* Take the bar down at once, with no slide. For an owner switching off, where
 * a bar still animating reads as the function still doing something.
 */
void adjust_close(uint8_t owner);

/* Keep a bar that is already up for at least `ms` longer.
 *
 * Called when the owner's button goes down, with the hold threshold, so a
 * readout cannot time out in the gap between the press and knowing whether it
 * was a hold. Without it, starting to hold late in a readout makes the bar
 * slide away and then come back, which looks like the chair losing its place.
 */
void adjust_defer(uint32_t ms);

/* True while the motion buttons mean levels. control.c uses this to route those
 * presses to the owner instead of to the motors.
 */
int adjust_armed(void);

/* ADJUST_*, so a pick can be sent to whoever asked for it. */
uint8_t adjust_owner(void);

/* Whether `who` is the one currently being adjusted. */
int adjust_is(uint8_t who);

/* Which bar lamps to light, bit 0 the bottom one. A mask rather than a level
 * because the bar animates, and a bar sliding off the top is not a level.
 */
uint8_t adjust_bar_mask(void);

/* What the owner's own lamp should do: blink while the buttons are borrowed.
 * Returns 1 when it should be lit, and is only meaningful for the owner.
 */
int adjust_blink(void);

/* Call every loop. Advances the animation and closes the window when its time
 * is up.
 */
void adjust_update(void);

/* The two halves of the lamp animation, as rules rather than as an animation.
 *
 * Anything that puts a mask up on the four motion lamps arrives and leaves the
 * same way, so these are shared: macros/preset.c uses them for the slot display
 * exactly as the bar does for a level.
 *
 * In: only the bottom `step` lamps have appeared yet.
 * Out: the whole thing shoved up by `step`, and whatever runs off the top is
 * gone — which gives a single lamp a dot that travels up and leaves, the same
 * gesture as a full row emptying.
 */
uint8_t adjust_fill_in(uint8_t mask, uint8_t step);
uint8_t adjust_slide_out(uint8_t mask, uint8_t step);

#endif /* ADJUST_H */
