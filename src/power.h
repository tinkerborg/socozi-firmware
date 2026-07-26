/* POWER gates the comfort functions, massage, heat and lumbar, the way the
 * factory handset does. It does not gate recline or headrest.
 */

#ifndef POWER_H
#define POWER_H

int  power_is_on(void);

/* Toggle and return the new state. */
int  power_toggle(void);

#endif /* POWER_H */
