# Chair firmware, specification

Normative spec for the replacement firmware in `../src`. Describes what the
firmware **shall** does, as currently built and verified.

**Process: update this spec before changing the code.** If behaviour and spec
disagree, one of them is a bug, decide which, then fix it.

Background on how the factory firmware works is in
[firmware-map.md](firmware-map.md); the hardware and toolchain are in
[custom-firmware.md](custom-firmware.md).

Status key: **[done]** implemented and verified on the chair ·
**[partial]** implemented, not fully verified · **[planned]** not implemented.

---

## 1. Scope

Replacement firmware for the SoCozi recliner control board (GD32E23x,
Cortex-M23, 64 KiB flash, board silkscreen `PT613A`).

Goals, in priority order:

1. Never move an actuator unsafely.
2. Match the factory firmware's user-facing behaviour from the wired handset.
3. Expose control and state to an external network device.
4. Remain debuggable over SWD at all times.

## 2. Platform requirements

- **SHALL** run on the GD32E23x at its reset default clock (8 MHz IRC), with no
  PLL configuration. All timing derives from a 1 kHz SysTick.
- Two builds from one source tree, each with `.elf` and `.map` alongside the
  `.bin`: `build/socozi.bin`, the default, which adds the behaviour in
  [enhancements-spec.md](enhancements-spec.md), and
  `build/socozi-reference.bin` from `make reference`, which is this spec and
  nothing more. Everything below applies to both unless it says otherwise.

  There is no separate watchdog-less variant: `make debug` freezes the watchdog
  while the core is halted, so the shipping image is also the one we develop
  against.

- The build **SHALL** enable the free watchdog, ~2 s timeout, kicked
  once per main-loop pass. Every other safety bound here, motion timeout,
  stall detection, heater auto-off, is enforced by the same loop that would be
  stuck if the firmware hung. The watchdog is the only cover for that case, and
  a hang with the heater or a motor energised is precisely the failure worth
  covering.

  Verified on the chair: `RCU_RSTSCK` bit 29 (FWDGTRSTF) sets after a halt, and
  the firmware restarts.

- `DBG_CTL` bit 8 (`FWDGT_HOLD`) at `0x40015804` freezes the watchdog while the
  core is halted, so a release build **can** be debugged.

  **It must be set from the debugger, not from firmware.** The same write from
  the core never takes, the release build set it every boot and it always read
  back 0, while a write over the debug port sticks immediately. The register
  appears to live in the debug power domain: it survives a system reset, and
  writing 0 does not clear it.

  It is **set-once**: writing 0 does not clear it, and neither does a system
  reset, only a power cycle. So it is applied by `make debug` alone, never by
  `flash`, `backup` or `restore`. A chair that has just been flashed should not
  be left with debug behaviour latched on.

  This also explains the factory firmware's behaviour. It never references that
  register at all, so its watchdog always resets on halt.
- **SHALL** keep SWD (PA13/PA14) functional and never repurpose those pins.
- **SHALL** be recoverable by reflashing `../factory-firmware.bin`.

## 3. Pin assignment

Normative. Anything not listed **SHALL** be left unconfigured.

| Pin                 | Direction   | Function                                                   |
|---------------------|-------------|------------------------------------------------------------|
| PA2                 | out         | valve shift register, DATA                                 |
| PA3                 | out         | valve shift register, CLOCK                                |
| PA4                 | out         | valve shift register, LATCH                                |
| PA5                 | out         | recline, ▼ pair (with PB10)                                |
| PA6                 | out         | recline, ▲ pair (with PB2)                                 |
| PA7                 | analog      | ADC channel 7                                              |
| PA9                 | AF1         | USART0 TX → handset                                        |
| PA10                | AF1         | USART0 RX ← handset                                        |
| PB0                 | analog      | ADC channel 8                                              |
| PB1                 | analog      | ADC channel 9                                              |
| PB2                 | out         | recline, ▲ pair (with PA6)                                 |
| PB8                 | in, float   | unidentified                                               |
| PB9                 | in, float   | unidentified                                               |
| PB10                | out         | recline, ▼ pair (with PA5)                                 |
| PB11                | out         | third motion axis, direction (not connected on this chair) |
| PB12                | in, pull-up | unidentified                                               |
| PB13                | out         | headrest, direction (0 = up, 1 = down)                     |
| PB14                | out         | third motion axis, enable (not connected on this chair)    |
| PB15                | out         | headrest, enable                                           |
| PC13                | out         | pump enable                                                |
| PC14                | out         | heater enable (inferred, **untested**)                     |
| PA0, PA1, PA8, PA15 | out         | unidentified, driven low                                   |

