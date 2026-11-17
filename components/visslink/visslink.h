#pragma once

#include <memory>
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
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif

#include "block_client.h"
#include "circuit_climate.h"
#include "circulation_boost.h"
#include "decode.h"
#include "dhw_setpoint.h"
#include "kw_protocol.h"
#include "schedule.h"
#include "schedule_editor.h"
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
  /// Registre multi-blocs (v0.5.1) : bloc à lire en premier à chaque cycle
  /// (jour courant), ou -1 si aucun (heure inconnue, registre simple).
  virtual int preferred_first_block() const { return -1; }
  /// v0.5.1 : ce bloc doit-il être relu sans attendre son tour (jour courant
  /// pas encore connu, et pas déjà demandé) ?
  virtual bool wants_block_refresh(uint8_t /*block*/) const { return false; }
  /// v0.5.1 : la relecture de ce bloc vient d'être demandée.
  virtual void on_block_refresh_requested(uint8_t /*block*/) {}
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
  /// v0.7 : consigne lue (°C entiers), ou -1 si inconnue (number seulement).
  virtual int setpoint_value() const { return -1; }
  /// Écriture (v0.3) : demande refusée ou abandonnée ; republier la dernière
  /// valeur lue pour que Home Assistant ne garde pas la valeur demandée.
  virtual void restore_read_state() {}
  virtual void dump_register_config() = 0;

  /// v0.7.3 : lecture réussie (données acceptées) à l'instant `now`.
  void mark_read_ok(uint32_t now) {
    this->last_read_ok_ms_ = now;
    this->has_read_ok_ = true;
  }
  /// v0.7.3 : lecture réussie depuis moins de 3 intervalles + 30 s.
  bool read_fresh(uint32_t now) const {
    return reading_fresh(this->has_read_ok_, this->last_read_ok_ms_, this->interval_ms_, now);
  }

 protected:
  uint16_t address_{0};
  uint32_t interval_ms_{60000};
  uint32_t last_read_ok_ms_{0};
  bool has_read_ok_{false};
};

#ifdef USE_SENSOR
/// Valeur numérique brute (aucune échelle : filtres `multiply` du YAML).
class VissLinkSensor : public sensor::Sensor, public VissLinkRegister {
 public:
  void set_data_type(DataType type) { this->data_type_ = type; }
  DataType get_data_type() const { return this->data_type_; }
  /// v0.5.1 : valeur brute signifiant « non disponible » (publiée inconnue).
  void add_invalid_raw_value(uint32_t raw) { this->invalid_raw_.add(raw); }

  uint8_t get_length() const override { return data_type_length(this->data_type_); }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void dump_register_config() override;

 protected:
  DataType data_type_{DataType::INT16};  ///< défaut v0.1 : température
  InvalidRawValues invalid_raw_;
  bool invalid_logged_{false};  ///< avertissement émis une fois par épisode
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
  int preferred_first_block() const override;
  /// Une seule demande par jour courant : un bloc illisible n'est pas relu
  /// en boucle, il attend le cycle suivant.
  bool wants_block_refresh(uint8_t block) const override {
    return !this->week_.is_known(block) && this->requested_day_ != block;
  }
  void on_block_refresh_requested(uint8_t block) override { this->requested_day_ = block; }
  void dump_register_config() override;

