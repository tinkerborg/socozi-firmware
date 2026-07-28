/* What the handset does.
 *
 * Routes button events to the comfort functions, the motion axes and the
 * macros, and mirrors the resulting state back to the handset LEDs. Everything
 * user-facing lives here; main.c only brings the board up and runs the loop.
 */

#ifndef CONTROL_H
#define CONTROL_H

/* Call once per main-loop pass, after handset_poll(). */
void control_update(void);

#endif /* CONTROL_H */
