#pragma once

// Protocole Optolink KW, lecture seule, non bloquant.
//
// Indépendant d'ESPHome pour être testé sur PC : le temps est passé en
// paramètre et loop() accepte tout port série exposant available(),
// read_byte(uint8_t *) et write_array(const uint8_t *, size_t), c'est-à-dire
// uart::UARTDevice sur l'ESP8266 et un faux port dans les tests.
//
// Déroulement d'une lecture :
//
//   Vitotronic -> 0x05                     synchro, ~toutes les 2 s au repos
//   VissLink   -> 0x01 0xF7 AH AL LEN      début + lecture, adresse, longueur
//   Vitotronic -> LEN octets               données brutes, sans en-tête ni CRC
//
//   IDLE -> WAIT_SYNC -> WAIT_RESPONSE -> CHECK_TRAILING -> IDLE
//              |               |                |
//              +---------------+----------------+--> RECOVERY -> IDLE
//
// IDLE est l'état initial : il vide aussi ce qui a pu être reçu avant le
// démarrage.

#include <cstddef>
#include <cstdint>

#include "time_utils.h"

namespace esphome {
namespace visslink {

/// Les valeurs KW sont transmises octet de poids faible en premier.
/// "D7 00" -> 215, "CC FF" -> -52.
inline int16_t decode_int16_le(const uint8_t *data) {
  return static_cast<int16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

enum class KwResult : uint8_t {
  NONE,                ///< pas de transaction terminée pendant cet appel
  SUCCESS,             ///< données valides dans data()
  SYNC_TIMEOUT,        ///< aucun 0x05 : interface absente ou chaudière muette
  RESPONSE_TIMEOUT,    ///< aucun octet après la requête
  RESPONSE_TOO_SHORT,  ///< réponse incomplète à l'expiration du délai
  RESPONSE_TOO_LONG,   ///< octets en trop : liaison désynchronisée
};

inline const char *kw_result_to_string(KwResult result) {
  switch (result) {
    case KwResult::SUCCESS:
      return "success";
    case KwResult::SYNC_TIMEOUT:
      return "sync timeout";
    case KwResult::RESPONSE_TIMEOUT:
      return "response timeout";
    case KwResult::RESPONSE_TOO_SHORT:
      return "response too short";
    case KwResult::RESPONSE_TOO_LONG:
      return "response too long";
    default:
      return "none";
  }
}

class KwProtocol {
 public:
  enum class State : uint8_t { RECOVERY, IDLE, WAIT_SYNC, WAIT_RESPONSE, CHECK_TRAILING };

  static constexpr uint8_t SYNC_BYTE = 0x05;
  static constexpr uint8_t START_BYTE = 0x01;
  static constexpr uint8_t READ_COMMAND = 0xF7;
  /// 8 octets : taille des futurs blocs de programmation horaire.
  static constexpr uint8_t MAX_READ_LENGTH = 8;
  /// Silence exigé après la réponse. Un octet dure ~2,5 ms à 4800 bauds 8E2 :
  /// un octet en trop arriverait bien avant la fin de cette fenêtre.
  static constexpr uint32_t TRAILING_WINDOW_MS = 25;
  /// Silence exigé pour sortir de RECOVERY.
  static constexpr uint32_t RECOVERY_QUIET_MS = 200;

  void set_sync_timeout(uint32_t ms) { this->sync_timeout_ms_ = ms; }
  void set_response_timeout(uint32_t ms) { this->response_timeout_ms_ = ms; }

  /// Démarre une lecture. Refusée hors de l'état IDLE (une seule transaction
  /// à la fois) ou si la longueur est invalide.
  bool request_read(uint16_t address, uint8_t length, uint32_t now) {
    if (this->state_ != State::IDLE || length == 0 || length > MAX_READ_LENGTH)
      return false;
    this->address_ = address;
    this->expected_ = length;
    this->received_ = 0;
    this->deadline_ms_ = now + this->sync_timeout_ms_;
    this->state_ = State::WAIT_SYNC;
    return true;
  }

  /// À appeler à chaque passage dans loop(). Ne bloque jamais : traite les
  /// octets déjà reçus puis rend la main. Renvoie le résultat de la
  /// transaction au moment où elle se termine, NONE sinon.
  template<typename Port> KwResult loop(uint32_t now, Port &port) {
    uint8_t byte;
    switch (this->state_) {
      case State::IDLE:
        // Consomme les 0x05 reçus hors transaction : on ne répondra jamais
        // plus tard à une synchro périmée.
        drain_(port);
        return KwResult::NONE;

      case State::WAIT_SYNC: {
        // Ne répondre qu'à un 0x05 frais : dernier octet lu, tampon vide.
        bool sync = false;
        while (port.available() > 0 && port.read_byte(&byte))
          sync = (byte == SYNC_BYTE);
        if (sync) {
          const uint8_t request[5] = {START_BYTE, READ_COMMAND, static_cast<uint8_t>(this->address_ >> 8),
                                      static_cast<uint8_t>(this->address_), this->expected_};
          port.write_array(request, sizeof(request));
          this->deadline_ms_ = now + this->response_timeout_ms_;
          this->state_ = State::WAIT_RESPONSE;
          return KwResult::NONE;
        }
        if (time_reached(now, this->deadline_ms_))
          return this->fail_(KwResult::SYNC_TIMEOUT, now);
        return KwResult::NONE;
      }

      case State::WAIT_RESPONSE:
        while (this->received_ < this->expected_ && port.available() > 0 && port.read_byte(&byte))
          this->buffer_[this->received_++] = byte;
        if (this->received_ == this->expected_) {
          this->deadline_ms_ = now + TRAILING_WINDOW_MS;
          this->state_ = State::CHECK_TRAILING;
          return this->loop(now, port);  // octets en trop déjà présents ?
        }
        if (time_reached(now, this->deadline_ms_))
          return this->fail_(this->received_ == 0 ? KwResult::RESPONSE_TIMEOUT : KwResult::RESPONSE_TOO_SHORT, now);
        return KwResult::NONE;

      case State::CHECK_TRAILING:
        // Sans en-tête ni somme de contrôle, le seul contrôle possible d'une
        // réponse KW est son cadrage : exactement LEN octets, puis le silence.
        if (port.available() > 0)
          return this->fail_(KwResult::RESPONSE_TOO_LONG, now);
        if (time_reached(now, this->deadline_ms_)) {
          this->state_ = State::IDLE;
          return KwResult::SUCCESS;
        }
        return KwResult::NONE;

      case State::RECOVERY:
        if (port.available() > 0) {
          drain_(port);
          this->deadline_ms_ = now + RECOVERY_QUIET_MS;
        } else if (time_reached(now, this->deadline_ms_)) {
          this->state_ = State::IDLE;
        }
        return KwResult::NONE;
    }
    return KwResult::NONE;
  }

  bool is_idle() const { return this->state_ == State::IDLE; }
  State state() const { return this->state_; }
  /// Données de la dernière lecture réussie, valides jusqu'à la requête suivante.
  const uint8_t *data() const { return this->buffer_; }
  uint8_t data_length() const { return this->received_; }

 protected:
  template<typename Port> static void drain_(Port &port) {
    uint8_t byte;
    while (port.available() > 0 && port.read_byte(&byte)) {
    }
  }

  /// Toute erreur rend l'état de la ligne incertain : on la vide, puis la
  /// requête suivante attendra un nouveau 0x05 (resynchronisation).
  KwResult fail_(KwResult result, uint32_t now) {
    this->received_ = 0;
    this->deadline_ms_ = now + RECOVERY_QUIET_MS;
    this->state_ = State::RECOVERY;
    return result;
  }

  State state_{State::IDLE};
  uint32_t sync_timeout_ms_{10000};
  uint32_t response_timeout_ms_{1000};
  uint32_t deadline_ms_{0};
  uint16_t address_{0};
  uint8_t expected_{0};
  uint8_t received_{0};
  uint8_t buffer_[MAX_READ_LENGTH]{};
};

}  // namespace visslink
}  // namespace esphome
