/* Pneumatics: massage pattern, pump gating, lumbar state machine.
 *
 * The pattern below is restated independently of pneumatics.c on purpose. It
 * comes from the factory table at 0x08004A1B, read byte for byte, so if
 * somebody edits the table in the firmware this test disagrees with them.
 */

#include "harness.h"
#include "../src/debug.h"       /* MOTION_* */
#include "../src/gpio.h"
#include "../src/handset.h"
#include "../src/motion.h"
#include "../src/pneumatics.h"

#define VALVE_TOP    0x01
#define VALVE_MID    0x02
#define VALVE_VENT   0x04
#define VALVE_BOTTOM 0x08

#define TICK_MS      100
#define PUMP_DELAY   300
#define VENT_MS      120000

struct step { uint8_t bits; uint16_t ticks; };

static const struct step expected[] = {
    { 0x01, 40 }, { 0x03, 40 }, { 0x0A, 40 }, { 0x07, 40 },
    { 0x05, 40 }, { 0x05, 40 }, { 0x03, 40 }, { 0x0A, 40 },
    { 0x07, 40 }, { 0x05, 40 }, { 0x04, 80 }, { 0x01, 40 },
    { 0x03, 40 },
    { 0x00,  5 }, { 0x03, 10 }, { 0x00,  5 }, { 0x03, 10 },
    { 0x00,  5 }, { 0x03, 10 }, { 0x00,  5 }, { 0x03, 20 },
    { 0x00,  5 }, { 0x03, 40 }, { 0x00,  5 }, { 0x03, 80 },
    { 0x00, 40 },
    { 0x08, 80 }, { 0x0A, 40 }, { 0x04,  5 }, { 0x0A, 10 },
    { 0x04,  5 }, { 0x0A, 10 }, { 0x04,  5 }, { 0x0A, 20 },
    { 0x04,  5 }, { 0x0A, 40 }, { 0x04,  5 }, { 0x0A, 80 },
    { 0x04, 80 },
};

#define NSTEPS (int)(sizeof(expected) / sizeof(expected[0]))

static void tick(void) { pneumatics_update(); }

/* Put every module back to a known state. The firmware's statics live for the
 * life of the process, so each test has to do this itself.
 */
static void reset(void)
{
    motion_request(MOTION_NONE);
    motion_update(0);
    pneumatics_shutdown();
    fakes_reset();
    tick_hook = tick;
    run_ms(VENT_MS + 10);       /* let the shutdown vent finish */
    CHECK_EQ(fake_shift, 0);
}

static void press(uint8_t button)
{
    pneumatics_button(button);
    run_ms(1);
}

TEST(massage_walks_the_factory_pattern)
{
    reset();
    press(HS_MASSAGE);

    for (int i = 0; i < NSTEPS; i++) {
        CHECK_EQ(fake_shift, expected[i].bits);

        /* One tick short of the step's duration, we must still be on it. */
        run_ms((uint32_t)expected[i].ticks * TICK_MS - 1);
        CHECK_EQ(fake_shift, expected[i].bits);

        run_ms(1);              /* crossing the boundary advances */
    }

    /* And it loops. */
    CHECK_EQ(fake_shift, expected[0].bits);
}

TEST(massage_cycle_is_1185_ticks)
{
    uint32_t total = 0;
    for (int i = 0; i < NSTEPS; i++) {
        total += expected[i].ticks;
    }
    CHECK_EQ(total, 1185);
    CHECK_EQ(total * TICK_MS, 118500);   /* just under two minutes */
}

TEST(pump_waits_out_the_delay_then_runs)
{
    reset();
    press(HS_MASSAGE);          /* step 0 opens the top cell */

    CHECK_EQ(fake_shift, VALVE_TOP);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    run_ms(PUMP_DELAY - 2);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    run_ms(2);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);
}

/* The regression that made the massage feel full of pauses: restarting the
 * delay on every valve change starves the short steps. The delay is only
 * allowed to restart after a step with no cell open.
 */
TEST(pump_stays_on_across_valve_changes)
{
    reset();
    press(HS_MASSAGE);
    run_ms(PUMP_DELAY);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);

    /* Steps 0 to 9 all keep at least one cell open (step 10 is the vent, which
     * legitimately stops the pump). Walk the boundaries between them and check
     * the pump never drops.
     */
    uint32_t elapsed = PUMP_DELAY;
    for (int i = 0; i < 9; i++) {
        uint32_t want = (uint32_t)expected[i].ticks * TICK_MS;
        run_ms(want - elapsed);
        elapsed = 0;
        CHECK_EQ(fake_shift, expected[i + 1].bits);
        CHECK_EQ(fake_pin[PIN_PUMP], 1);   /* still running after the change */
    }
}

