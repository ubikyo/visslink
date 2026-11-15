// Simulations de bout en bout : KwProtocol + RegisterScheduler face à une
// Vitotronic simulée. Ces tests éprouvent la logique, ils ne remplacent pas
// un essai sur la chaudière réelle.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <memory>

#include "decode.h"
#include "kw_protocol.h"
#include "schedule.h"
#include "scheduler.h"
#include "sim.h"
#include "test_framework.h"
#include "write_guard.h"

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
    bool schedule{false};  ///< programmation : blocs de 8 octets, adresse + 8 x bloc
    bool number{false};    ///< consigne réglable (v0.3), uint8
    bool mode{false};      ///< programme de fonctionnement (v0.3), sélecteur
    float min{0}, max{0};  ///< bornes configurées (number)
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
  std::map<size_t, WeekSchedule> weeks;  ///< programmations décodées
  std::map<size_t, int> rejected;        ///< blocs bien cadrés mais rejetés au décodage
  uint32_t stagger_ms{2000};  ///< VissLinkComponent::STARTUP_STAGGER_MS

  // v0.3 : écritures (même logique que VissLinkComponent).
  bool enable_writes{false};
  bool enable_mode_writes{false};
  bool enable_m2_mode_write{false};
  // v0.4.1 (false = comportement v0.4, pour comparaison) :
  bool preempt_unsent_reads{true};  ///< une commande prend la place d'une lecture pas encore émise
  bool strict_read_back{true};      ///< aucune lecture ordinaire pendant les relectures
  int preemptions{0};
  WriteTiming timing;
  std::vector<WriteTiming> timings;
  ReadBackTracker read_back;
  std::map<size_t, int> mode_state;  ///< programme publié (brut 00/03/05, -1 = inconnu)
  std::vector<ModeDivergence> divergence_log;  ///< divergence A1/M2 après chaque relecture de programme
  std::vector<ModeDivergence> divergence_after_write;
  bool link_ok{false};
  bool write_in_flight{false};
  WriteArbiter writes;
  std::map<size_t, WriteVerification> verify;
  std::map<size_t, float> number_state;  ///< état publié (NAN = inconnu)
  std::vector<VerifyResult> verify_results;
  std::vector<KwResult> write_results;
  std::vector<uint32_t> write_start_ms;
  int expired_writes{0};
  int refused_at_send{0};
  std::vector<std::pair<uint32_t, uint8_t>> params;  ///< (intervalle, blocs) par registre, pour reboot()

  Bench() {
    boiler.registers[0x0800] = {0xCC, 0xFF};  // -5,2 °C
    boiler.registers[0x0802] = {0x2C, 0x02};  // 55,6 °C
    boiler.registers[0x0804] = {0xDB, 0x01};  // 47,5 °C
  }
  size_t add(uint16_t address, uint32_t interval_ms, uint32_t now, DataType type = DataType::INT16,
             double scale = 0.1) {
    regs.push_back({address, type, false, scale});
    params.push_back({interval_ms, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  size_t add_binary(uint16_t address, uint32_t interval_ms, uint32_t now) {
    regs.push_back({address, DataType::UINT8, true, 1});
    params.push_back({interval_ms, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  size_t add_schedule(uint16_t address, uint32_t interval_ms, uint32_t now, uint8_t blocks = SCHEDULE_DAYS) {
    regs.push_back({address, DataType::UINT8, false, 1, true});
    params.push_back({interval_ms, blocks});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1), blocks);
    return regs.size() - 1;
  }
  size_t add_number(uint16_t address, float min, float max, uint32_t interval_ms, uint32_t now) {
    Reg r{address, DataType::UINT8, false, 1};
    r.number = true;
    r.min = min;
    r.max = max;
    regs.push_back(r);
    params.push_back({interval_ms, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  size_t add_mode(uint16_t address, uint32_t interval_ms, uint32_t now) {
    Reg r{address, DataType::UINT8, false, 1};
    r.mode = true;
    regs.push_back(r);
    params.push_back({interval_ms, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  int mode_peer(size_t index) const {
    if (!regs[index].mode)
      return ReadBackTracker::NONE;
    for (size_t i = 0; i < regs.size(); i++) {
      if (i != index && regs[i].mode)
        return static_cast<int>(i);
    }
    return ReadBackTracker::NONE;
  }
  int mode_of(size_t index) const {
    auto it = mode_state.find(index);
    return it == mode_state.end() ? -1 : it->second;
  }
  ModeDivergence divergence() const {
    int a = -1, b = -1;
    for (size_t i = 0; i < regs.size(); i++) {
      if (regs[i].address == 0x2301)
        a = mode_of(i);
      if (regs[i].address == 0x3301)
        b = mode_of(i);
    }
    return mode_divergence(a, b);
  }
  /// VissLinkComponent::submit_mode_write.
  std::string request_mode(size_t index, size_t option, uint32_t now) {
    const Reg &r = regs[index];
    const uint8_t value = option < OPERATING_MODE_COUNT ? OPERATING_MODE_VALUES[option] : 0xFF;
    bool busy = false;
    for (size_t i = 0; i < regs.size(); i++)
      busy |= regs[i].mode && writes.has_request(static_cast<int>(i));
    switch (mode_write_permitted(r.address, enable_writes, enable_mode_writes, enable_m2_mode_write)) {
      case ModePermission::OK:
        break;
      case ModePermission::WRITES_DISABLED:
        return "disabled";
      case ModePermission::MODE_WRITES_DISABLED:
        return "mode disabled";
      case ModePermission::M2_WRITE_DISABLED:
        return "m2 disabled";
      case ModePermission::NOT_WHITELISTED:
        return "read-only";
    }
    if (option >= OPERATING_MODE_COUNT)
      return write_check_to_string(WriteCheck::VALUE_NOT_ALLOWED);
    if (check_mode_write(r.address, 1, value) != WriteCheck::OK)
      return write_check_to_string(check_mode_write(r.address, 1, value));
    if (mode_of(index) < 0)
      return "not read";
    if (!link_ok)
      return "link";
    if (busy || read_back.waiting(static_cast<int>(index)))
      return "busy";
    if (mode_of(index) == value)
      return "unchanged";
    return writes.submit(static_cast<int>(index), r.address, value, now) == WriteArbiter::Submit::FULL ? "full"
                                                                                                    : "queued";
  }
  /// VissLinkComponent::read_back_done_.
  void read_back_done(size_t index, uint32_t now) {
    if (writes.active() && writes.current().owner == static_cast<int>(index) &&
        read_back.waiting(static_cast<int>(index)))
      timing.read_back = now;  // état du registre écrit publié
    if (!writes.active() || !read_back.done(static_cast<int>(index)))
      return;
    timing.done = now;
    timings.push_back(timing);
    const bool mode_write = regs[writes.current().owner].mode;
    writes.complete(now);
    if (mode_write)
      divergence_after_write.push_back(divergence());
  }
  bool has_read_value(size_t index) const {
    auto it = number_state.find(index);
    return it != number_state.end() && !std::isnan(it->second);
  }
  /// VissLinkComponent::submit_write : renvoie la cause du refus, ou
  /// "queued" / "replaced" / "unchanged".
  std::string request(size_t index, float value, uint32_t now) {
    const Reg &r = regs[index];
    uint8_t byte = 0;
    const WriteCheck check = check_write_value(r.address, 1, value, r.min, r.max, &byte);
    if (!enable_writes)
      return "disabled";
    if (check != WriteCheck::OK)
      return write_check_to_string(check);
    if (!has_read_value(index))
      return "not read";
    if (!link_ok)
      return "link";
    if (!writes.has_request(static_cast<int>(index)) && byte == static_cast<uint8_t>(number_state[index]))
      return "unchanged";
    switch (writes.submit(static_cast<int>(index), r.address, byte, now)) {
      case WriteArbiter::Submit::QUEUED:
        return "queued";
      case WriteArbiter::Submit::REPLACED:
        return "replaced";
      case WriteArbiter::Submit::FULL:
        return "full";
    }
    return "?";
  }
  /// Redémarrage de l'ESP : tout l'état en RAM est perdu (protocole,
  /// ordonnanceur, demandes d'écriture, états publiés). La configuration
  /// (registres, enable_writes) est inchangée ; la chaudière et la ligne aussi.
  void reboot(uint32_t now) {
    kw = KwProtocol();
    scheduler = RegisterScheduler();
    for (size_t i = 0; i < params.size(); i++)
      scheduler.add(params[i].first, now + stagger_ms * i, params[i].second);
    writes = WriteArbiter();
    write_in_flight = false;
    link_ok = false;
    verify.clear();
    number_state.clear();
    mode_state.clear();
    read_back = ReadBackTracker();
    serial.rx.clear();  // tampon de réception de l'UART perdu
  }
  /// Laisse tourner la chaudière seule (ESP en cours de démarrage).
  void boiler_only(uint32_t from, uint32_t duration_ms) {
    for (uint32_t t = from; t - from < duration_ms; t += 5)
      boiler.tick(t);
  }
  int boiler_value(uint16_t address) { return boiler.registers[address].empty() ? -1 : boiler.registers[address][0]; }

  void add_defaults(uint32_t now) {
    add(0x0800, 60000, now);
    add(0x0802, 30000, now);
    add(0x0804, 60000, now);
  }
  static uint8_t length(const Reg &r) {
    if (r.number || r.mode)
      return 1;
    if (r.schedule)
      return SCHEDULE_BLOCK_LENGTH;
    return r.binary ? 1 : data_type_length(r.type);
  }
  static uint16_t block_address(const Reg &r, uint8_t block) {
    return r.schedule ? schedule_day_address(r.address, block) : r.address;
  }

  /// Même enchaînement que VissLinkComponent::loop() / handle_result_().
  void step(uint32_t now) {
    const KwResult result = kw.loop(now, serial);
    if (result != KwResult::NONE && write_in_flight) {
      // VissLinkComponent::handle_write_result_
      write_in_flight = false;
      write_results.push_back(result);
      timing.sent = kw.sent_ms();
      timing.acked = now;
      if (result == KwResult::SYNC_TIMEOUT)
        link_ok = false;
      const WriteArbiter::Request &w = writes.current();
      verify[w.owner].expect(w.value);
      const int peer = mode_peer(w.owner);
      read_back.start(w.owner, peer);
      if (peer != ReadBackTracker::NONE && !scheduler.expedite(peer, now + 1))  // écrit relu d'abord
        read_back.done(peer);
      if (!scheduler.expedite(w.owner, now)) {
        read_back.done(w.owner);
        if (read_back.idle())
          writes.complete(now);
      }
    } else if (result != KwResult::NONE) {
      if (result == KwResult::SUCCESS)
        link_ok = true;
      else if (result == KwResult::SYNC_TIMEOUT)
        link_ok = false;
      const size_t index = scheduler.current();
      const uint8_t block = scheduler.block(index);
      const Reg &r = regs[index];
      bool accepted = result == KwResult::SUCCESS;
      if (accepted && r.schedule) {
        DaySchedule day;
        accepted = decode_schedule_day(kw.data(), kw.data_length(), &day) == ScheduleError::NONE;
        if (accepted)
          weeks[index].set_day(block, day);
        else
          rejected[index]++;
      }
      if (accepted) {
        scheduler.finish(true, now);
        successes[index]++;
        if (r.schedule) {
          // décodé ci-dessus
        } else if (r.number) {
          const uint8_t value = kw.data()[0];
          if (verify[index].pending())
            verify_results.push_back(verify[index].on_read(value));
          number_state[index] = value;  // toujours la valeur lue
          read_back_done(index, now);
        } else if (r.mode) {
          const uint8_t raw = kw.data()[0];
          if (verify[index].pending())
            verify_results.push_back(verify[index].on_read(raw));
          mode_state[index] = operating_mode_index(raw) >= 0 ? raw : -1;  // toujours la valeur lue
          divergence_log.push_back(divergence());
          read_back_done(index, now);
        } else if (r.binary)
          last_binary[index] = decode_binary(kw.data()[0]);
        else
          last_value[index] = decode_value(r.type, kw.data()) * r.scale;
        if (last_success_ms.count(index) && now - last_success_ms[index] > max_gap_ms[index])
          max_gap_ms[index] = now - last_success_ms[index];
        last_success_ms[index] = now;
      } else {
        failures[index]++;
        if (scheduler.finish(false, now)) {
          gave_up[index]++;
          if (r.number || r.mode) {
            if (verify[index].pending())
              verify_results.push_back(verify[index].on_give_up());
            if (r.number)
              number_state[index] = NAN;
            else
              mode_state[index] = -1;
            read_back_done(index, now);
          }
        }
      }
    }
    // VissLinkComponent::preempt_unsent_read_ (v0.4.1).
    if (preempt_unsent_reads && enable_writes && !write_in_flight && kw.is_waiting_sync() &&
        scheduler.current() != RegisterScheduler::NONE && !scheduler.entry(scheduler.current()).urgent &&
        writes.ready(now, link_ok) && kw.cancel_before_send()) {
      scheduler.release();
      preemptions++;
    }
    if (kw.is_idle() && scheduler.current() == RegisterScheduler::NONE) {
      WriteArbiter::Request w;
      while (writes.pop_expired(now, &w))
        expired_writes++;
      if (enable_writes && writes.next(now, link_ok, &w)) {
        // VissLinkComponent::start_write_ : contrôle complet avant émission
        // (VissLinkRegister::write_allowed, refus par défaut hors number).
        const Reg &r = regs[w.owner];
        uint8_t encoded = 0;
        const bool number_ok = r.number && find_writable(w.address) != nullptr &&
                               check_write_value(w.address, 1, w.value, r.min, r.max, &encoded) == WriteCheck::OK &&
                               encoded == w.value;
        const bool mode_ok =
            r.mode && is_mode_register(w.address) && check_mode_write(w.address, 1, w.value) == WriteCheck::OK &&
            mode_write_permitted(w.address, enable_writes, enable_mode_writes, enable_m2_mode_write) ==
                ModePermission::OK;
        const bool allowed = r.address == w.address && is_write_whitelisted(w.address) && (number_ok || mode_ok);
        if (allowed && kw.request_write(w.address, &w.value, 1, now)) {
          write_in_flight = true;
          write_start_ms.push_back(now);
          timing = WriteTiming();
          timing.requested = w.requested_ms;
          timing.started = now;
        } else {
          refused_at_send++;
          writes.complete(now);
        }
      }
    }
    if (kw.is_idle()) {
      const int index = scheduler.acquire(now, strict_read_back && !read_back.idle());
      if (index != RegisterScheduler::NONE &&
          !kw.request_read(block_address(regs[index], scheduler.block(index)), length(regs[index]), now))
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

// --- v0.2.1 : programmations horaires --------------------------------------

namespace {

constexpr uint16_t A1 = 0x2000, M2 = 0x3000, DHW = 0x2100, LOOP = 0x2200;

/// Trames relevées sur la chaudière, recopiées sur les 7 jours.
void load_real_schedules(SimBoiler &boiler) {
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    boiler.registers[schedule_day_address(A1, d)] = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    boiler.registers[schedule_day_address(M2, d)] = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    boiler.registers[schedule_day_address(DHW, d)] = {0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    boiler.registers[schedule_day_address(LOOP, d)] = {0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF};
  }
}

/// Les 12 registres actifs de l'exemple, brûleur déclaré en dernier (pire
/// cas pour le départage par ordre de déclaration).
struct Active {
  size_t outdoor, burner;
  explicit Active(Bench &b, uint32_t now) {
    auto &r = b.boiler.registers;
    r[0x0842] = {0x01};
    outdoor = b.add(0x0800, 60 * S, now);
    b.add(0x0802, 60 * S, now);
    b.add(0x0804, 60 * S, now);
    b.add(0x3900, 60 * S, now);
    for (uint16_t a : {0x2306, 0x2307, 0x3306, 0x3307, 0x6300})
      b.add(a, 5 * MIN, now, DataType::UINT8, 1);
    b.add(0x088A, H, now, DataType::UINT32, 1);
    b.add(0x08A7, H, now, DataType::UINT32, 1);
    burner = b.add_binary(0x0842, 30 * S, now);
  }
};

struct Circuits {
  size_t a1, m2, dhw, loop;
  Circuits(Bench &b, uint32_t now, uint32_t interval = 6 * H) {
    load_real_schedules(b.boiler);
    a1 = b.add_schedule(A1, interval, now);
    m2 = b.add_schedule(M2, interval, now);
    dhw = b.add_schedule(DHW, interval, now);
    loop = b.add_schedule(LOOP, interval, now);
  }
};

}  // namespace

TEST(sim_v021_four_circuits_read_and_decoded) {
  Bench b;
  Active act(b, 0);
  Circuits c(b, 0);
  b.run(0, 10 * MIN);
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());
  for (size_t i : {c.a1, c.m2, c.dhw, c.loop}) {
    CHECK(b.weeks[i].all_known());
    CHECK_EQ(b.successes[i], 7);  // un cycle de 7 blocs, pas de relecture avant 6 h
  }
  CHECK(format_week(b.weeks[c.a1]) == "Lu-Di 06:00-22:00");
  CHECK(format_week(b.weeks[c.m2]) == "Lu-Di 06:00-22:00");
  CHECK(format_week(b.weeks[c.dhw]) == "Lu-Di 04:00-23:00");
  CHECK(format_week(b.weeks[c.loop]) == "Lu-Di 06:10-08:30, 21:40-21:50");
}

TEST(sim_v021_requests_cover_seven_days_with_8_bytes) {
  Bench b;
  Circuits c(b, 0);
  b.run(0, 10 * MIN);
  for (uint16_t base : {A1, M2, DHW, LOOP}) {
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
      const uint16_t a = schedule_day_address(base, d);
      CHECK_EQ(b.boiler.requests_by_address[a], 1);
      CHECK(b.boiler.lengths_by_address[a] == std::set<uint8_t>{8});
    }
  }
  CHECK_EQ(b.boiler.requests, 28);
}

TEST(sim_v021_days_differ_per_circuit) {
  Bench b;
  Circuits c(b, 0);
  // Samedi et dimanche différents sur A1, mercredi vide sur le bouclage.
  for (uint8_t d : {5, 6})
    b.boiler.registers[schedule_day_address(A1, d)] = {0x38, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  b.boiler.registers[schedule_day_address(LOOP, 2)] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  b.run(0, 10 * MIN);
  CHECK(format_week(b.weeks[c.a1]) == "Lu-Ve 06:00-22:00 | Sa-Di 07:00-23:00");
  CHECK(format_week(b.weeks[c.loop]) ==
        "Lu-Ma 06:10-08:30, 21:40-21:50 | Me aucune plage | Je-Di 06:10-08:30, 21:40-21:50");
  CHECK(format_week(b.weeks[c.m2]) == "Lu-Di 06:00-22:00");
}

TEST(sim_v021_load_keeps_burner_cadence) {
  // 12 registres actifs + 4 programmations (28 blocs toutes les 6 h), sur
  // 6 h 30 (démarrage + un rafraîchissement complet). Référence : la même
  // configuration sans programmation. Le brûleur ne perd aucun cycle et son
  // écart maximal entre deux lectures n'augmente pas.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    Active ref_act(ref, 0);
    Active act(b, 0);
    Circuits c(b, 0);
    ref.run(0, 6 * H + 30 * MIN);
    b.run(0, 6 * H + 30 * MIN);

    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.requests, b.total_successes());  // aucune requête en double
    CHECK_EQ(b.successes[act.burner], ref.successes[ref_act.burner]);
    CHECK(b.max_gap_ms[act.burner] <= ref.max_gap_ms[ref_act.burner] + 1 * S);
    CHECK(b.max_gap_ms[act.outdoor] <= 90 * S);  // 60 s : jamais deux cycles perdus
    for (size_t i : {c.a1, c.m2, c.dhw, c.loop})
      CHECK_EQ(b.successes[i], 14);  // 2 cycles complets : t = 0 et t = 6 h
    if (vltest::failures() > 0)
      std::printf("    post=%u burner gap %u ms (ref %u ms)\n", static_cast<unsigned>(post),
                  static_cast<unsigned>(b.max_gap_ms[act.burner]),
                  static_cast<unsigned>(ref.max_gap_ms[ref_act.burner]));
  }
}

TEST(sim_v021_refresh_spread_over_minutes) {
  // Les 28 blocs sont étalés (~30 s entre deux blocs d'un circuit) : au plus
  // une lecture de programmation par circuit dans toute fenêtre de 30 s.
  Bench b;
  Active act(b, 0);
  Circuits c(b, 0);
  b.run(0, 2 * MIN);
  for (size_t i : {c.a1, c.m2, c.dhw, c.loop})
    CHECK(b.successes[i] <= 4);
  b.run(2 * MIN, 8 * MIN);
  for (size_t i : {c.a1, c.m2, c.dhw, c.loop})
    CHECK_EQ(b.successes[i], 7);  // complet en moins de 10 min
}

TEST(sim_v021_28_independent_registers_would_starve_burner) {
  // Justifie le regroupement des 7 jours en une entrée : déclarés comme 28
  // registres indépendants à 6 h, les blocs reviennent en rafale et le
  // brûleur saute des cycles (relevé : ~90 s sans lecture).
  Bench b;
  b.boiler.post_response_delay_ms = 3000;
  V02 v(b, 0);
  load_real_schedules(b.boiler);
  for (uint16_t base : {A1, M2, DHW, LOOP})
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      b.add_schedule(schedule_day_address(base, d), 6 * H, 0, 1);
  b.run(0, 10 * MIN);
  b.max_gap_ms.clear();
  b.run(10 * MIN, 6 * H);
  CHECK(b.max_gap_ms[v.burner] > 60 * S);
}

TEST(sim_v021_timeouts_short_responses_and_recovery) {
  Bench b;
  Active act(b, 0);
  Circuits c(b, 0, 30 * MIN);
  b.boiler.register_faults[schedule_day_address(A1, 2)] = SimBoiler::Fault::NO_RESPONSE;       // mercredi muet
  b.boiler.register_faults[schedule_day_address(LOOP, 1)] = SimBoiler::Fault::SHORT_RESPONSE;  // mardi tronqué
  b.boiler.register_faults[schedule_day_address(DHW, 4)] = SimBoiler::Fault::LONG_RESPONSE;    // vendredi trop long
  b.run(0, 10 * MIN);
  CHECK(!b.weeks[c.a1].is_known(2));
  CHECK(!b.weeks[c.loop].is_known(1));
  CHECK(!b.weeks[c.dhw].is_known(4));
  CHECK(b.gave_up[c.a1] == 1 && b.gave_up[c.loop] == 1 && b.gave_up[c.dhw] == 1);
  CHECK_EQ(b.weeks[c.a1].known_mask, 0x7F & ~(1 << 2));  // les autres jours sont lus
  CHECK(b.weeks[c.m2].all_known());
  CHECK(format_week(b.weeks[c.a1]) == "Lu-Ma 06:00-22:00 | Me non lu | Je-Di 06:00-22:00");
  CHECK(b.successes[act.burner] >= 19);  // le brûleur continue

  // Interface débranchée 2 min puis défauts levés : tout est relu au cycle
  // suivant (30 min ici), sans intervention.
  b.boiler.register_faults.clear();
  b.boiler.connected = false;
  b.run(10 * MIN, 2 * MIN);
  b.boiler.connected = true;
  b.run(12 * MIN, 40 * MIN);
  for (size_t i : {c.a1, c.m2, c.dhw, c.loop})
    CHECK(b.weeks[i].all_known());
  CHECK(format_week(b.weeks[c.a1]) == "Lu-Di 06:00-22:00");
}

TEST(sim_v021_malformed_block_never_replaces_good_data) {
  Bench b;
  Circuits c(b, 0, 30 * MIN);
  b.run(0, 10 * MIN);
  CHECK(format_week(b.weeks[c.loop]) == "Lu-Di 06:10-08:30, 21:40-21:50");

  // Lundi devenu illisible (paire incomplète, puis minute invalide) : la
  // trame est bien cadrée mais rejetée, retentée, et l'ancienne valeur reste.
  const uint16_t monday = schedule_day_address(LOOP, 0);
  b.boiler.registers[monday] = {0x31, 0x43, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  b.run(10 * MIN, 30 * MIN);
  CHECK_EQ(b.rejected[c.loop], 3);  // 1 + 2 nouvelles tentatives
  CHECK_EQ(b.gave_up[c.loop], 1);
  b.boiler.registers[monday] = {0x36, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  b.run(40 * MIN, 30 * MIN);
  CHECK_EQ(b.rejected[c.loop], 6);
  CHECK(b.weeks[c.loop].format_day_or_unknown(0) == "06:10-08:30, 21:40-21:50");

  // Modification légitime sur la régulation : prise en compte au cycle suivant.
  b.boiler.registers[monday] = {0x31, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  b.run(70 * MIN, 30 * MIN);
  CHECK(b.weeks[c.loop].format_day_or_unknown(0) == "06:10-08:30");
  CHECK(format_week(b.weeks[c.loop]) == "Lu 06:10-08:30 | Ma-Di 06:10-08:30, 21:40-21:50");
}

TEST(sim_v021_coexists_with_all_v02_registers) {
  // 18 registres v0.2 (expérimentaux compris) + 4 programmations, chaudière
  // nominale : aucun cycle perdu, aucun doublon, et pas plus de retard pour
  // le brûleur qu'avec les 18 registres seuls.
  Bench ref, b;
  V02 ref_v(ref, 0);
  V02 v(b, 0);
  Circuits c(b, 0);
  ref.run(0, 10 * MIN);
  b.run(0, 10 * MIN);
  const int burner0 = b.successes[v.burner];
  ref.max_gap_ms.clear();
  b.max_gap_ms.clear();
  ref.run(10 * MIN, 6 * H);
  b.run(10 * MIN, 6 * H);
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());
  CHECK(b.successes[v.burner] - burner0 >= 719);  // 720 attendues
  CHECK(b.max_gap_ms[v.burner] <= ref.max_gap_ms[ref_v.burner] + 1 * S);
  for (size_t i : {c.a1, c.m2, c.dhw, c.loop})
    CHECK(b.weeks[i].all_known());
}

// --- v0.2.2 : configuration de diagnostic -----------------------------------

namespace {

/// examples/iot-viessmann-diagnostic.yaml, dans l'ordre d'inscription du
/// firmware (sensor, dont les six registres TEST, puis binary_sensor, puis
/// text_sensor) : le brûleur reçoit sa première échéance après les TEST.
/// `group` : 0 = les six registres TEST, 1 = G1 (ECS), 2 = G2 (circulateurs),
/// 3 = G3 (défaut + consigne), -1 = aucun (exemple v0.2.1, référence).
struct DiagnosticExample {
  size_t outdoor, burner;
  std::vector<size_t> tests;
  std::vector<uint32_t> intervals;
  std::unique_ptr<Circuits> circuits;

  DiagnosticExample(Bench &b, int group) {
    auto &r = b.boiler.registers;
    r[0x0842] = {0x01};
    r[0x0846] = {0x01};
    r[0x0845] = {0x00};
    r[0x2906] = {0x01};
    r[0x3906] = {0x02};  // valeur hors 0/1 : publiée telle quelle
    r[0x0883] = {0x00};
    r[0x5502] = {0x4A, 0x02};
    outdoor = b.add(0x0800, 60 * S, 0);
    b.add(0x0802, 60 * S, 0);
    b.add(0x0804, 60 * S, 0);
    b.add(0x3900, 60 * S, 0);
    for (uint16_t a : {0x2306, 0x2307, 0x3306, 0x3307, 0x6300})
      b.add(a, 5 * MIN, 0, DataType::UINT8, 1);
    b.add(0x088A, H, 0, DataType::UINT32, 1);
    b.add(0x08A7, H, 0, DataType::UINT32, 1);
    if (group == 0 || group == 1) {
      test(b, 0x0846, 60 * S, DataType::UINT8);
      test(b, 0x0845, 60 * S, DataType::UINT8);
    }
    if (group == 0 || group == 2) {
      test(b, 0x2906, 2 * MIN, DataType::UINT8);
      test(b, 0x3906, 2 * MIN, DataType::UINT8);
    }
    if (group == 0 || group == 3) {
      test(b, 0x0883, 5 * MIN, DataType::UINT8);
      test(b, 0x5502, 2 * MIN, DataType::INT16);
    }
    burner = b.add_binary(0x0842, 30 * S, 0);
    circuits = std::make_unique<Circuits>(b, 0);
  }

  void test(Bench &b, uint16_t address, uint32_t interval, DataType type) {
    tests.push_back(b.add(address, interval, 0, type, 1));
    intervals.push_back(interval);
  }
};

/// 6 h 30 (démarrage + un rafraîchissement des programmations) ; écarts
/// mesurés en régime établi, après 10 min.
void run_diagnostic(Bench &b) {
  b.run(0, 10 * MIN);
  b.max_gap_ms.clear();
  b.run(10 * MIN, 6 * H + 20 * MIN);
}

}  // namespace

TEST(sim_v022_diagnostic_config_load) {
  // Exemple v0.2.1 + les six registres TEST, resynchro de 2 à 4 s.
  // Référence : l'exemple v0.2.1 seul (780 lectures du brûleur en 6 h 30).
  for (uint32_t post : {2000u, 2500u, 3000u, 3500u, 4000u}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    DiagnosticExample r(ref, -1);
    DiagnosticExample d(b, 0);
    run_diagnostic(ref);
    run_diagnostic(b);

    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.requests, b.total_successes());  // aucune requête en double
    for (size_t i : {d.circuits->a1, d.circuits->m2, d.circuits->dhw, d.circuits->loop})
      CHECK_EQ(b.successes[i], 14);  // 28 lectures horaires à t = 0 et t = 6 h
    for (size_t k = 0; k < d.tests.size(); k++)
      CHECK(b.max_gap_ms[d.tests[k]] <= 2 * d.intervals[k]);  // jamais deux cycles perdus
    CHECK(b.max_gap_ms[d.outdoor] <= 95 * S);                  // 60 s : jamais deux cycles perdus
    const int lost = ref.successes[r.burner] - b.successes[d.burner];
    if (post <= 3000) {
      // Seule la première lecture, décalée de 12 s au démarrage, peut manquer.
      CHECK(lost <= 1);
      CHECK(b.max_gap_ms[d.burner] <= ref.max_gap_ms[r.burner] + 5 * S);
    } else {
      // 3,5 à 4 s : quelques lectures retardées (relevé : 4 puis 11 sur 780).
      CHECK(lost <= 12);
      CHECK(b.max_gap_ms[d.burner] <= 65 * S);
    }
    if (vltest::failures() > 0)
      std::printf("    post=%u burner %d/%d gap %u ms (ref %u ms)\n", static_cast<unsigned>(post),
                  b.successes[d.burner], ref.successes[r.burner], static_cast<unsigned>(b.max_gap_ms[d.burner]),
                  static_cast<unsigned>(ref.max_gap_ms[r.burner]));
  }
  // Valeurs brutes : aucune conversion (0x02 reste 2, 0x5502 reste 586).
  Bench b;
  DiagnosticExample d(b, 0);
  b.run(0, 10 * MIN);
  CHECK_EQ(b.last_value[d.tests[3]], 2);
  CHECK_EQ(b.last_value[d.tests[5]], 586);
}

TEST(sim_v022_diagnostic_groups_of_two_at_4s) {
  // Stratégie de repli si la resynchro réelle approche 4 s : un groupe de
  // deux registres à la fois.
  for (int group : {1, 2, 3}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = 4000;
    DiagnosticExample r(ref, -1);
    DiagnosticExample d(b, group);
    run_diagnostic(ref);
    run_diagnostic(b);
    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.requests, b.total_successes());
    CHECK(ref.successes[r.burner] - b.successes[d.burner] <= 1);
    CHECK(b.max_gap_ms[d.burner] <= ref.max_gap_ms[r.burner] + 5 * S);
    for (size_t i : {d.circuits->a1, d.circuits->m2, d.circuits->dhw, d.circuits->loop})
      CHECK_EQ(b.successes[i], 14);
    if (vltest::failures() > 0)
      std::printf("    group=%d burner %d/%d gap %u ms (ref %u ms)\n", group, b.successes[d.burner],
                ref.successes[r.burner], static_cast<unsigned>(b.max_gap_ms[d.burner]),
                static_cast<unsigned>(ref.max_gap_ms[r.burner]));
  }
}

// --- v0.3 : écritures des consignes -----------------------------------------
//
// Chaudière SIMULÉE : ces tests éprouvent la logique de VissLink face au
// comportement documenté (accusé 00) et à des comportements supposés
// (refus, accusé perdu, valeur ignorée ou bornée). Ils ne valident rien sur
// la Vitotronic réelle.

namespace {

/// Exemple v0.3 : températures, compteurs, brûleur, 5 consignes en `number`
/// (5 min), 4 programmations. Écritures activées sauf mention contraire.
struct V03 {
  size_t outdoor, burner, a1_comfort, a1_reduced, m2_comfort, m2_reduced, dhw;
  std::unique_ptr<Circuits> circuits;

  explicit V03(Bench &b, bool schedules = true, bool writes = true) {
    b.enable_writes = writes;
    auto &r = b.boiler.registers;
    r[0x0842] = {0x01};
    r[0x2306] = {20};
    r[0x2307] = {16};
    r[0x3306] = {21};
    r[0x3307] = {17};
    r[0x6300] = {50};
    b.boiler.writable = {0x2306, 0x2307, 0x3306, 0x3307, 0x6300};
    outdoor = b.add(0x0800, 60 * S, 0);
    b.add(0x0802, 60 * S, 0);
    b.add(0x0804, 60 * S, 0);
    b.add(0x3900, 60 * S, 0);
    b.add(0x088A, H, 0, DataType::UINT32, 1);
    b.add(0x08A7, H, 0, DataType::UINT32, 1);
    burner = b.add_binary(0x0842, 30 * S, 0);
    a1_comfort = b.add_number(0x2306, 16, 24, 5 * MIN, 0);
    a1_reduced = b.add_number(0x2307, 10, 20, 5 * MIN, 0);
    m2_comfort = b.add_number(0x3306, 16, 24, 5 * MIN, 0);
    m2_reduced = b.add_number(0x3307, 10, 20, 5 * MIN, 0);
    dhw = b.add_number(0x6300, 40, 60, 5 * MIN, 0);
    if (schedules)
      circuits = std::make_unique<Circuits>(b, 0);
  }
  std::vector<size_t> numbers() const { return {a1_comfort, a1_reduced, m2_comfort, m2_reduced, dhw}; }
};

/// Démarrage : toutes les consignes lues au moins une fois.
constexpr uint32_t BOOT = 2 * MIN;

}  // namespace

TEST(sim_v03_write_confirmed_by_readback) {
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  CHECK(b.has_read_value(v.a1_comfort));
  CHECK_EQ(b.number_state[v.a1_comfort], 20);

  CHECK(b.request(v.a1_comfort, 21, BOOT) == "queued");
  CHECK_EQ(b.number_state[v.a1_comfort], 20);  // rien n'est publié avant la relecture
  b.run(BOOT, 20 * S);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK(b.boiler.write_log[0] == (std::vector<uint8_t>{0x01, 0xF4, 0x23, 0x06, 0x01, 0x15}));
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SUCCESS});
  CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::CONFIRMED});
  CHECK_EQ(b.boiler_value(0x2306), 21);
  CHECK_EQ(b.number_state[v.a1_comfort], 21);  // valeur LUE
  CHECK(!b.writes.active());
  CHECK(b.failures.empty());
}

TEST(sim_v03_readback_is_the_next_transaction) {
  // Même sous charge (tous les registres échus au démarrage), la lecture qui
  // suit l'écriture est la relecture du registre écrit.
  for (uint32_t post : {2000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    V03 v(b);
    b.run(0, BOOT);
    for (uint32_t t = BOOT; t < BOOT + 10 * MIN; t += 37 * S) {
      CHECK(b.request(v.dhw, (t / (37 * S)) % 2 ? 55 : 45, t) != "full");
      b.run(t, 37 * S);
    }
    const auto &log = b.boiler.frame_log;
    int writes = 0;
    for (size_t i = 0; i + 1 < log.size(); i++) {
      if (log[i].first == 0xF4) {
        writes++;
        CHECK(log[i + 1] == (std::pair<uint8_t, uint16_t>{0xF7, 0x6300}));
      }
    }
    CHECK(writes >= 10);
    CHECK(b.failures.empty());
  }
}

TEST(sim_v03_disabled_never_writes) {
  // Écritures désactivées (défaut) : aucune trame F4, quelles que soient
  // les demandes ; l'état reste la valeur lue.
  Bench b;
  V03 v(b, true, false);
  b.run(0, BOOT);
  int refused = 0;
  for (uint32_t t = BOOT; t < H; t += 5 * S) {
    for (size_t n : v.numbers())
      refused += b.request(n, b.regs[n].min + 1, t) == "disabled";
    b.run(t, 5 * S);
  }
  CHECK(refused > 3000);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.boiler_value(0x2306), 20);
  CHECK_EQ(b.number_state[v.a1_comfort], 20);
  CHECK(b.write_results.empty());
}

TEST(sim_v03_no_write_without_request) {
  // Écritures activées, aucune demande : jamais d'écriture, ni au démarrage,
  // ni après une coupure de la liaison, ni après un redémarrage.
  Bench b;
  V03 v(b);
  b.run(0, 30 * MIN);
  b.boiler.connected = false;
  b.run(30 * MIN, 5 * MIN);
  b.boiler.connected = true;
  b.run(35 * MIN, 30 * MIN);
  b.reboot(65 * MIN);
  b.run(65 * MIN, 6 * H);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK(b.writes.pending_count() == 0 && !b.writes.active());
}

TEST(sim_v03_rejected_write_is_not_retried) {
  for (auto fault : {SimBoiler::WriteFault::NACK_SYNC, SimBoiler::WriteFault::NACK_OTHER}) {
    Bench b;
    V03 v(b, false);
    b.boiler.write_faults[0x3306] = fault;
    b.run(0, BOOT);
    CHECK(b.request(v.m2_comfort, 23, BOOT) == "queued");
    b.run(BOOT, 10 * MIN);
    CHECK_EQ(b.boiler.write_frames, 1);  // aucune nouvelle tentative
    CHECK(b.write_results == std::vector<KwResult>{KwResult::WRITE_REJECTED});
    CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::MISMATCH});
    CHECK_EQ(b.number_state[v.m2_comfort], 21);  // la valeur lue, pas la demandée
    CHECK(b.successes[v.burner] >= 20);         // les lectures continuent
    CHECK(b.failures.empty());
  }
}

TEST(sim_v03_ack_without_effect_is_detected) {
  // Accusé 00 mais valeur non appliquée, ou bornée par la régulation : seule
  // la relecture le révèle.
  Bench b;
  V03 v(b, false);
  b.boiler.write_faults[0x2307] = SimBoiler::WriteFault::ACK_IGNORED;
  b.boiler.write_clamp[0x6300] = {10, 55};
  b.run(0, BOOT);
  CHECK(b.request(v.a1_reduced, 18, BOOT) == "queued");
  b.run(BOOT, MIN);
  CHECK(b.request(v.dhw, 60, BOOT + MIN) == "queued");
  b.run(BOOT + MIN, MIN);
  CHECK((b.write_results == std::vector<KwResult>{KwResult::SUCCESS, KwResult::SUCCESS}));
  CHECK((b.verify_results == std::vector<VerifyResult>{VerifyResult::MISMATCH, VerifyResult::MISMATCH}));
  CHECK_EQ(b.number_state[v.a1_reduced], 16);
  CHECK_EQ(b.number_state[v.dhw], 55);
  CHECK_EQ(b.boiler.write_frames, 2);
}

TEST(sim_v03_lost_or_bad_reply_outcome_from_readback) {
  struct Case {
    SimBoiler::WriteFault fault;
    KwResult result;
    VerifyResult verify;
    int value;
  };
  for (const Case &c : {Case{SimBoiler::WriteFault::NO_REPLY_APPLIED, KwResult::RESPONSE_TIMEOUT,
                             VerifyResult::CONFIRMED, 22},
                        Case{SimBoiler::WriteFault::NO_REPLY_LOST, KwResult::RESPONSE_TIMEOUT, VerifyResult::MISMATCH,
                             20},
                        Case{SimBoiler::WriteFault::LONG_REPLY, KwResult::RESPONSE_TOO_LONG, VerifyResult::CONFIRMED,
                             22}}) {
    Bench b;
    V03 v(b, false);
    b.boiler.write_faults[0x2306] = c.fault;
    b.run(0, BOOT);
    CHECK(b.request(v.a1_comfort, 22, BOOT) == "queued");
    b.run(BOOT, 10 * MIN);
    CHECK_EQ(b.boiler.write_frames, 1);  // jamais répétée, même sans accusé
    CHECK(b.write_results == std::vector<KwResult>{c.result});
    CHECK(b.verify_results == std::vector<VerifyResult>{c.verify});
    CHECK_EQ(b.number_state[v.a1_comfort], c.value);
    CHECK(b.successes[v.burner] >= 20);
  }
}

TEST(sim_v03_rapid_changes_on_one_entity_are_coalesced) {
  // 20 -> 21 -> 22 -> 23 -> 24 en moins d'une seconde : au plus deux
  // écritures (la première, déjà partie, et la dernière), valeur finale 24.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  uint32_t t = BOOT;
  for (int value = 21; value <= 24; value++, t += 200) {
    const std::string r = b.request(v.a1_comfort, value, t);
    CHECK(r == "queued" || r == "replaced");
    b.run(t, 200);
  }
  b.run(t, 2 * MIN);
  CHECK(b.boiler.write_frames >= 1 && b.boiler.write_frames <= 2);
  for (const auto &frame : b.boiler.write_log)
    CHECK(frame[5] == 21 || frame[5] == 24);
  CHECK_EQ(b.boiler_value(0x2306), 24);
  CHECK_EQ(b.number_state[v.a1_comfort], 24);
  CHECK(b.verify_results.back() == VerifyResult::CONFIRMED);
}

TEST(sim_v03_several_entities_serialized_and_spaced) {
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  const int target[] = {22, 18, 23, 19, 55};
  const auto numbers = v.numbers();
  for (size_t i = 0; i < numbers.size(); i++)
    CHECK(b.request(numbers[i], target[i], BOOT) == "queued");
  b.run(BOOT, 3 * MIN);
  CHECK_EQ(b.boiler.write_frames, 5);
  CHECK_EQ(b.verify_results.size(), 5u);
  for (VerifyResult r : b.verify_results)
    CHECK(r == VerifyResult::CONFIRMED);
  for (size_t i = 0; i < numbers.size(); i++)
    CHECK_EQ(b.number_state[numbers[i]], target[i]);
  for (size_t i = 1; i < b.write_start_ms.size(); i++)
    CHECK(b.write_start_ms[i] - b.write_start_ms[i - 1] >= WriteArbiter::MIN_SPACING_MS);
  // Une seule écriture à la fois : chaque écriture est suivie de sa relecture.
  const auto &log = b.boiler.frame_log;
  for (size_t i = 0; i + 1 < log.size(); i++) {
    if (log[i].first == 0xF4)
      CHECK(log[i + 1].first == 0xF7 && log[i + 1].second == log[i].second);
  }
  CHECK(b.failures.empty());
}

TEST(sim_v03_write_spam_keeps_periodic_reads) {
  // Pire cas : une demande toutes les 2 s, en alternance sur les 5 consignes,
  // pendant 6 h 30 (programmations comprises). Référence : mêmes registres,
  // aucune demande. Les écritures sont plafonnées par l'espacement minimal ;
  // le brûleur et les températures restent lus.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    V03 r(ref);
    V03 v(b);
    ref.run(0, BOOT);
    b.run(0, BOOT);
    const auto numbers = v.numbers();
    const uint32_t end = 6 * H + 30 * MIN;
    int k = 0;
    for (uint32_t t = BOOT; t < end; t += 2 * S, k++) {
      const size_t n = numbers[k % numbers.size()];
      b.request(n, b.regs[n].min + (k / 5) % 3, t);
      ref.run(t, 2 * S);
      b.run(t, 2 * S);
    }
    CHECK(b.failures.empty());
    CHECK(b.boiler.write_frames > 300);
    CHECK(b.boiler.write_frames <= static_cast<int>((end - BOOT) / WriteArbiter::MIN_SPACING_MS) + 1);
    for (size_t i = 0; i + 1 < b.boiler.frame_log.size(); i++) {
      if (b.boiler.frame_log[i].first == 0xF4)
        CHECK(b.boiler.frame_log[i + 1].first == 0xF7);  // jamais deux écritures de suite
    }
    for (size_t i : {v.circuits->a1, v.circuits->m2, v.circuits->dhw, v.circuits->loop})
      CHECK_EQ(b.successes[i], 14);
    // Relevé (v0.4.1, commandes prioritaires) : 2 s -> 0 lecture du brûleur
    // perdue sur 780 ; 3 s -> 1 ; 4 s -> 42 (écart 85 s ; v0.4 : 18). Ce
    // rythme (une écriture toutes les ~15 s pendant 6 h) n'a rien de
    // réaliste : il borne le pire cas.
    const int lost = ref.successes[r.burner] - b.successes[v.burner];
    if (post <= 3000) {
      CHECK(lost <= 1);
      CHECK(b.max_gap_ms[v.burner] <= 55 * S);
    } else {
      CHECK(lost <= 50);
      CHECK(b.max_gap_ms[v.burner] <= 90 * S);  // jamais plus de deux cycles sautés
    }
    CHECK(b.max_gap_ms[v.outdoor] <= 110 * S);
    if (vltest::failures() > 0)
      std::printf("    post=%u writes=%d burner %d/%d gap %u ms (ref %u) outdoor gap %u ms\n",
                  static_cast<unsigned>(post), b.boiler.write_frames, b.successes[v.burner], ref.successes[r.burner],
                  static_cast<unsigned>(b.max_gap_ms[v.burner]), static_cast<unsigned>(ref.max_gap_ms[r.burner]),
                  static_cast<unsigned>(b.max_gap_ms[v.outdoor]));
  }
}

TEST(sim_v03_link_loss_never_sends_a_stale_write) {
  // Chemin « demande périmée » : une demande attend l'espacement minimal qui
  // suit une écriture ; la liaison tombe pendant cette attente. La demande
  // n'est jamais envoyée, puis elle est abandonnée, même après le retour de
  // la liaison.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  CHECK(b.request(v.a1_comfort, 22, BOOT) == "queued");
  uint32_t t = BOOT;
  while (b.timings.empty() && t < BOOT + MIN)
    b.run(t, 5), t += 5;
  CHECK_EQ(b.boiler.write_frames, 1);  // première écriture terminée
  CHECK(b.request(v.dhw, 45, t) == "queued");  // en attente de l'espacement
  b.boiler.connected = false;
  b.run(t, 30 * S);
  CHECK(!b.link_ok);
  CHECK(b.request(v.a1_reduced, 18, t + 30 * S) == "link");  // refusée tout de suite
  b.run(t + 30 * S, 3 * MIN);
  CHECK_EQ(b.expired_writes, 1);  // abandonnée après REQUEST_EXPIRY_MS
  b.boiler.connected = true;
  b.run(t + 210 * S, 20 * MIN);
  CHECK_EQ(b.boiler.write_frames, 1);  // jamais envoyée plus tard
  CHECK_EQ(b.boiler_value(0x6300), 50);
  CHECK(b.link_ok);
  CHECK_EQ(b.number_state[v.dhw], 50);
}

TEST(sim_v041_link_loss_with_preempted_read_sends_nothing) {
  // v0.4.1 : la demande prend la place d'une lecture en attente du 0x05 ;
  // l'interface est débranchée : pas de 0x05, rien n'est émis
  // (sync timeout), aucune nouvelle tentative après le retour de la liaison.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  uint32_t t = BOOT;
  while (!b.kw.is_waiting_sync())
    b.run(t, 5), t += 5;
  b.boiler.connected = false;
  CHECK(b.request(v.dhw, 45, t) == "queued");
  b.run(t, 30 * S);
  CHECK_EQ(b.preemptions, 1);
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SYNC_TIMEOUT});
  CHECK(!b.link_ok);
  b.boiler.connected = true;
  b.run(t + 30 * S, 20 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.boiler_value(0x6300), 50);
  CHECK_EQ(b.number_state[v.dhw], 50);
}

