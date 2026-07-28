#include "control.h"

#include "adjust.h"
#include "button.h"
#include "debug.h"
#include "enhancements.h"
#include "handset.h"
#include "heat.h"
#include "macros/flatten.h"
#include "motion.h"
#include "pneumatics.h"
#include "power.h"
#include "settings.h"
#include "timing.h"

/* Stop a motion if the handset goes quiet, a disconnected or dead remote must
 * not leave a motor running. Poll replies arrive continuously, so a gap this
 * long means something is wrong.
 */
#define HANDSET_TIMEOUT_MS 250

#if ADJUST_IN_USE
/* The four motion lamps in bar order, bottom first. Shared by the bar graph and
 * by level_button(), which has to agree with it: pressing the button next to a
 * segment must ask for that many segments.
 */
static const uint8_t bar_lamp[ADJUST_LEVEL_MAX] = {
    LED_HEADREST_DOWN, LED_HEADREST_UP, LED_RECLINE_DOWN, LED_RECLINE_UP
};

/* Buttons whose hold opens an adjuster, and which therefore act on release. */
static int adjust_hold_button(uint8_t button)
{
#if ENH_HEAT_LEVELS
    if (button == HS_HEAT) {
        return 1;
    }
#endif
#if ENH_MASSAGE_LEVELS
    if (button == HS_MASSAGE) {
        return 1;
    }
#endif
    (void)button;
    return 0;
}
#endif /* ADJUST_IN_USE */

/* Mirror what we're actually driving back to the handset LEDs rather than
 * echoing the button: if a safety stop cuts a motion, the light goes out even
 * with the button still held.
 */
static void led_update(void)
{
    uint8_t bits = 0;
    int     power_led;

    switch (motion_active()) {
    case MOTION_RECLINE_UP:    bits |= LED_RECLINE_UP;    break;
    case MOTION_RECLINE_DOWN:  bits |= LED_RECLINE_DOWN;  break;
    case MOTION_HEADREST_UP:   bits |= LED_HEADREST_UP;   break;
    case MOTION_HEADREST_DOWN: bits |= LED_HEADREST_DOWN; break;

    case MOTION_FLATTEN:
        bits |= LED_RECLINE_DOWN | LED_HEADREST_DOWN;
        break;

    default: break;
    }

    if (pneumatics_lumbar_lit())  bits |= LED_LUMBAR;
    /* Not heat_is_on() or massage_on() alone: a lamp blinks while its own level
     * is being chosen, which is what says the motion buttons are borrowed.
     */
    if (heat_led())               bits |= LED_HEAT;

    if (pneumatics_massage_on()) {
        int lit = 1;

#if ENH_MASSAGE_LEVELS
        if (adjust_armed() && adjust_owner() == ADJUST_MASSAGE) {
            lit = adjust_blink();
        }
#endif
        if (lit)                  bits |= LED_MASSAGE;
    }

#if ADJUST_IN_USE
    /* Whatever is being adjusted, as a bar graph up the four motion lamps, so
     * the level is visible under your finger rather than only afterwards.
     * Bottom to top, which on the handset is headrest down first.
     *
     * Only when nothing is moving. Those lamps mirror the motors, and a motion
     * saying what it is doing outranks a level saying what it is about to be.
     */
    {
        uint8_t mask = adjust_bar_mask();

        if (mask && motion_active() == MOTION_NONE) {
            for (int i = 0; i < ADJUST_LEVEL_MAX; i++) {
                if (mask & (1u << i)) {
                    bits |= bar_lamp[i];
                }
            }
        }
    }
#endif

    power_led = power_is_on();

    /* A running macro can override the lamp, see flatten_led_power(). */
    if (flatten_led_power() >= 0) {
        power_led = flatten_led_power();
    }

    if (power_led)                bits |= LED_POWER;

    handset_set_leds(bits);
}

#if ADJUST_IN_USE

