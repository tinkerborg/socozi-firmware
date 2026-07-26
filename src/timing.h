/* 1 kHz millisecond timebase, from SysTick. */

#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

extern volatile uint32_t ms_ticks;

void timing_init(void);
void delay_ms(uint32_t ms);

/* Spin until every bit in `mask` reads back clear, giving up after `ms`.
 * Returns 1 on success, 0 on timeout.
 *
 * Nothing in this firmware may block forever on a register bit, a wrong guess
 * about the register map should degrade to a visible flag, not a hang.
 */
int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms);

#endif /* TIMING_H */
