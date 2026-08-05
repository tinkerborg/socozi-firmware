#include "console.h"

#if RTT

#include "debug.h"
#include "heat.h"
#include "motion.h"
#include "pneumatics.h"
#include "power.h"
#include "rtt.h"

static void line(const char *label, uint32_t value)
{
    rtt_write(label);
    rtt_write_u32(value);
    rtt_write("\r\n");
}

static void show_help(void)
{
    rtt_write("\r\n"
              "  p  positions\r\n"
              "  s  state\r\n"
              "  i  inputs\r\n"
              "  ?  this\r\n");
}

static void show_positions(void)
{
#if ENH_POSITION_TRACK
    line("recline  ", motion_pos_recline());
    line("headrest ", motion_pos_headrest());
    line("at home  ", (uint32_t)motion_at_home());
#endif
    line("motion   ", dbg.motion);
#if ENH_END_OF_TRAVEL_STOP
    line("arrivals ", dbg.arrivals);
#endif
}

static void show_state(void)
{
    line("power    ", (uint32_t)power_is_on());
    line("heat     ", (uint32_t)heat_is_on());
#if ENH_HEAT_LEVELS
    line("heat lvl ", (uint32_t)heat_level());
#endif
    line("massage  ", (uint32_t)pneumatics_massage_on());
#if ENH_MASSAGE_LEVELS
    line("mass lvl ", (uint32_t)pneumatics_massage_level());
#endif
    line("lumbar   ", (uint32_t)pneumatics_lumbar_lit());
    line("valves   ", dbg.valves);
}

/* The three we never identified. Sit in the chair and watch them.
 *
 * `seen` is the sticky OR of everything `inputs` has ever been, so a bit that
 * has only ever been low reads 0 there and is worth nothing; one that has
 * changed is worth chasing.
 */
static void show_inputs(void)
{
    line("inputs   ", dbg.inputs);
    line("seen     ", dbg.inputs_seen);
    line("adc      ", dbg.adc);
    line("hs errs  ", dbg.hs_errors);
}

void console_update(void)
{
    int c = rtt_getc();

    if (c < 0) {
        return;
    }

    /* Printable characters only.
     *
     * A terminal sends the newline along with the key, so one keypress arrives
     * as up to three bytes, and telnet's negotiation lands a few 0xFF sequences
     * in the buffer when the connection opens. Answering all of those is why
     * the help came out several times per press.
     */
    if (c < 0x20 || c > 0x7E) {
        return;
    }

    switch (c) {
    case 'p': show_positions(); break;
    case 's': show_state();     break;
    case 'i': show_inputs();    break;
    default:  show_help();      break;
    }
}

#endif /* RTT */