TEST(sim_v03_link_lost_during_write_frame_not_sent) {
  // Écriture partie vers la ligne alors que l'interface est débranchée : pas
  // de 0x05, donc pas de trame (SYNC_TIMEOUT), relecture impossible puis
  // abandonnée ; aucune nouvelle tentative après le retour de la liaison.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  CHECK(b.request(v.m2_reduced, 18, BOOT) == "queued");
  b.boiler.connected = false;
  b.run(BOOT, 2 * MIN);
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SYNC_TIMEOUT});
  CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::UNVERIFIED});
  CHECK(std::isnan(b.number_state[v.m2_reduced]));  // inconnu, pas « 18 »
  b.boiler.connected = true;
  b.run(BOOT + 2 * MIN, 10 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.number_state[v.m2_reduced], 17);
  CHECK(!b.writes.active());
}

TEST(sim_v03_reboot_during_write) {
  // L'ESP redémarre pendant l'émission de la trame d'écriture : la
  // régulation ne reçoit que 01 F4 23 et l'abandonne. Au redémarrage, rien
  // n'est rejoué : aucune écriture, la consigne lue est l'ancienne.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  CHECK(b.request(v.a1_comfort, 23, BOOT) == "queued");
  uint32_t t = BOOT;
  for (;; t += 5) {
    b.boiler.tick(t);
    b.step(t);
    if (b.write_in_flight && !b.serial.tx.empty() && b.serial.tx.size() >= 6 &&
        b.serial.tx[b.serial.tx.size() - 5] == 0xF4)
      break;
    CHECK(t < BOOT + MIN);
    if (t >= BOOT + MIN)
      return;
  }
  b.serial.tx.resize(b.serial.tx.size() - 3);  // 01 F4 23 seulement
  b.reboot(t);
  b.boiler_only(t, 3 * S);  // démarrage de l'ESP
  b.run(t + 3 * S, 15 * MIN);
  CHECK_EQ(b.boiler.discarded_partial_frames, 1);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.boiler_value(0x2306), 20);
  CHECK_EQ(b.number_state[v.a1_comfort], 20);
  CHECK(b.write_results.empty());
  CHECK(b.successes[v.burner] >= 25);
  CHECK(b.failures.empty());
}

