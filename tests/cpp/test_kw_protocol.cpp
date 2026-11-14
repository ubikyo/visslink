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
  /// Requête envoyée après un 0x05 frais.
  void start_read(uint16_t address, uint32_t now) {
    kw.request_read(address, 2, now);
    serial.push({0x05});
    loop(now + 5);
  }
};

}  // namespace

TEST(kw_idle_discards_boot_garbage_and_syncs) {
  Fixture f;
  f.serial.push({0x12, 0x34, 0x05});
  CHECK(f.kw.is_idle());
  f.loop(0);
  CHECK(f.serial.rx.empty());
  CHECK(f.serial.tx.empty());  // aucune émission hors requête
}

TEST(kw_read_sends_exact_frame_and_returns_data) {
  Fixture f;
  CHECK(f.kw.request_read(0x0800, 2, 0));
  CHECK(f.kw.state() == State::WAIT_SYNC);
  CHECK_EQ(f.loop(10), KwResult::NONE);
  CHECK(f.serial.tx.empty());  // rien n'est émis avant le 0x05

  f.serial.push({0x05});
  CHECK_EQ(f.loop(20), KwResult::NONE);
  const std::vector<uint8_t> expected = {0x01, 0xF7, 0x08, 0x00, 0x02};
  CHECK(f.serial.tx == expected);
  CHECK_EQ(f.serial.writes, 1);

  f.serial.push({0xD7, 0x00});
  CHECK_EQ(f.loop(60), KwResult::NONE);
  CHECK(f.kw.state() == State::CHECK_TRAILING);
  CHECK_EQ(f.loop(60 + KwProtocol::TRAILING_WINDOW_MS), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data_length(), 2);
  CHECK_EQ(decode_int16_le(f.kw.data()), 215);
  CHECK(f.kw.is_idle());
}

TEST(kw_eight_byte_read) {
  Fixture f;
  CHECK(f.kw.request_read(0x2000, 8, 0));
  f.serial.push({0x05});
  f.loop(5);
  CHECK_EQ(f.serial.tx[4], 8);
  f.serial.push({1, 2, 3, 4, 5, 6, 7, 8});
  f.loop(30);
  CHECK_EQ(f.loop(100), KwResult::SUCCESS);
  CHECK_EQ(f.kw.data_length(), 8);
  CHECK_EQ(f.kw.data()[7], 8);
}

TEST(kw_response_split_over_several_loops) {
  Fixture f;
  f.start_read(0x0802, 0);
  f.serial.push({0x2C});
  CHECK_EQ(f.loop(30), KwResult::NONE);
  CHECK(f.kw.state() == State::WAIT_RESPONSE);
  f.serial.push({0x01});
  f.loop(33);
  CHECK_EQ(f.loop(100), KwResult::SUCCESS);
  CHECK_EQ(decode_int16_le(f.kw.data()), 300);
}

TEST(kw_ignores_noise_and_stale_sync) {
  Fixture f;
  f.kw.request_read(0x0800, 2, 0);
  f.serial.push({0x05, 0x33, 0x44});  // 0x05 suivi d'autres octets : périmé
  f.loop(10);
  CHECK(f.serial.tx.empty());
  CHECK(f.kw.state() == State::WAIT_SYNC);
  f.serial.push({0x05, 0x05});  // seul le dernier, frais, déclenche l'envoi
  f.loop(20);
  CHECK_EQ(f.serial.tx.size(), 5u);
}

TEST(kw_sync_timeout) {
  Fixture f;
  f.kw.set_sync_timeout(5000);
  f.kw.request_read(0x0800, 2, 1000);
  CHECK_EQ(f.loop(5999), KwResult::NONE);
  CHECK_EQ(f.loop(6000), KwResult::SYNC_TIMEOUT);
  CHECK(f.serial.tx.empty());
  CHECK(f.kw.state() == State::RECOVERY);
}

TEST(kw_response_timeout) {
  Fixture f;
  f.kw.set_response_timeout(1000);
  f.start_read(0x0800, 0);  // requête émise à t=5
  CHECK_EQ(f.loop(1004), KwResult::NONE);
  CHECK_EQ(f.loop(1005), KwResult::RESPONSE_TIMEOUT);
  CHECK(f.kw.state() == State::RECOVERY);
}

TEST(kw_response_too_short) {
  Fixture f;
  f.start_read(0x0800, 0);
  f.serial.push({0xD7});
  f.loop(50);
  CHECK_EQ(f.loop(2000), KwResult::RESPONSE_TOO_SHORT);
  CHECK_EQ(f.kw.data_length(), 0);  // aucune donnée partielle exposée
}

TEST(kw_response_too_long) {
  Fixture f;
  f.start_read(0x0800, 0);
  f.serial.push({0xD7, 0x00, 0x11});
  CHECK_EQ(f.loop(50), KwResult::RESPONSE_TOO_LONG);
  CHECK(f.kw.state() == State::RECOVERY);
}

TEST(kw_extra_byte_in_trailing_window_is_an_error) {
  // Y compris un 0x05 : la Vitotronic accepte une nouvelle commande juste après
  // une réponse et n'émet pas de synchro immédiate. Un octet ici signale une
  // désynchronisation.
  for (uint8_t extra : {uint8_t{0x99}, uint8_t{0x05}}) {
    Fixture f;
    f.start_read(0x0800, 0);
    f.serial.push({0xD7, 0x00});
    CHECK_EQ(f.loop(50), KwResult::NONE);
    f.serial.push({extra});
    CHECK_EQ(f.loop(55), KwResult::RESPONSE_TOO_LONG);
  }
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
  f.start_read(0x0800, 0);
  f.serial.push({0xD7, 0x00, 0x11, 0x22});
  CHECK_EQ(f.loop(50), KwResult::RESPONSE_TOO_LONG);
  CHECK(!f.kw.request_read(0x0800, 2, 60));  // refus pendant la récupération
  f.serial.push({0x33});                     // ligne encore bruyante
  f.loop(100);
  f.loop(100 + KwProtocol::RECOVERY_QUIET_MS - 1);
  CHECK(f.kw.state() == State::RECOVERY);
  f.loop(100 + KwProtocol::RECOVERY_QUIET_MS);
  CHECK(f.kw.is_idle());
  CHECK(f.serial.rx.empty());

  f.serial.tx.clear();
  f.start_read(0x0804, 400);
  const std::vector<uint8_t> expected = {0x01, 0xF7, 0x08, 0x04, 0x02};
  CHECK(f.serial.tx == expected);
  f.serial.push({0xDB, 0x01});
  f.loop(450);
  CHECK_EQ(f.loop(500), KwResult::SUCCESS);
  CHECK_EQ(decode_int16_le(f.kw.data()), 475);
}

TEST(kw_timeouts_across_millis_overflow) {
  Fixture f;
  f.kw.set_sync_timeout(5000);
  const uint32_t start = 0xFFFFFFFFu - 1000;
  f.kw.request_read(0x0800, 2, start);
  CHECK_EQ(f.loop(start + 10), KwResult::NONE);
  CHECK_EQ(f.loop(2000), KwResult::NONE);  // millis() a débordé
  CHECK_EQ(f.loop(start + 5000), KwResult::SYNC_TIMEOUT);
}
