#include "macros/preset.h"

#if ENH_PRESET

#include "debug.h"
#include "handset.h"
#include "heat.h"
#include "motion.h"
#include "pneumatics.h"
#include "power.h"
#include "settings.h"
#include "timing.h"

/* Half on, half off, so a full cycle is twice this. The same rate the flatten
 * macro blinks at, because it means the same thing: the chair is moving and
 * nobody is holding a button.
 */
#define PRESET_MOVE_BLINK_MS 500

/* What a recall is doing. One axis at a time, the seat back first, because it
 * is the big move and the headrest reads as an adjustment to it rather than
 * the other way round.
 */
enum {
    STAGE_IDLE = 0,
    STAGE_RECLINE,
    STAGE_HEADREST,
};

static uint8_t stage;
static uint8_t slot_running;

/* Where the current stage is driving to, and which way. */
static uint32_t target_ms;
static uint32_t drive;

/* The armed window, and the flash that acknowledges a save. */
static uint32_t armed_ms;
static int      armed;

static uint32_t flash_ms;
static uint8_t  flash_slot;
static int      flashing;

#define PRESET_FLASH_MS (ADJUST_CONFIRM_FLASHES * 2u * ADJUST_CONFIRM_MS)

/* The slot display arrives and leaves the same way the level bar does, so the
 * two read as the same thing happening to the same four lamps.
 */
enum { LAMPS_OFF, LAMPS_IN, LAMPS_SHOW, LAMPS_OUT };

static uint8_t  lamps;
static uint8_t  lamp_step;
static uint32_t lamp_step_ms;

/* Frozen when the slide begins. What leaves is whatever was on screen at that
 * moment — the four slots on the way out of the window, or the single slot that
 * just took a preset once its flash has finished.
 */
static uint8_t lamp_leaving;

static uint8_t slot_display(void);

static void lamps_start(void)
{
    if (lamps == LAMPS_IN || lamps == LAMPS_SHOW) {
        return;                         /* already up; do not replay it */
    }

    lamps        = LAMPS_IN;
    lamp_step    = 1;
    lamp_step_ms = ms_ticks;
}

static void lamps_end(uint8_t leaving)
{
    if (lamps == LAMPS_OUT) {
        return;
    }

    lamps        = LAMPS_OUT;
    lamp_leaving = leaving;
    lamp_step    = 1;
    lamp_step_ms = ms_ticks;
}

int preset_armed(void)
{
    return armed;
}

void preset_arm(void)
{
    armed    = 1;
    armed_ms = ms_ticks;
    flashing = 0;
    lamps_start();
}

void preset_clear(uint8_t slot)
{
    if (!armed || slot >= PRESET_SLOTS) {
        return;
    }

    settings_clear_preset(slot);

    /* The window stays open, and the button it just emptied changes from lit to
     * blinking under your finger. That is the acknowledgement; a flash would
     * say "saved", which is the opposite of what happened.
     */
    armed_ms = ms_ticks;
}

void preset_cancel(void)
{
    /* No flash: nothing was written, so there is nothing to acknowledge. The
     * display sliding away is the whole of the feedback.
     */
    armed    = 0;
    flashing = 0;
    lamps_end(slot_display());
}

void preset_save(uint8_t slot)
{
    struct settings_preset p = { 0, 0, 0, 0, 0, 0 };

    if (slot >= PRESET_SLOTS) {
        return;
    }

    p.recline  = (uint8_t)(motion_pos_recline() / PRESET_STEP_MS);
    p.headrest = (uint8_t)(motion_pos_headrest() / PRESET_STEP_MS);

    /* Levels as they are now, not as they are remembered. A slot is a snapshot
     * of this chair, so it carries its own and is never disturbed by a later
     * manual adjustment.
     */
    if (heat_is_on()) {
        p.flags |= PRESET_FLAG_HEAT;
        p.heat   = (uint8_t)heat_level();
    }

    if (pneumatics_massage_on()) {
        p.flags  |= PRESET_FLAG_MASSAGE;
        p.massage = (uint8_t)pneumatics_massage_level();
    }

    if (pneumatics_lumbar_lit()) {
        p.flags |= PRESET_FLAG_LUMBAR;
        p.lumbar = pneumatics_lumbar_current();
    }

    settings_set_preset(slot, &p);

    /* The window is answered, so the POWER lamp stops asking and the slot that
     * took it says so instead.
     */
    armed      = 0;
    flashing   = 1;
    flash_slot = slot;
    flash_ms   = ms_ticks;
    lamps      = LAMPS_OFF;             /* the flash owns the lamps now */

    dbg.presets_saved++;
}