TEST(sim_v03_invalid_requests_never_reach_the_line) {
  Bench b;
  V03 v(b, false);
  CHECK(b.request(v.a1_comfort, 21, 0) == "not read");  // valeur actuelle inconnue
  b.run(0, BOOT);
  CHECK(b.request(v.a1_comfort, 25, BOOT) == "value outside the configured range");
  CHECK(b.request(v.a1_comfort, 15, BOOT) == "value outside the configured range");
  CHECK(b.request(v.a1_comfort, 20.5f, BOOT) == "value is not an integer");
  CHECK(b.request(v.a1_comfort, NAN, BOOT) == "value is not a finite number");
  CHECK(b.request(v.dhw, 61, BOOT) == "value outside the configured range");
  CHECK(b.request(v.a1_comfort, 20, BOOT) == "unchanged");  // déjà la valeur lue
  // Un registre lu mais hors liste blanche ne peut pas être écrit, même
  // déclaré par erreur comme consigne.
  const size_t rogue = b.add_number(0x0842, 0, 1, 5 * MIN, BOOT);
  b.run(BOOT, MIN);
  CHECK(b.request(rogue, 0, BOOT + MIN) == "address not in the write whitelist");
  b.run(BOOT + MIN, 5 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);
}

TEST(sim_v03_send_time_check_blocks_corrupted_requests) {
  // Défense en profondeur : une demande qui aurait échappé au contrôle de
  // dépôt (déposée directement dans l'arbitre) est refusée juste avant
  // émission : hors bornes configurées, hors plage documentée, adresse
  // différente de celle de l'entité, registre qui n'est pas une consigne.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  struct Bad {
    size_t owner;
    uint16_t address;
    uint8_t value;
  };
  const Bad bad[] = {
      {v.a1_comfort, 0x2306, 25},  // > max_value 24
      {v.a1_comfort, 0x2306, 15},  // < min_value 16
      {v.dhw, 0x6300, 61},         // > plage documentée et configurée
      {v.a1_comfort, 0x2307, 18},  // adresse d'une autre entité
      {v.burner, 0x0842, 0},       // registre lu, hors liste blanche
      {v.outdoor, 0x2306, 20},     // capteur, même avec une adresse autorisée
  };
  uint32_t t = BOOT;
  for (const Bad &x : bad) {
    b.writes.submit(static_cast<int>(x.owner), x.address, x.value, t);
    b.run(t, WriteArbiter::MIN_SPACING_MS + 5 * S);
    t += WriteArbiter::MIN_SPACING_MS + 5 * S;
  }
  CHECK_EQ(b.refused_at_send, 6);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK(b.write_results.empty());
}

