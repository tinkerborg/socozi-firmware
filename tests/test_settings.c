/* The persistent settings store, enhancements-spec.md §2.4.
 *
 * The interesting behaviour is all in how the log survives things: a reset, a
 * page that fills up, a flash that refuses, a write torn in half. The fake in
 * fakes.c models the real constraint that a write can only clear bits, so a
 * store that tried to rewrite a record in place fails here exactly as it would
 * on the chair.
 *
 * Records are counted with `dbg.settings_writes`, never with
 * `fake_flash_writes`: the latter counts *words*, and one record is several.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/enhancements.h"
#include "../src/flash.h"
#include "../src/settings.h"

/* Must match settings.c. Restated rather than shared so that changing the
 * record size there without thinking about the log shows up here.
 */
#define RECORD_BYTES 32u
#define RECORDS      (FLASH_PAGE_SIZE / RECORD_BYTES)

#define RECORD_ADDR(i) (FLASH_STORE_ADDR + (i) * RECORD_BYTES)

/* Nothing below exists in a reference build: the store is compiled out, and so
 * are the debug counters these assert on.
 */
#if ENH_SETTINGS_PERSIST

/* A power cycle: the part comes up again with flash exactly as it was. */
static void reboot(void)
{
    settings_init();
}

/* A fresh, never-written part. */
static void blank(void)
{
    fake_flash_wipe();
    dbg.settings_writes = 0;
    dbg.settings_erases = 0;
    dbg.settings_errors = 0;
    settings_init();
}

/* Set a value and let it commit. */
static void save(uint8_t level)
{
    settings_set_heat_level(level);
    settings_update(1);
}

static int record_erased(unsigned i)
{
    for (unsigned w = 0; w < RECORD_BYTES / 4u; w++) {
        if (flash_read(RECORD_ADDR(i) + w * 4u) != 0xFFFFFFFFu) {
            return 0;
        }
    }

    return 1;
}

TEST(a_blank_store_has_nothing_to_say)
{
    blank();
    CHECK_EQ(settings_heat_level(), 0);
}

TEST(a_saved_level_survives_a_reboot)
{
    blank();
    save(3);
    CHECK_EQ(settings_heat_level(), 3);

    reboot();
    CHECK_EQ(settings_heat_level(), 3);
}

/* The last record written is the live one, not the first. */
TEST(the_newest_record_wins)
{
    blank();
    save(1);
    save(4);
    save(2);

    reboot();
    CHECK_EQ(settings_heat_level(), 2);
    CHECK_EQ(dbg.settings_writes, 3);
}

/* Everything shares one record, so a change to any of it commits all of it.
 * Keeping the values independent is the point of the byte layout.
 */
TEST(the_settings_do_not_disturb_each_other)
{
    blank();

    settings_set_heat_level(3);
    settings_set_lumbar_level(120);
    settings_set_massage_level(2);
    settings_update(1);

    CHECK_EQ(dbg.settings_writes, 1);        /* one record, not three */

    reboot();
    CHECK_EQ(settings_heat_level(), 3);
    CHECK_EQ(settings_lumbar_level(), 120);
    CHECK_EQ(settings_massage_level(), 2);

    /* Changing one leaves the others exactly where they were. */
    settings_set_heat_level(1);
    settings_update(1);

    reboot();
    CHECK_EQ(settings_heat_level(), 1);
    CHECK_EQ(settings_lumbar_level(), 120);
    CHECK_EQ(settings_massage_level(), 2);
}

/* Every byte can be 0xFF, so a full record must still not look erased. That is
 * what the check byte buys.
 */
TEST(a_record_of_all_ones_is_not_an_erased_one)
{
    blank();

    settings_set_heat_level(0xFF);
    settings_set_lumbar_level(0xFF);
    settings_set_massage_level(0xFF);
    settings_update(1);

    reboot();
    CHECK_EQ(settings_heat_level(), 0xFF);
    CHECK_EQ(settings_lumbar_level(), 0xFF);
    CHECK_EQ(settings_massage_level(), 0xFF);
}