/* A button whose press was taken for something other than its own job, and
 * which must stay taken until it is let go. Without the latch the press picks a
 * level, the window closes on that same press, and the motor starts the moment
 * the level lands, with the button still down.
 */
static uint8_t consumed;

static void consume_motion(uint8_t button)
{
    consumed = button;
}

/* The four motion buttons in bar order, bottom lamp first, so pressing the one
 * next to a bar segment asks for that many segments.
 */
static int level_button(uint8_t button)
{
    switch (button) {
    case HS_HEADREST_DOWN: return 1;
    case HS_HEADREST_UP:   return 2;
    case HS_RECLINE_DOWN:  return 3;
    case HS_RECLINE_UP:    return 4;
    default:               return 0;
    }
}

/* Send a pick to whoever asked for it. The adjuster knows how many lamps to
 * light and nothing about what a level means; this is the other half.
 */
static void deliver_level(uint8_t want)
{
    switch (adjust_owner()) {
#if ENH_HEAT_LEVELS
    case ADJUST_HEAT:
        heat_set_level(want);
        break;
#endif
#if ENH_MASSAGE_LEVELS
    case ADJUST_MASSAGE:
        pneumatics_massage_set_level(want);
        break;
#endif
    default:
        return;
    }

    adjust_pick(want);
}

/* The button that owns the adjuster, so a press on it means "yes, that one"
 * rather than "take the bar down".
 */
static uint8_t adjust_owner_button(void)
{
    switch (adjust_owner()) {
#if ENH_HEAT_LEVELS
    case ADJUST_HEAT:    return HS_HEAT;
#endif
#if ENH_MASSAGE_LEVELS
    case ADJUST_MASSAGE: return HS_MASSAGE;
#endif
    default:             return HS_NONE;
    }
}

#endif /* ADJUST_IN_USE */

/* Short POWER press: toggle the gate. Acts on release, so it can be told apart
 * from a long press.
 */
static void power_short_press(void)
{
    if (!power_toggle()) {
        power_comfort_off();
    }
}

/* Route a comfort-function press. Everything here is behind the POWER gate;
 * POWER itself is handled separately because it depends on hold duration.
 */
static void comfort_button(uint8_t button)
{
    if (!power_is_on()) {
        return;
    }

    switch (button) {
    case HS_HEAT:
        heat_press();
        break;

    case HS_MASSAGE:
    case HS_LUMBAR:
        pneumatics_button(button);
        break;

    default:
        break;
    }
}

/* Comfort functions act on the press edge; motion acts while held. POWER is the
 * exception, it acts on release, so a short press can be told apart from the
 * long-press macro.
 */