TEST(sim_v03_lost_ack_is_indeterminate_until_readback) {
  // Accusé perdu : rien n'est publié comme appliqué avant la relecture ;
  // si la relecture échoue aussi, l'état devient inconnu (pas la valeur
  // demandée) ; la trame n'est jamais réémise.
  Bench b;
  V03 v(b, false);
  b.boiler.write_faults[0x2306] = SimBoiler::WriteFault::NO_REPLY_APPLIED;
  b.run(0, BOOT);
  CHECK(b.request(v.a1_comfort, 22, BOOT) == "queued");
  uint32_t t = BOOT;
  while (b.write_results.empty() && t < BOOT + MIN) {
    b.boiler.tick(t);
    b.step(t);
    CHECK_EQ(b.number_state[v.a1_comfort], 20);  // valeur lue, jamais 22 avant la relecture
    t += 5;
  }
  CHECK(b.write_results == std::vector<KwResult>{KwResult::RESPONSE_TIMEOUT});
  CHECK_EQ(b.number_state[v.a1_comfort], 20);  // indéterminé : toujours la dernière valeur lue
  b.boiler.connected = false;                  // la relecture échoue aussi
  b.run(t, 2 * MIN);
  CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::UNVERIFIED});
  CHECK(std::isnan(b.number_state[v.a1_comfort]));  // inconnu, pas 22
  b.boiler.connected = true;
  b.run(t + 2 * MIN, 10 * MIN);
  CHECK_EQ(b.boiler.write_frames, 1);              // jamais réémise
  CHECK_EQ(b.number_state[v.a1_comfort], 22);      // valeur réelle, apprise par lecture
  CHECK_EQ(b.boiler.write_frames_by_address[0x2306], 1);
}

