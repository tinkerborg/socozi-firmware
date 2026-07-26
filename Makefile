CROSS   ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

BUILD    = build
BACKUPS  = backup
WATCHDOG ?= 1

# Build variant. ENHANCED=1, the default, builds the enhanced firmware; the
# `reference` target rebuilds with ENHANCED=0, which is the factory-equivalent
# behaviour described in docs/firmware-spec.md and the fallback if an
# enhancement misbehaves in the chair.
#
# The two variants get different filenames so a reference image is never
# mistaken for an enhanced one on the bench. Nothing else needs separating:
# the firmware links in a single compiler invocation, so there are no
# intermediate objects to go stale between variants.
ENHANCED ?= 1

ifeq ($(ENHANCED),0)
SUFFIX = -reference
else
SUFFIX =
endif

TARGET   = $(BUILD)/socozi$(SUFFIX)

SRCS    = src/main.c src/adc.c src/gpio.c src/handset.c src/heat.c \
          src/motion.c src/pneumatics.c src/power.c src/timing.c \
          src/watchdog.c src/startup.c
LDSCRIPT = gd32e230c8.ld

# The link is one compiler invocation, so there is no per-object dependency
# tracking to lean on. Headers gate real behaviour now, enhancements.h most of
# all, so a header edit that didn't rebuild would silently flash a stale image.
HDRS    = $(wildcard src/*.h)

CFLAGS  = -mcpu=cortex-m23 -mthumb -Os -g3 -std=c11
CFLAGS += -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS = -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections

# DBG_CTL bit 8 freezes the watchdog while the core is halted. Set-once until a
# power cycle, and only writable over the debug port, so `debug` applies it and
# nothing else does.
DBG_HOLD = -c "mww 0x40015804 0x00000100"

OPENOCD_ARGS = -c "adapter driver cmsis-dap" \
               -c "transport select swd" \
               -c "adapter speed 1000" \
               -c "swd newdap chip cpu -enable" \
               -c "dap create chip.dap -chain-position chip.cpu" \
               -c "target create chip.cpu cortex_m -dap chip.dap" \
               -c "flash bank chip.flash stm32f1x 0x08000000 0x10000 0 0 chip.cpu"

all: $(TARGET).bin

# The factory-equivalent build, with every enhancement compiled out.
reference:
	@$(MAKE) --no-print-directory ENHANCED=0 all

flash-reference:
	@$(MAKE) --no-print-directory ENHANCED=0 flash

# Both tools are system packages; see README.md for per-distro install lines.
check:
	@ok=1; \
	for t in $(CC) openocd; do \
	  if command -v $$t >/dev/null 2>&1; then \
	    echo "  ok      $$t"; \
	  else \
	    echo "  MISSING $$t"; ok=0; \
	  fi; \
	done; \
	if [ $$ok -eq 0 ]; then \
	  echo; \
	  case "$$(uname -s)" in \
	    Darwin) echo "  brew install --cask gcc-arm-embedded && brew install open-ocd" ;; \
	    *) if   [ -f /etc/debian_version ]; then echo "  sudo apt install gcc-arm-none-eabi openocd"; \
	       elif [ -f /etc/arch-release ];   then echo "  sudo pacman -S arm-none-eabi-gcc openocd"; \
	       elif [ -f /etc/fedora-release ]; then echo "  sudo dnf install arm-none-eabi-gcc-cs openocd"; \
	       else echo "  install an arm-none-eabi toolchain and openocd"; fi ;; \
	  esac; \
	  exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET).elf: $(SRCS) $(HDRS) $(LDSCRIPT) | $(BUILD)
	$(CC) $(CFLAGS) -DWATCHDOG=$(WATCHDOG) -DENHANCED=$(ENHANCED) $(LDFLAGS) \
	      -Wl,-Map=$(TARGET).map $(SRCS) -o $@
	@$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

symbols: $(TARGET).elf

# Host tests. pneumatics.c, motion.c and heat.c reach the board only through
# gpio.h and timing.h, so linking fakes for those two runs the real logic
# natively. No hardware, no cross compiler.
TEST_SRCS  = pneumatics motion heat
TEST_BINS  = $(TEST_SRCS:%=$(BUILD)/test_%$(SUFFIX))
TEST_CC    = cc
TEST_FLAGS = -std=c11 -Wall -Wextra -g -Itests

$(BUILD)/test_%$(SUFFIX): tests/test_%.c tests/fakes.c src/pneumatics.c src/motion.c src/heat.c $(HDRS) | $(BUILD)
	$(TEST_CC) $(TEST_FLAGS) -DENHANCED=$(ENHANCED) $(filter %.c,$^) -o $@

# Both variants, always. An enhancement that breaks the reference path has
# broken the thing we fall back to, and that should fail the build, not wait to
# be discovered on the chair.
test:
	@$(MAKE) --no-print-directory ENHANCED=1 test-variant
	@$(MAKE) --no-print-directory ENHANCED=0 test-variant

test-variant: $(TEST_BINS)
	@echo "-- tests: ENHANCED=$(ENHANCED)"
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; exit $$fail

# Coverage of the three tested modules, in gcov format for Codecov.
#
# Objects are compiled individually into COV_DIR so the .gcno and .gcda files
# land next to them instead of scattering through the tree. Each test binary
# gets its own copy of the modules, prefixed with the test name; Codecov merges
# them.
#
# `cc` is clang on macOS and gcc on CI, and their coverage data is not
# interchangeable, so the reader has to match the compiler. Override with
# GCOV=... if the guess is wrong.
COV_DIR     = $(BUILD)/coverage$(SUFFIX)
COV_MODULES = src/pneumatics.c src/motion.c src/heat.c

ifeq ($(shell uname -s),Darwin)
GCOV ?= xcrun llvm-cov gcov
else
GCOV ?= gcov
endif

coverage:
	@rm -rf $(COV_DIR)
	@fail=0; \
	for t in $(TEST_SRCS); do \
	  d=$(COV_DIR)/$$t; mkdir -p $$d; objs=""; \
	  for s in $(COV_MODULES) tests/fakes.c tests/test_$$t.c; do \
	    o=$$d/`basename $$s .c`.o; \
	    $(TEST_CC) $(TEST_FLAGS) -DENHANCED=$(ENHANCED) --coverage -c $$s -o $$o || exit 1; \
	    objs="$$objs $$o"; \
	  done; \
	  $(TEST_CC) --coverage -o $$d/run $$objs || exit 1; \
	  $$d/run || fail=1; \
	  $(GCOV) -pb -o $$d $(COV_MODULES) >/dev/null; \
	  mv *.gcov $$d/ 2>/dev/null || true; \
	done; \
	exit $$fail
	@echo "gcov reports under $(COV_DIR)"

# What CI runs: both variants must build for the target, and the tests must pass
# with coverage. Kept as one target so the pipeline has a single entry point.
# `coverage` measures the default enhanced build.
ci: all reference coverage

backup:
	@mkdir -p $(BACKUPS)
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset halt" \
	  -c "dump_image $(BACKUPS)/backup-$(shell date +%Y%m%d-%H%M%S).bin 0x08000000 0x10000" \
	  -c "shutdown"

# `reset halt` rather than `halt`: catches the core before the watchdog is
# kicked, so writing over a running image doesn't need two passes.
flash: $(TARGET).bin
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset halt" \
	  -c "flash write_image erase $(TARGET).bin 0x08000000" \
	  -c "reset run" -c "shutdown"

# Timestamped names sort chronologically, so the last one is the newest.
LATEST_BACKUP = $(lastword $(sort $(wildcard $(BACKUPS)/backup-*.bin)))

restore:
	@test -n "$(LATEST_BACKUP)" || \
	  { echo "no backups in $(BACKUPS)/, run 'make backup' first"; exit 1; }
	@echo "restoring $(LATEST_BACKUP)"
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset halt" \
	  -c "flash write_image erase $(LATEST_BACKUP) 0x08000000" \
	  -c "reset run" -c "shutdown"

# The original factory image, kept separately from the rolling backups.
restore-factory:
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset halt" \
	  -c "flash write_image erase factory-firmware.bin 0x08000000" \
	  -c "reset run" -c "shutdown"

# Leaves a server on 3333 (gdb) and 4444 (telnet).
debug: symbols
	openocd $(OPENOCD_ARGS) -c "init" $(DBG_HOLD)

reset:
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset run" -c "shutdown"

clean:
	rm -rf $(BUILD)

.PHONY: all reference check test test-variant coverage ci symbols backup flash \
        flash-reference restore restore-factory debug reset clean
