#pragma once

// Moteur de communication Optolink, protocole KW, en lecture seule.
//
// Indépendant d'ESPHome : l'accès série passe par l'interface SerialPort et
// le temps est fourni par l'appelant, ce qui permet de simuler la chaudière
// dans les tests unitaires.
//
// Déroulement d'une lecture KW :
//
//   régulation -> 0x05                     (synchronisation, ~toutes les 2 s)
//   VissLink   -> 0x01 0xF7 AH AL LEN      (début + lecture, adresse, longueur)
//   régulation -> LEN octets de données    (sans en-tête ni somme de contrôle)
//
// Machine à états :
//
//   INIT -> IDLE -> WAIT_SYNC -> WAIT_RESPONSE -> CHECK_TRAILING -> IDLE
//                       |              |                |
//                       +--------------+----------------+--> RECOVERY -> IDLE
//
// Aucune fonction n'est bloquante : loop() traite les octets disponibles
// puis rend la main.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace visslink {

/// Accès minimal au port série. available() doit être testé avant
/// read_byte(), qui peut sinon attendre sur certaines plateformes.
class SerialPort {
 public:
  virtual size_t available() = 0;
  virtual bool read_byte(uint8_t *byte) = 0;
  virtual void write(const uint8_t *data, size_t length) = 0;

 protected:
  ~SerialPort() = default;
};

enum class KwState : uint8_t {
  INIT,
  IDLE,
  WAIT_SYNC,
  WAIT_RESPONSE,
  CHECK_TRAILING,
  RECOVERY,
};

enum class KwResult : uint8_t {
  NONE,               ///< aucune transaction terminée pendant cet appel
  SUCCESS,            ///< données valides disponibles via data()
  SYNC_TIMEOUT,       ///< aucun 0x05 reçu (interface absente, chaudière muette)
  RESPONSE_TIMEOUT,   ///< synchro OK mais aucun octet de réponse
  RESPONSE_TOO_SHORT, ///< réponse incomplète à l'expiration du délai
  RESPONSE_TOO_LONG,  ///< plus d'octets que demandé : désynchronisation
};

const char *kw_result_to_string(KwResult result);
const char *kw_state_to_string(KwState state);

struct KwStats {
  uint32_t success{0};
  uint32_t sync_timeouts{0};
  uint32_t response_timeouts{0};
  uint32_t too_short{0};
  uint32_t too_long{0};
  uint32_t discarded_bytes{0};  ///< octets parasites ignorés
};

class KwProtocol {
 public:
  static constexpr uint8_t SYNC_BYTE = 0x05;
  static constexpr uint8_t START_BYTE = 0x01;
  static constexpr uint8_t READ_COMMAND = 0xF7;
  static constexpr uint8_t MAX_READ_LENGTH = 4;
  /// Fenêtre de silence attendue après le dernier octet de réponse. Un octet
  /// dure ~2,5 ms à 4800 bauds (8E2) : une réponse trop longue arriverait
  /// bien avant la fin de cette fenêtre.
  static constexpr uint32_t TRAILING_WINDOW_MS = 25;
  /// Durée de silence requise pour sortir de RECOVERY.
  static constexpr uint32_t RECOVERY_QUIET_MS = 200;

  explicit KwProtocol(SerialPort *port) : port_(port) {}

  void set_sync_timeout(uint32_t ms) { this->sync_timeout_ms_ = ms; }
  void set_response_timeout(uint32_t ms) { this->response_timeout_ms_ = ms; }

  /// Démarre une lecture. Refusée (false) si une transaction est en cours ou
  /// si la longueur est invalide : jamais deux requêtes simultanées.
  bool request_read(uint16_t address, uint8_t length, uint32_t now);

  /// À appeler à chaque passage dans loop(). Renvoie le résultat de la
  /// transaction lorsqu'elle se termine, NONE sinon.
  KwResult loop(uint32_t now);

  bool is_idle() const { return this->state_ == KwState::IDLE; }
  /// Vrai lorsqu'une réactivité maximale de loop() est utile.
  bool is_transaction_active() const {
    return this->state_ == KwState::WAIT_SYNC || this->state_ == KwState::WAIT_RESPONSE ||
           this->state_ == KwState::CHECK_TRAILING;
  }
  KwState state() const { return this->state_; }
  const uint8_t *data() const { return this->buffer_; }
  uint8_t data_length() const { return this->received_; }
  const KwStats &stats() const { return this->stats_; }

 protected:
  void drain_();
  void enter_recovery_(uint32_t now);
  KwResult finish_(KwResult result, uint32_t now);

  SerialPort *port_;
  KwState state_{KwState::INIT};
  uint32_t sync_timeout_ms_{10000};
  uint32_t response_timeout_ms_{1000};
  uint32_t deadline_ms_{0};

  uint16_t address_{0};
  uint8_t expected_{0};
  uint8_t received_{0};
  uint8_t buffer_[MAX_READ_LENGTH]{};
  KwStats stats_;
};

}  // namespace visslink
}  // namespace esphome
