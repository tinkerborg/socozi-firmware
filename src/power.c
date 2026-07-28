#include "debug.h"
#include "heat.h"
#include "pneumatics.h"
#include "power.h"

static int on;

int power_is_on(void)
{
    return on;
}

int power_toggle(void)
{
    on = !on;
    dbg.power_on = on;
    return on;
}

void power_comfort_off(void)
{
    pneumatics_shutdown();      /* also starts the 120 s vent */
    heat_off();
}
