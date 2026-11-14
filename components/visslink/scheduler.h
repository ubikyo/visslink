#pragma once

// Ordonnanceur des lectures : un intervalle par registre, une lecture à la fois.
//
// Indépendant d'ESPHome (temps passé en paramètre) pour être testé sur PC.
//
//  - acquire() ne délivre rien tant qu'une lecture est en cours : aucun doublon,
//    aucune accumulation ;
//  - le registre le plus en retard passe en premier, à égalité le premier
//    déclaré : un registre échu attend au plus N-1 lectures ;
//  - l'échéance suivante vaut échéance + intervalle (pas de dérive), sauf si
//    elle est déjà dépassée : elle repart alors de maintenant (pas de
//    rattrapage) ;
//  - un échec est retenté après RETRY_DELAY_MS, au plus max_retries fois, puis
//    le registre attend son intervalle normal.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "time_utils.h"

namespace esphome {
namespace visslink {

class RegisterScheduler {
 public:
  static constexpr int NONE = -1;
  static constexpr uint32_t RETRY_DELAY_MS = 1000;

  struct Entry {
    uint32_t interval_ms;
    uint32_t due_ms;   ///< prochaine tentative
    uint8_t failures;  ///< échecs consécutifs dans le cycle en cours
  };

  void set_max_retries(uint8_t max_retries) { this->max_retries_ = max_retries; }
  uint8_t max_retries() const { return this->max_retries_; }

  /// Ajoute un registre, échu immédiatement. À appeler depuis setup() :
  /// loop() n'alloue jamais.
  void add(uint32_t interval_ms, uint32_t now) { this->entries_.push_back({interval_ms, now, 0}); }

  /// Index du registre à lire maintenant (marqué en cours), ou NONE.
  int acquire(uint32_t now) {
    if (this->current_ != NONE)
      return NONE;
    uint32_t best_lateness = 0;
    for (size_t i = 0; i < this->entries_.size(); i++) {
      const Entry &e = this->entries_[i];
      if (!time_reached(now, e.due_ms))
        continue;
      const uint32_t lateness = now - e.due_ms;
      if (this->current_ == NONE || lateness > best_lateness) {
        this->current_ = static_cast<int>(i);
        best_lateness = lateness;
      }
    }
    return this->current_;
  }

  /// Registre en cours de lecture, ou NONE.
  int current() const { return this->current_; }

  /// Termine la lecture en cours. Renvoie true si c'est un échec définitif
  /// pour ce cycle (nouvelles tentatives épuisées).
  bool finish(bool success, uint32_t now) {
    if (this->current_ == NONE)
      return false;
    Entry &e = this->entries_[this->current_];
    this->current_ = NONE;
    if (!success && e.failures < this->max_retries_) {
      e.failures++;
      e.due_ms = now + RETRY_DELAY_MS;
      return false;
    }
    const bool gave_up = !success;
    e.failures = 0;
    e.due_ms += e.interval_ms;
    if (time_reached(now, e.due_ms))
      e.due_ms = now + e.interval_ms;
    return gave_up;
  }

  const Entry &entry(size_t index) const { return this->entries_[index]; }

 protected:
  std::vector<Entry> entries_;
  int current_{NONE};
  uint8_t max_retries_{2};
};

}  // namespace visslink
}  // namespace esphome
