#include "gpio.h"
#include "handset.h"
#include "motion.h"
#include "pneumatics.h"
#include "adjust.h"
#include "debug.h"
#include "settings.h"
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

#if ENH_MASSAGE_LEVELS

/* Massage intensity, enhancements-spec.md §2.6.
 *
 * The pattern is the same shape at every level, in the same order. What shrinks
 * is how long each inflating step *lasts*: a shorter step opens the cell for
 * less time, which puts less air in it, and the sequencer moves on sooner. So a
 * gentler massage is also a quicker one, and it keeps moving rather than
 * sitting on a half-inflated cell waiting out a step it has finished with.
 *
 * Rests and vents keep their full duration. They are what lets a cell bleed
 * down between pulses, and shortening them would trap pressure from one pulse
 * into the next — which works against the intensity being asked for.
 *
 * Two percentages per level rather than one, because a flat multiplier ruins
 * the short pulses: a quarter of a 5-tick step is half a tick, which nobody
 * feels, and short pulses are most of the second and third movements. So the
 * scale is interpolated by step length — the long inflations take the full
 * reduction, the short ones much less.
 */
static const struct {
    uint8_t longest_pct;    /* applied to a MASSAGE_STEP_LONG step */
    uint8_t shortest_pct;   /* applied to a MASSAGE_STEP_SHORT step */
} intensity[ADJUST_LEVEL_MAX + 1] = {
    { 100, 100 },   /* [0] unused; level is never 0 */
    {  25,  60 },
    {  50,  72 },
    {  75,  85 },
    { 100, 100 },   /* exactly the reference pattern */
};

#define MASSAGE_LEVEL_DEFAULT ADJUST_LEVEL_MAX

/* The extremes of the table above, in ticks. */
#define MASSAGE_STEP_LONG  80u
#define MASSAGE_STEP_SHORT 5u

/* No inflating step shorter than this, whatever the arithmetic says.
 *
 * It is the length of the shortest step the factory pattern uses, and it is a
 * hard floor because of PUMP_DELAY_MS: after an all-closed step the pump takes
 * 300 ms to come up, so a step much below this one puts in nothing at all. A
 * reduced level must not quietly become no level.
 */
#define MASSAGE_STEP_FLOOR_MS (MASSAGE_STEP_SHORT * MASSAGE_TICK_MS)

static uint8_t massage_level = MASSAGE_LEVEL_DEFAULT;

int pneumatics_massage_level(void)
{
    return massage_level;
}

/* How long this step should last.
 *
 * Only inflating steps are shortened. A rest or a vent keeps its full length,
 * so the cell still has the same time to bleed down before the next pulse.
 */
static uint32_t massage_step_len(const struct pstep *s)
{
    uint32_t full = (uint32_t)s->ticks * MASSAGE_TICK_MS;
    uint32_t lo   = intensity[massage_level].longest_pct;
    uint32_t hi   = intensity[massage_level].shortest_pct;
    uint32_t pct;
    uint32_t ms;

    if (!(s->bits & VALVE_CELLS)) {
        return full;
    }

    if (s->ticks >= MASSAGE_STEP_LONG) {
        pct = lo;
    } else if (s->ticks <= MASSAGE_STEP_SHORT) {
        pct = hi;
    } else {
        pct = hi - (hi - lo) * ((uint32_t)s->ticks - MASSAGE_STEP_SHORT)
                   / (MASSAGE_STEP_LONG - MASSAGE_STEP_SHORT);
    }

    if (pct >= 100) {
        return full;
    }

    ms = full * pct / 100u;

    if (ms < MASSAGE_STEP_FLOOR_MS) {
        ms = MASSAGE_STEP_FLOOR_MS;
    }

    return (ms > full) ? full : ms;
}

#endif /* ENH_MASSAGE_LEVELS */

/* Lumbar is the bottom bladder alone, per the factory table at 0x080049DC.
 * Three presses: inflate, stop-and-hold, deflate.
 *
 * With ENH_LUMBAR_HOLD_SET the states are the same but the presses are not: a
 * press inflates, a release decides whether that was setting a level or asking
 * for the stored one, and a second press deflates. See enhancements-spec.md
 * §2.5.
 */
enum {
    LUMBAR_OFF = 0,
    LUMBAR_INFLATE,
    LUMBAR_HOLD,
    LUMBAR_DEFLATE,
};

#if ENH_LUMBAR_HOLD_SET

/* Released before this and it was a tap, asking for the stored level. Released
 * after it and it was deliberate, setting a new one.
 */
#define LUMBAR_SET_MS 500

/* The stored level is a duration in 100 ms units so that it fits a byte of the
 * settings record. 200 units is the inflate ceiling.
 */
#define LUMBAR_TENTH_MS 100u

