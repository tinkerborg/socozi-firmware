#include "settings.h"

#include "debug.h"
#include "enhancements.h"
#include "flash.h"

#if ENH_SETTINGS_PERSIST

#define SLOTS (FLASH_PAGE_SIZE / 4u)

#define SLOT_ADDR(i) (FLASH_STORE_ADDR + (i) * 4u)

/* A record is three payload bytes and a check byte.
 *
 * The check is the complement of their sum, which costs one byte and buys the
 * thing the log depends on: an erased word must not read as a record. Erased is
 * 0xFFFFFFFF, whose bytes sum to 0xFD, complement 0x02, which is not the 0xFF
 * sitting in the check position. So erased slots are invalid by construction
 * and the log needs no in-use marker, which would itself have to be written.
 *
 * It also rejects the older two-byte-and-complement format, so a chair carrying
 * records from before this grew a third setting reads them as absent and starts
 * from defaults rather than from garbage.
 */
#define PAYLOAD_MASK 0x00FFFFFFu

static uint8_t record_check(uint32_t payload)
{
    uint8_t sum = (uint8_t)(payload & 0xFF)
                + (uint8_t)((payload >> 8) & 0xFF)
                + (uint8_t)((payload >> 16) & 0xFF);

    return (uint8_t)~sum;
}

static uint32_t make_record(uint32_t payload)
{
    payload &= PAYLOAD_MASK;

    return payload | ((uint32_t)record_check(payload) << 24);
}

static int record_valid(uint32_t word)
{
    return (uint8_t)(word >> 24) == record_check(word & PAYLOAD_MASK);
}

/* Payload layout: one byte per setting. A value needing more than a byte would
 * need the record widened rather than the bytes rebalanced, because the check
 * byte is what makes an erased slot invalid.
 */
#define PAYLOAD_HEAT(p)    ((uint8_t)((p) & 0xFF))
#define PAYLOAD_LUMBAR(p)  ((uint8_t)(((p) >> 8) & 0xFF))
#define PAYLOAD_MASSAGE(p) ((uint8_t)(((p) >> 16) & 0xFF))

#define PAYLOAD_SET(p, shift, v) \
    (((p) & ~(0xFFu << (shift))) | ((uint32_t)(uint8_t)(v) << (shift)))

/* Live values, and the ones the store already holds. They differ exactly when
 * there is something to commit.
 */
static uint32_t live;
static uint32_t committed;

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

        live      = word & PAYLOAD_MASK;
        committed = live;
    }

    next_slot = i;
}

uint8_t settings_heat_level(void)
{
    return PAYLOAD_HEAT(live);
}

uint8_t settings_lumbar_level(void)
{
    return PAYLOAD_LUMBAR(live);
}

uint8_t settings_massage_level(void)
{
    return PAYLOAD_MASSAGE(live);
}

void settings_set_heat_level(uint8_t level)
{
    live = PAYLOAD_SET(live, 0, level);
}

void settings_set_lumbar_level(uint8_t tenths)
{
    live = PAYLOAD_SET(live, 8, tenths);
}

void settings_set_massage_level(uint8_t level)
{
    live = PAYLOAD_SET(live, 16, level);
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

    if (!flash_write_word(SLOT_ADDR(next_slot), make_record(live))) {
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
uint8_t settings_lumbar_level(void) { return 0; }
uint8_t settings_massage_level(void) { return 0; }

void settings_set_heat_level(uint8_t level) { (void)level; }
void settings_set_lumbar_level(uint8_t tenths) { (void)tenths; }
void settings_set_massage_level(uint8_t level) { (void)level; }

void settings_update(int quiet) { (void)quiet; }

#endif /* ENH_SETTINGS_PERSIST */
