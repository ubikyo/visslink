#pragma once

// Ordonnanceur de lectures : chaque registre a son propre intervalle.
//
// Indépendant d'ESPHome (temps passé en paramètre) pour être testé sur PC.
//
// Règles :
//  - une seule lecture en cours à la fois (aucun doublon possible) ;
//  - parmi les registres échus, le plus en retard passe en premier, à
//    égalité l'ordre de déclaration décide : un registre échu attend au plus
//    N-1 transactions, il ne peut pas être repoussé indéfiniment ;
//  - pas de rattrapage : si la chaudière est lente et qu'un cycle entier a
//    été manqué, l'échéance suivante repart de l'instant présent au lieu
//    d'empiler des lectures en retard ;
//  - un échec est retenté au plus `max_retries` fois, puis le registre est
//    replanifié à l'intervalle normal (il ne bloque pas les autres).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace visslink {

struct RegisterSlot {
  uint16_t address{0};
  uint8_t length{0};
  uint32_t interval_ms{0};

  uint32_t cycle_due_ms{0};  ///< échéance nominale du cycle en cours
  uint32_t next_due_ms{0};   ///< prochaine tentative (cycle ou nouvelle tentative)
  uint32_t last_success_ms{0};
  bool has_succeeded{false};
  bool in_flight{false};
  uint8_t failures{0};  ///< échecs consécutifs dans le cycle en cours
};

class RegisterScheduler {
 public:
  static constexpr int NONE = -1;

  void reserve(size_t count) { this->slots_.reserve(count); }
  /// Ajoute un registre et renvoie son index. À appeler pendant setup().
  size_t add(uint16_t address, uint8_t length, uint32_t interval_ms);

  void set_max_retries(uint8_t max_retries) { this->max_retries_ = max_retries; }
  void set_retry_delay(uint32_t retry_delay_ms) { this->retry_delay_ms_ = retry_delay_ms; }

  /// Rend tous les registres échus immédiatement.
  void start(uint32_t now);

  /// Renvoie l'index du registre à lire maintenant, ou NONE. Le registre
  /// renvoyé est marqué « en cours » jusqu'à on_success()/on_failure().
  int acquire_next(uint32_t now);

  void on_success(size_t index, uint32_t now);
  /// Renvoie true si les nouvelles tentatives sont épuisées pour ce cycle.
  bool on_failure(size_t index, uint32_t now);

  uint8_t max_retries() const { return this->max_retries_; }
  bool busy() const { return this->in_flight_ != NONE; }
  size_t size() const { return this->slots_.size(); }
  const RegisterSlot &slot(size_t index) const { return this->slots_[index]; }

 protected:
  void schedule_next_cycle_(RegisterSlot &slot, uint32_t now);

  std::vector<RegisterSlot> slots_;
  int in_flight_{NONE};
  uint8_t max_retries_{2};
  uint32_t retry_delay_ms_{1000};
};

}  // namespace visslink
}  // namespace esphome
