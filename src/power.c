#include "debug.h"
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
