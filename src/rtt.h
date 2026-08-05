/* A console over the debug link, with no pins and no peripheral.
 *
 * SEGGER's Real Time Transfer, which is a ring buffer in RAM and a magic
 * string. The debug probe reads target memory in the background — SWD's access
 * port does bus transactions independently of the core — so the chair keeps
 * running while the host reads what it wrote. A second buffer running the other
 * way carries input back.
 *
 *     make console        OpenOCD serves it on TCP 9090
 *     telnet localhost 9090
 *
 * Neither direction blocks. A full up-buffer drops characters rather than
 * stalling the control loop, which matters: this sits in the same loop that
 * kicks the watchdog and stops the motors.
 *
 * Cortex-M23 has no ITM, so SWO is not an option on this part. This is.
 */

#ifndef RTT_H
#define RTT_H

#include <stdint.h>

#ifndef RTT
#define RTT 0
#endif

#if RTT

/* Target to host. Silently drops when full. */
void rtt_putc(char c);
void rtt_write(const char *s);

/* Decimal, because there is no printf here and there never will be. */
void rtt_write_u32(uint32_t v);

/* Host to target. Returns -1 when there is nothing waiting. */
int rtt_getc(void);

#endif /* RTT */

/* Where the control block ended up, or 0 when RTT is compiled out. Published in
 * the debug block so the ESP32 bridge can find it without scanning SRAM for the
 * magic string. Declared either way so callers need no guard of their own.
 */
uint32_t rtt_control_block(void);

#endif /* RTT_H */