Free: PA11, PA12, PB3–PB7, PC15, PF0, PF1. PB6/PB7 are USART0's remap pins and
are the intended landing spot for a network module.

## 4. Safety requirements

Layered defences. Stall detection (§9) is the primary one; the rest hold even
if it fails or is mis-tuned. None are optional.

- **SHALL** pass through a full stop when changing direction on any axis, so two
  directions are never energised simultaneously, even transiently. **[done]**
- **SHALL** stop all motion if no valid handset frame is received for
  **250 ms**. A disconnected or dead handset must not leave a motor running.
  **[done]**
- **SHALL** stop any single continuous motion after **30 s**. Full travel is a
  few seconds; this is a backstop, not a normal limit. **[done]**
- **SHALL NOT** run the heater without a time bound. **[done]**, 60 min, §8.
  There is no temperature sensor, so that timer is the only bound.
- **SHALL** stop a motion whose current indicates a locked rotor. **[done]**, §9.
- The debug interface **SHALL** be observation only. Nothing written into the
  debug block may drive an output or alter behaviour. **[done]**

  An earlier version accepted commands there that could hold any pin at any
  level indefinitely, bypassing stall detection, the motion timeout and the
  power budget. It was useful for bench work and removed once the pin map was
  established, because anything with SWD access could otherwise drive a motor
  into a hard stop.

## 5. Power budget

The factory firmware never runs a high-current load while a motor is moving.
Three loads are excluded this way, **pump, heater and massage**, which reads
as a supply-current budget: the board cannot power a motor and a heater or pump
at once.

This is treated as a **hardware constraint, not a preference.** It applies to
network-initiated actions exactly as it does to handset ones.

Exclusion works at two levels, and both **SHALL** be reproduced:

1. **The button is refused.** Pressing MASSAGE, HEAT or LUMBAR while a motor
   runs does nothing, the handler declines and stops the moving axes instead.
   Observable on the chair: the heat button will not toggle mid-motion.
   **[done]**
2. **The output is cut.** For a function already running, its output is forced
   off for as long as motion continues. The enable flag stays set, so it resumes
   by itself once motion stops, it **pauses, it does not cancel**.
   `FUN_08000FA0` does this for heat; the massage engine does it for pump and
   valves. **[done]**

State is retained across a pause, so a massage pattern resumes where it left off
rather than restarting. Three requirements follow, all **[done]**:

- **Exhausting is not paused.** A pause exists to keep the pump off the supply
  while a motor draws from it, and the exhaust drives no pump, so anything that
  is only venting **SHALL** keep running for the whole motion: both the 120 s
  shutdown vent and `LUMBAR_DEFLATE`. Confirmed against the factory firmware on
  the chair, which carries on deflating when a motion button is pressed
  mid-vent.

  A motion **SHALL NOT** open the exhaust by itself. Only a vent already in
  progress continues; a motion never starts one.

  Ownership follows the same priority as the unpaused path, massage, then
  lumbar, then vent, so a stale vent cannot take the valves back from whatever
  is actually running. An earlier build got this wrong twice: first by closing
  the exhaust on any motion, which trapped the air on every shutdown macro, and
  then by leaving a vent flagged after massage took over, so the next motion
  button reopened the exhaust.

- Elapsed time during a pause **SHALL** be given back to the deadlines that
  actually stopped (massage step, massage auto-off, lumbar inflate and hold) when
  it ends, so the pattern does not jump ahead. The vent and lumbar deflate
  deadlines **SHALL NOT** move, having run throughout; extending them would hold
  the exhaust open for the length of the motion.

  The factory gets the paused half free, because its whole timebase is gated by
  the same motion check, so its counters simply stop.
- The massage engine **SHALL** rewrite the current step's valve bits on every
  pass, not only at a step transition. A pause drives the valves to zero, and
  waiting for the next transition to restore them would leave them shut for up
  to 8 s afterwards. The factory engine likewise rewrites its outputs every
  tick.

Motion itself is never refused, motors always win.

## 6. Handset

Wired remote on USART0, **9600 8N1**.

- The handset **does not transmit unsolicited**. The firmware **SHALL** poll it
  and parse the reply. **[done]**
