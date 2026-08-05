# Chair firmware, enhancements specification

Normative spec for behavior this firmware adds **beyond** the factory chair.
[firmware-spec.md](firmware-spec.md) stays a description of the reference
firmware alone; anything specified here is absent from a `make reference` build.

**Process: update this spec before changing the code**, same rule as the
firmware spec. If behavior and spec disagree, one of them is a bug, decide
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
flash-reference` gets back to known-good factory behavior in one step, without
checking out an older commit.

Requirements on the flag itself:

- A build with `ENHANCED=0` **SHALL** behave exactly as
  [firmware-spec.md](firmware-spec.md) describes. No enhancement may change
  reference behavior, including timing and pin state, when compiled out.
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

Reference behavior: `MOTION_FLATTEN` drives until the button is released or the
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
  anything is energized, across `MOTION_SETTLE_MS`, the recline stagger, and the
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
- Applies to the recline axis and to `MOTION_FLATTEN`, and **SHALL NOT** apply
  to the headrest. A held recline button that reaches a stop ends the same way
  an unattended move does, rather than holding the relays closed against an open
  limit switch until the button comes up.
- The headrest is excluded because its motor draws too little to register on a
  channel scaled for the recline motor: it reads as zero for its whole travel,
  so every headrest move looked like it arrived a second in. It runs until
  release, as the reference does, and is bounded by `MOTION_TIMEOUT_MS`.
- Requiring current to have been *seen* before allowing an arrival is the
  obvious alternative and does not work: driving down is gravity-assisted and
  can draw little enough to read as zero the whole way, so the flatten macro
  would never end.
- Arrival is also where §2.7's position estimate is corrected, since a stop is
  the only place the chair knows where it actually is.
- `MOTION_TIMEOUT_MS` and stall detection are unchanged and remain the backstop
  if the current sense ever lies.

**Unproven, like stall detection:** the zero-at-the-stops reading is measured on
the chair and logged, but this is the first thing to make a control decision
from it. That one axis had to be excluded is the first evidence of its limits.

### 2.2 Double-tap POWER, `ENH_POWER_DOUBLE_TAP` **[done]**

Everything off and the chair flat, without holding anything down. Requires §2.1.

The factory reaches this by holding POWER; with §2.8 that hold is spent on
presets instead, so the double tap is the only way in. It is also the one
gesture that always means the same thing, whatever else is running.

- Two POWER presses within `BUTTON_DOUBLE_MS` (400 ms) **SHALL** drive
  `MOTION_FLATTEN` and then switch everything off: comfort off, gate off, vent.
- The move runs unattended, so the request **SHALL** be renewed until motion
  ends — at the stops per §2.1, or on the ceiling.
- Any button press **SHALL** cancel it. A cancel aborts where it is and does not
  run the shutdown.
- POWER **SHALL** blink at 500 ms, half on half off, for the duration, so an
  unattended move is visibly deliberate and not a stuck button. The gate is
  still on during the move, so without this the lamp would simply sit lit.
- Counted in `dbg.auto_moves`.

The first tap runs its ordinary power toggle before a second one is known to be
coming. This is deliberate: nothing has to be deferred, so a single press keeps
its usual immediate response, and the macro ends with everything off whichever
way that toggle went. The visible effect is that a double tap starts its vent at
the first tap rather than at the end.

### 2.3 Heat levels, `ENH_HEAT_LEVELS` **[done]**

Reference behavior: HEAT is a plain toggle and the element is driven fully on,
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
  untouched chair behaves as the factory one does. It persists across a reset
  per §2.4, and a stored value outside 1..`HEAT_LEVEL_MAX` **SHALL** be treated
  as absent, so a corrupt record degrades to a working chair.
- Switching on is refused while a motor is moving, unchanged from reference.

#### Choosing a level

- Holding HEAT for `BUTTON_ADJUST_HOLD_MS` (1250 ms) **SHALL** switch heat on if
  it was off, and **SHALL** hand the four motion buttons over to the levels for
  `ADJUST_ARM_MS` (8 s). Held HEAT is shorter than held POWER's 2 s on purpose:
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
  `ADJUST_REPICK_MS` (2 s), renewed by each further pick, so a level can be
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

- A tap that switches heat on **SHALL** raise the bar for `ADJUST_READOUT_MS` (2 s) as
  a **readout only**: the four buttons keep their motor function throughout, and
  any button other than HEAT **SHALL** take the bar down early. HEAT itself does
  not, because that press may be the start of a hold.
- A hold **SHALL** raise the same bar as a **menu**, for as long as the buttons
  are handed over. Pressing one of the things a menu is offering does not
  dismiss it.
- The bar **SHALL** animate in one lamp at a time from the bottom, at
  `ADJUST_BAR_STEP_MS` (80 ms) a lamp.
- The bar **SHALL** animate out by shifting the whole bar up one lamp per step,
  discarding whatever runs off the top, until nothing is lit. One rule for every
  level: a level four bar empties from the bottom, and a level one bar is a
  single dot that travels up and leaves.
- Whichever frame is the **last one with a lamp still lit** **SHALL** be held for
  `ADJUST_BAR_EXIT_MS` (200 ms) rather than a step. How long the top lamp stays
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

#### The owner's own lamp

- The lamp of whatever is being adjusted **SHALL** blink at `ADJUST_BLINK_MS`
  (250 ms) while the buttons are handed over, and be steady otherwise for as
  long as that function is on. Blinking therefore means "still changeable", and
  it stops the moment the buttons go back to the motors.

#### Duty cycling

- The element **SHALL** be switched on a `HEAT_DUTY_PERIOD_MS` (20 s) cycle at
  25% / 50% / 75% / 100% for levels 1 to 4, phased from the moment heat was
  switched on so the cycle always begins with the element energized.
- Level 4 is 100%, that is, continuous. The top level is exactly the reference
  behavior, so the enhancement only ever removes heat.
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

### 2.4 Persistent settings, `ENH_SETTINGS_PERSIST` **[done]**

Reference behavior: nothing is remembered. Every reset starts from the
compiled-in defaults, which is what the factory firmware does — the flash region
that looked like a settings area turned out not to be one, see
[firmware-map.md](firmware-map.md#next-steps).

There is no EEPROM on this part, and the backup registers need a VBAT the board
does not provide, so main flash is the only option. It is used as an
**append-only log in one reserved page**, not as a fixed location that gets
rewritten.

- A page is **1 KiB**, the FMC's erase granularity. Measured on the chair: after
  erasing `0x08000000`, programming `0x08000400` sets PGERR.
- One page at `0x0800F000` **SHALL** be the store and **SHALL NOT** be used by
  the linker for anything else. The linker script reserves the last 4 KiB rather
  than the last 1 KiB, so the store can grow later without moving code.
- A record **SHALL** be 32 bytes: 31 payload bytes and a check byte, the check
  being the complement of their sum. An erased record is all `0xFF`, whose
  payload sums to a complement of `0x1E`, which is not the `0xFF` in the check
  position — so an erased record is invalid by construction and needs no
  separate in-use marker.
- The record **SHALL** hold, in order: the three loose levels (heat, lumbar,
  massage), then the four preset slots of §2.8 at six bytes each, then a bitmap
  of which slots have ever been written. 28 bytes used of 32.
- Records **SHALL** be appended to the first erased record. The live values are
  the **highest-indexed valid record**, which works because writes only ever
  move forward.
- A record that is not erased but fails its check **SHALL** be stepped over
  rather than read or written into. That is what a power loss part way through a
  multi-word write leaves behind, and the words are written in order so the
  check lands last: a torn record can never read as valid.
- When the page is full it **SHALL** be erased and writing **SHALL** restart at
  record zero.
- A record **SHALL** be written only when something actually differs from what
  is already committed. Choosing a level and then changing back writes nothing.

Everything shares one record, so any change rewrites all of it. That is what
keeps the scan simple — one record is the whole state, and the newest valid one
wins outright.

#### The slot-written bitmap

A preset of "flat, everything off" is legitimate and is all zeros, which is also
what an untouched slot reads as. Without a separate mark there is no way to tell
them apart, and recalling a button nobody has ever saved to would drive the
chair flat and switch everything off — a destructive surprise from a button that
has never been given a meaning. The bitmap is what lets §2.8 refuse it.

#### Wear

Not a consideration, and the arithmetic is worth writing down so nobody has to
redo it. The datasheet gives **100 kcycles** endurance and 25 years retention.
At 32 records per erase that is on the order of 3 × 10⁶ saves. A chair whose
settings changed every hour of every day would take roughly three centuries.

#### When it is safe to write

The core executes from flash, so a program or erase stalls instruction fetch:
the main loop stops for the duration, and every output stays latched exactly
where it was. Nothing can be stopped while it is stalled.

The datasheet's worst figure in the flash timing table is **42 ms**. Against the
margins this firmware already relies on — `MOTION_STALL_MS` of 4 s,
`HANDSET_TIMEOUT_MS` of 250 ms, a 2 s watchdog — that is comfortably inside all
of them, so no gate is strictly required.

- A commit **SHALL** nonetheless be deferred while a motion is running. It costs
  nothing: there are 128 saves of slack before an erase is due, so waiting for a
  quiet moment buys margin that then does not have to be reasoned about.
- A commit **SHALL** also be deferred while a level is being chosen, so one
  adjustment writes one record rather than one per press.
- A failed write **SHALL NOT** be retried. The values stay live in RAM and the
  chair works normally; retrying every loop would hammer a page that has already
  said no. Counted in `dbg.settings_errors`.

#### Reading it back

Every stored value is validated by whoever uses it, not by the store. A value
outside its own range **SHALL** be treated as absent and fall back to that
function's default, so a corrupt record, a half-written one, or one left by an
older firmware with a different layout all degrade to a working chair rather
than a refusing one.

That last case is routine rather than hypothetical: changing the record layout
invalidates everything already on the chair, and the chair comes up on defaults
once after such a change.

Writes counted in `dbg.settings_writes`, erases in `dbg.settings_erases`.

### 2.5 Lumbar hold-to-set, `ENH_LUMBAR_HOLD_SET` **[done]**

Reference behavior: LUMBAR is a four-state cycle, inflate → hold → deflate →
off, one press each, per [firmware-spec.md](firmware-spec.md#lumbar-done) §8.
Getting a particular firmness means pressing once, waiting, and pressing again
at the right moment, and there is no way to ask for the same firmness twice.

This replaces the cycle with a set-and-recall: hold to inflate to where you want
it, and from then on a tap goes straight back there.

There is no position or pressure sensor, so a "level" is only **how long the
bottom cell was inflated for**. That is enough to be repeatable, because the
same duration through the same pump and valve gives the same firmness. It is not
a measurement of anything, and it will not track a slow leak.

#### Setting a level

- Pressing LUMBAR while it is off or deflating **SHALL** begin inflating
  immediately, on the press edge, not on a hold threshold. The response to
  pressing the button is the pump starting.
- Releasing after `LUMBAR_SET_MS` (500 ms) or more **SHALL** stop the pump,
  hold, and store the elapsed inflate time as the level.
- Inflating **SHALL** stop at `LUMBAR_INFLATE_MAX_MS` however long the button is
  held, and that ceiling **SHALL** be stored as the level. It is the existing
  bound from the reference firmware and is unchanged.
- Elapsed time **SHALL** be measured from the valve opening, not from the pump
  starting. `PUMP_DELAY_MS` of it is therefore dead time — but the same dead
  time occurs on replay, so the two cancel and the stored figure stays faithful.
- Time spent paused by a motion **SHALL NOT** count, the same way the reference
  lumbar deadline already excludes it.

#### Recalling it

- Releasing before `LUMBAR_SET_MS` is a tap, and **SHALL** continue inflating to
  the stored level and then hold. The pump does not stop and restart; the tap
  simply means "keep going until the usual place" instead of "stop here".
- A tap with no level stored **SHALL** inflate to `LUMBAR_DEFAULT_MS`, half the
  ceiling. Not the ceiling itself: a first press should not put a stranger's
  back through the firmest setting the chair has, and finding it too soft costs
  one more press where finding it too hard costs a wait for the exhaust.
- If the stored level has already been passed by the time the button is
  released, it **SHALL** hold immediately rather than deflating back to it.
  There is no way down but the exhaust, and emptying the cell to reach a target
  the user has just overshot by a fraction of a second is worse than the
  overshoot.

#### Adding to a level

- Pressing **SHALL** always start the pump, whether the cell is empty or already
  up. What the press meant is decided on release, never on the press edge.
- Holding on a cell that already has air in it **SHALL** add to it, and the new
  level **SHALL** be the total — what was already there plus what this press
  put in. Firming up a cell that is nearly right should not mean emptying it and
  starting over.
- The ceiling **SHALL** be counted from the total, so holding a part-inflated
  cell cannot push it past `LUMBAR_INFLATE_MAX_MS`.

#### What is in the cell versus what is remembered

These are two different things and the distinction is load-bearing.

- The firmware **SHALL** track how much air is actually in the cell, separately
  from the remembered firmness. Adding to a level counts from what is in the
  cell, which is not always what is remembered.
- Only **holding the button** to a firmness **SHALL** update the remembered
  level. Arriving somewhere because a preset asked for it, or because nothing
  was stored and the default applied, is not a preference the user expressed.
- Without that split the first-use default would be written the first time it
  was used and would stop being a default. `LUMBAR_INFLATE_MAX_MS` would then be
  unreachable by any recall, since every path to the cell being full would go
  through the default first.

#### Switching off

- A **tap** on a cell that already has air in it **SHALL** deflate, over the
  same `VENT_MS` as the reference. There is nowhere else for a short press to
  go: the level is already what it is, and asking for it again is a no-op.
- Off therefore acts on **release**, not on the press edge, which is what leaves
  the hold free to mean "more".
- Pressing while deflating **SHALL** start inflating again. The user has already
  asked for off; a further press can only mean they changed their mind.

#### Persistence

The level **SHALL** be stored per §2.4, quantised to 100 ms units so it fits a
byte, giving 0 to `LUMBAR_INFLATE_MAX_MS` in 200 steps. Zero means unset.

Reported as `dbg.lumbar_level`, in the same 100 ms units.

A bar graph for the level is intended and not specified here.

### 2.6 Massage intensity, `ENH_MASSAGE_LEVELS` **[done]**

Reference behavior: one intensity, the only one the factory offers, and it is
the strongest the hardware can do. There is no way to ask for less.

Four intensities, chosen exactly the way heat levels are — hold MASSAGE, pick on
the borrowed motion buttons, same bar, same adjuster (§2.3). Only one thing owns
the adjuster at a time, so opening massage's takes it from heat's.

#### What intensity changes

- The pattern **SHALL** keep its shape and its order. Every step happens, in the
  same sequence, on the same cells.
- An inflating step **SHALL** be made **shorter**, not gated. The cell is open
  for less time, so less air goes in, and the sequencer moves on sooner. A
  gentler massage is therefore also a **quicker** one.
- It **SHALL NOT** hold a step open past the point it has finished inflating.
  The massage has to keep moving; a shortened step is a shorter note, not the
  same note followed by a rest.
- Rests and vents **SHALL** keep their full duration. They are what lets a cell
  bleed down between pulses, and shortening them would carry pressure from one
  pulse into the next, working against the intensity being asked for.
- Level 4 **SHALL** run every step at 100%, so the top level is byte-for-byte
  the reference pattern.

#### The curve

A flat multiplier is wrong, and this is the part worth stating rather than
leaving to a constant. The pattern's steps run from 5 to 80 ticks. A quarter of
a 5-tick step is half a tick, which nobody would feel, and short pulses are most
of the second and third movements — so a linear scale would not make the massage
gentler, it would delete two thirds of it.

- The scale **SHALL** therefore be interpolated by step length: the long
  inflations take the full reduction, the short pulses much less. The percentage
  is of the step's own duration.

| Level | Longest steps | Shortest steps |
|-------|---------------|----------------|
| 1     | 25%           | 60%            |
| 2     | 50%           | 72%            |
| 3     | 75%           | 85%            |
| 4     | 100%          | 100%           |

- No inflating step **SHALL** be shorter than `MASSAGE_STEP_FLOOR_MS`, which is
  the shortest step the factory pattern itself uses (500 ms). This is a hard
  floor rather than a preference: after an all-closed step the pump takes
  `PUMP_DELAY_MS` (300 ms) to come up, so a step much below it puts in nothing
  at all, and a reduced level must not quietly become no level.
- The numbers above are tuning, not architecture. They live in one table at the
  top of `pneumatics.c` and are expected to move after time on the chair.

#### Behavior

- Tapping MASSAGE **SHALL** toggle it, at the remembered intensity, and put the
  bar up as a readout. Like HEAT, it acts on release so the hold can mean
  something else.
- Holding MASSAGE **SHALL** switch it on if it was off, and open the adjuster on
  its intensity.
- Intensity **SHALL** persist per §2.4, and **SHALL** be re-read when massage
  starts, not held across a shutdown.
- Starting massage while a motor moves is refused, unchanged.

Reported as `dbg.massage_level`.

### 2.7 Position tracking, `ENH_POSITION_TRACK` **[done]**

Reference behavior: the chair has no idea where it is. Motion is a duration and
never a target, per [motion.h](../src/motion.h).

There is no encoder, no potentiometer, and no feedback of any kind except the
current-zero at the stops. So position is **dead reckoning**: an axis's position
is how many milliseconds of upward travel it is above its down stop, integrated
from how long its motor ran.

- Travel **SHALL** be counted only while the relays are closed, not from the
  request, since `MOTION_SETTLE_MS` is relays settling with nothing turning.
- Each axis **SHALL** clamp between zero and its stop-to-stop travel time,
  `MOTION_TRAVEL_RECLINE_MS` and `MOTION_TRAVEL_HEADREST_MS`. **Both are
  measured figures and everything scales off them.**
- Reaching a stop **SHALL** replace the estimate rather than adjust it: the axis
  is at a known end of its travel, whatever the arithmetic had accumulated. That
  is the only correction available, and §2.1 is where it happens.
- The clamp is a second, weaker correction. Driving into a stop and holding
  there converges the estimate either way — high estimates count down to zero
  while the chair sits against the stop, low ones reach zero early and stay. It
  works without any current sensing, which is why the headrest is still usable
  despite being excluded from §2.1.

#### What it cannot do

The error is not symmetric. Gravity and body weight assist one direction and
oppose the other, so a second down and a second up are not the same distance,
and there is nothing to correct that between stops. Short moves in the middle of
travel accumulate error with nothing pulling them back.

Current sensing could do better — speed is roughly `(V − I·R) / k`, so
integrating an estimated speed would capture load — but it needs motor constants
this project does not have and a supply-voltage measurement this board does not
provide. Time integration is accepted as good enough, with the stops as the
correction.

Reported as `dbg.pos_recline` and `dbg.pos_headrest`, in milliseconds.

### 2.8 Presets, `ENH_PRESET` **[done]**

Four saved chairs, one per motion button. Requires §2.7 for position and §2.1 so
an unattended move can end itself.

A preset is both axis positions, which comfort functions were running, and the
level each was running at. The four motion buttons are the four slots, in the
same bottom-to-top order the bar graph uses: headrest ▼, headrest ▲, recline ▼,
recline ▲.

#### The gestures

None of them takes a button away from its usual job.

- Holding POWER for `BUTTON_HOLD_MS` **SHALL** arm for `PRESET_ARM_MS` (8 s).
  This replaces the factory's hold-to-flatten, which §2.2 now covers.
- While armed, pressing a motion button **SHALL** write this chair into that
  slot. The press **SHALL NOT** reach the motors, and **SHALL** be suppressed
  until that button is released, for the same reason as a level pick in §2.3.
- Double-tapping a motion button **SHALL** recall that slot. A single press and
  a hold both still drive the motor: at a tap's timescale `MOTION_SETTLE_MS` has
  not elapsed, so nothing has moved by the time the second tap arrives.
- A recall **SHALL NOT** be behind the POWER gate, and **SHALL** switch the gate
  on as part of restoring. Recalling a preset is how you start using the chair;
  requiring it to already be on would be backwards.
- Recalling a slot that has never been written **SHALL** do nothing. See §2.4.

#### Feedback

Saving moves nothing, so the lamps are the only evidence anything happened.

- While armed, POWER **SHALL** blink at `PRESET_ARM_BLINK_MS` (500 ms), and the
  four motion lamps **SHALL** show which slots are taken: **lit** for a slot
  that holds something, **blinking** for one that is free. Without it the window
  is four dark buttons and no clue which of them means anything.
- On a save, POWER **SHALL** stop and the slot that took it **SHALL** flash
  `PRESET_SAVE_FLASHES` times at `PRESET_SAVE_FLASH_MS`, then hand the lamp
  back to whatever it was showing. The gesture is an event, not a mode.
- While a recall is moving, POWER **SHALL** blink at 500 ms, as §2.2 does, for
  the same reason: an unattended move must not look like a stuck button.

#### Recall

- The axes **SHALL** be driven **one at a time**, recline first. Both share the
  current sense channel, so running them together makes an arrival ambiguous —
  zero would mean "something stopped", not "this one did". Recline goes first
  because it is the big move and the headrest reads as an adjustment to it.
- A seek **SHALL** end when the estimate is within `MOTION_SEEK_MS` of the
  target, and not only when the motor runs out of travel. Chasing a tighter
  figure than that just chatters the relays against an estimate that was never
  that accurate.
- Any button press **SHALL** cancel, matching §2.2. A cancel stops where it is
  and restores nothing further.
- The comfort functions **SHALL** be restored only once the chair has stopped
  moving, since starting the heater or the pump is refused while a motor runs
  and would otherwise be silently dropped.

#### Levels, and what a recall is allowed to change

A slot carries its own levels. The levels in §2.4's loose bytes are a separate
thing: the **last-used memory**, which is what an ordinary press falls back to.

- A recall **SHALL** apply the slot's levels to the running state and **SHALL
  NOT** write them to the last-used memory.
- A manual adjustment **SHALL** write to the last-used memory and **SHALL NOT**
  reach any slot.
- Only an explicit preset write **SHALL** change a slot.

So the two never contaminate each other: recalling a soft preset does not lose
the firm setting you last chose by hand, and adjusting by hand does not silently
rewrite a preset you set up weeks ago.

This is why the modules expose both "run at this level" and "run at this level
and remember it" — the difference between the two is exactly the difference
between a recall and a press.

Counted in `dbg.presets_saved` and `dbg.presets_recalled`.

### Where this lives

The macros are in [../src/macros/](../src/macros/), one file per macro:
`flatten.c` for §2.2 and `preset.c` for §2.8. Trigger detection is `button.c`,
which turns button codes into press, tap, double-tap, hold and hold-release
events and knows nothing about what any of them mean; `control.c` routes those
events.

`button.c` knows two buttons apart, in `hold_ms_for()`, because the holds that
open a level adjuster are shorter than POWER's. That is the whole of the
special-casing; the alternative was threading a threshold in from `control.c`,
which would put timing back in the caller the module exists to take it out of.

#### Gestures and state

Every module here has two layers, and the split is what makes presets possible.

The **button handlers** are gestures: they toggle, they cycle, they depend on
what the last press did. `heat_press()`, `pneumatics_button()`. They are what a
finger talks to.

Underneath them are **state setters**: `heat_set()`, `pneumatics_massage_set()`,
`pneumatics_lumbar_set()`, `motion_request()`. They say what should be true, are
idempotent, and do not care what happened before.

A preset recall has a chair it wants to arrive at, not a finger. It uses the
setters. Miming presses instead means guarding every call with "is it already
on?", and getting the answer wrong turns a restore into a shutdown — a toggle
called on something already running switches it off. `motion.c` had this shape
from the start, because the flatten macro needed it; the others grew it when
presets arrived.

The same split runs through the levels, as §2.8 describes: "run at this" and
"run at this and remember it" are separate calls because a recall and a press
mean different things by the same number.

The adjuster is [../src/adjust.c](../src/adjust.c): the bar animation, the
window, and which function currently owns the borrowed buttons. It knows how
many lamps to light and nothing about what a level means, and publishes the bar
as a lamp mask rather than a level, because a bar sliding off the top is not a
level.

Its two users own only what a level means to them — `heat.c` the duty cycle,
`pneumatics.c` the pump gating. Neither knows the other exists; they meet at the
adjuster, which has one owner at a time because there is one set of motion
buttons and one bar.

`control.c` owns the mapping from mask to actual lamps, the routing of a
borrowed motion button to whoever asked for it, and the latch that keeps a
borrowed press off the motors until it is released. The four motion buttons are
borrowed by two different things — level picks in §2.3 and preset slots in §2.8
— and `control.c` is where they are told apart, since only one window is ever
open at a time.

#### The motion pause

`pneumatics.c` stops everything while a motor runs, and gives the elapsed time
back afterwards so a pattern resumes where it left off rather than jumping
ahead. Only deadlines that were **already running when the pause began** get
that time back.

Both halves of that matter. A vent and a lumbar deflate run right through a
pause, so crediting them would hold the exhaust open for the length of the
motion. And anything *started during* the pause never waited at all — a preset
recall sets lumbar going in the same pass that ends its own motion, so the
unwind lands the pass after. Crediting it the whole move pushes its deadline
into the future, the subtraction underflows, and the cell jumps straight to hold
without inflating: lit, but empty.

Covered by `tests/test_control.c`, driven by button code and elapsed time.
