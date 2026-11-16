#pragma once

// Climate du circuit plancher M2 (v0.7, EXPÉRIMENTAL).
//
// Indépendant d'ESPHome (testé sur PC). Le climate ne lit AUCUN registre et
// n'écrit RIEN lui-même : il agrège des entités VissLink existantes et
// transmet ses commandes à leurs chemins sécurisés.
//
//  - température actuelle : capteur d'ambiance de la Vitotrol (0x0898), jamais
//    la température de départ du plancher (0x3900) ;
//  - température cible : consigne CONFORT de M2 (0x3306), lue ; la consigne
//    réduite (0x3307) reste une entité number distincte ;
//  - mode : programme de M2 (0x3301) lu : 03 -> HEAT, 00 (ECS seule) et
//    05 (Arrêt) -> OFF ; toute autre valeur : mode inchangé (jamais deviné) ;
//  - action et état (v0.7.3) : programme M2 (0x3301), circulateur M2
//    (0x3906) et brûleur (0x0842), lus par leurs entités existantes ;
//    Chauffe = chauffage + circulateur + brûleur ; Inertie = chauffage +
//    circulateur, brûleur arrêté ; Arrêt = chauffage désactivé ou circulateur
//    arrêté ; Inconnu = donnée nécessaire inconnue ou périmée (aucune lecture
//    réussie depuis 3 intervalles + 30 s). Affichage seulement ;
//  - commandes : HEAT -> 03, OFF -> 00 (ECS seule, l'eau chaude reste
//    produite) ; jamais 05 ; consigne -> transaction de la consigne 0x3306
//    (enable_heating_setpoint_writes), mode -> sélecteur de M2
//    (enable_mode_writes + enable_m2_mode_write).

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "write_guard.h"

namespace esphome {
namespace visslink {

static constexpr uint16_t M2_COMFORT_SETPOINT = 0x3306;
static constexpr uint16_t M2_REDUCED_SETPOINT = 0x3307;

enum class CircuitHvac : uint8_t { OFF, HEAT, UNKNOWN };
enum class CircuitAction : uint8_t { OFF, HEATING, IDLE };

/// Programme lu (00/03/05, -1 inconnu) -> mode du climate.
inline CircuitHvac circuit_hvac_from_mode(int raw) {
  switch (raw) {
    case 0x03:
      return CircuitHvac::HEAT;  // Chauffage + ECS
    case 0x00:                   // ECS seule : chauffage arrêté, ECS produite
    case 0x05:                   // Arrêt (veille, hors gel)
      return CircuitHvac::OFF;
    default:
      return CircuitHvac::UNKNOWN;
  }
}

/// Commande du climate -> valeur du programme à écrire. Jamais 05 : OFF
/// arrête le chauffage du circuit, pas la chaudière ni l'eau chaude.
inline uint8_t mode_value_for_hvac(bool heat) { return heat ? 0x03 : 0x00; }

/// Index d'option du sélecteur de programme pour une commande du climate.
inline size_t mode_option_for_hvac(bool heat) {
  return static_cast<size_t>(operating_mode_index(mode_value_for_hvac(heat)));
}

/// v0.7.3 : état du plancher, publié par le text_sensor « État ».
enum class CircuitStatus : uint8_t { UNKNOWN, HEATING, INERTIA, STOPPED };

inline const char *circuit_status_text(CircuitStatus status) {
  switch (status) {
    case CircuitStatus::HEATING:
      return "Chauffe";
    case CircuitStatus::INERTIA:
      return "Inertie";
    case CircuitStatus::STOPPED:
      return "Arrêt";
    default:
      return "Inconnu";
  }
}

/// v0.7.3 : une valeur lue est périmée sans lecture réussie depuis
/// 3 intervalles de lecture + 30 s.
static constexpr uint32_t STALE_MARGIN_MS = 30000;
static constexpr uint32_t STALE_INTERVALS = 3;

inline uint64_t stale_after_ms(uint32_t interval_ms) {
  return static_cast<uint64_t>(STALE_INTERVALS) * interval_ms + STALE_MARGIN_MS;
}

/// `has_read` : au moins une lecture réussie ; `last_ok_ms` : sa date.
inline bool reading_fresh(bool has_read, uint32_t last_ok_ms, uint32_t interval_ms, uint32_t now) {
  return has_read && static_cast<uint64_t>(static_cast<uint32_t>(now - last_ok_ms)) <= stale_after_ms(interval_ms);
}

/// État à partir des valeurs lues ET fraîches (-1 : inconnue ou périmée) :
/// `mode_raw` programme M2, `pump` circulateur M2, `burner` brûleur (0/1).
/// Seules les données nécessaires comptent : chauffage désactivé -> Arrêt
/// quels que soient le circulateur et le brûleur ; circulateur arrêté ->
/// Arrêt quel que soit le brûleur.
inline CircuitStatus circuit_status(int mode_raw, int pump, int burner) {
  const CircuitHvac hvac = circuit_hvac_from_mode(mode_raw);
  if (hvac == CircuitHvac::OFF)
    return CircuitStatus::STOPPED;
  if (hvac == CircuitHvac::UNKNOWN)
    return CircuitStatus::UNKNOWN;
  if (pump == 0)
    return CircuitStatus::STOPPED;
  if (pump != 1)
    return CircuitStatus::UNKNOWN;
  if (burner == 1)
    return CircuitStatus::HEATING;
  if (burner == 0)
    return CircuitStatus::INERTIA;
  return CircuitStatus::UNKNOWN;
}

/// Action Home Assistant : `heating` seulement en Chauffe ; `off` quand le
/// mode affiché est OFF (chauffage désactivé) ; sinon `idle` (Inertie,
/// circulateur arrêté, Inconnu : jamais « chauffe » sans preuve).
inline CircuitAction circuit_action(CircuitHvac hvac, CircuitStatus status) {
  if (hvac == CircuitHvac::OFF)
    return CircuitAction::OFF;
  return status == CircuitStatus::HEATING ? CircuitAction::HEATING : CircuitAction::IDLE;
}

/// État publié par le climate, calculé à partir des entités lues.
struct CircuitClimateState {
  bool ready{false};    ///< consigne et programme connus au moins une fois
  float current{NAN};   ///< ambiance Vitotrol, NAN si inconnue
  float target{NAN};    ///< consigne confort lue
  CircuitHvac hvac{CircuitHvac::UNKNOWN};
  CircuitAction action{CircuitAction::IDLE};
  CircuitStatus status{CircuitStatus::UNKNOWN};