/* Where a tap lands when nothing has been stored yet. Half the ceiling rather
 * than the ceiling: a first press should not put a stranger's back through the
 * firmest setting the chair has, and finding it too soft costs one more press
 * where finding it too hard costs a wait for the exhaust.
 */
#define LUMBAR_DEFAULT_MS (LUMBAR_INFLATE_MAX_MS / 2)

/* Where the current inflate stops. Zero while the button is still down, because
 * until it comes up there is no target: the user is choosing one.
 */
static uint32_t lumbar_target_ms;

/* True from the press that started an inflate until the release that ends it.
 * Distinguishes "still choosing" from "running to a target".
 */
static int lumbar_choosing;

/* Air already in the cell when the current inflate burst began, as an inflate
 * duration. Holding the button on a cell that is already up adds to what is
 * there rather than starting again, so the level that gets stored is the total.
 */
static uint32_t lumbar_base_ms;

/* What is actually in the cell, which is not the same as what is remembered.
 *
 * A recall inflates to the preset's level, and the first use of an unset chair
 * inflates to the default — neither should become the remembered level, or the
 * default would be written once and stop being a default. Only holding the
 * button to a firmness you chose does that.
 */
static uint32_t lumbar_level_ms;

#endif

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

    /* Below full intensity an inflating step is simply shorter, so the whole
     * pattern plays through quicker. There is no coasting: the sequencer moves
     * on rather than sitting on a cell it has finished with.
     */
#if ENH_MASSAGE_LEVELS
#define STEP_LEN(step) massage_step_len(step)
#else
#define STEP_LEN(step) ((uint32_t)(step)->ticks * MASSAGE_TICK_MS)
#endif

    if (massage_step_ms == 0) {
        massage_step_ms = ms_ticks;
    } else if ((ms_ticks - massage_step_ms) >= STEP_LEN(s)) {
        massage_step     = (uint8_t)((massage_step + 1) % MASSAGE_STEPS);
        massage_step_ms  = ms_ticks;
        dbg.massage_step = massage_step;
        s                = &massage_pattern[massage_step];
    }

#undef STEP_LEN

    /* Assert the current step's bits every pass, not only on a transition.
     * The factory engine rewrites the valve outputs on every tick, and it
     * matters here: a motion pause drives the valves to 0, so waiting for the
     * next transition would leave them shut for up to 8 s afterwards.
     * valves_set only touches the shift register when the value changes.
     */
    valves_set(s->bits);
}

#if ENH_LUMBAR_HOLD_SET

uint8_t pneumatics_lumbar_level(void)
{
    return settings_lumbar_level();
}

/* Stop here. Rounded to the stored resolution so that what is recalled is
 * exactly what is recorded, rather than a hair under it.
 *
 * `remember` is the difference between a firmness you chose and one you were
 * given. Holding the button is a choice; arriving at a preset's level or the
 * first-use default is not, and writing those would overwrite what you last
 * picked by hand.
 */
static void lumbar_stop_at(uint32_t elapsed_ms, int remember)
{
    uint32_t tenths;

    if (elapsed_ms > LUMBAR_INFLATE_MAX_MS) {
        elapsed_ms = LUMBAR_INFLATE_MAX_MS;
    }

    tenths = (elapsed_ms + LUMBAR_TENTH_MS / 2u) / LUMBAR_TENTH_MS;
    if (tenths > 0xFF) {
        tenths = 0xFF;
    }

    lumbar_level_ms  = tenths * LUMBAR_TENTH_MS;
    dbg.lumbar_level = tenths;

    if (remember) {
        settings_set_lumbar_level((uint8_t)tenths);
    }

    set_lumbar(LUMBAR_HOLD);
}

static void lumbar_keep(uint32_t elapsed_ms)
{
    lumbar_stop_at(elapsed_ms, 1);
}

/* How much air is in the cell right now, as an inflate duration. */
static uint32_t lumbar_banked(void)
{
    switch (lumbar_state) {
    case LUMBAR_INFLATE:
        return lumbar_base_ms + (ms_ticks - lumbar_state_ms);

    case LUMBAR_HOLD:
        return lumbar_level_ms;

    default:
        return 0;
    }
}

/* The press always starts the pump; this is where it turns out what the press
 * meant. Held, it was "this much"; tapped, it was either "the usual amount" on
 * an empty cell or "off" on a full one.
 */
