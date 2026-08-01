/* Recline and headrest.
 *
 * Recline needs both pins of a pair energised; neither does anything alone.
 * The board has a third motion axis (PB11/PB14) that is not connected on this
 * chair, so it is not exposed here.
 *
 * There is no position feedback anywhere in this hardware. Motion is a
 * duration, never a target.
 */

#ifndef MOTION_H
#define MOTION_H

#include <stdint.h>

#include "enhancements.h"

/* --- Tunables ---
 *
 * Relay sequencing. The factory firmware never energises a motor at the moment
 * it selects a direction: it sets the direction pin, waits, and only then
 * closes the enable. That guarantees the direction relay has settled before any
 * current flows, so polarity never switches under load, which is what keeps
 * relay contacts from arcing.
 *
 * The factory values, from motion_control_tick's per-axis tick counter at
 * 100 ms per tick: enable at counter > 5, and for recline the second pin of the
 * pair one tick after the first.
 *
 * 600 ms is a long time to wait for a chair to start moving. It can be reduced
 * once we've confirmed the relays settle faster than that. The protection is
 * in the ordering, not the duration.
 */
#define MOTION_SETTLE_MS  600   /* direction set → enable closed */
#define MOTION_STAGGER_MS 100   /* recline: first pin → second pin */

/* Hard ceiling on any single continuous motion, as a backstop. */
#define MOTION_TIMEOUT_MS 30000

/* --- Stall detection ---
 *
 * Measured on this chair: running current reads 35–173, and inrush hits
 * 359–403 for a single 20 ms sample. A stalled rotor is electrically the same
 * as inrush, so it sits at that level continuously.
 *
 * The threshold is the factory's, and it sits sensibly between the two. The
 * hold time exists to ride out inrush, the factory uses 40 of its 100 ms
 * ticks, which is very conservative given inrush lasts one sample.
 *
 * Note this only catches an *obstruction*. Both ends of travel have internal
 * limit switches that open the circuit, so at the stops current drops to zero
 * rather than rising.
 */
#define MOTION_STALL_ADC     0x155   /* 341 */
#define MOTION_STALL_MS      4000

/* --- End of travel, ENH_END_OF_TRAVEL_STOP ---
 *
 * The mirror of stall detection. Both ends of travel open an internal limit
 * switch, so the motor disconnects itself and current falls to zero rather
 * than rising. With both axes on one sense channel, zero means *every* motor
 * has reached its stop, which is exactly the "arrived" condition.
 *
 * Two guards, because zero is also what the channel reads when nothing is
 * driving:
 *
 * MOTION_ARM_MS   from the first contact closing. Covers MOTION_STAGGER_MS
 *                 before the recline pair completes, plus the inrush spike,
 *                 measured at a single 20 ms sample.
 * MOTION_ARRIVED_MS  how long zero must hold. One dropped sample must not stop
 *                 a motion mid-travel.
 *
 * A failed conversion (0xFFFFFFFF) is not zero and resets the timer, so ADC
 * trouble falls back to MOTION_TIMEOUT_MS rather than stopping early.
 */
#define MOTION_ARM_MS        500
#define MOTION_ARRIVED_MS    300

#if ENH_POSITION_TRACK

/* --- Position, ENH_POSITION_TRACK ---
 *
 * Dead reckoning, and nothing better is available: there is no encoder, no pot,
 * and no feedback at all except the current-zero at the stops. A position is
 * how many milliseconds of upward travel an axis is above its down stop.
 *
 * It drifts, and the drift is not symmetric — gravity and body weight assist
 * one direction and oppose the other, so a second down and a second up are not
 * the same distance. Reaching the down stop re-zeroes the estimate, which is
 * the only correction there is. See enhancements-spec.md §2.7.
 *
 * MEASURE THESE ON THE CHAIR. They are the stop-to-stop travel time per axis
 * and they are currently guesses; everything scales off them.
 */
#define MOTION_TRAVEL_RECLINE_MS  26000u
#define MOTION_TRAVEL_HEADREST_MS 12000u

/* Close enough to the down stop to call it home, since the estimate will never
 * land exactly on zero.
 */
#define MOTION_HOME_MS 1500u

/* How close a restore has to get before it stops. Tighter than this and the
 * relays chatter on the last few hundred milliseconds for no visible gain.
 */
#define MOTION_SEEK_MS 400u

uint32_t motion_pos_recline(void);
uint32_t motion_pos_headrest(void);

/* Both axes within MOTION_HOME_MS of their down stops. */
int motion_at_home(void);

#endif /* ENH_POSITION_TRACK */

/* Values match dbg.motion / MOTION_* in debug.h. */

/* Ask for a motion. Safe to call every loop with the same value. Requesting
 * MOTION_NONE, or a different motion, stops immediately and restarts the
 * sequence.
 */
void motion_request(uint32_t want);

/* Call every loop with the latest current-sense reading. Advances the relay
 * sequence, and enforces both the timeout and stall detection.
 */
void motion_update(uint32_t current);

void motion_stop(void);

/* What has been requested, set immediately on press, so the handset LED
 * lights without waiting out the settle time.
 */
uint32_t motion_active(void);

#endif /* MOTION_H */
