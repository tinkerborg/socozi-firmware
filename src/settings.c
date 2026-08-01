#include "settings.h"

#include "debug.h"
#include "enhancements.h"
#include "flash.h"

#if ENH_SETTINGS_PERSIST

/* A record is two words: six payload bytes, one spare, and a check byte.
 *
 * The check is the complement of the sum of the seven bytes before it, which
 * costs one byte and buys the thing the log depends on: an erased record must
 * not read as a real one. Erased is two words of 0xFFFFFFFF, whose seven
 * payload bytes sum to 0xF9 for a complement of 0x06, which is not the 0xFF
 * sitting in the check position.
 *
 * It also rejects the older one-word formats, so a chair carrying records from
 * before the preset was added reads them as absent and starts from defaults
 * rather than from garbage.
 */
#define RECORD_WORDS 8
#define RECORD_BYTES (RECORD_WORDS * 4)

#define SLOTS (FLASH_PAGE_SIZE / RECORD_BYTES)

#define SLOT_ADDR(i) (FLASH_STORE_ADDR + (uint32_t)(i) * RECORD_BYTES)

/* Byte offsets within a record: three loose settings, then four presets of six
 * bytes each, then spare, then the check in the last byte. 27 bytes used of 32.
 */
#define B_PRESET_BYTES 6

enum {
    B_HEAT = 0,
    B_LUMBAR,
    B_MASSAGE,
    B_PRESETS,                                          /* 24 bytes from here */
    B_USED = B_PRESETS + SETTINGS_PRESETS * B_PRESET_BYTES,
    B_CHECK = RECORD_BYTES - 1,
};

/* Where a slot's six bytes start. */
#define PRESET_AT(slot) (B_PRESETS + (slot) * B_PRESET_BYTES)

/* One bit per slot, saying it has been written at least once. Needed because a
 * legitimate preset can be all zeros — flat with everything off — and that must
 * not be confused with a slot nobody has touched.
 */
#define USED_BIT(slot) ((uint8_t)(1u << (slot)))

/* Live values, and the ones the store already holds. They differ exactly when
 * there is something to commit.
 */
static uint8_t live[RECORD_BYTES];
static uint8_t committed[RECORD_BYTES];

/* The next slot to append to. Equal to SLOTS when the page is full. */
static uint16_t next_slot;

static uint8_t check_of(const uint8_t *b)
{
    uint8_t sum = 0;

    for (int i = 0; i < B_CHECK; i++) {
        sum = (uint8_t)(sum + b[i]);
    }

    return (uint8_t)~sum;
}

static int differs(void)
{
    for (int i = 0; i < B_CHECK; i++) {
        if (live[i] != committed[i]) {
            return 1;
        }
    }

    return 0;
}

