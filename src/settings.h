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

/* Stored values, or 0 if the store has nothing to say about one. Callers
 * validate: a value outside their own range means absent, so a corrupt or
 * half-written record degrades to a default rather than to a refusal.
 *
 * The lumbar level is an inflate duration in 100 ms units, which is what makes
 * it fit a byte. See enhancements-spec.md §2.5 for why a duration is the only
 * kind of level this hardware can express.
 */
uint8_t settings_heat_level(void);
uint8_t settings_lumbar_level(void);
uint8_t settings_massage_level(void);

/* Saved chairs, §2.8. One per motion button.
 *
 * A slot carries its own levels rather than borrowing the ones above. Those are
 * the last-used memory, which a recall reads from but never writes to, and a
 * manual adjustment writes to but never reaches a slot. Only an explicit preset
 * write touches a slot.
 */
#define SETTINGS_PRESETS 4

struct settings_preset {
    uint8_t recline;    /* PRESET_STEP_MS units above the down stop */
    uint8_t headrest;
    uint8_t flags;      /* PRESET_FLAG_*, which comfort functions were on */
    uint8_t heat;       /* 1..HEAT_LEVEL_MAX */
    uint8_t lumbar;     /* inflate time in 100 ms units */
    uint8_t massage;    /* 1..ADJUST_LEVEL_MAX */
};

/* A whole slot at a time, because the parts are only meaningful together: half
 * a preset is not a chair anybody asked for.
 */
void settings_get_preset(uint8_t slot, struct settings_preset *out);
void settings_set_preset(uint8_t slot, const struct settings_preset *p);

/* Back to never-written. Zeroing the bytes is not enough on its own — all
 * zeros is a legitimate preset — so this also clears the written bit.
 */
void settings_clear_preset(uint8_t slot);

/* True once a slot has been written. An unwritten slot is all zeros, which
 * would otherwise recall as "flat, everything off" and look like a preset
 * somebody chose.
 */
int settings_preset_used(uint8_t slot);

/* Note a new value. Does not touch flash; marks it to be committed. Setting the
 * value that is already committed does nothing at all, so picking a level and
 * changing back writes nothing.
 */
void settings_set_heat_level(uint8_t level);
void settings_set_lumbar_level(uint8_t tenths);
void settings_set_massage_level(uint8_t level);

/* Call every loop. Commits a pending change when `quiet` is true.
 *
 * `quiet` is the caller's judgement that stalling the loop for the length of a
 * flash write is currently free: nothing moving, and nothing being chosen. A
 * commit is never urgent, so it can wait indefinitely for one.
 */
void settings_update(int quiet);

#endif /* SETTINGS_H */
