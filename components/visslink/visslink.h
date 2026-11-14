#pragma once

#include <vector>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "kw_protocol.h"
#include "scheduler.h"

namespace esphome {
namespace visslink {

/// Température lue dans un registre de 2 octets (entier signé, dixièmes de
/// degré convertis par le filtre `multiply: 0.1` du YAML).
class VissLinkSensor : public sensor::Sensor {
 public:
  static constexpr uint8_t LENGTH = 2;

  void set_address(uint16_t address) { this->address_ = address; }
  void set_interval(uint32_t interval_ms) { this->interval_ms_ = interval_ms; }
  uint16_t get_address() const { return this->address_; }
  uint32_t get_interval() const { return this->interval_ms_; }

 protected:
  uint16_t address_{0};
  uint32_t interval_ms_{60000};
};

/// Relie l'ordonnanceur, le protocole KW et les capteurs.
class VissLinkComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void register_sensor(VissLinkSensor *sensor) { this->sensors_.push_back(sensor); }
  void set_sync_timeout(uint32_t ms) { this->kw_.set_sync_timeout(ms); }
  void set_response_timeout(uint32_t ms) { this->kw_.set_response_timeout(ms); }
  void set_max_retries(uint8_t retries) { this->scheduler_.set_max_retries(retries); }

 protected:
  void handle_result_(KwResult result, uint32_t now);

  KwProtocol kw_;
  RegisterScheduler scheduler_;
  std::vector<VissLinkSensor *> sensors_;  ///< même index que l'ordonnanceur
  bool link_ok_{false};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace visslink
}  // namespace esphome
