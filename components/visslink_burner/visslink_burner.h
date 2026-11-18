#pragma once

// Diagnostic des cycles du brûleur (v0.8.1). Colle ESPHome autour de
// CounterHistory / BurnerCycleTracker (visslink/burner_cycles.h, testés sur
// PC). N'émet AUCUNE lecture Optolink : observe une fois par seconde les
// lectures réussies des entités existantes (brûleur 0x0842, compteurs
// 0x088A et 0x08A7, température chaudière 0x0802) et publie des capteurs.

#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/visslink/burner_cycles.h"
#include "esphome/components/visslink/visslink.h"
#include "esphome/core/component.h"

namespace esphome {
namespace visslink_burner {

class VissLinkBurnerCycles : public Component {
 public:
  void set_burner(visslink::VissLinkBinarySensor *s) { this->burner_ = s; }
  void set_starts(visslink::VissLinkSensor *s) { this->starts_ = s; }
  void set_runtime(visslink::VissLinkSensor *s) { this->runtime_ = s; }
  void set_temperature(visslink::VissLinkSensor *s) { this->temperature_ = s; }

  void set_starts_24h_sensor(sensor::Sensor *s) { this->starts_24h_ = s; }
  void set_starts_per_hour_sensor(sensor::Sensor *s) { this->starts_per_hour_ = s; }
  void set_average_runtime_sensor(sensor::Sensor *s) { this->average_runtime_ = s; }
  void set_last_cycle_sensor(sensor::Sensor *s) { this->last_cycle_ = s; }
  void set_last_stop_sensor(sensor::Sensor *s) { this->last_stop_ = s; }
  void set_start_temperature_sensor(sensor::Sensor *s) { this->start_temperature_ = s; }
  void set_stop_temperature_sensor(sensor::Sensor *s) { this->stop_temperature_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_ = s; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  void update_();
  void observe_burner_();
  void observe_temperature_();
  void observe_counter_(visslink::VissLinkSensor *sensor, uint32_t *seen, visslink::CounterHistory *history,
                        const char *what);
  void publish_counters_(uint32_t now);
  void publish_status_(const visslink::BurnerCounterStats &stats);
  static void publish_if_changed_(sensor::Sensor *sensor, float value);

  visslink::VissLinkBinarySensor *burner_{nullptr};
  visslink::VissLinkSensor *starts_{nullptr};
  visslink::VissLinkSensor *runtime_{nullptr};
  visslink::VissLinkSensor *temperature_{nullptr};

  sensor::Sensor *starts_24h_{nullptr};
  sensor::Sensor *starts_per_hour_{nullptr};
  sensor::Sensor *average_runtime_{nullptr};
  sensor::Sensor *last_cycle_{nullptr};
  sensor::Sensor *last_stop_{nullptr};
  sensor::Sensor *start_temperature_{nullptr};
  sensor::Sensor *stop_temperature_{nullptr};
  text_sensor::TextSensor *status_{nullptr};

  visslink::BurnerCycleTracker tracker_;
  visslink::CounterHistory starts_history_;
  visslink::CounterHistory runtime_history_;
  uint32_t burner_seen_{0};
  uint32_t temperature_seen_{0};
  uint32_t starts_seen_{0};
  uint32_t runtime_seen_{0};
  float last_uncertainty_s_{NAN};  ///< incertitude du dernier cycle publié
  std::string last_status_;
};

}  // namespace visslink_burner
}  // namespace esphome
