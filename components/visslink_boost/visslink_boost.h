#pragma once

// Bouclage ECS à la demande (v0.5, EXPÉRIMENTAL, désactivé par défaut).
// Colle ESPHome autour de BoostSequencer (circulation_boost.h) : exécute ses
// actions (lectures et écritures via le composant visslink, sauvegarde en
// flash), et expose les entités Home Assistant. Toute la logique de décision
// est dans BoostSequencer, testée sur PC.

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/visslink/visslink.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace visslink_boost {

using visslink::BoostAction;
using visslink::BoostSequencer;

class VissLinkBoost;

/// Bouton : démarrer, restaurer ou oublier la sauvegarde.
class BoostButton : public button::Button {
 public:
  enum class Kind : uint8_t { START, RESTORE, FORGET };
  void set_parent(VissLinkBoost *parent, Kind kind) {
    this->parent_ = parent;
    this->kind_ = kind;
  }

 protected:
  void press_action() override;
  VissLinkBoost *parent_{nullptr};
  Kind kind_{Kind::START};
};

/// Durée demandée (10, 20, 30 min). Réglage local : rien n'est écrit dans la
/// régulation en changeant la durée.
/// v0.8.1 : pendant une opération, le choix passe par le composant (durée
/// raccourcie, ou refusée et ramenée à la durée en cours) : le select affiche
/// toujours la durée réellement utilisée.
class BoostDurationSelect : public select::Select {
 public:
  void set_parent(VissLinkBoost *parent) { this->parent_ = parent; }

 protected:
  void control(size_t index) override;
  VissLinkBoost *parent_{nullptr};
};

/// v0.5.1 : BoostRefreshPort sur le composant visslink.
class ComponentRefreshPort : public visslink::BoostRefreshPort {
 public:
  void set_parent(visslink::VissLinkComponent *parent) { this->parent_ = parent; }
  bool is_schedule_block(uint16_t address) const override { return this->parent_->is_schedule_block(address); }
  bool request_refresh(uint16_t address) override { return this->parent_->request_block_refresh(address); }
  bool cancel_refresh(uint16_t address) override { return this->parent_->cancel_block_refresh(address); }
  bool refresh_pending(uint16_t address) const override { return this->parent_->block_refresh_pending(address); }
  bool writes_idle() const override { return this->parent_->writes_idle(); }

 protected:
  visslink::VissLinkComponent *parent_{nullptr};
};

class VissLinkBoost : public Component, public visslink::VissLinkBlockClient {
 public:
  void set_parent(visslink::VissLinkComponent *parent) {
    this->parent_ = parent;
    this->port_.set_parent(parent);
  }
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  void set_duration_select(BoostDurationSelect *s) { this->duration_ = s; }
  /// v0.8.1 : durée choisie dans Home Assistant (rang dans BOOST_DURATIONS).
  void duration_selected(size_t index);
  void set_active_sensor(binary_sensor::BinarySensor *s) { this->active_sensor_ = s; }
  void set_restore_required_sensor(binary_sensor::BinarySensor *s) { this->restore_sensor_ = s; }
  void set_restoring_sensor(binary_sensor::BinarySensor *s) { this->restoring_sensor_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_sensor_ = s; }
  /// v0.7.1 (facultatifs) : plage inscrite dans la Vitotronic, temps restant
  /// avant la restauration. Affichage seulement.
  void set_range_sensor(text_sensor::TextSensor *s) { this->range_sensor_ = s; }
  void set_remaining_sensor(sensor::Sensor *s) { this->remaining_sensor_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  void press(BoostButton::Kind kind);

  // VissLinkBlockClient
  void on_direct_read(uint16_t address, bool ok, const uint8_t *data, uint8_t length, uint32_t now) override;
  void on_block_write(uint16_t address, bool sent, bool acked, uint32_t now) override;
  bool expects_block_write(uint16_t address, const uint8_t *data) const override {
    return this->sequencer_.expects_write(address, data);
  }
  /// v0.6 : restauration ou récupération en cours : passe avant la consigne
  /// ECS (la machine à états du bouclage n'est pas modifiée).
  bool restore_priority() const override { return visslink::boost_restore_priority(this->sequencer_); }
  /// v0.8 : verrou mutuel avec l'éditeur des programmations.
  bool block_operation_busy() const override { return !this->sequencer_.idle(); }
  bool locks_block(uint16_t address) const override { return this->sequencer_.locks_block(address); }

 protected:
  void dispatch_(uint32_t now);
  void refresh_display_(uint32_t now);
  bool persist_(const visslink::BoostRecord &record);
  void publish_(uint32_t now);
  void refuse_(const std::string &why, uint32_t now);

  visslink::VissLinkComponent *parent_{nullptr};
  time::RealTimeClock *clock_{nullptr};
  visslink::BoostConfig config_;
  BoostSequencer sequencer_;
  visslink::BoostRefreshRelay refresh_;
  ComponentRefreshPort port_;
  ESPPreferenceObject pref_;
  BoostAction pending_;          ///< action émise mais pas encore acceptée (lecture refusée : ligne occupée)
  bool has_pending_{false};
  BoostDurationSelect *duration_{nullptr};
  binary_sensor::BinarySensor *active_sensor_{nullptr};
  binary_sensor::BinarySensor *restore_sensor_{nullptr};
  binary_sensor::BinarySensor *restoring_sensor_{nullptr};  ///< v0.5.2, facultatif
  text_sensor::TextSensor *status_sensor_{nullptr};
  text_sensor::TextSensor *range_sensor_{nullptr};  ///< v0.7.1, facultatif
  sensor::Sensor *remaining_sensor_{nullptr};       ///< v0.7.1, facultatif (min)
  std::string last_range_;
  int32_t last_remaining_min_{-1};
  std::string refusal_;          ///< refus avant le séquenceur (autorisations, liaison)
  /// v0.7.1 : un refus (appui répété...) n'est affiché que 30 s, ou jusqu'au
  /// prochain changement d'état du séquenceur : l'état réel reprend ensuite.
  uint32_t refusal_until_ms_{0};
  std::string refusal_base_;
  std::string last_status_;
};

}  // namespace visslink_boost
}  // namespace esphome
