/* Replacement firmware for the SoCozi recliner controller (GD32E23x).
 *
 * See docs/firmware-spec.md for what this is meant to do, and
 * docs/firmware-map.md for how the factory firmware it replaces works.
 *
 * Deliberately has NO watchdog, so halting in GDB is safe, unlike the factory
 * firmware, which resets within milliseconds of a halt.
 *
 * SAFETY: motion is protected by over-current detection (motion.c) backed by a
 * hard time ceiling, and the heater by a 60 minute auto-off. There is no
 * temperature sensor anywhere in this hardware, so that timer is the only thing
 * bounding a stuck-on heater. Don't remove these.
 */

#include "adc.h"
#include "enhancements.h"
#include "gd32e23x.h"
#include "gpio.h"
#include "handset.h"
#include "heat.h"
#include "motion.h"
#include "pneumatics.h"
#include "power.h"
#include "debug.h"
#include "timing.h"
#include "watchdog.h"

/* Stop a motion if the handset goes quiet, a disconnected or dead remote must
 * not leave a motor running. Poll replies arrive continuously, so a gap this
 * long means something is wrong.
 */
#define HANDSET_TIMEOUT_MS 250

/* How often to poll the handset. It never transmits unprompted. */
#define HANDSET_POLL_MS 20

/* Hold POWER this long to trigger the all-off-and-flatten macro.
 *
 * The factory threshold is 20 counts of its action 0xF0, whose rate is set by a
 * compiler-generated division we never decoded. Observed on the chair as about
 * two seconds, which is what this reproduces.
 */
#define POWER_LONG_MS 2000

/* Double-press POWER within this long to run the same macro without holding.
 * Long enough for a deliberate double tap, short enough that two separate
 * intentional power toggles don't trip it.
 */
#define POWER_DOUBLE_MS 400

#if ENH_POWER_DOUBLE_TAP
/* Half on, half off, so a full cycle is twice this. */
#define POWER_BLINK_MS 500

/* File scope rather than a static inside handset_update(), because led_update()
 * has to see it to blink POWER while the move runs.
 */
static int auto_flatten;        /* macro driving with nothing held */
#endif

volatile struct debug_block dbg __attribute__((section(".debug_block")));

/* Mirror what we're actually driving back to the handset LEDs rather than
 * echoing the button: if a safety stop cuts a motion, the light goes out even
 * with the button still held.
 */
static void led_update(void)
{
    uint8_t bits = 0;

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
    if (pneumatics_massage_on())  bits |= LED_MASSAGE;
    if (heat_is_on())             bits |= LED_HEAT;

    int power_led = power_is_on();

#if ENH_POWER_DOUBLE_TAP
    /* The macro switches the gate off, so POWER would otherwise just go dark.
     * Blinking it says the chair is moving under its own direction rather than
     * because something is stuck.
     */
    if (auto_flatten) {
        power_led = ((ms_ticks / POWER_BLINK_MS) & 1);
    }
#endif

    if (power_led)                bits |= LED_POWER;

    handset_set_leds(bits);
}

/* Everything off. Shared by the POWER-off path and the long-press macro. */
static void comfort_all_off(void)
{
    pneumatics_shutdown();
    heat_off();
}

/* Short POWER press: toggle the gate. Acts on release, so it can be told apart
 * from a long press.
 */
static void power_short_press(void)
{
    if (!power_toggle()) {
        comfort_all_off();
    }
}

/* Long POWER press: start driving the chair flat, and nothing else.
 *
 * Comfort functions keep running and their LEDs stay lit for the whole move;
 * the factory only switches them off once the macro ends. Observed on the
 * chair: lumbar stays inflated and heat stays on while it drives, massage keeps
 * its LED although the pause stops the pump, and everything goes out together
 * at the end, followed by the vent.
 *
 * ENH_POWER_DOUBLE_TAP reuses this verbatim; a double tap is a shortcut for the
 * hold, not a second behaviour.
 */
static void power_long_press(void)
{
    motion_request(MOTION_FLATTEN);
}

/* The other half: what the factory does when the macro ends. Everything off,
 * gate off, and pneumatics_shutdown() starts the vent. Reached on release of a
 * held POWER, and under ENH_POWER_DOUBLE_TAP also when an unattended move
 * reaches the stops.
 */
static void power_macro_finish(void)
{
    comfort_all_off();

    if (power_is_on()) {
        power_toggle();
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
        heat_button();
        break;

    case HS_MASSAGE:
    case HS_LUMBAR:
        pneumatics_button(button);
        break;

    default:
        break;
    }
}

