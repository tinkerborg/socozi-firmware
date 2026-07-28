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

### 2.3 Heat levels, `ENH_HEAT_LEVELS` **[done]**

Reference behaviour: HEAT is a plain toggle and the element is driven fully on,
continuously, per [firmware-spec.md](firmware-spec.md#heat-done) §8.

This adds four heat levels, delivered by duty cycling the element. The design
problem is that the handset has no spare buttons and reports one code at a time,
so a level has to be expressed with the buttons that already exist. It is done
by borrowing the four motion buttons for a few seconds and saying clearly, on
the lamps, that they have been borrowed.

Tapping HEAT does not ask about level at all. It returns you to the level you
were last using, which is the answer nearly every time. Choosing a different one
is a separate, deliberate gesture.

#### Switching on and off

- HEAT **SHALL** act on **release**, not on the press edge, so a tap can be told
  apart from a hold. This deviates from
  [firmware-spec.md](firmware-spec.md#6-handset) §6, which has comfort functions
  act on the press; POWER already works this way for the same reason.
- A **tap** with heat off **SHALL** switch it on at the remembered level. A tap
  with heat on **SHALL** switch it off.
- The remembered level **SHALL** start at `HEAT_LEVEL_DEFAULT`, the top, so an
  untouched chair behaves as the factory one does. It is held in RAM and does
  not survive a reset; there is nowhere to persist it that is worth a flash
  write per adjustment.
- Switching on is refused while a motor is moving, unchanged from reference.

#### Choosing a level

- Holding HEAT for `BUTTON_HEAT_HOLD_MS` (1250 ms) **SHALL** switch heat on if
  it was off, and **SHALL** hand the four motion buttons over to the levels for
  `HEAT_ARM_MS` (8 s). Held HEAT is shorter than held POWER's 2 s on purpose:
  POWER's hold starts the chair moving and should be hard to do by accident,
  where this only opens something you can ignore.
- While handed over, the four motion buttons **SHALL** mean levels 1 to 4,
  bottom to top as the bar graph shows them: headrest ▼, headrest ▲, recline ▼,
  recline ▲.
- A press taken as a level **SHALL NOT** reach the motors, and **SHALL** be
  suppressed until that button is released. Without the latch the level lands,
  the window closes on the same press, and the motor starts with the button
  still down.
- A pick **SHALL** apply immediately and reopen the window for
  `HEAT_ARM_REPICK_MS` (2 s), renewed by each further pick, so a level can be
  felt and then corrected.
- A short HEAT press while handed over **SHALL** accept the level showing and
  end the window. It does not switch heat off; there is a question on the table
  and the press answers it.
- The window otherwise **SHALL** end on its own, on whatever level is showing.
- The buttons **SHALL** return to the motors when the window ends, not when the
  bar has finished animating away.

#### The bar graph

The four motion lamps show the level while it is relevant, and are otherwise
left alone.

- A tap that switches heat on **SHALL** raise the bar for `HEAT_BAR_MS` (2 s) as
  a **readout only**: the four buttons keep their motor function throughout, and
  any button other than HEAT **SHALL** take the bar down early. HEAT itself does
  not, because that press may be the start of a hold.
- A hold **SHALL** raise the same bar as a **menu**, for as long as the buttons
  are handed over. Pressing one of the things a menu is offering does not
  dismiss it.
- The bar **SHALL** animate in one lamp at a time from the bottom, at
  `HEAT_BAR_STEP_MS` (80 ms) a lamp.
- The bar **SHALL** animate out by shifting the whole bar up one lamp per step,
  discarding whatever runs off the top, until nothing is lit. One rule for every
  level: a level four bar empties from the bottom, and a level one bar is a
  single dot that travels up and leaves.
- Whichever frame is the **last one with a lamp still lit** **SHALL** be held for
  `HEAT_BAR_EXIT_MS` (200 ms) rather than a step. How long the top lamp stays
  lit otherwise depends on the width of the bar, and at level one it is a single
  step, which the eye misses because it is already following the dot off the
  edge.
- A pick **SHALL** snap the bar to the new level rather than replaying the fill.
  The animation is an entrance; once the bar is up a pick should land under your
  finger.
- Switching heat off **SHALL** take the bar down without the slide. There is
  nothing left to get out of the way of.
- A running motion **SHALL** outrank the bar on those four lamps. They mirror the
  motors, and a motion saying what it is doing outranks heat saying what it is
  about to do.

#### The HEAT lamp

- **SHALL** blink at `HEAT_ARM_BLINK_MS` (250 ms) while the buttons are handed
  over, and be steady otherwise for as long as heat is on. Blinking therefore
  means "still changeable", and it stops the moment the buttons go back to the
  motors.

#### Duty cycling

- The element **SHALL** be switched on a `HEAT_DUTY_PERIOD_MS` (20 s) cycle at
  25% / 50% / 75% / 100% for levels 1 to 4, phased from the moment heat was
  switched on so the cycle always begins with the element energised.
- Level 4 is 100%, that is, continuous. The top level is exactly the reference
  behaviour, so the enhancement only ever removes heat.
- The period is deliberately slow. Fast switching would put noise on a board
  that already has unexplained USART errors on the handset cable, and the gate
  drive on the heater FET is not characterised. The element's thermal mass
  averages a 20 s cycle without help.
- Duty cycling **SHALL** be the only thing the level changes. It is not a
  temperature setting; there is no sensor and nothing closes the loop.

#### Bounds, unchanged

- The 60 minute auto-off is a safety bound, not an enhancement, and **SHALL**
  remain wall clock at every level. Level one gets the same 60 minutes as level
  four, not four times longer.
- The element **SHALL** still be cut while a motor runs, and still resume by
  itself, per §8's power budget. The duty cycle keeps running underneath, so a
  motion does not re-phase it.

Reported as `dbg.heat_level`, 0 when off.

### 2.4 Persistent settings, `ENH_SETTINGS_PERSIST` **[planned]**

Reference behaviour: nothing is remembered. Every reset starts from the
compiled-in defaults, which is what the factory firmware does — the flash region
that looked like a settings area turned out not to be one, see
[firmware-map.md](firmware-map.md#next-steps).

There is no EEPROM on this part, and the backup registers need a VBAT the board
does not provide, so main flash is the only option. It is used as an
**append-only log in one reserved page**, not as a fixed location that gets
rewritten.

- The last 4 KiB page **SHALL** be reserved for the store and **SHALL NOT** be
  used by the linker for anything else.
- A record **SHALL** be one 32-bit word: the payload in the low half and its
  complement in the high half. An erased word is `0xFFFFFFFF`, whose halves are
  not complements, so an erased slot is invalid by construction and needs no
  separate in-use marker.
- Records **SHALL** be appended to the first erased slot. The live value is the
  **highest-indexed valid record**, which works because writes only ever move
  forward.
- When the page is full it **SHALL** be erased and writing **SHALL** restart at
  slot zero.
- A record **SHALL** be written only when the value actually differs from the
  one already committed. Choosing a level and then changing back to the
  committed one writes nothing.

#### Wear

Not a consideration, and the arithmetic is worth writing down so nobody has to
redo it. The datasheet gives **100 kcycles** endurance and 25 years retention.
At 1024 slots per erase that is on the order of 10⁸ saves. A chair whose heat
level changed every hour of every day would take roughly ten thousand years.

#### When it is safe to write

The core executes from flash, so a program or erase stalls instruction fetch:
the main loop stops for the duration, and every output stays latched exactly
where it was. Nothing can be stopped while it is stalled.

The datasheet's worst figure in the flash timing table is **42 ms**. Against the
margins this firmware already relies on — `MOTION_STALL_MS` of 4 s,
`HANDSET_TIMEOUT_MS` of 250 ms, a 2 s watchdog — that is comfortably inside all
of them, so no gate is strictly required.

- A commit **SHALL** nonetheless be deferred while a motion is running. It costs
  nothing: there are 1024 saves of slack before an erase is due, so waiting for
  a quiet moment buys margin that then does not have to be reasoned about.
- A commit **SHALL** also be deferred while a heat level is being chosen, so one
  adjustment writes one record rather than one per press.
- A failed write **SHALL NOT** be retried. The value stays live in RAM and the
  chair works normally; retrying every loop would hammer a page that has already
  said no. Counted in `dbg.settings_errors`.

#### What is stored

Just the heat level for now, in the low byte, with the rest of the payload
reserved. A level outside 1..`HEAT_LEVEL_MAX` **SHALL** be treated as absent and
fall back to `HEAT_LEVEL_DEFAULT`, so a corrupt or half-written record degrades
to a working chair rather than a refusing one.

Writes counted in `dbg.settings_writes`, erases in `dbg.settings_erases`.

### Where this lives

The macros are in [../src/macros/](../src/macros/), one file per macro. Hold and
double tap are two entry points into the same behaviour, not two
implementations. Trigger detection is `button.c`, which turns button codes into
press, tap, double-tap, hold and hold-release events and knows nothing about
what any of them mean; `control.c` routes those events.

`button.c` knows one button apart, in `hold_ms_for()`, because HEAT's hold is
shorter than POWER's. That is the whole of the special-casing; the alternative
was threading a threshold in from `control.c`, which would put timing back in
the caller the module exists to take it out of.

Level, duty, the window and the bar animation all live in `heat.c`, which
publishes the bar as a lamp mask rather than a level, because a bar sliding off
the top is not a level. `control.c` owns the mapping from that mask to actual
lamps, the routing of a borrowed motion button, and the latch that keeps a
borrowed press off the motors until it is released.

Both paths are covered by `tests/test_control.c`, driven by button code and
elapsed time.