  void set_day_sensor(uint8_t day, text_sensor::TextSensor *sensor) {
    if (day < SCHEDULE_DAYS)
      this->day_sensors_[day] = sensor;
  }
  /// v0.8 : conserve les 8 octets lus de chaque jour et l'heure de leur
  /// lecture (éditeur des programmations). Sans éditeur : rien d'alloué.
  void enable_raw_cache();
  bool has_raw_cache() const { return this->raw_ != nullptr; }
  /// Octets lus (jours connus seulement).
  void raw_week(WeekBlocks *out) const;
  /// millis() de la dernière lecture valide d'un jour ; false si jamais lu.
  bool day_read_ms(uint8_t day, uint32_t *ms) const;
  /// Programmation décodée (affichage).
  const WeekSchedule &week() const { return this->week_; }
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
  int requested_day_{-1};        ///< dernier jour dont la relecture a été demandée
  text_sensor::TextSensor *day_sensors_[SCHEDULE_DAYS]{};
  struct RawCache {
    WeekBlocks week;
    uint32_t read_ms[SCHEDULE_DAYS]{};
  };
  std::unique_ptr<RawCache> raw_;  ///< v0.8, éditeur seulement
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

/// Client du chemin expérimental des blocs horaires (bouclage ECS v0.5,
/// éditeur des programmations v0.8) : block_client.h.
using VissLinkBlockClient = BlockClient;
static_assert(BLOCK_OWNER_BOOST == WriteArbiter::BOOST_OWNER, "block owners");
static_assert(BLOCK_OWNER_EDITOR == WriteArbiter::EDITOR_OWNER, "block owners");

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
  int setpoint_value() const override { return this->has_read_value() ? static_cast<int>(this->state) : -1; }

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

#ifdef USE_TEXT_SENSOR
/// Horloge de la Vitotronic (0x088E), lecture seule (v0.5) : date, heure et
/// jour décodés et vérifiés, ou « invalide (raison) : octets bruts ».
class VissLinkClockText : public text_sensor::TextSensor, public VissLinkRegister {
 public:
  uint8_t get_length() const override { return VITO_CLOCK_LENGTH; }
  bool publish_data(uint8_t block, const uint8_t *data) override;
  void publish_unknown(uint8_t block) override;
  void dump_register_config() override;
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
  /// Écriture des blocs horaires du bouclage (v0.5, expérimental) : false par
  /// défaut, exige aussi enable_writes.
  void set_enable_schedule_writes(bool enable) { this->enable_schedule_writes_ = enable; }
  /// Écriture de la consigne ECS (0x6300, v0.6, expérimental) : false par
  /// défaut, exige aussi enable_writes. Aucune autre autorisation ne la permet.
  void set_enable_dhw_setpoint_write(bool enable) { this->enable_dhw_setpoint_write_ = enable; }
  bool dhw_setpoint_writes_enabled() const { return this->enable_writes_ && this->enable_dhw_setpoint_write_; }
  /// Écriture des consignes de chauffage A1/M2 (0x2306, 0x2307, 0x3306,
  /// 0x3307, v0.7, expérimental) : false par défaut, exige aussi
  /// enable_writes. Aucune autre autorisation ne la permet.
  void set_enable_heating_setpoint_writes(bool enable) { this->enable_heating_setpoint_writes_ = enable; }
  bool heating_setpoint_writes_enabled() const {
    return this->enable_writes_ && this->enable_heating_setpoint_writes_;
  }
#ifdef USE_NUMBER
  /// Consigne (ECS ou chauffage) gérée par sa transaction dédiée (v0.6/v0.7).
  void add_setpoint_number(VissLinkNumber *number);
#ifdef USE_TEXT_SENSOR
  /// Texte facultatif d'état des modifications de cette consigne.
  void set_setpoint_status_sensor(VissLinkNumber *number, text_sensor::TextSensor *sensor);
#endif
#endif
  /// v0.7 : une modification de cette consigne est en cours (relecture ou
  /// écriture) ; `target` = valeur demandée.
  bool setpoint_pending(const VissLinkRegister *reg, uint8_t *target) const;

