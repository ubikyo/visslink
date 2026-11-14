#pragma once

// Orchestration des lectures : relie l'ordonnanceur et le moteur KW.
//
// Indépendant d'ESPHome : le composant se contente d'appeler loop() et de
// publier les événements renvoyés. Les tests de simulation exercent donc
// exactement la même logique que le micrologiciel.

#include <cstdint>

#include "kw_protocol.h"
#include "scheduler.h"

namespace esphome {
namespace visslink {

/// Résultat d'une transaction terminée.
struct ReadEvent {
  size_t index{0};       ///< index du registre dans l'ordonnanceur
  KwResult result{KwResult::NONE};
  uint8_t attempt{0};    ///< numéro de la tentative (1 = première)
  bool exhausted{false}; ///< échec et plus de nouvelle tentative pour ce cycle
  uint8_t length{0};
  uint8_t data[KwProtocol::MAX_READ_LENGTH]{};  ///< valide si result == SUCCESS
};

class ReadEngine {
 public:
  explicit ReadEngine(SerialPort *port) : protocol_(port) {}

  KwProtocol &protocol() { return this->protocol_; }
  RegisterScheduler &scheduler() { return this->scheduler_; }
  const KwProtocol &protocol() const { return this->protocol_; }
  const RegisterScheduler &scheduler() const { return this->scheduler_; }

  /// Fait avancer la communication. Renvoie true et remplit `event` quand
  /// une transaction vient de se terminer.
  bool loop(uint32_t now, ReadEvent *event);

  bool transaction_active() const { return this->protocol_.is_transaction_active(); }

 protected:
  void start_next_(uint32_t now);

  KwProtocol protocol_;
  RegisterScheduler scheduler_;
  int current_{RegisterScheduler::NONE};
};

}  // namespace visslink
}  // namespace esphome