void pneumatics_lumbar_release(void)
{
    uint32_t elapsed;

    if (!lumbar_choosing) {
        return;
    }

    lumbar_choosing = 0;

    /* The inflate may already have finished at the ceiling while the button was
     * still down, in which case the level is set and there is nothing to decide.
     */
    if (lumbar_state != LUMBAR_INFLATE) {
        return;
    }

    elapsed = ms_ticks - lumbar_state_ms;

    /* Held. Whatever was already in the cell plus what this press added is the
     * new level, so holding a cell that is already up firms it further rather
     * than starting over.
     */
    if (elapsed >= LUMBAR_SET_MS) {
        lumbar_keep(lumbar_base_ms + elapsed);
        return;
    }

    /* A tap on a cell that already had air in it means off. There is nowhere
     * else for a short press to go: the level is already what it is, and asking
     * for it again would be a no-op.
     */
    if (lumbar_base_ms > 0) {
        set_lumbar(LUMBAR_DEFLATE);
        return;
    }

    /* A tap from empty: keep going to the level already stored, or to the
     * default if there is none.
     */
    {
        uint8_t stored = settings_lumbar_level();

        lumbar_target_ms = stored ? (uint32_t)stored * LUMBAR_TENTH_MS
                                  : LUMBAR_DEFAULT_MS;
    }

    /* Already past it. There is no way down but the exhaust, and emptying the
     * cell to reach a target overshot by a fraction of a second is worse than
     * the overshoot.
     */
    if (elapsed >= lumbar_target_ms) {
        lumbar_keep(elapsed);
    }
}

#endif /* ENH_LUMBAR_HOLD_SET */

