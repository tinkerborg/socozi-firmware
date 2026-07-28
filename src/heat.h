/* Seat heater on PC14.
 *
 * The hardware has no temperature sensor, so nothing here closes a loop. The
 * factory firmware simply energises the element, and the 60 minute auto-off is
 * the only bound on a stuck-on heater. That bound is not optional and applies
 * in both builds.
 *
 * With ENH_HEAT_LEVELS the element is duty cycled at one of four levels. See
 * enhancements-spec.md §2.3. A level is still not a temperature, only a
 * fraction of the time the element is energised.
 *
 * Tapping HEAT switches on at the level you last used, which is the answer
 * nearly every time, and asks nothing. Holding it hands the four motion buttons
 * over to the levels for a few seconds, with the bar graph saying so. The
 * handset has no spare buttons and reports one code at a time, so borrowing is
 * the only way to express a level at all.
 */

#ifndef HEAT_H
#define HEAT_H

#include <stdint.h>

#include "enhancements.h"

#if ENH_HEAT_LEVELS

/* Level 4 is 100% duty, which is continuous, and so exactly what the reference
 * build does. The enhancement only ever removes heat.
 */
#define HEAT_LEVEL_MAX 4

/* Where an untouched chair starts, so first use matches the factory. */
#define HEAT_LEVEL_DEFAULT HEAT_LEVEL_MAX

/* One switching cycle. Slow on purpose: the element averages it without help,
 * and fast switching would put noise on a board whose handset cable already
 * collects USART errors.
 */
#define HEAT_DUTY_PERIOD_MS 20000u

/* Three spans, because the three waits are for different things. HEAT_ARM_MS is
 * you deciding what level you want, having asked to be asked.
 * HEAT_ARM_REPICK_MS starts from a level you have just felt, so it only has to
 * be long enough to change your mind, and every press starts it again.
 * HEAT_BAR_MS is not a wait at all, just how long the bar stays up after a
 * plain tap to tell you what you got.
 */
#define HEAT_ARM_MS        8000u
#define HEAT_ARM_REPICK_MS 2000u
#define HEAT_BAR_MS        2000u
#define HEAT_ARM_BLINK_MS  250u

/* One lamp of the bar's fill-in and slide-out animations. */
#define HEAT_BAR_STEP_MS 80u

/* The last frame of the slide-out, held longer than a step.
 *
 * How long the top lamp stays lit on the way out depends on how wide the bar
 * is: a level four bar reaches it early and keeps it, a level one bar is a
 * single dot that touches it for one step and is gone, which the eye misses
 * because it is already tracking the dot off the edge. Dwelling on whichever
 * frame is the last lit one gives every level the same parting.
 */
#define HEAT_BAR_EXIT_MS 200u

/* HEAT held. Switches on at the remembered level if it was off, and hands the
 * four motion buttons over to the levels for HEAT_ARM_MS.
 */
void heat_arm(void);

/* Pick a level outright, 1..HEAT_LEVEL_MAX. Ignored unless the buttons have
 * been handed over. Takes effect at once, and holds them for another
 * HEAT_ARM_REPICK_MS so the pick can be corrected.
 */
void heat_select_level(uint8_t want);

/* Take the bar down early. For the plain tap, where the bar is only a readout
 * and any other button means the reader has moved on. Does nothing while the
 * buttons are handed over, since there the bar is not a readout but a menu.
 */
void heat_bar_cancel(void);

/* Keep a bar that is already up for at least `ms` longer.
 *
 * Called when HEAT goes down, with the hold threshold, so the readout cannot
 * time out in the gap between the press and knowing whether it was a hold.
 * Without it, starting to hold late in the readout makes the bar slide away and
 * then come back, which looks like the chair losing its place.
 */
void heat_bar_defer(uint32_t ms);

/* True while the level buttons are standing in for levels. control.c uses this
 * to route those presses here instead of to the motors.
 */
int heat_armed(void);

/* 0 off, 1..HEAT_LEVEL_MAX. */
int heat_level(void);

/* Which bar lamps should be lit, bit 0 being the bottom one. A mask rather than
 * a level because the bar animates, and a bar sliding off the top is not a
 * level any more. 0 for nothing lit.
 */
uint8_t heat_bar_mask(void);

#endif /* ENH_HEAT_LEVELS */

/* HEAT pressed.
 *
 * Reference: toggle, refused while a motor is moving per the power budget.
 * With levels: accepts the level and closes the choice if one is open,
 * otherwise toggles. So the press means "yes, that one" when there is a
 * question on the table, and "off" when there is not.
 */
void heat_press(void);

/* What the HEAT lamp should show while heat is on: blinking while a level is
 * being chosen, then steady.
 */
int heat_led(void);

/* Call every loop: drives the output, applies the auto-off and the duty cycle,
 * advances the bar, closes a choice that has run out of time, and cuts the
 * element while a motor runs.
 */
void heat_update(void);

void heat_off(void);
int  heat_is_on(void);

#endif /* HEAT_H */
