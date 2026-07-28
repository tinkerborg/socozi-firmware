/* Host-side test harness.
 *
 * The logic modules (pneumatics, motion, heat) touch no hardware registers.
 * They reach the board only through gpio.h and timing.h, so on the host we
 * link fakes for those two and the real code runs unmodified.
 *
 * Time does not pass by itself here. A test calls run_ms(), which advances
 * ms_ticks one millisecond at a time and calls tick_hook after each step, so
 * the modules see the same 1 kHz timebase they see on the chair.
 */

#ifndef HARNESS_H
#define HARNESS_H

#include <stdint.h>
#include <stdio.h>

/* --- fake gpio state --- */

/* Indexed by pin id, so PIN_PUMP etc. work directly. */
extern uint8_t  fake_pin[256];
extern uint32_t fake_shift;         /* last value clocked into the register */
extern uint32_t fake_shift_writes;  /* how many times, to catch redundant writes */

/* --- fake handset ---
 *
 * Lets a test press a button: set fake_button, let time run, read back the LED
 * bitmap the firmware sent. handset.c itself is not linked, so nothing here
 * models framing or checksums.
 */
extern uint8_t  fake_button;    /* what handset_button() reports */
extern uint32_t fake_age_ms;    /* what handset_age_ms() reports */
extern uint8_t  fake_leds;      /* last bitmap passed to handset_set_leds() */

void fakes_reset(void);

/* --- fake flash ---
 *
 * One page, behaving the way the real thing does: a write can only clear bits,
 * and only an erase puts them back. A store that tried to rewrite a record in
 * place would fail here exactly as it would on the chair.
 *
 * fakes_reset() does NOT erase it. Persistence across a reset is the whole
 * point, so a test that wants a blank part says so with fake_flash_wipe().
 */
extern uint32_t fake_flash[];           /* FLASH_PAGE_SIZE / 4 words */
extern uint32_t fake_flash_writes;
extern uint32_t fake_flash_erases;
extern int      fake_flash_fail;        /* refuse every write and erase */

void fake_flash_wipe(void);             /* back to a fresh, erased page */

/* --- fake time --- */

extern void (*tick_hook)(void);
void run_ms(uint32_t ms);

/* --- assertions --- */

extern int tests_run, tests_failed;

#define CHECK(cond)                                                           \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            tests_failed++;                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                     \
    } while (0)

#define CHECK_EQ(got, want)                                                   \
    do {                                                                      \
        long g = (long)(got), w = (long)(want);                               \
        tests_run++;                                                          \
        if (g != w) {                                                         \
            tests_failed++;                                                   \
            printf("  FAIL %s:%d  %s: got %ld (0x%lx), want %ld (0x%lx)\n",   \
                   __FILE__, __LINE__, #got, g, g, w, w);                     \
        }                                                                     \
    } while (0)

#define TEST(name) static void name(void)
#define RUN(name)  do { printf("- %s\n", #name); name(); } while (0)

#endif /* HARNESS_H */
