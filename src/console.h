/* Single-key console on the RTT link, for the bench.
 *
 * No line editing and no parser: one keypress, one answer. Everything it
 * reports is already in the debug block, so this adds no state of its own and
 * cannot affect what the chair does — it is a nicer way to read what a
 * debugger could read anyway.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "rtt.h"

#if RTT

/* Call every loop. Does nothing until a key arrives. */
void console_update(void);

#endif

#endif /* CONSOLE_H */