/* Step 13 closes everything for 500 ms, so the following pulse step really
 * does start with the pump off. That part is factory behaviour, not a bug.
 */
TEST(all_closed_step_resets_the_pump_delay)
{
    reset();
    press(HS_MASSAGE);

    uint32_t to_step13 = 0;
    for (int i = 0; i < 13; i++) {
        to_step13 += (uint32_t)expected[i].ticks * TICK_MS;
    }
    run_ms(to_step13);

    CHECK_EQ(fake_shift, 0x00);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    run_ms(5 * TICK_MS);                 /* into step 14, middle cell open */
    CHECK_EQ(fake_shift, 0x03);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);     /* delay restarted */

    run_ms(PUMP_DELAY);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);
}

TEST(motion_pauses_massage_without_cancelling_it)
{
    reset();
    press(HS_MASSAGE);
    run_ms(PUMP_DELAY);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);

    motion_request(MOTION_RECLINE_UP);
    run_ms(1);
    CHECK_EQ(fake_shift, 0);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);
    CHECK(pneumatics_massage_on());      /* paused, not cancelled */

    motion_request(MOTION_NONE);
    run_ms(1);
    CHECK(fake_shift != 0);
}

TEST(massage_stops_after_fifteen_minutes)
{
    reset();
    press(HS_MASSAGE);
    run_ms(15u * 60u * 1000u - 10);
    CHECK(pneumatics_massage_on());

    run_ms(20);
    CHECK(!pneumatics_massage_on());
    CHECK_EQ(fake_shift, VALVE_VENT);    /* falls into the vent */
}

/* Get lumbar inflated and holding, whichever way this build does it: a second
 * press in the reference, a long-enough release with ENH_LUMBAR_HOLD_SET.
 */
#if ENH_LUMBAR_HOLD_SET
/* A release, plus the tick that lets the new state reach the valves. press()
 * already does this for a press; nothing does it for a release.
 */
static void lumbar_release(void)
{
    pneumatics_lumbar_release();
    run_ms(1);
}
#endif

static void lumbar_to_hold(void)
{
    press(HS_LUMBAR);
#if ENH_LUMBAR_HOLD_SET
    run_ms(600);                         /* past LUMBAR_SET_MS */
    lumbar_release();
#else
    press(HS_LUMBAR);
#endif
}

/* Inflate, hold, deflate. However it is reached, the deflate step must open the
 * exhaust *alone*. Adding the bladder bit turns the pump on and inflates, which
 * is exactly the bug this catches.
 */
TEST(lumbar_cycles_inflate_hold_deflate)
{
    reset();

    press(HS_LUMBAR);
    CHECK_EQ(fake_shift, VALVE_BOTTOM);
    CHECK(pneumatics_lumbar_lit());
    run_ms(PUMP_DELAY);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);

#if ENH_LUMBAR_HOLD_SET
    /* Held long enough to be deliberate, so the release stops it here. */
    run_ms(600);
    lumbar_release();
#else
    press(HS_LUMBAR);
#endif
    CHECK_EQ(fake_shift, 0);             /* closed valve traps the air */
    CHECK_EQ(fake_pin[PIN_PUMP], 0);
    CHECK(pneumatics_lumbar_lit());

    press(HS_LUMBAR);
    CHECK_EQ(fake_shift, VALVE_VENT);    /* exhaust only, no bladder bit */
    CHECK_EQ(fake_pin[PIN_PUMP], 0);
    CHECK(!pneumatics_lumbar_lit());

    run_ms(VENT_MS);
    CHECK_EQ(fake_shift, 0);
}

TEST(lumbar_inflation_has_a_ceiling)
{
    reset();
    press(HS_LUMBAR);
    CHECK_EQ(fake_shift, VALVE_BOTTOM);

    run_ms(200 * TICK_MS);               /* the factory table's 200 ticks */
    CHECK_EQ(fake_shift, 0);             /* dropped to hold by itself */
    CHECK_EQ(fake_pin[PIN_PUMP], 0);
    CHECK(pneumatics_lumbar_lit());      /* holding, LED still lit */
}

TEST(massage_and_lumbar_are_exclusive)
{
    reset();

    press(HS_LUMBAR);
    CHECK(pneumatics_lumbar_lit());

    press(HS_MASSAGE);
    CHECK(pneumatics_massage_on());
    CHECK(!pneumatics_lumbar_lit());

    press(HS_LUMBAR);
    CHECK(!pneumatics_massage_on());
    CHECK(pneumatics_lumbar_lit());
}

TEST(shutdown_vents_everything)
{
    reset();
    press(HS_MASSAGE);
    run_ms(1000);

    pneumatics_shutdown();
    run_ms(1);
    CHECK(!pneumatics_massage_on());
    CHECK(!pneumatics_lumbar_lit());
    CHECK_EQ(fake_shift, VALVE_VENT);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    run_ms(VENT_MS);
    CHECK_EQ(fake_shift, 0);
}

