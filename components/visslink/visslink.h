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
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "decode.h"
#include "kw_protocol.h"
#include "schedule.h"
#include "scheduler.h"
#include "write_guard.h"

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
  /// Écriture (v0.3) : contrôle complet d'une valeur juste avant émission
  /// (liste blanche, longueur, bornes configurées et documentées). Refus par
  /// défaut : seul VissLinkNumber peut l'accepter.
  virtual bool write_allowed(uint8_t /*value*/) const { return false; }
  /// Écriture (v0.3) : une écriture de `value` vient d'être tentée, la
  /// prochaine lecture est la relecture de vérification.
  virtual void expect_written_value(uint8_t /*value*/) {}
  /// Programme de fonctionnement (v0.3) : valeur brute lue (00, 03, 05), ou
  /// -1 si inconnue ou si ce registre n'est pas un programme.
  virtual int mode_raw() const { return -1; }
  /// Écriture (v0.3) : demande refusée ou abandonnée ; republier la dernière
  /// valeur lue pour que Home Assistant ne garde pas la valeur demandée.
  virtual void restore_read_state() {}
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

class VissLinkComponent;

#ifdef USE_NUMBER
/// Consigne réglable (v0.3) : registre uint8 de la liste blanche, lu
/// périodiquement comme un capteur. L'état publié provient toujours d'une
/// lecture, jamais de la valeur demandée. Une demande n'aboutit à une
/// écriture que si `enable_writes: true` et si toutes les protections
/// passent (cf. VissLinkComponent::submit_write).
class VissLinkNumber : public number::Number, public VissLinkRegister {
 public:
  void set_parent(VissLinkComponent *parent) { this->parent_ = parent; }

  uint8_t get_length() const override { return 1; }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void expect_written_value(uint8_t value) override { this->verification_.expect(value); }
  void restore_read_state() override;
  bool write_allowed(uint8_t value) const override;
  void dump_register_config() override;
  /// Une valeur lue est disponible.
  bool has_read_value() const { return this->has_state() && !std::isnan(this->state); }

 protected:
  void control(float value) override;

  VissLinkComponent *parent_{nullptr};
  WriteVerification verification_;
};
#endif

#ifdef USE_TEXT_SENSOR
/// Programme de fonctionnement en lecture seule stricte (v0.3) : texte
/// « ECS seule » (00), « Chauffage + ECS » (03), « Arrêt » (05), « Inconnu »
/// pour toute autre valeur ou un échec de lecture. Jamais écrit : aucune
/// commande n'existe pour cette entité (0x2301 ou 0x3301). Variante non
/// modifiable du sélecteur.
class VissLinkModeText : public text_sensor::TextSensor, public VissLinkRegister {
 public:
  void set_parent(VissLinkComponent *parent) { this->parent_ = parent; }

  uint8_t get_length() const override { return 1; }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  int mode_raw() const override { return this->raw_; }
  void dump_register_config() override;

 protected:
  void publish_text_(const char *text);

  VissLinkComponent *parent_{nullptr};
  int raw_{-1};
};
#endif

#ifdef USE_SELECT
/// Programme de fonctionnement modifiable (v0.4, expérimental) : 0x2301 (A1)
/// ou 0x3301 (M2). Options : ECS seule (00), Chauffage + ECS (03), Arrêt
/// (05). Toute autre valeur lue rend l'état inconnu, avec un avertissement.
/// L'état publié provient toujours d'une lecture. Écriture : A1 exige
/// `enable_writes` ET `enable_mode_writes` ; M2 exige en plus
/// `enable_m2_mode_write` (mode_write_permitted, vérifié à la demande et
/// juste avant l'émission).
class VissLinkModeSelect : public select::Select, public VissLinkRegister {
 public:
  void set_parent(VissLinkComponent *parent) { this->parent_ = parent; }

  uint8_t get_length() const override { return 1; }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void expect_written_value(uint8_t value) override { this->verification_.expect(value); }
  void restore_read_state() override;
  bool write_allowed(uint8_t value) const override;
  int mode_raw() const override;
  void dump_register_config() override;

 protected:
  void control(size_t index) override;
  void publish_unknown_state_();

  VissLinkComponent *parent_{nullptr};
  WriteVerification verification_;
};
#endif

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
  /// Écritures Optolink (v0.3) : false par défaut, n'est activé que par
  /// `enable_writes: true` dans le YAML.
  void set_enable_writes(bool enable) { this->enable_writes_ = enable; }

  /// Écriture des programmes de fonctionnement (v0.3, expérimental) : false
  /// par défaut, exige aussi enable_writes.
  void set_enable_mode_writes(bool enable) { this->enable_mode_writes_ = enable; }
  /// Écriture du programme de M2 (0x3301, v0.4, non documentée) : false par
  /// défaut, exige aussi enable_writes et enable_mode_writes.
  void set_enable_m2_mode_write(bool enable) { this->enable_m2_mode_write_ = enable; }

#ifdef USE_NUMBER
  /// Demande d'écriture venant de Home Assistant (VissLinkNumber::control).
  void submit_write(VissLinkNumber *number, float value);
#endif
  /// Registre de programme (select A1 ou text_sensor), au plus deux.
  void add_mode_register(VissLinkRegister *reg);
#ifdef USE_SELECT
  /// Demande de programme venant de Home Assistant (VissLinkModeSelect::control).
  void submit_mode_write(VissLinkModeSelect *select, size_t index);
#endif
#ifdef USE_BINARY_SENSOR
  void set_mode_divergence_sensor(binary_sensor::BinarySensor *sensor) { this->mode_divergence_ = sensor; }
#endif
  /// Un programme a été lu (ou est devenu inconnu) : divergence A1/M2.
  void on_mode_state_changed();

 protected:
  void handle_result_(KwResult result, uint32_t now);
  void handle_write_result_(KwResult result, uint32_t now);
  bool start_write_(uint32_t now);
  void drop_expired_writes_(uint32_t now);
  int index_of_(const VissLinkRegister *reg) const;
  /// Registre de programme de l'autre circuit, ou NONE.
  int mode_peer_of_(int index) const;
  void read_back_done_(int index, uint32_t now);
  void log_mode_divergence_after_write_();

  KwProtocol kw_;
  RegisterScheduler scheduler_;
  std::vector<VissLinkRegister *> registers_;  ///< même index que l'ordonnanceur
  bool link_ok_{false};
  bool enable_writes_{false};
  bool enable_mode_writes_{false};
  bool enable_m2_mode_write_{false};
  ReadBackTracker read_back_;
  bool mode_write_checked_{false};  ///< écriture de programme : divergence à journaliser après les relectures
  VissLinkRegister *mode_registers_[2]{};  ///< sélecteurs de programme (A1, M2), au plus deux
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *mode_divergence_{nullptr};
#endif
  bool write_in_flight_{false};  ///< transaction KW en cours = écriture
  WriteArbiter writes_;
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace visslink
}  // namespace esphome