- Poll rate **SHALL** be 50 Hz (every 20 ms). **[done]**
- Poll frame is 6 bytes: `[type] [0x04] [p0] [p1] [p2] [checksum]`, where `p0` is
  the LED bitmap and the checksum is the sum of the preceding five bytes mod
  256. **[done]**, `p1`/`p2` are sent as zero; their meaning is unknown.
- Reply frame is 4 bytes: `[type] [0x04] [button] [checksum]`. `type` is `0x03`
  when idle and `0x06` when a button is down. **[done]**
- Frames failing the checksum or field checks **SHALL** be discarded and the
  parser reset. **[done]**

### USART error flags **[done]**

The firmware **SHALL** clear the USART error flags (`PERR`, `FERR`, `NERR`,
`ORERR`) on every poll, and **SHALL** reset the frame parser when any of them
was set.

This is not housekeeping, it is the difference between a working chair and a
dead one. This is the newer USART block: `STAT` sits at offset `0x1C` and the
flags are cleared **only** by writing to `INTC` at `0x20`. Reading `RDATA` does
not clear them, unlike the older peripheral.

While `ORERR` is set the receiver discards everything and `RBNE` never sets
again. The handset link is then dead permanently. Worse, the main loop stays
healthy and keeps kicking the watchdog, so nothing recovers it. The chair sits
there ignoring every button until it is reset over SWD.

Observed on the chair: found unresponsive after being left running, recovered
by `make reset` alone. Two plausible triggers, and one occurrence of either is
enough:

- Motor and relay noise on the unshielded 4 conductor handset cable, setting
  `FERR` or `NERR`. The same noise is bad enough to drop the SWD link.
- Overrun. The USART has a one byte buffer and no FIFO, and `tx_byte` will
  block up to 5 ms waiting for `TBE`.

`dbg.hs_errors` counts them and `dbg.hs_last_error` holds the flags from the
most recent one, so which trigger it is can be settled from data.

### Button codes

Sequential, in the physical order the buttons appear on the handset.

| Code   | Button      | Action                                          |
|--------|-------------|-------------------------------------------------|
| `0x00` | none / idle | stop motion **[done]**                          |
| `0x01` | POWER       | toggle comfort functions on/off **[done]**      |
| `0x02` | MASSAGE     | toggle massage pattern **[done]**               |
| `0x03` | HEAT        | toggle heater, 60 min auto-off **[done]**       |
| `0x04` | LUMBAR      | cycle inflate / hold / deflate / off **[done]** |
| `0x05` | RECLINE ▲   | drive while held **[done]**                     |
| `0x06` | RECLINE ▼   | drive while held **[done]**                     |
| `0x07` | HEADREST ▲  | drive while held **[done]**                     |
| `0x08` | HEADREST ▼  | drive while held **[done]**                     |

Note the handset's ▲ reclines the seat back **down**. Directions are specified
by button, never by physical motion.

Code `0x03` is inferred from the sequence rather than observed directly. All
others are confirmed by the chair responding to them.

Pneumatic functions act on the **press edge**, not per frame. Motion acts
continuously while held.

### LEDs **[partial]**

The outbound payload byte `p0` is an 8-bit bitmap, **one bit per button in the
same order as the button codes**:

| Bit | LED        | Driven                                              |
|-----|------------|-----------------------------------------------------|
| 0   | POWER      | **[done]**, lit while comfort functions are enabled |
| 1   | MASSAGE    | **[done]**, lit while the pattern runs              |
| 2   | HEAT       | **[done]**, lit while the heater is enabled         |
| 3   | LUMBAR     | **[done]**, lit during inflate and hold             |
| 4   | RECLINE ▲  | **[done]**                                          |
| 5   | RECLINE ▼  | **[done]**                                          |
| 6   | HEADREST ▲ | **[done]**                                          |
| 7   | HEADREST ▼ | **[done]**                                          |

Derived from `FUN_08001810` in the factory image, where bits 4–5 come from the
recline axis state and 6–7 from the headrest axis, which is what pins the
ordering down.

- Motion LEDs **SHALL** reflect the motion actually being driven, not the button
  pressed. If a safety stop cuts a motion, the LED goes out even with the button
  still held. **[done]**
- The factory firmware **blinks** bit 7 under some condition (gated on a counter
  below `0x1F`). Not reproduced, and the condition is not understood.
- LEDs are a general-purpose output, not only a button echo. They are intended
  to indicate state such as network activity or fault conditions later.

