#pragma once

#include <cstdint>

namespace esphome {
namespace visslink {

/// Vrai si `deadline` est atteinte à l'instant `now` (horloge millis()).
///
/// La comparaison par différence signée reste correcte lors du débordement
/// de millis() (tous les ~49,7 jours) tant que les écarts restent inférieurs
/// à 2^31 ms (~24,8 jours). Les intervalles sont donc limités à 7 jours.
inline bool time_reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace visslink
}  // namespace esphome