  // --- Blocs horaires (v0.5) ---
  void set_block_client(VissLinkBlockClient *client) { this->block_clients_.set(BLOCK_OWNER_BOOST, client); }
  /// v0.8 : éditeur des programmations.
  void set_editor_client(VissLinkBlockClient *client) { this->block_clients_.set(BLOCK_OWNER_EDITOR, client); }
  /// v0.8 : l'autre client de blocs (bouclage / éditeur) a une opération en cours.
  bool block_client_busy_other(int owner) const { return this->block_clients_.other_busy(owner); }
  /// v0.8 : ce bloc est verrouillé par l'autre client.
  bool block_locked_by_other(int owner, uint16_t address) const {
    return this->block_clients_.locked_by_other(owner, address);
  }
  /// v0.8 : relance tout de suite le cycle de lecture d'un registre (7 blocs
  /// d'une programmation, espacés de BLOCK_DELAY_MS). Sans effet si un cycle
  /// est déjà en cours.
  bool request_register_cycle(const VissLinkRegister *reg);
  bool schedule_writes_enabled() const { return this->enable_writes_ && this->enable_schedule_writes_; }
  bool link_ok() const { return this->link_ok_; }
  /// Aucune écriture en attente ni en cours (consignes, programmes, blocs).
  bool writes_idle() const { return this->writes_.pending_count() == 0 && !this->writes_.active(); }
  /// Lecture ponctuelle prioritaire : horloge 0x088E ou bloc du bouclage
  /// (v0.8, éditeur : bloc d'une des quatre programmations), 8 octets.
  /// Refusée si une autre est en cours ou si l'adresse n'est pas autorisée.
  /// Le résultat n'est rendu qu'au client `owner`.
  bool request_direct_read(uint16_t address, uint8_t length, int owner = BLOCK_OWNER_BOOST);
  /// v0.5.1 : relecture prioritaire d'un seul bloc d'une programmation
  /// déclarée (adresse du jour), publiée comme une lecture normale ; le
  /// cycle hebdomadaire n'est ni avancé ni recommencé. Jamais servie pendant
  /// une vérification d'écriture. Refusée si le registre est en cours de
  /// lecture ou a déjà une relecture en attente : réessayer plus tard.
  bool request_block_refresh(uint16_t address);
  /// Adresse d'un bloc d'une programmation déclarée (v0.5.1).
  bool is_schedule_block(uint16_t address) const;
  /// Retire un rafraîchissement demandé mais pas encore émis (v0.5.1).
  bool cancel_block_refresh(uint16_t address);
  /// Un rafraîchissement de ce bloc attend encore son tour (v0.5.1).
  bool block_refresh_pending(uint16_t address) const;
  /// Écriture d'un bloc (8 octets), via l'arbitre. Refusée sans les
  /// autorisations, hors liste blanche du propriétaire (bouclage : 0x2200 ;
  /// éditeur : les 28 blocs des programmations), si le bloc est invalide ou
  /// verrouillé par l'autre client.
  bool submit_block_write(uint16_t address, const uint8_t *data, uint32_t now, int owner = BLOCK_OWNER_BOOST);

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
  void handle_block_write_result_(KwResult result, uint32_t now);
  void handle_direct_result_(KwResult result, uint32_t now);
  bool start_direct_read_(uint32_t now);
  bool block_write_allowed_(int owner, uint16_t address, const uint8_t *data) const;
  bool start_write_(uint32_t now);
  void preempt_unsent_read_(uint32_t now);
  void current_day_first_(uint32_t now);
  bool find_schedule_block_(uint16_t address, size_t *index, uint8_t *block) const;
  void log_write_timing_(uint16_t address);
  void drop_expired_writes_(uint32_t now);
  int index_of_(const VissLinkRegister *reg) const;
  /// Registre de programme de l'autre circuit, ou NONE.
  int mode_peer_of_(int index) const;
  void read_back_done_(int index, uint32_t now);
  void log_mode_divergence_after_write_();
  // v0.6/v0.7 : consignes ECS et chauffage, une transaction par registre.
  struct SetpointChannel {
    SetpointChannel(VissLinkRegister *r, uint16_t address) : reg(r), writer(address) {}
    VissLinkRegister *reg;
    int owner{WriteArbiter::NONE};
    SetpointWriter writer;
    uint32_t published{0};  ///< version du texte publiée
#ifdef USE_TEXT_SENSOR
    text_sensor::TextSensor *status{nullptr};
#endif
  };
#ifdef USE_NUMBER
  void submit_setpoint_(SetpointChannel &ch, VissLinkNumber *number, float value);
#endif
  void setpoint_on_read_(SetpointChannel &ch, bool ok, uint8_t value, bool verify, uint32_t now);
  void setpoint_publish_(SetpointChannel &ch);
  SetpointChannel *setpoint_channel_(int index);
  bool setpoint_permitted_(uint16_t address) const;
  /// Consignes dont l'écriture attend (restauration du bouclage en cours).
  OwnerSet deferred_setpoints_() const;

