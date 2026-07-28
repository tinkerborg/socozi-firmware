/* Enhancement feature flags.
 *
 * Everything this firmware does beyond the factory behaviour sits behind a flag
 * from this header. `ENHANCED` is the master switch, defined on the command
 * line by the Makefile:
 *
 *     make              ENHANCED=1, the default build, all enhancements on
 *     make reference    ENHANCED=0, factory-equivalent behaviour
 *
 * The reference build is what we fall back to when an enhancement misbehaves in
 * the chair, so it has to stay buildable and testable. `make test` runs the host
 * tests against both.
 *
 * Per-enhancement flags default to ENHANCED, so one enhancement can be switched
 * off without giving up the others. Guard code with `#if`, never `#ifdef`, every
 * flag here is always defined as 0 or 1.
 *
 * Behaviour behind these flags is specified in docs/enhancements-spec.md;
 * docs/firmware-spec.md stays a description of the reference firmware alone.
 */

#ifndef ENHANCEMENTS_H
#define ENHANCEMENTS_H

#ifndef ENHANCED
#define ENHANCED 1
#endif

#if ENHANCED != 0 && ENHANCED != 1
#error "ENHANCED must be 0 or 1"
#endif

/* --- individual enhancements ---------------------------------------------
 *
 * One flag per enhancement, each defaulting to ENHANCED.
 */

/* Stop MOTION_FLATTEN when the current sense reads zero, which means every
 * motor has disconnected itself at its internal limit switch. See
 * docs/firmware-spec.md §9 and docs/enhancements-spec.md §2.1.
 */
#ifndef ENH_END_OF_TRAVEL_STOP
#define ENH_END_OF_TRAVEL_STOP ENHANCED
#endif

/* Double-press POWER as a shortcut for holding it: same shutdown macro, but it
 * runs unattended and ends itself at the stops. See enhancements-spec.md §2.2.
 */
#ifndef ENH_POWER_DOUBLE_TAP
#define ENH_POWER_DOUBLE_TAP ENHANCED
#endif

/* The double-tap macro runs with nothing held, so without the end-of-travel
 * stop it would drive into open limit switches until the 30 s ceiling expired.
 */
#if ENH_POWER_DOUBLE_TAP && !ENH_END_OF_TRAVEL_STOP
#error "ENH_POWER_DOUBLE_TAP requires ENH_END_OF_TRAVEL_STOP"
#endif

/* Four heat levels, delivered by duty cycling the element. See
 * enhancements-spec.md §2.3.
 */
#ifndef ENH_HEAT_LEVELS
#define ENH_HEAT_LEVELS ENHANCED
#endif

/* Remember settings across a reset, in a reserved flash page. See
 * enhancements-spec.md §2.4. Currently the heat and lumbar levels.
 */
#ifndef ENH_SETTINGS_PERSIST
#define ENH_SETTINGS_PERSIST ENHANCED
#endif

/* Hold LUMBAR to inflate to where you want it, then tap to go back there,
 * instead of the factory's inflate/hold/deflate/off cycle. See
 * enhancements-spec.md §2.5.
 */
#ifndef ENH_LUMBAR_HOLD_SET
#define ENH_LUMBAR_HOLD_SET ENHANCED
#endif

/* Four massage intensities, chosen the same way heat levels are: hold MASSAGE
 * and pick. The pattern is unchanged at every level; what shrinks is how much
 * of each inflating step the pump runs for. See enhancements-spec.md §2.6.
 */
#ifndef ENH_MASSAGE_LEVELS
#define ENH_MASSAGE_LEVELS ENHANCED
#endif

/* Whether anything uses adjust.c, which owns the shared bar graph and the
 * borrowed motion buttons. Both users are optional and either one alone is
 * enough to need it.
 */
#define ADJUST_IN_USE (ENH_HEAT_LEVELS || ENH_MASSAGE_LEVELS)

#endif /* ENHANCEMENTS_H */
