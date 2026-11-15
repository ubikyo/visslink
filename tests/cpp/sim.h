#pragma once
// Port série et chaudière Vitotronic simulés pour les tests du protocole KW.

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace vltest {

/// Port série en mémoire, mêmes méthodes que uart::UARTDevice.
/// `rx` = octets émis par la chaudière, `tx` = octets écrits par VissLink.
class MockSerial {
 public:
  size_t available() const { return rx.size(); }
  bool read_byte(uint8_t *byte) {
    if (rx.empty())
      return false;
    *byte = rx.front();
    rx.pop_front();
    return true;
  }
  void write_array(const uint8_t *data, size_t length) {
    tx.insert(tx.end(), data, data + length);
    writes++;
  }
  void push(std::initializer_list<uint8_t> bytes) { rx.insert(rx.end(), bytes); }

  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
  int writes{0};
};

/// Comportement simplifié d'une Vitotronic en mode KW :
///  - émet 0x05 toutes les `sync_period_ms` lorsqu'elle est libre ;
///  - après une requête 01 F7 AH AL LEN, répond au bout de `response_delay_ms` ;
///  - reprend ses 0x05 `post_response_delay_ms` après la réponse ;
///  - écriture 01 F4 AH AL LEN D... (v0.3) : applique la valeur si l'adresse
///    est dans `writable` et répond 0x00 ; sinon répond 0x05 sans rien
///    appliquer. Comportement réel d'un refus NON vérifié : hypothèse de test ;
///  - une trame incomplète est abandonnée après `frame_timeout_ms` de silence
///    (ESP redémarré pendant l'émission).
/// Des défauts peuvent être injectés pour éprouver la reprise sur erreur.
class SimBoiler {
 public:
  enum class Fault { NONE, NO_RESPONSE, SHORT_RESPONSE, LONG_RESPONSE };
  /// Réaction à une écriture.
  enum class WriteFault {
    NONE,              ///< appliquée, accusé 0x00
    NACK_SYNC,         ///< refusée, réponse 0x05
    NACK_OTHER,        ///< refusée, réponse 0x15
    ACK_IGNORED,       ///< accusé 0x00 mais valeur non appliquée
    NO_REPLY_APPLIED,  ///< appliquée, accusé perdu
    NO_REPLY_LOST,     ///< ni appliquée ni accusée
    LONG_REPLY,        ///< appliquée, réponse 00 42 (mal cadrée)
  };

  explicit SimBoiler(MockSerial *serial) : serial_(serial) {}

  uint32_t sync_period_ms{2000};
  uint32_t response_delay_ms{40};
  uint32_t post_response_delay_ms{3000};
  bool connected{true};  ///< false = interface optique débranchée
  Fault fault{Fault::NONE};
  std::map<uint16_t, Fault> register_faults;  ///< défaut permanent par registre
  std::map<uint16_t, std::vector<uint8_t>> registers;
  std::set<uint16_t> writable;                    ///< adresses acceptées en écriture
  std::map<uint16_t, WriteFault> write_faults;    ///< réaction par adresse
  std::map<uint16_t, std::pair<uint8_t, uint8_t>> write_clamp;  ///< bornes internes de la régulation
  uint32_t frame_timeout_ms{500};

  int write_frames{0};
  std::map<uint16_t, int> write_frames_by_address;
  std::vector<std::vector<uint8_t>> write_log;  ///< trames d'écriture complètes reçues
  int discarded_partial_frames{0};
  std::vector<std::pair<uint8_t, uint16_t>> frame_log;  ///< (F7/F4, adresse), dans l'ordre reçu

  int requests{0};
  std::map<uint16_t, int> requests_by_address;
  std::map<uint16_t, std::set<uint8_t>> lengths_by_address;  ///< LEN demandés

  void tick(uint32_t now) {
    if (!started_) {
      started_ = true;
      next_sync_ = now;
    }
    if (!connected) {
      serial_->tx.clear();  // rien n'atteint la chaudière
      consumed_ = 0;
      has_pending_ = false;
      next_sync_ = now + sync_period_ms;
      return;
    }
    // Analyse des octets reçus de VissLink (trames de longueur variable).
    if (serial_->tx.size() != last_tx_size_) {
      last_tx_size_ = serial_->tx.size();
      last_tx_ms_ = now;
    }
    while (consumed_ < serial_->tx.size()) {
      const size_t avail = serial_->tx.size() - consumed_;
      const uint8_t *r = &serial_->tx[consumed_];
      if (r[0] != 0x01) {
        consumed_++;
        continue;
      }
      if (avail < 5)
        break;
      if (r[1] == 0xF7) {
        consumed_ += 5;
        requests++;
        const uint16_t address = (r[2] << 8) | r[3];
        requests_by_address[address]++;
        pending_address_ = address;
        pending_length_ = r[4];
        lengths_by_address[address].insert(r[4]);
        frame_log.push_back({0xF7, address});
        pending_write_ = false;
        has_pending_ = true;
        respond_at_ = now + response_delay_ms;
      } else if (r[1] == 0xF4) {
        if (avail < 5u + r[4])
          break;  // trame d'écriture incomplète : attendre la suite
        const uint16_t address = (r[2] << 8) | r[3];
        const std::vector<uint8_t> frame(r, r + 5 + r[4]);
        consumed_ += 5 + r[4];
        write_frames++;
        write_frames_by_address[address]++;
        write_log.push_back(frame);
        frame_log.push_back({0xF4, address});
        receive_write_(address, std::vector<uint8_t>(frame.begin() + 5, frame.end()), now);
      } else {
        consumed_++;
      }
    }
    if (consumed_ < serial_->tx.size() && static_cast<int32_t>(now - last_tx_ms_) >= static_cast<int32_t>(frame_timeout_ms)) {
      consumed_ = serial_->tx.size();  // trame tronquée abandonnée, rien n'est appliqué
      discarded_partial_frames++;
    }
    if (has_pending_ && static_cast<int32_t>(now - respond_at_) >= 0) {
      has_pending_ = false;
      respond_(now);
    } else if (!has_pending_ && static_cast<int32_t>(now - next_sync_) >= 0) {
      serial_->rx.push_back(0x05);
      next_sync_ = now + sync_period_ms;
    }
  }

 protected:
  void receive_write_(uint16_t address, const std::vector<uint8_t> &data, uint32_t now) {
    WriteFault f = WriteFault::NONE;
    auto it = write_faults.find(address);
    if (it != write_faults.end())
      f = it->second;
    if (!writable.count(address))
      f = WriteFault::NACK_SYNC;
    const bool apply = f == WriteFault::NONE || f == WriteFault::NO_REPLY_APPLIED || f == WriteFault::LONG_REPLY;
    if (apply) {
      std::vector<uint8_t> value = data;
      auto clamp = write_clamp.find(address);
      if (clamp != write_clamp.end() && value.size() == 1) {
        value[0] = value[0] < clamp->second.first ? clamp->second.first : value[0];
        value[0] = value[0] > clamp->second.second ? clamp->second.second : value[0];
      }
      registers[address] = value;
    }
    pending_reply_.clear();
    switch (f) {
      case WriteFault::NONE:
      case WriteFault::ACK_IGNORED:
        pending_reply_ = {0x00};
        break;
      case WriteFault::NACK_SYNC:
        pending_reply_ = {0x05};
        break;
      case WriteFault::NACK_OTHER:
        pending_reply_ = {0x15};
        break;
      case WriteFault::LONG_REPLY:
        pending_reply_ = {0x00, 0x42};
        break;
      case WriteFault::NO_REPLY_APPLIED:
      case WriteFault::NO_REPLY_LOST:
        break;
    }
    pending_write_ = true;
    has_pending_ = true;
    respond_at_ = now + response_delay_ms;
  }

  void respond_(uint32_t now) {
    if (pending_write_) {
      serial_->rx.insert(serial_->rx.end(), pending_reply_.begin(), pending_reply_.end());
      next_sync_ = now + post_response_delay_ms;
      return;
    }
    Fault f = fault;
    auto it = register_faults.find(pending_address_);
    if (it != register_faults.end())
      f = it->second;
    std::vector<uint8_t> data = registers[pending_address_];
    data.resize(pending_length_, 0);
    switch (f) {
      case Fault::NO_RESPONSE:
        data.clear();
        break;
      case Fault::SHORT_RESPONSE:
        data.pop_back();
        break;
      case Fault::LONG_RESPONSE:
        data.push_back(0x42);
        data.push_back(0x43);
        break;
      case Fault::NONE:
        break;
    }
    serial_->rx.insert(serial_->rx.end(), data.begin(), data.end());
    next_sync_ = now + post_response_delay_ms;
  }

  MockSerial *serial_;
  bool started_{false};
  size_t consumed_{0};
  size_t last_tx_size_{0};
  uint32_t last_tx_ms_{0};
  bool pending_write_{false};
  std::vector<uint8_t> pending_reply_;
  uint32_t next_sync_{0};
  bool has_pending_{false};
  uint16_t pending_address_{0};
  uint8_t pending_length_{0};
  uint32_t respond_at_{0};
};

}  // namespace vltest