static uint32_t pos_for(uint8_t which)
{
    return which == STAGE_RECLINE ? motion_pos_recline()
                                  : motion_pos_headrest();
}

/* Pick a direction for this stage, or MOTION_NONE if it is already close
 * enough. MOTION_SEEK_MS of slop, because the estimate will never land exactly
 * and chasing the last fraction just chatters the relays.
 */
static uint32_t seek(uint8_t which)
{
    uint32_t pos = pos_for(which);

    if (pos + MOTION_SEEK_MS < target_ms) {
        return which == STAGE_RECLINE ? MOTION_RECLINE_UP : MOTION_HEADREST_UP;
    }

    if (target_ms + MOTION_SEEK_MS < pos) {
        return which == STAGE_RECLINE ? MOTION_RECLINE_DOWN
                                      : MOTION_HEADREST_DOWN;
    }

    return MOTION_NONE;
}

static void begin_recline(void)
{
    struct settings_preset p;

    settings_get_preset(slot_running, &p);

    target_ms = (uint32_t)p.recline * PRESET_STEP_MS;
    drive     = seek(STAGE_RECLINE);
    stage     = STAGE_RECLINE;
}

static void begin_headrest(void)
{
    struct settings_preset p;

    settings_get_preset(slot_running, &p);

    target_ms = (uint32_t)p.headrest * PRESET_STEP_MS;
    drive     = seek(STAGE_HEADREST);
    stage     = STAGE_HEADREST;
}

/* The comfort half of a recall, applied once the chair has stopped moving.
 * Heat and massage are refused while a motor runs, so this cannot happen
 * earlier without being silently dropped.
 */
static void restore_comfort(void)
{
    struct settings_preset p;

    settings_get_preset(slot_running, &p);

    if (!power_is_on()) {
        power_toggle();
    }

    /* Said as state, not as presses. Each of these is idempotent, so there is
     * nothing to guard against and nothing that depends on what the last press
     * happened to be.
     *
     * The levels come from the slot and are applied with the "use" calls, which
     * run at a level without remembering it. A recall must leave the last-used
     * memory exactly as the user last set it by hand.
     */
    /* Switching on first, then the level: both of these load the remembered
     * level as they start, so setting it beforehand would just be overwritten.
     */
    heat_set((p.flags & PRESET_FLAG_HEAT) != 0);
    if (p.flags & PRESET_FLAG_HEAT) {
        heat_use_level(p.heat);
    }

    /* Massage and lumbar share the pump and cannot both run, so the flags can
     * never have both set and neither of these can undo the other.
     */
    pneumatics_massage_set((p.flags & PRESET_FLAG_MASSAGE) != 0);
    if (p.flags & PRESET_FLAG_MASSAGE) {
        pneumatics_massage_use_level(p.massage);
    }

    pneumatics_lumbar_set((p.flags & PRESET_FLAG_LUMBAR) != 0, p.lumbar);
}

void preset_recall(uint8_t slot)
{
    if (armed || stage != STAGE_IDLE || slot >= PRESET_SLOTS) {
        return;
    }

    /* An unwritten slot is all zeros, which would recall as "flat, everything
     * off" and look exactly like a preset somebody chose. Doing nothing is the
     * honest answer to a button that has never been given a meaning.
     */
    if (!settings_preset_used(slot)) {
        return;
    }

    slot_running = slot;
    dbg.presets_recalled++;

    begin_recline();

    if (drive == MOTION_NONE) {
        begin_headrest();
    }

    if (drive == MOTION_NONE) {
        /* Nothing to move; this preset is where the chair already is. */
        stage = STAGE_IDLE;
        restore_comfort();
        return;
    }

    motion_request(drive);
}

