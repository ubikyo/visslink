#pragma once

// Protocole Optolink KW, non bloquant : lecture, et écriture d'un octet.
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
// Écriture (v0.3, cf. docs/ecriture-kw.md) :
//
//   Vitotronic -> 0x05
//   VissLink   -> 0x01 0xF4 AH AL LEN D1..DLEN
//   Vitotronic -> 1 octet                  0x00 = accepté ; 0x05 ou autre = refus
//
// Le format et l'accusé 0x00 sont documentés (openv, vcontrold). La longueur
// de la réponse n'est concordante entre les sources que pour LEN = 1.
// v0.5 : écriture de 8 octets autorisée en plus (blocs horaires, chemin
// expérimental isolé). Réponse acceptée : un octet 00, comme vcontrold
// (« RECV 1 SR ») ; VitoWiFi attend 8 octets. Toute autre réponse est une
// issue indéterminée (RESPONSE_TOO_LONG, WRITE_REJECTED...) que seule une
// relecture tranche. Aucune autre longueur n'est acceptée.
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

// Le décodage des valeurs (types, little-endian) est dans decode.h : ce
// moteur ne transporte que des octets bruts.

enum class KwResult : uint8_t {
  NONE,                ///< pas de transaction terminée pendant cet appel
  SUCCESS,             ///< données valides dans data()
  SYNC_TIMEOUT,        ///< aucun 0x05 : interface absente ou chaudière muette
  RESPONSE_TIMEOUT,    ///< aucun octet après la requête
  RESPONSE_TOO_SHORT,  ///< réponse incomplète à l'expiration du délai
  RESPONSE_TOO_LONG,   ///< octets en trop : liaison désynchronisée
  WRITE_REJECTED,      ///< écriture : accusé différent de 0x00 (octet dans data())
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
    case KwResult::WRITE_REJECTED:
      return "write rejected";
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
  static constexpr uint8_t WRITE_COMMAND = 0xF4;
  /// Accusé d'une écriture acceptée (« Bestätigung OK », openv ; « OK »,
  /// vcontrold). Toute autre valeur est un refus.
  static constexpr uint8_t WRITE_ACK = 0x00;
  /// Écriture d'un octet : seule longueur pour laquelle openv, vcontrold et
  /// VitoWiFi s'accordent sur la réponse.
  static constexpr uint8_t SINGLE_WRITE_LENGTH = 1;
  /// Écriture d'un bloc horaire (v0.5, expérimental).
  static constexpr uint8_t BLOCK_WRITE_LENGTH = 8;
  static constexpr uint8_t MAX_WRITE_LENGTH = BLOCK_WRITE_LENGTH;
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
    this->write_ = false;
    this->deadline_ms_ = now + this->sync_timeout_ms_;
    this->state_ = State::WAIT_SYNC;
    return true;
  }

  /// Démarre une écriture de 1 ou 8 octets. Mêmes conditions que
  /// request_read() : une seule transaction à la fois, et la trame ne part
  /// qu'après un 0x05 frais. Aucune nouvelle tentative n'est faite ici.
  bool request_write(uint16_t address, const uint8_t *data, uint8_t length, uint32_t now) {
    if (this->state_ != State::IDLE || data == nullptr ||
        (length != SINGLE_WRITE_LENGTH && length != BLOCK_WRITE_LENGTH))
      return false;
    for (uint8_t i = 0; i < length; i++)
      this->write_data_[i] = data[i];
    this->address_ = address;
    this->write_length_ = length;
    this->expected_ = 1;  // accusé
    this->received_ = 0;
    this->write_ = true;
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
          if (this->write_) {
            uint8_t request[5 + MAX_WRITE_LENGTH] = {START_BYTE, WRITE_COMMAND, static_cast<uint8_t>(this->address_ >> 8),
                                                     static_cast<uint8_t>(this->address_), this->write_length_};
            for (uint8_t i = 0; i < this->write_length_; i++)
              request[5 + i] = this->write_data_[i];
            port.write_array(request, 5 + this->write_length_);
          } else {
            const uint8_t request[5] = {START_BYTE, READ_COMMAND, static_cast<uint8_t>(this->address_ >> 8),
                                        static_cast<uint8_t>(this->address_), this->expected_};
            port.write_array(request, sizeof(request));
          }
          this->sent_ms_ = now;
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
          // Écriture : réponse bien cadrée, mais seul 0x00 vaut accord. Un
          // refus rend l'état de la ligne incertain : resynchronisation.
          if (this->write_ && this->buffer_[0] != WRITE_ACK) {
            const uint8_t reply = this->buffer_[0];
            const KwResult result = this->fail_(KwResult::WRITE_REJECTED, now);
            this->buffer_[0] = reply;
            this->received_ = 1;
            return result;
          }
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
  /// Requête acceptée mais encore rien d'émis (attente du 0x05).
  bool is_waiting_sync() const { return this->state_ == State::WAIT_SYNC; }
  /// Instant d'émission de la trame de la transaction en cours ou de la
  /// dernière (sur le 0x05) : mesure des délais.
  uint32_t sent_ms() const { return this->sent_ms_; }

  /// Abandonne une requête qui n'a encore RIEN émis (WAIT_SYNC) : la ligne
  /// n'a vu aucun octet, aucune transaction n'est interrompue. Refusé dans
  /// tout autre état (trame émise, réponse attendue ou reprise en cours).
  bool cancel_before_send() {
    if (this->state_ != State::WAIT_SYNC)
      return false;
    this->state_ = State::IDLE;
    this->write_ = false;
    return true;
  }
  /// La transaction en cours (ou la dernière terminée) est une écriture.
  bool is_write() const { return this->write_; }
  State state() const { return this->state_; }
  /// Données de la dernière lecture réussie (ou accusé d'une écriture, y
  /// compris refusée), valides jusqu'à la requête suivante.
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
  bool write_{false};
  uint32_t sent_ms_{0};
  uint8_t write_length_{0};
  uint8_t write_data_[MAX_WRITE_LENGTH]{};
};

}  // namespace visslink
}  // namespace esphome
