/* Control: what the chair does when you press a button.
 *
 * This is the layer the other test files skip. They call module APIs directly,
 * so nothing covered the route from a handset button code through the POWER
 * gate, the motion map and the macros to the relays. These tests drive the
 * firmware the way the handset does, by setting a button code and letting time
 * pass, and assert on the fake board and the LED bitmap sent back.
 *
 * Requirements are in docs/firmware-spec.md §6-§8, and the macro variants in
 * docs/enhancements-spec.md §2.2.
 */

#include "harness.h"

#include "adjust.h"
#include "button.h"
#include "macros/preset.h"
#include "control.h"
#include "debug.h"
#include "enhancements.h"
#include "gpio.h"
#include "handset.h"
#include "heat.h"
#include "motion.h"
#include "pneumatics.h"
#include "power.h"

#define VALVE_VENT 0x04
#define VENT_MS    120000

/* control_update() feeds motion_update() from dbg.adc, so that is where a test
 * injects a current-sense reading.
 */
#define RUNNING_CURRENT 100

static void tick(void) { control_update(); }

static void reset(void)
{
    /* Quiesce everything while the clock is still monotonic: fakes_reset()
     * winds ms_ticks back to zero, and the modules hold absolute timestamps.
     */
    fake_button = HS_NONE;
    fake_age_ms = 0;
    control_update();

    motion_request(MOTION_NONE);
    motion_update(0);

    fakes_reset();
    tick_hook = tick;
    dbg.adc   = 0;

    /* button.c's double-tap pairing outlives a test, and fakes_reset() winds
     * the clock back, so a leftover timestamp can land inside the window by
     * accident and turn the next test's first tap into a double tap.
     *
     * Tapping a motion button parks the pairing on a known button instead. It
     * has no side effect to undo: control.c ignores taps for anything but
     * POWER, and 20 ms is far short of MOTION_SETTLE_MS, so no relay closes.
     */
    fake_button = HS_HEADREST_UP;
    run_ms(20);
    fake_button = HS_NONE;
    run_ms(20);

    pneumatics_shutdown();
    heat_off();

    /* Remembered levels outlive their function being switched off, on purpose,
     * so they also outlive a test. Put them back to the defaults, or every test
     * after the first one to change them starts somewhere else.
     */
#if ENH_HEAT_LEVELS
    heat_press();
    heat_arm();
    heat_set_level(HEAT_LEVEL_DEFAULT);
    heat_press();                   /* accept */
    heat_off();
#endif
#if ENH_MASSAGE_LEVELS
    pneumatics_massage_arm();
    pneumatics_massage_set_level(ADJUST_LEVEL_MAX);
    pneumatics_shutdown();
#endif

    if (power_is_on()) {
        power_toggle();
    }
    run_ms(VENT_MS + 10);       /* let the shutdown vent finish */

    dbg.presses = 0;
    dbg.stops   = 0;
    dbg.stalls  = 0;
#if ENH_END_OF_TRAVEL_STOP
    dbg.arrivals = 0;
#endif
#if ENH_POWER_DOUBLE_TAP
    dbg.auto_moves = 0;
#endif
#if ENH_PRESET
    dbg.presets_saved    = 0;
    dbg.presets_recalled = 0;

    /* Slots outlive a test the same way the levels do. Wiping the store puts
     * every one back to unwritten.
     */
    fake_flash_wipe();
    settings_init();
#endif
}

static void hold(uint8_t button, uint32_t ms)
{
    fake_button = button;
    run_ms(ms);
}

static void release(uint32_t ms)
{
    fake_button = HS_NONE;
    run_ms(ms);
}

static void tap(uint8_t button)
{
    hold(button, 20);
    release(20);
}

/* Switch heat on from the handset, whichever build this is.
 *
 * The reference acts on the press; with ENH_HEAT_LEVELS HEAT acts on the
 * release, so a tap covers both. Waiting out the readout afterwards leaves no
 * bar up to confuse a later assertion about the motion lamps, and is harmless
 * in the reference build where there is no bar at all.
 */
static void heat_tap_on(void)
{
    tap(HS_HEAT);
#if ENH_HEAT_LEVELS
    run_ms(ADJUST_READOUT_MS + ADJUST_BAR_EXIT_MS
           + ADJUST_BAR_STEP_MS * ADJUST_LEVEL_MAX);
#endif
}

static int any_recline_pin(void)
{
    return fake_pin[PIN_RECLINE_A] || fake_pin[PIN_RECLINE_B]
        || fake_pin[PIN_RECLINE_C] || fake_pin[PIN_RECLINE_D];
}

/* Drive a motion far enough that its relays are fully closed. */
#define ENGAGED_MS (MOTION_SETTLE_MS + MOTION_STAGGER_MS + 10)