static void lumbar_tick(void)
{
    switch (lumbar_state) {
    case LUMBAR_INFLATE:
        valves_set(VALVE_BOTTOM);

#if ENH_LUMBAR_HOLD_SET
        /* While the button is still down there is no target but the ceiling:
         * the user is choosing, and the ceiling is where choosing runs out.
         * Counted from what was already in the cell, so holding a cell that is
         * part way up cannot push it past full.
         */
        if (lumbar_choosing) {
            if (lumbar_base_ms + (ms_ticks - lumbar_state_ms)
                >= LUMBAR_INFLATE_MAX_MS) {
                lumbar_keep(LUMBAR_INFLATE_MAX_MS);
            }
        } else if ((ms_ticks - lumbar_state_ms) >= lumbar_target_ms) {
            /* Arrived at a level somebody asked for rather than chose, so it
             * does not become the remembered one. That is what keeps the
             * first-use default a default instead of writing it once.
             */
            lumbar_stop_at(lumbar_base_ms + lumbar_target_ms, 0);
        }
#else
        if ((ms_ticks - lumbar_state_ms) >= LUMBAR_INFLATE_MAX_MS) {
            set_lumbar(LUMBAR_HOLD);
        }
#endif
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
#if ENH_MASSAGE_LEVELS
    adjust_close(ADJUST_MASSAGE);
#endif
}

#if ENH_MASSAGE_LEVELS

/* Anything outside the range means the store had nothing usable to say, so the
 * chair falls back to full intensity, which is the reference pattern.
 */
static uint8_t remembered_intensity(void)
{
    uint8_t v = settings_massage_level();

    return (v >= 1 && v <= ADJUST_LEVEL_MAX) ? v : MASSAGE_LEVEL_DEFAULT;
}

static void massage_start(void)
{
    if (massage_on) {
        return;
    }

    massage_on       = 1;
    dbg.massage_on   = 1;
    venting          = 0;   /* massage owns the valves, see set_lumbar */
    lumbar_state     = LUMBAR_OFF;
    dbg.lumbar_state = lumbar_state;
    massage_step     = 0;
    massage_step_ms  = 0;
    massage_start_ms = ms_ticks;
    massage_level    = remembered_intensity();
    dbg.massage_level = massage_level;
}

void pneumatics_massage_arm(void)
{
    /* Adjusting the intensity does not start the massage. Unlike heat, where a
     * level only means anything while it is running, an intensity is a setting
     * you might want to change before switching anything on — and a hold that
     * started the pump would make that impossible.
     *
     * Nothing starts here, so there is no power budget to check.
     */
    if (!massage_on) {
        massage_level     = remembered_intensity();
        dbg.massage_level = massage_level;
    }

    adjust_open(ADJUST_MASSAGE, massage_level);
}

void pneumatics_massage_use_level(uint8_t want)
{
    if (want == 0 || want > ADJUST_LEVEL_MAX) {
        return;
    }

    massage_level     = want;
    dbg.massage_level = want;
}

void pneumatics_massage_set_level(uint8_t want)
{
    pneumatics_massage_use_level(want);

    if (want >= 1 && want <= ADJUST_LEVEL_MAX) {
        settings_set_massage_level(want);
    }
}

#endif /* ENH_MASSAGE_LEVELS */

void pneumatics_shutdown(void)
{
    massage_off();
    lumbar_state     = LUMBAR_OFF;
    dbg.lumbar_state = lumbar_state;
#if ENH_LUMBAR_HOLD_SET
    lumbar_choosing = 0;
    lumbar_base_ms  = 0;
#endif
    start_vent();
}

void pneumatics_button(uint8_t button)
{
    switch (button) {
    case HS_MASSAGE:
#if ENH_MASSAGE_LEVELS
        /* Done choosing: take the intensity that is showing and carry on. Not
         * an off switch; there is a question on the table and this answers it.
         */
        if (adjust_armed() && adjust_owner() == ADJUST_MASSAGE) {
            adjust_accept();
            break;
        }

        if (massage_on) {
            massage_off();
            start_vent();
        } else {
            massage_start();
            adjust_show(ADJUST_MASSAGE, massage_level);
        }
#else
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
#endif
        break;

    case HS_LUMBAR:
        massage_off();

#if ENH_LUMBAR_HOLD_SET
        /* The press always starts the pump, whether the cell is empty or
         * already up. Which of the two it meant is decided on release, in
         * pneumatics_lumbar_release(): held, it adds; tapped on a full cell, it
         * was an off switch after all.
         *
         * Deciding here instead is what made holding a full cell deflate
         * immediately, with no way to say "more".
         */
        lumbar_base_ms   = lumbar_banked();
        lumbar_choosing  = 1;
        lumbar_target_ms = LUMBAR_INFLATE_MAX_MS;
        set_lumbar(LUMBAR_INFLATE);
#else
        switch (lumbar_state) {
        case LUMBAR_OFF:     set_lumbar(LUMBAR_INFLATE); break;
        case LUMBAR_INFLATE: set_lumbar(LUMBAR_HOLD);    break;
        case LUMBAR_HOLD:    set_lumbar(LUMBAR_DEFLATE); break;
        default:             set_lumbar(LUMBAR_OFF);     break;
        }
#endif
        break;

    default:
        break;
    }
}

/* Was this deadline already running when the pause began?
 *
 * Signed difference, so it stays right across the millisecond counter's wrap.
 * A zero stamp means "never started", which the massage engine uses.
 */
static int started_before_pause(uint32_t stamp)
{
    return stamp != 0 && (int32_t)(stamp - pause_start_ms) <= 0;
}

void pneumatics_massage_set(int on)
{
    if (on == (massage_on != 0)) {
        return;
    }

    if (!on) {
        massage_off();
        start_vent();
        return;
    }

    if (motion_active() != MOTION_NONE) {
        return;                         /* power budget, same as a press */
    }

#if ENH_MASSAGE_LEVELS
    massage_start();
#else
    massage_on       = 1;
    dbg.massage_on   = 1;
    venting          = 0;
    lumbar_state     = LUMBAR_OFF;
    dbg.lumbar_state = lumbar_state;
    massage_step     = 0;
    massage_step_ms  = 0;
    massage_start_ms = ms_ticks;
#endif
}

uint8_t pneumatics_lumbar_current(void)
{
#if ENH_LUMBAR_HOLD_SET
    return (uint8_t)(lumbar_level_ms / LUMBAR_TENTH_MS);
#else
    return 0;
#endif
}

void pneumatics_lumbar_set(int on, uint8_t tenths)
{
    if (on == (lumbar_state == LUMBAR_INFLATE || lumbar_state == LUMBAR_HOLD)) {
        return;
    }

    if (!on) {
        set_lumbar(LUMBAR_DEFLATE);
        return;
    }

    if (motion_active() != MOTION_NONE) {
        return;
    }

#if ENH_LUMBAR_HOLD_SET
    {
        /* Asked for outright, or the remembered one, or the first-use default,
         * in that order. Straight to a target with no press to interpret, and
         * the cell is empty so what is already in it is nothing.
         */
        uint8_t want = tenths ? tenths : settings_lumbar_level();

        lumbar_base_ms   = 0;
        lumbar_choosing  = 0;
        lumbar_target_ms = want ? (uint32_t)want * LUMBAR_TENTH_MS
                                : LUMBAR_DEFAULT_MS;
    }
#else
    (void)tenths;
#endif

    massage_off();
    set_lumbar(LUMBAR_INFLATE);
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

        /* Only deadlines that actually stopped get pushed forward.
         *
         * "Actually stopped" means two things. A vent and a lumbar deflate ran
         * right through the pause, so moving theirs would hold the exhaust open
         * for the length of the motion. And anything *started* during the pause
         * never waited at all: a preset recall sets lumbar going in the same
         * pass that ends its own motion, so the unwind lands the pass after,
         * and crediting it the whole move would push its deadline into the
         * future. The subtraction then underflows and it jumps straight to
         * hold, lit but empty.
         */
        if (started_before_pause(massage_step_ms))  massage_step_ms  += held;
        if (started_before_pause(massage_start_ms)) massage_start_ms += held;

        if (lumbar_state != LUMBAR_DEFLATE
            && started_before_pause(lumbar_state_ms)) {
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