// --- v0.3 : programmes de fonctionnement (expérimental) ----------------------

namespace {

/// Consignes (sans programmations horaires) + programmes A1 et M2, lus à
/// 03 (Chauffage + ECS). Chaudière simulée : 0x2301 et 0x3301 acceptent
/// l'écriture, pour qu'une trame M2 non autorisée se voie (registre modifié).
struct Modes {
  V03 base;
  size_t a1, m2;
  Modes(Bench &b, bool mode_writes = true, bool m2_write = false, bool schedules = false) : base(b, schedules) {
    b.enable_mode_writes = mode_writes;
    b.enable_m2_mode_write = m2_write;
    b.boiler.registers[0x2301] = {0x03};
    b.boiler.registers[0x3301] = {0x03};
    b.boiler.writable.insert(0x2301);
    b.boiler.writable.insert(0x3301);
    a1 = b.add_mode(0x2301, 5 * MIN, 0);
    m2 = b.add_mode(0x3301, 5 * MIN, 0);
  }
};

constexpr size_t ECS_ONLY = 0, HEATING_ECS = 1, OFF = 2;  // options du sélecteur

/// Les deux lectures qui suivent chaque écriture : les deux programmes.
bool both_read_back_after_each_write(const SimBoiler &boiler) {
  const auto &log = boiler.frame_log;
  bool ok = true;
  for (size_t i = 0; i < log.size(); i++) {
    if (log[i].first != 0xF4)
      continue;
    if (i + 2 >= log.size())
      return false;
    std::set<uint16_t> next = {log[i + 1].second, log[i + 2].second};
    ok &= log[i + 1].first == 0xF7 && log[i + 2].first == 0xF7 && next == std::set<uint16_t>{0x2301, 0x3301};
  }
  return ok;
}

}  // namespace