static void route_event(struct button_event ev)
{
    switch (ev.kind) {
    case BTN_PRESS:
#if ADJUST_IN_USE
        /* While the buttons are borrowed the four motion buttons are level
         * buttons. The press is consumed here: it must not reach the motors,
         * and consume_motion() keeps it off them for as long as it stays down.
         */
        if (adjust_armed()) {
            int pick = level_button(ev.button);

            if (pick > 0) {
                deliver_level((uint8_t)pick);
                consume_motion(ev.button);
                dbg.presses++;
                break;
            }
        }

        /* Anything that is neither a level nor the owner's own button means the
         * user has moved on, so the bar comes down. Not the owner's button,
         * though: that press may be the start of a hold, and taking the bar
         * down under it would make asking to adjust look like a mis-press.
         */
        if (ev.button == adjust_owner_button()) {
            adjust_defer(BUTTON_ADJUST_HOLD_MS);
        } else {
            adjust_dismiss();
        }

        /* HEAT and MASSAGE act on release, so a tap can be told apart from the
         * hold that opens the level buttons. POWER already works this way.
         */
        if (adjust_hold_button(ev.button)) {
            break;
        }
#endif
        if (ev.button != HS_POWER) {
            dbg.presses++;
            comfort_button(ev.button);
        }
        break;

    case BTN_TAP:
    case BTN_DOUBLE_TAP:
#if ENH_LUMBAR_HOLD_SET
        /* The press started inflating; the release says what it meant. Also
         * reached via BTN_HOLD_RELEASE below, since a long enough hold arrives
         * as that instead.
         */
        if (ev.button == HS_LUMBAR && power_is_on()) {
            pneumatics_lumbar_release();
            break;
        }
#endif
#if ADJUST_IN_USE
        /* The toggle, deferred to the release so the hold can mean something
         * else. Both taps of a double tap toggle, which is what two presses
         * have always done.
         */
        if (adjust_hold_button(ev.button)) {
            dbg.presses++;
            comfort_button(ev.button);
            break;
        }
#endif
        if (ev.button != HS_POWER) {
            break;
        }
        dbg.presses++;
#if ENH_POWER_DOUBLE_TAP
        if (ev.kind == BTN_DOUBLE_TAP) {
            /* The first tap has already run its ordinary toggle by the time a
             * second one is known to be coming. That washes out: the macro ends
             * with everything off whichever way the toggle went, so nothing has
             * to be deferred and a single press keeps its instant response.
             */
            flatten_tap_start();
            break;
        }
#endif
        power_short_press();
        break;

    case BTN_HOLD:
        if (ev.button == HS_POWER) {
            dbg.presses++;
            flatten_hold_start();
        }
        /* A held HEAT or MASSAGE asks to choose a level: on if it was off,
         * straight to the choice if it was already on. Behind the same POWER
         * gate a press is.
         */
#if ENH_HEAT_LEVELS
        if (ev.button == HS_HEAT && power_is_on()) {
            dbg.presses++;
            heat_arm();
        }
#endif
#if ENH_MASSAGE_LEVELS
        if (ev.button == HS_MASSAGE && power_is_on()) {
            dbg.presses++;
            pneumatics_massage_arm();
        }
#endif
        break;

    case BTN_HOLD_RELEASE:
        if (ev.button == HS_POWER) {
            flatten_hold_end();
        }
#if ENH_LUMBAR_HOLD_SET
        if (ev.button == HS_LUMBAR && power_is_on()) {
            pneumatics_lumbar_release();
        }
#endif
        break;

    default:
        break;
    }
}

static uint32_t motion_for(uint8_t button)
{
    switch (button) {
    case HS_RECLINE_UP:    return MOTION_RECLINE_UP;
    case HS_RECLINE_DOWN:  return MOTION_RECLINE_DOWN;
    case HS_HEADREST_UP:   return MOTION_HEADREST_UP;
    case HS_HEADREST_DOWN: return MOTION_HEADREST_DOWN;
    default:               return MOTION_NONE;
    }
}

void control_update(void)
{
    uint8_t  button = handset_button();
    uint32_t want;
    uint32_t macro;

    /* Treat a silent handset as "nothing pressed". */
    if (handset_age_ms() > HANDSET_TIMEOUT_MS) {
        button = HS_NONE;
    }

    route_event(button_update(button));

    pneumatics_update();
    heat_update();

#if ADJUST_IN_USE
    adjust_update();
#endif

    /* Commit a changed setting only when stalling the loop for a flash write is
     * free: nothing moving, and no level being chosen. Deferring while armed is
     * what makes one adjustment cost one record instead of one per press.
     */
    settings_update(motion_active() == MOTION_NONE
#if ADJUST_IN_USE
                    && !adjust_armed()
#endif
                    );

    want = motion_for(button);

#if ADJUST_IN_USE
    /* A button that picked a level stays picked until it comes up, or the motor
     * would start the instant the level landed.
     */
    if (consumed != HS_NONE) {
        if (button == consumed) {
            want = MOTION_NONE;
        } else {
            consumed = HS_NONE;
        }
    }
#endif

    /* A macro outranks the button map: it is what keeps the flatten alive while
     * POWER is held, and what renews it when nothing is.
     */
    macro = flatten_update(button);
    if (macro != MOTION_NONE) {
        want = macro;
    }

    motion_request(want);
    motion_update(dbg.adc);

    led_update();
}