### Debounce

The factory firmware requires several consecutive frames before acting; this
firmware acts on a single frame, which is why it responds faster.

**SHOULD** require **two consecutive agreeing frames** before starting a motion.
The checksum is only a sum and is weak, and the cable runs alongside motor
wiring. Cost is ~20 ms. **[planned]**

## 7. Motion control

Three axes exist in hardware. Only two are connected on this chair.

| Axis     | Pins                        | Assert                                       |
|----------|-----------------------------|----------------------------------------------|
| Recline  | PA6 + PB2 / PA5 + PB10      | **both** pins of a pair; neither works alone |
| Headrest | PB13 direction, PB15 enable | enable high, direction selects               |
| Third    | PB11 direction, PB14 enable | not connected on this chair                  |

- Motion **SHALL** run only while its button is held, and stop on release.
  **[done]**

### Relay sequencing **[done]**

The factory firmware never energises a motor at the instant it selects a
direction. Per axis it keeps a tick counter that resets on any button change,
then:

| Counter | Action                                  |
|---------|-----------------------------------------|
| < 2     | set the direction pin, enable **off**   |
| 2–5     | dead time                               |
| 6       | close the first contact                 |
| 7       | close the second contact (recline only) |

At 100 ms per tick that is a **600 ms** delay before the motor turns, with the
recline pair staggered a further 100 ms.

The delay is not debounce, or not only debounce: it guarantees the direction
relay has settled before current flows, so polarity never switches under load.
That is what stops relay contacts arcing themselves shut.

- Direction **SHALL** be selected before any enable closes. This ordering is
  the requirement; the duration is a tunable.
- `MOTION_SETTLE_MS` (600) and `MOTION_STAGGER_MS` (100) live in `motion.h`.
  They may be shortened once the relays are measured, 600 ms is a long time to
  wait for a chair to move, and it is why the factory feels sluggish.
- Any change of requested motion, including to none, **SHALL** drop all
  outputs first and restart the sequence. **[done]**
- The handset LED **SHALL** light on the press, not on engage, so the chair
  acknowledges the button immediately even though it hasn't moved yet. **[done]**
- The firmware **SHALL** track exactly one active motion at a time. **[done]**
- There is **no position feedback**. Position **SHALL NOT** be reported as
  absolute. Commands are durations, not targets. **[done, by omission]**

## 8. Pneumatics

Three full-width bladders stacked vertically in the seat back, one pump, one
shared exhaust. Valve bits are clocked out MSB-first on the PA2/PA3/PA4 shift
register and latched.

| Bit    | Function         |
|--------|------------------|
| `0x01` | top bladder      |
| `0x02` | middle bladder   |
| `0x04` | exhaust (shared) |
| `0x08` | bottom bladder   |

Verified behaviour:

- A cell inflates with its bit set and the pump (PC13) running.
- Closing a cell's valve **traps** its air. Cells do not self-vent.
- A cell vents only with **its own bit and `0x04`** both set.

Requirements:

- The pump **SHALL** run whenever any **cell** bit (`0x01`, `0x02`, `0x08`) is
  set, ignoring the exhaust bit. This matches the factory firmware, which tests
  those three bits only, so it will pump against an open exhaust if a cell is
  also open. **[done]**
- The pump **SHALL NOT** start until **300 ms** have passed since the last
  moment every cell bit was clear. `PUMP_DELAY_MS` in `pneumatics.c`.
  **[done]**

  The factory keeps a counter (`0x08000f0c`) incremented once per 100 ms tick
  by the massage engine and cleared **only** on the all-cells-closed branch;
  the pump runs once it exceeds 2. So the delay is measured from the last fully
  closed state, and the pump keeps running straight through valve changes that
  leave any cell open.

  This distinction matters. An earlier build restarted the delay on every valve
  change, which starved the short pulse steps of air and made the pattern feel
  full of pauses.
- The exhaust **SHALL** be held open for **120 s** when shutting a function
  down, matching the factory firmware. Its idle branch holds the vent while a
  counter is below 120, and that counter is incremented by action `0xF5`, which
  fires **once per second**, not once per 100 ms tick.

  Verified by address, not inferred: the SysTick secondary counter and the
  `0xF5` test are the same variable (`0x20001A02`), and the vent condition reads
  `0x20001EA0`. Confirmed audibly, the valve release two minutes later is very
  faint but real.

  Anything shorter does not fully empty the bladders.

  This applies to **all three** shutdown paths, massage off, lumbar off and
  power off. In the factory firmware they are not separate routines: whichever
  function stops, the massage engine falls into one shared idle branch that does
  the venting. Verified by address, lumbar's flags (`0x20001AF2`, `0x20001AF1`)
  are the same two variables that branch tests. The counter resets while any
  function is active, so the 120 s starts when the last one stops.
