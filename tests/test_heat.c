/* Heat: toggle, the power budget interaction, and the auto-off.
 *
 * There is no temperature sensor on this board, so the 60 minute timer is the
 * only thing bounding a stuck-on heater. It is not optional.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/gpio.h"
#include "../src/heat.h"
#include "../src/motion.h"

#define HEAT_TIMEOUT_MS (60u * 60u * 1000u)

static void tick(void)
{
    motion_update(0);
    heat_update();
}

static void reset(void)
{
    motion_request(MOTION_NONE);
    motion_update(0);
    heat_off();
    fakes_reset();
    tick_hook = tick;
}

TEST(button_toggles_the_element)
{
    reset();

    heat_button();
    run_ms(1);
    CHECK(heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 1);

    heat_button();
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

    heat_button();
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

    heat_button();
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

    heat_button();
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

    heat_button();
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

    heat_button();
    run_ms(1);
    heat_off();
    heat_off();
    run_ms(1);
    CHECK(!heat_is_on());
    CHECK_EQ(fake_pin[PIN_HEATER], 0);
}

int main(void)
{
    printf("heat\n");
    RUN(button_toggles_the_element);
    RUN(press_is_refused_while_a_motor_runs);
    RUN(element_is_cut_during_motion_then_resumes);
    RUN(heat_turns_itself_off_after_an_hour);
    RUN(the_auto_off_clock_runs_during_a_pause);
    RUN(heat_off_is_idempotent);

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
