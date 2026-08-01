#include "control.h"

#include "adjust.h"
#include "button.h"
#include "debug.h"
#include "enhancements.h"
#include "handset.h"
#include "heat.h"
#include "macros/flatten.h"
#include "macros/preset.h"
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

/* What it takes to be a thing with an adjustable level.
 *
 * Everything about *choosing* a level is in adjust.c and is shared. This is the
 * rest: which button opens it, which lamp reports it, and how to hand a level
 * to whoever the level belongs to. Two entries today, and the routing below
 * knows only that there is a list.
 */
struct adjuster {
    uint8_t id;                     /* ADJUST_* */
    uint8_t button;                 /* the hold that opens it */
    uint8_t lamp;                   /* LED_*, blinks while it is open */
    void  (*arm)(void);
    void  (*set_level)(uint8_t);
    int   (*is_on)(void);
};

static const struct adjuster adjusters[] = {
#if ENH_HEAT_LEVELS
    { ADJUST_HEAT, HS_HEAT, LED_HEAT,
      heat_arm, heat_set_level, heat_is_on },
#endif
#if ENH_MASSAGE_LEVELS
    { ADJUST_MASSAGE, HS_MASSAGE, LED_MASSAGE,
      pneumatics_massage_arm, pneumatics_massage_set_level,
      pneumatics_massage_on },
#endif
};

#define ADJUSTERS ((int)(sizeof(adjusters) / sizeof(adjusters[0])))

/* Whose hold this button is, or nothing. A button that owns an adjuster acts on
 * release, so that a tap can be told apart from the hold.
 */
static const struct adjuster *adjuster_for(uint8_t button)
{
    for (int i = 0; i < ADJUSTERS; i++) {
        if (adjusters[i].button == button) {
            return &adjusters[i];
        }
    }

    return 0;
}

/* Whoever currently has the bar and the borrowed buttons. */
static const struct adjuster *adjuster_owner(void)
{
    for (int i = 0; i < ADJUSTERS; i++) {
        if (adjusters[i].id == adjust_owner()) {
            return &adjusters[i];
        }
    }

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

#if ADJUST_IN_USE
    /* One rule for every adjustable thing: blinking while its level is being
     * chosen, steady the rest of the time.
     *
     * Blinking outranks whether the thing is running, because a level can be
     * adjusted while it is off — and then the lamp is the only thing saying the
     * motion buttons have been borrowed.
     */
    for (int i = 0; i < ADJUSTERS; i++) {
        const struct adjuster *a = &adjusters[i];
        int lit = a->is_on();

        if (adjust_armed() && adjust_owner() == a->id) {
            lit = adjust_blink();
        }

        if (lit)                  bits |= a->lamp;
    }
#else
    if (heat_is_on())             bits |= LED_HEAT;
    if (pneumatics_massage_on())  bits |= LED_MASSAGE;
#endif

#if ENH_PRESET
    /* The slot that just took a preset, flashing back so you can see which one
     * it was. Saving moves nothing, so this is the only evidence it happened.
     */
    {
        uint8_t mask = preset_lamp_mask();

        if (mask && motion_active() == MOTION_NONE) {
            for (int i = 0; i < PRESET_SLOTS; i++) {
                if (mask & (1u << i)) {
                    bits |= bar_lamp[i];
                }
            }
        }
    }
#endif

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

    /* A running macro can override the lamp: an unattended move should look
     * deliberate rather than like a stuck button.
     */
    if (flatten_led_power() >= 0) {
        power_led = flatten_led_power();
    }

#if ENH_PRESET
    if (preset_led_power() >= 0) {
        power_led = preset_led_power();
    }
#endif

    if (power_led)                bits |= LED_POWER;

    handset_set_leds(bits);
}

#if BUTTONS_BORROWED

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

#endif

