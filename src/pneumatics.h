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

/* Handle a MASSAGE or LUMBAR press edge. The caller is responsible for the
 * POWER gate.
 */
void pneumatics_button(uint8_t button);

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
