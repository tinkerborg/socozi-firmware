#include "socozi.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

#ifdef USE_UPDATE
#include "socozi_image.h"
#endif

namespace esphome {
namespace socozi {

static const char *const TAG = "socozi";

#define W(name) this->block_[SOCOZI_OFF_##name]

/* dbg.motion, matching the MOTION_* enum in src/debug.h. */
static const char *const MOTION_NAMES[] = {
    "idle", "recline_up", "recline_down", "headrest_up", "headrest_down", "flatten",
};

static float percent_of(uint32_t value, uint32_t full) {
  if (full == 0)
    return NAN;
  float pct = 100.0f * (float) value / (float) full;
  return pct > 100.0f ? 100.0f : pct;
}

static float minutes_left(uint32_t elapsed_ms, uint32_t limit_ms) {
  if (elapsed_ms >= limit_ms)
    return 0.0f;
  return (float) (limit_ms - elapsed_ms) / 60000.0f;
}

std::string socozi_version_string(uint32_t version) {
  char buf[24];

  if (version == 0)
    return "unknown";

  /* Bit 31 set means a build from a dirty tree, stamped with its build time
   * rather than a commit, because two different working trees at the same
   * commit must not compare equal. See src/version.h.
   */
  if (version & 0x80000000u) {
    snprintf(buf, sizeof(buf), "dev-%u", (unsigned) (version & 0x7FFFFFFFu));
  } else {
    snprintf(buf, sizeof(buf), "%07x", (unsigned) version);
  }

  return buf;
}

void SocoziComponent::setup() {
  ESP_LOGCONFIG(TAG, "Bringing up the SWD link...");

  if (!this->swd_.begin()) {
    /* Not a setup failure. The chair may simply be unpowered, and the poll
     * retries the handshake every time, so it joins when it wakes up.
     */
    ESP_LOGW(TAG, "chair did not answer; will keep trying");
  }
}

void SocoziComponent::update() {
  if (this->swd_.idcode() == 0 && !this->swd_.begin()) {
    this->link_ok_ = false;
    this->publish_();
    return;
  }

  uint32_t block[SOCOZI_DBG_WORDS];

  if (!this->swd_.read_mem(SOCOZI_DBG_BASE, block, SOCOZI_DBG_WORDS)) {
    /* Force a fresh handshake next time rather than retrying into a link that
     * has already said no.
     */
    this->swd_.begin();
    this->link_ok_ = false;
    this->publish_();
    return;
  }

  if (block[SOCOZI_OFF_magic] != SOCOZI_DBG_MAGIC) {
    ESP_LOGW(TAG, "no debug block at 0x%08X (magic 0x%08X)", SOCOZI_DBG_BASE, block[SOCOZI_OFF_magic]);
    this->link_ok_ = false;
    this->publish_();
    return;
  }

  memcpy(this->block_, block, sizeof(block));
  this->link_ok_ = true;
  this->publish_();
}

uint32_t SocoziComponent::chair_version() const {
#ifdef SOCOZI_OFF_version
  return this->link_ok_ ? this->block_[SOCOZI_OFF_version] : 0;
#else
  return 0;
#endif
}

bool SocoziComponent::safe_to_flash() const {
  if (!this->link_ok_)
    return false;

  return W(motion) == 0 && W(heat_on) == 0 && W(massage_on) == 0;
}

bool SocoziComponent::boolean_(SocoziValue value) const {
  switch (value) {
    case SOCOZI_LINK:
      return this->link_ok_;
    case SOCOZI_POWER:
      return W(power_on) != 0;
    case SOCOZI_HEAT:
      return W(heat_on) != 0;
    case SOCOZI_MASSAGE:
      return W(massage_on) != 0;
    case SOCOZI_LUMBAR:
      return W(lumbar_state) != 0;
    case SOCOZI_MOVING:
      return W(motion) != 0;
#if defined(SOCOZI_OFF_pos_recline) && defined(SOCOZI_HOME_MS)
    case SOCOZI_AT_HOME:
      return W(pos_recline) < SOCOZI_HOME_MS && W(pos_headrest) < SOCOZI_HOME_MS;
#endif
    default:
      return false;
  }
}

float SocoziComponent::numeric_(SocoziValue value) const {
  switch (value) {
    case SOCOZI_CURRENT:
      /* A failed conversion reads all ones; that is missing data, not a value. */
      return W(adc) == 0xFFFFFFFF ? NAN : (float) W(adc);
    case SOCOZI_VALVES:
      return (float) W(valves);
    case SOCOZI_MASSAGE_STEP:
      return (float) W(massage_step);
    case SOCOZI_INPUTS:
      return (float) W(inputs);
    case SOCOZI_LOOP_TICKS:
      return (float) W(ticks);
    case SOCOZI_HANDSET_FRAMES:
      return (float) W(hs_frames);
    case SOCOZI_HANDSET_ERRORS:
      return (float) W(hs_errors);
    case SOCOZI_PRESSES:
      return (float) W(presses);
    case SOCOZI_STOPS:
      return (float) W(stops);
    case SOCOZI_STALLS:
      return (float) W(stalls);
    case SOCOZI_HEAT_REMAINING:
      return W(heat_on) ? minutes_left(W(heat_ms), SOCOZI_HEAT_MAX_MS) : 0.0f;
    case SOCOZI_MASSAGE_REMAINING:
      return W(massage_on) ? minutes_left(W(massage_ms), SOCOZI_MASSAGE_MAX_MS) : 0.0f;
#ifdef SOCOZI_OFF_heat_level
    case SOCOZI_HEAT_LEVEL:
      return (float) W(heat_level);
#endif
#ifdef SOCOZI_OFF_massage_level
    case SOCOZI_MASSAGE_LEVEL:
      return (float) W(massage_level);
#endif
#ifdef SOCOZI_OFF_lumbar_level
    case SOCOZI_LUMBAR_LEVEL:
      /* Stored in 100 ms units of inflation, against the factory table's
       * ceiling on a single inflation.
       */
      return percent_of(W(lumbar_level) * SOCOZI_LUMBAR_UNIT_MS, SOCOZI_LUMBAR_MAX_MS);
#endif
#ifdef SOCOZI_OFF_pos_recline
    case SOCOZI_RECLINE:
      return percent_of(W(pos_recline), SOCOZI_TRAVEL_RECLINE_MS);
    case SOCOZI_HEADREST:
      return percent_of(W(pos_headrest), SOCOZI_TRAVEL_HEADREST_MS);
    case SOCOZI_RECLINE_MS:
      return (float) W(pos_recline);
    case SOCOZI_HEADREST_MS:
      return (float) W(pos_headrest);
#endif
#ifdef SOCOZI_OFF_arrivals
    case SOCOZI_ARRIVALS:
      return (float) W(arrivals);
#endif
#ifdef SOCOZI_OFF_auto_moves
    case SOCOZI_AUTO_MOVES:
      return (float) W(auto_moves);
#endif
#ifdef SOCOZI_OFF_presets_saved
    case SOCOZI_PRESETS_SAVED:
      return (float) W(presets_saved);
    case SOCOZI_PRESETS_RECALLED:
      return (float) W(presets_recalled);
#endif
#ifdef SOCOZI_OFF_settings_writes
    case SOCOZI_SETTINGS_WRITES:
      return (float) W(settings_writes);
    case SOCOZI_SETTINGS_ERASES:
      return (float) W(settings_erases);
    case SOCOZI_SETTINGS_ERRORS:
      return (float) W(settings_errors);
#endif
    default:
      return NAN;
  }
}

std::string SocoziComponent::text_(SocoziValue value) const {
  switch (value) {
    case SOCOZI_MOTION: {
      uint32_t m = W(motion);
      return m < (sizeof(MOTION_NAMES) / sizeof(MOTION_NAMES[0])) ? MOTION_NAMES[m] : "unknown";
    }
    case SOCOZI_VERSION:
      return socozi_version_string(this->chair_version());
    default:
      return "";
  }
}

/* Publishes only what changed. At a second per poll and this many entities,
 * republishing everything would be most of the API traffic on the device for
 * no new information.
 */
void SocoziComponent::publish_() {
  bool first = !this->primed_;

  this->primed_ = true;

#ifdef USE_BINARY_SENSOR
  for (auto &e : this->binary_sensors_) {
    /* A down link is not "off". Anything but the link sensor itself goes
     * unavailable rather than reporting a stale or invented state.
     */
    if (!this->link_ok_ && e.value != SOCOZI_LINK) {
      if (first)
        e.sensor->publish_state(false);
      continue;
    }
    e.sensor->publish_state(this->boolean_(e.value));
  }
#endif

#ifdef USE_SENSOR
  for (auto &e : this->sensors_) {
    float v = this->link_ok_ ? this->numeric_(e.value) : NAN;

    if (!first && (v == e.last || (std::isnan(v) && std::isnan(e.last))))
      continue;

    e.last = v;
    e.sensor->publish_state(v);
  }
#endif

#ifdef USE_TEXT_SENSOR
  for (auto &e : this->text_sensors_) {
    std::string v = this->link_ok_ ? this->text_(e.value) : "offline";

    if (!first && v == e.last)
      continue;

    e.last = v;
    e.sensor->publish_state(v);
  }
#endif
}

void SocoziComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SoCozi chair:");
  ESP_LOGCONFIG(TAG, "  Debug block: 0x%08X, %u words", SOCOZI_DBG_BASE, (unsigned) SOCOZI_DBG_WORDS);
  ESP_LOGCONFIG(TAG, "  SWD IDCODE: 0x%08X", this->swd_.idcode());
  ESP_LOGCONFIG(TAG, "  Link: %s", this->link_ok_ ? "up" : "down");
  ESP_LOGCONFIG(TAG, "  Chair firmware: %s", socozi_version_string(this->chair_version()).c_str());
  LOG_UPDATE_INTERVAL(this);
}

#ifdef USE_UPDATE

/* One page at a time, and one kilobyte at a time within it, so nothing large
 * lands on the stack of a task that also has to keep wifi alive.
 */
static const size_t CHUNK_WORDS = 256;

void SocoziUpdate::setup() {
  this->update_info_.title = "SoCozi chair firmware";
  this->update_info_.latest_version = socozi_version_string(SOCOZI_IMAGE_VERSION);
  this->update_info_.current_version = "unknown";
  this->state_ = update::UPDATE_STATE_UNKNOWN;
  this->publish_state();
}

/* Called from loop(), so it has to stay silent unless something moved.
 * Publishing on every pass is what turned the boot log into a wall.
 */
void SocoziUpdate::check() {
  if (this->state_ == update::UPDATE_STATE_INSTALLING)
    return;

  update::UpdateState want;
  std::string current;
  std::string summary;

  if (!this->parent_->link_ok()) {
    current = "offline";
    summary = "The chair is not answering on the debug link.";
    want = update::UPDATE_STATE_UNKNOWN;
  } else {
    uint32_t running = this->parent_->chair_version();

    current = socozi_version_string(running);

    if (running == SOCOZI_IMAGE_VERSION) {
      summary = "The chair is running this build.";
      want = update::UPDATE_STATE_NO_UPDATE;
    } else {
      summary = "The chair is running a different build than the one on this device.";
      want = update::UPDATE_STATE_AVAILABLE;
    }
  }

  if (want == this->state_ && current == this->update_info_.current_version)
    return;

  this->update_info_.current_version = current;
  this->update_info_.summary = summary;
  this->update_info_.has_progress = false;
  this->state_ = want;

  this->publish_state();
}

void SocoziUpdate::perform(bool force) {
  if (this->state_ == update::UPDATE_STATE_INSTALLING)
    return;

  if (!force && this->state_ != update::UPDATE_STATE_AVAILABLE) {
    ESP_LOGW(TAG, "nothing to install");
    return;
  }

  this->pending_ = true;
}

void SocoziUpdate::loop() {
  this->check();

  if (this->auto_install_ && this->state_ == update::UPDATE_STATE_AVAILABLE && !this->pending_ &&
      this->parent_->safe_to_flash()) {
    ESP_LOGI(TAG, "auto-install: chair is idle and out of date");
    this->pending_ = true;
  }

  if (!this->pending_)
    return;

  /* Wait for a quiet chair rather than refusing. Somebody who pressed Install
   * while the massage was running meant it; they just have to stop massaging.
   */
  if (!this->parent_->safe_to_flash()) {
    static uint32_t last_log = 0;
    if (millis() - last_log > 10000) {
      last_log = millis();
      ESP_LOGI(TAG, "waiting for the chair to be idle before flashing");
    }
    return;
  }

  this->pending_ = false;

  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->update_info_.has_progress = true;
  this->update_info_.progress = 0.0f;
  this->last_progress_ = 0.0f;
  this->publish_state();

  bool ok = this->install_();

  /* Back to UNKNOWN and let the next check() decide from what the chair now
   * reports, rather than asserting the install worked.
   */
  this->update_info_.has_progress = false;
  this->update_info_.current_version = "";
  this->state_ = update::UPDATE_STATE_UNKNOWN;
  this->publish_state();

  ESP_LOGI(TAG, "install %s", ok ? "succeeded" : "FAILED");
}

/* Every publish_state() makes the update component log its whole info struct,
 * so publishing once per page turned a ten-second install into pages of log.
 * Coarse steps only; nobody is reading a progress bar to the percent.
 */
static const float PROGRESS_STEP = 10.0f;

void SocoziUpdate::set_progress_(float percent) {
  App.feed_wdt();

  if (percent < 100.0f && percent - this->last_progress_ < PROGRESS_STEP)
    return;

  this->last_progress_ = percent;
  this->update_info_.progress = percent;
  this->publish_state();
}

bool SocoziUpdate::install_() {
  Swd &swd = this->parent_->swd();
  const size_t total_words = (SOCOZI_IMAGE_SIZE + 3) / 4;
  bool ok = false;

  ESP_LOGI(TAG, "installing %u bytes, version %s", (unsigned) SOCOZI_IMAGE_SIZE,
           socozi_version_string(SOCOZI_IMAGE_VERSION).c_str());

  do {
    uint32_t dbg_ctl = 0;

    if (!swd.begin() || !swd.halt())
      break;

    /* Freeze the free watchdog while the core is halted, or it resets the
     * chair out from under the programming run. Set-once until a power cycle
     * and writable only over this link.
     */
    if (!swd.read_word(DBG_CTL, &dbg_ctl) || !swd.write_word(DBG_CTL, dbg_ctl | DBG_CTL_FWDGT_HOLD))
      break;

    if (!swd.flash_unlock())
      break;

    /* Erase only the 1 KiB pages the image covers. flash_erase_page refuses
     * anything at or above the settings store regardless, which is what keeps
     * the presets and the remembered levels through a reflash.
     */
    size_t pages = (SOCOZI_IMAGE_SIZE + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    bool failed = false;

    for (size_t p = 0; p < pages; p++) {
      if (!swd.flash_erase_page(FLASH_BASE + (uint32_t) (p * FLASH_PAGE_SIZE))) {
        failed = true;
        break;
      }
      this->set_progress_(10.0f * (float) (p + 1) / (float) pages);
    }

    if (failed)
      break;

    uint32_t chunk[CHUNK_WORDS];
    size_t done = 0;

    while (done < total_words) {
      size_t run = total_words - done;
      if (run > CHUNK_WORDS)
        run = CHUNK_WORDS;

      /* Through memcpy, and padded with erased bytes: the image length is not
       * obliged to be a multiple of four, and the array is bytes.
       */
      memset(chunk, 0xFF, sizeof(chunk));
      size_t offset = done * 4;
      size_t bytes = run * 4;
      if (offset + bytes > SOCOZI_IMAGE_SIZE)
        bytes = SOCOZI_IMAGE_SIZE - offset;
      memcpy(chunk, &SOCOZI_IMAGE[offset], bytes);

      if (!swd.flash_write_words(FLASH_BASE + (uint32_t) offset, chunk, run)) {
        failed = true;
        break;
      }

      done += run;
      this->set_progress_(10.0f + 70.0f * (float) done / (float) total_words);
    }

    if (failed)
      break;

    swd.flash_lock();

    /* Read it back rather than trust it. */
    uint32_t got[CHUNK_WORDS];
    done = 0;

    while (done < total_words) {
      size_t run = total_words - done;
      if (run > CHUNK_WORDS)
        run = CHUNK_WORDS;

      memset(chunk, 0xFF, sizeof(chunk));
      size_t offset = done * 4;
      size_t bytes = run * 4;
      if (offset + bytes > SOCOZI_IMAGE_SIZE)
        bytes = SOCOZI_IMAGE_SIZE - offset;
      memcpy(chunk, &SOCOZI_IMAGE[offset], bytes);

      if (!swd.read_mem(FLASH_BASE + (uint32_t) offset, got, run) ||
          memcmp(chunk, got, run * 4) != 0) {
        ESP_LOGE(TAG, "verify failed at 0x%08X", (unsigned) (FLASH_BASE + offset));
        failed = true;
        break;
      }

      done += run;
      this->set_progress_(80.0f + 20.0f * (float) done / (float) total_words);
    }

    ok = !failed;
  } while (false);

  /* Whatever happened. A halted chair is the one outcome that must never
   * outlive this function: somebody may be sitting in it.
   */
  swd.flash_lock();
  swd.reset_run();

  return ok;
}

void SocoziUpdate::dump_config() {
  ESP_LOGCONFIG(TAG, "SoCozi chair update:");
  ESP_LOGCONFIG(TAG, "  Bundled image: %u bytes, version %s", (unsigned) SOCOZI_IMAGE_SIZE,
                socozi_version_string(SOCOZI_IMAGE_VERSION).c_str());
  ESP_LOGCONFIG(TAG, "  Auto install: %s", YESNO(this->auto_install_));
}

#endif  // USE_UPDATE

}  // namespace socozi
}  // namespace esphome

#endif  // USE_ESP32
