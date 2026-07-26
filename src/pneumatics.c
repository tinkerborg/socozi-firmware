#include "gpio.h"
#include "handset.h"
#include "motion.h"
#include "pneumatics.h"
#include "debug.h"
#include "timing.h"

#define VALVE_TOP    0x01
#define VALVE_MID    0x02
#define VALVE_VENT   0x04
#define VALVE_BOTTOM 0x08
#define VALVE_CELLS  (VALVE_TOP | VALVE_MID | VALVE_BOTTOM)

/* Duration of one pattern-table tick: 100 ms, measured from the factory image,
 * not guessed.
 *
 * Its SysTick runs at 1 kHz and keeps a counter that wraps at 100. At count 40
 * the handler raises the flag that enqueues action 0xF2, the function that
 * drives both motion control and the massage engine. So the engine ticks once
 * per 100 ms, making a 40-tick step 4 s and the 16-step wave about 55 s.
 */
#define MASSAGE_TICK_MS 100

/* How long to hold the exhaust open when shutting a function down.
 *
 * 120 s, matching the factory firmware, whose idle branch holds the exhaust
 * while a counter is under 120, incremented by action 0xF5, which fires once
 * per second, not once per tick. Verified by address: the SysTick secondary
 * counter and the 0xF5 test are the same variable (0x20001A02), and the vent
 * condition reads 0x20001EA0.
 *
 * Anything shorter does not fully empty the bladders.
 */
#define VENT_MS 120000

/* Ceiling on lumbar inflation if the user never presses again, the factory
 * table's 200 ticks.
 */
#define LUMBAR_INFLATE_MAX_MS (200 * MASSAGE_TICK_MS)

/* Massage auto-off. The factory sets a countdown of 15 and decrements it once
 * per minute, clearing the massage bit when it runs out.
 */
#define MASSAGE_TIMEOUT_MS (15u * 60u * 1000u)

struct pstep {
    uint8_t  bits;
    uint16_t ticks;
};

/* Transcribed from the factory image at 0x08004A1B, all 39 steps up to the
 * 0xAA sentinel at 0x08004B2C. One cycle is 1185 ticks, just under 2 minutes.
 *
 * Three movements: a wave down the back, then a run of short pulses on the
 * middle cell with ramping duration, then the same ramp on middle+bottom.
 */
static const struct pstep massage_pattern[] = {
    /* wave down the back and bleed off */
    { 0x01, 40 }, { 0x03, 40 }, { 0x0A, 40 }, { 0x07, 40 },
    { 0x05, 40 }, { 0x05, 40 }, { 0x03, 40 }, { 0x0A, 40 },
    { 0x07, 40 }, { 0x05, 40 }, { 0x04, 80 }, { 0x01, 40 },
    { 0x03, 40 },

    /* middle cell pulsed, holding longer each time */
    { 0x00,  5 }, { 0x03, 10 }, { 0x00,  5 }, { 0x03, 10 },
    { 0x00,  5 }, { 0x03, 10 }, { 0x00,  5 }, { 0x03, 20 },
    { 0x00,  5 }, { 0x03, 40 }, { 0x00,  5 }, { 0x03, 80 },
    { 0x00, 40 },

    /* bottom, then middle+bottom pulsed against the vent, same ramp */
    { 0x08, 80 }, { 0x0A, 40 }, { 0x04,  5 }, { 0x0A, 10 },
    { 0x04,  5 }, { 0x0A, 10 }, { 0x04,  5 }, { 0x0A, 20 },
    { 0x04,  5 }, { 0x0A, 40 }, { 0x04,  5 }, { 0x0A, 80 },
    { 0x04, 80 },
};

#define MASSAGE_STEPS (sizeof(massage_pattern) / sizeof(massage_pattern[0]))

/* Lumbar is the bottom bladder alone, per the factory table at 0x080049DC.
 * Three presses: inflate, stop-and-hold, deflate.
 */
enum {
    LUMBAR_OFF = 0,
    LUMBAR_INFLATE,
    LUMBAR_HOLD,
    LUMBAR_DEFLATE,
};