static void handset_update(void)
{
    static uint8_t  prev_button;
    static uint32_t power_down_ms;
    static int      power_long_done;
#if ENH_POWER_DOUBLE_TAP
    static uint32_t power_up_ms;    /* when the last short press was released */
    static int      power_tapped;
#endif

    uint8_t  button = handset_button();
    uint32_t want   = MOTION_NONE;

    /* Treat a silent handset as "nothing pressed". */
    if (handset_age_ms() > HANDSET_TIMEOUT_MS) {
        button = HS_NONE;
    }

    /* Comfort functions act on the press edge; motion acts while held.
     * POWER is the exception, it acts on release, so a short press can be
     * told apart from the long-press macro.
     */
    if (button != prev_button) {
        if (button == HS_POWER) {
            power_down_ms   = ms_ticks;
            power_long_done = 0;
        } else if (button != HS_NONE) {
            dbg.presses++;
            comfort_button(button);
        } else if (prev_button == HS_POWER && !power_long_done) {
            dbg.presses++;
#if ENH_POWER_DOUBLE_TAP
            /* The first tap has already run its ordinary toggle by the time we
             * know a second one is coming. That washes out: the macro ends with
             * every comfort function off and the gate off whichever way the
             * toggle went. So nothing needs deferring, and a single press keeps
             * its usual immediate response.
             */
            if (power_tapped && (ms_ticks - power_up_ms) <= POWER_DOUBLE_MS) {
                power_tapped = 0;
                auto_flatten = 1;
                dbg.auto_moves++;
                power_long_press();
            } else {
                power_tapped = 1;
                power_up_ms  = ms_ticks;
                power_short_press();
            }
#else
            power_short_press();
#endif
        } else if (prev_button == HS_POWER && power_long_done) {
            /* Release ends a held macro: this is where the factory switches
             * everything off and starts the vent.
             */
            power_macro_finish();
        }

        prev_button = button;
    }

    if (button == HS_POWER && !power_long_done &&
        (ms_ticks - power_down_ms) >= POWER_LONG_MS) {
        power_long_done = 1;
        dbg.presses++;
        power_long_press();
    }

    pneumatics_update();
    heat_update();

    switch (button) {
    case HS_RECLINE_UP:    want = MOTION_RECLINE_UP;    break;
    case HS_RECLINE_DOWN:  want = MOTION_RECLINE_DOWN;  break;
    case HS_HEADREST_UP:   want = MOTION_HEADREST_UP;   break;
    case HS_HEADREST_DOWN: want = MOTION_HEADREST_DOWN; break;

    /* Keep the flatten running for as long as POWER stays held; releasing it
     * falls through to MOTION_NONE and stops the motors.
     */
    case HS_POWER:         want = power_long_done ? MOTION_FLATTEN : MOTION_NONE; break;

    default:               want = MOTION_NONE;          break;
    }

#if ENH_POWER_DOUBLE_TAP
    /* Holding POWER keeps the flatten alive through the switch above. A double
     * tap has nothing held, so the request has to be renewed here instead,
     * until motion.c ends it, at the stops or on the timeout ceiling.
     *
     * Any button cancels, matching the rule that any other button interrupts
     * the held version.
     */
    if (auto_flatten) {
        if (button != HS_NONE) {
            auto_flatten = 0;           /* cancelled, leave everything as it is */
        } else if (motion_active() == MOTION_NONE) {
            /* The move ended on its own, at the stops or on the ceiling. This
             * is the unattended equivalent of releasing a held POWER.
             */
            auto_flatten = 0;
            power_macro_finish();
        } else {
            want = MOTION_FLATTEN;
        }
    }
#endif

    motion_request(want);
    motion_update(dbg.adc);

    led_update();
}

static void sample_inputs(void)
{
    uint32_t b  = gpio_istat_b();
    uint32_t in = 0;

    if (b & (1u << 8))  in |= 1u << 0;
    if (b & (1u << 9))  in |= 1u << 1;
    if (b & (1u << 12)) in |= 1u << 2;

    dbg.istat_a = gpio_istat_a();
    dbg.istat_b = b;
    dbg.istat_c = gpio_istat_c();
    dbg.inputs  = in;
    dbg.inputs_seen |= in;
}

static void sample_adc(void)
{
    uint32_t v = adc_read(ADC_CH_CURRENT);

    dbg.adc     = v;
    dbg.adc_ch8 = adc_read(ADC_CH_8);
    dbg.adc_ch9 = adc_read(ADC_CH_9);

    if (v != 0xFFFFFFFF) {
        if (v < dbg.adc_min)      dbg.adc_min      = v;
        if (v > dbg.adc_max)      dbg.adc_max      = v;
        if (v > dbg.adc_ch7_max)  dbg.adc_ch7_max  = v;
    }
}

/* The debug block lives in a NOLOAD section, so startup does not clear it.
 * Without this, stale RAM from a previous image reads back as plausible data.
 */
static void debug_init(void)
{
    volatile uint32_t *p = (volatile uint32_t *)&dbg;

    for (unsigned i = 0; i < sizeof(dbg) / 4; i++) {
        p[i] = 0;
    }

    dbg.adc_min = 0xFFFFFFFF;
    dbg.magic   = DEBUG_MAGIC;
}

int main(void)
{
    uint32_t last_poll_ms = 0;

    timing_init();
    gpio_init();
    debug_init();
    adc_init();
    handset_init();
    watchdog_init();

    for (;;) {
        sample_adc();
        sample_inputs();

        if ((ms_ticks - last_poll_ms) >= HANDSET_POLL_MS) {
            last_poll_ms = ms_ticks;
            handset_send(HS_TYPE_IDLE, handset_leds(), 0, 0);
        }

        handset_poll();
        handset_update();

        dbg.octl_a = gpio_octl_a();
        dbg.octl_b = gpio_octl_b();
        dbg.octl_c = gpio_octl_c();

        watchdog_kick();
        dbg.ticks++;
    }
}