- Venting **SHALL** open the exhaust **alone**, with no cell bit. The factory
  idle/deflate branch sets `0x04` by itself, and writing `0x04` alone is
  confirmed to deflate on the chair.

  Setting a cell bit alongside the exhaust turns the pump on by the rule above,
  so a "deflate" step built that way inflates instead. This bit us once.

### Massage **[done]**

MASSAGE toggles on and off. While on, the firmware runs the **OEM pattern**
transcribed from the factory table at `0x08004A1B`, all **39 steps**, up to the
`0xAA` sentinel at `0x08004B2C`. One cycle is 1185 ticks, just under **2
minutes**, and it loops.

Three movements:

1. **Wave down the back** (steps 1–13), cells fill top to bottom, then bleed
   from the top. `01 03 0A 07 05 05 03 0A 07 05 04 01 03`, mostly 40 ticks each.
2. **Middle cell pulsed** (14–26), `0x03` alternating with all-closed, holding
   longer each time: 10, 10, 10, 20, 40, 80 ticks.
3. **Bottom, then middle+bottom against the vent** (27–39), `0x08` then `0x0A`
   alternating with `0x04`, ramping the same way.

Switching it off opens the exhaust for the standard vent period.

An earlier version of this spec listed only the first 16 steps. That was a
truncated read of the table, and the firmware ran a 55-second cycle as a result.
The lumbar table at `0x080049DC` was re-checked and **is** complete, 8 entries
then the sentinel.

**Tick duration is 100 ms**, and every link in the derivation is verified by
address:

1. `FUN_080027F4` calls `SysTick_Config(SystemCoreClock / 1000)`, SysTick is
   **1 kHz**.
2. `SysTick_Handler` (`0x08002334`) increments a counter that wraps above 99,
   and sets bit 2 of the flag byte at `0x20001A19` when it equals 40, so once
   per 100 SysTicks.
3. `FUN_0800071C` turns that bit into action `0xF2`.
4. `0xF2` dispatches to `FUN_0800060C`, which calls the massage engine
   `FUN_08000C34`, once, unconditionally.

So the engine advances one step per 100 ms. A 40-tick step is **4 s** and one
full 39-step cycle is 1185 ticks ≈ **2 min**. `MASSAGE_TICK_MS` is set
accordingly. An earlier build used 20 ms, which ran everything five times too
fast.

The same handler wraps a second counter every 10 of those 100 ms periods, which
is where the once-per-second actions come from, a useful cross-check that the
1 kHz base is right.

### Timing profile

Each pattern step stores **five** durations, and one is selected by an index we
call the **timing profile** (the factory variable at `0x20001AE6`):

```text
step: bits = 0x03, durations = [40, 40, 40, 40, 40]
                                 ^ timing_profile picks one
```

That index is the only thing it controls, how long each step is held, and
therefore how fast the pattern runs.

It is **not** a user setting. `FUN_080013B8` sets it to 1 when massage starts
and `FUN_080010E4` sets it to 3 for lumbar; nothing on the handset changes it.
The massage engine then clamps it to 2–4, so massage's 1 becomes 3 and both
functions end up on the same profile anyway.

On this chair it has no observable effect, because all five durations are
identical in both tables. Presumably a model with speed buttons uses the other
columns.

**Not implemented**, and nothing is lost by omitting it.

Massage **SHALL** auto-off after **15 minutes**, see "Auto-off timers".
**[planned]**

### Heat **[done]**

Plain on/off toggle. **There is no temperature feedback anywhere in this
hardware**, PC14 is driven directly from one flag bit, nothing is measured.

Factory behaviour, from `FUN_0800158C`:

- Gated behind POWER, like massage and lumbar.
- Refuses to act while recline or headrest is moving, and stops those axes.
- Toggles bit 7 of `0x20001AF9`; `FUN_08000FA0` drives PC14 from that bit.
- On turn-on, sets a countdown at `0x20001E9E` to 60.

Requirements:

