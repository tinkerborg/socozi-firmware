/* Seat heater on PC14.
 *
 * On/off only. The hardware has no temperature sensor and the factory firmware
 * does no duty cycling. The element is simply energised. The 60 minute
 * auto-off is therefore the only bound on a stuck-on heater, and is not
 * optional.
 */

#ifndef HEAT_H
#define HEAT_H

/* Toggle. Refused while a motor is moving, per the power budget. */
void heat_button(void);

/* Call every loop: drives the output, applies the auto-off, and cuts the
 * element while a motor runs.
 */
void heat_update(void);

void heat_off(void);
int  heat_is_on(void);

#endif /* HEAT_H */
