/* Heat: toggle, the power budget interaction, the auto-off, and the levels.
 *
 * There is no temperature sensor on this board, so the 60 minute timer is the
 * only thing bounding a stuck-on heater. It is not optional, and it is wall
 * clock at every level.
 *
 * The bar graph and the borrowed buttons live in adjust.c and are shared with
 * massage intensity, so they are tested through test_control.c where the
 * handset drives them. What is tested here is what heat.c does with a level
 * once one has been chosen.
 */

#include "harness.h"
#include "../src/adjust.h"
#include "../src/debug.h"
#include "../src/enhancements.h"
#include "../src/gpio.h"
#include "../src/heat.h"
#include "../src/motion.h"

#define HEAT_TIMEOUT_MS (60u * 60u * 1000u)

static void tick(void)
{
    motion_update(0);
    heat_update();
#if ENH_HEAT_LEVELS
    adjust_update();
#endif
}

static void reset(void)
{
    motion_request(MOTION_NONE);
    motion_update(0);
    heat_off();
    fakes_reset();
    tick_hook = tick;

#if ENH_HEAT_LEVELS
    /* The remembered level outlives heat_off() on purpose, so it also outlives
     * a test. Put it back to the default, or every test after the first one to
     * change it starts somewhere else.
     */
    heat_press();
    heat_arm();
    heat_set_level(HEAT_LEVEL_DEFAULT);
    heat_press();                       /* accept */
    heat_off();
#endif
}

/* Switch heat on at the top level.
 *
 * Level 4 is 100% duty, which is continuous, which is exactly what the
 * reference build does. Pinning it there keeps the enhanced and reference
 * builds equivalent for every test below that is not about levels.
 */
static void heat_on(void)
{
    heat_press();

#if ENH_HEAT_LEVELS
    heat_arm();
    heat_set_level(HEAT_LEVEL_MAX);
    heat_press();                       /* accept, and close the window */
#endif
}

