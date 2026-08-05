# Firmware map, factory-firmware.bin

Ghidra project: `chair/chair`, program `factory-firmware.bin`.

Import settings (must be set manually, raw binary carries no header):

- Language: `ARM:LE:32:Cortex`
- Image base: `0x08000000`
- Extra memory blocks added so peripheral accesses resolve:
  - `SRAM` `0x20000000` +8 KiB, rw
  - `PERIPH` `0x40000000` +128 MiB, rw, volatile
  - `PPB` `0xE0000000` +1 MiB, rw, volatile

140 functions. **No strings in the image at all**, nothing logs, so the code
has to be read through peripherals and control flow.

## Vector table

Standard Cortex-M layout at `0x08000000`.

| Offset | Value               | Meaning                |
|--------|---------------------|------------------------|
| 0x00   | `0x20000420`        | initial SP             |
| 0x04   | `0x080000e0`        | Reset                  |
| 0x08   | `0x0800174c`        | NMI                    |
| 0x0C   | `0x08000f34`        | HardFault              |
| 0x2C   | `0x08001bda`        | SVC                    |
| 0x38   | `0x08001908`        | PendSV                 |
| 0x3C   | `0x08002334`        | SysTick                |
| 0x40+  | mostly `0x080000f2` | shared default handler |

Offsets `0x10`–`0x28` are zero, no MemManage/BusFault/UsageFault, consistent
with ARMv8-M baseline (M23).

## Peripherals referenced

| Base         | Peripheral | Notes                                   |
|--------------|------------|-----------------------------------------|
| `0x40012400` | ADC        | heaviest use, ~70 register accesses     |
| `0x40021000` | RCU/clocks |                                         |
| `0x40020000` | DMA        | INTF/INTC only; channel regs indirect   |
| `0x40003000` | Watchdog   |                                         |
| `0x40022000` | Flash ctrl | **wait states only**, during clock init |
| `0xE000E010` | SysTick    |                                         |

No UART, I2C, SPI or USB register constants anywhere in the image.

GPIO base addresses never appear as constants, ports are passed as pointers
through a HAL, so GPIO access is indirect. See `gpio_set_by_id` below.

**Caveat:** register *offsets* within ADC/RCU are inferred from the STM32F0 map
that GD32 clones. Verify against the GD32E23x user manual before relying on any
specific offset. Something already doesn't fit, `0x40021030` takes 57
references, which is far too many for a clock register.

## ADC path

ADC results are moved by **DMA into a circular buffer**, not read from the data
register, which is why `ADC_RDATA` (`0x4001244C`) appears only once, as a
literal in the DMA setup.

```
adc_dma_init (0x08000580)
  peripheral addr = 0x4001244C   (ADC_RDATA, literal at 0x080005DC)
  memory addr     = 0x20000010   (literal at 0x080005E0)
  mode            = circular (0x2000)
```

`adc_read_latest` (`0x08000220`) is a one-liner returning
`*(uint16_t *)0x20000010`.

Only one channel appears to be consumed. Its value is compared against `0x155`
(341). This is **motor current sense**, not button reading.

## Named so far

| Address      | Name                  | What it does                               |
|--------------|-----------------------|--------------------------------------------|
| `0x08000a48` | `gpio_set_by_id`      | `(id, state)` → pin table → drive pin      |
| `0x08000220` | `adc_read_latest`     | returns latest DMA'd ADC sample            |
| `0x08000580` | `adc_dma_init`        | configures ADC→RAM circular DMA            |
| `0x08001d14` | `motion_control_tick` | per-tick button debounce + motor drive     |
| `0x080031e0` | (unnamed)             | pin id → (port, pin) decoder               |
| `0x08003614` | (unnamed)             | low-level gpio write `(port, mask, state)` |

### `gpio_set_by_id` (`0x08000a48`)

```c
void gpio_set_by_id(uint8_t id, uint8_t state) {
    uint8_t decoded[4];
    decode_pin(decoded, id);
    gpio_write(*(uint32_t *)(PIN_TABLE + decoded[0] * 4), <pin>, state);
}
```

**Pin table lives at `0x08000a84`.** Decoding it yields the complete
ID → port/pin map, i.e. every actuator the board can drive. Not yet done.

IDs observed in use: `5`, `6`, `0x12`, `0x1a`, `0x1b`, `0x1d`, `0x1e`, `0x1f`.