TEST(sim_v03_mode_values_read_strictly) {
  for (auto raw_expected : std::vector<std::pair<uint8_t, int>>{{0x00, 0x00}, {0x03, 0x03}, {0x05, 0x05},
                                                                {0x01, -1}, {0x02, -1}, {0x04, -1}, {0xFF, -1}}) {
    Bench b;
    Modes m(b);
    b.boiler.registers[0x2301] = {raw_expected.first};
    b.run(0, BOOT);
    CHECK_EQ(b.mode_of(m.a1), raw_expected.second);  // valeur hors 00/03/05 : inconnue
    CHECK_EQ(b.mode_of(m.m2), 0x03);
  }
}

TEST(sim_v03_mode_writes_disabled_by_default) {
  for (int config = 0; config < 2; config++) {
    Bench b;
    Modes m(b, config == 1);  // 0 : rien d'activé ; 1 : enable_mode_writes sans enable_writes
    b.enable_writes = false;
    b.run(0, BOOT);
    for (size_t option : {ECS_ONLY, OFF, HEATING_ECS})
      CHECK(b.request_mode(m.a1, option, BOOT) == "disabled");
    b.enable_writes = true;  // consignes autorisées, programmes non
    b.enable_mode_writes = false;
    CHECK(b.request_mode(m.a1, OFF, BOOT) == "mode disabled");
    b.run(BOOT, 30 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK_EQ(b.mode_of(m.a1), 0x03);
  }
}

TEST(sim_v03_mode_each_value_written_then_both_read_back) {
  // 03 -> 00 -> 05 -> 03 sur A1, une commande à la fois. Après chaque
  // commande : relecture de A1 ET de M2, avant toute autre lecture.
  Bench b;
  Modes m(b);
  b.run(0, BOOT);
  uint32_t t = BOOT;
  const std::pair<size_t, uint8_t> steps[] = {{ECS_ONLY, 0x00}, {OFF, 0x05}, {HEATING_ECS, 0x03}};
  for (const auto &step : steps) {
    CHECK(b.request_mode(m.a1, step.first, t) == "queued");
    b.run(t, MIN);
    t += MIN;
    CHECK_EQ(b.boiler_value(0x2301), step.second);
    CHECK_EQ(b.mode_of(m.a1), step.second);  // publiée après relecture seulement
    CHECK(b.verify_results.back() == VerifyResult::CONFIRMED);
  }
  CHECK_EQ(b.boiler.write_frames, 3);
  CHECK(b.boiler.write_log[0] == (std::vector<uint8_t>{0x01, 0xF4, 0x23, 0x01, 0x01, 0x00}));
  CHECK(b.boiler.write_log[1] == (std::vector<uint8_t>{0x01, 0xF4, 0x23, 0x01, 0x01, 0x05}));
  CHECK(b.boiler.write_log[2] == (std::vector<uint8_t>{0x01, 0xF4, 0x23, 0x01, 0x01, 0x03}));
  CHECK(both_read_back_after_each_write(b.boiler));
  CHECK_EQ(b.boiler.write_frames_by_address[0x3301], 0);  // M2 jamais écrit
  CHECK(b.failures.empty());
}

TEST(sim_v03_mode_divergence_shown_never_corrected) {
  // A1 passe à Arrêt, M2 reste à Chauffage + ECS : divergence signalée
  // après les deux relectures, et aucune écriture sur M2, même 1 h plus tard.
  Bench b;
  Modes m(b);
  b.run(0, BOOT);
  CHECK(b.divergence() == ModeDivergence::SAME);
  CHECK(b.request_mode(m.a1, OFF, BOOT) == "queued");
  b.run(BOOT, H);
  CHECK(b.divergence_after_write == std::vector<ModeDivergence>{ModeDivergence::DIFFERENT});
  CHECK(b.divergence() == ModeDivergence::DIFFERENT);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK_EQ(b.boiler.write_frames_by_address[0x3301], 0);
  CHECK_EQ(b.mode_of(m.m2), 0x03);
  // Retour volontaire de A1 : plus de divergence.
  CHECK(b.request_mode(m.a1, HEATING_ECS, BOOT + H) == "queued");
  b.run(BOOT + H, MIN);
  CHECK(b.divergence() == ModeDivergence::SAME);
  CHECK(b.divergence_after_write.back() == ModeDivergence::SAME);
}

TEST(sim_v03_mode_divergence_when_regulation_couples_circuits) {
  // Si la régulation recopie elle-même le programme sur M2, la double
  // relecture le montre (pas de divergence) sans que VissLink écrive M2.
  Bench b;
  Modes m(b);
  b.run(0, BOOT);
  CHECK(b.request_mode(m.a1, ECS_ONLY, BOOT) == "queued");
  for (uint32_t t = BOOT; t < BOOT + MIN; t += 5) {
    b.boiler.tick(t);
    if (b.boiler.registers[0x2301][0] == 0x00)
      b.boiler.registers[0x3301] = {0x00};  // couplage supposé, simulé
    b.step(t);
  }
  CHECK(b.divergence_after_write == std::vector<ModeDivergence>{ModeDivergence::SAME});
  CHECK_EQ(b.mode_of(m.m2), 0x00);
  CHECK_EQ(b.boiler.write_frames_by_address[0x3301], 0);
}

TEST(sim_v03_mode_positive_ack_is_not_proof) {
  Bench b;
  Modes m(b);
  b.boiler.write_faults[0x2301] = SimBoiler::WriteFault::ACK_IGNORED;
  b.run(0, BOOT);
  CHECK(b.request_mode(m.a1, OFF, BOOT) == "queued");
  b.run(BOOT, 10 * MIN);
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SUCCESS});        // accusé 00…
  CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::MISMATCH});  // …mais rien n'a changé
  CHECK_EQ(b.mode_of(m.a1), 0x03);
  CHECK_EQ(b.boiler.write_frames, 1);
}