#if ADJUST_IN_USE

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
    const struct adjuster *a = adjuster_owner();

    if (!a) {
        return;
    }

    a->set_level(want);
    adjust_pick(want);
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
#if ENH_PRESET
        /* Any button stops a recall, and stopping it is all that press does.
         * Reaching for a control to halt the chair should not also switch that
         * control on, or drive the motor the button belongs to.
         *
         * preset_update() sees the same press and does the cancelling; this is
         * only here to swallow the press before anything else acts on it.
         */
        if (preset_moving()) {
            consume_motion(ev.button);
            dbg.presses++;
            break;
        }

        /* A POWER hold turns the four motion buttons into the four preset
         * slots for a few seconds. The press only claims the button; what it
         * means is decided on release, because a tap saves and a hold clears.
         * consume_motion() keeps it off the motor until it comes up.
         */
        if (preset_armed() && level_button(ev.button) > 0) {
            consume_motion(ev.button);
            dbg.presses++;
            break;
        }
#endif
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
        {
            const struct adjuster *a = adjuster_owner();

            if (a && ev.button == a->button) {
                adjust_defer(BUTTON_SHORT_HOLD_MS);
            } else {
                adjust_dismiss();
            }
        }

        /* A button that owns an adjuster acts on release, so a tap can be told
         * apart from the hold that opens the level buttons. POWER already works
         * this way.
         */
        if (adjuster_for(ev.button)) {
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
#if ENH_PRESET
        /* POWER opened the store window, so POWER backs out of it — and does
         * not also toggle the gate on the way. On the release, like every other
         * POWER gesture, so a hold can still re-arm.
         */
        if (ev.button == HS_POWER && preset_armed()) {
            preset_cancel();
            break;
        }

        /* Released before the hold threshold, so this was a tap: save. The
         * matching hold clears instead, in BTN_HOLD below.
         */
        if (preset_armed()) {
            int which = level_button(ev.button);

            if (which > 0) {
                preset_save((uint8_t)(which - 1));
                break;
            }
        }

        /* Double tap on a motion button goes to that slot. A single tap and a
         * hold both still mean the motor, so nothing is taken away: at a tap's
         * timescale the settle has not finished and nothing has moved anyway.
         *
         * Not behind the POWER gate. Recalling a preset is how you start using
         * the chair, so requiring the chair to already be on would be backwards
         * — the recall switches it on itself.
         */
        if (ev.kind == BTN_DOUBLE_TAP) {
            int which = level_button(ev.button);

            if (which > 0) {
                dbg.presses++;
                preset_recall((uint8_t)(which - 1));
                break;
            }
        }
#endif
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
        if (adjuster_for(ev.button)) {
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
#if ENH_PRESET
        /* Held rather than tapped, because clearing throws something away and
         * a tap already means save.
         */
        if (preset_armed()) {
            int which = level_button(ev.button);

            if (which > 0) {
                preset_clear((uint8_t)(which - 1));
                break;
            }
        }
#endif
        if (ev.button == HS_POWER) {
            dbg.presses++;
#if ENH_PRESET
            /* Replaces the factory's hold-to-flatten. Nothing moves; the
             * motion buttons become preset slots until the window closes, and
             * the release below has nothing to do.
             */
            preset_arm();
#else
            flatten_hold_start();
#endif
        }
#if ADJUST_IN_USE
        /* A held HEAT or MASSAGE asks to choose a level. It changes only the
         * level: whether the thing is running is left exactly as it was.
         * Behind the same POWER gate a press is.
         */
        {
            const struct adjuster *a = adjuster_for(ev.button);

            if (a && power_is_on()) {
                dbg.presses++;
                a->arm();
            }
        }
#endif
        break;

    case BTN_HOLD_RELEASE:
#if !ENH_PRESET
        if (ev.button == HS_POWER) {
            flatten_hold_end();
        }
#endif
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

#if BUTTONS_BORROWED
    /* A button taken for something else stays taken until it comes up, or the
     * motor would start the instant its other job finished.
     */
    if (consumed != HS_NONE) {
        if (button == consumed) {
            want = MOTION_NONE;
        } else {
            consumed = HS_NONE;
        }
    }
#endif

    /* A macro outranks the button map: it is what keeps an unattended move
     * alive when nothing is holding a button.
     */
    macro = flatten_update(button);
    if (macro != MOTION_NONE) {
        want = macro;
    }

#if ENH_PRESET
    macro = preset_update(button);
    if (macro != MOTION_NONE) {
        want = macro;
    }
#endif

    motion_request(want);
    motion_update(dbg.adc);

    led_update();
}
