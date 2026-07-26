/* Motion: relay sequencing, stall detection, timeout, fault latching.
 *
 * The sequencing is the safety-critical part. The direction relay must settle
 * before any current flows, so polarity never switches under load.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/gpio.h"
#include "../src/motion.h"

static uint32_t current;        /* what motion_update sees this pass */

static void tick(void) { motion_update(current); }

static void reset(void)
{
    motion_request(MOTION_NONE);
    motion_update(0);
    fakes_reset();
    current   = 0;
    tick_hook = tick;
    dbg.stalls = 0;
    dbg.stops  = 0;
}

static int any_recline_pin(void)
{
    return fake_pin[PIN_RECLINE_A] || fake_pin[PIN_RECLINE_B]
        || fake_pin[PIN_RECLINE_C] || fake_pin[PIN_RECLINE_D];
}

TEST(nothing_is_energised_during_the_settle)
{
    reset();
    motion_request(MOTION_RECLINE_UP);

    run_ms(MOTION_SETTLE_MS - 1);
    CHECK(!any_recline_pin());
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 0);

    /* But the request is visible immediately, so the handset LED lights. */
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
}

TEST(recline_engages_one_pin_then_the_other)
{
    reset();
    motion_request(MOTION_RECLINE_UP);

    run_ms(MOTION_SETTLE_MS);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_B], 0);       /* staggered */

    run_ms(MOTION_STAGGER_MS);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_B], 1);

    /* The other direction's pair is never touched. */
    CHECK_EQ(fake_pin[PIN_RECLINE_C], 0);
    CHECK_EQ(fake_pin[PIN_RECLINE_D], 0);
}

TEST(headrest_sets_direction_before_enabling)
{
    reset();
    motion_request(MOTION_HEADREST_DOWN);

    CHECK_EQ(fake_pin[PIN_HEADREST_DIR], 1);    /* set immediately */
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 0);     /* but not energised */

    run_ms(MOTION_SETTLE_MS);
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 1);

    reset();
    motion_request(MOTION_HEADREST_UP);
    CHECK_EQ(fake_pin[PIN_HEADREST_DIR], 0);
}

TEST(flatten_drives_both_axes)
{
    reset();
    motion_request(MOTION_FLATTEN);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    CHECK_EQ(fake_pin[PIN_RECLINE_C], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_D], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_EN], 1);
    CHECK_EQ(fake_pin[PIN_HEADREST_DIR], 1);
}

/* Two directions must never be energised at once, even for one pass. */
TEST(direction_change_passes_through_a_full_stop)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);

    motion_request(MOTION_RECLINE_DOWN);
    CHECK(!any_recline_pin());                  /* everything dropped */

    run_ms(MOTION_SETTLE_MS);
    CHECK_EQ(fake_pin[PIN_RECLINE_C], 1);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 0);
}

TEST(repeated_requests_do_not_restart_the_sequence)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);

    motion_request(MOTION_RECLINE_UP);          /* button still held */
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);       /* not dropped */
}

TEST(release_stops_immediately)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    motion_request(MOTION_NONE);
    CHECK(!any_recline_pin());
    CHECK_EQ(motion_active(), MOTION_NONE);
}

TEST(sustained_overcurrent_stalls)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    current = MOTION_STALL_ADC + 1;
    run_ms(MOTION_STALL_MS - 100);
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);   /* still riding it out */

    run_ms(200);
    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());
    CHECK_EQ(dbg.stalls, 1);
    CHECK_EQ(dbg.stops, 1);
}

/* Inrush hits stall level for a single sample. It must not trip the cutoff. */
TEST(brief_inrush_does_not_stall)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    current = 400;                  /* measured inrush range is 359 to 403 */
    run_ms(20);
    current = 120;                  /* measured running range is 35 to 173 */
    run_ms(MOTION_STALL_MS * 2);

    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    CHECK_EQ(dbg.stalls, 0);
}

TEST(a_failed_conversion_is_not_a_stall)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);

    current = 0xFFFFFFFF;           /* ADC timed out, no opinion */
    run_ms(MOTION_STALL_MS * 2);

    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    CHECK_EQ(dbg.stalls, 0);
}

TEST(motion_times_out)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_TIMEOUT_MS + 100);

    CHECK_EQ(motion_active(), MOTION_NONE);
    CHECK(!any_recline_pin());
    CHECK_EQ(dbg.stops, 1);
    CHECK_EQ(dbg.stalls, 0);
}

/* After a stall, holding the button must not immediately re-drive into
 * whatever stopped it. Releasing clears the latch.
 */
TEST(a_fault_latches_until_release)
{
    reset();
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);
    current = MOTION_STALL_ADC + 1;
    run_ms(MOTION_STALL_MS + 100);
    CHECK_EQ(motion_active(), MOTION_NONE);

    current = 0;
    motion_request(MOTION_RECLINE_UP);          /* still held */
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);
    CHECK_EQ(motion_active(), MOTION_NONE);     /* refused */
    CHECK(!any_recline_pin());

    motion_request(MOTION_NONE);                /* released, latch clears */
    motion_request(MOTION_RECLINE_UP);
    run_ms(MOTION_SETTLE_MS + MOTION_STAGGER_MS);
    CHECK_EQ(motion_active(), MOTION_RECLINE_UP);
    CHECK_EQ(fake_pin[PIN_RECLINE_A], 1);
}

int main(void)
{
    printf("motion\n");
    RUN(nothing_is_energised_during_the_settle);
    RUN(recline_engages_one_pin_then_the_other);
    RUN(headrest_sets_direction_before_enabling);
    RUN(flatten_drives_both_axes);
    RUN(direction_change_passes_through_a_full_stop);
    RUN(repeated_requests_do_not_restart_the_sequence);
    RUN(release_stops_immediately);
    RUN(sustained_overcurrent_stalls);
    RUN(brief_inrush_does_not_stall);
    RUN(a_failed_conversion_is_not_a_stall);
    RUN(motion_times_out);
    RUN(a_fault_latches_until_release);

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
