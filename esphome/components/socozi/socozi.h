/* The chair, in Home Assistant.
 *
 * An ESP32-C3 on the chair's SWD pins reads the firmware's debug block once a
 * second and publishes it. Nothing is halted and nothing is parsed: the block
 * is a plain struct pinned to the base of SRAM, and the offsets come from the
 * firmware build itself, in socozi_layout.h. See
 * docs/esphome-design.md.
 *
 * Read-only. Pressing buttons from Home Assistant needs a writable block in
 * RAM and a different safety argument; it is deliberately not here.
 *
 * The exception is the update entity, which halts the chair and reprograms it.
 * That path checks that the chair is idle first and, whatever happens, always
 * ends by resetting it and letting it run.
 */

#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "swd.h"

#include "socozi_layout.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_UPDATE
#include "esphome/components/update/update_entity.h"
#endif

#include <vector>

namespace esphome {
namespace socozi {

/* Everything the chair can be asked for. Which of these are legal for a given
 * platform is enforced in the Python, so a numeric value cannot end up on a
 * binary sensor.
 */
enum SocoziValue : uint8_t {
  /* Binary */
  SOCOZI_POWER,
  SOCOZI_HEAT,
  SOCOZI_MASSAGE,
  SOCOZI_LUMBAR,
  SOCOZI_MOVING,
  SOCOZI_LINK,
  SOCOZI_AT_HOME,

  /* Numeric */
  SOCOZI_HEAT_LEVEL,
  SOCOZI_HEAT_REMAINING,
  SOCOZI_MASSAGE_LEVEL,
  SOCOZI_MASSAGE_REMAINING,
  SOCOZI_LUMBAR_LEVEL,
  SOCOZI_RECLINE,
  SOCOZI_HEADREST,
  SOCOZI_RECLINE_MS,
  SOCOZI_HEADREST_MS,
  SOCOZI_CURRENT,
  SOCOZI_VALVES,
  SOCOZI_MASSAGE_STEP,
  SOCOZI_INPUTS,
  SOCOZI_LOOP_TICKS,
  SOCOZI_HANDSET_FRAMES,
  SOCOZI_HANDSET_ERRORS,
  SOCOZI_PRESSES,
  SOCOZI_STOPS,
  SOCOZI_STALLS,
  SOCOZI_ARRIVALS,
  SOCOZI_AUTO_MOVES,
  SOCOZI_PRESETS_SAVED,
  SOCOZI_PRESETS_RECALLED,
  SOCOZI_SETTINGS_WRITES,
  SOCOZI_SETTINGS_ERASES,
  SOCOZI_SETTINGS_ERRORS,

  /* Text */
  SOCOZI_MOTION,
  SOCOZI_VERSION,
};

class SocoziComponent : public PollingComponent {
 public:
  void set_pins(uint8_t swclk, uint8_t swdio) { this->swd_.set_pins(swclk, swdio); }
  void set_clock_delay(uint8_t us) { this->swd_.set_delay_us(us); }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  /* True when the last poll came back with the magic word in place. */
  bool link_ok() const { return this->link_ok_; }

  /* The chair's build stamp, or 0 when the link is down. */
  uint32_t chair_version() const;

  /* Raw word out of the last good read. Index is a SOCOZI_OFF_* constant. */
  uint32_t word(size_t index) const { return this->block_[index]; }

  Swd &swd() { return this->swd_; }

  /* Nothing may halt the chair while it is doing something. */
  bool safe_to_flash() const;

#ifdef USE_SENSOR
  void add_sensor(SocoziValue value, sensor::Sensor *s) { this->sensors_.push_back({value, s, NAN}); }
#endif
#ifdef USE_BINARY_SENSOR
  void add_binary_sensor(SocoziValue value, binary_sensor::BinarySensor *s) {
    this->binary_sensors_.push_back({value, s});
  }
#endif
#ifdef USE_TEXT_SENSOR
  void add_text_sensor(SocoziValue value, text_sensor::TextSensor *s) {
    this->text_sensors_.push_back({value, s, ""});
  }
#endif

 protected:
  float numeric_(SocoziValue value) const;
  bool boolean_(SocoziValue value) const;
  std::string text_(SocoziValue value) const;
  void publish_();

  Swd swd_;
  uint32_t block_[SOCOZI_DBG_WORDS]{};
  bool link_ok_{false};
  bool primed_{false};

#ifdef USE_SENSOR
  struct SensorEntry {
    SocoziValue value;
    sensor::Sensor *sensor;
    float last;
  };
  std::vector<SensorEntry> sensors_;
#endif
#ifdef USE_BINARY_SENSOR
  struct BinaryEntry {
    SocoziValue value;
    binary_sensor::BinarySensor *sensor;
  };
  std::vector<BinaryEntry> binary_sensors_;
#endif
#ifdef USE_TEXT_SENSOR
  struct TextEntry {
    SocoziValue value;
    text_sensor::TextSensor *sensor;
    std::string last;
  };
  std::vector<TextEntry> text_sensors_;
#endif
};

/* Human-readable form of a build stamp. See src/version.h for the encoding. */
std::string socozi_version_string(uint32_t version);

#ifdef USE_UPDATE

/* The chair's firmware, as a Home Assistant update entity.
 *
 * The image is compiled into this binary, so one ESPHome OTA delivers both
 * halves and there is one firmware for the device. What is left is deciding
 * whether the chair is running it, and installing it if not.
 */
class SocoziUpdate : public update::UpdateEntity, public Component, public Parented<SocoziComponent> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void check() override;
  void perform(bool force) override;

  void set_auto_install(bool enable) { this->auto_install_ = enable; }

 protected:
  bool install_();
  void set_progress_(float percent);

  bool pending_{false};
  bool auto_install_{false};
  float last_progress_{0.0f};
};

#endif  // USE_UPDATE

}  // namespace socozi
}  // namespace esphome

#endif  // USE_ESP32