- Heat **SHALL** auto-off after **60 minutes**, matching the factory. **[done]**
- Heat **SHALL NOT** be enabled without that timeout in place. With no
  temperature sensor, the timeout is the only thing bounding a stuck-on heater.
- The heater is driven **fully on, continuously**, no PWM, no duty cycling, no
  thermostat. PC14 is written in only two places (`FUN_08000FA0` and the
  all-stop routine), and the one candidate counter (`0x20001EA3`) is written but
  never read anywhere, so it is dead code.

PC14 is traced from the button and **confirmed on the chair**, driving it
produces heat. The chain: heat button → action `0x32` → `FUN_0800158C` sets bit
7 of `0x20001AF9` → `FUN_08000FA0` reads that variable and drives pin id `0x2E`.

Heat is **not** paused while the pump runs, only while a motor moves. The
factory condition tests the three motion axis bit-pairs and nothing else, so
heater and pump running together is within the supply budget.

### Auto-off timers

`FUN_08002B7C` runs once per second and keeps per-function counters that wrap at
60, so these countdowns are in **minutes**. On each wrap the relevant timer
decrements, and when it falls below 2 the function's enable bit is cleared.

| Function | Timeout | Set by         | Timer        |
|----------|---------|----------------|--------------|
| Massage  | 15 min  | `FUN_080013B8` | `0x20001E9C` |
| Heat     | 60 min  | `FUN_0800158C` | `0x20001E9E` |

Both **SHALL** be reproduced. **[done]**, implemented as elapsed-millisecond
comparisons rather than minute counters, same result.

Lumbar has no equivalent minute timer; its bound is the 200-tick inflate
ceiling.

### Lumbar **[done]**

Bottom bladder only, matching the factory table at `0x080049DC`. LUMBAR cycles
through four states, one per press:

| State   | Valves | Pump | Behaviour                          |
|---------|--------|------|------------------------------------|
| off     | `0x00` | off  | idle                               |
| inflate | `0x08` | on   | fills the bottom bladder           |
| hold    | `0x00` | off  | valve closed, air trapped          |
| deflate | `0x04` | off  | exhaust alone, for 120 s, then off |

Inflate **SHALL** run until the next press, or until the ceiling. It is not a
fixed-duration fill. The ceiling is the factory table's 200 ticks = **20 s**.
**[done]**

LED behaviour: on from the first press, stays on through hold, out when the
third press starts the deflate. **[done]**

### POWER **[done]**

POWER is a **toggle** that gates the comfort functions, massage, heat and
lumbar, matching the factory handset. It does **not** gate recline or headrest,
which work regardless.

- While powered off, MASSAGE / HEAT / LUMBAR presses **SHALL** be ignored
  entirely. **[done]**
- Turning power off **SHALL** stop massage and lumbar and start the 120 s vent.
  **[done]**
- The POWER LED **SHALL** be lit whenever the comfort functions are enabled.
  **[done]**

The earlier hold-to-bleed behaviour has been removed. It existed as a failsafe
while the valve mapping was unknown; powering off now vents anyway, so it was
redundant.

### Long-press POWER, all-off and flatten **[done]**

Holding POWER runs a shutdown macro that the handset doesn't advertise:
everything switches off, then after a pause the chair drives itself flat.
Confirmed on the chair.

Implemented in `FUN_08001974`, called from action `0xF0`:

```c
if (power_button_held) hold_counter++;

if (hold_counter > 0x13) {
    headrest_axis(2);            /* drive down */
    recline_axis(1);             /* drive down */
    flags[0x18] |= 0x80;         /* auto-move in progress */

    if (power_button_released) {
        hold_counter = 0;
        stop_everything();       /* FUN_08001044 */
        ...clear handset/LED bits...
        timer_0x20001E00 = 0x29C;
    }
}
```

Behaviour:

- **Short press** (counter 1–`0x13`, then released) → ordinary power toggle.
- **Long press** (counter > `0x13`) → start driving both motion axes toward
  their down/flat position and set the auto-move flag.
- **On release after a long press** → stop everything and load a timer with
  `0x29C` (668). The same variable is initialised to `0x32C` (812) at boot.

The threshold is 20 counts of action `0xF0`. That action's rate is set by a
compiler-generated division in the SysTick handler, the constants involved
(`0xC28F5C29`, `0x33333333`, `0x0A3D70A3`) are reciprocal-multiply magic, so
Ghidra renders "every N ms" as a multiply-and-compare. N wasn't decoded.

