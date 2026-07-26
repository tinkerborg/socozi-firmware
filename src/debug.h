/* Debug block, live state readable over SWD.
 *
 * Pinned to the start of RAM (0x20000000) by the linker script so a debugger
 * can find it without symbols:
 *
 *     monitor mdw 0x20000000 40
 *
 * Write-only from the firmware's side: nothing here feeds back into behaviour,
 * so a debugger can read it freely and a corrupt value cannot move the chair.
 *
 * Field order here is authoritative; offsets have changed as fields were added,
 * so read the struct rather than trusting a remembered offset.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

#define DEBUG_MAGIC 0x44424730u  /* "DBG0" */

/* dbg.motion values */
enum {
    MOTION_NONE = 0,
    MOTION_RECLINE_UP,
    MOTION_RECLINE_DOWN,
    MOTION_HEADREST_UP,
    MOTION_HEADREST_DOWN,
    MOTION_FLATTEN,     /* both axes down together, the long-press POWER macro */
};

#define ADC_FLAG_RSTCLB_TIMEOUT (1u << 0)
#define ADC_FLAG_CLB_TIMEOUT    (1u << 1)
#define ADC_FLAG_EOC_TIMEOUT    (1u << 2)

struct debug_block {
    uint32_t magic;         /* DEBUG_MAGIC once main() is running */
    uint32_t ticks;         /* free-running loop counter, proves we're alive */

    /* --- analog --- */
    uint32_t adc;           /* channel 7 (PA7), 0-4095, or 0xFFFFFFFF on timeout */
    uint32_t adc_min;
    uint32_t adc_max;
    uint32_t adc_ch7_max;   /* peak on the presumed current-sense channel */
    uint32_t adc_ch8;       /* PB0 */
    uint32_t adc_ch9;       /* PB1 */
    uint32_t adc_flags;     /* ADC_FLAG_*, non-zero means the ADC misbehaved */

    /* --- raw port state --- */
    uint32_t istat_a;
    uint32_t istat_b;
    uint32_t istat_c;
    uint32_t octl_a;        /* readback of what we're driving */
    uint32_t octl_b;
    uint32_t octl_c;

    /* The three unidentified inputs: bit0 = PB8, bit1 = PB9, bit2 = PB12. */
    uint32_t inputs;
    uint32_t inputs_seen;   /* sticky OR of every value `inputs` has taken */

    /* --- handset --- */
    uint32_t hs_frames;     /* valid frames received */
    uint32_t hs_polls;      /* poll frames sent */
    uint32_t hs_bytes;      /* raw bytes received, valid frame or not */
    uint32_t hs_last_byte;  /* for debugging framing */
    uint32_t hs_errors;     /* USART overrun/framing/noise events, see handset.c */
    uint32_t hs_last_error; /* STAT bits from the most recent one */
    uint32_t hs_button;     /* button code from the most recent frame */
    uint32_t leds;          /* LED bitmap being sent */
    uint32_t presses;       /* button press edges seen */

    /* --- motion --- */
    uint32_t motion;        /* MOTION_* */
    uint32_t motion_ms;     /* how long the current motion has run */
    uint32_t stops;         /* safety stops, any cause */
    uint32_t stalls;        /* of those, over-current trips */

    /* --- pneumatics --- */
    uint32_t power_on;      /* comfort functions enabled */
    uint32_t valves;        /* valve bits latched in the shift register */
    uint32_t massage_on;
    uint32_t massage_step;
    uint32_t massage_ms;    /* elapsed, against the 15 min auto-off */
    uint32_t lumbar_state;

    /* --- heat --- */
    uint32_t heat_on;
    uint32_t heat_ms;       /* elapsed, against the 60 min auto-off */
};

extern volatile struct debug_block dbg;

#endif /* DEBUG_H */
