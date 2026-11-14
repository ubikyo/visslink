#pragma once

#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "decode.h"
#include "kw_protocol.h"
#include "scheduler.h"

namespace esphome {
namespace visslink {

/// Registre lu périodiquement : ce que partagent toutes les plateformes.
/// Le composant ne connaît que cette interface ; chaque plateforme décode et
/// publie à sa façon.
class VissLinkRegister {
 public:
  void set_address(uint16_t address) { this->address_ = address; }
  void set_interval(uint32_t interval_ms) { this->interval_ms_ = interval_ms; }
  uint16_t get_address() const { return this->address_; }
  uint32_t get_interval() const { return this->interval_ms_; }

  /// Nombre d'octets à lire.
  virtual uint8_t get_length() const = 0;
  /// Publie la valeur reçue (`get_length()` octets).
  virtual void publish_data(const uint8_t *data) = 0;
  /// Lecture abandonnée pour ce cycle : l'ancienne valeur n'est plus fiable.
  virtual void publish_unknown() = 0;
  virtual void dump_register_config() = 0;

 protected:
  uint16_t address_{0};
  uint32_t interval_ms_{60000};
};

#ifdef USE_SENSOR
/// Valeur numérique brute (aucune échelle : filtres `multiply` du YAML).
class VissLinkSensor : public sensor::Sensor, public VissLinkRegister {
 public:
  void set_data_type(DataType type) { this->data_type_ = type; }
  DataType get_data_type() const { return this->data_type_; }

  uint8_t get_length() const override { return data_type_length(this->data_type_); }
  void publish_data(const uint8_t *data) override;
  void publish_unknown() override;
  void dump_register_config() override;

 protected:
  DataType data_type_{DataType::INT16};  ///< défaut v0.1 : température
};
#endif

#ifdef USE_BINARY_SENSOR
/// État binaire sur 1 octet : 0x00 = off, 0x01 = on, autre = inconnu.
class VissLinkBinarySensor : public binary_sensor::BinarySensor, public VissLinkRegister {
 public:
  uint8_t get_length() const override { return 1; }
  void publish_data(const uint8_t *data) override;
  void publish_unknown() override;
  void dump_register_config() override;
};
#endif

/// Relie l'ordonnanceur, le protocole KW et les registres.
class VissLinkComponent : public Component, public uart::UARTDevice {
 public:
  /// Écart entre les premières échéances de deux registres consécutifs
  /// (période nominale du 0x05, donc durée minimale d'une lecture).
  static constexpr uint32_t STARTUP_STAGGER_MS = 2000;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void add_register(VissLinkRegister *reg) { this->registers_.push_back(reg); }
  void set_sync_timeout(uint32_t ms) { this->kw_.set_sync_timeout(ms); }
  void set_response_timeout(uint32_t ms) { this->kw_.set_response_timeout(ms); }
  void set_max_retries(uint8_t retries) { this->scheduler_.set_max_retries(retries); }

 protected:
  void handle_result_(KwResult result, uint32_t now);

  KwProtocol kw_;
  RegisterScheduler scheduler_;
  std::vector<VissLinkRegister *> registers_;  ///< même index que l'ordonnanceur
  bool link_ok_{false};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace visslink
}  // namespace esphome