/* Venting versus motion.
 *
 * The motion pause is a pump-current budget. The exhaust drives no pump, so
 * anything that is only exhausting keeps running through a motion, while
 * anything that inflates stops. Confirmed against the factory firmware on the
 * chair: it keeps deflating when a motion button is pressed mid-vent.
 *
 * The converse matters just as much. A motion must never *start* a vent.
 */

TEST(a_vent_keeps_running_through_a_motion)
{
    reset();
    press(HS_MASSAGE);
    run_ms(1000);

    pneumatics_shutdown();
    run_ms(1);
    CHECK_EQ(fake_shift, VALVE_VENT);

    motion_request(MOTION_RECLINE_UP);
    run_ms(5000);
    CHECK_EQ(fake_shift, VALVE_VENT);       /* still deflating */
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    motion_request(MOTION_NONE);
    run_ms(1);
    CHECK_EQ(fake_shift, VALVE_VENT);       /* neither cut short nor restarted */

    /* 5002 ms of the vent has elapsed. The motion must not have pushed the
     * deadline out, or the exhaust stays open for the length of the move.
     */
    run_ms(VENT_MS - 5002 - 1);
    CHECK_EQ(fake_shift, VALVE_VENT);
    run_ms(2);
    CHECK_EQ(fake_shift, 0);
}

TEST(a_motion_does_not_start_a_vent)
{
    reset();                                /* nothing open, nothing pending */

    motion_request(MOTION_RECLINE_UP);
    run_ms(2000);
    CHECK_EQ(fake_shift, 0);

    motion_request(MOTION_NONE);
    run_ms(1);
    CHECK_EQ(fake_shift, 0);
}

TEST(massage_takes_the_valves_from_a_pending_vent)
{
    reset();
    pneumatics_shutdown();
    run_ms(1);
    CHECK_EQ(fake_shift, VALVE_VENT);

    /* Massage takes ownership mid-vent, so the vent is abandoned rather than
     * left pending. A stale flag here used to reopen the exhaust on the next
     * motion press.
     */
    press(HS_MASSAGE);
    run_ms(1000);

    motion_request(MOTION_RECLINE_UP);
    run_ms(2000);
    CHECK_EQ(fake_shift, 0);

    motion_request(MOTION_NONE);
    run_ms(1);
}

TEST(lumbar_deflate_keeps_running_through_a_motion)
{
    reset();
    lumbar_to_hold();
    press(HS_LUMBAR);                       /* deflate */
    CHECK_EQ(fake_shift, VALVE_VENT);

    motion_request(MOTION_RECLINE_UP);
    run_ms(3000);
    CHECK_EQ(fake_shift, VALVE_VENT);
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    motion_request(MOTION_NONE);
    run_ms(1);
    CHECK_EQ(fake_shift, VALVE_VENT);
}

TEST(lumbar_inflation_still_pauses_for_a_motion)
{
    reset();
    press(HS_LUMBAR);                       /* inflate */
    run_ms(PUMP_DELAY + 10);
    CHECK_EQ(fake_shift, VALVE_BOTTOM);
    CHECK_EQ(fake_pin[PIN_PUMP], 1);

    motion_request(MOTION_RECLINE_UP);
    run_ms(10);
    CHECK_EQ(fake_shift, 0);                /* inflation is what costs current */
    CHECK_EQ(fake_pin[PIN_PUMP], 0);

    motion_request(MOTION_NONE);
    run_ms(10);
    CHECK_EQ(fake_shift, VALVE_BOTTOM);     /* resumes */
}

int main(void)
{
    printf("pneumatics\n");
    RUN(massage_walks_the_factory_pattern);
    RUN(massage_cycle_is_1185_ticks);
    RUN(pump_waits_out_the_delay_then_runs);
    RUN(pump_stays_on_across_valve_changes);
    RUN(all_closed_step_resets_the_pump_delay);
    RUN(motion_pauses_massage_without_cancelling_it);
    RUN(massage_stops_after_fifteen_minutes);
    RUN(lumbar_cycles_inflate_hold_deflate);
    RUN(lumbar_inflation_has_a_ceiling);
    RUN(massage_and_lumbar_are_exclusive);
    RUN(shutdown_vents_everything);
    RUN(a_vent_keeps_running_through_a_motion);
    RUN(a_motion_does_not_start_a_vent);
    RUN(massage_takes_the_valves_from_a_pending_vent);
    RUN(lumbar_deflate_keeps_running_through_a_motion);
    RUN(lumbar_inflation_still_pauses_for_a_motion);

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
