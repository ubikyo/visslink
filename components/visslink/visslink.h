#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

#include "read_engine.h"

namespace esphome {
namespace visslink {

/// Capteur associé à un registre de la régulation.
class VissLinkSensor : public sensor::Sensor {
 public:
  void set_address(uint16_t address) { this->address_ = address; }
  void set_length(uint8_t length) { this->length_ = length; }
  void set_signed(bool is_signed) { this->signed_ = is_signed; }
  void set_interval(uint32_t interval_ms) { this->interval_ms_ = interval_ms; }

  uint16_t get_address() const { return this->address_; }
  uint8_t get_length() const { return this->length_; }
  bool is_signed() const { return this->signed_; }
  uint32_t get_interval() const { return this->interval_ms_; }

 protected:
  uint16_t address_{0};
  uint8_t length_{2};
  bool signed_{true};
  uint32_t interval_ms_{60000};
};

/// Adaptateur entre l'UART ESPHome et l'interface SerialPort du moteur KW.
class UartSerialPort final : public SerialPort {
 public:
  explicit UartSerialPort(uart::UARTDevice *device) : device_(device) {}
  size_t available() override { return this->device_->available(); }
  bool read_byte(uint8_t *byte) override { return this->device_->read_byte(byte); }
  void write(const uint8_t *data, size_t length) override { this->device_->write_array(data, length); }

 protected:
  uart::UARTDevice *device_;
};

/// Composant principal : relie l'ordonnanceur, le moteur KW et les capteurs.
class VissLinkComponent : public Component, public uart::UARTDevice {
 public:
  VissLinkComponent() : port_(this), engine_(&port_) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void register_sensor(VissLinkSensor *sensor) { this->sensors_.push_back(sensor); }
  void set_sync_timeout(uint32_t ms) { this->engine_.protocol().set_sync_timeout(ms); }
  void set_response_timeout(uint32_t ms) { this->engine_.protocol().set_response_timeout(ms); }
  void set_max_retries(uint8_t retries) { this->engine_.scheduler().set_max_retries(retries); }

 protected:
  void handle_event_(const ReadEvent &event);

  UartSerialPort port_;
  ReadEngine engine_;
  std::vector<VissLinkSensor *> sensors_;  ///< même index que l'ordonnanceur
  ReadEvent event_;
  bool link_ok_{false};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace visslink
}  // namespace esphome
