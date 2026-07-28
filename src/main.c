/* Replacement firmware for the SoCozi recliner controller (GD32E23x).
 *
 * See docs/firmware-spec.md for what this is meant to do, and
 * docs/firmware-map.md for how the factory firmware it replaces works.
 *
 * This file brings the board up, samples the inputs into the debug block, and
 * runs the loop. What the chair actually *does* is control.c.
 *
 * SAFETY: motion is protected by over-current detection (motion.c) backed by a
 * hard time ceiling, and the heater by a 60 minute auto-off. There is no
 * temperature sensor anywhere in this hardware, so that timer is the only thing
 * bounding a stuck-on heater. Don't remove these.
 */

#include "adc.h"
#include "control.h"
#include "debug.h"
#include "gd32e23x.h"
#include "gpio.h"
#include "handset.h"
#include "settings.h"
#include "timing.h"
#include "watchdog.h"

/* How often to poll the handset. It never transmits unprompted. */
#define HANDSET_POLL_MS 20

volatile struct debug_block dbg __attribute__((section(".debug_block")));

static void sample_inputs(void)
{
    uint32_t b  = gpio_istat_b();
    uint32_t in = 0;

    if (b & (1u << 8))  in |= 1u << 0;
    if (b & (1u << 9))  in |= 1u << 1;
    if (b & (1u << 12)) in |= 1u << 2;

    dbg.istat_a = gpio_istat_a();
    dbg.istat_b = b;
    dbg.istat_c = gpio_istat_c();
    dbg.inputs  = in;
    dbg.inputs_seen |= in;
}

static void sample_adc(void)
{
    uint32_t v = adc_read(ADC_CH_CURRENT);

    dbg.adc     = v;
    dbg.adc_ch8 = adc_read(ADC_CH_8);
    dbg.adc_ch9 = adc_read(ADC_CH_9);

    if (v != 0xFFFFFFFF) {
        if (v < dbg.adc_min)      dbg.adc_min      = v;
        if (v > dbg.adc_max)      dbg.adc_max      = v;
        if (v > dbg.adc_ch7_max)  dbg.adc_ch7_max  = v;
    }
}

/* The debug block lives in a NOLOAD section, so startup does not clear it.
 * Without this, stale RAM from a previous image reads back as plausible data.
 */
static void debug_init(void)
{
    volatile uint32_t *p = (volatile uint32_t *)&dbg;

    for (unsigned i = 0; i < sizeof(dbg) / 4; i++) {
        p[i] = 0;
    }

    dbg.adc_min = 0xFFFFFFFF;
    dbg.magic   = DEBUG_MAGIC;
}

int main(void)
{
    uint32_t last_poll_ms = 0;

    timing_init();
    gpio_init();
    debug_init();
    adc_init();
    handset_init();

    /* After debug_init, which zeroes the block the counters live in, and before
     * the loop, which is where anything asks for a stored value.
     */
    settings_init();

    watchdog_init();

    for (;;) {
        sample_adc();
        sample_inputs();

        if ((ms_ticks - last_poll_ms) >= HANDSET_POLL_MS) {
            last_poll_ms = ms_ticks;
            handset_send(HS_TYPE_IDLE, handset_leds(), 0, 0);
        }

        handset_poll();
        control_update();

        dbg.octl_a = gpio_octl_a();
        dbg.octl_b = gpio_octl_b();
        dbg.octl_c = gpio_octl_c();

        watchdog_kick();
        dbg.ticks++;
    }
}
