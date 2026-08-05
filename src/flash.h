/* Flash writes, the third hardware seam.
 *
 * Like gpio.h and timing.h, this exists so the logic above it can be linked
 * against a fake and run on the host. settings.c is the only caller, and it
 * reaches the FMC through nothing else.
 *
 * Two things to know about writing flash on this part, both of which shape the
 * store above:
 *
 *   - A word can only ever have bits *cleared*. Programming a word that is not
 *     erased sets PGERR. Only a whole-page erase puts the ones back.
 *   - The core executes from flash, so a program or erase stalls instruction
 *     fetch. The main loop stops, and every output stays latched where it was.
 *     A word program is microseconds and invisible; a page erase is tens of
 *     milliseconds, 42 ms worst case in the datasheet. See
 *     enhancements-spec.md §2.4 for why that is inside every margin this
 *     firmware already relies on, and why a commit waits for a quiet moment
 *     anyway.
 */

#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

/* 64 KiB in 64 pages of 1 KiB.
 *
 * 1 KiB is the FMC's erase granularity, not the 4 KiB this used to assume.
 * Measured on the chair, over the ESP32 bridge: after erasing 0x08000000, a
 * write to 0x08000400 came back with PGERR, so the erase had covered exactly
 * one kilobyte. The old value was never caught because the store only erases
 * once it has filled, and it had not.
 *
 * The store is one page, and nothing else may live in it. gd32e230c8.ld
 * reserves the last 4 KiB rather than the last 1 KiB, so there is room to grow
 * the store without moving code; only the first page of that is used today.
 */
#define FLASH_PAGE_SIZE  1024u
#define FLASH_STORE_ADDR 0x0800F000u

uint32_t flash_read(uint32_t addr);

/* Both return 1 on success, 0 on a timeout, an FMC error, or a readback that
 * does not match. Never retried by the caller: see the spec.
 */
int flash_write_word(uint32_t addr, uint32_t value);
int flash_erase_page(uint32_t addr);

#endif /* FLASH_H */