/* --- the POWER gate, §8 --- */

TEST(comfort_buttons_do_nothing_while_power_is_off)
{
    reset();
    CHECK(!power_is_on());

    heat_tap_on();
    CHECK(!heat_is_on());

    tap(HS_MASSAGE);
    CHECK(!pneumatics_massage_on());
}

TEST(power_on_lets_comfort_buttons_through)
{
    reset();
    tap(HS_POWER);
    CHECK(power_is_on());

    heat_tap_on();
    CHECK(heat_is_on());

    tap(HS_MASSAGE);
    CHECK(pneumatics_massage_on());
}

TEST(power_off_stops_comfort_and_starts_the_vent)
{
    reset();
    tap(HS_POWER);
    heat_tap_on();
    tap(HS_MASSAGE);
    CHECK(heat_is_on());

    tap(HS_POWER);
    CHECK(!power_is_on());
    CHECK(!heat_is_on());
    CHECK(!pneumatics_massage_on());
    CHECK_EQ(fake_shift, VALVE_VENT);
}

/* --- motion, §7 --- */

TEST(a_motion_button_drives_its_axis_while_held)
{
    reset();
    hold(HS_RECLINE_UP, ENGAGED_MS);

    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_B], 1);

    release(10);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());
}

TEST(motion_is_not_gated_by_power)
{
    reset();
    CHECK(!power_is_on());          /* motion works regardless, §8 */

    hold(HS_HEADREST_UP, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_HEADREST_UP);
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_DIR], 0);

    release(10);
}

TEST(a_silent_handset_stops_motion)
{
    reset();
    hold(HS_RECLINE_DOWN, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_RECLINE_DOWN);

    /* The button code is unchanged; the frames simply stopped arriving. */
    fake_age_ms = 251;
    run_ms(10);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());

    fake_age_ms = 0;
    release(10);
}

TEST(leds_mirror_what_is_driven)
{
    reset();
    hold(HS_RECLINE_UP, ENGAGED_MS);
    CHECK_EQ(fake_leds & LED_RECLINE_UP, LED_RECLINE_UP);

    release(10);
    CHECK_EQ(fake_leds & LED_RECLINE_UP, 0);

    tap(HS_POWER);
    CHECK_EQ(fake_leds & LED_POWER, LED_POWER);
}

/* --- the hold macro, §8 --- */

#if !ENH_PRESET

/* The factory gesture, kept only where the POWER hold is still free. With
 * ENH_PRESET the hold arms the preset slots and the flatten is reached by the
 * double tap instead, covered under §2.2.
 */
TEST(holding_power_drives_the_chair_flat)
{
    reset();
    hold(HS_POWER, BUTTON_HOLD_MS + ENGAGED_MS);

    CHECK_EQ(motion_active(), MOTION_FLATTEN);
    CHECK_EQ(fake_pin[PIN_RECLINE_C], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_D], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_DIR], 1);
    CHECK_EQ(fake_leds & (LED_RECLINE_DOWN | LED_HEADREST_DOWN),
             LED_RECLINE_DOWN | LED_HEADREST_DOWN);

    release(10);
}

TEST(a_held_macro_keeps_comfort_on_until_release)
{
    reset();
    tap(HS_POWER);
    heat_tap_on();
    tap(HS_LUMBAR);
    CHECK(heat_is_on());
    CHECK(pneumatics_lumbar_lit());

    /* The factory switches nothing off until the macro ends, so the chair
     * drives with lumbar still inflated and heat still on.
     */
    hold(HS_POWER, BUTTON_HOLD_MS + ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_FLATTEN);
    CHECK(heat_is_on());
    CHECK(pneumatics_lumbar_lit());
    CHECK(power_is_on());

    release(10);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!heat_is_on());
    CHECK(!pneumatics_lumbar_lit());
    CHECK(!power_is_on());
    CHECK_EQ(fake_shift, VALVE_VENT);       /* and only then, the vent */
}

#else /* ENH_PRESET */

/* The hold is spent on presets, so it must not move anything. */
TEST(holding_power_arms_rather_than_flattening)
{
    reset();
    hold(HS_POWER, BUTTON_HOLD_MS + ENGAGED_MS);

    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());

    release(10);
}

#endif /* !ENH_PRESET */

TEST(a_short_power_press_is_not_the_macro)
{
    reset();
    tap(HS_POWER);
    CHECK(power_is_on());
    CHECK_EQ(motion_active(), MOTION_NONE); /* no flatten from a tap */
}

#if ENH_POWER_DOUBLE_TAP

/* --- the double-tap shortcut, enhancements-spec.md §2.2 --- */

/* Two taps inside the window. The first runs its ordinary toggle, which is
 * deliberate; see §2.2.
 */
