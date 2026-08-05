CROSS   ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

BUILD    = build
BACKUPS  = backup
WATCHDOG ?= 1

# The RTT console, src/rtt.c. Costs about 600 bytes of RAM and changes nothing
# the chair does, so it is on in both variants; `make RTT=0` leaves it out.
RTT      ?= 1

# The ESP32 bridge's two extra fields in the debug block, src/debug.h. Costs
# eight bytes of RAM and no behavior, but it does grow the block, so the
# `reference` target turns it off to keep that build's RAM layout untouched.
BRIDGE   ?= 1

# One word identifying this build, published in the debug block so the bridge
# can tell what the chair is running. See src/version.h for the encoding: a
# clean tree gives the commit, a dirty one gives the build time.
#
# Held in a file rather than computed per invocation. A dirty tree stamps the
# time, so evaluating it twice would give two answers — and the image the
# bridge carries would then claim a version the firmware inside it was never
# compiled with, which is exactly the comparison the whole scheme rests on.
# The file is rebuilt when, and only when, the image is.
VERSION_FILE = $(BUILD)/version.txt
FW_VERSION   = $(shell cat $(VERSION_FILE) 2>/dev/null || echo 0)

# Build variant. ENHANCED=1, the default, builds the enhanced firmware; the
# `reference` target rebuilds with ENHANCED=0, which is the factory-equivalent
# behavior described in docs/firmware-spec.md and the fallback if an
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

SRCS    = src/main.c src/adc.c src/adjust.c src/button.c src/console.c \
          src/control.c src/flash.c src/gpio.c src/handset.c src/heat.c \
          src/motion.c src/pneumatics.c src/power.c src/rtt.c \
          src/settings.c src/timing.c src/watchdog.c src/startup.c \
          src/macros/flatten.c src/macros/preset.c
LDSCRIPT = gd32e230c8.ld

