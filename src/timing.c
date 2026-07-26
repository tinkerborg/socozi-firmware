#include "gd32e23x.h"
#include "timing.h"

volatile uint32_t ms_ticks;

void SysTick_Handler(void)
{
    ms_ticks++;
}

void timing_init(void)
{
    /* 8 MHz IRC by default, so 8000 cycles per millisecond. */
    SYSTICK_RELOAD = 8000 - 1;
    SYSTICK_VAL    = 0;
    SYSTICK_CTL    = 7;   /* enable, interrupt, processor clock */
}

void delay_ms(uint32_t ms)
{
    uint32_t start = ms_ticks;

    while ((ms_ticks - start) < ms) {
        /* spin */
    }
}

int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    uint32_t start = ms_ticks;

    while (*reg & mask) {
        if ((ms_ticks - start) >= ms) {
            return 0;
        }
    }

    return 1;
}
