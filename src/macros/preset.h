/* Four saved chairs, one per motion button.
 *
 * A preset is both axis positions plus which comfort functions were running.
 * The four motion buttons are the four slots, in the same bottom-to-top order
 * the bar graph uses: headrest ▼, headrest ▲, recline ▼, recline ▲.
 *
 * Three gestures, and none of them takes a button away from its usual job:
 *
 *   POWER held          arm for PRESET_ARM_MS. The POWER lamp blinks to say
 *                       the next motion button pressed will be written to,
 *                       rather than moving anything.
 *   motion button       while armed, saves this chair into that slot. The
 *                       POWER lamp stops, and that button's own lamp flashes
 *                       back so you can see which slot took it.
 *   motion double tap   recalls that slot. A single press still drives its
 *                       motor, and a hold still drives it for as long as it is
 *                       held; only a double tap means "go there".
 *
 * POWER double tap is untouched and still means everything off and flat.
 *
 * Recall drives unattended, one axis at a time. Both axes share the current
 * sense channel, so running them together would make an arrival ambiguous —
 * zero current would mean "something stopped", not "this one did".
 */

#ifndef MACROS_PRESET_H
#define MACROS_PRESET_H

#include <stdint.h>

#include "../adjust.h"
#include "../enhancements.h"
#include "../settings.h"

#if ENH_PRESET

#define PRESET_SLOTS SETTINGS_PRESETS

/* Positions are stored a byte each, so they are kept in 200 ms units: the
 * recline axis travels ~26 s, which is 130 steps, and 100 ms units would not
 * fit. The resolution is far finer than the estimate is accurate anyway.
 */
#define PRESET_STEP_MS 200u

/* Which comfort functions were running when the preset was taken. Levels are
 * not stored here — heat, lumbar and massage each remember their own, so a
 * preset only has to say whether they were on.
 */
#define PRESET_FLAG_HEAT    (1u << 0)
#define PRESET_FLAG_MASSAGE (1u << 1)
#define PRESET_FLAG_LUMBAR  (1u << 2)

/* How long the motion buttons stand in for preset slots after a POWER hold,
 * and how fast the POWER lamp blinks to say so.
 */
#define PRESET_ARM_MS       8000u
#define PRESET_ARM_BLINK_MS 500u

/* Saving moves nothing, so the lamp is the only evidence it happened. The slot
 * that took it flashes and then goes back to whatever it was showing — the
 * gesture is an event, not a mode.
 *
 * Same flash as a level pick, from ADJUST_CONFIRM_* in adjust.h: both mean
 * "that landed", so they should not look different.
 */

/* POWER held: hand the motion buttons over to the slots. */
void preset_arm(void);

/* True while they are handed over, so control.c routes those presses here
 * instead of to the motors.
 */
int preset_armed(void);

/* Save this chair into `slot`, ending the armed window. */
void preset_save(uint8_t slot);

/* Forget `slot`, so its button goes back to blinking as free. Held rather than
 * tapped, because it throws something away and a tap already means save.
 */
void preset_clear(uint8_t slot);

/* Back out without writing anything. POWER opened the window, so POWER closes
 * it: the alternative is waiting out PRESET_ARM_MS with four buttons that do
 * not do their usual job.
 */
void preset_cancel(void);

/* Go to `slot`. Does nothing while armed — there the press means save. */
void preset_recall(uint8_t slot);

/* True while a recall is moving the chair. Any button cancels one, and the
 * button that does the canceling does nothing else — reaching for a control
 * to stop the chair should not also switch that control on.
 */
int preset_moving(void);

/* Call once per loop, before the motion request is issued, with the current
 * button code. Returns the motion the macro wants, or MOTION_NONE.
 *
 * Any button cancels a recall, matching the flatten macro. A cancel stops where
 * it is and does not restore anything further.
 */
uint32_t preset_update(uint8_t button);

/* What the POWER lamp should show, or -1 to leave it alone. */
int preset_led_power(void);

/* Which slot lamps to flash, as a bitmask over the four motion buttons in slot
 * order, or 0 for none. control.c maps it to actual lamps.
 */
uint8_t preset_lamp_mask(void);

#endif /* ENH_PRESET */

#endif /* MACROS_PRESET_H */