### `motion_control_tick` (`0x08001d14`)

Handles three two-button axes. Each axis is a bit pair (bit0 = one direction,
bit1 = the other) at a fixed offset in the state struct:

| Struct offset | Axis         |
|---------------|--------------|
| `+2`          | unidentified |
| `+4`          | unidentified |
| `+7`          | unidentified |

Which is which is not yet established. Note the handset has only two ▲/▼ pairs
(RECLINE, HEADREST) but the chair likely has **three** motion outputs, seat
back, footrest, headrest, so one handset pair probably drives two motors, or
one of these three axes is the lumbar pump rather than a motor.

Per axis the logic is: detect change → reset a debounce counter → then branch on
how long the button has been held:

- counter `< 2..6` → one set of `gpio_set_by_id` calls (tap behavior)
- counter `> 5`    → different calls, plus latch bits set in the struct (hold)

So taps and holds do different things. Worth pinning down. It may be how
massage modes cycle versus how motors jog.

**Safety cutoff**, top of the function:

```c
if (adc > 0x155) overcurrent_ticks++;   // saturates at 0xDC
else if (overcurrent_ticks < 0x29) overcurrent_ticks = 0;

if (overcurrent_ticks > 0x28) {         // ~40 ticks sustained
    clear all motor direction bits;
    set fault bit (state[8] |= 0x10);
}
```

That is end-of-travel / stall detection via the R050 shunts.

## Handset serial protocol

**Correction to an earlier note in this file:** USART0 *is* used. The base
address `0x40013800` appears four times in the image; it was missed initially
because Ghidra's typed-data list didn't surface those literal-pool constants.

Confirmed on hardware: `GPIOA_CTL` = `0x28281555` (PA9/PA10 in AF mode) and
`GPIOA_AFSEL1` = `0x00000110` (AF1 on both) = **USART0_TX / USART0_RX**.

### Port config

`uart_init` (`0x080003D4`) is called once from `0x08000328` with base
`0x40013800` and baud `0x2580` = **9600**. It enables the clock, configures
8N1, enables the RX interrupt, and enables IRQ 27 (USART0).

The driver also has an unused branch for USART1 (`0x40004400`, IRQ 28). Vendor
HAL boilerplate, nothing initializes it, and it proves nothing about whether
this die actually has a second USART.

### The handset is polled, not autonomous

**The handset does not transmit unsolicited.** It answers a poll from the
mainboard. A listener-only implementation receives nothing at all, confirmed by
building one and seeing zero bytes arrive while buttons were held.

The mainboard sends its 6-byte frame; the handset replies with a 4-byte frame.
Polling at 50 Hz works well.

### Frame format

`USART0_IRQHandler` (`0x080029C0`) reads one byte and feeds it to the parser at
`0x080029FC`. Frames are **4 bytes**:

| Byte | Value            | Meaning                   |
|------|------------------|---------------------------|
| 0    | `0x03` or `0x06` | start / message type      |
| 1    | `0x04`           | fixed, probably length    |
| 2    | payload          | button state              |
| 3    | checksum         | sum of bytes 0–2, mod 256 |

Any byte that doesn't fit the expected position resets the parser via
`FUN_08001BA0`. On a good checksum the parser copies all 4 bytes to
`0x20001A6F` and sets **bit `0x40`** in the main I/O state struct at
`0x20001E19` to signal "frame received".

### `0x55` escape sequence

Before frame parsing, the byte is tested against `'U'` (`0x55`). **Ten
consecutive `0x55` bytes** call `FUN_08001034` and reset the counter.

`FUN_08001034` **is an empty function**, it returns immediately. So the hook
exists but does nothing in shipped firmware. Presumably a factory-test or
bootloader entry that was stubbed out for production. No hidden capability
here.

### Transmit side, board → handset

`FUN_08001BDC` (reached via the SVC vector at `0x0800_002C`) builds and sends
the outbound frame. Outbound frames are **6 bytes**, not 4:

| Byte | Value            | Meaning                                                                           |
|------|------------------|-----------------------------------------------------------------------------------|
| 0    | `0x03` or `0x06` | `0x06` when bit 3 of `state[1]` is set, else `0x03`; the bit is cleared after use |
| 1    | `0x04`           | fixed                                                                             |
| 2–4  | payload          | 3 bytes, handset LED / display state                                              |
| 5    | checksum         | sum of bytes 0–4, mod 256                                                         |

