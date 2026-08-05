#include "debug.h"
#include "enhancements.h"
#include "gpio.h"
#include "motion.h"
#include "timing.h"

/* Where we are in the relay sequence for the current request. */
enum {
    SEQ_IDLE = 0,
    SEQ_SETTLING,   /* direction selected, waiting for the relay to settle */
    SEQ_FIRST,      /* first enable closed */
    SEQ_RUNNING,    /* fully energized */
};

/* A stall or timeout latches until the button is released, so a held button
 * can't immediately re-drive into whatever stopped it.
 */
static int      fault_latched;

/* Reaching a stop latches the same way, and for the same reason: a held button
 * would otherwise re-request immediately, restart the 600 ms sequence, and
 * chatter the relays against an open limit switch.
 */
#if ENH_END_OF_TRAVEL_STOP
static int      arrived_latched;
static uint32_t zero_since_ms;  /* when the sense channel last went to zero */
static int      zero;

/* Which motions can end themselves at a stop.
 *
 * Not the headrest. Its motor is small enough that it reads as zero for its
 * whole travel on a channel scaled for the recline motor, so the detector saw
 * every headrest move as arriving a second in. Recline and the flatten macro
 * both register, and the flatten is the one that has to end itself.
 *
 * Requiring current to have been seen first was the other way to fix that, and
 * it broke the flatten: driving down is gravity-assisted and can draw little
 * enough to read as zero the whole way, so the macro never ended.
 */
static int arrival_applies(uint32_t want)
{
    return want == MOTION_FLATTEN
        || want == MOTION_RECLINE_UP
        || want == MOTION_RECLINE_DOWN;
}
#else
enum { arrived_latched = 0 };
#endif

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

/* Select direction without energizing anything. */
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
#if ENH_END_OF_TRAVEL_STOP
        arrived_latched = 0;
#endif
    } else if (fault_latched || arrived_latched) {
        return;
    }

    if (want == requested) {
        return;
    }

    /* Any change, including to MOTION_NONE, drops everything first, so two
     * directions are never energized at once even for an instant.
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

#if ENH_END_OF_TRAVEL_STOP
    /* Nothing is driving yet, so the channel reads zero. Start from "not at
     * zero" rather than carrying a count over from the previous motion.
     */
    zero = 0;
#endif
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

#if ENH_END_OF_TRAVEL_STOP
/* The mirror of stalled(): current falling to zero while driving means every
 * motor has opened its own limit switch. Zero is also what the channel reads
 * before anything is energized, hence MOTION_ARM_MS.
 */
static int arrived(uint32_t current)
{
    if ((ms_ticks - running_ms) < MOTION_ARM_MS) {
        return 0;
    }

    /* 0xFFFFFFFF, a failed conversion, lands here too: no reading is not the
     * same as a zero reading, and must not stop a motion.
     */
    if (current != 0) {
        zero = 0;
        return 0;
    }

    if (!zero) {
        zero          = 1;
        zero_since_ms = ms_ticks;
        return 0;
    }

    return (ms_ticks - zero_since_ms) >= MOTION_ARRIVED_MS;
}
#endif

#if ENH_POSITION_TRACK

/* Milliseconds of upward travel above the down stop. */
static uint32_t pos_recline;
static uint32_t pos_headrest;
static uint32_t track_ms;

uint32_t motion_pos_recline(void)  { return pos_recline; }
uint32_t motion_pos_headrest(void) { return pos_headrest; }

int motion_at_home(void)
{
    return pos_recline <= MOTION_HOME_MS && pos_headrest <= MOTION_HOME_MS;
}

static void advance(uint32_t *pos, uint32_t travel, int up, uint32_t d)
{
    if (up) {
        *pos = (*pos + d > travel) ? travel : *pos + d;
    } else {
        *pos = (d > *pos) ? 0 : *pos - d;
    }
}

/* Integrate the time the motor has actually been turning. Called only once the
 * relays are closed, so the settle time does not count as travel.
 */
static void track(void)
{
    uint32_t d = ms_ticks - track_ms;

    track_ms = ms_ticks;

    switch (requested) {
    case MOTION_RECLINE_UP:
        advance(&pos_recline, MOTION_TRAVEL_RECLINE_MS, 1, d);
        break;
    case MOTION_RECLINE_DOWN:
        advance(&pos_recline, MOTION_TRAVEL_RECLINE_MS, 0, d);
        break;
    case MOTION_HEADREST_UP:
        advance(&pos_headrest, MOTION_TRAVEL_HEADREST_MS, 1, d);
        break;
    case MOTION_HEADREST_DOWN:
        advance(&pos_headrest, MOTION_TRAVEL_HEADREST_MS, 0, d);
        break;
    case MOTION_FLATTEN:
        advance(&pos_recline, MOTION_TRAVEL_RECLINE_MS, 0, d);
        advance(&pos_headrest, MOTION_TRAVEL_HEADREST_MS, 0, d);
        break;
    default:
        break;
    }

    dbg.pos_recline  = pos_recline;
    dbg.pos_headrest = pos_headrest;
}

#endif /* ENH_POSITION_TRACK */

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
#if ENH_POSITION_TRACK
            /* Travel is counted from here, not from the request: the settle is
             * relays closing, with nothing turning yet.
             */
            track_ms = ms_ticks;
#endif
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

#if ENH_POSITION_TRACK
    track();
#endif

    if (stalled(current)) {
        motion_stop();
        dbg.stalls++;
        dbg.stops++;
        fault_latched = 1;
        return;
    }

#if ENH_END_OF_TRAVEL_STOP
    /* Every motion, not just the flatten macro. Holding a button into a stop
     * used to keep the relays closed against an open limit switch until the
     * button came up; now it ends the same way an unattended move does.
     *
     * It is also the only correction dead reckoning gets. At a stop the axis is
     * at a known end of its travel, whatever the estimate had accumulated, so
     * the estimate is replaced rather than adjusted.
     *
     * Not a fault: reaching the stop is the move succeeding, so it does not
     * count as a safety stop.
     */
    if (arrival_applies(requested) && arrived(current)) {
        motion_stop();
        dbg.arrivals++;
        arrived_latched = 1;

#if ENH_POSITION_TRACK
        switch (requested) {
        case MOTION_RECLINE_UP:
            pos_recline = MOTION_TRAVEL_RECLINE_MS;
            break;
        case MOTION_HEADREST_UP:
            pos_headrest = MOTION_TRAVEL_HEADREST_MS;
            break;
        case MOTION_RECLINE_DOWN:
            pos_recline = 0;
            break;
        case MOTION_HEADREST_DOWN:
            pos_headrest = 0;
            break;
        case MOTION_FLATTEN:
            pos_recline  = 0;
            pos_headrest = 0;
            break;
        default:
            break;
        }

        dbg.pos_recline  = pos_recline;
        dbg.pos_headrest = pos_headrest;
#endif
        return;
    }
#endif

    if (dbg.motion_ms > MOTION_TIMEOUT_MS) {
        motion_stop();
        dbg.stops++;
        fault_latched = 1;
    }
}