TEST(sim_v03_mode_timeout_never_resent) {
  for (auto fault : {SimBoiler::WriteFault::NO_REPLY_LOST, SimBoiler::WriteFault::NO_REPLY_APPLIED,
                     SimBoiler::WriteFault::NACK_SYNC}) {
    Bench b;
    Modes m(b);
    b.boiler.write_faults[0x2301] = fault;
    b.run(0, BOOT);
    CHECK(b.request_mode(m.a1, ECS_ONLY, BOOT) == "queued");
    b.run(BOOT, 30 * MIN);
    CHECK_EQ(b.boiler.write_frames, 1);
    CHECK_EQ(b.mode_of(m.a1), fault == SimBoiler::WriteFault::NO_REPLY_APPLIED ? 0x00 : 0x03);
    CHECK(both_read_back_after_each_write(b.boiler));
  }
}

TEST(sim_v04_mode_values_refused_and_m2_needs_third_permission) {
  Bench b;
  Modes m(b);  // enable_writes + enable_mode_writes, sans enable_m2_mode_write
  CHECK(b.request_mode(m.a1, OFF, 0) == "not read");
  b.run(0, BOOT);
  for (size_t option : {ECS_ONLY, HEATING_ECS, OFF})
    CHECK(b.request_mode(m.m2, option, BOOT) == "m2 disabled");  // 0x3301 : troisième autorisation
  for (size_t option : {size_t(3), size_t(4), size_t(255)}) {
    CHECK(b.request_mode(m.a1, option, BOOT) == "operating mode value not allowed (only 00, 03, 05)");
    CHECK(b.request_mode(m.m2, option, BOOT) == "m2 disabled");  // l'autorisation passe avant tout
  }
  CHECK(b.request_mode(m.a1, HEATING_ECS, BOOT) == "unchanged");
  b.run(BOOT, 10 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);

  Bench c;
  Modes n(c, true, true);  // trois autorisations
  c.run(0, BOOT);
  for (size_t option : {size_t(3), size_t(4), size_t(255)})
    CHECK(c.request_mode(n.m2, option, BOOT) == "operating mode value not allowed (only 00, 03, 05)");
  CHECK(c.request_mode(n.m2, HEATING_ECS, BOOT) == "unchanged");
  c.run(BOOT, 10 * MIN);
  CHECK_EQ(c.boiler.write_frames, 0);
}

TEST(sim_v03_mode_one_command_at_a_time) {
  Bench b;
  Modes m(b);
  b.run(0, BOOT);
  CHECK(b.request_mode(m.a1, OFF, BOOT) == "queued");
  CHECK(b.request_mode(m.a1, ECS_ONLY, BOOT) == "busy");  // pas de remplacement ni d'enchaînement
  b.run(BOOT, 2 * S);
  CHECK(b.request_mode(m.a1, ECS_ONLY, BOOT + 2 * S) == "busy");
  b.run(BOOT + 2 * S, MIN);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK_EQ(b.mode_of(m.a1), 0x05);
  CHECK(b.request_mode(m.a1, HEATING_ECS, BOOT + MIN) == "queued");  // après les deux relectures
  b.run(BOOT + MIN, MIN);
  CHECK_EQ(b.boiler.write_frames, 2);
  CHECK(b.write_start_ms[1] - b.write_start_ms[0] >= WriteArbiter::MIN_SPACING_MS);
}

TEST(sim_v03_mode_no_write_without_request_reboot_or_reconnect) {
  Bench b;
  Modes m(b);
  b.run(0, 20 * MIN);
  b.boiler.connected = false;
  b.run(20 * MIN, 3 * MIN);
  b.boiler.connected = true;
  b.run(23 * MIN, 10 * MIN);
  b.reboot(33 * MIN);
  b.run(33 * MIN, H);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.mode_of(m.a1), 0x03);
}

// --- v0.4 : programme de M2 (0x3301), troisième autorisation -----------------

TEST(sim_v04_m2_write_with_three_permissions) {
  // M2 03 -> 00 puis retour : relecture de M2 ET de A1 après chaque
  // commande, divergence affichée, A1 jamais écrit.
  Bench b;
  Modes m(b, true, true);
  b.run(0, BOOT);
  CHECK(b.request_mode(m.m2, ECS_ONLY, BOOT) == "queued");
  CHECK_EQ(b.mode_of(m.m2), 0x03);  // rien n'est publié avant la relecture
  b.run(BOOT, MIN);
  CHECK(b.boiler.write_log.back() == (std::vector<uint8_t>{0x01, 0xF4, 0x33, 0x01, 0x01, 0x00}));
  CHECK_EQ(b.mode_of(m.m2), 0x00);
  CHECK_EQ(b.mode_of(m.a1), 0x03);
  CHECK(b.verify_results.back() == VerifyResult::CONFIRMED);
  CHECK(b.divergence_after_write == std::vector<ModeDivergence>{ModeDivergence::DIFFERENT});
  CHECK(b.request_mode(m.m2, HEATING_ECS, BOOT + MIN) == "queued");
  b.run(BOOT + MIN, MIN);
  CHECK(b.divergence_after_write.back() == ModeDivergence::SAME);
  CHECK(both_read_back_after_each_write(b.boiler));
  CHECK_EQ(b.boiler.write_frames_by_address[0x2301], 0);
  CHECK_EQ(b.boiler.write_frames, 2);
  CHECK(b.failures.empty());
}

TEST(sim_v04_m2_permission_matrix) {
  // Les 8 combinaisons des trois options : M2 n'est écrit que si les trois
  // sont vraies ; A1 si les deux premières le sont.
  for (int mask = 0; mask < 8; mask++) {
    const bool writes = mask & 1, modes = mask & 2, m2 = mask & 4;
    Bench b;
    Modes m(b, modes, m2);
    b.enable_writes = writes;
    b.run(0, BOOT);
    b.request_mode(m.m2, OFF, BOOT);
    b.run(BOOT, MIN);
    b.request_mode(m.a1, ECS_ONLY, BOOT + MIN);
    b.run(BOOT + MIN, MIN);
    CHECK_EQ(b.boiler.write_frames_by_address[0x3301], writes && modes && m2 ? 1 : 0);
    CHECK_EQ(b.boiler.write_frames_by_address[0x2301], writes && modes ? 1 : 0);
    CHECK_EQ(b.boiler_value(0x3301), writes && modes && m2 ? 0x05 : 0x03);
  }
}

TEST(sim_v04_m2_never_written_without_third_permission_after_reboot_reconnect) {
  Bench b;
  Modes m(b);  // enable_m2_mode_write: false
  b.run(0, BOOT);
  uint32_t t = BOOT;
  for (int cycle = 0; cycle < 3; cycle++) {
    CHECK(b.request_mode(m.m2, cycle == 1 ? OFF : ECS_ONLY, t) == "m2 disabled");
    b.boiler.connected = false;  // coupure de la liaison
    b.run(t, 2 * MIN);
    b.boiler.connected = true;  // reconnexion
    b.run(t + 2 * MIN, 5 * MIN);
    CHECK(b.request_mode(m.m2, OFF, t + 7 * MIN) == "m2 disabled");
    b.reboot(t + 7 * MIN);  // redémarrage de l'ESP
    b.boiler_only(t + 7 * MIN, 3 * S);
    b.run(t + 7 * MIN + 3 * S, 5 * MIN);
    CHECK(b.request_mode(m.m2, OFF, t + 12 * MIN + 3 * S) == "m2 disabled");
    t += 13 * MIN;
    b.run(t - 57 * S, 57 * S);
  }
  b.run(t, H);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.boiler_value(0x3301), 0x03);
  CHECK_EQ(b.mode_of(m.m2), 0x03);
}

