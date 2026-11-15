// Écriture KW (v0.3) : encodage, synchronisation, accusés, reprise.
// Le format suit openv (« Protokoll KW ») et vcontrold (protocole KW2,
// setaddr : SEND 01 F4 $addr $hexlen ; SEND BYTES ; RECV 1 SR).

#include "kw_protocol.h"
#include "sim.h"
#include "test_framework.h"

using namespace esphome::visslink;
using vltest::MockSerial;
using State = KwProtocol::State;

namespace {

struct Fixture {
  MockSerial serial;
  KwProtocol kw;
  KwResult loop(uint32_t now) { return kw.loop(now, serial); }
  /// Écriture d'un octet envoyée après un 0x05 frais.
  void start_write(uint16_t address, uint8_t value, uint32_t now) {
    CHECK(kw.request_write(address, &value, 1, now));
    serial.push({0x05});
    loop(now + 5);
  }
  /// Fin de la fenêtre de silence après une réponse.
  KwResult settle(uint32_t now) {
    loop(now);
    return loop(now + KwProtocol::TRAILING_WINDOW_MS);
  }
};

}  // namespace

TEST(kw_write_sends_exact_frame_after_sync) {
  Fixture f;
  const uint8_t value = 0x15;  // 21 °C
  CHECK(f.kw.request_write(0x2306, &value, 1, 0));
  CHECK(f.kw.is_write());
  CHECK(f.kw.state() == State::WAIT_SYNC);
  CHECK_EQ(f.loop(10), KwResult::NONE);
  CHECK(f.serial.tx.empty());  // rien avant le 0x05

  f.serial.push({0x05});
  CHECK_EQ(f.loop(20), KwResult::NONE);
  const std::vector<uint8_t> expected = {0x01, 0xF4, 0x23, 0x06, 0x01, 0x15};
  CHECK(f.serial.tx == expected);
  CHECK_EQ(f.serial.writes, 1);  // une seule émission, trame entière
  CHECK(f.kw.state() == State::WAIT_RESPONSE);
}

TEST(kw_write_ack_00_is_success) {
  Fixture f;
  f.start_write(0x6300, 50, 0);
  f.serial.push({0x00});
  CHECK_EQ(f.loop(60), KwResult::NONE);
  CHECK(f.kw.state() == State::CHECK_TRAILING);
  CHECK_EQ(f.loop(60 + KwProtocol::TRAILING_WINDOW_MS), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data_length(), 1);
  CHECK_EQ(f.kw.data()[0], 0x00);
  CHECK(f.kw.is_idle());
}

TEST(kw_write_reply_05_is_rejected_and_resyncs) {
  // vcontrold : 05 = « SYNC (NOT OK) ».
  Fixture f;
  f.start_write(0x2306, 21, 0);
  f.serial.push({0x05});
  CHECK_EQ(f.settle(60), KwResult::WRITE_REJECTED);
  CHECK_EQ(f.kw.data()[0], 0x05);  // octet reçu, pour le journal
  CHECK(f.kw.state() == State::RECOVERY);
  f.loop(60 + KwProtocol::TRAILING_WINDOW_MS + KwProtocol::RECOVERY_QUIET_MS + 1);
  CHECK(f.kw.is_idle());
}

TEST(kw_write_any_other_reply_is_rejected) {
  // vcontrold : toute valeur autre que 00 = « NOT OK ».
  for (uint8_t reply : {0x01, 0x15, 0xFF}) {
    Fixture f;
    f.start_write(0x2306, 21, 0);
    f.serial.push({reply});
    CHECK_EQ(f.settle(60), KwResult::WRITE_REJECTED);
    CHECK_EQ(f.kw.data()[0], reply);
  }
}

TEST(kw_write_no_reply_is_timeout_without_resend) {
  Fixture f;
  f.start_write(0x2306, 21, 0);
  const size_t sent = f.serial.tx.size();
  CHECK_EQ(f.loop(500), KwResult::NONE);
  CHECK_EQ(f.loop(5 + 1000), KwResult::RESPONSE_TIMEOUT);
  CHECK(f.kw.state() == State::RECOVERY);
  f.loop(5000);
  CHECK(f.kw.is_idle());
  CHECK_EQ(f.serial.tx.size(), sent);  // le moteur ne répète jamais une écriture
}

TEST(kw_write_extra_bytes_are_rejected) {
  Fixture f;
  f.start_write(0x2306, 21, 0);
  f.serial.push({0x00, 0x42});
  CHECK_EQ(f.loop(60), KwResult::RESPONSE_TOO_LONG);
  CHECK(f.kw.state() == State::RECOVERY);
}

TEST(kw_write_without_sync_is_never_sent) {
  Fixture f;
  const uint8_t value = 21;
  CHECK(f.kw.request_write(0x2306, &value, 1, 0));
  CHECK_EQ(f.loop(9999), KwResult::NONE);
  CHECK_EQ(f.loop(10000), KwResult::SYNC_TIMEOUT);
  CHECK(f.serial.tx.empty());  // aucun octet n'a atteint la régulation
}

