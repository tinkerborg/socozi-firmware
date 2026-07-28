/* The all-off-and-flatten macro.
 *
 * Drives both motion axes to their down position and then switches every
 * comfort function off. Reproduces the factory firmware's hidden POWER-hold
 * behaviour, which the handset does not advertise; see docs/firmware-spec.md §8.
 *
 * Two ways in, one behaviour:
 *
 *   held        POWER held past the threshold. Drives while it stays down, and
 *               finishes on release. This is the factory's.
 *   unattended  ENH_POWER_DOUBLE_TAP. Nothing is held, so the macro keeps its
 *               own motion alive and finishes when the motion ends, at the
 *               stops or on the timeout ceiling.
 *
 * The shutdown is deliberately at the *end*, not the start: while the chair
 * drives, lumbar stays inflated, heat stays on, and massage keeps its LED even
 * though the motion pause stops its pump. Everything goes out together when the
 * macro ends, and the vent follows.
 */

#ifndef MACROS_FLATTEN_H
#define MACROS_FLATTEN_H

#include <stdint.h>

/* POWER held past the threshold. */
void flatten_hold_start(void);

/* POWER released after a hold. Runs the shutdown. */
void flatten_hold_end(void);

/* Double tap. Runs unattended, and ends itself. */
void flatten_tap_start(void);

/* Call once per loop, before the motion request is issued, with the current
 * button code. Returns the motion the macro wants, or MOTION_NONE.
 *
 * Any button cancels an unattended run, matching the rule that any other button
 * interrupts the held one. A cancel aborts and does *not* run the shutdown.
 */
uint32_t flatten_update(uint8_t button);

/* POWER LED override while running unattended: 1 or 0 to force the lamp, or -1
 * for no opinion. The gate is still on during the move, so without this the
 * lamp would sit steady and an unattended move would look like a stuck button.
 */
int flatten_led_power(void);

#endif /* MACROS_FLATTEN_H */