/* --- presets, §2.8 --- */

/* A slot keeps its own levels, and they are not the loose ones. */
TEST(a_slot_carries_its_own_levels)
{
    struct settings_preset p = { 12, 34, 0x05, 2, 88, 3 };
    struct settings_preset got;

    blank();

    settings_set_heat_level(4);
    settings_set_preset(1, &p);
    settings_update(1);

    reboot();

    settings_get_preset(1, &got);
    CHECK_EQ(got.recline, 12);
    CHECK_EQ(got.headrest, 34);
    CHECK_EQ(got.flags, 0x05);
    CHECK_EQ(got.heat, 2);
    CHECK_EQ(got.lumbar, 88);
    CHECK_EQ(got.massage, 3);

    /* The loose level is untouched by any of that. */
    CHECK_EQ(settings_heat_level(), 4);
}

/* Slots do not bleed into each other. */
TEST(slots_are_independent)
{
    struct settings_preset a = { 1, 2, 3, 1, 4, 1 };
    struct settings_preset b = { 9, 8, 7, 4, 6, 4 };
    struct settings_preset got;

    blank();

    settings_set_preset(0, &a);
    settings_set_preset(3, &b);
    settings_update(1);

    reboot();

    settings_get_preset(0, &got);
    CHECK_EQ(got.recline, 1);
    CHECK_EQ(got.massage, 1);

    settings_get_preset(3, &got);
    CHECK_EQ(got.recline, 9);
    CHECK_EQ(got.massage, 4);

    /* And the ones nobody wrote are still empty. */
    CHECK(!settings_preset_used(1));
    CHECK(!settings_preset_used(2));
}

/* All zeros is a legitimate preset — flat, everything off — so "written" has to
 * be tracked separately from "non-zero".
 */
TEST(an_all_zero_preset_still_counts_as_written)
{
    struct settings_preset zero = { 0, 0, 0, 0, 0, 0 };

    blank();
    CHECK(!settings_preset_used(2));

    settings_set_preset(2, &zero);
    settings_update(1);

    reboot();
    CHECK(settings_preset_used(2));
}

/* --- the log itself --- */

/* Nothing is written until the caller says it is a quiet moment, and the value
 * is live in RAM in the meantime.
 */
TEST(a_change_waits_for_a_quiet_moment)
{
    blank();

    settings_set_heat_level(2);
    settings_update(0);
    settings_update(0);
    CHECK_EQ(dbg.settings_writes, 0);
    CHECK_EQ(settings_heat_level(), 2);      /* live immediately */

    /* And a reboot before the commit loses it, which is the accepted cost. */
    reboot();
    CHECK_EQ(settings_heat_level(), 0);
}

/* Setting the value that is already stored writes nothing. Picking a level and
 * changing back should not cost a record.
 */
TEST(rewriting_the_same_value_writes_nothing)
{
    blank();
    save(3);
    CHECK_EQ(dbg.settings_writes, 1);

    save(3);
    save(3);
    CHECK_EQ(dbg.settings_writes, 1);

    /* Away and back inside one quiet gap is also nothing: only the value at
     * commit time matters.
     */
    settings_set_heat_level(1);
    settings_set_heat_level(3);
    settings_update(1);
    CHECK_EQ(dbg.settings_writes, 1);
}

/* Records are appended, never overwritten. A store that rewrote one would be
 * refused by the fake, exactly as the FMC would refuse it.
 */
TEST(records_are_appended_not_overwritten)
{
    blank();

    for (unsigned i = 0; i < 8; i++) {
        save((uint8_t)((i % 4u) + 1u));
    }

    CHECK_EQ(dbg.settings_writes, 8);
    CHECK_EQ(fake_flash_erases, 0);

    for (unsigned i = 0; i < 8; i++) {
        CHECK(!record_erased(i));
    }
    CHECK(record_erased(8));
}

/* Filling the page erases it and starts again, and the values survive that:
 * one record is the whole state, so there is nothing else to lose.
 */
