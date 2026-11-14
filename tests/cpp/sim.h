#pragma once
// Port série et chaudière Vitotronic simulés pour les tests du protocole KW.

#include <cstdint>
#include <deque>
#include <map>
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
///  - reprend ses 0x05 `post_response_delay_ms` après la réponse.
/// Des défauts peuvent être injectés pour éprouver la reprise sur erreur.
class SimBoiler {
 public:
  enum class Fault { NONE, NO_RESPONSE, SHORT_RESPONSE, LONG_RESPONSE };

  explicit SimBoiler(MockSerial *serial) : serial_(serial) {}

  uint32_t sync_period_ms{2000};
  uint32_t response_delay_ms{40};
  uint32_t post_response_delay_ms{3000};
  bool connected{true};  ///< false = interface optique débranchée
  Fault fault{Fault::NONE};
  std::map<uint16_t, Fault> register_faults;  ///< défaut permanent par registre
  std::map<uint16_t, std::vector<uint8_t>> registers;

  int requests{0};
  std::map<uint16_t, int> requests_by_address;

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
    // Analyse des octets reçus de VissLink.
    while (consumed_ < serial_->tx.size() && serial_->tx.size() - consumed_ >= 5) {
      const uint8_t *r = &serial_->tx[consumed_];
      consumed_ += 5;
      if (r[0] != 0x01 || r[1] != 0xF7)
        continue;
      requests++;
      const uint16_t address = (r[2] << 8) | r[3];
      requests_by_address[address]++;
      pending_address_ = address;
      pending_length_ = r[4];
      has_pending_ = true;
      respond_at_ = now + response_delay_ms;
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
  void respond_(uint32_t now) {
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
  uint32_t next_sync_{0};
  bool has_pending_{false};
  uint16_t pending_address_{0};
  uint8_t pending_length_{0};
  uint32_t respond_at_{0};
};

}  // namespace vltest
