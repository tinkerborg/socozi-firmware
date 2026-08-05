# Writing replacement firmware

Start here if you're picking this up fresh. Read [hardware.md](hardware.md) for the
hardware and [firmware-map.md](firmware-map.md) for how the factory image works.

## Decision already made

Replace the firmware outright rather than proxy the handset. The handset
protocol does not expose enough control for what's wanted, and network control
is the goal. An ESP32 will be added for connectivity.

## Target

- **MCU**: GD32E23x, Cortex-M23, LQFP48, 64 KiB flash @ `0x08000000`, 1 KiB pages
- **Language**: C. MicroPython is not viable. The chip has 64 KiB flash and
  single-digit KiB of RAM; MicroPython needs roughly 10× that before any
  application code.
- **Toolchain**: `arm-none-eabi-gcc` (already installed via the
  `gcc-arm-embedded` cask for the Bus Pirate work)
- **Vendor SDK**: GigaDevice GD32E23x firmware library. The part is an
  STM32F0-alike, so STM32F0 examples translate closely.
- **Flashing/debug**: SWD via Bus Pirate 6 running native CMSIS-DAP. Same path
  already used for reading. Flash is unprotected.

### Recovery

`factory-firmware.bin` is a verified full dump. A bad flash is recoverable:

```sh
openocd -c "adapter driver cmsis-dap" -c "transport select swd" -c "adapter speed 1000" \
  -f target/stm32f1x.cfg -c "init" -c "halt" \
  -c "flash write_image erase factory-firmware.bin 0x08000000" \
  -c "reset" -c "shutdown"
```

Take a fresh dump before the first write anyway.

## Hardware inventory

Actuators the board drives:

- **Recline**, probably two motors (seat back, footrest)
- **Headrest**, one motor
- **Lumbar**, a **pump** (pneumatic, not a motor)
- **Massage**, air bladders in the seat back, 2 bladders / 4 hoses, pump + valves
- **Heat**, resistive heater

Sensing available. This is the important constraint:

- **One ADC channel**, PB0, fed from two R050 (50 mΩ) shunts. Motor/actuator
  current. That is the *only* analog input.
- No position encoders, no hall sensors, no pressure sensor, no thermistor
  appear anywhere in the firmware.
- Two floating digital inputs (PB8, PB9) and one pulled-up input (PB12) whose
  purposes are unidentified, these are the only candidates for limit switches.

**Consequence for feature specs:** the firmware cannot know absolute position.
"Recline to 40%" is not expressible; "run the recline motor for 3.2 seconds" is.
Any position tracking must be dead-reckoned by timing and will drift. If PB8/PB9
turn out to be limit switches, homing at an endstop becomes possible, worth
determining early, since it changes what can be promised.

## Pin map