Same framing and checksum rule as receive, but with a 3-byte payload instead of
1. Sent with `uart_send(0x40013800, buf, 6)`.

The `0x03`/`0x06` distinction is a real message-type flag on both directions, worth decoding, it likely separates routine status from event/ack.

### Button codes, observed live

Read from `0x20001A6F` over SWD while the firmware ran and buttons were held.

Read from `0x20001A6F` over SWD while the firmware ran.

| Payload | Button        | Confirmed |
|---------|---------------|-----------|
| `0x00`  | none (idle)   | yes       |
| `0x01`  | POWER         | yes       |
| `0x02`  | MASSAGE       | yes       |
| `0x03`  | HEAT          | inferred  |
| `0x04`  | LUMBAR        | yes       |
| `0x05`  | RECLINE up    | yes       |
| `0x06`  | RECLINE down  | yes       |
| `0x07`  | HEADREST up   | inferred  |
| `0x08`  | HEADREST down | inferred  |

Byte 0 is `0x03` when idle and `0x06` when a button is down. That is the
message-type distinction.

The payload is **not** a bitmap. It is a sequential code, numbered in the exact
physical order the buttons appear on the handset. The confirmed values follow
that order exactly, which makes the three inferred rows near-certain.

HEAT read as `0xFF` twice, but both reads happened around an MCU reset (below),
so `0xFF` is treated as corruption rather than a real code.

### Known problem: SWD drops under load

Pressing POWER, HEAT, or a recline button kills the debug link hard enough that
the Bus Pirate stops enumerating over USB and has to be replugged. Loads
switching on the board appear to be browning out or faulting the probe.

This blocks live analysis of exactly the buttons that matter most. Under
investigation on the BP6 firmware side. Mitigations not yet tried: lower adapter
speed, shorter leads, better ground, USB isolator.

### Parser state

| Address      | Purpose                                 |
|--------------|-----------------------------------------|
| `0x2000001C` | consecutive-`0x55` counter              |
| `0x20001A59` | frame byte index                        |
| `0x20001A54` | frame assembly buffer (4 bytes)         |
| `0x20001A5A` | running checksum                        |
| `0x20001A6F` | last valid frame (4 bytes)              |
| `0x20001AE3` | timeout counter, cleared on valid frame |

## Pin map

`pin_configure` (`0x080008B8`) takes the same packed pin ID and a mode enum.
The mode enum decodes as:

| Mode | Meaning                        |
|------|--------------------------------|
| 0    | output, push-pull, no pull     |
| 1    | output, open-drain             |
| 2    | alternate function, open-drain |
| 3    | alternate function, push-pull  |
| 4    | input, floating                |
| 5    | input, pull-up                 |
| 6    | analog                         |
| 7    | input                          |

`FUN_08000242` configures every pin the board uses, in one place:

| ID     | Pin  | Mode           | Role                                |
|--------|------|----------------|-------------------------------------|
| `0x00` | PA0  | output         | unidentified                        |
| `0x01` | PA1  | output         | unidentified                        |
| `0x02` | PA2  | output         | **shift register DATA**             |
| `0x03` | PA3  | output         | **shift register CLOCK**            |
| `0x04` | PA4  | output         | **shift register LATCH**            |
| `0x05` | PA5  | output         | motion axis output                  |
| `0x06` | PA6  | output         | motion axis output                  |
| `0x08` | PA8  | (arg lost)     | unidentified                        |
| `0x09` | PA9  | AF1            | **USART0_TX** → handset             |
| `0x0A` | PA10 | AF1            | **USART0_RX** ← handset             |
| `0x0F` | PA15 | (arg lost)     | unidentified                        |
| `0x10` | PB0  | analog         | **ADC current sense** (R050 shunts) |
| `0x11` | PB1  | (arg lost)     | unidentified                        |
| `0x12` | PB2  | output         | motion axis output                  |
| `0x18` | PB8  | input, float   | unidentified input                  |
| `0x19` | PB9  | input, float   | unidentified input                  |
| `0x1A` | PB10 | output, OD     | motion axis output                  |
| `0x1B` | PB11 | output         | motion axis output                  |
| `0x1C` | PB12 | input, pull-up | unidentified input                  |
| `0x1D` | PB13 | output         | motion axis output                  |
| `0x1E` | PB14 | output         | motion axis output                  |
| `0x1F` | PB15 | output         | motion axis output                  |
| `0x2D` | PC13 | output         | gated actuator, see "Pump / heat"   |
| `0x2E` | PC14 | output         | gated actuator, see "Pump / heat"   |