# The link is one compiler invocation, so there is no per-object dependency
# tracking to lean on. Headers gate real behavior now, enhancements.h most of
# all, so a header edit that didn't rebuild would silently flash a stale image.
HDRS    = $(wildcard src/*.h src/macros/*.h)

# -Isrc so subdirectory modules include "motion.h" rather than "../motion.h".
CFLAGS  = -mcpu=cortex-m23 -mthumb -Os -g3 -std=c11 -Isrc
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
	@$(MAKE) --no-print-directory ENHANCED=0 BRIDGE=0 all

flash-reference:
	@$(MAKE) --no-print-directory ENHANCED=0 BRIDGE=0 flash

# python3 is only needed for the ESP32 bridge; the venv it builds is disposable
# and lives under build/.
check:
	@ok=1; \
	for t in $(CC) openocd python3; do \
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

$(VERSION_FILE): $(SRCS) $(HDRS) $(LDSCRIPT) | $(BUILD)
	@if [ -z "$$(git status --porcelain 2>/dev/null)" ] && \
	    h=$$(git rev-parse --short=7 HEAD 2>/dev/null); then \
	  printf '0x%s\n' "$$h" > $@; \
	else \
	  printf '0x%08x\n' $$(( 2147483648 + ($$(date +%s) & 2147483647) )) > $@; \
	fi

$(TARGET).elf: $(SRCS) $(HDRS) $(LDSCRIPT) $(VERSION_FILE) | $(BUILD)
	$(CC) $(CFLAGS) -DWATCHDOG=$(WATCHDOG) -DRTT=$(RTT) -DENHANCED=$(ENHANCED) \
	      -DBRIDGE=$(BRIDGE) -DFW_VERSION=$(FW_VERSION) $(LDFLAGS) \
	      -Wl,-Map=$(TARGET).map $(SRCS) -o $@
	@$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

symbols: $(TARGET).elf

# Host tests. pneumatics.c, motion.c, heat.c and settings.c reach the board only
# through gpio.h, timing.h and flash.h, so linking fakes for those three runs
# the real logic natively. No hardware, no cross compiler.
TEST_SRCS  = pneumatics motion heat control settings
TEST_BINS  = $(TEST_SRCS:%=$(BUILD)/test_%$(SUFFIX))
TEST_CC    = cc
TEST_FLAGS = -std=c11 -Wall -Wextra -g -Itests -Isrc

# control.c reaches the board only through the same two seams, plus handset.h,
# which fakes.c now stands in for. That puts button handling and the macros
# under test without linking any peripheral code.
TEST_MODULES = src/pneumatics.c src/motion.c src/heat.c src/button.c \
               src/control.c src/power.c src/settings.c src/adjust.c \
               src/macros/flatten.c src/macros/preset.c

$(BUILD)/test_%$(SUFFIX): tests/test_%.c tests/fakes.c $(TEST_MODULES) $(HDRS) | $(BUILD)
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
COV_MODULES = $(TEST_MODULES)

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

# The bridge as well. Separate from `ci` because it downloads a RISC-V
# toolchain and an ESP-IDF the firmware build has no use for, which is several
# minutes on a cold cache.
ci-esp: esp

backup:
	@mkdir -p $(BACKUPS)
	openocd $(OPENOCD_ARGS) \
	  -c "init" -c "reset halt" \
	  -c "dump_image $(BACKUPS)/backup-$(shell date +%Y%m%d-%H%M%S).bin 0x08000000 0x10000" \
	  -c "shutdown"

# The RTT console. Leaves OpenOCD in the foreground serving TCP 9090; connect
# with `telnet localhost 9090` from another terminal and press ? for the keys.
#
# The chair keeps running throughout. RTT is read out of SRAM by the debug
# access port, which does its own bus transactions, so nothing is halted and
# the watchdog is never in danger.
console:
	openocd $(OPENOCD_ARGS) \
	  -c "init" \
	  -c "rtt setup 0x20000000 0x2000 \"SEGGER RTT\"" \
	  -c "rtt start" \
	  -c "rtt server start 9090 0"

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

# --- ESP32 bridge ---------------------------------------------------------
#
# An ESP32-C3 on the SWD pins, running ESPHome, so the chair appears in Home
# Assistant and can be reflashed over wifi. See docs/esphome-design.md.
#
# Nothing here is vendored. ESPHome goes into a throwaway venv under build/,
# and it fetches PlatformIO and the RISC-V toolchain into its own caches in
# ~/.esphome and ~/.platformio, where any other ESPHome project on this machine
# shares them. The two generated headers are the only files that land in the
# tree, they are both gitignored, and `make esp-clean` removes the lot.
#
# They sit beside the component's own sources rather than in a subdirectory
# because ESPHome copies only the top level of an external component into its
# build.
ESP_DIR      = esphome
ESP_GEN      = $(ESP_DIR)/components/socozi
ESP_VENV     = $(BUILD)/esphome-venv
ESPHOME      = $(ESP_VENV)/bin/esphome
ESPHOME_VER ?= 2025.7.0
ESP_YAML    ?= $(ESP_DIR)/chair.yaml

$(ESPHOME):
	python3 -m venv $(ESP_VENV)
	$(ESP_VENV)/bin/pip install --quiet --upgrade pip
	$(ESP_VENV)/bin/pip install --quiet "esphome==$(ESPHOME_VER)"

# Host-compiled with the firmware's flags, so the offsets it prints are the
# ones the image being built actually uses.
$(BUILD)/gen-dbg-layout: tools/gen-dbg-layout.c $(HDRS) $(VERSION_FILE) | $(BUILD)
	$(TEST_CC) $(TEST_FLAGS) -DENHANCED=$(ENHANCED) -DBRIDGE=$(BRIDGE) \
	  -DRTT=$(RTT) -DFW_VERSION=$(FW_VERSION) $< -o $@

$(ESP_GEN)/socozi_layout.h: $(BUILD)/gen-dbg-layout
	$< > $@

$(ESP_GEN)/socozi_image.h: $(TARGET).bin tools/gen-image-header.sh
	tools/gen-image-header.sh $(TARGET).bin $(FW_VERSION) > $@

esp-gen: $(ESP_GEN)/socozi_layout.h $(ESP_GEN)/socozi_image.h

# Placeholders, so a fresh clone and CI both compile without a manual step.
# Good enough to build with and useless to run with, which is the intent.
$(ESP_DIR)/secrets.yaml:
	cp $(ESP_DIR)/secrets.yaml.example $@

esp: $(ESPHOME) esp-gen $(ESP_DIR)/secrets.yaml
	$(ESPHOME) compile $(ESP_YAML)

# First time, over USB-C. Later updates go over wifi; both are `esphome run`,
# which offers the choice when no device is named.
esp-flash: $(ESPHOME) esp-gen $(ESP_DIR)/secrets.yaml
	$(ESPHOME) run $(if $(ESP_DEVICE),--device $(ESP_DEVICE)) $(ESP_YAML)

esp-ota: esp-flash

esp-logs: $(ESPHOME)
	$(ESPHOME) logs $(ESP_YAML)

esp-clean:
	rm -rf $(ESP_VENV) $(BUILD)/gen-dbg-layout $(ESP_DIR)/.esphome
	rm -f $(ESP_GEN)/socozi_layout.h $(ESP_GEN)/socozi_image.h

clean:
	rm -rf $(BUILD)

.PHONY: all reference check test test-variant coverage ci symbols backup flash \
        flash-reference restore restore-factory debug reset console clean \
        esp esp-gen esp-flash esp-ota esp-logs esp-clean ci-esp