  KwProtocol kw_;
  RegisterScheduler scheduler_;
  std::vector<VissLinkRegister *> registers_;  ///< même index que l'ordonnanceur
  bool link_ok_{false};
  bool enable_writes_{false};
  bool enable_mode_writes_{false};
  bool enable_m2_mode_write_{false};
  bool enable_schedule_writes_{false};
  bool enable_dhw_setpoint_write_{false};
  bool enable_heating_setpoint_writes_{false};
  std::vector<SetpointChannel> setpoints_;  ///< consignes déclarées (au plus 5)
  BlockClients block_clients_;
  /// Lecture ponctuelle (v0.5) : une à la fois, nouvelles tentatives comme
  /// une lecture ordinaire.
  struct DirectRead {
    bool pending{false};
    bool in_flight{false};
    uint16_t address{0};
    uint8_t length{0};
    uint8_t failures{0};
    uint32_t due_ms{0};
    int owner{BLOCK_OWNER_BOOST};  ///< v0.8 : client destinataire du résultat
  } direct_;
  bool block_verify_pending_{false};  ///< écriture de bloc : arbitre libéré après la relecture
  ReadBackTracker read_back_;
  WriteTiming timing_;         ///< chronologie de l'écriture en cours (v0.4.1)
  bool timing_valid_{false};   ///< trame réellement émise
  bool mode_write_checked_{false};  ///< écriture de programme : divergence à journaliser après les relectures
  VissLinkRegister *mode_registers_[2]{};  ///< sélecteurs de programme (A1, M2), au plus deux
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *mode_divergence_{nullptr};
#endif
  bool write_in_flight_{false};  ///< transaction KW en cours = écriture
  WriteArbiter writes_;
  HighFrequencyLoopRequester high_freq_;
};

#if defined(USE_CLIMATE) && defined(USE_NUMBER) && defined(USE_SELECT)
/// Climate du circuit plancher M2 (v0.7, expérimental). Ne lit aucun
/// registre et n'écrit rien lui-même : il agrège des entités VissLink
/// existantes (circuit_climate.h) et passe ses commandes à leurs chemins
/// sécurisés (VissLinkComponent::submit_write, submit_mode_write). Ce n'est
/// PAS un thermostat de pièce : la cible est la consigne confort du circuit.
class VissLinkCircuitClimate : public climate::Climate, public Component {
 public:
  void set_parent(VissLinkComponent *parent) { this->parent_ = parent; }
  void set_comfort_number(VissLinkNumber *number) { this->comfort_ = number; }
  void set_mode_select(VissLinkModeSelect *select) { this->mode_select_ = select; }
#ifdef USE_SENSOR
  void set_room_sensor(sensor::Sensor *sensor) { this->room_ = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_pump_sensor(VissLinkBinarySensor *sensor) { this->pump_ = sensor; }
  void set_burner_sensor(VissLinkBinarySensor *sensor) { this->burner_ = sensor; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_status_sensor(text_sensor::TextSensor *sensor) { this->status_sensor_ = sensor; }
#endif

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  /// Recalcule l'état depuis les entités ; publie s'il a changé (ou `force`).
  void refresh_(bool force);
  int mode_raw_() const;
  /// v0.7.3 : programme lu et frais (-1 sinon).
  int fresh_mode_raw_(uint32_t now) const;
#ifdef USE_BINARY_SENSOR
  /// v0.7.3 : 1/0 lu et frais, -1 sinon (absent, inconnu ou périmé).
  static int fresh_binary_(const VissLinkBinarySensor *sensor, uint32_t now);
#endif
  void publish_status_();

  VissLinkComponent *parent_{nullptr};
  VissLinkNumber *comfort_{nullptr};
  VissLinkModeSelect *mode_select_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *room_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  VissLinkBinarySensor *pump_{nullptr};
  VissLinkBinarySensor *burner_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_sensor_{nullptr};
#endif
  CircuitClimateState state_;
  CircuitStatus published_status_{CircuitStatus::UNKNOWN};
  bool status_published_{false};
};
#endif

}  // namespace visslink
}  // namespace esphome
