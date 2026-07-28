/* Settings that survive a reset, in one reserved flash page.
 *
 * See enhancements-spec.md §2.4. In short: an append-only log of 32-bit
 * records, because flash can only clear bits and only a whole-page erase puts
 * them back. Writing a new value means appending a record, not rewriting one,
 * and the live value is the last valid record in the page.
 *
 * That buys wear headroom nobody has to think about again: 1024 slots per erase
 * against 100 kcycles of endurance is on the order of 10^8 saves.
 *
 * Values are held in RAM and read back from there, so nothing on the hot path
 * touches flash. A change is committed later, by settings_update(), and only
 * when the caller says it is a quiet moment.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#include "enhancements.h"

/* Scan the store and load whatever is in it. Call once, before anything asks
 * for a value.
 */
void settings_init(void);

/* The stored heat level, or 0 if the store has nothing to say. Callers
 * validate: a level outside their own range means absent, so a corrupt or
 * half-written record degrades to a default rather than to a refusal.
 */
uint8_t settings_heat_level(void);

/* Note a new level. Does not touch flash; marks it to be committed. Setting the
 * value that is already committed does nothing at all, so picking a level and
 * changing back writes nothing.
 */
void settings_set_heat_level(uint8_t level);

/* Call every loop. Commits a pending change when `quiet` is true.
 *
 * `quiet` is the caller's judgement that stalling the loop for the length of a
 * flash write is currently free: nothing moving, and nothing being chosen. A
 * commit is never urgent, so it can wait indefinitely for one.
 */
void settings_update(int quiet);

#endif /* SETTINGS_H */
