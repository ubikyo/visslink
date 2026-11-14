// Simulations de bout en bout : KwProtocol + RegisterScheduler face à une
// Vitotronic simulée. Ces tests éprouvent la logique, ils ne remplacent pas
// un essai sur la chaudière réelle.

#include <map>

#include "kw_protocol.h"
#include "scheduler.h"
#include "sim.h"
#include "test_framework.h"

using namespace esphome::visslink;
using vltest::MockSerial;
using vltest::SimBoiler;

namespace {

struct Bench {
  MockSerial serial;
  SimBoiler boiler{&serial};
  KwProtocol kw;
  RegisterScheduler scheduler;
  std::vector<uint16_t> addresses;
  std::map<size_t, int> successes, failures, gave_up;
  std::map<size_t, float> last_value;

  Bench() {
    boiler.registers[0x0800] = {0xCC, 0xFF};  // -5,2 °C
    boiler.registers[0x0802] = {0x2C, 0x02};  // 55,6 °C
    boiler.registers[0x0804] = {0xDB, 0x01};  // 47,5 °C
  }
  void add(uint16_t address, uint32_t interval_ms, uint32_t now) {
    addresses.push_back(address);
    scheduler.add(interval_ms, now);
  }
  void add_defaults(uint32_t now) {
    add(0x0800, 60000, now);
    add(0x0802, 30000, now);
    add(0x0804, 60000, now);
  }

  /// Même enchaînement que VissLinkComponent::loop().
  void step(uint32_t now) {
    const KwResult result = kw.loop(now, serial);
    if (result != KwResult::NONE) {
      const size_t index = scheduler.current();
      if (result == KwResult::SUCCESS) {
        scheduler.finish(true, now);
        successes[index]++;
        last_value[index] = decode_int16_le(kw.data()) * 0.1f;
      } else {
        failures[index]++;
        if (scheduler.finish(false, now))
          gave_up[index]++;
      }
    }
    if (kw.is_idle()) {
      const int index = scheduler.acquire(now);
      if (index != RegisterScheduler::NONE && !kw.request_read(addresses[index], 2, now))
        scheduler.finish(false, now);
    }
  }

  /// Avance le temps par pas de 5 ms (≈ boucle ESPHome rapide).
  void run(uint32_t from, uint32_t duration_ms) {
    for (uint32_t t = from; t - from < duration_ms; t += 5) {
      boiler.tick(t);
      step(t);
    }
  }
  int total_successes() const {
    int total = 0;
    for (const auto &kv : successes)
      total += kv.second;
    return total;
  }
};

}  // namespace

TEST(sim_three_temperatures_individual_intervals) {
  Bench b;
  b.add_defaults(0);
  b.run(0, 600000);  // 10 minutes
  CHECK_NEAR(b.last_value[0], -5.2, 1e-3);
  CHECK_NEAR(b.last_value[1], 55.6, 1e-3);
  CHECK_NEAR(b.last_value[2], 47.5, 1e-3);
  CHECK(b.successes[0] >= 9 && b.successes[0] <= 11);
  CHECK(b.successes[1] >= 18 && b.successes[1] <= 21);
  CHECK(b.successes[2] >= 9 && b.successes[2] <= 11);
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());  // aucune requête en double
}

TEST(sim_slow_boiler_does_not_accumulate_requests) {
  Bench b;
  b.boiler.post_response_delay_ms = 8000;  // synchro très lente
  b.boiler.registers[0x0806] = {0x01, 0x00};
  b.add_defaults(0);
  b.add(0x0806, 1000, 0);  // intervalle irréaliste : saturation volontaire
  b.run(0, 600000);
  CHECK_EQ(b.boiler.requests, b.total_successes());
  CHECK(b.total_successes() <= 80);  // une lecture par synchro (~8 s)
  for (size_t i = 0; i < 4; i++)
    CHECK(b.successes[i] >= 5);  // aucun registre affamé
  CHECK(b.failures.empty());
}

TEST(sim_unplugged_interface_then_recovery) {
  Bench b;
  b.add_defaults(0);
  b.run(0, 120000);
  CHECK(b.successes[1] >= 3);

  b.boiler.connected = false;  // interface débranchée 3 minutes
  b.run(120000, 180000);
  CHECK(b.failures[1] > 0);
  CHECK(b.gave_up[1] > 0);  // le capteur passera à « inconnu »

  b.boiler.connected = true;
  const int before = b.successes[1];
  b.run(300000, 180000);
  CHECK(b.successes[1] - before >= 4);  // reprise automatique
  CHECK(b.successes[0] > 0 && b.successes[2] > 0);
}

TEST(sim_bad_register_does_not_block_others) {
  Bench b;
  b.boiler.register_faults[0x0800] = SimBoiler::Fault::NO_RESPONSE;
  b.add_defaults(0);
  b.run(0, 600000);
  CHECK_EQ(b.successes[0], 0);
  CHECK(b.gave_up[0] >= 5);
  CHECK(b.successes[1] >= 15);
  CHECK(b.successes[2] >= 8);
  CHECK(b.boiler.requests_by_address[0x0800] <= 3 * 11);  // 1 + 2 tentatives par cycle
}

TEST(sim_transient_errors_are_retried) {
  for (auto fault : {SimBoiler::Fault::SHORT_RESPONSE, SimBoiler::Fault::LONG_RESPONSE}) {
    Bench b;
    b.boiler.fault = fault;
    b.add_defaults(0);
    b.run(0, 30000);
    CHECK_EQ(b.total_successes(), 0);  // rien d'invalide publié
    CHECK(b.failures[0] > 0);
    b.boiler.fault = SimBoiler::Fault::NONE;
    b.run(30000, 120000);
    CHECK(b.successes[0] > 0 && b.successes[1] > 0 && b.successes[2] > 0);
    CHECK_NEAR(b.last_value[0], -5.2, 1e-3);
  }
}

TEST(sim_millis_overflow) {
  Bench b;
  const uint32_t start = 0xFFFFFFFFu - 200000;
  b.add_defaults(start);
  b.run(start, 600000);
  CHECK(b.successes[1] >= 18 && b.successes[1] <= 21);
  CHECK(b.successes[0] >= 9 && b.successes[0] <= 11);
  CHECK(b.failures.empty());
}