Never configured, therefore free: **PA7, PA11, PA12, PB3, PB4, PB5, PB6, PB7,
PC15, PF0, PF1**. PA13/PA14 are SWDIO/SWCLK.

Verified on hardware: `GPIOA_CTL` = `0x28281555`, consistent with the above
(PA0–PA6 output, PA7/PA8 input, PA9/PA10 AF, PA13/PA14 AF for SWD).

## Output shift register

Three of those GPIOs are a bit-banged serial-to-parallel expander, not direct
outputs. `FUN_08001CA4` shifts **4 bits**, MSB first:

```c
for (i = 0; i < 4; i++) {
    gpio_set(PA2, msb_of(value));   // DATA
    gpio_set(PA3, 0);               // CLOCK low
    delay();
    gpio_set(PA3, 1);               // CLOCK high
    delay();
    value <<= 1;
}
```

`FUN_08001C78` wraps it: shift 4 bits, then pulse PA4 low→high as **LATCH**.

So there are 4 additional outputs behind a shift register, likely the valve
bank for the air bladders, or relay drives. Physically identifying which
register output goes where needs the PCB traced.

## Subsystems

### Massage engine, `FUN_08000C34`

The largest control routine. Notable structure:

- Runs only when **no motion axis is active**, it checks all three axis bit
  pairs are clear before doing anything, and force-clears PC13 otherwise.
  Motors and massage are mutually exclusive.
- A **timing profile** index is held at `*DAT_08000F08` (`0x20001AE6`), clamped
  to **2–4**. It selects which of the five durations stored per pattern step to
  use, and nothing else. Set to 1 by massage and 3 by lumbar; no handset button
  changes it. Not a user-facing intensity control.
- Two state machines, one per pattern table, each calling `FUN_08000AB4` with a
  table pointer, two counters, and the timing profile. `FUN_08000AB4`
  returns a packed word whose bytes are unpacked into the next step and the
  output bits. **This is the massage pattern sequencer**, the tables it reads
  are the actual massage programs.
- The resulting 4 bits are written into a state byte, and if any are set for
  more than 2 ticks, PC13 is driven high.

### Heat, `FUN_08000FA0`

Much simpler, and gated by the same "no motion active" check:

```c
if (no_axis_active) {
    if (*DAT_0800102C < 0) gpio_set(PC14, 1);   // sign bit = enable
    else { gpio_set(PC14, 0); *DAT_08001030 = 0; }
} else {
    gpio_set(PC14, 0);
}
```

Bang-bang on a flag's sign bit, no ADC feedback in this function. **There is no
temperature sensor read here**, the only ADC channel is current sense. If heat
level is adjustable it must be duty-cycled elsewhere, or it is simply on/off.

Given PC13 is the massage-side gate and PC14 the heat-side gate, PC13 is most
likely the **pump enable** and PC14 the **heater enable**.

### All-stop, `FUN_080025BC`

Clears every output: PC13 low, PC14 low, motor bits cleared, several counters
zeroed, and calls sub-resets. This is the panic/shutdown path and a good model
for the equivalent function in new firmware.

### Startup, `FUN_08000328`

```text
FUN_080027F4()          // SysTick init; spins forever on failure
FUN_08000242()          // configure all pins
uart_init(0x40013800, 9600)
FUN_08000228()          // ADC + DMA init
FUN_08000348()          // timer setup
```

## Command architecture

Buttons do not act directly. The flow is:

```text
USART0 RX IRQ (0x080029C0)
  → frame parser (0x080029FC)          validates, stores frame at 0x20001A6F
  → FUN_08001B68                       copies payload byte to 0x20001ABE
  → FUN_0800167C                       maps code → action id via table at 0x08004B4C
  → FUN_08001AB4                       enqueues action id (10-slot queue, dedup)
  → FUN_0800190C                       dequeues
  → FUN_08002664                       dispatches action id to a handler
```

`FUN_0800167C` writes `0xFF` back to `0x20001ABE` after dispatch to mark the
press consumed, and sets a gate bit so a held button fires once.

### Action dispatch table (`FUN_08002664`)