It doesn't need to be: the hold is **observed as about two seconds** on the
chair, which is what we implement. 20 counts at ~100 ms fits.

The auto-move flag matters beyond this macro: `motion_control_tick` checks it
and, when set, **zeroes the manual axis counters**, restarting the 600 ms relay
sequence. That is presumably so a held button can't fight the automatic move.

Requirements:

- **SHALL** be reproduced: long-press POWER drives recline and headrest to their
  down position, and switches every comfort function off **when the macro ends**,
  not when it starts. **[done]**

  Observed on the chair, and matching `FUN_08001974` above, where
  `stop_everything()` sits inside the `power_button_released` branch: while
  POWER is held the chair drives, lumbar stays inflated, heat stays on, and
  massage keeps its LED although the motion pause stops its pump. Everything
  goes out together on release, and the vent follows.

  An earlier version of this spec had the order backwards and the firmware
  followed it, shutting down at the start of the hold.
- The move **SHALL** be bounded in time. There is no position feedback, so it
  runs until the limit switches open or the bound expires. **[done]**, it uses
  the same 30 s `MOTION_TIMEOUT_MS` ceiling as any other motion.
- What the `0x29C` timer gates is **not yet understood**. It is written on
  release and initialised at boot, but I have not found what reads it.

As implemented:

- POWER acts on **release**, not press, so short and long can be distinguished.
  `POWER_LONG_MS` is 2000 in `main.c`.
- The flatten runs as `MOTION_FLATTEN`, a motion like any other, same relay
  sequencing, same timeout, and any other button interrupts it. It drives both
  axes: headrest direction down, plus the recline down pair.
- It continues while POWER stays held and stops on release.
- Both down LEDs light while it runs.
- `power_long_press()` starts the motion and does nothing else. The shutdown is
  `power_macro_finish()`, called on release: comfort functions off, power gate
  off, and `pneumatics_shutdown()` starts the vent. Splitting the macro in two
  is what puts the shutdown at the end rather than the beginning.
- The vent then runs to completion even though the flatten is still stopping,
  per the exhaust rule in §8.

Difference from the factory: theirs keeps driving briefly after release (that is
what the `0x29C` timer appears to be for), ours stops immediately. Revisit if
the chair doesn't reach flat in practice.

## 8a. Persistence. There isn't any

The factory firmware **never writes flash at runtime**. The only reference to
the flash controller (`0x40022000`) anywhere in the image is `FUN_08003EFC`,
which is clock initialisation: enable HXTAL, wait for stability, set flash wait
states, configure and engage the PLL. No erase, no program.

So nothing is persisted across power cycles, and the chair always boots with
everything off.

This corrects an earlier assumption in these docs. `factory-firmware.bin` and a
live dump differ in a small region around `0x0800B5xx`, which was read as a
settings journal. It cannot be, no code writes it. The difference is
programming-time: a different production run, or the file came from a different
unit.

Incidentally, `FUN_08003EFC` also shows the factory runs from **HXTAL plus
PLL**, where we run from the 8 MHz internal RC.

## 9. Current sensing and stall detection **[done]**

**Channel 7 (PA7) is the current sense**, confirmed by logging it through motor
runs. It reads 0 at rest and rises the moment a motor draws current. Earlier
notes in this repo claimed it stayed 0 under load; that was an artefact of
sampling after the motion had already stopped.

Measured on this chair, via the debug block's sample log:

| Condition     | Channel 7                              |
|---------------|----------------------------------------|
| Idle          | 0                                      |
| Inrush        | 359–403, for a **single 20 ms sample** |
| Running       | 35–173                                 |
| End of travel | **drops to 0**                         |

### Ends of travel are not stalls

Both directions terminate on **internal limit switches inside the actuator**.
At the stop the motor disconnects itself: current goes to zero, not up. The
firmware never sees an over-current there, and the mechanism stops whether or
not any firmware is watching.

So stall detection exists only for an **obstruction mid-travel**, where the
limit switch is never reached and the rotor is genuinely blocked. That case has
not been measured, doing so means deliberately jamming the mechanism, but a
locked rotor is electrically the same as inrush, so it will sit at the inrush
level continuously.

### Requirements

- Motion **SHALL** stop when the current sense stays at or above
  `MOTION_STALL_ADC` for `MOTION_STALL_MS`. **[done]**
