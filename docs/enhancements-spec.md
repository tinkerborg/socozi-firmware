# Chair firmware, enhancements specification

Normative spec for behaviour this firmware adds **beyond** the factory chair.
[firmware-spec.md](firmware-spec.md) stays a description of the reference
firmware alone; anything specified here is absent from a `make reference` build.

**Process: update this spec before changing the code**, same rule as the
firmware spec. If behaviour and spec disagree, one of them is a bug, decide
which, then fix it.

Status key: **[done]** implemented and verified on the chair ·
**[partial]** implemented, not fully verified · **[planned]** not implemented.

---

## 1. The feature flag

All enhancements are compile-time gated by `ENHANCED`, defined by the Makefile
and defaulted in [../src/enhancements.h](../src/enhancements.h).

```sh
make                   # ENHANCED=1, build/socozi.bin, the default image
make reference         # ENHANCED=0, build/socozi-reference.bin
make flash             # flash the enhanced image
make flash-reference   # flash the reference image
```

The enhanced build is the default and the one that runs on the chair. The
reference build exists as the fallback: if an enhancement misbehaves in a way
that is awkward to debug from a chair someone is sitting in, `make
flash-reference` gets back to known-good factory behaviour in one step, without
checking out an older commit.

Requirements on the flag itself:

- A build with `ENHANCED=0` **SHALL** behave exactly as
  [firmware-spec.md](firmware-spec.md) describes. No enhancement may change
  reference behaviour, including timing and pin state, when compiled out.
- Every enhancement **SHALL** sit behind a flag from `enhancements.h`, and each
  per-enhancement flag **SHALL** default to `ENHANCED`. This keeps a single
  enhancement switchable without giving up the rest.
- Guards **SHALL** use `#if`, not `#ifdef`. Every flag is always defined, as 0
  or 1, so a misspelled name is a compile error rather than a silently disabled
  feature.
- Both variants **SHALL** build, and `make test` **SHALL** run the host tests
  against both. `make ci` builds both.
- Safety bounds are **not** enhancements and **SHALL NOT** be flagged off: the
  motion timeout, stall detection, direction-change stop, and the heater's 60
  minute auto-off apply identically in both builds. An enhancement may make a
  bound tighter, never looser.

## 2. Enhancements

### 2.1 End-of-travel stop, `ENH_END_OF_TRAVEL_STOP` **[done]**

Reference behaviour: `MOTION_FLATTEN` drives until the button is released or the
30 s ceiling expires, holding the relays closed against an open limit switch for
whatever is left.

Both ends of travel open a limit switch **inside the actuator**, so the motor
disconnects itself and the sense channel falls to zero rather than rising. Both
axes share one channel, so zero means every motor has arrived. This is the
signal [firmware-spec.md](firmware-spec.md#9-current-sensing-and-stall-detection)
§9 already identifies under "Still open".

- `MOTION_FLATTEN` **SHALL** stop when the current sense reads zero
  continuously for `MOTION_ARRIVED_MS`, no earlier than `MOTION_ARM_MS` after
  the first contact closes. Tunables in `motion.h`, 300 ms and 500 ms.
- The arming window exists because zero is also what the channel reads before
  anything is energised, across `MOTION_SETTLE_MS`, the recline stagger, and the
  inrush spike. The hold time exists so one dropped sample cannot stop a motion
  mid-travel.
- A failed conversion (`0xFFFFFFFF`) **SHALL NOT** count as zero. No reading is
  not a zero reading, so ADC trouble falls back to the timeout rather than
  stopping early.
- Arrival **SHALL** latch until the request drops to `MOTION_NONE`, exactly as a
  fault does. Without it a held POWER re-requests immediately, restarts the
  600 ms sequence, and chatters the relays against the open switch.
- Arrival is **not** a safety stop. It counts in `dbg.arrivals`, and **SHALL
  NOT** increment `dbg.stops` or `dbg.stalls`.
- Scoped to `MOTION_FLATTEN`. The four button-held motions still run until
  release; extending it to them is still §9's open question.
- `MOTION_TIMEOUT_MS` and stall detection are unchanged and remain the backstop
  if the current sense ever lies.

**Unproven, like stall detection:** the zero-at-the-stops reading is measured on
the chair and logged, but this is the first thing to make a control decision
from it.

### 2.2 Double-tap POWER, `ENH_POWER_DOUBLE_TAP` **[done]**

A shortcut for holding POWER, **not** a second behaviour. Requires §2.1.

- Two POWER presses within `POWER_DOUBLE_MS` (400 ms, `main.c`) **SHALL** run
  the same macro a hold runs, via the same `power_long_press()`.
- The hold **SHALL** keep working unchanged, in both builds.
- The move runs unattended, so the request **SHALL** be renewed until motion
  ends. Where the held version ends on release, this one ends when the motion
  does, at the stops per §2.1 or on the ceiling, and then runs the same
  `power_macro_finish()`: comfort off, gate off, vent.
- Any button press **SHALL** cancel it, matching the rule that any other button
  interrupts the held version. A cancel aborts, and does not run the shutdown.
- POWER **SHALL** blink at `POWER_BLINK_MS` (500 ms, half on half off) for the
  duration, so an unattended move is visibly deliberate and not a stuck button.
  The gate is still on during the move, so without this the LED would simply sit
  lit.
- Counted in `dbg.auto_moves`.

The first tap runs its ordinary power toggle before a second one is known to be
coming. This is deliberate: nothing has to be deferred, so a single press keeps
its usual immediate response, and the macro ends with everything off whichever
way that toggle went. The visible effect is that a double tap starts its vent at
the first tap rather than at the end.

### Not covered by tests

`power_long_press()`, `power_macro_finish()` and the double-tap detection live in
`main.c`, which is not in the host test build. Both flags are exercised only
through `motion.c` and `pneumatics.c`; the macro sequencing itself is verified on
the chair. Splitting it out of `main.c` would fix that.