TEST(a_full_page_is_erased_and_reused)
{
    blank();

    /* Alternate so every save is a change and therefore a record. */
    for (unsigned i = 0; i < RECORDS; i++) {
        save((uint8_t)((i % 2u) + 1u));
    }

    CHECK_EQ(dbg.settings_writes, RECORDS);
    CHECK_EQ(fake_flash_erases, 0);

    save(4);
    CHECK_EQ(fake_flash_erases, 1);
    CHECK_EQ(dbg.settings_erases, 1);
    CHECK_EQ(settings_heat_level(), 4);

    reboot();
    CHECK_EQ(settings_heat_level(), 4);

    /* And the page is being used from the start again. */
    CHECK(!record_erased(0));
    CHECK(record_erased(1));
}

/* An erased record must not read as a real one. That is what the check byte
 * buys, and it is why the log needs no in-use marker.
 */
TEST(an_erased_record_is_not_a_record)
{
    blank();
    save(2);

    reboot();
    CHECK_EQ(settings_heat_level(), 2);

    CHECK(record_erased(1));
    CHECK(settings_heat_level() != 0xFF);
}

/* A power loss part way through a multi-word write leaves a record that is
 * neither erased nor valid. It must be stepped over, not read and not written
 * into — writing into it would fail, since flash only clears bits.
 */
TEST(a_torn_record_is_stepped_over)
{
    blank();
    save(2);

    /* Half a record in the next slot: first word only. */
    flash_write_word(RECORD_ADDR(1), 0x000000AAu);

    reboot();
    CHECK_EQ(settings_heat_level(), 2);      /* the good one still wins */

    /* The next append goes past the wreckage rather than into it. */
    save(5);
    CHECK_EQ(dbg.settings_writes, 2);
    CHECK(!record_erased(2));

    reboot();
    CHECK_EQ(settings_heat_level(), 5);
}

/* A refused write is not retried. The values stay live, and the loop does not
 * spend the rest of its life hammering a page that has said no.
 */
TEST(a_refused_write_is_not_retried)
{
    blank();

    fake_flash_fail = 1;
    save(3);

    CHECK_EQ(dbg.settings_errors, 1);
    CHECK_EQ(settings_heat_level(), 3);      /* still live in RAM */

    settings_update(1);
    settings_update(1);
    CHECK_EQ(dbg.settings_errors, 1);        /* and no second attempt */

    fake_flash_fail = 0;
}

/* Garbage must not become a setting. Whoever reads a value range-checks it, and
 * this is the record they have to cope with.
 */
TEST(a_corrupt_record_is_ignored)
{
    blank();

    /* Right length, wrong check byte. */
    for (unsigned w = 0; w < RECORD_BYTES / 4u; w++) {
        flash_write_word(RECORD_ADDR(0) + w * 4u, 0x00000003u);
    }

    reboot();
    CHECK_EQ(settings_heat_level(), 0);      /* rejected outright */
    CHECK_EQ(dbg.settings_writes, 0);
}

#endif /* ENH_SETTINGS_PERSIST */

int main(void)
{
    printf("settings\n");
#if ENH_SETTINGS_PERSIST
    RUN(a_blank_store_has_nothing_to_say);
    RUN(a_saved_level_survives_a_reboot);
    RUN(the_newest_record_wins);
    RUN(the_settings_do_not_disturb_each_other);
    RUN(a_record_of_all_ones_is_not_an_erased_one);
    RUN(a_slot_carries_its_own_levels);
    RUN(slots_are_independent);
    RUN(an_all_zero_preset_still_counts_as_written);
    RUN(a_change_waits_for_a_quiet_moment);
    RUN(rewriting_the_same_value_writes_nothing);
    RUN(records_are_appended_not_overwritten);
    RUN(a_full_page_is_erased_and_reused);
    RUN(an_erased_record_is_not_a_record);
    RUN(a_torn_record_is_stepped_over);
    RUN(a_refused_write_is_not_retried);
    RUN(a_corrupt_record_is_ignored);
#else
    printf("- disabled in this build\n");
#endif

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
