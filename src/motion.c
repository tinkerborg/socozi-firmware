#include "debug.h"
#include "gpio.h"
#include "motion.h"
#include "timing.h"

/* Where we are in the relay sequence for the current request. */
enum {
    SEQ_IDLE = 0,
    SEQ_SETTLING,   /* direction selected, waiting for the relay to settle */
    SEQ_FIRST,      /* first enable closed */
    SEQ_RUNNING,    /* fully energised */
};

/* A stall or timeout latches until the button is released, so a held button
 * can't immediately re-drive into whatever stopped it.
 */
static int      fault_latched;

static uint32_t requested;      /* MOTION_*, set the moment a button is seen */
static uint32_t requested_ms;   /* when the request started */
static uint32_t running_ms;     /* when the motor actually started turning */
static uint8_t  seq;

uint32_t motion_active(void)
{
    return requested;
}

void motion_stop(void)
{
    pin_write(PIN_RECLINE_A, 0);
    pin_write(PIN_RECLINE_B, 0);
    pin_write(PIN_RECLINE_C, 0);
    pin_write(PIN_RECLINE_D, 0);
    pin_write(PIN_HEADREST_EN, 0);

    requested       = MOTION_NONE;
    seq             = SEQ_IDLE;
    dbg.motion      = MOTION_NONE;
    dbg.motion_ms   = 0;
}

/* Select direction without energising anything. */
static void select_direction(uint32_t want)
{
    switch (want) {
    case MOTION_HEADREST_UP:
        pin_write(PIN_HEADREST_DIR, 0);
        break;

    case MOTION_HEADREST_DOWN:
    case MOTION_FLATTEN:
        pin_write(PIN_HEADREST_DIR, 1);
        break;

    default:
        /* Recline has no separate direction pin, the pair chosen at engage
         * time is what selects direction.
         */
        break;
    }
}

/* Close the first contact of the requested motion. */
static void engage_first(uint32_t want)
{
    switch (want) {
    case MOTION_RECLINE_UP:    pin_write(PIN_RECLINE_A, 1);   break;
    case MOTION_RECLINE_DOWN:  pin_write(PIN_RECLINE_C, 1);   break;
    case MOTION_HEADREST_UP:
    case MOTION_HEADREST_DOWN: pin_write(PIN_HEADREST_EN, 1); break;

    case MOTION_FLATTEN:
        /* Both axes at once, as the factory macro does. */
        pin_write(PIN_RECLINE_C, 1);
        pin_write(PIN_HEADREST_EN, 1);
        break;

    default: break;
    }
}

/* Close the second contact. Recline only; headrest has just the one. */
static void engage_second(uint32_t want)
{
    switch (want) {
    case MOTION_RECLINE_DOWN:
    case MOTION_FLATTEN:      pin_write(PIN_RECLINE_D, 1); break;
    case MOTION_RECLINE_UP:   pin_write(PIN_RECLINE_B, 1); break;
    default: break;
    }
}

void motion_request(uint32_t want)
{
    /* Releasing the button clears a latched fault. */
    if (want == MOTION_NONE) {
        fault_latched = 0;
    } else if (fault_latched) {
        return;
    }

    if (want == requested) {
        return;
    }

    /* Any change, including to MOTION_NONE, drops everything first, so two
     * directions are never energised at once even for an instant.
     */
    motion_stop();

    if (want == MOTION_NONE) {
        return;
    }

    select_direction(want);

    requested    = want;
    requested_ms = ms_ticks;
    seq          = SEQ_SETTLING;
    dbg.motion   = want;
}

/* Watch for a locked rotor. Only meaningful once current is actually flowing,
 * so this is not called until the sequence has engaged.
 */
static int stalled(uint32_t current)
{
    static uint32_t over_since_ms;
    static int      over;

    if (current == 0xFFFFFFFF) {    /* conversion failed, no opinion */
        return 0;
    }

    if (current < MOTION_STALL_ADC) {
        over = 0;
        return 0;
    }

    if (!over) {
        over          = 1;
        over_since_ms = ms_ticks;
        return 0;
    }

    return (ms_ticks - over_since_ms) >= MOTION_STALL_MS;
}

void motion_update(uint32_t current)
{
    uint32_t elapsed;

    if (requested == MOTION_NONE) {
        return;
    }

    elapsed = ms_ticks - requested_ms;

    switch (seq) {
    case SEQ_SETTLING:
        if (elapsed >= MOTION_SETTLE_MS) {
            engage_first(requested);
            running_ms = ms_ticks;
            seq        = SEQ_FIRST;
        }
        break;

    case SEQ_FIRST:
        if (elapsed >= (MOTION_SETTLE_MS + MOTION_STAGGER_MS)) {
            engage_second(requested);
            seq = SEQ_RUNNING;
        }
        break;

    default:
        break;
    }

    if (seq == SEQ_SETTLING) {
        return;
    }

    dbg.motion_ms = ms_ticks - running_ms;

    if (stalled(current)) {
        motion_stop();
        dbg.stalls++;
        dbg.stops++;
        fault_latched = 1;
        return;
    }

    if (dbg.motion_ms > MOTION_TIMEOUT_MS) {
        motion_stop();
        dbg.stops++;
        fault_latched = 1;
    }
}
