#include "kw_protocol.h"
#include "sim.h"
#include "test_framework.h"

using namespace esphome::visslink;
using vltest::MockSerial;

namespace {

/// Protocole prêt (état INIT franchi) avec les délais par défaut.
struct Fixture {
  MockSerial serial;
  KwProtocol kw{&serial};
  Fixture() { kw.loop(0); }
};

}  // namespace

TEST(kw_init_discards_boot_garbage) {
  MockSerial serial;
  serial.push({0x12, 0x34, 0x05});
  KwProtocol kw(&serial);
  CHECK(kw.state() == KwState::INIT);
  kw.loop(0);
  CHECK(kw.is_idle());
  CHECK(serial.rx.empty());
}

TEST(kw_read_sends_exact_frame_and_returns_data) {
  Fixture f;
  CHECK(f.kw.request_read(0x0800, 2, 0));
  CHECK(f.kw.state() == KwState::WAIT_SYNC);
  CHECK_EQ(f.kw.loop(10), KwResult::NONE);
  CHECK(f.serial.tx.empty());  // rien n'est émis avant le 0x05

  f.serial.push({0x05});
  CHECK_EQ(f.kw.loop(20), KwResult::NONE);
  const std::vector<uint8_t> expected = {0x01, 0xF7, 0x08, 0x00, 0x02};
  CHECK(f.serial.tx == expected);

  f.serial.push({0xD7, 0x00});
  CHECK_EQ(f.kw.loop(60), KwResult::NONE);  // fenêtre de contrôle des octets en trop
  CHECK(f.kw.state() == KwState::CHECK_TRAILING);
  CHECK_EQ(f.kw.loop(60 + KwProtocol::TRAILING_WINDOW_MS), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data_length(), 2);
  CHECK_EQ(f.kw.data()[0], 0xD7);
  CHECK_EQ(f.kw.data()[1], 0x00);
  CHECK(f.kw.is_idle());
  CHECK_EQ(f.serial.writes, 1);
}

TEST(kw_response_split_over_several_loops) {
  Fixture f;
  f.kw.request_read(0x0802, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(5);
  f.serial.push({0x2C});
  CHECK_EQ(f.kw.loop(30), KwResult::NONE);
  CHECK(f.kw.state() == KwState::WAIT_RESPONSE);
  f.serial.push({0x01});
  f.kw.loop(33);
  CHECK_EQ(f.kw.loop(100), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data()[0], 0x2C);
  CHECK_EQ(f.kw.data()[1], 0x01);
}

TEST(kw_ignores_noise_and_stale_sync) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  // Un 0x05 suivi d'autres octets est périmé : pas de réponse.
  f.serial.push({0x05, 0x33, 0x44});
  f.kw.loop(10);
  CHECK(f.serial.tx.empty());
  CHECK(f.kw.state() == KwState::WAIT_SYNC);
  // Plusieurs 0x05 accumulés : seul le dernier, frais, déclenche l'envoi.
  f.serial.push({0x05, 0x05});
  f.kw.loop(20);
  CHECK_EQ(f.serial.tx.size(), 5u);
  CHECK(f.kw.stats().discarded_bytes >= 4);
}

TEST(kw_sync_timeout) {
  Fixture f;
  f.kw.set_sync_timeout(5000);
  f.kw.request_read(0x0800, 2, 1000);
  CHECK_EQ(f.kw.loop(5999), KwResult::NONE);
  CHECK_EQ(f.kw.loop(6000), KwResult::SYNC_TIMEOUT);
  CHECK(f.serial.tx.empty());
  CHECK(f.kw.state() == KwState::RECOVERY);
  CHECK_EQ(f.kw.stats().sync_timeouts, 1u);
}

TEST(kw_response_timeout) {
  Fixture f;
  f.kw.set_response_timeout(1000);
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  CHECK_EQ(f.kw.loop(1009), KwResult::NONE);
  CHECK_EQ(f.kw.loop(1010), KwResult::RESPONSE_TIMEOUT);
  CHECK(f.kw.state() == KwState::RECOVERY);
}

TEST(kw_response_too_short) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  f.serial.push({0xD7});
  f.kw.loop(50);
  CHECK_EQ(f.kw.loop(2000), KwResult::RESPONSE_TOO_SHORT);
  CHECK_EQ(f.kw.data_length(), 0);  // aucune donnée partielle exposée
}

TEST(kw_response_too_long) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  f.serial.push({0xD7, 0x00, 0x11});
  CHECK_EQ(f.kw.loop(50), KwResult::RESPONSE_TOO_LONG);
  CHECK(f.kw.state() == KwState::RECOVERY);
}

TEST(kw_response_too_long_detected_in_trailing_window) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  f.serial.push({0xD7, 0x00});
  CHECK_EQ(f.kw.loop(50), KwResult::NONE);
  f.serial.push({0x99});
  CHECK_EQ(f.kw.loop(55), KwResult::RESPONSE_TOO_LONG);
}

TEST(kw_single_trailing_sync_is_not_an_error) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  f.serial.push({0xD7, 0x00, 0x05});
  CHECK_EQ(f.kw.loop(50), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data()[0], 0xD7);
}

TEST(kw_refuses_concurrent_or_invalid_requests) {
  Fixture f;
  CHECK(!f.kw.request_read(0x0800, 0, 0));
  CHECK(!f.kw.request_read(0x0800, KwProtocol::MAX_READ_LENGTH + 1, 0));
  CHECK(f.kw.request_read(0x0800, 2, 0));
  CHECK(!f.kw.request_read(0x0802, 2, 0));  // une seule transaction à la fois
}

TEST(kw_recovery_flushes_then_next_read_succeeds) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05});
  f.kw.loop(10);
  f.serial.push({0xD7, 0x00, 0x11, 0x22});
  CHECK_EQ(f.kw.loop(50), KwResult::RESPONSE_TOO_LONG);
  CHECK(!f.kw.request_read(0x0800, 2, 60));  // refus pendant la récupération
  f.serial.push({0x33});                     // ligne encore bruyante
  f.kw.loop(100);
  f.kw.loop(100 + KwProtocol::RECOVERY_QUIET_MS - 1);
  CHECK(f.kw.state() == KwState::RECOVERY);
  f.kw.loop(100 + KwProtocol::RECOVERY_QUIET_MS);
  CHECK(f.kw.is_idle());
  CHECK(f.serial.rx.empty());

  f.serial.tx.clear();
  CHECK(f.kw.request_read(0x0804, 2, 400));
  f.serial.push({0x05});
  f.kw.loop(410);
  const std::vector<uint8_t> expected = {0x01, 0xF7, 0x08, 0x04, 0x02};
  CHECK(f.serial.tx == expected);
  f.serial.push({0xE8, 0x01});
  f.kw.loop(450);
  CHECK_EQ(f.kw.loop(500), KwResult::SUCCESS);
}

TEST(kw_timeouts_across_millis_overflow) {
  Fixture f;
  f.kw.set_sync_timeout(5000);
  const uint32_t start = 0xFFFFFFFFu - 1000;
  f.kw.request_read(0x0800, 2, start);
  CHECK_EQ(f.kw.loop(start + 10), KwResult::NONE);
  CHECK_EQ(f.kw.loop(2000), KwResult::NONE);  // millis() a débordé
  CHECK_EQ(f.kw.loop(start + 5000), KwResult::SYNC_TIMEOUT);
}