static uint32_t pack(const uint8_t *b, int word)
{
    const uint8_t *p = b + word * 4;

    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void unpack(const uint32_t *w, uint8_t *b)
{
    for (int i = 0; i < RECORD_WORDS; i++) {
        b[i * 4 + 0] = (uint8_t)(w[i] & 0xFF);
        b[i * 4 + 1] = (uint8_t)((w[i] >> 8) & 0xFF);
        b[i * 4 + 2] = (uint8_t)((w[i] >> 16) & 0xFF);
        b[i * 4 + 3] = (uint8_t)((w[i] >> 24) & 0xFF);
    }
}

void settings_init(void)
{
    uint16_t i;

    for (i = 0; i < RECORD_BYTES; i++) {
        live[i]      = 0;
        committed[i] = 0;
    }

    for (i = 0; i < SLOTS; i++) {
        uint32_t w[RECORD_WORDS];
        uint8_t  b[RECORD_BYTES];
        int      erased = 1;
        int      k;

        for (k = 0; k < RECORD_WORDS; k++) {
            w[k] = flash_read(SLOT_ADDR(i) + (uint32_t)k * 4u);
            if (w[k] != 0xFFFFFFFFu) {
                erased = 0;
            }
        }

        /* The first fully erased slot is where the next append goes. */
        if (erased) {
            break;
        }

        unpack(w, b);

        /* Anything else is a used slot. A torn write — the first word
         * programmed and the second still erased — lands here, fails the check,
         * and is stepped over rather than taken as data or written into again.
         */
        if (b[B_CHECK] == check_of(b)) {
            for (int j = 0; j < B_CHECK; j++) {
                live[j]      = b[j];
                committed[j] = b[j];
            }
        }
    }

    next_slot = i;
}

uint8_t settings_heat_level(void)     { return live[B_HEAT]; }
uint8_t settings_lumbar_level(void)   { return live[B_LUMBAR]; }
uint8_t settings_massage_level(void)  { return live[B_MASSAGE]; }

int settings_preset_used(uint8_t slot)
{
    return slot < SETTINGS_PRESETS && (live[B_USED] & USED_BIT(slot)) != 0;
}

void settings_get_preset(uint8_t slot, struct settings_preset *out)
{
    const uint8_t *p;

    if (slot >= SETTINGS_PRESETS) {
        out->recline = out->headrest = out->flags = 0;
        out->heat = out->lumbar = out->massage = 0;
        return;
    }

    p = &live[PRESET_AT(slot)];

    out->recline  = p[0];
    out->headrest = p[1];
    out->flags    = p[2];
    out->heat     = p[3];
    out->lumbar   = p[4];
    out->massage  = p[5];
}

void settings_set_heat_level(uint8_t level)    { live[B_HEAT] = level; }
void settings_set_lumbar_level(uint8_t tenths) { live[B_LUMBAR] = tenths; }
void settings_set_massage_level(uint8_t level) { live[B_MASSAGE] = level; }

void settings_set_preset(uint8_t slot, const struct settings_preset *p)
{
    uint8_t *q;

    if (slot >= SETTINGS_PRESETS) {
        return;
    }

    q = &live[PRESET_AT(slot)];

    q[0] = p->recline;
    q[1] = p->headrest;
    q[2] = p->flags;
    q[3] = p->heat;
    q[4] = p->lumbar;
    q[5] = p->massage;

    live[B_USED] |= USED_BIT(slot);
}

void settings_clear_preset(uint8_t slot)
{
    if (slot >= SETTINGS_PRESETS) {
        return;
    }

    for (int i = 0; i < B_PRESET_BYTES; i++) {
        live[PRESET_AT(slot) + i] = 0;
    }

    live[B_USED] &= (uint8_t)~USED_BIT(slot);
}

void settings_update(int quiet)
{
    uint8_t out[RECORD_BYTES];
    int     i;

    if (!differs() || !quiet) {
        return;
    }

    if (next_slot >= SLOTS) {
        if (!flash_erase_page(FLASH_STORE_ADDR)) {
            /* No retry. The values stay live in RAM and the chair works
             * normally; hammering a page that has already refused would not
             * make it agree.
             */
            goto give_up;
        }

        next_slot = 0;
        dbg.settings_erases++;
    }

    for (i = 0; i < B_CHECK; i++) {
        out[i] = live[i];
    }
    out[B_CHECK] = check_of(out);

    /* In order, so the check byte in the last word lands last: a power loss
     * part way through leaves a record that fails its check rather than one
     * that reads as valid but is half old.
     */
    for (i = 0; i < RECORD_WORDS; i++) {
        if (!flash_write_word(SLOT_ADDR(next_slot) + (uint32_t)i * 4u,
                              pack(out, i))) {
            next_slot++;                /* that slot is spoiled, skip it */
            goto give_up;
        }
    }

    next_slot++;

    for (i = 0; i < B_CHECK; i++) {
        committed[i] = live[i];
    }

    dbg.settings_writes++;
    return;

give_up:
    for (i = 0; i < B_CHECK; i++) {
        committed[i] = live[i];
    }
    dbg.settings_errors++;
}

#else /* !ENH_SETTINGS_PERSIST */

void settings_init(void) {}

uint8_t settings_heat_level(void) { return 0; }
uint8_t settings_lumbar_level(void) { return 0; }
uint8_t settings_massage_level(void) { return 0; }
int settings_preset_used(uint8_t slot) { (void)slot; return 0; }

void settings_get_preset(uint8_t slot, struct settings_preset *out)
{
    (void)slot;
    out->recline = out->headrest = out->flags = 0;
    out->heat = out->lumbar = out->massage = 0;
}

void settings_set_heat_level(uint8_t level) { (void)level; }
void settings_set_lumbar_level(uint8_t tenths) { (void)tenths; }
void settings_set_massage_level(uint8_t level) { (void)level; }

void settings_set_preset(uint8_t slot, const struct settings_preset *p)
{
    (void)slot; (void)p;
}

void settings_clear_preset(uint8_t slot) { (void)slot; }

void settings_update(int quiet) { (void)quiet; }

#endif /* ENH_SETTINGS_PERSIST */