  bool same_as(const CircuitClimateState &o) const {
    auto same = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
    return this->ready == o.ready && same(this->current, o.current) && same(this->target, o.target) &&
           this->hvac == o.hvac && this->action == o.action && this->status == o.status;
  }
};

/// `previous` : dernier état publié (un programme inconnu ne le change pas).
/// `room` : ambiance Vitotrol (NAN inconnue) ; `comfort` : consigne confort
/// lue (-1 inconnue) ; `mode_raw` : programme lu (-1 inconnu ou périmé) ;
/// `pump` : circulateur, `burner` : brûleur (-1 inconnu ou périmé).
inline CircuitClimateState circuit_climate_state(const CircuitClimateState &previous, float room, int comfort,
                                                 int mode_raw, int pump, int burner) {
  CircuitClimateState s;
  s.current = std::isfinite(room) ? room : NAN;
  s.target = comfort >= 0 ? static_cast<float>(comfort) : NAN;
  const CircuitHvac hvac = circuit_hvac_from_mode(mode_raw);
  s.hvac = hvac != CircuitHvac::UNKNOWN ? hvac : previous.hvac;
  s.ready = previous.ready || (comfort >= 0 && s.hvac != CircuitHvac::UNKNOWN);
  s.status = circuit_status(mode_raw, pump, burner);
  s.action = circuit_action(s.hvac, s.status);
  return s;
}

/// Tri d'une commande de température reçue par le climate.
enum class TemperatureCommand : uint8_t {
  FORWARD,    ///< transmise à la transaction de la consigne confort
  UNCHANGED,  ///< égale à la consigne lue : rien n'est demandé
  PENDING,    ///< déjà en cours pour cette valeur : pas de doublon
  INVALID,    ///< non finie
};

inline TemperatureCommand classify_temperature_command(float value, int current, bool pending, uint8_t pending_target) {
  if (!std::isfinite(value))
    return TemperatureCommand::INVALID;
  const long rounded = std::lround(value);
  if (pending && rounded == pending_target)
    return TemperatureCommand::PENDING;
  if (!pending && current >= 0 && std::fabs(value - static_cast<float>(current)) < 0.01f)
    return TemperatureCommand::UNCHANGED;
  return TemperatureCommand::FORWARD;
}

/// Tri d'une commande de mode : rien à faire si le programme lu correspond
/// déjà (pas d'écriture inutile).
inline bool hvac_command_needed(CircuitHvac requested, CircuitHvac current) { return requested != current; }

}  // namespace visslink
}  // namespace esphome
