#include "kw_protocol.h"
#include "time_utils.h"

namespace esphome {
namespace visslink {

const char *kw_result_to_string(KwResult result) {
  switch (result) {
    case KwResult::NONE:
      return "none";
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
  }
  return "unknown";
}

const char *kw_state_to_string(KwState state) {
  switch (state) {
    case KwState::INIT:
      return "INIT";
    case KwState::IDLE:
      return "IDLE";
    case KwState::WAIT_SYNC:
      return "WAIT_SYNC";
    case KwState::WAIT_RESPONSE:
      return "WAIT_RESPONSE";
    case KwState::CHECK_TRAILING:
      return "CHECK_TRAILING";
    case KwState::RECOVERY:
      return "RECOVERY";
  }
  return "UNKNOWN";
}

bool KwProtocol::request_read(uint16_t address, uint8_t length, uint32_t now) {
  if (this->state_ != KwState::IDLE)
    return false;
  if (length == 0 || length > MAX_READ_LENGTH)
    return false;
  this->address_ = address;
  this->expected_ = length;
  this->received_ = 0;
  this->deadline_ms_ = now + this->sync_timeout_ms_;
  this->state_ = KwState::WAIT_SYNC;
  return true;
}

void KwProtocol::drain_() {
  uint8_t byte;
  while (this->port_->available() > 0 && this->port_->read_byte(&byte))
    this->stats_.discarded_bytes++;
}

void KwProtocol::enter_recovery_(uint32_t now) {
  this->state_ = KwState::RECOVERY;
  this->deadline_ms_ = now + RECOVERY_QUIET_MS;
}

KwResult KwProtocol::finish_(KwResult result, uint32_t now) {
  switch (result) {
    case KwResult::SUCCESS:
      this->stats_.success++;
      this->state_ = KwState::IDLE;
      return result;
    case KwResult::SYNC_TIMEOUT:
      this->stats_.sync_timeouts++;
      break;
    case KwResult::RESPONSE_TIMEOUT:
      this->stats_.response_timeouts++;
      break;
    case KwResult::RESPONSE_TOO_SHORT:
      this->stats_.too_short++;
      break;
    case KwResult::RESPONSE_TOO_LONG:
      this->stats_.too_long++;
      break;
    case KwResult::NONE:
      break;
  }
  // Toute erreur invalide l'état de la liaison : on vide la ligne et on
  // attendra un nouveau 0x05 pour la prochaine requête.
  this->received_ = 0;
  this->enter_recovery_(now);
  return result;
}

KwResult KwProtocol::loop(uint32_t now) {
  switch (this->state_) {
    case KwState::INIT:
      // Ignore ce qui a pu être reçu avant le démarrage du composant.
      this->drain_();
      this->state_ = KwState::IDLE;
      return KwResult::NONE;

    case KwState::IDLE:
      // Hors transaction, la régulation émet ses 0x05 : on les consomme
      // pour ne jamais répondre plus tard à une synchro périmée.
      this->drain_();
      return KwResult::NONE;

    case KwState::WAIT_SYNC: {
      // On ne répond qu'à un 0x05 frais : le dernier octet lu doit être 0x05
      // et le tampon de réception doit être vide ensuite.
      bool sync = false;
      uint8_t byte;
      while (this->port_->available() > 0 && this->port_->read_byte(&byte)) {
        if (sync)
          this->stats_.discarded_bytes++;  // 0x05 précédent périmé
        sync = (byte == SYNC_BYTE);
        if (!sync)
          this->stats_.discarded_bytes++;
      }
      if (sync) {
        const uint8_t request[5] = {START_BYTE, READ_COMMAND, static_cast<uint8_t>(this->address_ >> 8),
                                    static_cast<uint8_t>(this->address_ & 0xFF), this->expected_};
        this->port_->write(request, sizeof(request));
        this->received_ = 0;
        this->deadline_ms_ = now + this->response_timeout_ms_;
        this->state_ = KwState::WAIT_RESPONSE;
        return KwResult::NONE;
      }
      if (time_reached(now, this->deadline_ms_))
        return this->finish_(KwResult::SYNC_TIMEOUT, now);
      return KwResult::NONE;
    }

    case KwState::WAIT_RESPONSE: {
      uint8_t byte;
      while (this->received_ < this->expected_ && this->port_->available() > 0 && this->port_->read_byte(&byte))
        this->buffer_[this->received_++] = byte;
      if (this->received_ == this->expected_) {
        this->deadline_ms_ = now + TRAILING_WINDOW_MS;
        this->state_ = KwState::CHECK_TRAILING;
        return this->loop(now);  // contrôle immédiat d'octets déjà reçus en trop
      }
      if (time_reached(now, this->deadline_ms_))
        return this->finish_(this->received_ == 0 ? KwResult::RESPONSE_TIMEOUT : KwResult::RESPONSE_TOO_SHORT, now);
      return KwResult::NONE;
    }

    case KwState::CHECK_TRAILING: {
      // Le protocole KW n'a ni en-tête ni somme de contrôle : la seule façon
      // de détecter une désynchronisation est de vérifier qu'aucun octet
      // supplémentaire ne suit la réponse.
      // Exception : un unique 0x05 est la synchro suivante de la régulation,
      // il est ignoré et la réponse reste valide.
      uint8_t byte;
      while (this->port_->available() > 0 && this->port_->read_byte(&byte)) {
        if (byte != SYNC_BYTE || this->port_->available() > 0)
          return this->finish_(KwResult::RESPONSE_TOO_LONG, now);
        this->stats_.discarded_bytes++;
        return this->finish_(KwResult::SUCCESS, now);
      }
      if (time_reached(now, this->deadline_ms_))
        return this->finish_(KwResult::SUCCESS, now);
      return KwResult::NONE;
    }

    case KwState::RECOVERY: {
      // Vide la ligne jusqu'à obtenir RECOVERY_QUIET_MS de silence.
      if (this->port_->available() > 0) {
        this->drain_();
        this->deadline_ms_ = now + RECOVERY_QUIET_MS;
        return KwResult::NONE;
      }
      if (time_reached(now, this->deadline_ms_))
        this->state_ = KwState::IDLE;
      return KwResult::NONE;
    }
  }
  return KwResult::NONE;
}

}  // namespace visslink
}  // namespace esphome
