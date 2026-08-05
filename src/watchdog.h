/* Free watchdog.
 *
 * Compiled in only when WATCHDOG is non-zero. The release build enables it; the
 * debug build does not, because a watchdog resets the part within milliseconds
 * of halting in GDB, which makes debugging impractical.
 *
 * Why bother: every other safety bound in this firmware, the motion timeout,
 * the heater auto-off, stall detection, is enforced by the same loop that
 * would be stuck if the firmware hung. The watchdog is the only thing that
 * covers that case, and a hang with the heater or a motor energized is exactly
 * the failure worth covering.
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#ifndef WATCHDOG
#define WATCHDOG 0
#endif

#if WATCHDOG

void watchdog_init(void);
void watchdog_kick(void);

#else

static inline void watchdog_init(void) { }
static inline void watchdog_kick(void) { }

#endif

#endif /* WATCHDOG_H */
