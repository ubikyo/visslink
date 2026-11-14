// Simulations de bout en bout : ReadEngine (code embarqué) + chaudière simulée.

#include <map>
#include <set>

#include "decode.h"
#include "read_engine.h"
#include "sim.h"
#include "test_framework.h"

using namespace esphome::visslink;
using vltest::MockSerial;
using vltest::SimBoiler;

namespace {

struct Bench {
  MockSerial serial;
  SimBoiler boiler{&serial};
  ReadEngine engine{&serial};
  std::map<size_t, int> successes;
  std::map<size_t, int> failures;
  std::map<size_t, int> exhausted;
  std::map<size_t, float> last_value;
  int overlapping_requests{0};

  Bench() {
    boiler.registers[0x0800] = {0xCC, 0xFF};  // -5,2 °C
    boiler.registers[0x0802] = {0x2C, 0x02};  // 55,6 °C
    boiler.registers[0x0804] = {0xDB, 0x01};  // 47,5 °C
    RegisterScheduler &s = engine.scheduler();
    s.add(0x0800, 2, 60000);
    s.add(0x0802, 2, 30000);
    s.add(0x0804, 2, 60000);
  }

  /// Avance le temps par pas de 5 ms (≈ cadence de loop()).
  void run(uint32_t from, uint32_t duration_ms) {
    ReadEvent ev;
    for (uint32_t t = from; static_cast<int32_t>(from + duration_ms - t) > 0; t += 5) {
      boiler.tick(t);
      if (engine.loop(t, &ev)) {
        if (ev.result == KwResult::SUCCESS) {
          successes[ev.index]++;
          last_value[ev.index] = decode_register(ev.data, ev.length, true) * 0.1f;
        } else {
          failures[ev.index]++;
          if (ev.exhausted)
            exhausted[ev.index]++;
        }
      }
      if (engine.scheduler().busy() && engine.protocol().is_idle())
        overlapping_requests++;  // incohérence : registre en cours sans transaction
    }
  }
};

}  // namespace

TEST(sim_three_temperatures_individual_intervals) {
  Bench b;
  b.engine.scheduler().start(0);
  b.run(0, 600000);  // 10 minutes
  CHECK_NEAR(b.last_value[0], -5.2, 1e-3);
  CHECK_NEAR(b.last_value[1], 55.6, 1e-3);
  CHECK_NEAR(b.last_value[2], 47.5, 1e-3);
  CHECK(b.successes[0] >= 9 && b.successes[0] <= 11);
  CHECK(b.successes[1] >= 18 && b.successes[1] <= 21);
  CHECK(b.successes[2] >= 9 && b.successes[2] <= 11);
  CHECK(b.failures.empty());
  // Une requête KW par lecture réussie : aucune requête dupliquée.
  CHECK_EQ(b.boiler.requests, b.successes[0] + b.successes[1] + b.successes[2]);
  CHECK_EQ(b.overlapping_requests, 0);
}

TEST(sim_slow_boiler_does_not_accumulate_requests) {
  Bench b;
  b.boiler.post_response_delay_ms = 8000;  // synchro très lente après chaque lecture
  RegisterScheduler &s = b.engine.scheduler();
  s.add(0x0806, 2, 1000);  // intervalle irréaliste : saturation volontaire
  b.boiler.registers[0x0806] = {0x01, 0x00};
  s.start(0);
  b.run(0, 600000);
  // Au plus une lecture par synchro (~8 s) : ~75 lectures en 10 min, quel que
  // soit le nombre d'échéances manquées.
  const int total = b.successes[0] + b.successes[1] + b.successes[2] + b.successes[3];
  CHECK_EQ(b.boiler.requests, total);
  CHECK(total <= 80);
  // Aucun registre n'est affamé.
  for (size_t i = 0; i < 4; i++)
    CHECK(b.successes[i] >= 5);
  CHECK(b.failures.empty());
}

TEST(sim_unplugged_interface_then_recovery) {
  Bench b;
  b.engine.scheduler().start(0);
  b.run(0, 120000);
  const int before = b.successes[1];
  CHECK(before >= 3);

  b.boiler.connected = false;  // interface optique débranchée 3 minutes
  b.run(120000, 180000);
  CHECK(b.failures[0] + b.failures[1] + b.failures[2] > 0);
  CHECK(b.exhausted[1] > 0);  // le capteur passera à « inconnu »
  CHECK(b.engine.protocol().stats().sync_timeouts > 0);

  b.boiler.connected = true;
  const int after_reconnect = b.successes[1];
  b.run(300000, 180000);
  CHECK(b.successes[1] - after_reconnect >= 4);  // reprise automatique
  CHECK(b.successes[0] > 0 && b.successes[2] > 0);
}

TEST(sim_bad_register_does_not_block_others) {
  Bench b;
  b.boiler.register_faults[0x0800] = SimBoiler::Fault::NO_RESPONSE;
  b.engine.scheduler().start(0);
  b.run(0, 600000);
  CHECK_EQ(b.successes[0], 0);
  CHECK(b.exhausted[0] >= 5);
  CHECK(b.successes[1] >= 15);
  CHECK(b.successes[2] >= 8);
  // Tentatives limitées : (1 + 2 nouvelles tentatives) par cycle au maximum.
  CHECK(b.boiler.requests_by_address[0x0800] <= 3 * 11);
}

TEST(sim_transient_errors_are_retried) {
  for (auto fault : {SimBoiler::Fault::SHORT_RESPONSE, SimBoiler::Fault::LONG_RESPONSE}) {
    Bench b;
    b.boiler.fault = fault;
    b.engine.scheduler().start(0);
    b.run(0, 30000);
    CHECK_EQ(b.successes[0] + b.successes[1] + b.successes[2], 0);  // rien d'invalide publié
    CHECK(b.failures[0] > 0);
    b.boiler.fault = SimBoiler::Fault::NONE;
    b.run(30000, 120000);
    CHECK(b.successes[0] > 0 && b.successes[1] > 0 && b.successes[2] > 0);
    CHECK_NEAR(b.last_value[0], -5.2, 1e-3);
  }
}

TEST(sim_trailing_sync_after_response_is_tolerated) {
  Bench b;
  b.boiler.fault = SimBoiler::Fault::TRAILING_SYNC;
  b.engine.scheduler().start(0);
  b.run(0, 120000);
  CHECK(b.failures.empty());
  CHECK(b.successes[1] >= 4);
}

TEST(sim_millis_overflow) {
  Bench b;
  const uint32_t start = 0xFFFFFFFFu - 200000;
  b.engine.scheduler().start(start);
  b.run(start, 600000);
  CHECK(b.successes[1] >= 18 && b.successes[1] <= 21);
  CHECK(b.successes[0] >= 9 && b.successes[0] <= 11);
  CHECK(b.failures.empty());
}