uint32_t preset_update(uint8_t button)
{
    if (armed && (ms_ticks - armed_ms) >= PRESET_ARM_MS) {
        armed = 0;
        lamps_end(slot_display());
    }

    if (flashing && (ms_ticks - flash_ms) >= PRESET_FLASH_MS) {
        flashing = 0;
    }

    /* Walk the slot display on and off, a lamp at a time, the same way the
     * level bar does.
     */
    if (lamps == LAMPS_IN || lamps == LAMPS_OUT) {
        if ((ms_ticks - lamp_step_ms) >= ADJUST_BAR_STEP_MS) {
            lamp_step_ms = ms_ticks;

            if (++lamp_step > PRESET_SLOTS) {
                lamps = (lamps == LAMPS_IN) ? LAMPS_SHOW : LAMPS_OFF;
            }
        }
    }

    if (stage == STAGE_IDLE) {
        return MOTION_NONE;
    }

    /* Any button cancels, matching the flatten macro. A cancel stops where it
     * is rather than finishing the move or restoring anything.
     */
    if (button != HS_NONE) {
        stage = STAGE_IDLE;
        drive = MOTION_NONE;
        return MOTION_NONE;
    }

    /* A seek ends when the estimate says it has arrived, not only when the
     * motor runs out of travel. Without this check the request is simply
     * renewed every pass and the axis drives to its stop every time.
     */
    if (motion_active() != MOTION_NONE && seek(stage) == MOTION_NONE) {
        motion_request(MOTION_NONE);
    }

    /* Still running: keep the request alive. Nothing is holding a button, so
     * without this the motion would drop on the next pass.
     */
    if (motion_active() != MOTION_NONE) {
        return drive;
    }

    /* This stage has ended, at its target, at a stop, or on the ceiling. */
    if (stage == STAGE_RECLINE) {
        begin_headrest();

        if (drive != MOTION_NONE) {
            motion_request(drive);
            return drive;
        }
    }

    stage = STAGE_IDLE;
    drive = MOTION_NONE;
    restore_comfort();

    return MOTION_NONE;
}

int preset_moving(void)
{
    return stage != STAGE_IDLE;
}

int preset_led_power(void)
{
    /* Asking which slot. Counted from the hold rather than off the free-running
     * clock, so it always starts lit.
     */
    if (armed) {
        uint32_t half = (ms_ticks - armed_ms) / PRESET_ARM_BLINK_MS;

        return (int)((half & 1u) == 0u);
    }

    if (stage != STAGE_IDLE) {
        return (int)((ms_ticks / PRESET_MOVE_BLINK_MS) & 1u);
    }

    /* Nothing to say, so the lamp goes back to meaning what it usually does. */
    return -1;
}

/* Which slots to show, before the arrival or departure animation is applied.
 *
 * Lit means "there is something here, and pressing me replaces it"; blinking
 * means "empty". Without the distinction the window is four dark buttons and no
 * clue which of them does anything.
 */
static uint8_t slot_display(void)
{
    uint8_t lit   = 0;
    uint8_t empty = 0;

    for (uint8_t i = 0; i < PRESET_SLOTS; i++) {
        if (settings_preset_used(i)) {
            lit |= (uint8_t)(1u << i);
        } else {
            empty |= (uint8_t)(1u << i);
        }
    }

    return (((ms_ticks - armed_ms) / PRESET_ARM_BLINK_MS) & 1u)
         ? lit
         : (uint8_t)(lit | empty);
}

uint8_t preset_lamp_mask(void)
{
    if (flashing) {
        uint32_t half = (ms_ticks - flash_ms) / ADJUST_CONFIRM_MS;

        return (half & 1u) ? 0 : (uint8_t)(1u << flash_slot);
    }

    switch (lamps) {
    case LAMPS_IN:
        return adjust_fill_in(slot_display(), lamp_step);

    case LAMPS_SHOW:
        return slot_display();

    case LAMPS_OUT:
        return adjust_slide_out(lamp_leaving, lamp_step);

    default:
        return 0;
    }
}

#endif /* ENH_PRESET */
