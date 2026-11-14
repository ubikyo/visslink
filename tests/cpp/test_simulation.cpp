// Simulations de bout en bout : KwProtocol + RegisterScheduler face à une
// Vitotronic simulée. Ces tests éprouvent la logique, ils ne remplacent pas
// un essai sur la chaudière réelle.

#include <cstdio>
#include <map>

#include "decode.h"
#include "kw_protocol.h"
#include "scheduler.h"
#include "sim.h"
#include "test_framework.h"

using namespace esphome::visslink;
using vltest::MockSerial;
using vltest::SimBoiler;

namespace {

struct Bench {
  /// Ce que le composant sait d'un registre (VissLinkRegister).
  struct Reg {
    uint16_t address;
    DataType type;
    bool binary;
    double scale;
  };

  MockSerial serial;
  SimBoiler boiler{&serial};
  KwProtocol kw;
  RegisterScheduler scheduler;
  std::vector<Reg> regs;
  std::map<size_t, int> successes, failures, gave_up;
  std::map<size_t, double> last_value;
  std::map<size_t, BinaryValue> last_binary;
  std::map<size_t, uint32_t> last_success_ms, max_gap_ms;
  uint32_t stagger_ms{2000};  ///< VissLinkComponent::STARTUP_STAGGER_MS