| Action | Handler      | Meaning                                      |
|--------|--------------|----------------------------------------------|
| `0x0E` | `0x08001660` | POWER                                        |
| `0x1F` | `0x08001248` | **axis+2 direction 2, not on handset**       |
| `0x20` | `0x080011CC` | **axis+2 direction 1, not on handset**       |
| `0x21` | `0x08001340` | RECLINE up                                   |
| `0x22` | `0x080012C4` | RECLINE down                                 |
| `0x28` | `0x080013B8` | MASSAGE                                      |
| `0x2A` | ,            | mapped from button codes 9/10, `0xE0`/`0xE1` |
| `0x2B` | `0x08001750` | idle / no button                             |
| `0x32` | `0x0800158C` | HEAT                                         |
| `0x51` | `0x080010E4` | LUMBAR (pump)                                |
| `0x68` | `0x08001498` | HEADREST up                                  |
| `0x69` | `0x08001514` | HEADREST down                                |
| `0x6A` | `0x0800161C` | **preset macro, not on handset**             |
| `0xA0` | `0x080025BC` | all-stop                                     |
| `0xA1` | `0x08001B68` | process handset frame                        |
| `0xF0` | `0x0800064C` | low-level, unidentified                      |
| `0xF1` | `0x080006A8` | low-level, unidentified                      |
| `0xF2` | `0x0800060C` | low-level, unidentified                      |
| `0xF4` | `0x08000688` | low-level, unidentified                      |
| `0xF5` | `0x080006C8` | low-level, unidentified                      |

`0x6A` stops one axis, drives axis+2 one way and axis+7 the other, then sets
status flags, a preset position (flat / zero-gravity or similar).

## Motion axes

Three axes, each a bit pair in the I/O state struct, each with a setter taking
`1` (one direction), `2` (other direction), or `7` (stop).

| Axis field | Setter       | GPIO                      | Driven by                  | Actuator                |
|------------|--------------|---------------------------|----------------------------|-------------------------|
| `state+4`  | `0x08000820` | PA5, PA6, PB2, PB10       | RECLINE up/down            | unconfirmed             |
| `state+2`  | `0x080004B8` | PB11 (dir), PB14 (enable) | actions `0x1F`/`0x20` only | unconfirmed             |
| `state+7`  | `0x08000F38` | PB13 (dir), PB15 (enable) | HEADREST up/down           | **HEADREST, confirmed** |

### Confirmed on hardware

Verified by driving pins directly with the diagnostic firmware in `../src`.

**Headrest** (`state+7`):

- PB15 high = motor runs, low = stopped
- PB13 low = up, PB13 high = down
- A 300 ms pulse produces visible movement

**Recline** (`state+4`), needs **two pins together**; neither does anything
alone:

| Pins       | Result                                    |
|------------|-------------------------------------------|
| PA6 + PB2  | direction matching the handset's ▲ button |
| PA5 + PB10 | direction matching the handset's ▼ button |
| PB2 alone  | nothing                                   |
| PA6 alone  | nothing                                   |

Since each direction needs its own *pair* of pins, this is one motor with four
control lines, most likely two relays per polarity, not two motors driven
together. An earlier guess in this file that it was two motors was wrong.

Note the handset labeling is counter-intuitive: the ▲ button reclines the seat
back **down**. Directions here are recorded by button, not by physical motion.

**Third axis** (`state+2`, PB11/PB14): driven in both directions, **nothing
happened**. Not connected on this chair, probably a feature of another model
sharing the same board. This is also the axis with no handset button and only
the hidden `0x1F`/`0x20` actions, which fits.

**Pump**: PC13 high runs the pump. Audible immediately.

**Valves**: confirmed by inflating one cell at a time with the bladders
connected.

| Bit    | Function                    |
|--------|-----------------------------|
| `0x01` | **top** bladder             |
| `0x02` | **middle** bladder          |
| `0x04` | **exhaust / vent** (shared) |
| `0x08` | **bottom** bladder          |

Three full-width bladders stacked vertically in the seat back, all in the lumbar
region, Southern Motion's own marketing describes the SoCozi air cells as
covering "the entire lumbar region", which matches.

Cell behavior:

- A cell inflates when its bit is set and the pump runs.
- Closing a cell's valve **traps** its air. Cells do not vent on their own. They are not three-way valves, contrary to an earlier hypothesis in this file.
- A cell vents only when **its own bit and the exhaust bit `0x04`** are both set.