TEST(sim_v04_m2_never_written_after_a1_timeout_or_concurrently) {
  for (auto fault : {SimBoiler::WriteFault::NO_REPLY_LOST, SimBoiler::WriteFault::NO_REPLY_APPLIED,
                     SimBoiler::WriteFault::NACK_SYNC}) {
    Bench b;
    Modes m(b);  // enable_m2_mode_write: false
    b.boiler.write_faults[0x2301] = fault;
    b.run(0, BOOT);
    CHECK(b.request_mode(m.a1, OFF, BOOT) == "queued");
    CHECK(b.request_mode(m.m2, OFF, BOOT) == "m2 disabled");  // concurrente : refusée d'abord par l'autorisation
    for (uint32_t t = BOOT; t < BOOT + MIN; t += 1 * S) {
      CHECK(b.request_mode(m.m2, ECS_ONLY, t) == "m2 disabled");  // pendant l'écriture A1 et ses relectures
      b.run(t, 1 * S);
    }
    b.run(BOOT + MIN, 30 * MIN);
    CHECK_EQ(b.boiler.write_frames, 1);  // la seule écriture A1, jamais réémise
    CHECK_EQ(b.boiler.write_frames_by_address[0x3301], 0);
    CHECK_EQ(b.boiler_value(0x3301), 0x03);
  }
  // Avec la troisième autorisation : une commande M2 pendant celle de A1 est
  // refusée (une écriture de programme à la fois), pas mise en file.
  Bench c;
  Modes n(c, true, true);
  c.run(0, BOOT);
  CHECK(c.request_mode(n.a1, OFF, BOOT) == "queued");
  CHECK(c.request_mode(n.m2, OFF, BOOT) == "busy");
  c.run(BOOT, 5 * S);
  CHECK(c.request_mode(n.m2, OFF, BOOT + 5 * S) == "busy");
  c.run(BOOT + 5 * S, MIN);
  CHECK_EQ(c.boiler.write_frames, 1);
  CHECK_EQ(c.boiler.write_frames_by_address[0x3301], 0);
}

TEST(sim_v04_m2_send_time_check_blocks_bypassed_requests) {
  // Défense en profondeur : une demande M2 déposée directement dans
  // l'arbitre (sans passer par le contrôle de dépôt) est refusée juste avant
  // l'émission tant que enable_m2_mode_write est faux.
  Bench b;
  Modes m(b);
  b.run(0, BOOT);
  b.writes.submit(static_cast<int>(m.m2), 0x3301, 0x05, BOOT);
  b.run(BOOT, MIN);
  CHECK_EQ(b.refused_at_send, 1);
  CHECK_EQ(b.boiler.write_frames, 0);
  // Autorisée au dépôt puis retirée avant l'émission : refusée aussi.
  Bench c;
  Modes n(c, true, true);
  c.run(0, BOOT);
  CHECK(c.request_mode(n.a1, OFF, BOOT) == "queued");
  c.run(BOOT, MIN);
  c.writes.submit(static_cast<int>(n.m2), 0x3301, 0x05, BOOT + MIN);
  c.enable_m2_mode_write = false;
  c.run(BOOT + MIN, MIN);
  CHECK_EQ(c.refused_at_send, 1);
  CHECK_EQ(c.boiler.write_frames_by_address[0x3301], 0);
}

// --- v0.4.1 : réactivité des commandes manuelles ------------------------------

namespace {

struct Latency {
  uint32_t queue_max{0}, applied_max{0}, read_back_max{0};
  double applied_mean{0};
  size_t count{0};
};

Latency latency(const Bench &b) {
  Latency l;
  for (const auto &t : b.timings) {
    l.queue_max = std::max(l.queue_max, t.queue_ms());
    l.applied_max = std::max(l.applied_max, t.applied_ms());
    l.read_back_max = std::max(l.read_back_max, t.read_back_ms());
    l.applied_mean += t.applied_ms();
  }
  l.count = b.timings.size();
  if (l.count > 0)
    l.applied_mean /= l.count;
  return l;
}

/// Une commande A1 toutes les 47 s (aller-retour 03 <-> 05) pendant 6 h 20,
/// programmations horaires comprises (lecture complète au démarrage et à 6 h).
void run_a1_commands(Bench &b, Modes &m) {
  b.run(0, BOOT);
  for (uint32_t t = BOOT; t < 6 * H + 20 * MIN; t += 47 * S) {
    CHECK(b.request_mode(m.a1, b.mode_of(m.a1) == 0x03 ? OFF : HEATING_ECS, t) == "queued");
    b.run(t, 47 * S);
  }
}

}  // namespace

TEST(sim_v041_command_latency_under_load) {
  // Relevé (ms, moyenne / max de « demande -> état publié ») :
  //   2 s : v0.4 3282/5290  -> v0.4.1 3134/4155
  //   3 s : v0.4 4956/9115  -> v0.4.1 4311/6145
  //   4 s : v0.4 7354/12125 -> v0.4.1 5719/8135
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench old_b, b;
    old_b.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    old_b.preempt_unsent_reads = old_b.strict_read_back = false;  // comportement v0.4
    Modes old_m(old_b, true, false, true);
    Modes m(b, true, false, true);
    run_a1_commands(old_b, old_m);
    run_a1_commands(b, m);
    const Latency before = latency(old_b), after = latency(b);

    CHECK_EQ(after.count, 483u);  // toutes les commandes exécutées
    CHECK(b.preemptions > 0);
    // La ligne est obtenue dès la fin de l'éventuelle trame en cours.
    CHECK(after.queue_max <= 100);
    // Ensuite : un 0x05 pour l'écriture, un pour la relecture (au plus la
    // resynchro chacun), plus émission, réponse et fenêtres de silence.
    CHECK(after.applied_max <= 2 * post + 300);
    CHECK(after.read_back_max <= post + 100);  // aucune lecture intercalée
    CHECK(after.applied_max <= before.applied_max);
    CHECK(after.applied_mean <= before.applied_mean);
    // Sans pénaliser les lectures périodiques ni les programmations.
    CHECK(b.successes[m.base.burner] + 3 >= old_b.successes[old_m.base.burner]);
    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.requests, b.total_successes());  // aucune lecture abandonnée après émission
    CHECK(both_read_back_after_each_write(b.boiler));
    for (size_t i : {m.base.circuits->a1, m.base.circuits->m2, m.base.circuits->dhw, m.base.circuits->loop})
      CHECK_EQ(b.successes[i], 14);
    if (vltest::failures() > 0)
      std::printf("    post=%u applied mean %.0f/%.0f max %u/%u queue max %u/%u\n", static_cast<unsigned>(post),
                  before.applied_mean, after.applied_mean, static_cast<unsigned>(before.applied_max),
                  static_cast<unsigned>(after.applied_max), static_cast<unsigned>(before.queue_max),
                  static_cast<unsigned>(after.queue_max));
  }
}

TEST(sim_v041_command_during_schedule_reading) {
  // Pendant la lecture des 28 blocs de programmation (démarrage), une
  // commande passe devant tous les blocs restants.
  Bench b;
  b.boiler.post_response_delay_ms = 3000;
  Modes m(b, true, false, true);
  b.run(0, 75 * S);  // A1 et M2 lus, programmations en cours de lecture
  CHECK(b.mode_of(m.a1) == 0x03);
  CHECK(b.successes[m.base.circuits->a1] < 7);
  CHECK(b.request_mode(m.a1, ECS_ONLY, 75 * S) == "queued");
  b.run(75 * S, 15 * S);
  CHECK_EQ(b.timings.size(), 1u);
  CHECK(b.timings[0].queue_ms() <= 100);
  CHECK(b.timings[0].applied_ms() <= 2 * 3000 + 300);
  CHECK_EQ(b.mode_of(m.a1), 0x00);
  b.run(90 * S, 10 * MIN);
  for (size_t i : {m.base.circuits->a1, m.base.circuits->m2, m.base.circuits->dhw, m.base.circuits->loop})
    CHECK_EQ(b.successes[i], 7);  // programmations lues entièrement
}

TEST(sim_v041_written_register_read_back_first) {
  // Commande M2 : M2 (le registre écrit, index plus grand) est relu avant
  // A1 ; son état est publié une synchro plus tôt.
  Bench b;
  Modes m(b, true, true);
  b.run(0, BOOT);
  CHECK(b.request_mode(m.m2, ECS_ONLY, BOOT) == "queued");
  b.run(BOOT, MIN);
  const auto &log = b.boiler.frame_log;
  size_t w = 0;
  while (w < log.size() && log[w].first != 0xF4)
    w++;
  CHECK(w + 2 < log.size());
  CHECK(log[w + 1] == (std::pair<uint8_t, uint16_t>{0xF7, 0x3301}));
  CHECK(log[w + 2] == (std::pair<uint8_t, uint16_t>{0xF7, 0x2301}));
  CHECK(b.timings[0].applied_ms() < b.timings[0].total_ms());
}

TEST(sim_v041_no_ordinary_read_inside_read_back_retries) {
  // Écriture d'une consigne (une seule relecture) pendant la lecture des
  // programmations ; la relecture échoue une fois. v0.4.1 : pendant le délai
  // de nouvelle tentative, aucune lecture ordinaire n'est intercalée ;
  // v0.4 : une lecture ordinaire passait.
  for (bool strict : {false, true}) {
    Bench b;
    b.strict_read_back = b.preempt_unsent_reads = strict;
    V03 v(b);  // programmations comprises
    b.run(0, 75 * S);
    CHECK(b.request(v.dhw, 45, 75 * S) == "queued");
    bool armed = false, cleared = false;
    for (uint32_t t = 75 * S; t < 3 * MIN; t += 5) {
      b.boiler.tick(t);
      b.step(t);
      if (!armed && b.boiler.write_frames == 1) {
        b.boiler.register_faults[0x6300] = SimBoiler::Fault::SHORT_RESPONSE;  // relecture tronquée
        armed = true;
      }
      if (armed && !cleared && b.failures[v.dhw] > 0) {
        b.boiler.register_faults.clear();
        cleared = true;
      }
    }
    const auto &log = b.boiler.frame_log;
    size_t w = 0;
    while (log[w].first != 0xF4)
      w++;
    CHECK(w + 2 < log.size());
    CHECK(log[w + 1].second == 0x6300);  // relecture (échouée)
    const bool next_is_retry = log[w + 2].second == 0x6300;
    CHECK_EQ(next_is_retry, strict);
    CHECK_EQ(b.number_state[v.dhw], 45);
    CHECK_EQ(b.boiler.write_frames, 1);  // la relecture est retentée, jamais l'écriture
  }
}
