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
//  - action : circulateur M2 (0x3906), affichage seulement ;
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

/// Action affichée : circulateur M2 (-1 inconnu, 0 arrêt, 1 marche).
inline CircuitAction circuit_action(CircuitHvac hvac, int pump) {
  if (hvac == CircuitHvac::OFF)
    return CircuitAction::OFF;
  return pump == 1 ? CircuitAction::HEATING : CircuitAction::IDLE;
}

/// État publié par le climate, calculé à partir des entités lues.
struct CircuitClimateState {
  bool ready{false};    ///< consigne et programme connus au moins une fois
  float current{NAN};   ///< ambiance Vitotrol, NAN si inconnue
  float target{NAN};    ///< consigne confort lue
  CircuitHvac hvac{CircuitHvac::UNKNOWN};
  CircuitAction action{CircuitAction::IDLE};

  bool same_as(const CircuitClimateState &o) const {
    auto same = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
    return this->ready == o.ready && same(this->current, o.current) && same(this->target, o.target) &&
           this->hvac == o.hvac && this->action == o.action;
  }
};

/// `previous` : dernier état publié (un programme inconnu ne le change pas).
/// `room` : ambiance Vitotrol (NAN inconnue) ; `comfort` : consigne confort
/// lue (-1 inconnue) ; `mode_raw` : programme lu (-1 inconnu) ; `pump` :
/// circulateur (-1 inconnu).
inline CircuitClimateState circuit_climate_state(const CircuitClimateState &previous, float room, int comfort,
                                                 int mode_raw, int pump) {
  CircuitClimateState s;
  s.current = std::isfinite(room) ? room : NAN;
  s.target = comfort >= 0 ? static_cast<float>(comfort) : NAN;
  const CircuitHvac hvac = circuit_hvac_from_mode(mode_raw);
  s.hvac = hvac != CircuitHvac::UNKNOWN ? hvac : previous.hvac;
  s.ready = previous.ready || (comfort >= 0 && s.hvac != CircuitHvac::UNKNOWN);
  s.action = circuit_action(s.hvac, pump);
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
