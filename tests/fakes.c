/* Host stand-ins for the two hardware seams, gpio.h and timing.h, plus the
 * debug block that main.c normally defines.
 *
 * Nothing here models the board. pin_write just records a level, so a test can
 * ask "is the pump energised right now" and get an answer without a chair.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/gpio.h"
#include "../src/timing.h"

volatile struct debug_block dbg;

uint8_t  fake_pin[256];
uint32_t fake_shift;
uint32_t fake_shift_writes;

volatile uint32_t ms_ticks;

void (*tick_hook)(void);

int tests_run, tests_failed;

void fakes_reset(void)
{
    for (int i = 0; i < 256; i++) {
        fake_pin[i] = 0;
    }
    fake_shift        = 0;
    fake_shift_writes = 0;
    ms_ticks          = 0;
    tick_hook         = 0;
}

void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        ms_ticks++;
        if (tick_hook) {
            tick_hook();
        }
    }
}

/* --- gpio.h --- */

void gpio_init(void) {}

void pin_write(uint8_t id, uint32_t level)
{
    fake_pin[id] = level ? 1 : 0;
}

void all_outputs_off(void)
{
    for (int i = 0; i < 256; i++) {
        fake_pin[i] = 0;
    }
}

void shift_write(uint32_t value)
{
    fake_shift = value;
    fake_shift_writes++;
}

uint32_t gpio_istat_a(void) { return 0; }
uint32_t gpio_istat_b(void) { return 0; }
uint32_t gpio_istat_c(void) { return 0; }
uint32_t gpio_octl_a(void) { return 0; }
uint32_t gpio_octl_b(void) { return 0; }
uint32_t gpio_octl_c(void) { return 0; }

/* --- timing.h --- */

void timing_init(void) {}

void delay_ms(uint32_t ms) { run_ms(ms); }

int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    (void)reg; (void)mask; (void)ms;
    return 1;
}