static void double_tap_power(void)
{
    tap(HS_POWER);
    tap(HS_POWER);
}

TEST(a_double_tap_runs_the_macro_with_nothing_held)
{
    reset();
    double_tap_power();

    CHECK_EQ(dbg.auto_moves, 1);

    /* Nothing is held, and it keeps driving anyway. */
    dbg.adc = RUNNING_CURRENT;
    run_ms(ENGAGED_MS);
    CHECK_EQ(fake_button, HS_NONE);
    CHECK_EQ(motion_active(), MOTION_FLATTEN);
    CHECK_EQ(fake_pin[PIN_RECLINE_C], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 1);
}

TEST(an_unattended_macro_shuts_down_at_the_stops)
{
    reset();
    double_tap_power();
    CHECK(power_is_on());       /* the first tap's ordinary toggle, §2.2 */

    dbg.adc = RUNNING_CURRENT;
    run_ms(MOTION_SETTLE_MS + MOTION_ARM_MS);
    CHECK_EQ(motion_active(), MOTION_FLATTEN);
    CHECK(power_is_on());       /* nothing shuts down while it drives */

    /* Every motor disconnects itself at its limit switch. */
    dbg.adc = 0;
    run_ms(MOTION_ARRIVED_MS + 20);

    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK_EQ(dbg.arrivals, 1);
    CHECK(!power_is_on());      /* and only then, the shutdown */
    CHECK_EQ(fake_shift, VALVE_VENT);
}

TEST(any_button_cancels_an_unattended_macro)
{
    reset();
    double_tap_power();
    dbg.adc = RUNNING_CURRENT;
    run_ms(ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_FLATTEN);
    CHECK(power_is_on());

    hold(HS_HEADREST_UP, 10);
    CHECK(motion_active() != MOTION_FLATTEN);

    /* A cancel aborts rather than completing: the gate is still on, which it
     * would not be had the shutdown run.
     */
    CHECK(power_is_on());

    /* And it is cancelled for good; releasing does not hand the chair back. */
    release(ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_NONE);
}

TEST(power_blinks_while_the_macro_runs_unattended)
{
    reset();
    double_tap_power();
    dbg.adc = RUNNING_CURRENT;
    run_ms(ENGAGED_MS);

    int seen_lit = 0, seen_dark = 0;

    for (int i = 0; i < 40; i++) {
        run_ms(50);
        if (fake_leds & LED_POWER) seen_lit = 1;
        else                       seen_dark = 1;
    }

    CHECK(seen_lit);
    CHECK(seen_dark);
}

#else /* reference build */

TEST(a_second_tap_is_just_another_power_toggle)
{
    reset();
    tap(HS_POWER);
    CHECK(power_is_on());

    tap(HS_POWER);
    CHECK(!power_is_on());
    CHECK_EQ(motion_active(), MOTION_NONE);     /* no macro in the reference */
}

#endif /* ENH_POWER_DOUBLE_TAP */

#if ADJUST_IN_USE

/* --- levels, enhancements-spec.md §2.3 --- */

/* The four lamps the bar borrows, bottom to top, and the buttons that pick
 * those levels while the bar is a menu.
 */
#define BAR_LAMPS (LED_HEADREST_DOWN | LED_HEADREST_UP \
                   | LED_RECLINE_DOWN | LED_RECLINE_UP)

static const uint8_t bar_lamp[ADJUST_LEVEL_MAX] = {
    LED_HEADREST_DOWN, LED_HEADREST_UP, LED_RECLINE_DOWN, LED_RECLINE_UP
};

static const uint8_t level_button[ADJUST_LEVEL_MAX] = {
    HS_HEADREST_DOWN, HS_HEADREST_UP, HS_RECLINE_DOWN, HS_RECLINE_UP
};

/* Hold an adjuster's own button past its threshold, shorter than POWER's. */
static void hold_adjust(uint8_t button)
{
    hold(button, BUTTON_SHORT_HOLD_MS + 20);
    release(20);
}

static void hold_heat(void)    { hold_adjust(HS_HEAT); }
static void hold_massage(void) { hold_adjust(HS_MASSAGE); }

/* Let the bar finish sliding away, so a later assertion about the motion lamps
 * is not reading an outro.
 */
static void settle_bar(void)
{
    run_ms(ADJUST_BAR_EXIT_MS + ADJUST_BAR_STEP_MS * ADJUST_LEVEL_MAX + 20);
}

#if ENH_HEAT_LEVELS

/* A tap asks nothing and returns to the level last used, which is what makes
 * the common case one press.
 */
TEST(a_tap_switches_heat_on_at_the_remembered_level)
{
    reset();
    tap(HS_POWER);

    tap(HS_HEAT);
    CHECK(heat_is_on());
    CHECK_EQ(heat_level(), HEAT_LEVEL_DEFAULT);
    CHECK(!adjust_armed());               /* a readout, not a menu */

    tap(HS_HEAT);
    CHECK(!heat_is_on());
}

