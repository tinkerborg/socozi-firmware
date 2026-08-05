#include "swd.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <rom/ets_sys.h>
#include <soc/gpio_struct.h>

namespace esphome {
namespace socozi {

static const char *const TAG = "socozi.swd";

/* ACK, three bits, LSB first. */
static const uint8_t ACK_OK = 0x1;
static const uint8_t ACK_WAIT = 0x2;

/* A WAIT means the access port is busy, not broken, so retry. Anything else is
 * a fault and gets a line reset rather than another attempt at the same thing.
 */
static const uint8_t WAIT_RETRIES = 32;

/* Auto-increment on this access port wraps at a 1 KiB boundary rather than
 * carrying, so a block read has to re-arm TAR at every one of them. Getting
 * this wrong reads the same kilobyte over and over, which looks like plausible
 * data and is the reason it is spelled out here.
 */
static const uint32_t TAR_WRAP = 1024;

static uint8_t parity32(uint32_t v) {
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return v & 1;
}

void Swd::set_pins(uint8_t swclk, uint8_t swdio) {
  this->clk_mask_ = 1u << swclk;
  this->io_mask_ = 1u << swdio;

  /* Straight to the matrix: these are plain GPIOs and the bit-bang wants the
   * register write, not a driver call.
   */
  gpio_reset_pin(static_cast<gpio_num_t>(swclk));
  gpio_reset_pin(static_cast<gpio_num_t>(swdio));
  gpio_set_direction(static_cast<gpio_num_t>(swclk), GPIO_MODE_OUTPUT);
  gpio_set_direction(static_cast<gpio_num_t>(swdio), GPIO_MODE_INPUT_OUTPUT);

  this->clk_low_();
  this->io_out_(true);
}

void Swd::half_() { ets_delay_us(this->delay_us_); }

void Swd::clk_high_() { GPIO.out_w1ts.val = this->clk_mask_; }
void Swd::clk_low_() { GPIO.out_w1tc.val = this->clk_mask_; }

void Swd::io_out_(bool level) {
  if (level) {
    GPIO.out_w1ts.val = this->io_mask_;
  } else {
    GPIO.out_w1tc.val = this->io_mask_;
  }
  GPIO.enable_w1ts.val = this->io_mask_;
}

void Swd::io_input_() { GPIO.enable_w1tc.val = this->io_mask_; }

bool Swd::io_read_() { return (GPIO.in.val & this->io_mask_) != 0; }

/* The target samples on the rising edge and drives on the falling one, so the
 * host does the mirror of that: put the bit up, clock high, clock low.
 */
void Swd::write_bits_(uint32_t value, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    this->io_out_((value >> i) & 1);
    this->half_();
    this->clk_high_();
    this->half_();
    this->clk_low_();
  }
}

uint32_t Swd::read_bits_(uint8_t count) {
  uint32_t value = 0;

  for (uint8_t i = 0; i < count; i++) {
    this->half_();
    if (this->io_read_())
      value |= 1u << i;
    this->clk_high_();
    this->half_();
    this->clk_low_();
  }

  return value;
}

void Swd::idle_(uint8_t clocks) {
  this->io_out_(false);
  this->write_bits_(0, clocks);
}

void Swd::line_reset_() {
  this->io_out_(true);
  this->write_bits_(0xFFFFFFFF, 32);
  this->write_bits_(0xFFFFFFFF, 32);
  this->idle_(8);
}

bool Swd::begin() {
  uint32_t v = 0;

  this->ready_ = false;
  this->csw_ = 0;

  /* Line reset, the JTAG-to-SWD selection word, line reset again. The second
   * one is what the sequence is specified to end with; without it a part that
   * was already in SWD can be left mid-packet.
   */
  this->line_reset_();
  this->write_bits_(0xE79E, 16);
  this->line_reset_();

  if (!this->dp_read_(DP_IDCODE, &v) || v == 0 || v == 0xFFFFFFFF) {
    ESP_LOGW(TAG, "no IDCODE (got 0x%08X)", v);
    return false;
  }

  this->idcode_ = v;

  /* Clear whatever the last session left sticky, then bring up the debug and
   * system power domains and wait for both acknowledgements.
   */
  if (!this->clear_errors_())
    return false;

  if (!this->dp_write_(DP_SELECT, 0))
    return false;

  if (!this->dp_write_(DP_CTRLSTAT, 0x50000000))
    return false;

  for (uint8_t i = 0; i < 50; i++) {
    if (!this->dp_read_(DP_CTRLSTAT, &v))
      return false;
    if ((v & 0xA0000000) == 0xA0000000) {
      this->ready_ = true;
      return true;
    }
    delay(1);
  }

  ESP_LOGW(TAG, "debug power-up not acknowledged (CTRL/STAT 0x%08X)", v);
  return false;
}

bool Swd::clear_errors_() {
  /* STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR */
  return this->dp_write_(DP_ABORT, 0x1E);
}

bool Swd::transfer_(bool ap, bool rnw, uint8_t addr, uint32_t *data) {
  uint8_t a2 = (addr >> 2) & 1;
  uint8_t a3 = (addr >> 3) & 1;
  uint8_t p = (ap ? 1 : 0) ^ (rnw ? 1 : 0) ^ a2 ^ a3;

  uint8_t request = 0x81 | (ap ? 0x02 : 0) | (rnw ? 0x04 : 0) | (uint8_t)(a2 << 3) |
                    (uint8_t)(a3 << 4) | (uint8_t)(p << 5);

  for (uint8_t attempt = 0; attempt < WAIT_RETRIES; attempt++) {
    uint8_t ack;

    this->write_bits_(request, 8);

    /* Turnaround: release the line for a clock so the target can drive. */
    this->io_input_();
    this->half_();
    this->clk_high_();
    this->half_();
    this->clk_low_();

    ack = (uint8_t)(this->read_bits_(3) & 0x7);

    if (ack == ACK_WAIT) {
      /* Target still driving; give the line back before trying again. */
      this->half_();
      this->clk_high_();
      this->half_();
      this->clk_low_();
      this->idle_(8);
      continue;
    }

    if (ack != ACK_OK) {
      this->half_();
      this->clk_high_();
      this->half_();
      this->clk_low_();
      this->idle_(8);
      ESP_LOGW(TAG, "ack %u on %s %s 0x%X", ack, ap ? "AP" : "DP", rnw ? "read" : "write", addr);
      this->clear_errors_();
      return false;
    }

    if (rnw) {
      uint32_t value = this->read_bits_(32);
      uint8_t got = (uint8_t)(this->read_bits_(1) & 1);

      /* Turnaround back to the host. */
      this->half_();
      this->clk_high_();
      this->half_();
      this->clk_low_();
      this->idle_(8);

      if (got != parity32(value)) {
        ESP_LOGW(TAG, "parity error reading %s 0x%X", ap ? "AP" : "DP", addr);
        return false;
      }

      *data = value;
      return true;
    }

    /* Write: turnaround back to the host, then the payload. */
    this->half_();
    this->clk_high_();
    this->half_();
    this->clk_low_();

    this->write_bits_(*data, 32);
    this->write_bits_(parity32(*data), 1);
    this->idle_(8);

    return true;
  }

  ESP_LOGW(TAG, "gave up after %u WAITs", WAIT_RETRIES);
  return false;
}

bool Swd::set_csw_(uint32_t csw) {
  if (this->csw_ == csw)
    return true;
  if (!this->ap_write_(AP_CSW, csw))
    return false;
  this->csw_ = csw;
  return true;
}

bool Swd::read_mem(uint32_t addr, uint32_t *dst, size_t words) {
  size_t done = 0;

  if (!this->set_csw_(CSW_WORD_INC))
    return false;

  while (done < words) {
    /* However many words fit before the auto-increment wraps. */
    uint32_t at = addr + (uint32_t)(done * 4);
    size_t room = (TAR_WRAP - (at % TAR_WRAP)) / 4;
    size_t run = words - done;
    uint32_t discard;

    if (run > room)
      run = room;

    if (!this->ap_write_(AP_TAR, at))
      return false;

    /* AP reads are pipelined: each one returns the *previous* access's data.
     * So prime with one throwaway, then shift each result back by one, and
     * take the last word out of RDBUFF.
     */
    if (!this->ap_read_(AP_DRW, &discard))
      return false;

    for (size_t i = 0; i + 1 < run; i++) {
      if (!this->ap_read_(AP_DRW, &dst[done + i]))
        return false;
    }

    if (!this->dp_read_(DP_RDBUFF, &dst[done + run - 1]))
      return false;

    done += run;
  }

  return true;
}

bool Swd::write_mem(uint32_t addr, const uint32_t *src, size_t words) {
  size_t done = 0;

  if (!this->set_csw_(CSW_WORD_INC))
    return false;

  while (done < words) {
    uint32_t at = addr + (uint32_t)(done * 4);
    size_t room = (TAR_WRAP - (at % TAR_WRAP)) / 4;
    size_t run = words - done;

    if (run > room)
      run = room;

    if (!this->ap_write_(AP_TAR, at))
      return false;

    for (size_t i = 0; i < run; i++) {
      if (!this->ap_write_(AP_DRW, src[done + i]))
        return false;
    }

    done += run;
  }

  return true;
}

bool Swd::read_word(uint32_t addr, uint32_t *value) {
  if (!this->set_csw_(CSW_WORD_FIXED))
    return false;
  if (!this->ap_write_(AP_TAR, addr))
    return false;

  uint32_t discard;
  if (!this->ap_read_(AP_DRW, &discard))
    return false;

  return this->dp_read_(DP_RDBUFF, value);
}

bool Swd::write_word(uint32_t addr, uint32_t value) {
  if (!this->set_csw_(CSW_WORD_FIXED))
    return false;
  if (!this->ap_write_(AP_TAR, addr))
    return false;
  return this->ap_write_(AP_DRW, value);
}

bool Swd::halt() {
  uint32_t v = 0;

  if (!this->write_word(DHCSR, DHCSR_KEY | DHCSR_DEBUGEN | DHCSR_HALT))
    return false;

  for (uint8_t i = 0; i < 50; i++) {
    if (!this->read_word(DHCSR, &v))
      return false;
    if (v & DHCSR_S_HALT)
      return true;
    delay(1);
  }

  ESP_LOGW(TAG, "core did not halt (DHCSR 0x%08X)", v);
  return false;
}

bool Swd::run() { return this->write_word(DHCSR, DHCSR_KEY); }

bool Swd::reset_run() {
  /* Ask for a system reset, then let go of the core. The reset clears
   * DHCSR itself, so the order only matters in that the request has to be the
   * last thing this link does while it still owns the part.
   */
  this->write_word(AIRCR, AIRCR_SYSRESETREQ);
  delay(50);

  /* The link is gone with the reset; a fresh handshake is how it comes back. */
  this->begin();
  return this->run();
}

bool Swd::flash_unlock() {
  uint32_t ctl = 0;

  if (!this->read_word(FMC_CTL, &ctl))
    return false;

  if (ctl & FMC_CTL_LK) {
    if (!this->write_word(FMC_KEY, FMC_KEY0) || !this->write_word(FMC_KEY, FMC_KEY1))
      return false;
    if (!this->read_word(FMC_CTL, &ctl))
      return false;
    if (ctl & FMC_CTL_LK) {
      ESP_LOGE(TAG, "FMC refused to unlock");
      return false;
    }
  }

  return true;
}

bool Swd::flash_lock() {
  uint32_t ctl = 0;
  if (!this->read_word(FMC_CTL, &ctl))
    return false;
  return this->write_word(FMC_CTL, ctl | FMC_CTL_LK);
}

/* Wait out the controller, then take and clear whatever it is reporting. Mirror
 * of finish() in src/flash.c, including the write-one-to-clear of ENDF, so the
 * next operation starts from a clean status rather than this one's leftovers.
 */
bool Swd::fmc_wait_() {
  uint32_t stat = 0;

  /* The datasheet's worst figure is 42 ms; this is a backstop for a controller
   * that never finished, not a duration anything is expected to spend here.
   */
  for (uint16_t i = 0; i < 500; i++) {
    if (!this->read_word(FMC_STAT, &stat))
      return false;
    if (!(stat & FMC_STAT_BUSY))
      break;
    delay(1);
  }

  if (stat & FMC_STAT_BUSY) {
    ESP_LOGE(TAG, "FMC still busy");
    return false;
  }

  bool ok = !(stat & (FMC_STAT_PGERR | FMC_STAT_WPERR));

  this->write_word(FMC_STAT, FMC_STAT_PGERR | FMC_STAT_WPERR | FMC_STAT_ENDF);

  if (!ok)
    ESP_LOGE(TAG, "FMC error, STAT 0x%08X", stat);

  return ok;
}

bool Swd::flash_erase_page(uint32_t addr) {
  uint32_t ctl = 0;
  uint32_t check = 0;

  /* The one check that matters. Above this line is the settings store, and an
   * erase there loses the presets and every remembered level.
   */
  if (addr < FLASH_BASE || addr >= FLASH_STORE_ADDR) {
    ESP_LOGE(TAG, "refusing to erase 0x%08X", addr);
    return false;
  }

  if (!this->read_word(FMC_CTL, &ctl))
    return false;

  if (!this->write_word(FMC_CTL, ctl | FMC_CTL_PER))
    return false;
  if (!this->write_word(FMC_ADDR, addr))
    return false;
  if (!this->write_word(FMC_CTL, ctl | FMC_CTL_PER | FMC_CTL_START))
    return false;

  bool ok = this->fmc_wait_();

  this->write_word(FMC_CTL, ctl & ~FMC_CTL_PER);

  if (!ok)
    return false;

  if (!this->read_word(addr, &check))
    return false;

  if (check != 0xFFFFFFFF) {
    ESP_LOGE(TAG, "page 0x%08X did not erase", addr);
    return false;
  }

  return true;
}

bool Swd::flash_write_words(uint32_t addr, const uint32_t *src, size_t words) {
  uint32_t ctl = 0;

  if (addr < FLASH_BASE || addr + words * 4 > FLASH_STORE_ADDR) {
    ESP_LOGE(TAG, "refusing to write %u words at 0x%08X", (unsigned) words, addr);
    return false;
  }

  if (!this->read_word(FMC_CTL, &ctl))
    return false;

  if (!this->write_word(FMC_CTL, ctl | FMC_CTL_PG))
    return false;

  bool ok = true;

  for (size_t i = 0; i < words && ok; i++) {
    /* An erased word is already what we want. Skipping it saves the write and
     * the status poll, and on an image with any run of padding that is most of
     * the time this loop would otherwise spend.
     */
    if (src[i] == 0xFFFFFFFF)
      continue;

    ok = this->write_word(addr + (uint32_t)(i * 4), src[i]) && this->fmc_wait_();

    if ((i & 0x3F) == 0) {
      App.feed_wdt();
    }
  }

  this->write_word(FMC_CTL, ctl & ~FMC_CTL_PG);

  return ok;
}

}  // namespace socozi
}  // namespace esphome

#endif  // USE_ESP32