- Both constants are the factory's: **`0x155` (341)** and **4 s**. The
  threshold sits between running current and locked rotor; the hold time exists
  to ride out inrush. 4 s is very conservative given inrush lasts one sample,
  and can be shortened. Tunables in `motion.h`.
- A trip **SHALL** latch until the button is released, so a held button cannot
  immediately re-drive into whatever stopped it. **[done]** The timeout ceiling
  latches the same way.
- A failed ADC conversion **SHALL NOT** be treated as a stall. **[done]**

### Still open

Current dropping to zero while a motion is driving is an unambiguous "limit
reached" signal. We currently keep the relay energised into an open switch until
the button is released. Dropping it on that signal would be quieter and easier
on the contacts. Not implemented.

### The other two channels

Channel 8 (PB0) sits steady around 373–393 with a small wobble; channel 9 (PB1)
reads 0. Neither is identified. The ADC needs `RCU_CFG0` ADCPSC and `RCU_CFG2`
configured before any of it works, see §3 and `adc.c`.

## 10. Debug interface

A `struct debug_block` at **`0x20000000`**, pinned by the linker so it is
readable over SWD without symbols.

- **SHALL** be zeroed at startup. It lives in a `NOLOAD` section, so stale RAM
  from a previous image otherwise reads back as plausible data. **[done]**
- **SHALL** expose live state: tick counter, ADC channels, raw input states,
  output states, handset frame/byte/error counters, current motion, safety stop
  count, and pneumatic, massage and heat state. **[done]**
- **SHALL** be write-only from the firmware's side. No field may be read back
  and acted on, so the block cannot influence behaviour. See §4. **[done]**

Field layout is in [../README.md](../README.md#debug-block).

## 11. Network control, planned

None implemented.

Intended shape: an ESP32 on PB6/PB7 as a second serial link, with the handset
remaining on PA9/PA10 so both work simultaneously.

- **SHALL** apply the same safety rules in §4 to network-initiated motion.
- **SHALL NOT** allow a network command to hold a motor without a duration.
- **SHOULD** report state, current motion, pump, heat, last handset button, so
  an integration can display it.

## 12. Build and flash

Targets are listed in [../README.md](../README.md#build-and-flash). The ones
that matter here:

```sh
make              # build/socozi.bin, the enhanced default
make reference    # build/socozi-reference.bin, this spec alone
make test         # host tests for both variants, no hardware needed
make backup       # timestamped dump into backup/
make flash
make flash-reference
make restore      # back to the newest backup
```

Flash is unprotected and `../factory-firmware.bin` is a verified dump.

**Note:** attaching GDB halts the CPU. Always `monitor resume` afterwards, or
the firmware sits frozen and appears broken.

### Tests

`pneumatics.c`, `motion.c` and `heat.c` reach the board only through `gpio.h`
and `timing.h`. `make test` links host fakes for those two and runs the real
logic natively, with the test driving `ms_ticks` by hand, so a two minute
massage cycle verifies instantly.

Behaviour specified here **SHOULD** have a test. Everything currently covered:
the full 39 step massage pattern and its 1185 tick length, pump gating in all
three cases, the lumbar cycle, both auto-off timers, motion pause and resume,
relay sequencing and stagger, stall detection against measured inrush and
running currents, the motion timeout, and fault latching.

Also covered, from §8's exhaust rule: a vent surviving a motion without having
its deadline extended, a motion never starting a vent, massage taking the valves
from a pending vent, lumbar deflate continuing through a motion, and lumbar
inflation still pausing for one.

`control.c`, `button.c` and the macros are covered too, driven the way the
handset drives them: a test sets a button code, lets time pass, and asserts on
the relay pins, the valve bits and the LED bitmap sent back. `fakes.c` stands in
for the three `handset.h` entry points they use. That covers the POWER gate, the
motion map, the handset timeout, LED mirroring, and both ways into the flatten
macro.

`handset.c` itself is still not covered. It touches USART registers directly, so
framing and checksum would need splitting out from the peripheral access first.

## 13. Known deviations from factory behaviour

Deliberate, not defects:

- Acts on a single handset frame rather than debouncing, so it responds
  noticeably faster.
- No tap-versus-hold distinction. The factory firmware has one; on this chair it
  appears to serve only noise immunity, since the preset macro it would trigger
  drives an axis this chair doesn't have.
- No massage timing profile selection, the factory's is fixed anyway.
- LED bit 7 is not blinked.