  Bench() {
    boiler.registers[0x0800] = {0xCC, 0xFF};  // -5,2 °C
    boiler.registers[0x0802] = {0x2C, 0x02};  // 55,6 °C
    boiler.registers[0x0804] = {0xDB, 0x01};  // 47,5 °C
  }
  size_t add(uint16_t address, uint32_t interval_ms, uint32_t now, DataType type = DataType::INT16,
             double scale = 0.1) {
    regs.push_back({address, type, false, scale});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  size_t add_binary(uint16_t address, uint32_t interval_ms, uint32_t now) {
    regs.push_back({address, DataType::UINT8, true, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  void add_defaults(uint32_t now) {
    add(0x0800, 60000, now);
    add(0x0802, 30000, now);
    add(0x0804, 60000, now);
  }
  static uint8_t length(const Reg &r) { return r.binary ? 1 : data_type_length(r.type); }

  /// Même enchaînement que VissLinkComponent::loop().
  void step(uint32_t now) {
    const KwResult result = kw.loop(now, serial);
    if (result != KwResult::NONE) {
      const size_t index = scheduler.current();
      if (result == KwResult::SUCCESS) {
        scheduler.finish(true, now);
        successes[index]++;
        const Reg &r = regs[index];
        if (r.binary)
          last_binary[index] = decode_binary(kw.data()[0]);
        else
          last_value[index] = decode_value(r.type, kw.data()) * r.scale;
        if (last_success_ms.count(index) && now - last_success_ms[index] > max_gap_ms[index])
          max_gap_ms[index] = now - last_success_ms[index];
        last_success_ms[index] = now;
      } else {
        failures[index]++;
        if (scheduler.finish(false, now))
          gave_up[index]++;
      }
    }
    if (kw.is_idle()) {
      const int index = scheduler.acquire(now);
      if (index != RegisterScheduler::NONE && !kw.request_read(regs[index].address, length(regs[index]), now))
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

// --- v0.2 : registres typés, intervalles 30 s à 1 h ------------------------

namespace {

constexpr uint32_t S = 1000;
constexpr uint32_t MIN = 60 * S;
constexpr uint32_t H = 60 * MIN;

/// Jeu de registres de l'exemple v0.2, y compris les expérimentaux (18).
struct V02 {
  size_t outdoor, boiler_temp, dhw, flow_m2, setpoint_calc;
  size_t a1_comfort, a1_reduced, m2_comfort, m2_reduced, dhw_setpoint;
  size_t burner, starts, hours;
  size_t circulation, dhw_charge, fault, pump_a1, pump_m2;

  explicit V02(Bench &b, uint32_t now) {
    auto &r = b.boiler.registers;
    r[0x3900] = {0x5E, 0x01};              // 35,0 °C
    r[0x5502] = {0x4A, 0x02};              // 58,6 °C
    r[0x2306] = {0x14};                    // 20 °C
    r[0x2307] = {0x10};                    // 16 °C
    r[0x3306] = {0x15};                    // 21 °C
    r[0x3307] = {0x11};                    // 17 °C
    r[0x6300] = {0x32};                    // 50 °C
    r[0x0842] = {0x01};                    // brûleur en marche
    r[0x088A] = {0x39, 0x30, 0x00, 0x00};  // 12 345 démarrages
    r[0x08A7] = {0x00, 0x51, 0x25, 0x02};  // 36 000 000 s = 10 000 h
    r[0x0846] = {0x00};
    r[0x0845] = {0x01};
    r[0x0883] = {0x00};
    r[0x2906] = {0x01};
    r[0x3906] = {0x00};

    outdoor = b.add(0x0800, 60 * S, now);
    boiler_temp = b.add(0x0802, 60 * S, now);
    dhw = b.add(0x0804, 60 * S, now);
    flow_m2 = b.add(0x3900, 60 * S, now);
    setpoint_calc = b.add(0x5502, 60 * S, now);
    a1_comfort = b.add(0x2306, 5 * MIN, now, DataType::UINT8, 1);
    a1_reduced = b.add(0x2307, 5 * MIN, now, DataType::UINT8, 1);
    m2_comfort = b.add(0x3306, 5 * MIN, now, DataType::UINT8, 1);
    m2_reduced = b.add(0x3307, 5 * MIN, now, DataType::UINT8, 1);
    dhw_setpoint = b.add(0x6300, 5 * MIN, now, DataType::UINT8, 1);
    burner = b.add_binary(0x0842, 30 * S, now);
    starts = b.add(0x088A, H, now, DataType::UINT32, 1);
    hours = b.add(0x08A7, H, now, DataType::UINT32, 0.00027777778);
    circulation = b.add_binary(0x0846, 60 * S, now);
    dhw_charge = b.add_binary(0x0845, 60 * S, now);
    fault = b.add_binary(0x0883, 60 * S, now);
    pump_a1 = b.add_binary(0x2906, 60 * S, now);
    pump_m2 = b.add_binary(0x3906, 60 * S, now);
  }
};

}  // namespace

TEST(sim_v02_all_types_decoded) {
  Bench b;
  V02 v(b, 0);
  b.run(0, 10 * MIN);
  CHECK(b.failures.empty());
  CHECK_NEAR(b.last_value[v.outdoor], -5.2, 1e-6);  // négatif
  CHECK_NEAR(b.last_value[v.flow_m2], 35.0, 1e-6);
  CHECK_NEAR(b.last_value[v.setpoint_calc], 58.6, 1e-6);
  CHECK_EQ(b.last_value[v.a1_comfort], 20);
  CHECK_EQ(b.last_value[v.m2_reduced], 17);
  CHECK_EQ(b.last_value[v.dhw_setpoint], 50);
  CHECK_EQ(b.last_value[v.starts], 12345);
  CHECK_NEAR(b.last_value[v.hours], 10000.0, 0.01);
  CHECK(b.last_binary[v.burner] == BinaryValue::ON);
  CHECK(b.last_binary[v.circulation] == BinaryValue::OFF);
  CHECK(b.last_binary[v.pump_a1] == BinaryValue::ON);
}

TEST(sim_v02_request_length_matches_type) {
  Bench b;
  V02 v(b, 0);
  b.run(0, 10 * MIN);
  const auto &len = b.boiler.lengths_by_address;
  auto only = [&](uint16_t a, uint8_t l) { return len.count(a) && len.at(a) == std::set<uint8_t>{l}; };
  CHECK(only(0x0800, 2));
  CHECK(only(0x5502, 2));
  CHECK(only(0x2306, 1));
  CHECK(only(0x6300, 1));
  CHECK(only(0x0842, 1));
  CHECK(only(0x088A, 4));
  CHECK(only(0x08A7, 4));
  CHECK(only(0x3906, 1));
}

TEST(sim_v02_alternating_types_back_to_back) {
  // Lectures enchaînées de longueurs 1, 2, 4 : aucun octet d'une réponse ne
  // doit déborder sur la suivante (le contrôle de cadrage le détecterait).
  Bench b;
  b.boiler.registers[0x088A] = {0x39, 0x30, 0x00, 0x00};
  b.boiler.registers[0x2306] = {0x14};
  b.boiler.registers[0x0842] = {0x01};
  const size_t t = b.add(0x0800, 1 * S, 0);
  const size_t u32 = b.add(0x088A, 1 * S, 0, DataType::UINT32, 1);
  const size_t u8 = b.add(0x2306, 1 * S, 0, DataType::UINT8, 1);
  const size_t bin = b.add_binary(0x0842, 1 * S, 0);
  b.run(0, 5 * MIN);
  CHECK(b.failures.empty());
  for (size_t i : {t, u32, u8, bin})
    CHECK(b.successes[i] >= 20);
  CHECK_NEAR(b.last_value[t], -5.2, 1e-6);
  CHECK_EQ(b.last_value[u32], 12345);
  CHECK_EQ(b.last_value[u8], 20);
  CHECK(b.last_binary[bin] == BinaryValue::ON);
  CHECK_EQ(b.boiler.requests, b.total_successes());
}

TEST(sim_v02_eighteen_registers_no_saturation) {
  // 18 registres, 30 s à 1 h, chaudière réaliste (~1 lecture / 3 s, soit
  // ~80 % de charge) : aucun cycle perdu, aucune requête en double, retard
  // ponctuel borné. Mesure en régime établi, après 10 min.
  Bench b;
  V02 v(b, 0);
  b.run(0, 10 * MIN);
  b.max_gap_ms.clear();
  b.run(10 * MIN, 2 * H);  // 130 min au total
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());
  CHECK(b.successes[v.burner] >= 259);       // 30 s : 260 attendues
  CHECK(b.successes[v.outdoor] >= 129);      // 60 s : 130
  CHECK(b.successes[v.circulation] >= 129);  // 60 s : 130
  CHECK(b.successes[v.a1_comfort] >= 26);    // 5 min : 26
  CHECK(b.successes[v.starts] == 3);         // 1 h : 3 (t = 0, 1 h, 2 h)
  CHECK(b.successes[v.hours] == 3);
  for (size_t i = 0; i < b.regs.size(); i++) {
    const uint32_t interval = b.scheduler.entry(i).interval_ms;
    if (interval <= 5 * MIN)
      CHECK(b.max_gap_ms[i] <= interval + 20 * S);
  }
}

TEST(sim_v02_simultaneous_start_loses_cycles) {
  // Justifie le décalage de démarrage : sans lui, les échéances restent
  // alignées (échéance + intervalle) et une rafale toutes les 5 min fait
  // sauter des cycles au registre de 30 s.
  Bench b;
  b.stagger_ms = 0;
  V02 v(b, 0);
  b.run(0, 130 * MIN);
  CHECK(b.successes[v.burner] < 259);
}

TEST(sim_v02_slow_boiler_degrades_without_starvation) {
  // Synchro toutes les 8 s après chaque réponse : capacité (~7 lectures/min)
  // inférieure à la demande (~16/min). Les intervalles s'allongent mais
  // chaque registre reste lu, sans rafale ni doublon.
  Bench b;
  b.boiler.post_response_delay_ms = 8000;
  V02 v(b, 0);
  b.run(0, 2 * H);
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());
  for (size_t i = 0; i < b.regs.size(); i++) {
    CHECK(b.successes[i] >= 2);
    CHECK(b.max_gap_ms[i] <= H + 18 * 9 * S);  // au pire N lectures d'attente
  }
}

TEST(sim_v02_timeouts_and_recovery_mixed_types) {
  Bench b;
  V02 v(b, 0);
  b.boiler.register_faults[0x08A7] = SimBoiler::Fault::NO_RESPONSE;   // uint32 muet
  b.boiler.register_faults[0x0842] = SimBoiler::Fault::LONG_RESPONSE;  // binaire désynchronisé
  b.run(0, 10 * MIN);
  CHECK_EQ(b.successes[v.hours], 0);
  CHECK(b.gave_up[v.hours] >= 1);
  CHECK_EQ(b.successes[v.burner], 0);
  CHECK(b.gave_up[v.burner] >= 5);
  CHECK(b.successes[v.outdoor] >= 7);  // les autres continuent
  CHECK(b.successes[v.starts] >= 1);

  b.boiler.register_faults.clear();
  b.boiler.connected = false;  // interface débranchée 2 minutes
  b.run(10 * MIN, 2 * MIN);
  b.boiler.connected = true;
  const int burner_before = b.successes[v.burner];
  b.run(12 * MIN, 50 * MIN);
  CHECK(b.successes[v.burner] - burner_before >= 80);
  CHECK(b.last_binary[v.burner] == BinaryValue::ON);
  CHECK(b.successes[v.hours] >= 1);  // relu au cycle suivant (1 h)
  CHECK_NEAR(b.last_value[v.hours], 10000.0, 0.01);
}

TEST(sim_v02_stagger_holds_30s_reads_across_boiler_timings) {
  // Délai réel de reprise du 0x05 après une réponse inconnu : 2 à 4 s
  // balayés. 18 registres, décalage initial de 2 s par registre.
  for (uint32_t post : {2000u, 2500u, 3000u, 3500u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    V02 v(b, 0);

    // Démarrage : les 18 registres sont lus en moins de 90 s, et le
    // brûleur garde déjà sa cadence de 30 s.
    b.run(0, 90 * S);
    CHECK_EQ(b.successes.size(), b.regs.size());
    CHECK(b.max_gap_ms[v.burner] <= 35 * S);

    // Régime établi sur 2 h : aucun cycle de 30 s ou 60 s perdu.
    b.run(90 * S, 10 * MIN - 90 * S);
    const int burner0 = b.successes[v.burner], outdoor0 = b.successes[v.outdoor];
    const int circulation0 = b.successes[v.circulation];
    b.max_gap_ms.clear();
    b.run(10 * MIN, 2 * H);
    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.requests, b.total_successes());
    CHECK(b.successes[v.burner] - burner0 >= 239);  // 240 attendues
    CHECK(b.successes[v.outdoor] - outdoor0 >= 119);  // 120 attendues
    CHECK(b.successes[v.circulation] - circulation0 >= 119);
    CHECK(b.max_gap_ms[v.burner] < 60 * S);  // retard ponctuel, jamais un cycle sauté
    if (vltest::failures() > 0)
      std::printf("    post_response_delay_ms=%u\n", static_cast<unsigned>(post));
  }
}
