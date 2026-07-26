# SoCozi replacement firmware

[![CI](https://github.com/tinkerborg/socozi-firmware/actions/workflows/ci.yml/badge.svg)](https://github.com/tinkerborg/socozi-firmware/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tinkerborg/socozi-firmware/branch/master/graph/badge.svg)](https://codecov.io/gh/tinkerborg/socozi-firmware)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

This is a reverse-engineered open firmware for the control board in a 
SoCozi (Southern Motion) power recliner. 

This is tested in a SoCozi Davidson recliner. This chair's control board
uses a GD32E23x / Cortex-M23, and is silkscreened `PT613A`.

This has full feature parity with the factory firmware and is running daily on my chair.

## Two builds

The default build is the **enhanced** firmware. Everything it does beyond the
factory chair is behind the `ENHANCED` compile-time flag, and `make reference`
builds the same source with that flag off, giving factory-equivalent behaviour.
That reference image is the fallback: if an enhancement misbehaves in a chair
you are sitting in, `make flash-reference` gets back to known-good in one step.

Docs:

- [docs/firmware-spec.md](docs/firmware-spec.md): the spec for the reference
  firmware. Update it before you change code.
- [docs/enhancements-spec.md](docs/enhancements-spec.md): the spec for
  everything behind the `ENHANCED` flag.
- [docs/firmware-map.md](docs/firmware-map.md): notes on how the factory
  firmware works.
- [docs/hardware.md](docs/hardware.md): the board, the handset, the pinout.
- [docs/custom-firmware.md](docs/custom-firmware.md): toolchain notes

## Dependencies

An ARM cross compiler and OpenOCD. Both are system packages.

### macOS

```sh
brew install --cask gcc-arm-embedded
brew install open-ocd
```

### Debian / Ubuntu

```sh
sudo apt install gcc-arm-none-eabi openocd
```

### Arch

```sh
sudo pacman -S arm-none-eabi-gcc openocd
```

### Fedora

```sh
sudo dnf install arm-none-eabi-gcc-cs openocd
```

Running `make check` will check prerequisites for your platform.

Debugging requires `arm-none-eabi-gdb`. It comes with the toolchain
everywhere except Debian/Ubuntu, where it is a separate `gdb-multiarch`
package.

### Hardware

An openocd compatible SWD probe on the board's SWD header, which is labelled.
Any CMSIS-DAP probe should work, and can be configured by modifying the
`OPENOCD_ARGS` setting in the Makefile.

## Build and flash

```sh
make                   # build/socozi.bin, the enhanced image
make reference         # build/socozi-reference.bin, factory-equivalent
make check             # check build prerequisites
make test              # unit tests, both variants, no chair required
make coverage          # unit tests + gcov reports in build/coverage/
make ci                # what the pipeline runs: build + coverage
make backup            # dump current flash to backup/, timestamped
make flash             # erase + write + run
make flash-reference   # same, with the factory-equivalent image
make restore           # write the newest backup/ dump back
make restore-factory   # write factory-firmware.bin back
make reset             # reboot the board, no flash access
make debug             # OpenOCD on :3333 (gdb) and :4444 (telnet)
make clean
```

Run `make backup` before flashing to back up your factory firmware.

The watchdog is on in the shipping image. `make debug` sets `DBG_CTL` bit 8,
which freezes the watchdog while the core is halted, so there is no separate
debug build. That bit is set once until you power cycle, and only `make debug`
sets it, so flashing never leaves the chair in debug mode.

## Debug block

Live state sits at **`0x20000000`**, pinned there by the linker so you can read
it without symbols.

**[src/debug.h](src/debug.h) is the source of truth.** Fields get appended over
time, so read the struct instead of trusting a remembered offset. You get the
loop counter, all three ADC channels, raw port state, handset counters, motion
state, valve bits, massage and lumbar state, and heat.

Dump the fixed part:

```text
monitor mdw 0x20000000 40
```

The block is read only as far as the firmware is concerned. Nothing in it feeds
back into behaviour, so a debugger cannot drive an output through it and a
stray write cannot move the chair.

Field offsets move whenever the struct changes, so pull them from the map file
rather than hardcoding them:

```sh
make symbols && grep -n '<field>' build/socozi.map
```

## Safety

- **Stall detection has never fired on real hardware.** The `0x155` threshold
  is copied from the factory firmware, not measured. Treat it as unproven.
- Motion is also bounded by a 30 s timeout, and stops within 250 ms of the
  handset going quiet. Direction changes go through a full stop first, so two
  directions are never driven at once.
- **There is no temperature sensor on the board.** The only thing bounding the
  heater is a 60 minute timer. This is identical to the factory firmware
  implementation.
- Every output is driven only by the handset logic, and there is no way to
  bypass the bounds above from the debug interface.

## Contributing

This targets one board in one chair. If yours is different, start with the pin
map in [docs/firmware-spec.md](docs/firmware-spec.md#3-pin-assignment) and the
valve and massage tables in `src/pneumatics.c`.

Behaviour changes go in the spec first, then the code. Almost every requirement
in here was recovered by reading disassembly, and the reasoning behind a number
is worth more than the number.

## License

MIT. See [LICENSE](LICENSE).

## Disclaimer

Use at Your Own Risk

The code in this repository is provided "as is", without warranty of any kind, 
express or implied.

The author does not guarantee that the code is free of bugs, errors, or flaws.
You use this code entirely at your own risk. The author shall not be held liable for
any damages, data loss, or system failures resulting from the use or misuse 
of this software.