static int      massage_on;
static uint8_t  massage_step;
static uint32_t massage_step_ms;
static uint32_t massage_start_ms;
static uint8_t  lumbar_state;
static uint32_t lumbar_state_ms;
static uint8_t  valves_now;

/* Motion pauses everything. Held here so the elapsed time can be given back
 * when it resumes, see pneumatics_update.
 */
static int      paused;
static uint32_t pause_start_ms;

/* All three shutdown paths, massage off, lumbar off, power off, share one
 * vent timer, mirroring the factory firmware where they fall into a single
 * idle branch.
 */
static uint32_t vent_start_ms;
static int      venting;

int pneumatics_massage_on(void) { return massage_on; }

int pneumatics_lumbar_lit(void)
{
    return lumbar_state == LUMBAR_INFLATE || lumbar_state == LUMBAR_HOLD;
}

/* The factory holds the pump off for the first few ticks after a fully closed
 * state. Its counter increments once per 100 ms tick and is cleared *only*
 * when every cell bit is clear; the pump runs once the count exceeds 2. So the
 * delay is measured from the last all-cells-closed moment, and the pump keeps
 * running straight through valve changes that leave any cell open.
 *
 * An earlier attempt restarted this on every valve change, which starved the
 * short pulse steps. Don't do that.
 */
#define PUMP_DELAY_MS 300

static uint32_t cells_closed_ms;

/* Re-evaluated every pass, not just on valve changes: the pump comes up part
 * way through a step, so it cannot be decided once at the transition.
 *
 * Cell bits are 0, 1 and 3. The exhaust is not tested, so the factory will
 * run the pump against an open exhaust if a cell is also open.
 */
static void pump_update(void)
{
    if (!(valves_now & VALVE_CELLS)) {
        cells_closed_ms = ms_ticks;
        pin_write(PIN_PUMP, 0);
    } else {
        pin_write(PIN_PUMP, (ms_ticks - cells_closed_ms) >= PUMP_DELAY_MS);
    }
}

static void valves_set(uint8_t bits)
{
    if (bits != valves_now) {
        shift_write(bits);
        valves_now = bits;
        dbg.valves = bits;
    }
    pump_update();
}

static void start_vent(void)
{
    valves_set(VALVE_VENT);
    vent_start_ms = ms_ticks;
    venting       = 1;
}

/* A vent runs to completion even while a motor is moving.
 *
 * The motion pause is a pump-current budget, and the exhaust drives no pump, so
 * there is nothing to save by closing it. Confirmed on the chair: the factory
 * firmware keeps deflating when a motion button is pressed mid-vent.
 */
static void vent_tick(void)
{
    if ((ms_ticks - vent_start_ms) >= VENT_MS) {
        venting = 0;
        valves_set(0);
    } else {
        valves_set(VALVE_VENT);
    }
}

static void set_lumbar(uint8_t state)
{
    lumbar_state       = state;
    lumbar_state_ms    = ms_ticks;
    dbg.lumbar_state = state;

    /* Lumbar owns the valves now, including its own deflate. Leaving a vent
     * flagged would let it reopen the exhaust later.
     */
    venting = 0;
}

static void massage_tick(void)
{
    const struct pstep *s = &massage_pattern[massage_step];

    if (massage_step_ms == 0) {
        massage_step_ms = ms_ticks;
    } else if ((ms_ticks - massage_step_ms)
               >= (uint32_t)s->ticks * MASSAGE_TICK_MS) {
        massage_step     = (uint8_t)((massage_step + 1) % MASSAGE_STEPS);
        massage_step_ms  = ms_ticks;
        dbg.massage_step = massage_step;
        s                = &massage_pattern[massage_step];
    }

    /* Assert the current step's bits every pass, not only on a transition.
     * The factory engine rewrites the valve outputs on every tick, and it
     * matters here: a motion pause drives the valves to 0, so waiting for the
     * next transition would leave them shut for up to 8 s afterwards.
     * valves_set only touches the shift register when the value changes.
     */
    valves_set(s->bits);
}

