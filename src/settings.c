#include "settings.h"

#include "debug.h"
#include "enhancements.h"
#include "flash.h"

#if ENH_SETTINGS_PERSIST

#define SLOTS (FLASH_PAGE_SIZE / 4u)

#define SLOT_ADDR(i) (FLASH_STORE_ADDR + (i) * 4u)

/* A record is the payload in the low half and its complement in the high half.
 *
 * That is the whole validity scheme, and it costs nothing: an erased word is
 * 0xFFFFFFFF, whose halves are 0xFFFF and 0xFFFF, and 0xFFFF is not the
 * complement of 0xFFFF. So erased slots are invalid by construction and the log
 * needs no separate in-use marker, which would itself have to be written.
 */
#define RECORD(payload) (((uint32_t)(uint16_t)~(payload) << 16) | (uint16_t)(payload))

static int record_valid(uint32_t word)
{
    return (uint16_t)(word >> 16) == (uint16_t)~(uint16_t)word;
}

static uint16_t record_payload(uint32_t word)
{
    return (uint16_t)word;
}

/* Payload layout. One byte spoken for, the rest reserved so a second setting
 * can be added without changing the record format.
 */
#define PAYLOAD_HEAT(p)  ((uint8_t)((p) & 0xFF))
#define PAYLOAD(heat)    ((uint16_t)(heat))

/* Live values, and the ones the store already holds. They differ exactly when
 * there is something to commit.
 */
static uint16_t live;
static uint16_t committed;

/* The next slot to append to. Equal to SLOTS when the page is full. */
static uint16_t next_slot;

void settings_init(void)
{
    uint16_t i;

    live      = 0;
    committed = 0;
    next_slot = 0;

    /* Writes only ever move forward, so the last valid record is the live one
     * and the first erased slot is where the next append goes.
     */
    for (i = 0; i < SLOTS; i++) {
        uint32_t word = flash_read(SLOT_ADDR(i));

        if (!record_valid(word)) {
            break;
        }

        live      = record_payload(word);
        committed = live;
    }

    next_slot = i;
}

uint8_t settings_heat_level(void)
{
    return PAYLOAD_HEAT(live);
}

void settings_set_heat_level(uint8_t level)
{
    live = PAYLOAD(level);
}

void settings_update(int quiet)
{
    if (live == committed || !quiet) {
        return;
    }

    if (next_slot >= SLOTS) {
        if (!flash_erase_page(FLASH_STORE_ADDR)) {
            /* No retry. The value stays live in RAM and the chair works
             * normally; hammering a page that has already refused would not
             * make it agree.
             */
            committed = live;
            dbg.settings_errors++;
            return;
        }

        next_slot = 0;
        dbg.settings_erases++;
    }

    if (!flash_write_word(SLOT_ADDR(next_slot), RECORD(live))) {
        committed = live;
        dbg.settings_errors++;
        return;
    }

    next_slot++;
    committed = live;
    dbg.settings_writes++;
}

#else /* !ENH_SETTINGS_PERSIST */

void settings_init(void) {}

uint8_t settings_heat_level(void) { return 0; }

void settings_set_heat_level(uint8_t level) { (void)level; }

void settings_update(int quiet) { (void)quiet; }

#endif /* ENH_SETTINGS_PERSIST */
