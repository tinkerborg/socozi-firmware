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
 * nearly every time, and asks nothing. Holding it opens the adjuster, which is
 * adjust.c and shared with massage intensity.
 */

#ifndef HEAT_H
#define HEAT_H

#include <stdint.h>

#include "enhancements.h"

#if ENH_HEAT_LEVELS

#include "adjust.h"

/* Level 4 is 100% duty, which is continuous, and so exactly what the reference
 * build does. The enhancement only ever removes heat.
 */
#define HEAT_LEVEL_MAX ADJUST_LEVEL_MAX

/* Where an untouched chair starts, so first use matches the factory. */
#define HEAT_LEVEL_DEFAULT HEAT_LEVEL_MAX

/* One switching cycle. Slow on purpose: the element averages it without help,
 * and fast switching would put noise on a board whose handset cable already
 * collects USART errors.
 */
#define HEAT_DUTY_PERIOD_MS 20000u

/* HEAT held. Switches on at the remembered level if it was off, and opens the
 * adjuster on it.
 */
void heat_arm(void);

/* A level was picked while heat owned the adjuster. */
void heat_set_level(uint8_t want);

/* 0 off, 1..HEAT_LEVEL_MAX. */
int heat_level(void);

#endif /* ENH_HEAT_LEVELS */

/* HEAT pressed.
 *
 * Reference: toggle, refused while a motor is moving per the power budget.
 * With levels: accepts the level and closes the adjuster if heat has it open,
 * otherwise toggles. So the press means "yes, that one" when there is a
 * question on the table, and "off" when there is not.
 */
void heat_press(void);

/* What the HEAT lamp should show while heat is on: blinking while a level is
 * being chosen, then steady.
 */
int heat_led(void);

/* Call every loop: drives the output, applies the auto-off and the duty cycle,
 * and cuts the element while a motor runs.
 */
void heat_update(void);

void heat_off(void);
int  heat_is_on(void);

#endif /* HEAT_H */
