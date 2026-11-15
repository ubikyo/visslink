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
//    le registre attend son intervalle normal ;
//  - un registre peut couvrir plusieurs blocs (programmation horaire : un bloc
//    par jour). Il reste une seule entrée : après chaque bloc, il revient
//    après BLOCK_DELAY_MS, en concurrence normale avec les autres registres,
//    et ne reprend son intervalle qu'après le dernier bloc. Le nombre
//    d'entrées, donc l'attente maximale des autres registres, est inchangé ;
//  - expedite() (v0.3) rend un registre échu immédiatement et prioritaire
//    pour une seule lecture : relecture de vérification après une écriture.
//    Il ne peut y en avoir qu'une à la fois (une écriture à la fois), donc
//    l'attente des autres registres augmente au plus d'une lecture.

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
  /// Pause entre deux blocs d'un même registre : étale les 7 lectures d'une
  /// programmation sur ~4 min au lieu d'une rafale. Valeur choisie par
  /// simulation (cf. tests sim_v021_* et README, « Charge de la liaison »).
  static constexpr uint32_t BLOCK_DELAY_MS = 30000;

  struct Entry {
    uint32_t interval_ms;
    uint32_t due_ms;    ///< prochaine tentative
    uint32_t cycle_ms;  ///< échéance du premier bloc du cycle (registres multi-blocs)
    uint8_t failures;   ///< échecs consécutifs dans le cycle en cours
    uint8_t blocks;     ///< nombre de blocs lus à chaque cycle (>= 1)
    uint8_t block;      ///< bloc à lire
    bool urgent;        ///< relecture demandée par expedite(), servie en premier
  };

  void set_max_retries(uint8_t max_retries) { this->max_retries_ = max_retries; }
  uint8_t max_retries() const { return this->max_retries_; }

  /// Ajoute un registre de `blocks` blocs, échu à `now`. À appeler depuis
  /// setup() : loop() n'alloue jamais.
  void add(uint32_t interval_ms, uint32_t now, uint8_t blocks = 1) {
    this->entries_.push_back({interval_ms, now, now, 0, blocks > 0 ? blocks : uint8_t(1), 0, false});
  }

  /// Index du registre à lire maintenant (marqué en cours), ou NONE.
  int acquire(uint32_t now) {
    if (this->current_ != NONE)
      return NONE;
    uint32_t best_lateness = 0;
    for (size_t i = 0; i < this->entries_.size(); i++) {
      if (this->entries_[i].urgent && time_reached(now, this->entries_[i].due_ms)) {
        this->current_ = static_cast<int>(i);
        return this->current_;
      }
    }
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
      // Une relecture urgente garde sa priorité pendant ses nouvelles tentatives.
      e.failures++;
      e.due_ms = now + RETRY_DELAY_MS;
      return false;
    }
    const bool gave_up = !success;
    e.failures = 0;
    if (e.urgent) {
      // Relecture hors cycle : la cadence normale du registre reprend.
      e.urgent = false;
      e.due_ms = e.cycle_ms;
      if (time_reached(now, e.due_ms))
        e.due_ms = now + e.interval_ms;
      return gave_up;
    }
    // Bloc suivant, y compris après un abandon : un jour illisible ne prive
    // pas les autres de leur lecture.
    if (e.block + 1 < e.blocks) {
      if (e.block == 0)
        e.cycle_ms = e.due_ms;
      e.block++;
      e.due_ms = now + BLOCK_DELAY_MS;
      return gave_up;
    }
    if (e.block > 0) {
      e.block = 0;
      e.due_ms = e.cycle_ms;  // cadence calée sur le début du cycle
    }
    e.due_ms += e.interval_ms;
    if (time_reached(now, e.due_ms))
      e.due_ms = now + e.interval_ms;
    return gave_up;
  }

  /// Lecture prioritaire et immédiate d'un registre à un seul bloc (sans
  /// effet sur un registre multi-blocs ou en cours de lecture). Sa cadence
  /// normale reprend ensuite : l'échéance prévue est conservée.
  bool expedite(size_t index, uint32_t now) {
    if (index >= this->entries_.size() || static_cast<int>(index) == this->current_)
      return false;
    Entry &e = this->entries_[index];
    if (e.blocks != 1)
      return false;
    if (!e.urgent)
      e.cycle_ms = e.due_ms;  // échéance normale, mise de côté
    e.urgent = true;
    e.failures = 0;
    e.due_ms = now;
    return true;
  }

  const Entry &entry(size_t index) const { return this->entries_[index]; }
  /// Bloc à lire (ou en cours de lecture) pour un registre.
  uint8_t block(size_t index) const { return this->entries_[index].block; }

 protected:
  std::vector<Entry> entries_;
  int current_{NONE};
  uint8_t max_retries_{2};
};

}  // namespace visslink
}  // namespace esphome
