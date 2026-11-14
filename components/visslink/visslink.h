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
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "decode.h"
#include "kw_protocol.h"
#include "schedule.h"
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

  /// Nombre d'octets à lire par bloc.
  virtual uint8_t get_length() const = 0;
  /// Nombre de blocs lus à chaque cycle (programmation : un par jour).
  virtual uint8_t get_block_count() const { return 1; }
  /// Adresse d'un bloc.
  virtual uint16_t get_block_address(uint8_t block) const { return this->address_; }
  /// Publie la valeur reçue pour un bloc (`get_length()` octets). Renvoie
  /// false si les données sont rejetées : la lecture est alors retentée comme
  /// un échec de communication, sans rien publier.
  virtual bool publish_data(uint8_t block, const uint8_t *data) = 0;
  /// Lecture d'un bloc abandonnée pour ce cycle.
  virtual void publish_unknown(uint8_t block) = 0;
  /// Appelé une fois par seconde (états dépendant de l'heure locale).
  virtual void on_clock_tick() {}
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
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
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
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void dump_register_config() override;
};
#endif

#ifdef USE_TEXT_SENSOR
/// Programmation hebdomadaire d'un circuit : 7 blocs de 8 octets à partir de
/// l'adresse du lundi, une seule entrée dans l'ordonnanceur.
///
/// L'état principal est le résumé de la semaine ; en option, un capteur par
/// jour, la programmation du jour courant et l'indicateur de plage active
/// (ces deux derniers d'après l'horloge `time`).
class VissLinkSchedule : public text_sensor::TextSensor, public VissLinkRegister {
 public:
  uint8_t get_length() const override { return SCHEDULE_BLOCK_LENGTH; }
  uint8_t get_block_count() const override { return SCHEDULE_DAYS; }
  uint16_t get_block_address(uint8_t block) const override { return schedule_day_address(this->address_, block); }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void on_clock_tick() override { this->refresh_clock_(); }
  void dump_register_config() override;

  void set_day_sensor(uint8_t day, text_sensor::TextSensor *sensor) {
    if (day < SCHEDULE_DAYS)
      this->day_sensors_[day] = sensor;
  }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  void set_today_sensor(text_sensor::TextSensor *sensor) { this->today_sensor_ = sensor; }
#ifdef USE_BINARY_SENSOR
  void set_active_sensor(binary_sensor::BinarySensor *sensor) { this->active_sensor_ = sensor; }
#endif
#endif

 protected:
  void publish_week_();
  void refresh_clock_();
  static void publish_text_(text_sensor::TextSensor *sensor, const std::string &text);

  WeekSchedule week_;
  bool first_pass_done_{false};  ///< les 7 blocs ont été tentés au moins une fois
  text_sensor::TextSensor *day_sensors_[SCHEDULE_DAYS]{};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
  text_sensor::TextSensor *today_sensor_{nullptr};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *active_sensor_{nullptr};
#endif
#endif
};
#endif

static_assert(SCHEDULE_BLOCK_LENGTH <= KwProtocol::MAX_READ_LENGTH, "schedule block exceeds KW read length");

/// Relie l'ordonnanceur, le protocole KW et les registres.
class VissLinkComponent : public Component, public uart::UARTDevice {
 public:
  /// Écart entre les premières échéances de deux registres consécutifs
  /// (période nominale du 0x05, donc durée minimale d'une lecture).
  static constexpr uint32_t STARTUP_STAGGER_MS = 2000;
  /// Période de réévaluation des états dépendant de l'heure (jour courant,
  /// plage active).
  static constexpr uint32_t CLOCK_TICK_MS = 1000;

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
