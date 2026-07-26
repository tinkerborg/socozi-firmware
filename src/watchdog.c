#include "watchdog.h"

#if WATCHDOG

#include "gd32e23x.h"

/* ~2 s timeout: the 40 kHz RC divided by 64 gives 625 Hz, reloaded from 1250.
 *
 * Long enough that a slow loop iteration never trips it, the ADC conversions
 * and the bit-banged shift register are the slowest things we do, and short
 * enough that a hang with a motor running is measured in seconds, not minutes.
 */
#define WDG_PRESCALER 4      /* /64 */
#define WDG_RELOAD    1250

/* Note: DBG_CTL's FWDGT_HOLD bit, which freezes the watchdog while the core is
 * halted, is deliberately NOT set here. Writes to that register from the core
 * never take. It appears writable only over the debug port. The Makefile's
 * OpenOCD invocations set it instead, and it persists across resets.
 */
void watchdog_init(void)
{
    FWDGT_CTL = FWDGT_KEY_UNLOCK;

    while (FWDGT_STAT & FWDGT_STAT_PUD) { }
    FWDGT_PSC = WDG_PRESCALER;

    while (FWDGT_STAT & FWDGT_STAT_RUD) { }
    FWDGT_RLD = WDG_RELOAD;

    FWDGT_CTL = FWDGT_KEY_RELOAD;
    FWDGT_CTL = FWDGT_KEY_ENABLE;
}

void watchdog_kick(void)
{
    FWDGT_CTL = FWDGT_KEY_RELOAD;
}

#endif /* WATCHDOG */
