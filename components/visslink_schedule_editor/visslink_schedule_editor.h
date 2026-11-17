#pragma once

// Éditeur des programmations horaires (v0.8, EXPÉRIMENTAL) : liaison entre
// le séquenceur (visslink/schedule_editor.h, testé sur PC), le composant
// VissLink (arbitre, lectures ponctuelles, liste blanche) et les actions
// Home Assistant déclarées dans `api: actions:`.
//
// Les méthodes publiques sont appelées par les lambdas de ces actions :
//   read(circuit, root)                       -> JSON de la programmation lue
//   set_days(circuit, days, ranges, expected) -> mêmes plages sur ces jours
//   copy_day(circuit, source, days, expected) -> copie exacte d'un jour lu
//   clear_days(circuit, days, expected)       -> aucune plage sur ces jours
//   refresh(circuit)                          -> relecture des 7 jours
//   undo()                                    -> annule la dernière écriture
// Les quatre dernières renvoient false si la demande est refusée ; le motif
// est alors last_error(). Une demande acceptée n'est jamais « enregistrée »
// avant la relecture exacte des 8 octets (entité d'état et read()).

#include <string>

#include "esphome/components/json/json_util.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/visslink/visslink.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace visslink_schedule_editor {

using visslink::EditKind;
using visslink::EditRequest;
using visslink::ScheduleEditor;
using visslink::VissLinkSchedule;

class VissLinkScheduleEditor : public Component, public visslink::VissLinkBlockClient {
 public:
  void set_parent(visslink::VissLinkComponent *parent) { this->parent_ = parent; }
  void add_schedule(VissLinkSchedule *schedule);
  void set_status_sensor(text_sensor::TextSensor *sensor) { this->status_sensor_ = sensor; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  // --- Actions Home Assistant (lambdas de `api: actions:`) ---
  // Jours en texte "0,1,2" ou "*" : api.respond (ESPHome 2026.10) ne peut
  // pas recevoir un int[].
  template<typename S> void read(const S &circuit, JsonObject root) { this->read_(str_(circuit), root); }
  template<typename S, typename D, typename R, typename E>
  bool set_days(const S &circuit, const D &days, const R &ranges, const E &expected) {
    return this->submit_(EditKind::SET, str_(circuit), str_(days), str_(ranges), -1, str_(expected));
  }
  template<typename S, typename D, typename E>
  bool copy_day(const S &circuit, int source, const D &days, const E &expected) {
    return this->submit_(EditKind::COPY, str_(circuit), str_(days), std::string(), source, str_(expected));
  }
  template<typename S, typename D, typename E> bool clear_days(const S &circuit, const D &days, const E &expected) {
    return this->submit_(EditKind::CLEAR, str_(circuit), str_(days), std::string(), -1, str_(expected));
  }
  template<typename S> bool refresh(const S &circuit) { return this->refresh_(str_(circuit)); }
  bool undo();
  /// Motif du dernier refus (action Home Assistant en échec).
  const std::string &last_error() const { return this->last_error_; }

  // --- VissLinkBlockClient ---
  void on_direct_read(uint16_t address, bool ok, const uint8_t *data, uint8_t length, uint32_t now) override;
  void on_block_write(uint16_t address, bool sent, bool acked, uint32_t now) override;
  bool expects_block_write(uint16_t address, const uint8_t *data) const override {
    return this->sequencer_.expects_write(address, data);
  }
  bool block_operation_busy() const override { return this->sequencer_.busy(); }
  bool locks_block(uint16_t address) const override { return this->sequencer_.locks_block(address); }

 protected:
  static std::string str_(const std::string &s) { return s; }
  static std::string str_(const char *s) { return s != nullptr ? std::string(s) : std::string(); }
  template<typename T> static std::string str_(const T &s) {
    return s.size() == 0 ? std::string() : std::string(s.c_str(), s.size());
  }

  bool submit_(EditKind kind, const std::string &circuit, const std::string &days, const std::string &ranges,
               int source, const std::string &expected);
  /// Contexte de validation (cache, verrous) ; false + last_error_ si refus.
  bool context_(const std::string &circuit, visslink::EditContext *ctx);
  bool start_(const EditRequest &request, const std::string &error);
  bool refresh_(const std::string &circuit);
  void read_(const std::string &circuit, JsonObject root);
  bool refuse_(const std::string &why);
  VissLinkSchedule *schedule_for_(uint8_t circuit) const {
    return circuit < visslink::EDITOR_CIRCUIT_COUNT ? this->schedules_[circuit] : nullptr;
  }
  void dispatch_(uint32_t now);
  bool persist_(const visslink::EditorRecord &record);
  void publish_(uint32_t now);

  visslink::VissLinkComponent *parent_{nullptr};
  VissLinkSchedule *schedules_[visslink::EDITOR_CIRCUIT_COUNT]{};
  text_sensor::TextSensor *status_sensor_{nullptr};
  ScheduleEditor sequencer_;
  ESPPreferenceObject pref_;
  std::string last_error_;
  std::string refusal_;  ///< refus affiché 30 s dans l'entité d'état
  uint32_t refusal_until_ms_{0};
  std::string last_status_;
};

}  // namespace visslink_schedule_editor
}  // namespace esphome
