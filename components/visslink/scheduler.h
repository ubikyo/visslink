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
//    l'attente des autres registres augmente au plus d'une lecture ;
//  - v0.4.1 : acquire(now, true) ne délivre que les relectures urgentes
//    (aucune lecture ordinaire intercalée tant qu'une vérification est en
//    cours) ; release() rend une lecture acquise mais jamais émise, sans
//    toucher à son échéance (préemption par une commande manuelle) ;
//  - v0.5.1 : un cycle multi-blocs peut commencer par un bloc choisi
//    (set_cycle_start : le jour courant d'abord) et parcourt ensuite les
//    blocs en rond ; expedite_block() relit un seul bloc hors cycle, avec une
//    priorité inférieure aux relectures de vérification (URGENT_REFRESH) ou
//    égale (URGENT_VERIFY), puis le cycle reprend là où il en était ; la
//    pause entre blocs est réglable (set_block_delay), 30 s par défaut.

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
  /// Pause par défaut entre deux blocs d'un même registre : étale les 7
  /// lectures d'une programmation sur ~3 min au lieu d'une rafale. Valeur
  /// choisie par simulation (cf. tests sim_v021_* et sim_v051_*).
  static constexpr uint32_t BLOCK_DELAY_MS = 30000;

  /// Niveaux de priorité d'une relecture hors cycle.
  static constexpr uint8_t URGENT_NONE = 0;
  /// Rafraîchissement d'un bloc (jour courant, demande ponctuelle) : avant
  /// les lectures ordinaires, jamais pendant une vérification d'écriture.
  static constexpr uint8_t URGENT_REFRESH = 1;
  /// Relecture de vérification après une écriture (expedite) : en premier.
  static constexpr uint8_t URGENT_VERIFY = 2;

  struct Entry {
    uint32_t interval_ms;
    uint32_t due_ms;    ///< prochaine tentative
    uint32_t cycle_ms;  ///< échéance du premier bloc du cycle (registres multi-blocs)
    uint8_t failures;   ///< échecs consécutifs dans le cycle en cours
    uint8_t blocks;     ///< nombre de blocs lus à chaque cycle (>= 1)
    uint8_t block;      ///< bloc à lire (celui de la relecture hors cycle si urgent)
    uint8_t urgent;     ///< URGENT_NONE, URGENT_REFRESH ou URGENT_VERIFY
    uint8_t start;      ///< premier bloc du cycle (multi-blocs)
    uint8_t done;       ///< blocs terminés dans le cycle en cours
    // État du cycle mis de côté pendant une relecture hors cycle.
    uint32_t saved_due_ms;
    uint8_t saved_block;
    uint8_t saved_failures;
  };

  void set_max_retries(uint8_t max_retries) { this->max_retries_ = max_retries; }
  uint8_t max_retries() const { return this->max_retries_; }
  void set_block_delay(uint32_t delay_ms) { this->block_delay_ms_ = delay_ms; }
  uint32_t block_delay() const { return this->block_delay_ms_; }

  /// Ajoute un registre de `blocks` blocs, échu à `now`. À appeler depuis
  /// setup() : loop() n'alloue jamais.
  void add(uint32_t interval_ms, uint32_t now, uint8_t blocks = 1) {
    Entry e{};
    e.interval_ms = interval_ms;
    e.due_ms = now;
    e.cycle_ms = now;
    e.blocks = blocks > 0 ? blocks : uint8_t(1);
    this->entries_.push_back(e);
  }

  /// Index du registre à lire maintenant (marqué en cours), ou NONE.
  /// `urgent_only` : seulement une relecture de vérification (expedite()).
  int acquire(uint32_t now, bool urgent_only = false) {
    if (this->current_ != NONE)
      return NONE;
    // Vérifications d'abord, la plus ancienne demandée en premier (le
    // registre écrit avant l'autre circuit) ; puis les rafraîchissements de
    // bloc ; puis les lectures ordinaires.
    int index = this->most_late_(now, URGENT_VERIFY);
    if (index == NONE && !urgent_only)
      index = this->most_late_(now, URGENT_REFRESH);
    if (index == NONE && !urgent_only)
      index = this->most_late_(now, URGENT_NONE);
    this->current_ = index;
    return index;
  }

  /// Rend la lecture en cours sans l'avoir faite (aucun octet émis) : ni
  /// succès ni échec, échéance et tentatives inchangées. Le registre reste
  /// échu et sera relu dès que possible.
  void release() { this->current_ = NONE; }

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
    if (e.urgent != URGENT_NONE) {
      // Relecture hors cycle : le cycle reprend là où il en était.
      e.urgent = URGENT_NONE;
      e.block = e.saved_block;
      e.failures = e.saved_failures;
      e.due_ms = e.saved_due_ms;
      if (e.blocks == 1) {
        if (time_reached(now, e.due_ms))
          e.due_ms = now + e.interval_ms;  // lecture prévue déjà faite
      } else if (time_reached(now + this->block_delay_ms_, e.due_ms)) {
        e.due_ms = now + this->block_delay_ms_;  // pas deux blocs à la suite
      }
      return gave_up;
    }
    // Bloc suivant, y compris après un abandon : un jour illisible ne prive
    // pas les autres de leur lecture.
    if (e.done + 1 < e.blocks) {
      if (e.done == 0)
        e.cycle_ms = e.due_ms;
      e.done++;
      e.block = static_cast<uint8_t>((e.block + 1) % e.blocks);
      e.due_ms = now + this->block_delay_ms_;
      return gave_up;
    }
    if (e.done > 0) {
      e.done = 0;
      e.due_ms = e.cycle_ms;  // cadence calée sur le début du cycle
    }
    e.block = e.start;
    e.due_ms += e.interval_ms;
    if (time_reached(now, e.due_ms))
      e.due_ms = now + e.interval_ms;
    return gave_up;
  }

  /// Registre multi-blocs : bloc par lequel commencera le cycle, tant
  /// qu'aucun bloc du cycle n'a été lu (sinon refusé, l'ordre d'un cycle
  /// commencé ne change pas). Les cycles suivants commencent au même bloc,
  /// jusqu'au prochain appel.
  bool set_cycle_start(size_t index, uint8_t block) {
    if (index >= this->entries_.size() || static_cast<int>(index) == this->current_)
      return false;
    Entry &e = this->entries_[index];
    if (e.blocks < 2 || block >= e.blocks || e.done != 0 || e.urgent != URGENT_NONE || e.failures != 0)
      return false;
    e.start = block;
    e.block = block;
    return true;
  }

  /// Lecture prioritaire et immédiate d'un registre à un seul bloc (sans
  /// effet sur un registre multi-blocs ou en cours de lecture). Sa cadence
  /// normale reprend ensuite : l'échéance prévue est conservée.
  bool expedite(size_t index, uint32_t now) {
    if (index >= this->entries_.size() || this->entries_[index].blocks != 1)
      return false;
    return this->expedite_block(index, 0, now, URGENT_VERIFY);
  }

  /// Relecture immédiate d'un seul bloc, hors cycle (v0.5.1). Refusée si le
  /// registre est en cours de lecture ou si une autre relecture hors cycle
  /// est déjà demandée pour lui (une seule à la fois par registre). Le cycle
  /// en cours n'est ni avancé ni recommencé.
  bool expedite_block(size_t index, uint8_t block, uint32_t now, uint8_t level = URGENT_REFRESH) {
    if (index >= this->entries_.size() || static_cast<int>(index) == this->current_ || level == URGENT_NONE)
      return false;
    Entry &e = this->entries_[index];
    if (block >= e.blocks)
      return false;
    if (e.urgent != URGENT_NONE) {
      if (e.block != block)
        return false;
    } else {
      e.saved_due_ms = e.due_ms;  // échéance normale, mise de côté
      e.saved_block = e.block;
      e.saved_failures = e.failures;
    }
    if (level > e.urgent)
      e.urgent = level;
    e.block = block;
    e.failures = 0;
    e.due_ms = now;
    return true;
  }

  /// Retire un rafraîchissement de ce bloc pas encore commencé (rien
  /// d'émis) : le cycle reprend comme si rien n'avait été demandé. Sans
  /// effet sur une vérification (URGENT_VERIFY), un autre bloc ou une
  /// lecture en cours.
  bool cancel_refresh(size_t index, uint8_t block) {
    if (index >= this->entries_.size() || static_cast<int>(index) == this->current_)
      return false;
    Entry &e = this->entries_[index];
    if (e.urgent != URGENT_REFRESH || e.block != block)
      return false;
    e.urgent = URGENT_NONE;
    e.block = e.saved_block;
    e.failures = e.saved_failures;
    e.due_ms = e.saved_due_ms;
    return true;
  }
  /// v0.8 : rafraîchissement manuel. Le prochain cycle complet du registre
  /// commence maintenant (blocs toujours espacés de block_delay) ; sa cadence
  /// repart de cet instant. Sans effet (true) si un cycle est déjà commencé ;
  /// false pendant une relecture hors cycle ou de nouvelles tentatives.
  bool request_cycle(size_t index, uint32_t now) {
    if (index >= this->entries_.size())
      return false;
    Entry &e = this->entries_[index];
    if (e.done != 0 || static_cast<int>(index) == this->current_)
      return true;  // cycle déjà en cours
    if (e.urgent != URGENT_NONE || e.failures != 0)
      return false;
    e.due_ms = now;
    return true;
  }

  /// Un rafraîchissement de ce bloc est en attente (ou en cours).
  bool refresh_pending(size_t index, uint8_t block) const {
    return index < this->entries_.size() && this->entries_[index].urgent == URGENT_REFRESH &&
           this->entries_[index].block == block;
  }

  const Entry &entry(size_t index) const { return this->entries_[index]; }
  size_t size() const { return this->entries_.size(); }
  /// Bloc à lire (ou en cours de lecture) pour un registre.
  uint8_t block(size_t index) const { return this->entries_[index].block; }

 protected:
  /// Registre échu de ce niveau le plus en retard (à égalité, le premier
  /// déclaré), ou NONE.
  int most_late_(uint32_t now, uint8_t level) const {
    int best = NONE;
    uint32_t best_lateness = 0;
    for (size_t i = 0; i < this->entries_.size(); i++) {
      const Entry &e = this->entries_[i];
      if (e.urgent != level || !time_reached(now, e.due_ms))
        continue;
      const uint32_t lateness = now - e.due_ms;
      if (best == NONE || lateness > best_lateness) {
        best = static_cast<int>(i);
        best_lateness = lateness;
      }
    }
    return best;
  }

  std::vector<Entry> entries_;
  int current_{NONE};
  uint8_t max_retries_{2};
  uint32_t block_delay_ms_{BLOCK_DELAY_MS};
};

}  // namespace visslink
}  // namespace esphome
