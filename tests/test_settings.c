/* The persistent settings store, enhancements-spec.md §2.4.
 *
 * The interesting behaviour is all in how the log survives things: a reset, a
 * page that fills up, a flash that refuses. The fake in fakes.c models the real
 * constraint that a write can only clear bits, so a store that tried to rewrite
 * a record in place fails here exactly as it would on the chair.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/enhancements.h"
#include "../src/flash.h"
#include "../src/settings.h"

#define SLOTS (FLASH_PAGE_SIZE / 4u)

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

/* Nothing is written until the caller says it is a quiet moment, and the value
 * is live in RAM in the meantime.
 */
TEST(a_change_waits_for_a_quiet_moment)
{
    blank();

    settings_set_heat_level(2);
    settings_update(0);
    settings_update(0);
    CHECK_EQ(fake_flash_writes, 0);
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
    CHECK_EQ(fake_flash_writes, 1);

    save(3);
    save(3);
    CHECK_EQ(fake_flash_writes, 1);

    /* Away and back inside one quiet gap is also nothing: only the value at
     * commit time matters.
     */
    settings_set_heat_level(1);
    settings_set_heat_level(3);
    settings_update(1);
    CHECK_EQ(fake_flash_writes, 1);
}

/* Records are appended, never overwritten. A store that rewrote a slot would
 * be refused by the fake, exactly as the FMC would refuse it.
 */
TEST(records_are_appended_not_overwritten)
{
    blank();

    for (unsigned i = 0; i < 8; i++) {
        save((uint8_t)((i % 4u) + 1u));
    }

    CHECK_EQ(fake_flash_writes, 8);
    CHECK_EQ(fake_flash_erases, 0);

    /* Eight distinct slots used, in order. */
    for (unsigned i = 0; i < 8; i++) {
        CHECK(flash_read(FLASH_STORE_ADDR + i * 4u) != 0xFFFFFFFFu);
    }
    CHECK_EQ(flash_read(FLASH_STORE_ADDR + 8u * 4u), 0xFFFFFFFFu);
}

/* Filling the page erases it and starts again, and the value survives that. */
TEST(a_full_page_is_erased_and_reused)
{
    blank();

    /* Alternate so every save is a change and therefore a record. */
    for (unsigned i = 0; i < SLOTS; i++) {
        save((uint8_t)((i % 2u) + 1u));
    }

    CHECK_EQ(fake_flash_writes, SLOTS);
    CHECK_EQ(fake_flash_erases, 0);

    save(4);
    CHECK_EQ(fake_flash_erases, 1);
    CHECK_EQ(dbg.settings_erases, 1);
    CHECK_EQ(settings_heat_level(), 4);

    reboot();
    CHECK_EQ(settings_heat_level(), 4);

    /* And the page is being used from the start again. */
    CHECK(flash_read(FLASH_STORE_ADDR) != 0xFFFFFFFFu);
    CHECK_EQ(flash_read(FLASH_STORE_ADDR + 4u), 0xFFFFFFFFu);
}

/* An erased slot must not read as a record. This is what the complement in the
 * high half buys, and it is why the log needs no in-use marker.
 */
TEST(an_erased_slot_is_not_a_record)
{
    blank();
    save(2);

    reboot();
    CHECK_EQ(settings_heat_level(), 2);

    /* Slot one is still erased, and the scan stopped there rather than reading
     * 0xFFFFFFFF as a value.
     */
    CHECK_EQ(flash_read(FLASH_STORE_ADDR + 4u), 0xFFFFFFFFu);
    CHECK(settings_heat_level() != 0xFF);
}

/* A refused write is not retried. The value stays live, and the loop does not
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

/* Garbage in the store must not become a level. heat.c range-checks whatever
 * comes back, and this is the value it has to cope with.
 */
TEST(a_corrupt_record_is_ignored)
{
    blank();

    /* A word whose halves are not complements: half a write, or noise. */
    flash_write_word(FLASH_STORE_ADDR, 0x00000003u);

    reboot();
    CHECK_EQ(settings_heat_level(), 0);
}

#endif /* ENH_SETTINGS_PERSIST */

int main(void)
{
    printf("settings\n");
#if ENH_SETTINGS_PERSIST
    RUN(a_blank_store_has_nothing_to_say);
    RUN(a_saved_level_survives_a_reboot);
    RUN(the_newest_record_wins);
    RUN(a_change_waits_for_a_quiet_moment);
    RUN(rewriting_the_same_value_writes_nothing);
    RUN(records_are_appended_not_overwritten);
    RUN(a_full_page_is_erased_and_reused);
    RUN(an_erased_slot_is_not_a_record);
    RUN(a_refused_write_is_not_retried);
    RUN(a_corrupt_record_is_ignored);
#else
    printf("- disabled in this build\n");
#endif

    printf("%d checks, %d failed\n\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
