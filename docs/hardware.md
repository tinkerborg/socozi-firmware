# Hardware

The board, the handset, and how to get a debugger onto it. Reverse engineering
notes on the factory firmware are in [firmware-map.md](firmware-map.md).

## Handset

Wired remote on a 4 conductor cable. Every button has an LED.

| Button   | Type              | LED |
|----------|-------------------|-----|
| POWER    | toggle, long hold | yes |
| MASSAGE  | toggle            | yes |
| HEAT     | toggle            | yes |
| LUMBAR   | 3 state cycle     | yes |
| RECLINE  | ▲ / ▼ pair        | yes |
| HEADREST | ▲ / ▼ pair        | yes |

The cable carries power, ground and a 9600 8N1 serial pair. The handset never
transmits on its own, the board polls it. Protocol is in
[firmware-map.md](firmware-map.md#handset-serial-protocol).

LUMBAR cycles inflate, hold, deflate. A long POWER hold shuts everything down
and flattens the chair.

## Board

Control box PCB, silkscreen **`PT613A`**.

- **MCU**: LQFP48, identified over SWD as **GD32E23x**
  - Cortex-M23 r1p0 (CPUID `0x411cd200`, PARTNO `0xD20`)
  - DBG ID register `0x40015800` = `0x19090410`
  - 64 KiB flash at `0x08000000`, 64 pages of 1 KiB, **not read protected**.
    The page size is the FMC's erase granularity, measured on the chair: after
    erasing `0x08000000`, programming `0x08000400` sets PGERR.
  - Flash size register `0x1FFFF7E0` = `0x00080040` (64 KiB)
  - 8 KiB SRAM at `0x20000000`
- Runs at the reset default 8 MHz IRC. No PLL setup anywhere in the factory
  firmware.
- **Current sensing**: two `R050` (50 mΩ) shunts near the output stages, into
  ADC channels 7, 8 and 9. Channel 7 is the one the factory over current cutoff
  watches.
- Crystal Y1, several buck converters, discrete FETs for pump, valves and
  heater.

`0xE0042000`, the STM32 F1/F2/F4 DBGMCU_IDCODE address, is **not** readable on
this part. It faults. Use `0x40015800`.

## Actuators

Two motors, one pump, one heater.

- **Recline**: one motor, moving the seat back and footrest together. Each
  direction needs both pins of a pair energized, neither does anything alone.
- **Headrest**: one motor, separate direction and enable pins.
- **Third motion axis**: present on the board (PB11/PB14), not connected on
  this chair. Probably another model's feature on a shared board.
- **Pneumatics**: three bladders stacked vertically in the seat back, one hose
  each, one pump, one shared exhaust. See below.
- **Heat**: resistive element, on or off, no duty cycling and no temperature
  sensor anywhere on the board.

There is **no position feedback of any kind**. No encoders, no hall sensors, no
pressure sensor. Both ends of travel have internal limit switches that simply
open the motor circuit, so current drops to zero at the stops rather than
rising.

### Valves

Four valve bits go out through a shift register on PA2/PA3/PA4:

| Bit    | Bladder          |
|--------|------------------|
| `0x01` | top              |
| `0x02` | middle           |
| `0x04` | exhaust (shared) |
| `0x08` | bottom           |

Confirmed on the chair. Cells do **not** self vent. Closing a valve traps the
air, and venting means opening the exhaust on its own. Both massage and lumbar
use these same three bladders, lumbar being the bottom one alone.

## Debug access

SWD on the labeled header, via a Bus Pirate 6 running native CMSIS-DAP
firmware. No special probe needed. See the Makefile for the exact invocation
the build uses.

```sh
openocd -c "adapter driver cmsis-dap" -c "transport select swd" -c "adapter speed 1000" \
  -f target/stm32f1x.cfg -c "init" -c "halt" \
  -c "<commands>" -c "shutdown"
```

`stm32f1x.cfg` connects and its flash driver probes the GD32 correctly, so it
is close enough for reads and writes. It is *not* an F1, so do not trust
anything F1 specific in that config beyond flash geometry.

Put **220 Ω series resistors on SWCLK and SWDIO**. Without them the link drops
as soon as a motor runs.

Useful commands:

- `mdw <addr> [count]` reads memory
- `dump_image <file> 0x08000000 0x10000` takes a full flash dump
- `halt` stops the CPU. Add `resume` before `shutdown` if the chair is
  connected.

## There is no persistent storage

The factory firmware never writes flash at runtime. The only reference to the
flash controller in the whole image is in clock init, setting wait states. The
chair always boots with everything off.

An earlier version of this file claimed `0x0800B5xx` to `0x0800B6xx` was a
settings journal, because `factory-firmware.bin` and a live dump differ there.
That was wrong, no code writes it. The difference is from programming time, a
different production run or a different unit.