Beware when testing: hoses are easy to reconnect in the wrong order, and the
symptom is the wrong bladder inflating plus deflation appearing not to work at
all. The factory firmware shows the same symptom, which is a quick way to tell a
plumbing problem from a firmware one.

## Massage patterns

`FUN_08000AB4` walks a table of **7-byte entries**:

| Byte | Meaning                                                  |
|------|----------------------------------------------------------|
| 0    | unused in both observed tables (always `0x00`)           |
| 1    | **valve bits**, or `0xAA` as the end-of-table marker     |
| 2–6  | five step durations; one is chosen by the timing profile |

It advances until it finds an entry with a non-zero duration for the current
timing profile, wrapping when it hits the `0xAA` sentinel.

Two tables are referenced from the massage engine.

**Idle/rest table** at `0x080049DC`, one step with bits `0x08` held for 200
ticks, then seven steps of all-valves-closed for 2 ticks each.

**Massage program** at `0x08004A1B`, 16 steps, valve bits in order:

```text
01  03  0A  07  05  05  03  0A  07  05  04  01  03  00  03  00
```

Durations are 40 ticks for most steps, 80 for the `0x04` step, and 5–10 for the
tail.

With the valve mapping confirmed (bit 0 top, bit 1 middle, bit 2 vent, bit 3
bottom), the sequence reads as a downward wave followed by a bleed-off:

| Step | Bits   | Meaning             |
|------|--------|---------------------|
| 1    | `0x01` | top                 |
| 2    | `0x03` | top + middle        |
| 3    | `0x0A` | middle + bottom     |
| 4    | `0x07` | top + middle + vent |
| 5    | `0x05` | top + vent          |
| …    | `0x04` | vent only           |

Inflate traveling down the back, then release from the top. That rolling
handoff is what produces the kneading feel.

An earlier hypothesis in this file, that the valves were three-way and vented
whenever de-energized, was **wrong**. Cells hold their air when their valve
closes; venting requires the exhaust bit.

All five durations are **identical** in both tables, so the timing profile has
no observable effect on this chair. The five-column structure is unused
capability, presumably a model with speed buttons populates it differently.

`state+4` uses four pins in two pairs, PA6+PB2 for one direction, PA5+PB10 for
the other. That is either two motors driven together or a pair of half-bridges
for one motor. Not determined.

What is established: **the firmware drives three independent motion axes**, and
only two of them have handset buttons. The third (`state+2`) is reachable only
via actions `0x1F`/`0x20`.

What is **not** established: how many physical motors exist, or which axis
connects to which. The chair may have separate back and footrest motors, or one
motor moving both. Resolving this needs the physical connectors traced, or each
axis driven one at a time while watching the chair.

Every motion handler refuses to act unless the other axes are idle, and calls
`FUN_0800054C(1)` afterward, which sets a countdown (`10`), probably a
motion-timeout or ramp.

## Globals

Resolved from the pointer tables at `0x08002114` and `0x080022c8`.

| Address      | Size | Meaning                                                                  |
|--------------|------|--------------------------------------------------------------------------|
| `0x20000010` | u16  | ADC DMA circular buffer                                                  |
| `0x20001e19` | ,    | **main I/O / button state struct** (very hot: 39–69 xrefs on its fields) |
| `0x20001ea6` | u16  | last ADC sample, copied each tick                                        |
| `0x20001ea8` | u8   | over-current tick counter                                                |
| `0x20001ea1` | u8   | counter, increments while a flag is set                                  |
| `0x20001aa0` | ,    | flags block                                                              |
| `0x2000000a` | u8   | debounce counter                                                         |
| `0x2000000b` | u8   | debounce counter                                                         |
| `0x2000000c` | u8   | debounce counter                                                         |
| `0x2000000d` | u8   | previous-button-state shadow                                             |

## Next steps

All of the original next steps are done: the pin table is decoded, the handset
protocol is understood in both directions, every actuator is confirmed on the
chair, and the flash region turned out not to be settings at all.

What remains unresolved, none of it blocking:

- The `p1`/`p2` bytes of the outbound handset frame. We send zeros and the
  handset behaves correctly.
- The `0x03` versus `0x06` message-type distinction.
- The `0x29C` timer written when a long POWER press is released.
- The clunk at the end of travel on the factory firmware, which we never
  explained and deliberately don't reproduce.
