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

void fakes_reset(void);

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
