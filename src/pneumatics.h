/* Pneumatics: three bladders, one pump, one shared exhaust.
 *
 * Valve bits, confirmed on the chair:
 *
 *   0x01  top bladder
 *   0x02  middle bladder
 *   0x04  exhaust (shared)
 *   0x08  bottom bladder
 *
 * A cell inflates with its bit set and the pump running. Closing its valve
 * traps the air, cells do not self-vent. Venting opens the exhaust alone;
 * adding a cell bit turns the pump on and inflates instead.
 */

#ifndef PNEUMATICS_H
#define PNEUMATICS_H

#include <stdint.h>

#include "enhancements.h"

/* Handle a MASSAGE or LUMBAR press edge. The caller is responsible for the
 * POWER gate.
 */
void pneumatics_button(uint8_t button);

#if ENH_LUMBAR_HOLD_SET

/* LUMBAR released, however long it was down for.
 *
 * The press starts inflating; this decides what that press meant. Held, and it
 * stops here and remembers the firmness. Tapped, and it keeps going to the
 * firmness already remembered. See enhancements-spec.md §2.5.
 *
 * Must be called for a release after a hold as well as after a tap, since the
 * two arrive as different events.
 */
void pneumatics_lumbar_release(void);

/* Inflate time in 100 ms units, 0 when nothing is stored. */
uint8_t pneumatics_lumbar_level(void);

#endif

#if ENH_MASSAGE_LEVELS

/* MASSAGE held. Switches massage on if it was off, and opens the adjuster on
 * its intensity. See enhancements-spec.md §2.6.
 */
void pneumatics_massage_arm(void);

/* An intensity was picked while massage owned the adjuster. */
void pneumatics_massage_set_level(uint8_t want);

/* 1..ADJUST_LEVEL_MAX. Never 0: intensity is a property of the pattern, not a
 * thing that is switched off with it, so it reads the same whether massage is
 * running or not.
 */
int pneumatics_massage_level(void);

#endif

/* Stop everything and start the vent. Called when POWER is switched off. */
void pneumatics_shutdown(void);

/* Call every loop. Advances the massage pattern or lumbar state machine, and
 * pauses everything while a motor is running.
 */
void pneumatics_update(void);

/* State, for driving the handset LEDs. */
int pneumatics_massage_on(void);
int pneumatics_lumbar_lit(void);

#endif /* PNEUMATICS_H */