See the full table in [firmware-map.md](firmware-map.md#pin-map). Summary:

| Pins                                        | Use                                             |
|---------------------------------------------|-------------------------------------------------|
| PA9 / PA10                                  | USART0 to handset, 9600 8N1                     |
| PA2 / PA3 / PA4                             | bit-banged shift register: DATA / CLOCK / LATCH |
| PB0                                         | ADC current sense                               |
| PA5, PA6, PB2, PB10, PB11, PB13, PB14, PB15 | motor direction outputs                         |
| PC13                                        | pump enable (probable)                          |
| PC14                                        | heater enable (probable)                        |
| PA0, PA1, PA8, PA15, PB1                    | outputs, unidentified                           |
| PB8, PB9, PB12                              | inputs, unidentified                            |
| PA13 / PA14                                 | SWD, do not repurpose                           |

**Free pins: PA7, PA11, PA12, PB3, PB4, PB5, PB6, PB7, PC15, PF0, PF1.**

PB6/PB7 are the USART0 remap pins. Since new firmware owns the pin assignment,
one workable arrangement is ESP32 on PB6/PB7 as USART0 and the handset dropped
entirely; another is keeping the handset on PA9/PA10 and giving the ESP32 a
different interface on free pins. Whether USART1 physically exists on this die
is **unverified**, the vendor HAL in the factory image references it, but that
proves nothing. Check the GD32E230 datasheet rather than assuming.

## Behavior worth preserving

### Over-current cutoff, safety critical

From `motion_control_tick` (`0x08001D14`):

```c
if (adc > 0x155) overcurrent_ticks++;          // saturates at 0xDC
else if (overcurrent_ticks < 0x29) overcurrent_ticks = 0;

if (overcurrent_ticks > 0x28) {                // ~40 consecutive ticks
    clear all motor direction bits;
    state[8] |= 0x10;                          // fault flag
}
```

This is end-of-travel and stall detection. There are no limit switches proven,
so **this is the only thing stopping a motor from driving into a hard stop**.
Reimplement it faithfully. The threshold `0x155` (341 counts) and the ~40-tick
debounce should be carried over and then tuned against measurements, not
guessed.

The tick rate this counts in has not been established, find the SysTick period
before treating "40 ticks" as a duration.

### Motion and massage are mutually exclusive

Both the massage engine and the heat control refuse to run while any motion
axis is active, and force their outputs off. Preserve this, it's presumably
about total current draw from the supply.

### Tap versus hold

`motion_control_tick` branches on how long a button has been held (counter < 2
vs > 5), doing different things in each case. Whatever replaces it should decide
deliberately how momentary versus continuous commands behave, especially for a
network interface where "hold" has no natural meaning. A network command should
probably carry an explicit duration.

### Massage intensity. There is none

Each pattern step stores five durations and an index selects one. It looked
like an intensity setting, but nothing cycles it: the massage handler writes 1
(clamped straight back to 3) and the lumbar handler writes 3, and no other code
touches it. All five columns are identical in both tables, so it has no effect
on this chair. See the timing-profile section of `firmware-spec.md`.

## Persistence. There is none

**The factory firmware never writes flash at runtime.** The only reference to
the flash controller in the image is in clock initialization, setting wait
states. Nothing is stored across power cycles, and the chair always boots with
everything off.

An earlier note here described `0x0800B5xx`–`0x0800B6xx` as a settings journal,
because `factory-firmware.bin` and a live dump differ there. That reading was
wrong, no code writes it. The difference is programming-time: a different
production run, or the file came from a different unit.

If you want persistence, it has to be built from scratch, and there is no
working erase/write reference in the factory image to copy.

## Handset protocol

Fully documented in [firmware-map.md](firmware-map.md#handset-serial-protocol).
In brief, 9600 8N1:

- **Handset → board**: 4 bytes, `[0x03|0x06] [0x04] [buttons] [checksum]`
- **Board → handset**: 6 bytes, `[0x03|0x06] [0x04] [3 payload bytes] [checksum]`
- Checksum is the sum of all preceding bytes, mod 256

Keep this if you want the physical remote to keep working. The outbound 3-byte
payload drives the handset LEDs and is the natural place to reflect state.

## Verified on hardware

Confirmed by driving pins with the diagnostic firmware in `../src`:

| Actuator | Control                                                 |
|----------|---------------------------------------------------------|
| Headrest | PB15 = run, PB13 = direction (low up, high down)        |
| Recline  | PA6 + PB2 together = ▲ button direction; PA5 + PB10 = ▼ |
| Pump     | PC13 high                                               |
| Valves   | 4 bits clocked out on PA2/PA3/PA4 shift register        |

Recline needs **both** pins of a pair; neither does anything alone. The third
motion axis (PB11/PB14) does nothing on this chair, likely another model's
feature on a shared board.

Handset button codes are a sequential table, `0x01`–`0x08` in the physical order
the buttons appear. See `firmware-map.md`.

## Implemented so far

`../src` is no longer only a diagnostic image. Working on the chair:

- **Handset control of recline and headrest.** Motion runs while a button is
  held and stops on release.
- The handset must be **polled**. It never transmits unprompted. We send the
  6-byte frame at 50 Hz and parse the 4-byte reply.
- Safety bounds, since there is still no stall detection: a 250 ms
  handset-silence timeout, a 30 s hard cap per motion, and every direction
  change passes through a full stop so two directions are never energized at
  once.
- The probe block is zeroed at startup. It lives in a `NOLOAD` section, so
  before this fix stale RAM from a previous image read back as plausible
  counters, which briefly looked like a million received frames when the true
  count was zero.

Response is noticeably snappier than the factory firmware, which debounces
several ticks before acting.

- **Massage**, the OEM 16-step valve pattern, toggled by the MASSAGE button.
- **Lumbar**, bottom bladder, cycling inflate → hold → deflate → off, matching
  the factory behavior.
- **Handset LEDs**, the outbound payload's first byte is a bit-per-button
  bitmap; recline and headrest LEDs light while their motion runs.
- POWER held bleeds all bladders.

Valve map is confirmed: bit 0 top, bit 1 middle, bit 2 exhaust, bit 3 bottom.

Not yet implemented: heat, massage intensity levels, network control.

## What is not yet reversed

Being explicit so nobody assumes more coverage than exists. Roughly 40 of 140
functions have been read.

- **Which valve bit maps to which hose.** Needs the bladders reconnected and
  someone feeling each port while the bits are cycled.
- **Whether the valves are three-way** (vent when de-energized) or need a
  dedicated exhaust. The pattern tables suggest three-way; untested.
- **PB8, PB9, PB12**, all three read high at rest. Whether any is a limit
  switch is still unknown, and it determines whether homing is possible.
- **How massage intensity works.** The pattern table's five per-level durations
  are identical, so level must act somewhere else.
- **The `0x03` vs `0x06` message-type distinction** in the serial protocol.
- **SysTick period**, needed to convert tick counts into real time.
- **The `0xF0`–`0xF5` actions** in the dispatch table.
- **Heat**, never tested on hardware. PC14 is inferred, not confirmed.

## ADC

**Three channels, not one.** The factory firmware's ADC init (`0x08000178`)
configures a 3-entry regular sequence:

| Rank | Channel | Pin |
|------|---------|-----|
| 0    | 7       | PA7 |
| 1    | 8       | PB0 |
| 2    | 9       | PB1 |

Results DMA into `0x20000010` as three halfwords. `adc_read_latest`
(`0x08000220`) returns the **first**, channel 7, which is what the
over-current cutoff compares against `0x155`.

Earlier notes in this repo claimed PB0 was the only analog input. That was
wrong; PA7 and PB1 are analog too, and PA7 is not free.

### The clock config is mandatory

The factory firmware calls `rcu_adc_clock_config(6)` (`0x080037D4`) before
anything else:

```c
RCU_CFG0 = (RCU_CFG0 & ~(3u << 14)) | (2u << 14);   /* ADCPSC */
RCU_CFG2 |= (1u << 8);                              /* ADC clock select */
```

Without it the ADC has no usable clock, calibration never completes, and
conversions hang. This was the bug in our first diagnostic build.

Incidentally this identifies `0x40021030`, the register with 57 references
that didn't fit "clock register" earlier, as **RCU_CFG2**.

### Open problem: which channel is the current sense

With the clock fixed, conversions work. But:

- **Channel 7 reads 0**, and stayed 0 across a full motor run, including
  `adc_ch7_max`.
- Channel 8 sits steady around 373–376, looks like a fixed reference or supply
  monitor.
- Channel 9 read 83 during a motor run and 0 at rest.

So channel 9 behaves like the current sense, yet channel 7 is the one the
factory cutoff watches. Either the shunt amplifier is only powered under some
condition we aren't meeting, or the rank-to-channel mapping is not what the
decompilation suggests.

**Until this is resolved there is no working stall detection.** That is the
factory firmware's only safety mechanism, so any custom firmware driving motors
is currently unprotected. Bound every motion command by time.

Calibration still times out, which costs a few counts of offset error, tolerable for threshold comparisons, and not the blocker here.

## Suggested order of work

1. **Fix the ADC.** It's the safety mechanism; nothing else should ship without
   it.
2. Determine what PB8/PB9/PB12 are wired to.
3. Map valve bits to hoses once the bladders are reconnected.
4. Find the SysTick period so tick counts become durations.
5. Measure real stall current, to set the cutoff threshold from data rather than
   copying `0x155` blindly.