/* The hold asks about the level and nothing else. Whether heat is running is a
 * separate question, and the hold leaves it exactly as it found it.
 */
TEST(holding_heat_opens_the_level_buttons)
{
    reset();
    tap(HS_POWER);

    hold_heat();
    CHECK(adjust_armed());
    CHECK(!heat_is_on());               /* off stays off */

    tap(HS_HEAT);                       /* accept, closing the window */
    CHECK(!adjust_armed());
    CHECK(!heat_is_on());               /* an answer, not a switch */

    settle_bar();
    tap(HS_HEAT);                       /* now switch it on */
    CHECK(heat_is_on());

    settle_bar();
    hold_heat();
    CHECK(adjust_armed());
    CHECK(heat_is_on());                /* and on stays on */

    /* The release that ended the hold did not toggle it either. */
    run_ms(50);
    CHECK(heat_is_on());
}

/* Each of the four buttons picks its own level, bottom to top. */
TEST(the_motion_buttons_pick_levels_while_armed)
{
    for (int i = 0; i < HEAT_LEVEL_MAX; i++) {
        reset();
        tap(HS_POWER);

        hold_heat();
        CHECK(adjust_armed());

        tap(level_button[i]);
        CHECK_EQ(heat_level(), i + 1);
    }
}

/* The whole reason the latch exists. A press taken as a level must not drive
 * its motor, and must stay off it for as long as it is held: the window closes
 * on that same press, and without the latch the motor starts underneath it.
 */
TEST(a_borrowed_button_never_reaches_its_motor)
{
    reset();
    tap(HS_POWER);
    hold_heat();
    CHECK(adjust_armed());

    /* Hold the level button down long past the window it just closed. */
    hold(HS_RECLINE_UP, ENGAGED_MS + 2000);

    CHECK_EQ(heat_level(), HEAT_LEVEL_MAX);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());
    CHECK(!adjust_armed());               /* the pick closed it underneath */

    release(10);

    /* Released and pressed again, it is a motion button once more. */
    hold(HS_RECLINE_UP, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    release(10);
}

/* Once the window is shut the buttons are motors again, with nothing latched
 * over from the pick that shut it.
 */
TEST(the_buttons_go_back_to_the_motors)
{
    reset();
    tap(HS_POWER);
    hold_heat();

    tap(level_button[0]);
    CHECK_EQ(heat_level(), 1);
    CHECK(!adjust_armed());             /* the pick shut it immediately */

    settle_bar();

    hold(HS_HEADREST_DOWN, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_HEADREST_DOWN);
    CHECK_EQ(heat_level(), 1);          /* and did not pick anything */
    release(10);
}

/* The bar a tap puts up is a readout: the buttons still drive, and any other
 * button takes it down.
 */
TEST(the_readout_bar_leaves_the_buttons_alone)
{
    reset();
    tap(HS_POWER);

    tap(HS_HEAT);
    CHECK(fake_leds & BAR_LAMPS);       /* something of the bar is lit */

    /* A motion button still moves the chair rather than picking a level. */
    hold(HS_HEADREST_DOWN, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_HEADREST_DOWN);
    CHECK_EQ(heat_level(), HEAT_LEVEL_DEFAULT);
    release(10);

    settle_bar();
    CHECK_EQ(fake_leds & BAR_LAMPS, 0);
}

/* Any other button dismisses the readout, but HEAT does not: that press may be
 * the start of the hold that opens the menu, and taking the bar down under it
 * makes asking to adjust look like a mis-press.
 */
TEST(heat_does_not_dismiss_its_own_readout)
{
    reset();
    tap(HS_POWER);

    tap(HS_HEAT);
    CHECK(fake_leds & BAR_LAMPS);

    hold(HS_HEAT, BUTTON_SHORT_HOLD_MS - 200);
    CHECK(fake_leds & BAR_LAMPS);       /* still up, part way into the hold */

    run_ms(220);                        /* and it becomes the menu */
    CHECK(adjust_armed());
    release(20);

    /* Where LUMBAR, which owns no bar of its own, takes it down. Not MASSAGE:
     * that is an adjuster too now, so it would replace heat's bar with its own
     * rather than clear it.
     */
    tap(HS_HEAT);                       /* accept */
    settle_bar();
    tap(HS_HEAT);                       /* off */
    tap(HS_HEAT);                       /* on, readout up */
    CHECK(fake_leds & BAR_LAMPS);

    tap(HS_LUMBAR);                     /* starts an inflate; irrelevant here */
    settle_bar();
    CHECK_EQ(fake_leds & BAR_LAMPS, 0);
}