TEST(button_toggles_the_element)
{
    reset();

    heat_on();
    run_ms(1);
    CHECK(heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 1);

    heat_press();
    run_ms(1);
    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

/* The factory firmware refuses the press outright rather than queueing it.
 * Observable on the chair: the heat button will not toggle mid-motion.
 */
TEST(press_is_refused_while_a_motor_runs)
{
    reset();

    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    heat_on();
    run_ms(1);
    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

/* Already running heat pauses for the duration of a motion and resumes by
 * itself. It is a pause, not a cancel.
 */
TEST(element_is_cut_during_motion_then_resumes)
{
    reset();

    heat_on();
    run_ms(1);
    CHECK_EQ(fake_pin[PIN_HEATER], 1);

    motion_request(MOTION_RECLINE_UP);
    run_ms(1);
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
    CHECK(heat_is_on());                /* flag kept */

    motion_request(MOTION_NONE);
    run_ms(1);
    CHECK_EQ(fake_pin[PIN_HEATER], 1);  /* back by itself */
}

TEST(heat_turns_itself_off_after_an_hour)
{
    reset();

    heat_on();
    run_ms(HEAT_TIMEOUT_MS - 100);
    CHECK(heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 1);

    run_ms(200);
    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

/* Time spent paused by motion still counts against the auto-off. The timer
 * bounds how long the element can be live, and pausing does not make it safer.
 */
TEST(the_auto_off_clock_runs_during_a_pause)
{
    reset();

    heat_on();
    motion_request(MOTION_RECLINE_UP);
    run_ms(HEAT_TIMEOUT_MS + 100);

    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

TEST(heat_off_is_idempotent)
{
    reset();

    heat_off();
    run_ms(1);
    CHECK(!heat_is_on());

    heat_on();
    run_ms(1);
    heat_off();
    heat_off();
    run_ms(1);
    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

#if ENH_HEAT_LEVELS

/* --- levels, enhancements-spec.md §2.3 --- */

/* Each level is its share of the switching period, and the period always opens
 * with the element energised.
 */
TEST(each_level_is_its_share_of_the_period)
{
    static const int on_pct[HEAT_LEVEL_MAX + 1] = { 0, 25, 50, 75, 100 };

    for (int want = 1; want <= HEAT_LEVEL_MAX; want++) {
        int lit = 0;

        reset();
        heat_press();
        heat_arm();
        heat_set_level((uint8_t)want);
        heat_press();

        CHECK_EQ(heat_level(), want);

        /* Sample a whole period and count how much of it was energised. */
        for (uint32_t t = 0; t < HEAT_DUTY_PERIOD_MS; t += 100) {
            run_ms(100);
            lit += fake_pin[PIN_HEATER] ? 1 : 0;
        }

        CHECK_EQ(lit, on_pct[want] * (int)(HEAT_DUTY_PERIOD_MS / 100) / 100);
    }
}

/* The top level is continuous, and so byte for byte what the reference does. */
TEST(the_top_level_never_switches_off)
{
    reset();
    heat_on();

    for (uint32_t t = 0; t < HEAT_DUTY_PERIOD_MS * 2u; t += 100) {
        run_ms(100);
        CHECK_EQ(fake_pin[PIN_HEATER], 1);
    }
}

/* Switching off and on again returns to the level last used, which is the
 * whole reason a plain tap needs to ask nothing.
 */
TEST(the_level_is_remembered_across_a_switch_off)
{
    reset();

    heat_press();
    heat_arm();
    heat_set_level(2);
    heat_press();
    CHECK_EQ(heat_level(), 2);

    heat_press();                       /* off */
    run_ms(1);
    CHECK(!heat_is_on());

    heat_press();                       /* on again, no level said */
    run_ms(1);
    CHECK(heat_is_on());
    CHECK_EQ(heat_level(), 2);
}

/* Holding while already on goes straight to the choice and does not switch
 * off, restart the auto-off clock, or disturb the level.
 */
TEST(arming_while_on_only_opens_the_choice)
{
    reset();

    heat_press();
    run_ms(60000);                      /* a minute into the hour */
    CHECK(heat_is_on());

    heat_arm();
    CHECK(adjust_armed());
    CHECK_EQ(adjust_owner(), ADJUST_HEAT);
    CHECK(heat_is_on());
    CHECK_EQ(heat_level(), HEAT_LEVEL_DEFAULT);

    /* Still on the original clock: an hour from the first press, not from the
     * hold.
     */
    run_ms(HEAT_TIMEOUT_MS - 60000);
    CHECK(!heat_is_on());
}

/* Accepting closes the choice without switching heat off. */
TEST(the_owners_press_accepts_rather_than_switching_off)
{
    reset();

    heat_press();
    heat_arm();
    heat_set_level(3);
    CHECK(adjust_armed());

    heat_press();
    CHECK(!adjust_armed());
    CHECK(heat_is_on());
    CHECK_EQ(heat_level(), 3);
}

/* Switching off hands the adjuster back, so the motion buttons are motion
 * buttons again immediately.
 */
TEST(switching_off_closes_the_adjuster)
{
    reset();

    heat_press();
    heat_arm();
    CHECK(adjust_armed());

    heat_press();                       /* accept */
    heat_press();                       /* off */
    CHECK(!heat_is_on());
    CHECK(!adjust_armed());
    CHECK_EQ(adjust_bar_mask(), 0);
}

/* The lamp blinks while the buttons are borrowed and is steady once they are
 * not, so blinking means "still changeable".
 */
TEST(the_lamp_blinks_only_while_armed)
{
    int lit = 0, dark = 0;

    reset();
    heat_press();
    heat_arm();

    for (uint32_t t = 0; t < ADJUST_BLINK_MS * 4u; t += 50) {
        run_ms(50);
        if (heat_led()) lit++; else dark++;
    }

    CHECK(lit > 0);
    CHECK(dark > 0);

    heat_press();                       /* accept, closing the window */
    run_ms(1);
    CHECK(!adjust_armed());

    for (uint32_t t = 0; t < ADJUST_BLINK_MS * 4u; t += 50) {
        run_ms(50);
        CHECK(heat_led());
    }
}

#endif /* ENH_HEAT_LEVELS */

int main(void)
{
    printf("heat\n");
    RUN(button_toggles_the_element);
    RUN(press_is_refused_while_a_motor_runs);
    RUN(element_is_cut_during_motion_then_resumes);
    RUN(heat_turns_itself_off_after_an_hour);
    RUN(the_auto_off_clock_runs_during_a_pause);
    RUN(heat_off_is_idempotent);
#if ENH_HEAT_LEVELS
    RUN(each_level_is_its_share_of_the_period);
    RUN(the_top_level_never_switches_off);
    RUN(the_level_is_remembered_across_a_switch_off);
    RUN(arming_while_on_only_opens_the_choice);
    RUN(the_owners_press_accepts_rather_than_switching_off);
    RUN(switching_off_closes_the_adjuster);
    RUN(the_lamp_blinks_only_while_armed);
#endif

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
