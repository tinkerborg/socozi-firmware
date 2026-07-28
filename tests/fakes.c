/* Host stand-ins for the two hardware seams, gpio.h and timing.h, plus the
 * debug block that main.c normally defines.
 *
 * Nothing here models the board. pin_write just records a level, so a test can
 * ask "is the pump energised right now" and get an answer without a chair.
 */

#include "harness.h"
#include "../src/debug.h"
#include "../src/flash.h"
#include "../src/gpio.h"
#include "../src/handset.h"
#include "../src/timing.h"

volatile struct debug_block dbg;

uint8_t  fake_pin[256];
uint32_t fake_shift;
uint32_t fake_shift_writes;

uint8_t  fake_button;
uint32_t fake_age_ms;
uint8_t  fake_leds;

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
    fake_button       = HS_NONE;
    fake_age_ms       = 0;
    fake_leds         = 0;
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

/* --- handset.h ---
 *
 * Only the three entry points control.c uses. The rest of handset.h is framing
 * and USART access, which no test links.
 */

uint8_t  handset_button(void) { return fake_button; }
uint32_t handset_age_ms(void) { return fake_age_ms; }

void handset_set_leds(uint8_t bits) { fake_leds = bits; }

/* --- flash.h ---
 *
 * Modelled honestly rather than as a byte array: a write ANDs, because NOR
 * flash can only clear bits, and programming a word that is not erased is an
 * error rather than a silent overwrite. Getting that wrong is the easiest way
 * to write a store that passes on the host and corrupts itself on the chair.
 */

uint32_t fake_flash[FLASH_PAGE_SIZE / 4];
uint32_t fake_flash_writes;
uint32_t fake_flash_erases;
int      fake_flash_fail;

static uint32_t *flash_slot(uint32_t addr)
{
    uint32_t off = addr - FLASH_STORE_ADDR;

    if (addr < FLASH_STORE_ADDR || off >= FLASH_PAGE_SIZE || (off & 3u)) {
        return 0;
    }

    return &fake_flash[off / 4];
}

void fake_flash_wipe(void)
{
    for (unsigned i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        fake_flash[i] = 0xFFFFFFFFu;
    }
    fake_flash_writes = 0;
    fake_flash_erases = 0;
    fake_flash_fail   = 0;
}

uint32_t flash_read(uint32_t addr)
{
    uint32_t *p = flash_slot(addr);

    return p ? *p : 0xFFFFFFFFu;
}

int flash_write_word(uint32_t addr, uint32_t value)
{
    uint32_t *p = flash_slot(addr);

    if (!p || fake_flash_fail) {
        return 0;
    }

    /* Setting a bit that is currently clear needs an erase first, and the FMC
     * reports PGERR rather than doing it.
     */
    if ((value & ~*p) != 0) {
        return 0;
    }

    *p = value;
    fake_flash_writes++;
    return 1;
}

int flash_erase_page(uint32_t addr)
{
    if (addr != FLASH_STORE_ADDR || fake_flash_fail) {
        return 0;
    }

    for (unsigned i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        fake_flash[i] = 0xFFFFFFFFu;
    }
    fake_flash_erases++;
    return 1;
}

/* --- timing.h --- */

void timing_init(void) {}

void delay_ms(uint32_t ms) { run_ms(ms); }

int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    (void)reg; (void)mask; (void)ms;
    return 1;
}