TEST(kw_write_ignores_stale_sync) {
  // Un 0x05 suivi d'autres octets n'est pas une synchro fraîche.
  Fixture f;
  const uint8_t value = 21;
  CHECK(f.kw.request_write(0x2306, &value, 1, 0));
  f.serial.push({0x05, 0x33});
  f.loop(10);
  CHECK(f.serial.tx.empty());
  f.serial.push({0x05});
  f.loop(20);
  CHECK_EQ(f.serial.tx.size(), 6u);
}

TEST(kw_write_invalid_requests_refused) {
  Fixture f;
  const uint8_t two[2] = {20, 21};
  CHECK(!f.kw.request_write(0x2306, two, 0, 0));  // longueur nulle
  CHECK(!f.kw.request_write(0x2306, two, 2, 0));  // > MAX_WRITE_LENGTH
  CHECK(!f.kw.request_write(0x2306, nullptr, 1, 0));
  CHECK_EQ(KwProtocol::MAX_WRITE_LENGTH, 1);
  CHECK(f.kw.is_idle());

  // Une seule transaction à la fois, lecture ou écriture.
  CHECK(f.kw.request_read(0x0800, 2, 0));
  CHECK(!f.kw.request_write(0x2306, two, 1, 0));
  CHECK(!f.kw.is_write());
  Fixture g;
  CHECK(g.kw.request_write(0x2306, two, 1, 0));
  CHECK(!g.kw.request_write(0x2307, two, 1, 0));
  CHECK(!g.kw.request_read(0x0800, 2, 0));
}

TEST(kw_read_right_after_write_uses_read_frame) {
  Fixture f;
  f.start_write(0x2306, 21, 0);
  f.serial.push({0x00});
  CHECK_EQ(f.settle(60), KwResult::SUCCESS);
  f.serial.tx.clear();

  CHECK(f.kw.request_read(0x2306, 1, 100));
  CHECK(!f.kw.is_write());
  f.serial.push({0x05});
  f.loop(110);
  const std::vector<uint8_t> expected = {0x01, 0xF7, 0x23, 0x06, 0x01};
  CHECK(f.serial.tx == expected);
  f.serial.push({0x15});
  CHECK_EQ(f.settle(150), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data()[0], 21);
}

TEST(kw_write_reply_after_recovery_does_not_leak) {
  // Accusé arrivé après le délai : il est vidé pendant la reprise et ne
  // peut pas être pris pour la réponse de la lecture suivante.
  Fixture f;
  f.start_write(0x2306, 21, 0);
  CHECK_EQ(f.loop(1005), KwResult::RESPONSE_TIMEOUT);
  f.serial.push({0x00});  // accusé tardif
  f.loop(1010);
  f.loop(1010 + KwProtocol::RECOVERY_QUIET_MS);
  CHECK(f.kw.is_idle());
  CHECK(f.serial.rx.empty());
  CHECK(f.kw.request_read(0x2306, 1, 1300));
  f.serial.push({0x05});
  f.loop(1310);
  f.serial.push({0x14});
  CHECK_EQ(f.settle(1350), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data()[0], 20);
}

// --- v0.4.1 : préemption d'une requête non émise -----------------------------

TEST(kw_cancel_only_before_anything_is_sent) {
  // WAIT_SYNC : rien n'a été émis, annulation possible.
  Fixture f;
  CHECK(f.kw.request_read(0x0800, 2, 0));
  CHECK(f.kw.is_waiting_sync());
  CHECK(f.kw.cancel_before_send());
  CHECK(f.kw.is_idle());
  CHECK(f.serial.tx.empty());
  // Ensuite, une écriture attend un 0x05 frais comme toute requête.
  const uint8_t v = 0x00;
  CHECK(f.kw.request_write(0x2301, &v, 1, 10));
  f.loop(20);
  CHECK(f.serial.tx.empty());
  f.serial.push({0x05});
  f.loop(30);
  CHECK(f.serial.tx == (std::vector<uint8_t>{0x01, 0xF4, 0x23, 0x01, 0x01, 0x00}));
  CHECK_EQ(f.kw.sent_ms(), 30u);

  // Trame émise (WAIT_RESPONSE, CHECK_TRAILING) ou reprise (RECOVERY) :
  // jamais annulée.
  Fixture g;
  CHECK(g.kw.request_read(0x0800, 2, 0));
  g.serial.push({0x05});
  g.loop(5);
  CHECK(g.kw.state() == State::WAIT_RESPONSE);
  CHECK(!g.kw.cancel_before_send());
  g.serial.push({0x10, 0x00});
  g.loop(50);
  CHECK(g.kw.state() == State::CHECK_TRAILING);
  CHECK(!g.kw.cancel_before_send());
  g.serial.push({0x99});
  CHECK_EQ(g.loop(60), KwResult::RESPONSE_TOO_LONG);
  CHECK(g.kw.state() == State::RECOVERY);
  CHECK(!g.kw.cancel_before_send());
  Fixture h;
  CHECK(!h.kw.cancel_before_send());  // IDLE : rien à annuler
}
