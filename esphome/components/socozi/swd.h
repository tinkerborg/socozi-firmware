/* Bit-banged SWD master for the ESP32-C3, plus GD32E23x flash programming.
 *
 * Two GPIOs and no peripheral. See docs/esphome-design.md §5 for why this is
 * written rather than borrowed.
 *
 * Reading the chair's state does not disturb it: the debug access port does its
 * own bus transactions and the core never notices. Halting and flashing very
 * much do, which is why everything past halt() lives behind the checks in
 * socozi.cpp rather than being callable on a whim.
 */

#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace socozi {

/* ADIv5. Only the handful this needs. */
static const uint8_t DP_IDCODE = 0x00;
static const uint8_t DP_ABORT = 0x00;
static const uint8_t DP_CTRLSTAT = 0x04;
static const uint8_t DP_SELECT = 0x08;
static const uint8_t DP_RDBUFF = 0x0C;

static const uint8_t AP_CSW = 0x00;
static const uint8_t AP_TAR = 0x04;
static const uint8_t AP_DRW = 0x0C;

/* 32-bit accesses, with and without the address auto-incrementing.
 *
 * Low byte is [5:4] AddrInc and [2:0] Size: 0x52 is word-sized with single
 * increment, 0x02 is word-sized with the address held. Size 0 is a *byte*
 * access, which is what 0x50 said by mistake — block reads still worked
 * because they use the other constant, so this only ever broke the single-word
 * path, which is every register access in the flashing sequence.
 */
static const uint32_t CSW_WORD_INC = 0x23000052;
static const uint32_t CSW_WORD_FIXED = 0x23000002;

/* Cortex-M debug, and the GD32's debug hold register. */
static const uint32_t DHCSR = 0xE000EDF0;
static const uint32_t DHCSR_KEY = 0xA05F0000;
static const uint32_t DHCSR_DEBUGEN = 1 << 0;
static const uint32_t DHCSR_HALT = 1 << 1;
static const uint32_t DHCSR_S_HALT = 1 << 17;
static const uint32_t AIRCR = 0xE000ED0C;
static const uint32_t AIRCR_SYSRESETREQ = 0x05FA0004;

/* DBG_CTL bit 8 freezes the free watchdog while the core is halted. Set-once
 * until a power cycle, and only writable over the debug port — which is the
 * whole reason a halted chair does not reset itself out from under us.
 */
static const uint32_t DBG_CTL = 0x40015804;
static const uint32_t DBG_CTL_FWDGT_HOLD = 1 << 8;

/* FMC, matching src/gd32e23x.h. Same registers our own flash.c drives. */
static const uint32_t FMC_KEY = 0x40022004;
static const uint32_t FMC_STAT = 0x4002200C;
static const uint32_t FMC_CTL = 0x40022010;
static const uint32_t FMC_ADDR = 0x40022014;
static const uint32_t FMC_KEY0 = 0x45670123;
static const uint32_t FMC_KEY1 = 0xCDEF89AB;
static const uint32_t FMC_STAT_BUSY = 1 << 0;
static const uint32_t FMC_STAT_PGERR = 1 << 2;
static const uint32_t FMC_STAT_WPERR = 1 << 4;
static const uint32_t FMC_STAT_ENDF = 1 << 5;
static const uint32_t FMC_CTL_PG = 1 << 0;
static const uint32_t FMC_CTL_PER = 1 << 1;
static const uint32_t FMC_CTL_START = 1 << 6;
static const uint32_t FMC_CTL_LK = 1 << 7;

static const uint32_t FLASH_BASE = 0x08000000;

/* The FMC's erase granularity, matching src/flash.h. 1 KiB, not the 4 KiB the
 * datasheet reading originally suggested — erasing 0x08000000 and then writing
 * 0x08000400 sets PGERR, which is how this was settled.
 */
static const uint32_t FLASH_PAGE_SIZE = 1024;

/* The settings store, src/flash.h. Heat and massage levels, lumbar firmness,
 * the four presets. Nothing here may erase at or above this address.
 */
static const uint32_t FLASH_STORE_ADDR = 0x0800F000;

class Swd {
 public:
  void set_pins(uint8_t swclk, uint8_t swdio);

  /* Half period. 1 µs is about 500 kHz once the loop overhead is counted, and
   * is comfortable for 100 Ω in series into a short cable.
   */
  void set_delay_us(uint8_t us) { this->delay_us_ = us; }

  /* Line reset, JTAG-to-SWD, read IDCODE, power up the debug domains. Safe to
   * call repeatedly; it is also the recovery path after any error.
   */
  bool begin();

  uint32_t idcode() const { return this->idcode_; }

  bool read_mem(uint32_t addr, uint32_t *dst, size_t words);
  bool write_mem(uint32_t addr, const uint32_t *src, size_t words);
  bool read_word(uint32_t addr, uint32_t *value);
  bool write_word(uint32_t addr, uint32_t value);

  bool halt();
  bool run();
  bool reset_run();

  /* Programming. Every one of these assumes the core is halted. */
  bool flash_unlock();
  bool flash_lock();
  bool flash_erase_page(uint32_t addr);
  bool flash_write_words(uint32_t addr, const uint32_t *src, size_t words);

 protected:
  /* One SWD packet. `addr` is the register offset, 0x0/0x4/0x8/0xC. */
  bool transfer_(bool ap, bool rnw, uint8_t addr, uint32_t *data);

  bool dp_read_(uint8_t addr, uint32_t *v) { return this->transfer_(false, true, addr, v); }
  bool dp_write_(uint8_t addr, uint32_t v) { return this->transfer_(false, false, addr, &v); }
  bool ap_read_(uint8_t addr, uint32_t *v) { return this->transfer_(true, true, addr, v); }
  bool ap_write_(uint8_t addr, uint32_t v) { return this->transfer_(true, false, addr, &v); }

  bool set_csw_(uint32_t csw);
  bool clear_errors_();
  bool fmc_wait_();

  void clk_high_();
  void clk_low_();
  void io_out_(bool level);
  void io_input_();
  bool io_read_();
  void half_();

  void write_bits_(uint32_t value, uint8_t count);
  uint32_t read_bits_(uint8_t count);
  void idle_(uint8_t clocks);
  void line_reset_();

  uint32_t clk_mask_{0};
  uint32_t io_mask_{0};
  uint32_t idcode_{0};
  uint32_t csw_{0};
  uint8_t delay_us_{1};
  bool ready_{false};
};

}  // namespace socozi
}  // namespace esphome

#endif  // USE_ESP32
