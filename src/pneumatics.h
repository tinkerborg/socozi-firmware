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

/* The massage engine's tick, and the factory table's ceiling on a single
 * lumbar inflation. In the header because the lumbar level is stored and
 * reported in 100 ms units, so anything turning that into a percentage needs
 * both — the ESP32 bridge among them.
 */
#define MASSAGE_TICK_MS 100

#define LUMBAR_INFLATE_MAX_MS (200 * MASSAGE_TICK_MS)

/* Massage auto-off. The factory sets a countdown of 15 and decrements it once
 * per minute, clearing the massage bit when it runs out.
 */
#define MASSAGE_TIMEOUT_MS (15u * 60u * 1000u)

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

/* --- state, rather than gestures ---
 *
 * The calls above are button handlers: they toggle, they cycle, they depend on
 * what the last press did. A preset recall does not have a finger, it has a
 * chair it wants to arrive at, so it needs to say what should be true rather
 * than mime the press that would have made it true.
 *
 * The button handlers are the thin layer; these are what they act on.
 */

/* Inflate and hold. `tenths` is the firmness to go to in 100 ms units, or 0 to
 * use the remembered one. Idempotent: asking while it is already up does
 * nothing, since air only leaves through the exhaust.
 *
 * Arriving this way does not become the remembered firmness. A preset carries
 * its own, and must not overwrite what you last chose by hand.
 */
void pneumatics_lumbar_set(int on, uint8_t tenths);

/* What is in the cell right now in 100 ms units, which is not the same as what
 * is remembered.
 */
uint8_t pneumatics_lumbar_current(void);

/* Run or stop the massage pattern, at its remembered intensity. */
void pneumatics_massage_set(int on);

#endif

#if ENH_MASSAGE_LEVELS

/* MASSAGE held. Switches massage on if it was off, and opens the adjuster on
 * its intensity. See enhancements-spec.md §2.6.
 */
void pneumatics_massage_arm(void);

/* An intensity was picked while massage owned the adjuster. Runs at it and
 * remembers it, because a deliberate choice is what the memory is for.
 */
void pneumatics_massage_set_level(uint8_t want);

/* Run at this intensity without remembering it, for a preset recall. */
void pneumatics_massage_use_level(uint8_t want);

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
