/* POWER gates the comfort functions, massage, heat and lumbar, the way the
 * factory handset does. It does not gate recline or headrest.
 */

#ifndef POWER_H
#define POWER_H

int  power_is_on(void);

/* Toggle and return the new state. */
int  power_toggle(void);

/* Switch off everything the gate covers, and start the pneumatic vent.
 *
 * Lives here because this is the module that decides what "comfort functions"
 * means; both the power-off path and the shutdown macro need it.
 */
void power_comfort_off(void);

#endif /* POWER_H */