static void lumbar_tick(void)
{
    switch (lumbar_state) {
    case LUMBAR_INFLATE:
        valves_set(VALVE_BOTTOM);
        if ((ms_ticks - lumbar_state_ms) >= LUMBAR_INFLATE_MAX_MS) {
            set_lumbar(LUMBAR_HOLD);
        }
        break;

    case LUMBAR_HOLD:
        valves_set(0);              /* valve closed traps the air */
        break;

    case LUMBAR_DEFLATE:
        valves_set(VALVE_VENT);     /* exhaust alone, see header */
        if ((ms_ticks - lumbar_state_ms) >= VENT_MS) {
            lumbar_state = LUMBAR_OFF;
            dbg.lumbar_state = lumbar_state;
            valves_set(0);
        }
        break;

    default:
        break;
    }
}

static void massage_off(void)
{
    massage_on     = 0;
    dbg.massage_on = 0;
}

void pneumatics_shutdown(void)
{
    massage_off();
    lumbar_state     = LUMBAR_OFF;
    dbg.lumbar_state = lumbar_state;
    start_vent();
}

void pneumatics_button(uint8_t button)
{
    switch (button) {
    case HS_MASSAGE:
        massage_on = !massage_on;
        if (massage_on) {
            venting          = 0;   /* massage owns the valves, see set_lumbar */
            lumbar_state     = LUMBAR_OFF;
            dbg.lumbar_state = lumbar_state;
            massage_step     = 0;
            massage_step_ms  = 0;
            massage_start_ms = ms_ticks;
        } else {
            start_vent();
        }
        dbg.massage_on = massage_on;
        break;

    case HS_LUMBAR:
        massage_off();

        switch (lumbar_state) {
        case LUMBAR_OFF:     set_lumbar(LUMBAR_INFLATE); break;
        case LUMBAR_INFLATE: set_lumbar(LUMBAR_HOLD);    break;
        case LUMBAR_HOLD:    set_lumbar(LUMBAR_DEFLATE); break;
        default:             set_lumbar(LUMBAR_OFF);     break;
        }
        break;

    default:
        break;
    }
}

void pneumatics_update(void)
{
    /* The factory firmware refuses to run massage or heat while a motor is
     * moving, presumably a supply-current budget. Mirror it, but only pause:
     * state is kept so the pattern resumes where it left off.
     */
    if (motion_active() != MOTION_NONE) {
        if (!paused) {
            paused         = 1;
            pause_start_ms = ms_ticks;
        }

        /* Same ownership order as the unpaused path below, so a stale flag from
         * a lower priority can never take the valves back. Only the two
         * exhaust-only cases keep running, see vent_tick(); everything that
         * inflates pauses, because inflation is what costs pump current.
         */
        if (massage_on) {
            valves_set(0);              /* pump stops, LED stays lit */
        } else if (lumbar_state == LUMBAR_DEFLATE) {
            lumbar_tick();
        } else if (lumbar_state != LUMBAR_OFF) {
            valves_set(0);              /* inflate and hold both pause */
        } else if (venting) {
            vent_tick();
        } else {
            valves_set(0);
        }
        return;
    }

    /* Coming out of a pause, push every deadline forward by however long it
     * lasted, so the pattern resumes where it left off instead of jumping
     * ahead. The factory gets this for free: its whole timebase is gated by
     * the same motion check, so its counters simply stop.
     */
    if (paused) {
        uint32_t held = ms_ticks - pause_start_ms;

        paused = 0;
        if (massage_step_ms)  massage_step_ms  += held;
        if (massage_start_ms) massage_start_ms += held;

        /* Only deadlines that actually stopped get pushed forward. A vent and a
         * lumbar deflate ran right through the pause, so moving theirs would
         * hold the exhaust open for the length of the motion.
         */
        if (lumbar_state != LUMBAR_DEFLATE) {
            lumbar_state_ms += held;
        }
    }

    if (massage_on) {
        dbg.massage_ms = ms_ticks - massage_start_ms;

        if (dbg.massage_ms >= MASSAGE_TIMEOUT_MS) {
            massage_off();
            start_vent();
            return;
        }

        massage_tick();
    } else if (lumbar_state != LUMBAR_OFF) {
        lumbar_tick();
    } else if (venting) {
        vent_tick();
    } else if (valves_now != 0) {
        valves_set(0);          /* nothing owns these, don't leave them open */
    }

    pump_update();
}