/* The bar fills a lamp at a time from the bottom. */
TEST(the_bar_fills_in_from_the_bottom)
{
    reset();
    tap(HS_POWER);

    hold(HS_HEAT, BUTTON_SHORT_HOLD_MS + 5);
    CHECK_EQ(fake_leds & BAR_LAMPS, bar_lamp[0]);

    for (int i = 1; i < HEAT_LEVEL_DEFAULT; i++) {
        uint8_t want = 0;

        run_ms(ADJUST_BAR_STEP_MS);

        for (int j = 0; j <= i; j++) {
            want |= bar_lamp[j];
        }
        CHECK_EQ(fake_leds & BAR_LAMPS, want);
    }

    release(20);
}

/* A motion owns those lamps. Heat is only borrowing them. */
TEST(a_motion_outranks_the_bar_graph)
{
    reset();
    tap(HS_POWER);

    tap(HS_HEAT);
    CHECK(fake_leds & BAR_LAMPS);

    hold(HS_RECLINE_UP, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    CHECK_EQ(fake_leds & BAR_LAMPS, LED_RECLINE_UP);

    release(10);
}

/* Level survives an off and on, so a tap really is enough. */
TEST(the_level_is_remembered_between_uses)
{
    reset();
    tap(HS_POWER);

    hold_heat();
    tap(level_button[1]);               /* level two */
    CHECK_EQ(heat_level(), 2);

    tap(HS_HEAT);                       /* accept */
    settle_bar();
    tap(HS_HEAT);                       /* off */
    CHECK(!heat_is_on());

    tap(HS_HEAT);                       /* on again, saying nothing */
    CHECK(heat_is_on());
    CHECK_EQ(heat_level(), 2);
}

/* Everything here is behind the POWER gate, holds included. */
TEST(heat_gestures_are_gated_by_power)
{
    reset();
    CHECK(!power_is_on());

    hold_heat();
    CHECK(!heat_is_on());
    CHECK(!adjust_armed());
}

/* The level is a duty cycle and nothing else. Level one spends most of the
 * period off; the top level never switches off at all.
 */
TEST(the_level_sets_the_duty_cycle)
{
    reset();
    tap(HS_POWER);

    hold_heat();
    tap(level_button[0]);               /* level one, 25% */
    tap(HS_HEAT);                       /* accept */
    CHECK_EQ(heat_level(), 1);
    CHECK_EQ(fake_pin[PIN_HEATER], 1);  /* the cycle opens energised */

    run_ms(HEAT_DUTY_PERIOD_MS / 2);    /* past 25%, into the gap */
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
    CHECK(heat_is_on());                /* a gap is not an off */

    tap(HS_HEAT);                       /* off */
    settle_bar();

    hold_heat();
    tap(level_button[HEAT_LEVEL_MAX - 1]);
    tap(HS_HEAT);
    CHECK_EQ(heat_level(), HEAT_LEVEL_MAX);

    run_ms(HEAT_DUTY_PERIOD_MS);        /* a whole cycle, never off */
    CHECK_EQ(fake_pin[PIN_HEATER], 1);
}

#endif /* ENH_HEAT_LEVELS */

#if ENH_MASSAGE_LEVELS

/* --- massage intensity, enhancements-spec.md §2.6 --- */

/* The same gesture as heat, on the massage button, driving the same adjuster —
 * and like heat it changes only the intensity. Whether the massage is running
 * is a separate question that the hold does not answer.
 */
TEST(holding_massage_opens_the_level_buttons)
{
    reset();
    tap(HS_POWER);

    hold_massage();
    CHECK(adjust_armed());
    CHECK_EQ(adjust_owner(), ADJUST_MASSAGE);
    CHECK(!pneumatics_massage_on());    /* off stays off */

    /* The release that ended the hold did not start it either. */
    run_ms(50);
    CHECK(!pneumatics_massage_on());

    tap(HS_MASSAGE);                    /* accept */
    CHECK(!adjust_armed());
    CHECK(!pneumatics_massage_on());    /* an answer, not a switch */

    settle_bar();
    tap(HS_MASSAGE);                    /* and now the toggle is a toggle */
    CHECK(pneumatics_massage_on());
}

/* Each of the four buttons picks its own intensity. */
TEST(the_motion_buttons_pick_intensities)
{
    for (int i = 0; i < ADJUST_LEVEL_MAX; i++) {
        reset();
        tap(HS_POWER);

        hold_massage();
        tap(level_button[i]);
        CHECK_EQ(pneumatics_massage_level(), i + 1);
    }
}

/* Only one of them can have the buttons. Opening massage's takes them from
 * heat's rather than leaving two menus fighting over four lamps.
 */
TEST(only_one_thing_owns_the_adjuster)
{
    reset();
    tap(HS_POWER);

    hold_heat();
    CHECK_EQ(adjust_owner(), ADJUST_HEAT);

    hold_massage();
    CHECK_EQ(adjust_owner(), ADJUST_MASSAGE);
    CHECK(adjust_armed());

    /* And a pick now goes to massage, not to heat. */
    tap(level_button[0]);
    CHECK_EQ(pneumatics_massage_level(), 1);
    CHECK_EQ(heat_level(), HEAT_LEVEL_DEFAULT);
}

/* Intensity survives massage being switched off and on. */
TEST(the_intensity_is_remembered_between_uses)
{
    reset();
    tap(HS_POWER);

    hold_massage();
    tap(level_button[1]);               /* level two */
    tap(HS_MASSAGE);                    /* accept */
    CHECK_EQ(pneumatics_massage_level(), 2);

    settle_bar();
    tap(HS_MASSAGE);                    /* off */
    CHECK(!pneumatics_massage_on());

    tap(HS_MASSAGE);                    /* on again, saying nothing */
    CHECK(pneumatics_massage_on());
    CHECK_EQ(pneumatics_massage_level(), 2);
}

/* Gated by POWER, holds included, same as everything else in here. */
TEST(massage_gestures_are_gated_by_power)
{
    reset();
    CHECK(!power_is_on());

    hold_massage();
    CHECK(!pneumatics_massage_on());
    CHECK(!adjust_armed());
}

#endif /* ENH_MASSAGE_LEVELS */

#endif /* ADJUST_IN_USE */

#if ENH_PRESET

/* --- presets, enhancements-spec.md §2.8 --- */

/* Open the store window. */
static void arm_presets(void)
{
    hold(HS_POWER, BUTTON_HOLD_MS + 20);
    release(20);
}

/* Hold POWER, then tap a motion button, without moving that motor. */
static void save_preset(int slot)
{
    arm_presets();
    tap(level_button[slot]);
}

/* The same window, but holding the slot button, which clears it instead. */
static void clear_preset(int slot)
{
    arm_presets();
    hold(level_button[slot], BUTTON_SHORT_HOLD_MS + 20);
    release(20);
}

/* Every lamp the four slots can light. */
#define SLOT_LAMPS BAR_LAMPS

/* Let a save's confirmation flash run out. Longer than settle_bar(), which only
 * covers a slide.
 */
static void settle_flash(void)
{
    run_ms(ADJUST_CONFIRM_FLASHES * 2u * ADJUST_CONFIRM_MS + 50);
}

/* Drive an axis up for a while so there is a position worth saving. */
static void raise_recline(uint32_t ms)
{
    hold(HS_RECLINE_UP, MOTION_SETTLE_MS + MOTION_STAGGER_MS + ms);
    release(20);
}

TEST(a_slot_records_where_the_chair_is)
{
    reset();
    tap(HS_POWER);

    raise_recline(3000);
    CHECK(motion_pos_recline() > 0);

    save_preset(0);
    CHECK_EQ(dbg.presets_saved, 1);

    /* Saving moves nothing. */
    CHECK_EQ(motion_active(), MOTION_NONE);
}

/* While armed, a motion button writes a slot rather than driving its motor,
 * and stays off it for as long as it is held.
 */
TEST(an_armed_press_never_reaches_its_motor)
{
    reset();
    tap(HS_POWER);

    hold(HS_POWER, BUTTON_HOLD_MS + 20);
    release(20);

    /* Held past the point the relays would have closed, and nothing moves. The
     * save itself waits for the release, because a hold means clear.
     */
    hold(HS_RECLINE_UP, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());
    CHECK_EQ(dbg.presets_saved, 0);

    release(10);
    CHECK_EQ(dbg.presets_saved, 1);

    /* Released and pressed again, it is a motion button once more. */
    hold(HS_RECLINE_UP, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    release(10);
}

/* The window closes on its own, and the buttons go back to the motors. */
TEST(the_armed_window_expires)
{
    reset();
    tap(HS_POWER);

    hold(HS_POWER, BUTTON_HOLD_MS + 20);
    release(PRESET_ARM_MS + 50);

    hold(HS_HEADREST_DOWN, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_HEADREST_DOWN);
    CHECK_EQ(dbg.presets_saved, 0);
    release(10);
}

/* A double tap on a slot that has never been written must do nothing. All
 * zeros is a legitimate preset, so without the written-bitmap this would drive
 * the chair flat and switch everything off.
 */
TEST(recalling_an_empty_slot_does_nothing)
{
    reset();
    tap(HS_POWER);

    raise_recline(3000);
    release(20);

    tap(level_button[2]);
    tap(level_button[2]);
    run_ms(50);

    CHECK_EQ(dbg.presets_recalled, 0);
    CHECK_EQ(motion_active(), MOTION_NONE);
}

/* Save somewhere, move away, and come back to it. */
TEST(a_recall_returns_to_the_saved_position)
{
    uint32_t saved;

    reset();

    /* Current flowing, so nothing "arrives" mid-travel and the seek has to
     * reach the target on the position estimate alone. At zero the end-of-
     * travel stop fires a second in and re-zeroes, which would pass this test
     * without ever exercising a seek.
     */
    dbg.adc = RUNNING_CURRENT;

    tap(HS_POWER);

    raise_recline(4000);
    saved = motion_pos_recline();
    CHECK(saved > 0);

    save_preset(1);
    settle_bar();

    /* Away from it, far enough that a seek has to move. */
    hold(HS_RECLINE_DOWN, MOTION_SETTLE_MS + 2000);
    release(20);
    CHECK(motion_pos_recline() < saved);

    tap(level_button[1]);
    tap(level_button[1]);
    CHECK_EQ(dbg.presets_recalled, 1);

    /* Unattended: nothing is held, and the macro renews the request itself.
     *
     * Two sources of slop, and the test has to allow both: the slot stores the
     * position in PRESET_STEP_MS units, so its target is up to a step below
     * where the chair actually was, and the seek stops within MOTION_SEEK_MS of
     * that target rather than chasing it exactly.
     */
    run_ms(MOTION_SETTLE_MS + 4000);
    CHECK(motion_pos_recline() + MOTION_SEEK_MS + PRESET_STEP_MS >= saved);
    CHECK(motion_pos_recline() <= saved + MOTION_SEEK_MS);
}

/* Held rather than tapped, because clearing throws something away and a tap
 * already means save.
 */
TEST(holding_a_slot_clears_it)
{
    reset();
    tap(HS_POWER);

    raise_recline(3000);
    save_preset(2);
    CHECK(settings_preset_used(2));
    settle_flash();

    clear_preset(2);
    CHECK(!settings_preset_used(2));
    CHECK_EQ(dbg.presets_saved, 1);     /* a clear is not a save */

    /* Out of the window first, or the taps below would be taken as a save and
     * put something back in the slot.
     */
    tap(HS_POWER);
    CHECK(!preset_armed());
    settle_bar();

    /* And an emptied slot recalls as nothing, like one never written. */
    tap(level_button[2]);
    tap(level_button[2]);
    CHECK_EQ(dbg.presets_recalled, 0);
}

/* A cleared slot survives a reboot as cleared: all zeros is a legitimate
 * preset, so "never written" has to be stored rather than inferred.
 */
TEST(a_cleared_slot_stays_cleared)
{
    reset();
    tap(HS_POWER);

    raise_recline(3000);
    save_preset(1);
    settle_bar();

    clear_preset(1);
    settle_bar();

    settings_init();                    /* as if the chair had been reset */
    CHECK(!settings_preset_used(1));
}

/* POWER opened the window, so POWER backs out of it — without writing anything
 * and without toggling the gate on the way.
 */
TEST(power_cancels_the_store_window)
{
    reset();
    tap(HS_POWER);
    CHECK(power_is_on());

    arm_presets();
    CHECK(preset_armed());

    tap(HS_POWER);
    CHECK(!preset_armed());
    CHECK(power_is_on());               /* not also a toggle */
    CHECK_EQ(dbg.presets_saved, 0);

    /* And the motion buttons are motors again. */
    settle_bar();
    hold(HS_HEADREST_DOWN, ENGAGED_MS);
    CHECK_EQ(motion_active(), MOTION_HEADREST_DOWN);
    CHECK_EQ(dbg.presets_saved, 0);
    release(10);
}

/* The window says which of the four slots are taken, or the store mode is four
 * dark buttons and no clue which of them means anything.
 */
TEST(the_store_window_shows_which_slots_are_taken)
{
    reset();
    tap(HS_POWER);

    raise_recline(3000);
    save_preset(0);
    settle_bar();

    arm_presets();

    /* Let the display finish arriving, then watch a blink cycle. Slot 0 is
     * taken so it stays lit; the other three are free and blink, so at some
     * point in the cycle they are dark while slot 0 is not.
     */
    run_ms(ADJUST_BAR_STEP_MS * ADJUST_LEVEL_MAX + 20);

    {
        int saw_only_taken = 0;
        uint8_t free_lamps = (uint8_t)(SLOT_LAMPS & ~bar_lamp[0]);

        for (uint32_t t = 0; t < PRESET_ARM_BLINK_MS * 3u; t += 20) {
            run_ms(20);

            if ((fake_leds & bar_lamp[0])
                && (fake_leds & free_lamps) == 0) {
                saw_only_taken = 1;
            }
        }

        CHECK(saw_only_taken);
    }
}

/* The slot display arrives a lamp at a time, the same way the level bar does. */
TEST(the_store_window_animates_in)
{
    reset();
    tap(HS_POWER);

    hold(HS_POWER, BUTTON_HOLD_MS + 5);

    /* One lamp so far, and it is the bottom one. */
    CHECK_EQ(fake_leds & SLOT_LAMPS, bar_lamp[0]);

    run_ms(ADJUST_BAR_STEP_MS);
    CHECK_EQ(fake_leds & SLOT_LAMPS,
             (uint8_t)(bar_lamp[0] | bar_lamp[1]));

    release(20);
}

/* Saving flashes the slot that took it and then leaves, rather than settling
 * on anything: the gesture is an event, not a mode.
 */
TEST(a_save_flashes_the_slot_then_clears)
{
    int dark = 0;

    reset();
    tap(HS_POWER);

    raise_recline(3000);
    save_preset(3);

    /* Flashing: the lamp is not simply held on. */
    for (uint32_t t = 0; t < ADJUST_CONFIRM_MS * 2u; t += 20) {
        run_ms(20);
        if ((fake_leds & bar_lamp[3]) == 0) {
            dark = 1;
        }
    }
    CHECK(dark);

    /* And then nothing at all. */
    settle_flash();
    CHECK_EQ(fake_leds & SLOT_LAMPS, 0);
}

/* A recall is not behind the POWER gate, and switches it on itself. */
TEST(a_recall_works_with_the_chair_off)
{
    reset();
    tap(HS_POWER);
    raise_recline(3000);
    save_preset(3);
    settle_bar();

    tap(HS_POWER);                      /* off */
    CHECK(!power_is_on());

    tap(level_button[3]);
    tap(level_button[3]);
    run_ms(MOTION_SETTLE_MS + 4000);

    CHECK(power_is_on());
}

#endif /* ENH_PRESET */

int main(void)
{
    printf("control\n");
    RUN(comfort_buttons_do_nothing_while_power_is_off);
    RUN(power_on_lets_comfort_buttons_through);
    RUN(power_off_stops_comfort_and_starts_the_vent);
    RUN(a_motion_button_drives_its_axis_while_held);
    RUN(motion_is_not_gated_by_power);
    RUN(a_silent_handset_stops_motion);
    RUN(leds_mirror_what_is_driven);
#if !ENH_PRESET
    RUN(holding_power_drives_the_chair_flat);
    RUN(a_held_macro_keeps_comfort_on_until_release);
#else
    RUN(holding_power_arms_rather_than_flattening);
#endif
    RUN(a_short_power_press_is_not_the_macro);
#if ENH_HEAT_LEVELS
    RUN(a_tap_switches_heat_on_at_the_remembered_level);
    RUN(holding_heat_opens_the_level_buttons);
    RUN(the_motion_buttons_pick_levels_while_armed);
    RUN(a_borrowed_button_never_reaches_its_motor);
    RUN(the_buttons_go_back_to_the_motors);
    RUN(the_readout_bar_leaves_the_buttons_alone);
    RUN(heat_does_not_dismiss_its_own_readout);
    RUN(the_bar_fills_in_from_the_bottom);
    RUN(a_motion_outranks_the_bar_graph);
    RUN(the_level_is_remembered_between_uses);
    RUN(heat_gestures_are_gated_by_power);
    RUN(the_level_sets_the_duty_cycle);
#endif
#if ENH_MASSAGE_LEVELS
    RUN(holding_massage_opens_the_level_buttons);
    RUN(the_motion_buttons_pick_intensities);
    RUN(only_one_thing_owns_the_adjuster);
    RUN(the_intensity_is_remembered_between_uses);
    RUN(massage_gestures_are_gated_by_power);
#endif
#if ENH_PRESET
    RUN(a_slot_records_where_the_chair_is);
    RUN(an_armed_press_never_reaches_its_motor);
    RUN(the_armed_window_expires);
    RUN(recalling_an_empty_slot_does_nothing);
    RUN(a_recall_returns_to_the_saved_position);
    RUN(a_recall_works_with_the_chair_off);
    RUN(holding_a_slot_clears_it);
    RUN(a_cleared_slot_stays_cleared);
    RUN(power_cancels_the_store_window);
    RUN(the_store_window_shows_which_slots_are_taken);
    RUN(the_store_window_animates_in);
    RUN(a_save_flashes_the_slot_then_clears);
#endif
#if ENH_POWER_DOUBLE_TAP
    RUN(a_double_tap_runs_the_macro_with_nothing_held);
    RUN(an_unattended_macro_shuts_down_at_the_stops);
    RUN(any_button_cancels_an_unattended_macro);
    RUN(power_blinks_while_the_macro_runs_unattended);
#else
    RUN(a_second_tap_is_just_another_power_toggle);
#endif

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
