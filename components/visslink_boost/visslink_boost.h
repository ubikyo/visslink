#pragma once

// Bouclage ECS à la demande (v0.5, EXPÉRIMENTAL, désactivé par défaut).
// Colle ESPHome autour de BoostSequencer (circulation_boost.h) : exécute ses
// actions (lectures et écritures via le composant visslink, sauvegarde en
// flash), et expose les entités Home Assistant. Toute la logique de décision
// est dans BoostSequencer, testée sur PC.

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
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
class BoostDurationSelect : public select::Select {
 protected:
  void control(size_t index) override { this->publish_state(index); }
};

class VissLinkBoost : public Component, public visslink::VissLinkBlockClient {
 public:
  void set_parent(visslink::VissLinkComponent *parent) { this->parent_ = parent; }
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  void set_validated_days_mask(uint8_t mask) { this->config_.validated_days_mask = mask; }
  void set_duration_select(BoostDurationSelect *s) { this->duration_ = s; }
  void set_active_sensor(binary_sensor::BinarySensor *s) { this->active_sensor_ = s; }
  void set_restore_required_sensor(binary_sensor::BinarySensor *s) { this->restore_sensor_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_sensor_ = s; }

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

 protected:
  void dispatch_(uint32_t now);
  bool persist_(const visslink::BoostRecord &record);
  void publish_();
  void refuse_(const std::string &why);

  visslink::VissLinkComponent *parent_{nullptr};
  time::RealTimeClock *clock_{nullptr};
  visslink::BoostConfig config_;
  BoostSequencer sequencer_;
  ESPPreferenceObject pref_;
  BoostAction pending_;          ///< action émise mais pas encore acceptée (lecture refusée : ligne occupée)
  bool has_pending_{false};
  BoostDurationSelect *duration_{nullptr};
  binary_sensor::BinarySensor *active_sensor_{nullptr};
  binary_sensor::BinarySensor *restore_sensor_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};
  std::string refusal_;          ///< refus avant le séquenceur (autorisations, liaison)
  std::string last_status_;
};

}  // namespace visslink_boost
}  // namespace esphome
