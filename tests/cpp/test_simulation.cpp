// Simulations de bout en bout : KwProtocol + RegisterScheduler face à une
// Vitotronic simulée. Ces tests éprouvent la logique, ils ne remplacent pas
// un essai sur la chaudière réelle.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <memory>

#include "decode.h"
#include "circuit_climate.h"
#include "dhw_setpoint.h"
#include "kw_protocol.h"
#include "schedule.h"
#include "scheduler.h"
#include "sim.h"
#include "test_framework.h"
#include "write_guard.h"
#include "circulation_boost.h"
#include "block_client.h"
#include "schedule_editor.h"
#include <functional>

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
    uint8_t raw_len{0};    ///< lecture brute de N octets (horloge 0x088E)
    InvalidRawValues invalid{};  ///< v0.5.1 : valeurs publiées inconnues (sensor)
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
  /// v0.5.1 : instant de la première publication de chaque jour.
  std::map<size_t, std::map<uint8_t, uint32_t>> day_published_ms;
  /// v0.5.1 : jour courant selon l'horloge `time` (0 = lundi), -1 = inconnue.
  int today{-1};
  bool today_first{true};  ///< false = comportement v0.5 (lundi d'abord, pas de relecture du jour)
  std::map<size_t, int> today_requested;  ///< VissLinkSchedule::requested_day_
  std::vector<std::pair<uint32_t, uint16_t>> schedule_reads;  ///< (instant, adresse) des blocs reçus
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

  // v0.5 : blocs horaires du bouclage (même logique que VissLinkComponent).
  bool enable_schedule_writes{false};
  struct DirectRead {
    bool pending{false}, in_flight{false};
    uint16_t address{0};
    uint8_t length{0}, failures{0};
    uint32_t due_ms{0};
    int owner{WriteArbiter::BOOST_OWNER};  ///< v0.8 : client destinataire
  } direct;
  /// v0.8 : éditeur des programmations (VissLinkComponent::block_clients_).
  BlockClient *editor{nullptr};
  /// v0.8 : octets lus de chaque jour (VissLinkSchedule::raw_week).
  std::map<size_t, WeekBlocks> raw_weeks;
  /// VissLinkComponent::block_clients_ : bouclage (fonctions ci-dessous) et éditeur.
  bool boost_busy() const { return boost_busy_fn && boost_busy_fn(); }
  std::function<bool()> boost_busy_fn;
  std::function<bool(uint16_t)> boost_locks_fn;
  bool locked_by_other(int owner, uint16_t address) const {
    if (owner == WriteArbiter::BOOST_OWNER)
      return editor != nullptr && editor->locks_block(address);
    return boost_locks_fn && boost_locks_fn(address);
  }
  bool block_verify_pending{false};
  std::function<void(uint16_t, bool, const uint8_t *, uint8_t, uint32_t)> on_direct_read;
  std::function<void(uint16_t, bool, bool, uint32_t)> on_block_write;
  std::function<bool(uint16_t, const uint8_t *)> expects_block_write;
  std::vector<KwResult> block_write_results;
  int block_refused_at_send{0};

  bool schedule_writes_enabled() const { return enable_writes && enable_schedule_writes; }
  bool writes_idle() const { return writes.pending_count() == 0 && !writes.active(); }
  static bool is_block_owner(int owner) { return BlockClients::is_block_owner(owner); }
  bool block_write_allowed(uint16_t address, const uint8_t *data, int owner = WriteArbiter::BOOST_OWNER) const {
    ScheduleSlots slots;
    const bool whitelisted = owner == WriteArbiter::BOOST_OWNER    ? is_schedule_write_whitelisted(address)
                             : owner == WriteArbiter::EDITOR_OWNER ? is_schedule_editor_whitelisted(address)
                                                                   : false;
    const bool expected = owner == WriteArbiter::BOOST_OWNER
                              ? (expects_block_write && expects_block_write(address, data))
                              : (editor != nullptr && editor->expects_block_write(address, data));
    return schedule_writes_enabled() && whitelisted &&
           decode_schedule_slots(data, SCHEDULE_BLOCK_LENGTH, &slots) == ScheduleError::NONE &&
           !locked_by_other(owner, address) && expected;
  }
  bool submit_block_write(uint16_t address, const uint8_t *data, uint32_t now, int owner = WriteArbiter::BOOST_OWNER) {
    if (!block_write_allowed(address, data, owner))
      return false;
    return writes.submit_block(owner, address, data, now) == WriteArbiter::Submit::QUEUED;
  }
  void notify_block_write(int owner, uint16_t address, bool sent, bool acked, uint32_t now) {
    if (owner == WriteArbiter::EDITOR_OWNER) {
      if (editor != nullptr)
        editor->on_block_write(address, sent, acked, now);
    } else if (on_block_write) {
      on_block_write(address, sent, acked, now);
    }
  }
  bool request_direct_read(uint16_t address, uint8_t len, uint32_t now, int owner = WriteArbiter::BOOST_OWNER) {
    const bool block_ok = owner == WriteArbiter::EDITOR_OWNER ? is_schedule_editor_whitelisted(address)
                                                              : is_schedule_write_whitelisted(address);
    const bool allowed =
        len == 8 && ((owner == WriteArbiter::BOOST_OWNER && address == VITO_CLOCK_ADDRESS) || block_ok);
    if (!allowed || direct.pending)
      return false;
    direct = DirectRead();
    direct.owner = owner;
    direct.pending = true;
    direct.address = address;
    direct.length = len;
    direct.due_ms = now;
    return true;
  }
  void deliver_direct(bool ok, uint32_t now) {
    direct.pending = false;
    if (block_verify_pending) {
      block_verify_pending = false;
      writes.complete(now);
    }
    if (direct.owner == WriteArbiter::EDITOR_OWNER) {
      if (editor != nullptr)
        editor->on_direct_read(direct.address, ok, ok ? kw.data() : nullptr, ok ? kw.data_length() : 0, now);
    } else if (on_direct_read) {
      on_direct_read(direct.address, ok, ok ? kw.data() : nullptr, ok ? kw.data_length() : 0, now);
    }
  }
  /// v0.8 : VissLinkSchedule::publish_data appelé par l'éditeur (bloc relu).
  void publish_schedule_block(uint16_t address, const uint8_t *data, uint32_t now) {
    size_t index;
    uint8_t block;
    DaySchedule day;
    if (!find_schedule_block(address, &index, &block) ||
        decode_schedule_day(data, SCHEDULE_BLOCK_LENGTH, &day) != ScheduleError::NONE)
      return;
    weeks[index].set_day(block, day);
    std::memcpy(raw_weeks[index].raw[block], data, SCHEDULE_BLOCK_LENGTH);
    raw_weeks[index].known_mask |= static_cast<uint8_t>(1u << block);
    (void) now;
  }
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

  // v0.6/v0.7 : consignes ECS et chauffage, une transaction par registre
  // (même logique que VissLinkComponent::SetpointChannel).
  bool enable_dhw_setpoint_write{false};
  bool enable_heating_setpoint_writes{false};
  SetpointWriter dhw;                    ///< consigne ECS (0x6300)
  int dhw_owner{WriteArbiter::NONE};
  std::map<size_t, SetpointWriter> heat;  ///< consignes de chauffage, par index
  /// VissLinkBlockClient::restore_priority (bouclage ECS).
  std::function<bool()> restore_priority;
  std::vector<DhwAction> dhw_actions;  ///< issues des demandes et relectures
  /// Observation (tests) : état de restauration du bouclage, indépendant de
  /// la priorité, et écritures de consigne démarrées pendant une restauration.
  std::function<bool()> restore_observer;
  int dhw_started_in_restore{0};
  int setpoint_started_in_restore{0};
  SetpointWriter *writer_for(int index) {
    if (index == WriteArbiter::NONE)
      return nullptr;
    if (index == dhw_owner)
      return &dhw;
    auto it = heat.find(static_cast<size_t>(index));
    return it == heat.end() ? nullptr : &it->second;
  }
  /// VissLinkComponent::setpoint_permitted_.
  bool setpoint_permitted(uint16_t address) const {
    if (is_dhw_setpoint_write_whitelisted(address))
      return dhw_write_permitted(address, enable_writes, enable_dhw_setpoint_write) == DhwPermission::OK;
    return heating_write_permitted(address, enable_writes, enable_heating_setpoint_writes) == HeatingPermission::OK;
  }
  /// VissLinkComponent::deferred_setpoints_.
  OwnerSet deferred_setpoints() const {
    OwnerSet set;
    if (!restore_priority || !restore_priority())
      return set;
    if (dhw_owner != WriteArbiter::NONE)
      set.add(dhw_owner);
    for (const auto &kv : heat)
      set.add(static_cast<int>(kv.first));
    return set;
  }
  /// VissLinkComponent::setpoint_on_read_.
  void setpoint_on_read(size_t index, bool ok, uint8_t value, bool verify, uint32_t now) {
    SetpointWriter *w = writer_for(static_cast<int>(index));
    const DhwAction a = w->on_read(ok, value, verify, now);
    if (a != DhwAction::NONE)
      dhw_actions.push_back(a);
    if (a == DhwAction::SUBMIT &&
        writes.submit(static_cast<int>(index), w->address(), w->target(), now) == WriteArbiter::Submit::FULL)
      w->on_dropped("full");
  }

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
  size_t add_raw(uint16_t address, uint8_t len, uint32_t interval_ms, uint32_t now) {
    Reg r{address, DataType::UINT8, false, 1};
    r.raw_len = len;
    regs.push_back(r);
    params.push_back({interval_ms, 1});
    scheduler.add(interval_ms, now + stagger_ms * (regs.size() - 1));
    return regs.size() - 1;
  }
  /// VissLinkComponent::clock_tick_ (une fois par seconde) : cycle des
  /// programmations commencé par le jour courant, et relecture de ce jour
  /// s'il n'est pas encore connu.
  void clock_tick(uint32_t now) {
    if (!today_first || today < 0)
      return;
    const uint8_t d = static_cast<uint8_t>(today);
    for (size_t i = 0; i < regs.size(); i++) {
      if (!regs[i].schedule || params[i].second != SCHEDULE_DAYS)
        continue;
      scheduler.set_cycle_start(i, d);
      const auto &e = scheduler.entry(i);
      auto it = today_requested.find(i);
      const bool requested = it != today_requested.end() && it->second == today;
      const bool next_in_cycle = e.urgent == RegisterScheduler::URGENT_NONE && e.done == 0 && e.block == d;
      if (!weeks[i].is_known(d) && !requested && !next_in_cycle && scheduler.expedite_block(i, d, now))
        today_requested[i] = today;
    }
  }
  /// VissLinkComponent::find_schedule_block_.
  bool find_schedule_block(uint16_t address, size_t *index, uint8_t *block) const {
    for (size_t i = 0; i < regs.size(); i++) {
      if (params[i].second < 2)
        continue;
      for (uint8_t blk = 0; blk < params[i].second; blk++) {
        if (block_address(regs[i], blk) == address) {
          *index = i;
          *block = blk;
          return true;
        }
      }
    }
    return false;
  }
  /// VissLinkComponent::request_block_refresh.
  bool request_block_refresh(uint16_t address, uint32_t now) {
    size_t i;
    uint8_t blk;
    return find_schedule_block(address, &i, &blk) && scheduler.expedite_block(i, blk, now);
  }
  bool cancel_block_refresh(uint16_t address) {
    size_t i;
    uint8_t blk;
    return find_schedule_block(address, &i, &blk) && scheduler.cancel_refresh(i, blk);
  }
  bool block_refresh_pending(uint16_t address) const {
    size_t i;
    uint8_t blk;
    return find_schedule_block(address, &i, &blk) && scheduler.refresh_pending(i, blk);
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
    // VissLinkComponent::add_setpoint_number
    if (address == DHW_SETPOINT_ADDRESS)
      dhw_owner = static_cast<int>(regs.size() - 1);
    else if (is_heating_setpoint_write_whitelisted(address))
      heat.emplace(regs.size() - 1, SetpointWriter(address));
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
    if (SetpointWriter *w = writer_for(static_cast<int>(index))) {
      // VissLinkComponent::submit_setpoint_ (v0.6/v0.7).
      DhwContext ctx;
      ctx.enable_writes = enable_writes;
      ctx.enable_dhw_setpoint_write = enable_dhw_setpoint_write;
      ctx.enable_heating_setpoint_writes = enable_heating_setpoint_writes;
      ctx.link_ok = link_ok;
      ctx.has_value = has_read_value(index);
      ctx.config_min = r.min;
      ctx.config_max = r.max;
      if (const HeatingSetpoint *h = find_heating_setpoint(r.address)) {
        for (size_t i = 0; i < regs.size(); i++) {
          if (regs[i].number && regs[i].address == h->peer && has_read_value(i)) {
            ctx.peer_known = true;
            ctx.peer_value = static_cast<uint8_t>(number_state[i]);
          }
        }
      }
      std::vector<const SetpointWriter *> writers = {&dhw};
      for (const auto &kv : heat)
        writers.push_back(&kv.second);
      setpoint_gate(writers, w, now, &ctx);  // VissLinkComponent::submit_setpoint_
      const DhwAction a = w->request(value, ctx, now);
      dhw_actions.push_back(a);
      if (a != DhwAction::PRE_READ)
        return std::string("refused: ") + w->reason();
      scheduler.expedite_block(index, 0, now);  // URGENT_REFRESH
      return "pre-read";
    }
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
    const uint32_t delay = scheduler.block_delay();
    scheduler = RegisterScheduler();
    scheduler.set_block_delay(delay);
    for (size_t i = 0; i < params.size(); i++)
      scheduler.add(params[i].first, now + stagger_ms * i, params[i].second);
    today_requested.clear();
    weeks.clear();  // programmations publiées perdues avec la RAM
    raw_weeks.clear();
    day_published_ms.clear();
    writes = WriteArbiter();
    dhw = SetpointWriter();  // rien de persistant
    for (auto &kv : heat)
      kv.second = SetpointWriter(kv.second.address());
    write_in_flight = false;
    link_ok = false;
    verify.clear();
    number_state.clear();
    mode_state.clear();
    read_back = ReadBackTracker();
    direct = DirectRead();
    block_verify_pending = false;
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
    if (r.raw_len)
      return r.raw_len;
    return r.binary ? 1 : data_type_length(r.type);
  }
  static uint16_t block_address(const Reg &r, uint8_t block) {
    return r.schedule ? schedule_day_address(r.address, block) : r.address;
  }

  /// Même enchaînement que VissLinkComponent::loop() / handle_result_().
  void step(uint32_t now) {
    const KwResult result = kw.loop(now, serial);
    dhw.poll(now);  // VissLinkComponent::loop
    for (auto &kv : heat)
      kv.second.poll(now);
    if (result != KwResult::NONE && write_in_flight && is_block_owner(writes.current().owner)) {
      // VissLinkComponent::handle_block_write_result_
      write_in_flight = false;
      block_write_results.push_back(result);
      const bool sent = result != KwResult::SYNC_TIMEOUT;
      if (!sent)
        link_ok = false;
      const uint16_t address = writes.current().address;
      const int owner = writes.current().owner;
      if (sent)
        block_verify_pending = true;
      else
        writes.complete(now);
      notify_block_write(owner, address, sent, result == KwResult::SUCCESS, now);
      if (sent && !direct.pending) {
        block_verify_pending = false;
        writes.complete(now);
      }
    } else if (result != KwResult::NONE && direct.in_flight) {
      // VissLinkComponent::handle_direct_result_
      direct.in_flight = false;
      if (result == KwResult::SUCCESS) {
        link_ok = true;
        deliver_direct(true, now);
      } else {
        if (result == KwResult::SYNC_TIMEOUT)
          link_ok = false;
        if (direct.failures < scheduler.max_retries()) {
          direct.failures++;
          direct.due_ms = now + RegisterScheduler::RETRY_DELAY_MS;
        } else {
          deliver_direct(false, now);
        }
      }
    } else if (result != KwResult::NONE && write_in_flight) {
      // VissLinkComponent::handle_write_result_
      write_in_flight = false;
      write_results.push_back(result);
      timing.sent = kw.sent_ms();
      timing.acked = now;
      if (result == KwResult::SYNC_TIMEOUT)
        link_ok = false;
      const WriteArbiter::Request &w = writes.current();
      if (SetpointWriter *sw = writer_for(w.owner))
        sw->on_ack(result == KwResult::SUCCESS          ? DhwAck::RECEIVED
                   : result == KwResult::WRITE_REJECTED ? DhwAck::REJECTED
                   : result == KwResult::SYNC_TIMEOUT   ? DhwAck::NOT_SENT
                                                        : DhwAck::INDETERMINATE);
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
        if (accepted) {
          weeks[index].set_day(block, day);
          std::memcpy(raw_weeks[index].raw[block], kw.data(), SCHEDULE_BLOCK_LENGTH);
          raw_weeks[index].known_mask |= static_cast<uint8_t>(1u << block);
          day_published_ms[index].emplace(block, now);
          schedule_reads.push_back({now, block_address(r, block)});
        } else
          rejected[index]++;
      }
      if (accepted) {
        scheduler.finish(true, now);
        successes[index]++;
        if (r.schedule) {
          // décodé ci-dessus
        } else if (r.number) {
          const uint8_t value = kw.data()[0];
          const bool verifying = writes.active() && writes.current().owner == static_cast<int>(index) &&
                                 read_back.waiting(static_cast<int>(index));
          if (verify[index].pending())
            verify_results.push_back(verify[index].on_read(value));
          number_state[index] = value;  // toujours la valeur lue
          if (writer_for(static_cast<int>(index)) != nullptr)
            setpoint_on_read(index, true, value, verifying, now);
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
        else if (r.invalid.contains(decode_raw_le(kw.data(), length(r))))
          last_value[index] = NAN;  // VissLinkSensor::publish_data : « inconnu »
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
            const bool verifying = writes.active() && writes.current().owner == static_cast<int>(index) &&
                                   read_back.waiting(static_cast<int>(index));
            const bool was_pending = verify[index].pending();
            if (was_pending)
              verify_results.push_back(verify[index].on_give_up());
            // VissLinkNumber::publish_unknown : consigne ECS, issue inconnue
            // -> dernière valeur confirmée conservée (v0.6).
            uint8_t lo, hi;
            if (r.number && !(was_pending && setpoint_write_limits(r.address, &lo, &hi)))
              number_state[index] = NAN;
            else if (r.mode)
              mode_state[index] = -1;
            if (writer_for(static_cast<int>(index)) != nullptr)
              setpoint_on_read(index, false, 0, verifying, now);
            read_back_done(index, now);
          }
        }
      }
    }
    // VissLinkComponent::preempt_unsent_read_ (v0.4.1, lectures ponctuelles v0.5).
    const bool direct_ready = direct.pending && !direct.in_flight && time_reached(now, direct.due_ms);
    if (preempt_unsent_reads && !write_in_flight && !direct.in_flight && kw.is_waiting_sync() &&
        scheduler.current() != RegisterScheduler::NONE && scheduler.entry(scheduler.current()).urgent != RegisterScheduler::URGENT_VERIFY &&
        ((enable_writes && writes.ready(now, link_ok, deferred_setpoints())) || direct_ready) && kw.cancel_before_send()) {
      scheduler.release();
      preemptions++;
    }
    if (kw.is_idle() && scheduler.current() == RegisterScheduler::NONE && !direct.in_flight) {
      WriteArbiter::Request w;
      while (writes.pop_expired(now, &w)) {
        expired_writes++;
        if (SetpointWriter *sw = writer_for(w.owner))
          sw->on_dropped("expired");
        if (is_block_owner(w.owner))
          notify_block_write(w.owner, w.address, false, false, now);
      }
      bool started = false;
      if (enable_writes && writes.next(now, link_ok, &w, deferred_setpoints())) {
        if (is_block_owner(w.owner)) {
          // VissLinkComponent::start_write_, bloc horaire : contrôles revérifiés.
          if (w.length == 8 && block_write_allowed(w.address, w.data, w.owner) &&
              kw.request_write(w.address, w.data, 8, now)) {
            write_in_flight = true;
            started = true;
          } else {
            block_refused_at_send++;
            writes.complete(now);
            notify_block_write(w.owner, w.address, false, false, now);
          }
        } else {
          // VissLinkComponent::start_write_ : contrôle complet avant émission
          // (VissLinkRegister::write_allowed, refus par défaut hors number).
          const Reg &r = regs[w.owner];
          uint8_t encoded = 0;
          SetpointWriter *sw = writer_for(w.owner);
          const bool setpoint_ok = find_writable(w.address) == nullptr ||
                                   (sw != nullptr && sw->address() == w.address && setpoint_permitted(w.address) &&
                                    sw->expects_write(w.value));
          const bool number_ok = r.number && find_writable(w.address) != nullptr && setpoint_ok &&
                                 check_write_value(w.address, 1, w.value, r.min, r.max, &encoded) == WriteCheck::OK &&
                                 encoded == w.value;
          const bool mode_ok =
              r.mode && is_mode_register(w.address) && check_mode_write(w.address, 1, w.value) == WriteCheck::OK &&
              mode_write_permitted(w.address, enable_writes, enable_mode_writes, enable_m2_mode_write) ==
                  ModePermission::OK;
          const bool allowed = r.address == w.address && is_write_whitelisted(w.address) && (number_ok || mode_ok);
          if (allowed && kw.request_write(w.address, &w.value, 1, now)) {
            write_in_flight = true;
            started = true;
            if (restore_observer && restore_observer() && writer_for(w.owner) != nullptr) {
              setpoint_started_in_restore++;
              if (static_cast<int>(w.owner) == dhw_owner)
                dhw_started_in_restore++;
            }
            write_start_ms.push_back(now);
            timing = WriteTiming();
            timing.requested = w.requested_ms;
            timing.started = now;
          } else {
            refused_at_send++;
            writes.complete(now);
            if (SetpointWriter *sw = writer_for(w.owner))
              sw->on_dropped("refused at send time");
          }
        }
      }
      // VissLinkComponent::start_direct_read_
      if (!started && direct.pending && time_reached(now, direct.due_ms) && read_back.idle() &&
          kw.request_read(direct.address, direct.length, now))
        direct.in_flight = true;
    }
    if (kw.is_idle()) {
      const int index =
          scheduler.acquire(now, (strict_read_back && !read_back.idle()) || direct.pending || block_verify_pending);
      if (index != RegisterScheduler::NONE &&
          !kw.request_read(block_address(regs[index], scheduler.block(index)), length(regs[index]), now))
        scheduler.finish(false, now);
    }
  }

  /// Avance le temps par pas de 5 ms (≈ boucle ESPHome rapide).
  void run(uint32_t from, uint32_t duration_ms) {
    for (uint32_t t = from; t - from < duration_ms; t += 5) {
      boiler.tick(t);
      if (t % 1000 == 0)
        clock_tick(t);
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
    // v0.7 : les consignes de chauffage ont leur propre autorisation.
    b.enable_heating_setpoint_writes = writes;
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

  CHECK(b.request(v.a1_comfort, 21, BOOT) == "pre-read");  // v0.7 : relecture avant écriture
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
    // v0.7 : une demande par minute (30 s minimum après une écriture, qui
    // peut se terminer ~20 s après la demande à 4 s de resynchro).
    for (uint32_t t = BOOT; t < BOOT + 12 * MIN; t += MIN) {
      CHECK(b.request(v.m2_comfort, (t / MIN) % 2 ? 22 : 18, t) == "pre-read");
      b.run(t, MIN);
    }
    const auto &log = b.boiler.frame_log;
    int writes = 0;
    for (size_t i = 0; i + 1 < log.size(); i++) {
      if (log[i].first == 0xF4) {
        writes++;
        CHECK(log[i + 1] == (std::pair<uint8_t, uint16_t>{0xF7, 0x3306}));
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
    for (size_t n : v.numbers()) {
      const std::string r = b.request(n, b.regs[n].min + 1, t);
      refused += r == "disabled" || r == "refused: writes are disabled (enable_writes: false)";
    }
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
    CHECK(b.request(v.m2_comfort, 22, BOOT) == "pre-read");  // v0.7 : M2 confort <= 22
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
  b.boiler.write_clamp[0x3306] = {16, 20};  // la régulation borne à 20
  b.run(0, BOOT);
  CHECK(b.request(v.a1_reduced, 18, BOOT) == "pre-read");
  b.run(BOOT, MIN);
  CHECK(b.request(v.m2_comfort, 22, BOOT + MIN) == "pre-read");
  b.run(BOOT + MIN, MIN);
  CHECK((b.write_results == std::vector<KwResult>{KwResult::SUCCESS, KwResult::SUCCESS}));
  CHECK((b.verify_results == std::vector<VerifyResult>{VerifyResult::MISMATCH, VerifyResult::MISMATCH}));
  CHECK_EQ(b.number_state[v.a1_reduced], 16);
  CHECK_EQ(b.number_state[v.m2_comfort], 20);  // la valeur relue, bornée
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
    CHECK(b.request(v.a1_comfort, 22, BOOT) == "pre-read");
    b.run(BOOT, 10 * MIN);
    CHECK_EQ(b.boiler.write_frames, 1);  // jamais répétée, même sans accusé
    CHECK(b.write_results == std::vector<KwResult>{c.result});
    CHECK(b.verify_results == std::vector<VerifyResult>{c.verify});
    CHECK_EQ(b.number_state[v.a1_comfort], c.value);
    CHECK(b.successes[v.burner] >= 20);
  }
}

TEST(sim_v03_rapid_changes_on_one_entity_are_coalesced) {
  // 20 -> 21 -> 22 -> 23 en moins d'une seconde. v0.3 : la dernière
  // remplaçait la demande en attente. v0.7 : une seule opération par
  // consigne ; les suivantes sont refusées tant qu'elle est en cours, jamais
  // mises en file : une seule écriture, 21.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  uint32_t t = BOOT;
  CHECK(b.request(v.a1_comfort, 21, t) == "pre-read");
  for (int value = 22; value <= 23; value++) {
    t += 200;
    b.run(t - 200, 200);
    CHECK(b.request(v.a1_comfort, value, t) == "refused: another heating setpoint change is in progress");
  }
  b.run(t, 2 * MIN);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK_EQ(b.boiler.write_log[0][5], 21);
  CHECK_EQ(b.boiler_value(0x2306), 21);
  CHECK_EQ(b.number_state[v.a1_comfort], 21);
  CHECK(b.verify_results.back() == VerifyResult::CONFIRMED);
}

TEST(sim_v03_several_entities_serialized_and_spaced) {
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  const int target[] = {22, 18, 22, 19, 55};  // v0.7 : M2 confort <= 22
  const auto numbers = v.numbers();
  b.enable_dhw_setpoint_write = true;  // v0.6 : la consigne ECS a sa propre autorisation
  // v0.7 : une modification de consigne à la fois, 15 s entre deux consignes
  // (garde commune) : les autres demandes simultanées sont refusées.
  CHECK(b.request(numbers[0], target[0], BOOT) == "pre-read");
  for (size_t i = 1; i < numbers.size(); i++)
    CHECK(b.request(numbers[i], target[i], BOOT) == "refused: another setpoint change is in progress");
  uint32_t t = BOOT;
  for (size_t i = 1; i < numbers.size(); i++) {
    while (b.writer_for(static_cast<int>(numbers[i - 1]))->busy())
      b.run(t, 5), t += 5;
    CHECK(b.request(numbers[i], target[i], t) == "refused: too soon after another setpoint write");
    b.run(t, SETPOINT_GLOBAL_SPACING_MS);
    t += SETPOINT_GLOBAL_SPACING_MS;
    CHECK(b.request(numbers[i], target[i], t) == "pre-read");
  }
  b.run(t, 3 * MIN);
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
    // v0.6 : même rotation sur les cinq consignes, mais l'ECS est refusée
    // (enable_dhw_setpoint_write: false, et 40..42 hors limites internes) :
    // seules les quatre consignes d'ambiance sont écrites. La charge propre
    // à l'ECS (relecture préalable) est mesurée par sim_v06_dhw_*.
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
      // Jamais plus de deux cycles sautés : 3 × 30 s + latence d'une lecture
      // (v0.6 : 93 s mesuré, l'ECS ne s'écrivant plus dans cette rafale ;
      // 85 s en v0.4.1 avec l'ECS).
      CHECK(b.max_gap_ms[v.burner] <= 95 * S);
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
  CHECK(b.request(v.a1_comfort, 22, BOOT) == "pre-read");
  uint32_t t = BOOT;
  while (b.timings.empty() && t < BOOT + MIN)
    b.run(t, 5), t += 5;
  CHECK_EQ(b.boiler.write_frames, 1);  // première écriture terminée
  // v0.7 : garde commune (15 s), puis relecture préalable ; la liaison tombe
  // entre la relecture et l'émission de l'écriture.
  b.run(t, SETPOINT_GLOBAL_SPACING_MS);
  t += SETPOINT_GLOBAL_SPACING_MS;
  CHECK(b.request(v.m2_comfort, 22, t) == "pre-read");
  while (b.writer_for(static_cast<int>(v.m2_comfort))->state() != DhwState::WRITING && t < BOOT + 2 * MIN)
    b.run(t, 5), t += 5;
  CHECK(!b.write_in_flight || !b.kw.is_idle());  // pas encore émise
  b.boiler.connected = false;
  b.run(t, 30 * S);
  CHECK(!b.link_ok);
  CHECK(b.request(v.a1_reduced, 18, t + 30 * S).rfind("refused: ", 0) == 0);  // tout de suite
  b.run(t + 30 * S, 3 * MIN);
  // Abandonnée après REQUEST_EXPIRY_MS, ou partie sans 0x05 (trame non émise,
  // v0.7 : la relecture préalable décale l'instant de la coupure).
  CHECK(b.expired_writes == 1 || b.write_results.back() == KwResult::SYNC_TIMEOUT);
  CHECK(b.writes.pending_count() == 0);
  b.boiler.connected = true;
  b.run(t + 210 * S, 20 * MIN);
  CHECK_EQ(b.boiler.write_frames, 1);  // jamais envoyée plus tard
  CHECK_EQ(b.boiler_value(0x3306), 21);
  CHECK(b.link_ok);
  CHECK_EQ(b.number_state[v.m2_comfort], 21);
}

TEST(sim_v041_link_loss_with_preempted_read_sends_nothing) {
  // v0.4.1 : la demande prend la place d'une lecture en attente du 0x05 ;
  // l'interface est débranchée : pas de 0x05, rien n'est émis
  // (sync timeout), aucune nouvelle tentative après le retour de la liaison.
  // v0.7 : une consigne n'est déposée qu'après sa relecture préalable ; la
  // préemption est donc vérifiée avec une commande de programme (A1), seule
  // commande encore déposée directement.
  Bench b;
  V03 v(b, false);
  b.enable_mode_writes = true;
  b.boiler.registers[0x2301] = {0x03};
  b.boiler.writable.insert(0x2301);
  const size_t a1 = b.add_mode(0x2301, 5 * MIN, 0);
  b.run(0, BOOT);
  uint32_t t = BOOT;
  while (!b.kw.is_waiting_sync())
    b.run(t, 5), t += 5;
  b.boiler.connected = false;
  CHECK(b.request_mode(a1, 0, t) == "queued");  // ECS seule
  b.run(t, 30 * S);
  CHECK_EQ(b.preemptions, 1);
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SYNC_TIMEOUT});
  CHECK(!b.link_ok);
  b.boiler.connected = true;
  b.run(t + 30 * S, 20 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.boiler_value(0x2301), 0x03);
  CHECK_EQ(b.mode_of(a1), 0x03);
  CHECK_EQ(b.number_state[v.m2_comfort], 21);
}

TEST(sim_v03_link_lost_during_write_frame_not_sent) {
  // Écriture partie vers la ligne alors que l'interface est débranchée : pas
  // de 0x05, donc pas de trame (SYNC_TIMEOUT), relecture impossible puis
  // abandonnée ; aucune nouvelle tentative après le retour de la liaison.
  Bench b;
  V03 v(b, false);
  b.run(0, BOOT);
  CHECK(b.request(v.m2_reduced, 18, BOOT) == "pre-read");
  uint32_t t = BOOT;
  while (!b.write_in_flight && t < BOOT + MIN)
    b.run(t, 5), t += 5;  // v0.7 : relecture préalable faite, trame partie
  b.boiler.connected = false;
  b.run(t, 2 * MIN);
  CHECK(b.write_results == std::vector<KwResult>{KwResult::SYNC_TIMEOUT});
  CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::UNVERIFIED});
  // v0.7 : la dernière valeur confirmée reste affichée (jamais « 18 »).
  CHECK_EQ(b.number_state[v.m2_reduced], 17);
  b.boiler.connected = true;
  b.run(t + 2 * MIN, 10 * MIN);
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
  CHECK(b.request(v.a1_comfort, 23, BOOT) == "pre-read");
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
  CHECK(b.request(v.a1_comfort, 21, 0) == "refused: current value not read yet");  // valeur actuelle inconnue
  b.run(0, BOOT);
  CHECK(b.request(v.a1_comfort, 25, BOOT) == "refused: value outside the configured range");
  CHECK(b.request(v.a1_comfort, 15, BOOT) == "refused: value outside the configured range");
  CHECK(b.request(v.a1_comfort, 20.5f, BOOT) == "refused: value is not an integer");
  CHECK(b.request(v.a1_comfort, NAN, BOOT) == "refused: value is not a finite number");
  CHECK(b.request(v.m2_comfort, 25, BOOT) == "refused: value outside the configured range");
  CHECK(b.request(v.m2_comfort, 23, BOOT) ==
        "refused: heating setpoint outside the internal limits of this register");  // v0.7 : M2 confort <= 22
  // Déjà la valeur lue : la relecture préalable le confirme, rien n'est écrit.
  CHECK(b.request(v.a1_comfort, 20, BOOT) == "pre-read");
  b.run(BOOT, 20 * S);
  CHECK(b.writer_for(static_cast<int>(v.a1_comfort))->last_outcome() == DhwOutcome::UNCHANGED);
  // Un registre lu mais hors liste blanche ne peut pas être écrit, même
  // déclaré par erreur comme consigne.
  const size_t rogue = b.add_number(0x0842, 0, 1, 5 * MIN, BOOT);
  b.run(BOOT + 20 * S, MIN);
  CHECK(b.request(rogue, 0, BOOT + MIN) == "address not in the write whitelist");
  b.run(BOOT + 20 * S + MIN, 5 * MIN);
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
  CHECK(b.request(v.a1_comfort, 22, BOOT) == "pre-read");
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
  CHECK_EQ(b.number_state[v.a1_comfort], 20);  // v0.7 : dernière valeur confirmée, jamais 22
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
    CHECK(b.request(v.m2_comfort, 22, 75 * S) == "pre-read");  // v0.7 : M2 confort <= 22
    bool armed = false, cleared = false;
    for (uint32_t t = 75 * S; t < 3 * MIN; t += 5) {
      b.boiler.tick(t);
      b.step(t);
      if (!armed && b.boiler.write_frames == 1) {
        b.boiler.register_faults[0x3306] = SimBoiler::Fault::SHORT_RESPONSE;  // relecture tronquée
        armed = true;
      }
      if (armed && !cleared && b.failures[v.m2_comfort] > 0) {
        b.boiler.register_faults.clear();
        cleared = true;
      }
    }
    const auto &log = b.boiler.frame_log;
    size_t w = 0;
    while (log[w].first != 0xF4)
      w++;
    CHECK(w + 2 < log.size());
    CHECK(log[w + 1].second == 0x3306);  // relecture (échouée)
    const bool next_is_retry = log[w + 2].second == 0x3306;
    CHECK_EQ(next_is_retry, strict);
    CHECK_EQ(b.number_state[v.m2_comfort], 22);
    CHECK_EQ(b.boiler.write_frames, 1);  // la relecture est retentée, jamais l'écriture
  }
}

// --- v0.5 : bouclage ECS à la demande (blocs horaires de 8 octets) ------------
//
// Chaudière SIMULÉE : écritures de 8 octets appliquées et accusées par 00
// (hypothèse vcontrold), défauts injectés. Rien ici ne valide l'écriture sur
// la KW3 réelle.

namespace {

/// Même logique que VissLinkBoost (components/visslink_boost) : exécute les
/// actions du séquenceur, flash simulée.
struct BoostGlue {
  Bench &b;
  BoostSequencer seq;
  BoostConfig config;
  bool flash_has{false};
  BoostRecord flash;
  bool save_fails{false};
  int saves{0};
  BoostAction pending;
  bool has_pending{false};
  /// v0.5.1 : VissLinkBoost::refresh_display_ (ComponentRefreshPort).
  struct Port : BoostRefreshPort {
    Bench &b;
    uint32_t now{0};
    explicit Port(Bench &bench) : b(bench) {}
    bool is_schedule_block(uint16_t a) const override {
      size_t i;
      uint8_t blk;
      return b.find_schedule_block(a, &i, &blk);
    }
    bool request_refresh(uint16_t a) override { return b.request_block_refresh(a, now); }
    bool cancel_refresh(uint16_t a) override { return b.cancel_block_refresh(a); }
    bool refresh_pending(uint16_t a) const override { return b.block_refresh_pending(a); }
    bool writes_idle() const override { return b.writes_idle(); }
  } port{b};
  BoostRefreshRelay relay;
  bool relay_enabled{true};
  std::vector<std::pair<BoostRefreshRelay::Event, uint32_t>> relay_events;

  void refresh(uint32_t now) {
    if (!relay_enabled)
      return;
    port.now = now;
    const auto e = relay.poll(seq, port, now);
    if (e != BoostRefreshRelay::Event::NONE)
      relay_events.push_back({e, now});
  }
  int relay_count(BoostRefreshRelay::Event e) const {
    int n = 0;
    for (const auto &p : relay_events)
      n += p.first == e;
    return n;
  }

  BoostGlue(Bench &bench, uint8_t validated_mask) : b(bench) {
    config.validated_days_mask = validated_mask;
    boot();
  }
  void boot() {
    seq = BoostSequencer();
    relay = BoostRefreshRelay();
    seq.configure(config);
    seq.boot(flash_has ? &flash : nullptr);
    has_pending = false;
    b.on_direct_read = [this](uint16_t, bool ok, const uint8_t *d, uint8_t len, uint32_t now) {
      seq.on_read(ok, d, len, now);
      dispatch(now);
    };
    b.on_block_write = [this](uint16_t, bool sent, bool acked, uint32_t now) {
      seq.on_write(sent, acked, now);
      dispatch(now);
    };
    b.expects_block_write = [this](uint16_t a, const uint8_t *d) { return seq.expects_write(a, d); };
    b.boost_busy_fn = [this]() { return !seq.idle(); };
    b.boost_locks_fn = [this](uint16_t a) { return seq.locks_block(a); };
  }
  /// VissLinkBoost::press(START).
  std::string start(uint16_t duration, const HaTime &ha, uint32_t now) {
    if (!b.schedule_writes_enabled())
      return "disabled";
    if (!b.link_ok)
      return "link";
    if (!b.writes_idle() && seq.idle())
      return "busy";
    if (seq.idle() && b.editor != nullptr && b.editor->block_operation_busy())
      return "editor";  // v0.8 : modification de programmation en cours
    const BoostStart r = seq.start(duration, ha, now);
    dispatch(now);
    return r == BoostStart::ACCEPTED ? "accepted" : r == BoostStart::RECOVERY_PENDING ? "recovery" : "refused";
  }
  bool restore(uint32_t now) {
    if (!b.schedule_writes_enabled())
      return false;
    const bool ok = seq.request_restore(now);
    dispatch(now);
    return ok;
  }
  /// VissLinkBoost::dispatch_.
  void dispatch(uint32_t now) {
    for (int guard = 0; guard < 8; guard++) {
      if (!has_pending) {
        pending = seq.poll(now);
        if (pending.type == BoostActionType::NONE)
          return;
        has_pending = true;
      }
      const BoostAction a = pending;
      switch (a.type) {
        case BoostActionType::READ:
          if (!b.request_direct_read(a.address, a.length, now))
            return;
          has_pending = false;
          return;
        case BoostActionType::WRITE:
          has_pending = false;
          if (!b.submit_block_write(a.address, a.data, now))
            seq.on_write(false, false, now);
          else
            return;
          break;
        case BoostActionType::SAVE:
          has_pending = false;
          saves++;
          if (!save_fails) {
            flash = a.record;
            flash_has = true;
          }
          seq.on_saved(!save_fails, now);
          break;
        case BoostActionType::CLEAR:
          has_pending = false;
          saves++;
          flash_has = false;
          seq.on_saved(true, now);
          break;
        default:
          has_pending = false;
          return;
      }
    }
  }
};

/// Horloge de la régulation simulée : 2026-11-15 (dimanche) 14:00:00 à t = 0.
constexpr uint32_t CLOCK_BASE_S = 14 * 3600;

void set_vito_clock(SimBoiler &boiler, uint32_t now_ms) {
  const uint32_t t = CLOCK_BASE_S + now_ms / 1000;
  auto bcd = [](uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); };
  boiler.registers[VITO_CLOCK_ADDRESS] = {0x20, 0x26, 0x11, 0x15, 0x07, bcd(t / 3600 % 24), bcd(t / 60 % 60),
                                          bcd(t % 60)};
}

HaTime ha_now(uint32_t now_ms) {
  HaTime h;
  h.valid = true;
  h.weekday = 6;
  h.minute_of_day = static_cast<uint16_t>((CLOCK_BASE_S + now_ms / 1000) / 60 % 1440);
  return h;
}

/// Pas de 5 ms : chaudière, composant, colle du bouclage, horloge.
void run_boost(Bench &b, BoostGlue &g, uint32_t from, uint32_t duration_ms) {
  for (uint32_t t = from; t - from < duration_ms; t += 5) {
    if (t % 1000 == 0) {
      set_vito_clock(b.boiler, t);
      b.clock_tick(t);
    }
    b.boiler.tick(t);
    b.step(t);
    g.dispatch(t);
    g.refresh(t);
  }
}

const std::vector<uint8_t> LOOP_SUNDAY = {0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF};
const std::vector<uint8_t> LOOP_SUNDAY_BOOST = {0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF};  // + 14:00-14:20

struct BoostBench {
  Modes modes;
  BoostGlue glue;
  BoostBench(Bench &b, bool schedule_writes = true)
      : modes(b, true, false, true), glue(b, 1u << 6) {
    b.enable_schedule_writes = schedule_writes;
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      b.boiler.writable.insert(schedule_day_address(LOOP, d));
    set_vito_clock(b.boiler, 0);
  }
};

int writes_to(const SimBoiler &boiler, uint16_t address) {
  auto it = boiler.write_frames_by_address.find(address);
  return it == boiler.write_frames_by_address.end() ? 0 : it->second;
}

}  // namespace

TEST(sim_v05_boost_end_to_end) {
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);  // 14:02
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "accepted");
  run_boost(b, x.glue, BOOT, MIN);
  CHECK(x.glue.seq.state() == BoostState::ACTIVE);
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY_BOOST);
  CHECK(x.glue.flash_has);
  // Trame d'écriture : 01 F4 22 30 08 + 8 octets, une seule.
  CHECK(b.boiler.write_log.size() == 1);
  CHECK(b.boiler.write_log[0] == (std::vector<uint8_t>{0x01, 0xF4, 0x22, 0x30, 0x08, 0x31, 0x43, 0x70, 0x72, 0xAC,
                                                        0xAD, 0xFF, 0xFF}));
  // Horloge et bloc lus avant, relecture juste après l'écriture.
  const auto &log = b.boiler.frame_log;
  size_t w = 0;
  while (log[w].first != 0xF4)
    w++;
  CHECK(log[w + 1] == (std::pair<uint8_t, uint16_t>{0xF7, 0x2230}));
  bool clock_before = false, block_before = false;
  for (size_t i = 0; i < w; i++) {
    clock_before |= log[i].second == VITO_CLOCK_ADDRESS;
    block_before |= log[i] == std::pair<uint8_t, uint16_t>{0xF7, 0x2230} && clock_before;
  }
  CHECK(clock_before && block_before);
  // v0.5.2 : restauration 10 min après la confirmation (vers 14:12), et non
  // plus à la fin de la plage 14:20 + 2 min.
  run_boost(b, x.glue, BOOT + MIN, 25 * MIN);
  CHECK(x.glue.seq.idle());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(!x.glue.flash_has);
  CHECK(x.glue.seq.status().rfind("Programmation restaurée (vérifiée par relecture), après 10 min 0", 0) == 0);
  CHECK_EQ(writes_to(b.boiler, 0x2230), 2);
  CHECK_EQ(b.boiler.write_frames, 2);  // rien d'autre : ni 0x0846, ni les autres programmes
  CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
  CHECK(b.failures.empty());
  CHECK(b.successes[x.modes.base.burner] >= 50);
}

TEST(sim_v05_boost_disabled_by_default) {
  for (int config = 0; config < 2; config++) {
    Bench b;
    BoostBench x(b, false);  // enable_schedule_writes: false
    b.enable_writes = config == 1;
    run_boost(b, x.glue, 0, BOOT);
    CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "disabled");
    CHECK(!x.glue.restore(BOOT));
    run_boost(b, x.glue, BOOT, H);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  }
  // Le séquenceur lui-même ne peut rien faire passer sans l'autorisation :
  // une écriture de bloc est refusée au dépôt et à l'émission.
  Bench b;
  BoostBench x(b, false);
  b.enable_writes = true;
  run_boost(b, x.glue, 0, BOOT);
  const uint8_t block[8] = {0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF};
  CHECK(!b.submit_block_write(0x2230, block, BOOT));
  b.writes.submit_block(WriteArbiter::BOOST_OWNER, 0x2230, block, BOOT);  // contournement
  run_boost(b, x.glue, BOOT, MIN);
  CHECK_EQ(b.block_refused_at_send, 1);
  CHECK_EQ(b.boiler.write_frames, 0);
}

TEST(sim_v05_only_loop_blocks_can_be_written) {
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  const uint8_t block[8] = {0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF};
  for (uint16_t a : {0x2000, 0x2030, 0x3000, 0x2100, 0x2238, 0x088E, 0x0846, 0x2301})
    CHECK(!b.submit_block_write(a, block, BOOT));
  // Injectés directement dans l'arbitre : refusés juste avant l'émission.
  uint32_t t = BOOT;
  for (uint16_t a : {0x2000, 0x2100, 0x088E, 0x2230}) {  // 0x2230 : non attendu par le séquenceur
    b.writes.submit_block(WriteArbiter::BOOST_OWNER, a, block, t);
    run_boost(b, x.glue, t, 15 * S);
    t += 15 * S;
  }
  CHECK_EQ(b.block_refused_at_send, 4);
  CHECK_EQ(b.boiler.write_frames, 0);
  // Lecture ponctuelle : seulement l'horloge et les blocs du bouclage.
  CHECK(!b.request_direct_read(0x0846, 8, t));
  CHECK(!b.request_direct_read(0x2000, 8, t));
  CHECK(!b.request_direct_read(0x2230, 1, t));
}

TEST(sim_v05_boost_kw_sync_loss_and_unexpected_replies) {
  {  // Interface débranchée juste avant l'écriture : rien n'est émis.
    Bench b;
    BoostBench x(b);
    run_boost(b, x.glue, 0, BOOT);
    CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "accepted");
    uint32_t t = BOOT;
    while (x.glue.seq.state() != BoostState::WRITE_PENDING && t < BOOT + MIN)
      run_boost(b, x.glue, t, 5), t += 5;
    b.boiler.connected = false;
    run_boost(b, x.glue, t, 2 * MIN);
    CHECK(x.glue.seq.idle());
    CHECK(!x.glue.flash_has);
    CHECK_EQ(b.boiler.write_frames, 0);
    b.boiler.connected = true;
    run_boost(b, x.glue, t + 2 * MIN, 30 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);  // jamais réémise
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  }
  {  // Réponse de 8 octets (format VitoWiFi) : issue indéterminée, la relecture tranche.
    Bench b;
    BoostBench x(b);
    b.boiler.write_faults[0x2230] = SimBoiler::WriteFault::LONG_REPLY;
    run_boost(b, x.glue, 0, BOOT);
    x.glue.start(10, ha_now(BOOT), BOOT);
    run_boost(b, x.glue, BOOT, MIN);
    CHECK(b.block_write_results.front() == KwResult::RESPONSE_TOO_LONG);
    CHECK(x.glue.seq.state() == BoostState::ACTIVE);  // appliqué : constaté par relecture
    CHECK_EQ(b.boiler.write_frames, 1);
  }
  {  // Accusé 00 mais bloc inchangé : refus constaté, sauvegarde effacée.
    Bench b;
    BoostBench x(b);
    b.boiler.write_faults[0x2230] = SimBoiler::WriteFault::ACK_IGNORED;
    run_boost(b, x.glue, 0, BOOT);
    x.glue.start(10, ha_now(BOOT), BOOT);
    run_boost(b, x.glue, BOOT, 30 * MIN);
    CHECK(x.glue.seq.idle());
    CHECK(x.glue.seq.status() == "Refusé par la régulation : bloc inchangé après écriture");
    CHECK(!x.glue.flash_has);
    CHECK_EQ(b.boiler.write_frames, 1);
  }
  {  // Accusé perdu, écriture appliquée : relecture -> plage active ; restaurée ensuite.
    Bench b;
    BoostBench x(b);
    b.boiler.write_faults[0x2230] = SimBoiler::WriteFault::NO_REPLY_APPLIED;
    run_boost(b, x.glue, 0, BOOT);
    x.glue.start(10, ha_now(BOOT), BOOT);
    run_boost(b, x.glue, BOOT, MIN);
    CHECK(x.glue.seq.state() == BoostState::ACTIVE);
    run_boost(b, x.glue, BOOT + MIN, 25 * MIN);
    CHECK(x.glue.seq.idle());
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  }
}

TEST(sim_v05_boost_non_conforming_read_back) {
  // Le bloc relu après l'écriture n'est ni le temporaire ni l'origine :
  // état indéterminé, sauvegarde conservée, plus aucune écriture.
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  uint32_t t = BOOT;
  while (x.glue.seq.state() != BoostState::VERIFY_PENDING && t < BOOT + MIN)
    run_boost(b, x.glue, t, 5), t += 5;
  b.boiler.registers[0x2230] = {0x31, 0x43, 0x70, 0x71, 0xAC, 0xAD, 0xFF, 0xFF};
  run_boost(b, x.glue, t, H);
  CHECK(x.glue.seq.recovery_required());
  CHECK(x.glue.flash_has);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK(x.glue.start(10, ha_now(t + H), t + H) == "recovery");
}

TEST(sim_v05_boost_panel_change_never_overwritten) {
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  run_boost(b, x.glue, BOOT, MIN);
  const std::vector<uint8_t> edited = {0x31, 0x43, 0x70, 0x72, 0xA8, 0xAD, 0xFF, 0xFF};
  b.boiler.registers[0x2230] = edited;  // modification depuis le panneau
  run_boost(b, x.glue, BOOT + MIN, 30 * MIN);
  CHECK(x.glue.seq.recovery_required());
  CHECK(b.boiler.registers[0x2230] == edited);
  CHECK_EQ(b.boiler.write_frames, 1);
}

TEST(sim_v05_boost_power_cut_while_active_then_manual_restore) {
  for (BoostState cut_at : {BoostState::WRITE_PENDING, BoostState::VERIFY_PENDING, BoostState::ACTIVE,
                            BoostState::RESTORE_VERIFY}) {
    Bench b;
    BoostBench x(b);
    run_boost(b, x.glue, 0, BOOT);
    x.glue.start(10, ha_now(BOOT), BOOT);
    uint32_t t = BOOT;
    while (x.glue.seq.state() != cut_at && t < BOOT + 40 * MIN)
      run_boost(b, x.glue, t, 5), t += 5;
    CHECK(x.glue.seq.state() == cut_at);
    // Coupure : RAM de l'ESP perdue, flash et régulation conservées.
    b.reboot(t);
    x.glue.boot();
    b.boiler_only(t, 5 * S);
    const int frames = b.boiler.write_frames;
    run_boost(b, x.glue, t + 5 * S, H);
    CHECK_EQ(b.boiler.write_frames, frames);  // aucune écriture automatique au démarrage
    if (x.glue.flash_has) {
      CHECK(x.glue.seq.recovery_required());
      CHECK(x.glue.start(10, ha_now(t + H), t + H) == "recovery");  // tout nouveau déclenchement bloqué
      CHECK(x.glue.restore(t + H));
      run_boost(b, x.glue, t + H, 2 * MIN);
      CHECK(x.glue.seq.idle());
      CHECK(!x.glue.flash_has);
    }
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
    if (vltest::failures() > 0)
      std::printf("    coupure en %s\n", boost_state_to_string(cut_at));
  }
}

TEST(sim_v05_boost_and_a1_command_serialized) {
  // Bouclage et commande de mode A1 demandés presque ensemble : une seule
  // écriture à la fois, chacune suivie de sa relecture ; aucune régression
  // sur A1/M2.
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "accepted");
  run_boost(b, x.glue, BOOT, 3 * S);
  CHECK(b.request_mode(x.modes.a1, ECS_ONLY, BOOT + 3 * S) == "queued");
  run_boost(b, x.glue, BOOT + 3 * S, 2 * MIN);
  CHECK(x.glue.seq.state() == BoostState::ACTIVE);
  CHECK_EQ(b.mode_of(x.modes.a1), 0x00);
  CHECK(b.verify_results.back() == VerifyResult::CONFIRMED);
  const auto &log = b.boiler.frame_log;
  for (size_t i = 0; i + 1 < log.size(); i++) {
    if (log[i].first == 0xF4)
      CHECK(log[i + 1].first == 0xF7 && log[i + 1].second == log[i].second);  // relecture immédiate
  }
  CHECK_EQ(writes_to(b.boiler, 0x2301), 1);
  CHECK_EQ(writes_to(b.boiler, 0x3301), 0);
  run_boost(b, x.glue, BOOT + 3 * S + 2 * MIN, 25 * MIN);
  CHECK(x.glue.seq.idle());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(b.failures.empty());
}

TEST(sim_v071_boost_10_min_in_a_30_min_range_restored_after_10_min) {
  // 10 min demandées à 14:09:10 (heure Vitotronic) : la plage inscrite,
  // arrondie à la dizaine avec la marge, est 14:00-14:30 (30 min). La
  // restauration a lieu 10 min après la confirmation par relecture, vers
  // 14:19, pas à 14:30. Appuis répétés pendant la plage : refusés, minuterie
  // non prolongée. Pompe 0x0846 : régulation simulée (marche si l'heure est
  // dans une plage du bloc du jour), lue toutes les 15 s, jamais écrite.
  Bench b;
  BoostBench x(b);
  const size_t pump = b.add_binary(0x0846, 15 * S, 0);
  b.boiler.registers[0x0846] = {0x00};
  auto regulation = [&](uint32_t t) {  // la Vitotronic commande la pompe
    ScheduleSlots slots;
    bool on = false;
    if (decode_schedule_slots(b.boiler.registers[0x2230].data(), SCHEDULE_BLOCK_LENGTH, &slots) == ScheduleError::NONE) {
      const uint32_t minute = (CLOCK_BASE_S + t / 1000) / 60 % 1440;
      for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++)
        on |= slots.used[i] && minute >= slots.start[i] && minute < slots.end[i];
    }
    b.boiler.registers[0x0846] = {static_cast<uint8_t>(on ? 0x01 : 0x00)};
  };
  auto run = [&](uint32_t from, uint32_t duration) {  // pas exacts : jamais de retour en arrière
    for (uint32_t done = 0; done < duration;) {
      const uint32_t chunk = std::min<uint32_t>(S, duration - done);
      regulation(from + done);
      run_boost(b, x.glue, from + done, chunk);
      done += chunk;
    }
  };
  const uint32_t press = 9 * MIN + 10 * S;  // 14:09:10
  run(0, press);
  CHECK(b.last_binary[pump] == BinaryValue::OFF);
  CHECK(x.glue.start(10, ha_now(press), press) == "accepted");
  uint32_t t = press, confirmed = 0;
  while (confirmed == 0 && t < press + MIN) {
    run_boost(b, x.glue, t, 5);
    t += 5;
    if (x.glue.seq.state() == BoostState::ACTIVE)
      confirmed = t;
  }
  CHECK(confirmed != 0);
  CHECK_EQ(x.glue.seq.plan().start_min, 14 * 60);
  CHECK_EQ(x.glue.seq.plan().end_min, 14 * 60 + 30);  // plage de 30 min
  CHECK(b.boiler.registers[0x2230] == (std::vector<uint8_t>{0x31, 0x43, 0x70, 0x73, 0xAC, 0xAD, 0xFF, 0xFF}));
  CHECK(x.glue.seq.range_text() == "14:00-14:30 (dimanche)");
  CHECK_EQ(x.glue.seq.remaining_s(confirmed), 600u);
  // Appuis répétés à +1 min, +5 min, +9 min 50 s : refusés.
  uint32_t expired = 0;
  for (uint32_t at : {1 * MIN, 5 * MIN, 9 * MIN + 50 * S}) {
    run(t, confirmed + at - t);
    t = confirmed + at;
    CHECK(x.glue.start(10, ha_now(t), t) == "refused");
    CHECK(x.glue.start(30, ha_now(t), t) == "refused");
    CHECK_EQ(x.glue.seq.remaining_s(t), (10 * MIN - at) / 1000);
    CHECK(x.glue.seq.range_text() == "14:00-14:30 (dimanche)");
    if (at == 5 * MIN)
      CHECK(b.last_binary[pump] == BinaryValue::ON);  // la régulation fait tourner la pompe
  }
  CHECK_EQ(writes_to(b.boiler, 0x2230), 1);
  // Expiration : à 10 min de la confirmation, au pas de 5 ms près.
  while (expired == 0 && t < confirmed + 11 * MIN) {
    regulation(t);
    run_boost(b, x.glue, t, 5);
    t += 5;
    if (x.glue.seq.state() != BoostState::ACTIVE)
      expired = t;
  }
  CHECK(expired >= confirmed + 10 * MIN && expired <= confirmed + 10 * MIN + 10);
  run(t, MIN);
  t += MIN;
  CHECK(x.glue.seq.idle());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(x.glue.seq.status().rfind("Programmation restaurée (vérifiée par relecture), après 10 min 0", 0) == 0);
  CHECK(x.glue.seq.range_text() == "Aucune");
  CHECK_EQ(x.glue.seq.remaining_s(t), 0u);
  CHECK((CLOCK_BASE_S + t / 1000) / 60 % 1440 < 14 * 60 + 30);  // avant la fin de la plage
  CHECK(b.last_binary[pump] == BinaryValue::OFF);                 // pompe arrêtée par la régulation
  CHECK_EQ(writes_to(b.boiler, 0x2230), 2);
  CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
  CHECK_EQ(b.boiler.write_frames, 2);
  CHECK(x.glue.start(10, ha_now(t), t) == "refused");  // 15 min entre deux déclenchements
  CHECK(b.failures.empty());
}

TEST(sim_v05_boost_refused_on_unvalidated_day_or_bad_clock) {
  {  // Jour non validé : aucune sauvegarde, aucune écriture.
    Bench b;
    BoostBench x(b);
    x.glue.config.validated_days_mask = 1u << 0;  // lundi seulement
    x.glue.boot();
    run_boost(b, x.glue, 0, BOOT);
    x.glue.start(10, ha_now(BOOT), BOOT);
    run_boost(b, x.glue, BOOT, MIN);
    CHECK(x.glue.seq.idle());
    CHECK_EQ(x.glue.saves, 0);
    CHECK_EQ(b.boiler.write_frames, 0);
  }
  {  // Horloge Home Assistant décalée d'une heure (changement d'heure ?) : refus.
    Bench b;
    BoostBench x(b);
    run_boost(b, x.glue, 0, BOOT);
    HaTime ha = ha_now(BOOT);
    ha.minute_of_day += 60;
    x.glue.start(10, ha, BOOT);
    run_boost(b, x.glue, BOOT, MIN);
    CHECK(x.glue.seq.idle());
    CHECK_EQ(b.boiler.write_frames, 0);
  }
}

// --- v0.5.1 : rafraîchissement des programmations ------------------------------
//
// Configurations v0.5 réelles (ordre d'inscription du firmware = ordre du
// YAML) et mesures de délai de publication des programmations, à 2, 3 et
// 4 s de resynchro. VL_REPORT=1 affiche le tableau des mesures.

namespace {

struct V05Config {
  size_t outdoor{0}, burner{0}, clock{0}, circulation{0};
  std::vector<size_t> eco_party, schedules;

  /// `trial` : examples/iot-viessmann-essai-lecture-v0.5.yaml (0x0846 à 60 s,
  /// programmation du bouclage à 10 min) ; sinon examples/iot-viessmann.yaml.
  /// `eco_ms` : intervalle Éco/Party (60 s dans les exemples depuis la
  /// v0.5.1 ; 30 s pour reproduire les mesures de la v0.5).
  V05Config(Bench &b, bool trial, uint32_t eco_ms = 60 * S) {
    auto &r = b.boiler.registers;
    r[0x0842] = {0x01};
    for (uint16_t a : {0x2302, 0x2303, 0x3302, 0x3303, 0x0846})
      r[a] = {0x00};
    r[0x2306] = {20};
    r[0x2307] = {16};
    r[0x3306] = {21};
    r[0x3307] = {17};
    r[0x6300] = {50};
    r[0x2301] = {0x03};
    r[0x3301] = {0x03};
    load_real_schedules(b.boiler);
    set_vito_clock(b.boiler, 0);
    outdoor = b.add(0x0800, 60 * S, 0);
    b.add(0x0802, 60 * S, 0);
    b.add(0x0804, 60 * S, 0);
    b.add(0x3900, 60 * S, 0);
    b.add(0x088A, H, 0, DataType::UINT32, 1);
    b.add(0x08A7, H, 0, DataType::UINT32, 1);
    if (trial)
      circulation = b.add(0x0846, 60 * S, 0, DataType::UINT8, 1);
    burner = b.add_binary(0x0842, 30 * S, 0);
    for (uint16_t a : {0x2302, 0x2303, 0x3302, 0x3303})
      eco_party.push_back(b.add_binary(a, eco_ms, 0));
    b.add_number(0x2306, 16, 24, 5 * MIN, 0);
    b.add_number(0x2307, 10, 20, 5 * MIN, 0);
    b.add_number(0x3306, 16, 24, 5 * MIN, 0);
    b.add_number(0x3307, 10, 20, 5 * MIN, 0);
    b.add_number(0x6300, 40, 60, 5 * MIN, 0);
    b.add_mode(0x2301, 5 * MIN, 0);
    b.add_mode(0x3301, 5 * MIN, 0);
    for (uint16_t base : {A1, M2, DHW})
      schedules.push_back(b.add_schedule(base, 6 * H, 0));
    schedules.push_back(b.add_schedule(LOOP, trial ? 10 * MIN : 6 * H, 0));
    clock = b.add_raw(VITO_CLOCK_ADDRESS, 8, 10 * MIN, 0);
  }
};

struct RefreshMeasure {
  uint32_t today_ms{0};  ///< dernier des 4 circuits à publier le jour courant
  uint32_t week_ms{0};   ///< dernier des 28 blocs publié
  int burner{0};         ///< lectures du brûleur sur la durée
  uint32_t burner_gap{0}, eco_gap{0}, outdoor_gap{0};
  bool ok{true};
};

/// Démarrage à t = 0, horloge `time` valide dès `clock_valid_ms` (jour
/// `today`), mesure sur `duration`.
RefreshMeasure measure_refresh(bool trial, uint32_t post, uint32_t delay, bool today_first, int today,
                               uint32_t duration, uint32_t clock_valid_ms = 0) {
  Bench b;
  b.boiler.post_response_delay_ms = post;
  b.scheduler.set_block_delay(delay);
  b.today_first = today_first;
  V05Config c(b, trial, 30 * S);  // tableaux de docs/programmations-v0.5.1.md (Éco/Party v0.5)
  if (clock_valid_ms > 0)
    b.run(0, clock_valid_ms);
  b.today = today;
  b.run(clock_valid_ms, duration - clock_valid_ms);
  RefreshMeasure m;
  for (size_t i : c.schedules) {
    auto &days = b.day_published_ms[i];
    if (days.size() != SCHEDULE_DAYS || !days.count(static_cast<uint8_t>(today))) {
      m.ok = false;
      continue;
    }
    m.today_ms = std::max(m.today_ms, days[static_cast<uint8_t>(today)]);
    for (const auto &kv : days)
      m.week_ms = std::max(m.week_ms, kv.second);
  }
  // Une lecture peut être en cours à la fin de la mesure.
  m.ok &= b.failures.empty() && b.boiler.requests - b.total_successes() <= 1;
  m.burner = b.successes[c.burner];
  m.burner_gap = b.max_gap_ms[c.burner];
  m.outdoor_gap = b.max_gap_ms[c.outdoor];
  for (size_t i : c.eco_party)
    m.eco_gap = std::max(m.eco_gap, b.max_gap_ms[i]);
  return m;
}

bool report_enabled() { return std::getenv("VL_REPORT") != nullptr; }

}  // namespace

TEST(sim_v051_report_refresh_times) {
  if (!report_enabled())
    return;
  std::printf("\n  config  resync delai  mode        jour(Di)  jour(Lu)  semaine  brul/6h30 ecart_brul ecart_eco ecart_ext\n");
  for (bool trial : {false, true}) {
    for (uint32_t post : {2000u, 3000u, 4000u}) {
      for (uint32_t delay : {30000u, 20000u, 15000u, 10000u}) {
        for (bool tf : {false, true}) {
          const RefreshMeasure sun = measure_refresh(trial, post, delay, tf, 6, 6 * H + 30 * MIN);
          const RefreshMeasure mon = measure_refresh(trial, post, delay, tf, 0, 15 * MIN);
          std::printf("  %-7s %4.1fs %4us  %-10s %7.0fs %8.0fs %7.0fs %9d %9.1fs %8.1fs %8.1fs%s\n",
                      trial ? "essai" : "prod", post / 1000.0, static_cast<unsigned>(delay / 1000),
                      tf ? "jour-1er" : "v0.5", sun.today_ms / 1000.0, mon.today_ms / 1000.0, sun.week_ms / 1000.0,
                      sun.burner, sun.burner_gap / 1000.0, sun.eco_gap / 1000.0, sun.outdoor_gap / 1000.0,
                      sun.ok && mon.ok ? "" : "  (ECHEC)");
        }
      }
    }
  }
}

namespace {

uint32_t published(Bench &b, size_t reg, uint8_t day) {
  auto it = b.day_published_ms[reg].find(day);
  return it == b.day_published_ms[reg].end() ? UINT32_MAX : it->second;
}

int requests_to(const SimBoiler &boiler, uint16_t address) {
  auto it = boiler.requests_by_address.find(address);
  return it == boiler.requests_by_address.end() ? 0 : it->second;
}

}  // namespace

TEST(sim_v051_current_day_published_first_without_extra_load) {
  // Production v0.5, horloge valide dès le démarrage, dimanche (pire cas de
  // l'ordre lundi -> dimanche). Mesuré : 47 / 69 / 91 s au lieu de
  // 238 / 364 / 471 s. Même nombre de lectures, même cadence du brûleur.
  const uint32_t limit[] = {60 * S, 80 * S, 105 * S};
  int k = 0;
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    ref.today_first = false;
    V05Config rc(ref, false), c(b, false);
    ref.today = b.today = 6;
    ref.run(0, 6 * H + 30 * MIN);
    b.run(0, 6 * H + 30 * MIN);
    CHECK(b.failures.empty());
    for (size_t i : c.schedules) {
      const uint32_t today = published(b, i, 6);
      CHECK(today <= limit[k]);
      for (uint8_t d = 0; d < 6; d++)
        CHECK(published(b, i, d) > today);  // le jour courant avant les autres
      CHECK_EQ(b.successes[i], 14);            // deux cycles complets, aucun bloc en plus
      CHECK(published(ref, i, 6) > 3 * MIN);   // v0.5 : dimanche en dernier
    }
    // Charge identique (à la lecture en cours près).
    CHECK(std::abs(b.boiler.requests - ref.boiler.requests) <= 1);
    CHECK(std::abs(b.successes[c.burner] - ref.successes[rc.burner]) <= 1);
    CHECK(b.max_gap_ms[c.burner] <= ref.max_gap_ms[rc.burner] + 1 * S);
    if (vltest::failures() > 0)
      std::printf("    post=%u today %u ms (v0.5 %u ms) burner %d/%d\n", static_cast<unsigned>(post),
                  static_cast<unsigned>(published(b, c.schedules[0], 6)),
                  static_cast<unsigned>(published(ref, rc.schedules[0], 6)), b.successes[c.burner],
                  ref.successes[rc.burner]);
    k++;
  }
}

TEST(sim_v051_clock_valid_late_current_day_refreshed_once) {
  // L'heure n'arrive qu'à t = 120 s (premiers cycles commencés par lundi) :
  // le dimanche est relu hors cycle, une fois par programmation.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    V05Config c(b, false);
    b.run(0, 120 * S);
    for (size_t i : c.schedules)
      CHECK(!b.weeks[i].is_known(6));
    b.today = 6;
    b.run(120 * S, 15 * MIN);
    CHECK(b.failures.empty());
    for (size_t i : c.schedules) {
      // Quatre rafraîchissements en file, chacun derrière au plus une
      // lecture en cours : <= 5 transactions.
      CHECK(published(b, i, 6) <= 120 * S + 5 * (post + 1000));
      CHECK(b.weeks[i].all_known());
      CHECK_EQ(requests_to(b.boiler, schedule_day_address(b.regs[i].address, 6)), 2);  // relecture + cycle
      CHECK_EQ(b.successes[i], 8);
    }
  }
}

TEST(sim_v051_block_refresh_after_panel_change) {
  // Programmation modifiée sur le panneau (dimanche, bouclage) : une
  // relecture ponctuelle la publie en quelques secondes au lieu de 6 h.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    V05Config c(b, false);
    b.today = 6;
    b.run(0, 20 * MIN);
    const size_t loop = c.schedules[3];
    const uint32_t next_cycle = b.scheduler.entry(loop).due_ms;
    const int before = b.boiler.requests;
    b.boiler.registers[0x2230] = {0x31, 0x43, 0x70, 0x71, 0xAC, 0xAD, 0xFF, 0xFF};  // + 14:00-14:10
    CHECK(!b.request_block_refresh(0x2238, 20 * MIN));  // hors programmation
    CHECK(!b.request_block_refresh(0x0800, 20 * MIN));  // registre simple
    CHECK(b.request_block_refresh(0x2230, 20 * MIN));
    uint32_t t = 20 * MIN;
    while (format_day(b.weeks[loop].days[6]) != "06:10-08:30, 14:00-14:10, 21:40-21:50" && t < 21 * MIN) {
      b.run(t, 100);
      t += 100;
    }
    // Au plus la lecture en cours puis le bloc : 2 transactions.
    CHECK(t - 20 * MIN <= 2 * (post + 1000));
    CHECK(b.boiler.requests - before <= 2);
    CHECK_EQ(b.scheduler.entry(loop).due_ms, next_cycle);  // cycle hebdomadaire inchangé
    for (uint8_t d = 0; d < 6; d++)
      CHECK_EQ(requests_to(b.boiler, schedule_day_address(LOOP, d)), 1);
    if (vltest::failures() > 0)
      std::printf("    post=%u refresh %u ms\n", static_cast<unsigned>(post), static_cast<unsigned>(t - 20 * MIN));
  }
}

TEST(sim_v051_refresh_never_inside_write_read_back) {
  Bench b;
  b.boiler.post_response_delay_ms = 3000;
  Modes m(b, true, false, true);
  b.today = 6;
  b.run(0, BOOT + 5 * MIN);
  CHECK(b.request_mode(m.a1, OFF, BOOT + 5 * MIN) == "queued");
  CHECK(b.request_block_refresh(0x2230, BOOT + 5 * MIN));
  b.run(BOOT + 5 * MIN, 2 * MIN);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK(both_read_back_after_each_write(b.boiler));  // A1 puis M2, rien entre
  CHECK_EQ(requests_to(b.boiler, 0x2230), 2);        // cycle + relecture, après les vérifications
}

TEST(sim_v051_unreadable_current_day_is_not_read_in_a_loop) {
  Bench b;
  b.boiler.post_response_delay_ms = 3000;
  V05Config c(b, false);
  b.boiler.register_faults[schedule_day_address(A1, 6)] = SimBoiler::Fault::NO_RESPONSE;
  b.run(0, 120 * S);
  b.today = 6;
  b.run(120 * S, 30 * MIN);
  // 3 tentatives dans le cycle + 3 pour la relecture, puis plus rien avant
  // le cycle suivant (6 h).
  CHECK(requests_to(b.boiler, schedule_day_address(A1, 6)) <= 6);
  CHECK(!b.weeks[c.schedules[0]].is_known(6));
  CHECK(b.max_gap_ms[c.burner] < 60 * S);
}

TEST(sim_v051_invalid_raw_values_published_unknown) {
  // 0x2544 / 0x3544 lus FF FF sur la KW3 : « inconnu », pas -0,1 °C. Sans
  // liste, FF FF reste -0,1 °C (température extérieure légitime).
  Bench b;
  b.boiler.registers[0x0800] = {0xFF, 0xFF};
  b.boiler.registers[0x2544] = {0xFF, 0xFF};
  b.boiler.registers[0x3544] = {0xFF, 0xFF};
  const size_t outdoor = b.add(0x0800, 60 * S, 0);
  const size_t a1 = b.add(0x2544, 60 * S, 0);
  const size_t m2 = b.add(0x3544, 60 * S, 0);
  b.regs[a1].invalid.add(0xFFFF);
  b.regs[m2].invalid.add(0xFFFF);
  b.run(0, 2 * MIN);
  CHECK(b.failures.empty());
  CHECK_EQ(b.boiler.requests, b.total_successes());  // réponse valide : aucune nouvelle tentative
  CHECK_NEAR(b.last_value[outdoor], -0.1, 1e-9);
  CHECK(std::isnan(b.last_value[a1]));
  CHECK(std::isnan(b.last_value[m2]));
  b.boiler.registers[0x2544] = {0xC2, 0x01};  // 45,0 °C
  b.run(2 * MIN, 2 * MIN);
  CHECK_NEAR(b.last_value[a1], 45.0, 1e-9);
  CHECK(std::isnan(b.last_value[m2]));
}

TEST(sim_v051_report_refresh_latency) {
  if (!report_enabled())
    return;
  std::printf("\n  resync  heure_tardive(120s)->jour  relecture_ponctuelle  cycle_6h(jour-1er, 28 blocs)\n");
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench late;
    late.boiler.post_response_delay_ms = post;
    V05Config lc(late, false, 30 * S);  // mesures v0.5 (docs)
    late.run(0, 120 * S);
    late.today = 6;
    late.run(120 * S, 10 * MIN);
    uint32_t late_ms = 0;
    for (size_t i : lc.schedules)
      late_ms = std::max(late_ms, published(late, i, 6) - 120 * S);

    Bench b;
    b.boiler.post_response_delay_ms = post;
    V05Config c(b, false, 30 * S);
    b.today = 6;
    b.run(0, 20 * MIN);
    b.boiler.registers[0x2230] = {0x31, 0x43, 0x70, 0x71, 0xAC, 0xAD, 0xFF, 0xFF};
    b.request_block_refresh(0x2230, 20 * MIN);
    uint32_t t = 20 * MIN;
    while (format_day(b.weeks[c.schedules[3]].days[6]) != "06:10-08:30, 14:00-14:10, 21:40-21:50" && t < 21 * MIN) {
      b.run(t, 100);
      t += 100;
    }
    // Rafraîchissement à 6 h : durée entre le premier et le dernier bloc.
    b.schedule_reads.clear();
    b.run(21 * MIN, 6 * H + 15 * MIN - 21 * MIN);
    uint32_t first = UINT32_MAX, last = 0;
    for (const auto &r : b.schedule_reads) {
      if (r.first < 6 * H - 5 * MIN)
        continue;
      first = std::min(first, r.first);
      last = std::max(last, r.first);
    }
    std::printf("  %4.1fs  %24.1fs  %19.1fs  %22.0fs\n", post / 1000.0, late_ms / 1000.0, (t - 20 * MIN) / 1000.0,
                (last - first) / 1000.0);
  }
}

// --- v0.5.1 : bouclage ECS, actualisation de la programmation publiée --------

namespace {

std::string day_text(const std::vector<uint8_t> &block) {
  DaySchedule d;
  if (decode_schedule_day(block.data(), block.size(), &d) != ScheduleError::NONE)
    return "?";
  return format_day(d);
}

std::string day_text(const uint8_t *block) { return day_text(std::vector<uint8_t>(block, block + 8)); }

/// Trames F7/F4 vers `address` à partir de l'indice `from` de frame_log.
std::vector<uint8_t> frames_to(const SimBoiler &boiler, uint16_t address, size_t from) {
  std::vector<uint8_t> out;
  for (size_t i = from; i < boiler.frame_log.size(); i++)
    if (boiler.frame_log[i].second == address)
      out.push_back(boiler.frame_log[i].first);
  return out;
}

/// Chaque écriture de bloc est immédiatement suivie de SA relecture (rien
/// entre les deux, ni actualisation ni autre lecture).
bool block_writes_verified_next(const SimBoiler &boiler) {
  const auto &log = boiler.frame_log;
  for (size_t i = 0; i < log.size(); i++) {
    if (log[i].first != 0xF4 || log[i].second != 0x2230)
      continue;
    if (i + 1 >= log.size() || log[i + 1] != std::pair<uint8_t, uint16_t>{0xF7, 0x2230})
      return false;
  }
  return true;
}

void run_until(Bench &b, BoostGlue &g, uint32_t &t, BoostState state, uint32_t limit) {
  const uint32_t end = t + limit;
  while (g.seq.state() != state && t < end)
    run_boost(b, g, t, 5), t += 5;
}

}  // namespace

TEST(sim_v051_boost_display_follows_write_and_restore) {
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    b.today = 6;
    BoostBench x(b);
    const size_t loop = x.modes.base.circuits->loop;
    run_boost(b, x.glue, 0, 10 * MIN);
    CHECK(format_day(b.weeks[loop].days[6]) == day_text(LOOP_SUNDAY));
    const size_t from = b.boiler.frame_log.size();
    CHECK(x.glue.start(10, ha_now(10 * MIN), 10 * MIN) == "accepted");
    uint32_t t = 10 * MIN;
    run_until(b, x.glue, t, BoostState::ACTIVE, 2 * MIN);
    CHECK(x.glue.seq.state() == BoostState::ACTIVE);
    const std::string temporary = day_text(x.glue.seq.record().temporary);
    const uint32_t active_at = t;
    // Affichage actualisé en quelques secondes (au lieu de 6 h).
    while (format_day(b.weeks[loop].days[6]) != temporary && t < active_at + MIN)
      run_boost(b, x.glue, t, 5), t += 5;
    CHECK(format_day(b.weeks[loop].days[6]) == temporary);
    CHECK(t - active_at <= 2 * (post + 1000));
    // Restauration automatique à la fin de la durée demandée.
    run_until(b, x.glue, t, BoostState::IDLE, 40 * MIN);
    CHECK(x.glue.seq.status().rfind("Programmation restaurée (vérifiée par relecture)", 0) == 0);
    run_boost(b, x.glue, t, 30 * S);
    t += 30 * S;
    CHECK(format_day(b.weeks[loop].days[6]) == day_text(LOOP_SUNDAY));
    // 0x2230 : lecture, écriture, vérification, actualisation ; puis
    // relecture, restauration, vérification, actualisation. Rien d'autre.
    CHECK(frames_to(b.boiler, 0x2230, from) ==
          (std::vector<uint8_t>{0xF7, 0xF4, 0xF7, 0xF7, 0xF7, 0xF4, 0xF7, 0xF7}));
    CHECK(block_writes_verified_next(b.boiler));
    CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::SUBMITTED), 2);
    CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::CANCELLED), 0);
    CHECK_EQ(writes_to(b.boiler, 0x2230), 2);
    CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
    CHECK(b.failures.empty());
    if (vltest::failures() > 0)
      std::printf("    post=%u\n", static_cast<unsigned>(post));
  }
}

TEST(sim_v051_boost_early_restore_cancels_unserved_refresh) {
  // Restauration demandée dans le même passage que la confirmation : la
  // relecture d'affichage n'est jamais émise, la vérification reste seule.
  Bench b;
  b.today = 6;
  BoostBench x(b);
  run_boost(b, x.glue, 0, 10 * MIN);
  const size_t from = b.boiler.frame_log.size();
  x.glue.start(10, ha_now(10 * MIN), 10 * MIN);
  uint32_t t = 10 * MIN;
  x.glue.relay_enabled = false;  // pas de passage du relais entre les deux
  run_until(b, x.glue, t, BoostState::ACTIVE, 2 * MIN);
  x.glue.relay_enabled = true;
  x.glue.refresh(t);  // demandée...
  CHECK(x.glue.restore(t));  // ... puis restauration aussitôt
  run_boost(b, x.glue, t, 5);
  t += 5;
  CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::CANCELLED), 1);
  run_until(b, x.glue, t, BoostState::IDLE, 5 * MIN);
  run_boost(b, x.glue, t, 30 * S);
  CHECK(frames_to(b.boiler, 0x2230, from) ==
        (std::vector<uint8_t>{0xF7, 0xF4, 0xF7, 0xF7, 0xF4, 0xF7, 0xF7}));  // une seule actualisation, à la fin
  CHECK(block_writes_verified_next(b.boiler));
  CHECK(format_day(b.weeks[x.modes.base.circuits->loop].days[6]) == day_text(LOOP_SUNDAY));
}

TEST(sim_v051_boost_recovery_refreshes_display) {
  Bench b;
  b.today = 6;
  BoostBench x(b);
  run_boost(b, x.glue, 0, 10 * MIN);
  x.glue.start(10, ha_now(10 * MIN), 10 * MIN);
  uint32_t t = 10 * MIN;
  run_until(b, x.glue, t, BoostState::ACTIVE, 2 * MIN);
  b.reboot(t);  // coupure pendant la plage
  x.glue.boot();
  b.boiler_only(t, 5 * S);
  t += 5 * S;
  run_boost(b, x.glue, t, 5 * MIN);  // relu par le cycle : plage temporaire affichée
  t += 5 * MIN;
  const size_t loop = x.modes.base.circuits->loop;
  CHECK(x.glue.seq.recovery_required());
  CHECK(format_day(b.weeks[loop].days[6]) == day_text(LOOP_SUNDAY_BOOST.data()) ||
        format_day(b.weeks[loop].days[6]) == day_text(x.glue.flash.temporary));
  CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::SUBMITTED), 1);  // avant la coupure
  const size_t from = b.boiler.frame_log.size();
  CHECK(x.glue.restore(t));
  run_until(b, x.glue, t, BoostState::IDLE, 2 * MIN);
  run_boost(b, x.glue, t, 30 * S);
  CHECK(x.glue.seq.idle());
  CHECK(format_day(b.weeks[loop].days[6]) == day_text(LOOP_SUNDAY));
  CHECK(frames_to(b.boiler, 0x2230, from) == (std::vector<uint8_t>{0xF7, 0xF4, 0xF7, 0xF7}));
  CHECK(block_writes_verified_next(b.boiler));
  CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::SUBMITTED), 2);
}

TEST(sim_v051_boost_no_display_refresh_without_confirmed_change) {
  // Écriture ignorée par la régulation : la vérification relit l'origine,
  // rien n'a changé, aucune lecture de plus.
  Bench b;
  b.today = 6;
  BoostBench x(b);
  b.boiler.writable.erase(0x2230);
  run_boost(b, x.glue, 0, 10 * MIN);
  const size_t from = b.boiler.frame_log.size();
  x.glue.start(10, ha_now(10 * MIN), 10 * MIN);
  uint32_t t = 10 * MIN;
  run_boost(b, x.glue, t, 2 * MIN);
  CHECK(!x.glue.seq.active());
  CHECK(frames_to(b.boiler, 0x2230, from) == (std::vector<uint8_t>{0xF7, 0xF4, 0xF7}));
  CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::SUBMITTED), 0);
}

TEST(sim_v051_boost_refresh_waits_for_a1_command) {
  // Commande A1 déposée pendant la vérification du bloc : elle passe d'abord
  // (écriture puis relecture A1 et M2, rien entre), l'actualisation après.
  Bench b;
  b.today = 6;
  BoostBench x(b);
  run_boost(b, x.glue, 0, 10 * MIN);
  const size_t from = b.boiler.frame_log.size();
  x.glue.start(10, ha_now(10 * MIN), 10 * MIN);
  uint32_t t = 10 * MIN;
  run_until(b, x.glue, t, BoostState::VERIFY_PENDING, 2 * MIN);
  CHECK(b.request_mode(x.modes.a1, OFF, t) == "queued");
  run_boost(b, x.glue, t, 2 * MIN);
  t += 2 * MIN;
  const auto &log = b.boiler.frame_log;
  size_t a1_write = 0, refresh = 0;
  int reads_2230 = 0;
  for (size_t i = from; i < log.size(); i++) {
    if (log[i] == std::pair<uint8_t, uint16_t>{0xF4, 0x2301})
      a1_write = i;
    if (log[i] == std::pair<uint8_t, uint16_t>{0xF7, 0x2230} && ++reads_2230 == 3)
      refresh = i;
  }
  CHECK(a1_write > 0 && refresh > 0);
  CHECK(refresh > a1_write + 2);  // après l'écriture A1 et ses deux relectures
  std::set<uint16_t> next = {log[a1_write + 1].second, log[a1_write + 2].second};
  CHECK(next == (std::set<uint16_t>{0x2301, 0x3301}));
  CHECK(block_writes_verified_next(b.boiler));
  CHECK_EQ(b.boiler_value(0x2301), 0x05);
  CHECK_EQ(b.boiler_value(0x3301), 0x03);  // M2 jamais écrit
  CHECK_EQ(x.glue.relay_count(BoostRefreshRelay::Event::SUBMITTED), 1);
}

TEST(sim_v051_boost_without_loop_schedule_reads_nothing_more) {
  Bench b;
  Modes m(b, true, false, false);  // aucune programmation déclarée
  BoostGlue g(b, 1u << 6);
  b.enable_schedule_writes = true;
  b.boiler.writable.insert(0x2230);
  b.boiler.registers[0x2230] = LOOP_SUNDAY;
  set_vito_clock(b.boiler, 0);
  run_boost(b, g, 0, BOOT);
  const size_t from = b.boiler.frame_log.size();
  CHECK(g.start(10, ha_now(BOOT), BOOT) == "accepted");
  run_boost(b, g, BOOT, MIN);
  CHECK(g.seq.state() == BoostState::ACTIVE);
  CHECK(frames_to(b.boiler, 0x2230, from) == (std::vector<uint8_t>{0xF7, 0xF4, 0xF7}));
  CHECK_EQ(g.relay_count(BoostRefreshRelay::Event::NOT_DECLARED), 1);
  (void) m;
}

TEST(sim_v051_trial_config_eco_party_60s_load) {
  // Essai en lecture seule, Éco/Party à 60 s (v0.5.1) contre 30 s (v0.5).
  // Mesuré : 780/780 lectures du brûleur à 2 et 3 s ; à 4 s, 719 au lieu de
  // 593 (la liaison reste chargée : ~13 requêtes/min).
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b30, b60;
    b30.boiler.post_response_delay_ms = b60.boiler.post_response_delay_ms = post;
    V05Config c30(b30, true, 30 * S), c60(b60, true);
    b30.today = b60.today = 6;
    b30.run(0, 6 * H + 30 * MIN);
    b60.run(0, 6 * H + 30 * MIN);
    CHECK(b60.failures.empty());
    CHECK(b60.successes[c60.burner] >= b30.successes[c30.burner]);
    if (post <= 3000)
      CHECK(b60.successes[c60.burner] >= 778);  // 780 attendues en 6 h 30
    else
      CHECK(b60.successes[c60.burner] >= b30.successes[c30.burner] + 100);
    for (size_t i : c60.eco_party)
      CHECK(b60.max_gap_ms[i] <= 2 * MIN);  // jamais deux cycles perdus
    if (report_enabled() || vltest::failures() > 0)
      std::printf("    essai post=%u : brûleur %d (Éco/Party 30 s : %d), écart max %.1f s (%.1f s), "
                  "requêtes/min %.1f (%.1f)\n",
                  static_cast<unsigned>(post), b60.successes[c60.burner], b30.successes[c30.burner],
                  b60.max_gap_ms[c60.burner] / 1000.0, b30.max_gap_ms[c30.burner] / 1000.0,
                  b60.boiler.requests / 390.0, b30.boiler.requests / 390.0);
  }
}

// --- v0.5.2 : 10 min demandées = 10 min de plage, au niveau des trames KW ------

namespace {

/// Avance par pas de 100 ms jusqu'à `done` (au plus `limit`) ; renvoie l'instant.
template<typename F> uint32_t run_boost_until(Bench &b, BoostGlue &g, uint32_t t, uint32_t limit, F done) {
  const uint32_t end = t + limit;
  while (t < end && !done()) {
    run_boost(b, g, t, 100);
    t += 100;
  }
  return t;
}

}  // namespace

TEST(sim_v052_boost_restored_after_requested_duration) {
  // Durée mesurée sur la régulation simulée : de la plage temporaire
  // confirmée par relecture au bloc d'origine réécrit. v0.5 : fin de plage
  // 14:20 + 2 min ; v0.5.2 : 10 min après la confirmation (~14:12).
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    for (uint16_t d : BOOST_DURATIONS) {
      Bench b;
      b.boiler.post_response_delay_ms = post;
      BoostBench x(b);
      run_boost(b, x.glue, 0, BOOT);
      CHECK(x.glue.start(d, ha_now(BOOT), BOOT) == "accepted");
      const uint32_t confirmed =
          run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
      CHECK(x.glue.seq.state() == BoostState::ACTIVE);
      const std::vector<uint8_t> temporary = b.boiler.registers[0x2230];
      CHECK(temporary != LOOP_SUNDAY);
      // Jamais avant la durée demandée.
      run_boost(b, x.glue, confirmed, d * MIN - S);
      CHECK(b.boiler.registers[0x2230] == temporary);
      CHECK_EQ(writes_to(b.boiler, 0x2230), 1);
      // Bloc d'origine réécrit peu après l'expiration (relecture + écriture).
      const uint32_t restored = run_boost_until(b, x.glue, confirmed + d * MIN - S, MIN,
                                                [&] { return b.boiler.registers[0x2230] == LOOP_SUNDAY; });
      CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
      const uint32_t lasted = restored - confirmed;
      CHECK(lasted >= d * MIN);
      CHECK(lasted <= d * MIN + 3 * post + 2 * S);  // à la précision des échanges KW près
      // Toujours avant la fin de la plage programmée (horloge Vitotronic).
      CHECK(CLOCK_BASE_S + restored / 1000 < x.glue.seq.record().end_min * 60u);
      run_boost(b, x.glue, restored, MIN);
      CHECK(x.glue.seq.idle());
      CHECK(!x.glue.flash_has);
      CHECK_EQ(writes_to(b.boiler, 0x2230), 2);
      CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
      CHECK(b.failures.empty());
      if (report_enabled())
        std::printf("  resynchro %u s, %2u min : plage %.1f s (confirmée %.1f s, restaurée %.1f s)\n",
                    static_cast<unsigned>(post / 1000), static_cast<unsigned>(d), lasted / 1000.0,
                    confirmed / 1000.0, restored / 1000.0);
      if (vltest::failures() > 0)
        std::printf("    post=%u durée=%u lasted=%u\n", static_cast<unsigned>(post), static_cast<unsigned>(d),
                    static_cast<unsigned>(lasted));
    }
  }
}

TEST(sim_v052_boost_link_lost_at_expiry_then_restored) {
  // Interface débranchée de 9 à 14 min (4 min après l'expiration, dans la
  // fenêtre automatique de 15 min) : restauration en cours, pas d'alerte,
  // aucune écriture à l'aveugle ; restauration après le rebranchement,
  // précédée d'une relecture.
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  const uint32_t confirmed =
      run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
  run_boost(b, x.glue, confirmed, 9 * MIN);
  b.boiler.connected = false;
  run_boost(b, x.glue, confirmed + 9 * MIN, 5 * MIN);
  CHECK(x.glue.seq.restoring());
  CHECK(!x.glue.seq.restore_alert());
  CHECK(!x.glue.seq.recovery_required());
  CHECK_EQ(writes_to(b.boiler, 0x2230), 1);
  CHECK(x.glue.flash_has);
  b.boiler.connected = true;
  const uint32_t back = confirmed + 14 * MIN;
  const uint32_t restored =
      run_boost_until(b, x.glue, back, 2 * MIN, [&] { return b.boiler.registers[0x2230] == LOOP_SUNDAY; });
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(restored - back <= 45 * S);
  run_boost(b, x.glue, restored, MIN);
  CHECK(x.glue.seq.idle());
  CHECK(!x.glue.seq.restore_alert());
  CHECK(!x.glue.seq.restoring());
  CHECK(!x.glue.flash_has);
  CHECK_EQ(writes_to(b.boiler, 0x2230), 2);
  CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
}

TEST(sim_v052_boost_link_lost_beyond_window_requires_manual_recovery) {
  // Interface débranchée de 9 à 30 min : relectures pendant 15 min après
  // l'expiration, puis récupération manuelle. Rebranchée ensuite : aucune
  // écriture automatique ; le bouton « Restaurer » rétablit le bloc.
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  const uint32_t confirmed =
      run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
  run_boost(b, x.glue, confirmed, 9 * MIN);
  b.boiler.connected = false;
  const uint32_t recovery = run_boost_until(b, x.glue, confirmed + 9 * MIN, 30 * MIN,
                                            [&] { return x.glue.seq.recovery_required(); });
  CHECK(x.glue.seq.recovery_required());
  // Fin de la durée + 15 min, à la durée d'une relecture KW près.
  CHECK(recovery >= confirmed + 25 * MIN);
  CHECK(recovery <= confirmed + 25 * MIN + 30 * S);
  CHECK(x.glue.flash_has);
  b.boiler.connected = true;
  run_boost(b, x.glue, recovery, H);
  CHECK_EQ(writes_to(b.boiler, 0x2230), 1);
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY_BOOST);
  CHECK(x.glue.seq.restore_alert());
  CHECK(x.glue.restore(recovery + H));
  run_boost(b, x.glue, recovery + H, 2 * MIN);
  CHECK(x.glue.seq.idle());
  CHECK(!x.glue.seq.restore_alert());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK(!x.glue.flash_has);
  CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
}

TEST(sim_v052_boost_no_alert_during_normal_restore) {
  // Resynchro lente (4 s) et commande A1 au moment de l'expiration : la
  // restauration attend le bus, sans jamais lever l'alerte.
  Bench b;
  b.boiler.post_response_delay_ms = 4000;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  const uint32_t confirmed =
      run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
  run_boost(b, x.glue, confirmed, 10 * MIN - 2 * S);
  CHECK(b.request_mode(x.modes.a1, ECS_ONLY, confirmed + 10 * MIN - 2 * S) == "queued");
  bool alert = false, restoring = false;
  uint32_t t = confirmed + 10 * MIN - 2 * S;
  while (t < confirmed + 12 * MIN) {
    run_boost(b, x.glue, t, 100);
    t += 100;
    alert |= x.glue.seq.restore_alert();
    restoring |= x.glue.seq.restoring();
  }
  CHECK(restoring);
  CHECK(!alert);
  CHECK(x.glue.seq.idle());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
  CHECK_EQ(writes_to(b.boiler, 0x2301), 1);
  CHECK(b.failures.empty());
}

TEST(sim_v052_boost_reboot_during_restore_window_writes_nothing) {
  Bench b;
  BoostBench x(b);
  run_boost(b, x.glue, 0, BOOT);
  x.glue.start(10, ha_now(BOOT), BOOT);
  const uint32_t confirmed =
      run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
  run_boost(b, x.glue, confirmed, 9 * MIN);
  b.boiler.connected = false;
  run_boost(b, x.glue, confirmed + 9 * MIN, 4 * MIN);
  CHECK(x.glue.seq.restoring());
  const uint32_t t = confirmed + 13 * MIN;
  b.reboot(t);
  x.glue.boot();
  b.boiler.connected = true;
  b.boiler_only(t, 5 * S);
  run_boost(b, x.glue, t + 5 * S, H);
  CHECK(x.glue.seq.recovery_required());
  CHECK(x.glue.seq.restore_alert());
  CHECK_EQ(writes_to(b.boiler, 0x2230), 1);  // aucune écriture automatique
  CHECK(x.glue.flash_has);
}

// --- v0.6 : consigne d'eau chaude sanitaire -----------------------------------

namespace {

/// Production v0.6 : consignes v0.3 (ECS 40..60, lue toutes les 5 min).
struct Dhw {
  V03 v;
  Dhw(Bench &b, bool writes = true, bool dhw_write = true) : v(b, false, writes) {
    b.enable_dhw_setpoint_write = dhw_write;
  }
  size_t number() const { return v.dhw; }
};

/// Pas de 5 ms jusqu'à `done` (au plus `limit`) ; renvoie l'instant atteint.
template<typename F> uint32_t run_until(Bench &b, uint32_t t, uint32_t limit, F done) {
  const uint32_t end = t + limit;
  while (t < end && !done()) {
    b.boiler.tick(t);
    b.step(t);
    t += 5;
  }
  return t;
}

/// Index de la première trame (fonction, adresse) à partir de `from`.
size_t find_frame(const SimBoiler &boiler, uint8_t fn, uint16_t address, size_t from = 0) {
  for (size_t i = from; i < boiler.frame_log.size(); i++) {
    if (boiler.frame_log[i] == std::pair<uint8_t, uint16_t>{fn, address})
      return i;
  }
  return SIZE_MAX;
}

/// Chaque trame d'écriture est suivie immédiatement de la relecture de la
/// même adresse.
bool read_back_follows_each_write(const SimBoiler &boiler) {
  const auto &log = boiler.frame_log;
  for (size_t i = 0; i < log.size(); i++) {
    if (log[i].first == 0xF4 && (i + 1 >= log.size() || log[i + 1] != std::pair<uint8_t, uint16_t>{0xF7, log[i].second}))
      return false;
  }
  return true;
}

}  // namespace

TEST(sim_v06_dhw_read_and_refused_without_both_permissions) {
  struct Case {
    bool writes, dhw, others;
    const char *reason;
  };
  for (const Case &c : {Case{false, false, false, "refused: writes are disabled (enable_writes: false)"},
                        Case{true, false, false,
                             "refused: DHW setpoint write is disabled (enable_dhw_setpoint_write: false)"},
                        // Les autres autorisations ne permettent jamais 0x6300.
                        Case{true, false, true,
                             "refused: DHW setpoint write is disabled (enable_dhw_setpoint_write: false)"}}) {
    Bench b;
    Dhw d(b, c.writes, c.dhw);
    b.enable_mode_writes = b.enable_m2_mode_write = b.enable_schedule_writes = c.others;
    b.run(0, BOOT);
    CHECK_EQ(b.number_state[d.number()], 50);  // lecture valide de 0x6300
    CHECK(b.request(d.number(), 55, BOOT) == c.reason);
    b.run(BOOT, 10 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK_EQ(writes_to(b.boiler, 0x6300), 0);
    CHECK_EQ(b.boiler_value(0x6300), 50);
    CHECK_EQ(b.number_state[d.number()], 50);
    CHECK(!b.dhw.busy());
  }
}

TEST(sim_v06_dhw_write_confirmed_after_fresh_read) {
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    Dhw d(b);
    b.run(0, BOOT);
    const size_t before = b.boiler.frame_log.size();
    CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
    CHECK_EQ(b.number_state[d.number()], 50);  // rien n'est publié avant la relecture
    const uint32_t done = run_until(b, BOOT, MIN, [&] { return !b.dhw.busy(); });
    CHECK(!b.dhw.busy());
    CHECK(b.dhw.last_outcome() == DhwOutcome::CONFIRMED);
    // Relecture préalable, puis écriture, puis relecture de vérification.
    const size_t pre = find_frame(b.boiler, 0xF7, 0x6300, before);
    const size_t write = find_frame(b.boiler, 0xF4, 0x6300, before);
    CHECK(pre < write && write != SIZE_MAX);
    CHECK(read_back_follows_each_write(b.boiler));
    CHECK_EQ(b.boiler.write_frames, 1);
    CHECK(b.boiler.write_log[0] == (std::vector<uint8_t>{0x01, 0xF4, 0x63, 0x00, 0x01, 0x37}));
    CHECK(b.write_results == std::vector<KwResult>{KwResult::SUCCESS});
    CHECK(b.verify_results == std::vector<VerifyResult>{VerifyResult::CONFIRMED});
    CHECK_EQ(b.boiler_value(0x6300), 55);
    CHECK_EQ(b.number_state[d.number()], 55);
    CHECK(b.dhw.status() == "Confirmée : 55 °C relue (précédente 50 °C, accusé reçu)");
    CHECK(done - BOOT <= 3 * post + 6 * S);  // demande -> confirmation : quelques transactions
    CHECK(b.failures.empty());
    if (report_enabled())
      std::printf("  resynchro %u s : demande -> confirmation %.1f s\n", static_cast<unsigned>(post / 1000),
                  (done - BOOT) / 1000.0);
  }
}

TEST(sim_v06_dhw_unchanged_and_panel_changes) {
  Bench b;
  Dhw d(b);
  b.run(0, BOOT);
  // Panneau : 55 °C, l'entité affiche encore 50 (lecture toutes les 5 min).
  b.boiler.registers[0x6300] = {55};
  CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
  uint32_t t = run_until(b, BOOT, MIN, [&] { return !b.dhw.busy(); });
  CHECK(b.dhw.last_outcome() == DhwOutcome::UNCHANGED);  // déjà 55 : rien n'est écrit
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(b.number_state[d.number()], 55);  // affichage synchronisé par la relecture
  // Panneau : 53 °C ; demande 55 (égale à l'affichage périmé) : la relecture
  // révèle 53, l'écriture a lieu.
  b.boiler.registers[0x6300] = {53};
  CHECK(b.request(d.number(), 55, t) == "pre-read");
  t = run_until(b, t, MIN, [&] { return !b.dhw.busy(); });
  CHECK(b.dhw.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK_EQ(b.boiler_value(0x6300), 55);
  // Panneau : 52 °C, sans demande : la lecture périodique la publie.
  b.boiler.registers[0x6300] = {52};
  b.run(t, 6 * MIN);
  CHECK_EQ(b.number_state[d.number()], 52);
  CHECK(b.dhw.panel_changes() >= 1);
  CHECK(b.dhw.status() == "Modifiée hors de Home Assistant (panneau ?) : 55 -> 52 °C");
  CHECK_EQ(b.boiler.write_frames, 1);  // jamais corrigée automatiquement
}

TEST(sim_v06_dhw_out_of_range_and_repeated_requests) {
  Bench b;
  Dhw d(b);
  b.run(0, BOOT);
  CHECK(b.request(d.number(), 45, BOOT) == "refused: DHW setpoint outside the internal safety limits (50..60)");
  CHECK(b.request(d.number(), 61, BOOT) == "refused: value outside the configured range");
  CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
  CHECK(b.request(d.number(), 56, BOOT + 100) == "refused: another DHW setpoint change is in progress");
  CHECK(b.request(d.number(), 57, BOOT + 200) == "refused: another DHW setpoint change is in progress");
  uint32_t t = run_until(b, BOOT, MIN, [&] { return !b.dhw.busy(); });
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK_EQ(b.boiler_value(0x6300), 55);
  CHECK(b.request(d.number(), 56, t) == "refused: too soon after the previous write");
  b.run(t, DhwSetpointWriter::MIN_INTERVAL_MS);
  t += DhwSetpointWriter::MIN_INTERVAL_MS;
  CHECK(b.request(d.number(), 56, t) == "pre-read");
  t = run_until(b, t, MIN, [&] { return !b.dhw.busy(); });
  b.run(t, 20 * MIN);
  CHECK_EQ(b.boiler.write_frames, 2);  // 55 puis 56, rien d'autre
  CHECK_EQ(b.boiler.write_log[1][5], 56);
  CHECK_EQ(b.number_state[d.number()], 56);
}

TEST(sim_v06_dhw_negative_ack_or_clamped_value_never_retried) {
  {
    Bench b;
    Dhw d(b);
    b.boiler.write_faults[0x6300] = SimBoiler::WriteFault::NACK_OTHER;
    b.run(0, BOOT);
    CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
    b.run(BOOT, 20 * MIN);
    CHECK(b.write_results == std::vector<KwResult>{KwResult::WRITE_REJECTED});
    CHECK(b.dhw.last_outcome() == DhwOutcome::MISMATCH);
    CHECK(b.dhw.status() == "Échec : relu 50 °C au lieu de 55 °C (accusé négatif) ; pas de nouvel essai");
    CHECK_EQ(b.number_state[d.number()], 50);
    CHECK_EQ(b.boiler.write_frames, 1);
  }
  {
    Bench b;
    Dhw d(b);
    b.boiler.write_clamp[0x6300] = {10, 55};  // la régulation borne à 55
    b.run(0, BOOT);
    CHECK(b.request(d.number(), 58, BOOT) == "pre-read");
    b.run(BOOT, 20 * MIN);
    CHECK(b.dhw.last_outcome() == DhwOutcome::MISMATCH);
    CHECK(b.dhw.status() == "Échec : relu 55 °C au lieu de 58 °C (accusé reçu) ; pas de nouvel essai");
    CHECK_EQ(b.number_state[d.number()], 55);  // la valeur relue, jamais la valeur demandée
    CHECK_EQ(b.boiler.write_frames, 1);
  }
}

TEST(sim_v06_dhw_link_lost_during_operation) {
  {  // Interface débranchée au moment d'émettre : rien n'est émis, issue
     // indéterminée, dernière valeur confirmée affichée, aucune réémission.
    Bench b;
    Dhw d(b);
    b.run(0, BOOT);
    CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
    uint32_t t = run_until(b, BOOT, MIN, [&] { return b.write_in_flight; });
    CHECK(b.write_in_flight);
    b.boiler.connected = false;
    t = run_until(b, t, 2 * MIN, [&] { return !b.dhw.busy(); });
    CHECK(b.dhw.last_outcome() == DhwOutcome::INDETERMINATE);
    CHECK(b.dhw.status() ==
          "Indéterminé : relecture impossible après l'écriture de 55 °C (trame non émise) ; dernière valeur "
          "confirmée 50 °C");
    CHECK_EQ(b.number_state[d.number()], 50);
    b.boiler.connected = true;
    b.run(t, 20 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK_EQ(b.boiler_value(0x6300), 50);
    CHECK_EQ(b.number_state[d.number()], 50);
  }
  {  // Liaison perdue pendant la relecture préalable : rien n'est écrit.
    Bench b;
    Dhw d(b);
    b.run(0, BOOT);
    b.boiler.connected = false;
    CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
    uint32_t t = run_until(b, BOOT, 3 * MIN, [&] { return !b.dhw.busy(); });
    CHECK(!b.dhw.busy());
    CHECK(b.dhw.last_outcome() == DhwOutcome::DROPPED);
    b.boiler.connected = true;
    b.run(t, 20 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK_EQ(b.boiler_value(0x6300), 50);
  }
}

TEST(sim_v06_dhw_reboot_during_operation_writes_nothing) {
  enum Cut { PRE_READ, QUEUED, IN_FLIGHT };
  for (Cut cut : {PRE_READ, QUEUED, IN_FLIGHT}) {
    Bench b;
    Dhw d(b);
    b.run(0, BOOT);
    CHECK(b.request(d.number(), 55, BOOT) == "pre-read");
    uint32_t t = BOOT;
    if (cut == QUEUED)
      t = run_until(b, t, MIN, [&] { return b.dhw.state() == DhwState::WRITING && !b.write_in_flight; });
    if (cut == IN_FLIGHT)
      t = run_until(b, t, MIN, [&] { return b.write_in_flight; });
    const int frames = b.boiler.write_frames;
    b.reboot(t);
    b.boiler_only(t, 5 * S);
    b.run(t + 5 * S, 30 * MIN);
    CHECK(!b.dhw.busy());
    CHECK_EQ(b.boiler.write_frames, frames);  // aucune écriture après le redémarrage
    // Valeur affichée = valeur lue : 50, ou 55 si la trame était partie.
    CHECK_EQ(b.number_state[d.number()], b.boiler_value(0x6300));
    if (vltest::failures() > 0)
      std::printf("    coupure %d\n", static_cast<int>(cut));
  }
}

TEST(sim_v06_dhw_waits_for_circulation_restore) {
  // Consigne ECS demandée autour de l'expiration du bouclage : jamais
  // d'écriture de consigne pendant une restauration ; les deux opérations
  // aboutissent, chaque écriture suivie de sa relecture.
  // `a1_before_ms` : programme A1 demandé juste avant ; l'espacement de 10 s
  // qui suit son écriture retient la consigne ECS dans l'arbitre au moment
  // où la restauration dépose son bloc (le cas que la priorité protège).
  auto scenario = [](int32_t offset_ms, bool priority, uint32_t a1_before_ms = 0) {
    Bench b;
    BoostBench x(b);
    const size_t dhw = x.modes.base.dhw;
    b.enable_dhw_setpoint_write = true;
    if (priority)
      b.restore_priority = [&x] { return boost_restore_priority(x.glue.seq); };
    b.restore_observer = [&x] { return boost_restore_priority(x.glue.seq); };
    run_boost(b, x.glue, 0, BOOT);
    CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "accepted");
    const uint32_t confirmed =
        run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
    const uint32_t ask = confirmed + 10 * MIN + offset_ms;
    if (a1_before_ms != 0) {
      run_boost(b, x.glue, confirmed, ask - a1_before_ms - confirmed);
      // v0.7 : commande de programme A1 (une consigne relèverait de la garde
      // commune des consignes) ; seule sert l'espacement qui suit l'écriture.
      CHECK(b.request_mode(x.modes.a1, ECS_ONLY, ask - a1_before_ms) == "queued");
      run_boost(b, x.glue, ask - a1_before_ms, a1_before_ms);
    } else {
      run_boost(b, x.glue, confirmed, ask - confirmed);
    }
    CHECK(b.request(dhw, 55, ask) == "pre-read");
    run_boost(b, x.glue, ask, 3 * MIN);
    CHECK(x.glue.seq.idle());
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
    CHECK(!x.glue.flash_has);
    CHECK_EQ(b.boiler_value(0x6300), 55);
    CHECK(b.dhw.last_outcome() == DhwOutcome::CONFIRMED);
    CHECK(read_back_follows_each_write(b.boiler));
    CHECK_EQ(writes_to(b.boiler, 0x0846), 0);
    return b.dhw_started_in_restore;
  };
  int without = 0;
  for (int32_t offset : {-20000, -6000, -3000, -1000, 0, 2000}) {
    for (uint32_t a1 : {0u, 4000u, 8000u, 12000u}) {
      CHECK_EQ(scenario(offset, true, a1), 0);
      without += scenario(offset, false, a1);
    }
  }
  // Contrôle : sans la priorité, la consigne passe au moins une fois pendant
  // une restauration (le test détecte bien le défaut).
  CHECK(without > 0);
  if (report_enabled())
    std::printf("  sans priorité : %d cas d'écriture ECS pendant une restauration\n", without);
}

TEST(sim_v06_dhw_realistic_use_keeps_periodic_reads) {
  // Trois modifications par heure pendant 6 h 30 (programmations comprises) :
  // lecture du brûleur comparée à la même installation sans demande.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench ref, b;
    ref.boiler.post_response_delay_ms = b.boiler.post_response_delay_ms = post;
    V03 r(ref);
    V03 v(b);
    b.enable_dhw_setpoint_write = true;
    ref.run(0, BOOT);
    b.run(0, BOOT);
    const uint32_t end = 6 * H + 30 * MIN;
    int k = 0;
    for (uint32_t t = BOOT; t < end; t += 20 * MIN, k++) {
      b.request(v.dhw, k % 2 ? 50 : 55, t);
      ref.run(t, 20 * MIN);
      b.run(t, 20 * MIN);
    }
    CHECK(b.failures.empty());
    CHECK_EQ(b.boiler.write_frames, k);
    const int lost = ref.successes[r.burner] - b.successes[v.burner];
    CHECK(lost <= 2);
    if (report_enabled() || vltest::failures() > 0)
      std::printf("  resynchro %u s : %d écritures ECS, brûleur %d/%d, écart max %.1f s (réf. %.1f s)\n",
                  static_cast<unsigned>(post / 1000), b.boiler.write_frames, b.successes[v.burner],
                  ref.successes[r.burner], b.max_gap_ms[v.burner] / 1000.0, ref.max_gap_ms[r.burner] / 1000.0);
  }
}

// --- v0.7 : consignes de chauffage A1/M2 et climate du plancher M2 ------------

namespace {

/// Production v0.7 : consignes v0.3, programmes A1/M2, ambiance Vitotrol
/// (0x0898) et circulateur M2 (0x3906) pour le climate.
struct Heat {
  V03 v;
  size_t a1_mode, m2_mode, room, pump;
  Heat(Bench &b, bool writes = true, bool heating = true) : v(b, false, writes) {
    b.enable_heating_setpoint_writes = heating;
    b.boiler.registers[0x2301] = {0x03};
    b.boiler.registers[0x3301] = {0x03};
    b.boiler.registers[0x0898] = {0xCC, 0x00};  // 20,4 °C
    b.boiler.registers[0x3906] = {0x01};
    b.boiler.writable.insert(0x2301);
    b.boiler.writable.insert(0x3301);
    a1_mode = b.add_mode(0x2301, 5 * MIN, 0);
    m2_mode = b.add_mode(0x3301, 5 * MIN, 0);
    room = b.add(0x0898, 60 * S, 0);
    pump = b.add_binary(0x3906, 60 * S, 0);
  }
};

/// Même logique que VissLinkCircuitClimate (climate M2) : état calculé à
/// partir des entités lues, commandes transmises aux chemins sécurisés.
struct ClimateGlue {
  Bench &b;
  const Heat &h;
  CircuitClimateState state;
  int publishes{0};
  bool with_burner{true};  ///< v0.7.3 : burner_id déclaré
  uint32_t now{0};          ///< heure du dernier refresh (millis())
  ClimateGlue(Bench &bench, const Heat &heat) : b(bench), h(heat) {}
  /// VissLinkRegister::read_fresh (v0.7.3).
  bool fresh(size_t index) const {
    const auto it = b.last_success_ms.find(index);
    return reading_fresh(it != b.last_success_ms.end(), it == b.last_success_ms.end() ? 0 : it->second,
                         b.params[index].first, now);
  }
  int fresh_mode() const { return fresh(h.m2_mode) ? b.mode_of(h.m2_mode) : -1; }
  int fresh_binary(size_t index) const {
    const auto it = b.last_binary.find(index);
    if (!fresh(index) || it == b.last_binary.end() || it->second == BinaryValue::INVALID)
      return -1;
    return it->second == BinaryValue::ON ? 1 : 0;
  }
  int comfort() const { return b.has_read_value(h.v.m2_comfort) ? static_cast<int>(b.number_state.at(h.v.m2_comfort)) : -1; }
  /// VissLinkCircuitClimate::refresh_ (toutes les secondes).
  void refresh(bool force = false) {
    const auto room_it = b.last_value.find(h.room);
    const float room = room_it == b.last_value.end() ? NAN : static_cast<float>(room_it->second);
    const CircuitClimateState next = circuit_climate_state(state, room, comfort(), fresh_mode(), fresh_binary(h.pump),
                                                           with_burner ? fresh_binary(h.v.burner) : -1);
    if (!next.ready || (!force && next.same_as(state)))
      return;
    state = next;
    publishes++;
  }
  /// VissLinkCircuitClimate::control, consigne.
  std::string set_temperature(float value, uint32_t now) {
    uint8_t pending_target = 0;
    SetpointWriter *w = b.writer_for(static_cast<int>(h.v.m2_comfort));
    const bool pending = w->busy();
    pending_target = w->target();
    std::string r;
    switch (classify_temperature_command(value, comfort(), pending, pending_target)) {
      case TemperatureCommand::FORWARD:
        r = b.request(h.v.m2_comfort, value, now);
        break;
      case TemperatureCommand::UNCHANGED:
        r = "unchanged";
        break;
      case TemperatureCommand::PENDING:
        r = "pending";
        break;
      case TemperatureCommand::INVALID:
        r = "invalid";
        break;
    }
    refresh(true);
    return r;
  }
  /// VissLinkCircuitClimate::control, mode.
  std::string set_hvac(bool heat, uint32_t now) {
    std::string r = "unchanged";
    if (hvac_command_needed(heat ? CircuitHvac::HEAT : CircuitHvac::OFF, circuit_hvac_from_mode(b.mode_of(h.m2_mode))))
      r = b.request_mode(h.m2_mode, mode_option_for_hvac(heat), now);
    refresh(true);
    return r;
  }
  /// Pas de 100 ms avec rafraîchissement du climate chaque seconde.
  void run(uint32_t from, uint32_t duration) {
    for (uint32_t t = from; t - from < duration; t += 100) {
      b.run(t, 100);
      now = t + 100;
      if (t % 1000 == 0)
        refresh();
    }
  }
};

constexpr uint16_t HEATING_ADDRESSES[] = {0x2306, 0x2307, 0x3306, 0x3307};

}  // namespace

TEST(sim_v07_heating_setpoints_read_and_refused_without_permission) {
  struct Case {
    bool writes, heating, others;
    const char *reason;
  };
  for (const Case &c : {Case{false, false, false, "refused: writes are disabled (enable_writes: false)"},
                        Case{true, false, false,
                             "refused: heating setpoint writes are disabled (enable_heating_setpoint_writes: false)"},
                        // ECS, programmes, M2, programmations : aucune ne la permet.
                        Case{true, false, true,
                             "refused: heating setpoint writes are disabled (enable_heating_setpoint_writes: false)"}}) {
    Bench b;
    Heat h(b, c.writes, c.heating);
    b.enable_dhw_setpoint_write = b.enable_mode_writes = b.enable_m2_mode_write = b.enable_schedule_writes = c.others;
    b.run(0, BOOT);
    const size_t numbers[] = {h.v.a1_comfort, h.v.a1_reduced, h.v.m2_comfort, h.v.m2_reduced};
    const int read[] = {20, 16, 21, 17};
    for (size_t i = 0; i < 4; i++) {
      CHECK_EQ(b.number_state[numbers[i]], read[i]);  // lecture de chaque consigne
      CHECK(b.request(numbers[i], read[i] + 1 > 20 ? read[i] - 1 : read[i] + 1, BOOT) == c.reason);
    }
    b.run(BOOT, 10 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    for (uint16_t a : HEATING_ADDRESSES)
      CHECK_EQ(writes_to(b.boiler, a), 0);
  }
}

TEST(sim_v07_each_heating_setpoint_written_and_confirmed) {
  // Un registre à la fois, comme la procédure d'essai : relecture préalable,
  // écriture d'un octet, relecture immédiate, valeur confirmée publiée.
  for (uint32_t post : {2000u, 3000u, 4000u}) {
    Bench b;
    b.boiler.post_response_delay_ms = post;
    Heat h(b);
    b.run(0, BOOT);
    struct Step {
      size_t number;
      uint16_t address;
      uint8_t from, to;
    };
    const Step steps[] = {{h.v.a1_comfort, 0x2306, 20, 21},
                          {h.v.a1_reduced, 0x2307, 16, 17},
                          {h.v.m2_comfort, 0x3306, 21, 20},
                          {h.v.m2_reduced, 0x3307, 17, 16}};
    uint32_t t = BOOT;
    for (const Step &s : steps) {
      const size_t before = b.boiler.frame_log.size();
      CHECK(b.request(s.number, s.to, t) == "pre-read");
      t = run_until(b, t, MIN, [&] { return !b.writer_for(static_cast<int>(s.number))->busy(); });
      SetpointWriter *w = b.writer_for(static_cast<int>(s.number));
      CHECK(w->last_outcome() == DhwOutcome::CONFIRMED);
      CHECK(w->status() == "Confirmée : " + std::to_string(s.to) + " °C relue (précédente " + std::to_string(s.from) +
                               " °C, accusé reçu)");
      CHECK(find_frame(b.boiler, 0xF7, s.address, before) < find_frame(b.boiler, 0xF4, s.address, before));
      CHECK_EQ(b.boiler_value(s.address), s.to);
      CHECK_EQ(b.number_state[s.number], s.to);
      CHECK(w->last_duration_ms() <= 3 * post + 6 * S);
      if (report_enabled())
        std::printf("  resynchro %u s : 0x%04X demande -> confirmation %.1f s\n", static_cast<unsigned>(post / 1000),
                    s.address, w->last_duration_ms() / 1000.0);
      b.run(t, SETPOINT_GLOBAL_SPACING_MS);  // garde commune
      t += SETPOINT_GLOBAL_SPACING_MS;
    }
    CHECK_EQ(b.boiler.write_frames, 4);
    CHECK(read_back_follows_each_write(b.boiler));
    CHECK(b.failures.empty());
  }
}

TEST(sim_v07_heating_comfort_reduced_coherence_and_panel_changes) {
  Bench b;
  Heat h(b);
  b.boiler.registers[0x2306] = {18};  // A1 confort 18
  b.run(0, BOOT);
  // Confort sous la réduite, réduite au-dessus du confort : refusées.
  CHECK(b.request(h.v.m2_comfort, 16, BOOT) == "refused: comfort setpoint below the reduced setpoint");
  CHECK(b.request(h.v.a1_reduced, 19, BOOT) == "refused: reduced setpoint above the comfort setpoint");
  // Vitotrol : M2 confort passe à 20 ; l'entité affiche encore 21. Demande
  // 20 : la relecture préalable le voit, rien n'est écrit.
  b.boiler.registers[0x3306] = {20};
  CHECK(b.request(h.v.m2_comfort, 20, BOOT) == "pre-read");
  uint32_t t = run_until(b, BOOT, MIN, [&] { return !b.writer_for(static_cast<int>(h.v.m2_comfort))->busy(); });
  CHECK(b.writer_for(static_cast<int>(h.v.m2_comfort))->last_outcome() == DhwOutcome::UNCHANGED);
  CHECK_EQ(b.number_state[h.v.m2_comfort], 20);
  // Panneau : A1 réduite passe à 15, publiée à la lecture suivante.
  b.boiler.registers[0x2307] = {15};
  b.run(t, 6 * MIN);
  CHECK_EQ(b.number_state[h.v.a1_reduced], 15);
  CHECK(b.writer_for(static_cast<int>(h.v.a1_reduced))->status() ==
        "Modifiée hors de Home Assistant (panneau ou Vitotrol ?) : 16 -> 15 °C");
  CHECK_EQ(b.boiler.write_frames, 0);  // jamais corrigée
}

TEST(sim_v07_heating_coherence_needs_the_other_setpoint_first) {
  // Une modification qui croiserait l'autre consigne est refusée sans rien
  // écrire ; elle passe une fois l'autre consigne modifiée ET relue, dans
  // l'ordre : baisser -> réduite d'abord, monter -> confort d'abord.
  Bench b;
  Heat h(b);
  b.boiler.registers[0x2306] = {18};  // A1 : confort 18, réduite 16
  b.run(0, BOOT);
  uint32_t t = BOOT;
  auto change = [&](size_t number, uint16_t address, uint8_t value) {
    CHECK(b.request(number, value, t) == "pre-read");
    t = run_until(b, t, MIN, [&] { return !b.writer_for(static_cast<int>(number))->busy(); });
    CHECK(b.writer_for(static_cast<int>(number))->last_outcome() == DhwOutcome::CONFIRMED);
    CHECK_EQ(b.boiler_value(address), value);
    CHECK_EQ(b.number_state[number], value);
    b.run(t, SETPOINT_GLOBAL_SPACING_MS);  // garde commune
    t += SETPOINT_GLOBAL_SPACING_MS;
  };
  // M2 (confort 21, réduite 17) vers confort 16 / réduite 15 : baisser.
  CHECK(b.request(h.v.m2_comfort, 16, t) == "refused: comfort setpoint below the reduced setpoint");
  CHECK(b.writer_for(static_cast<int>(h.v.m2_comfort))->status() ==
        "Refusé : confort 16 °C inférieur à la réduite (17 °C)");
  CHECK_EQ(b.boiler.write_frames, 0);
  change(h.v.m2_reduced, 0x3307, 15);
  change(h.v.m2_comfort, 0x3306, 16);  // la réduite relue (15) sert au contrôle
  // A1 (confort 18, réduite 16) vers confort 21 / réduite 20 : monter.
  CHECK(b.request(h.v.a1_reduced, 20, t) == "refused: reduced setpoint above the comfort setpoint");
  CHECK(b.writer_for(static_cast<int>(h.v.a1_reduced))->status() ==
        "Refusé : réduite 20 °C supérieure au confort (18 °C)");
  CHECK_EQ(b.boiler.write_frames, 2);
  change(h.v.a1_comfort, 0x2306, 21);
  change(h.v.a1_reduced, 0x2307, 20);
  // Égalité permise (confort == réduite).
  change(h.v.m2_reduced, 0x3307, 16);
  CHECK_EQ(b.boiler.write_frames, 5);
  CHECK(read_back_follows_each_write(b.boiler));
  CHECK(b.failures.empty());
  // Ordre de passage sur le bus : 0x3307, 0x3306, 0x2306, 0x2307, 0x3307.
  const uint16_t order[] = {0x3307, 0x3306, 0x2306, 0x2307, 0x3307};
  size_t from = 0;
  for (uint16_t a : order) {
    const size_t at = find_frame(b.boiler, 0xF4, a, from);
    CHECK(at != SIZE_MAX);
    from = at + 1;
  }
}

TEST(sim_v07_heating_link_loss_and_reboot_write_nothing_more) {
  {  // Liaison perdue au moment d'émettre : indéterminé, dernière valeur
     // confirmée affichée, jamais réémise.
    Bench b;
    Heat h(b);
    b.run(0, BOOT);
    CHECK(b.request(h.v.a1_comfort, 22, BOOT) == "pre-read");
    uint32_t t = run_until(b, BOOT, MIN, [&] { return b.write_in_flight; });
    b.boiler.connected = false;
    t = run_until(b, t, 2 * MIN, [&] { return !b.writer_for(static_cast<int>(h.v.a1_comfort))->busy(); });
    CHECK(b.writer_for(static_cast<int>(h.v.a1_comfort))->last_outcome() == DhwOutcome::INDETERMINATE);
    CHECK_EQ(b.number_state[h.v.a1_comfort], 20);
    b.boiler.connected = true;
    b.run(t, 20 * MIN);
    CHECK_EQ(b.boiler.write_frames, 0);
    CHECK_EQ(b.boiler_value(0x2306), 20);
  }
  for (bool in_flight : {false, true}) {  // redémarrage pendant l'opération
    Bench b;
    Heat h(b);
    b.run(0, BOOT);
    CHECK(b.request(h.v.m2_reduced, 16, BOOT) == "pre-read");
    uint32_t t = in_flight ? run_until(b, BOOT, MIN, [&] { return b.write_in_flight; }) : BOOT + 100;
    const int frames = b.boiler.write_frames;
    b.reboot(t);
    b.boiler_only(t, 5 * S);
    b.run(t + 5 * S, 30 * MIN);
    CHECK_EQ(b.boiler.write_frames, frames);
    for (const auto &kv : b.heat)
      CHECK(!kv.second.busy());
    CHECK_EQ(b.number_state[h.v.m2_reduced], b.boiler_value(0x3307));
  }
}

TEST(sim_v07_heating_and_dhw_one_change_at_a_time) {
  Bench b;
  Heat h(b);
  b.enable_dhw_setpoint_write = true;
  b.run(0, BOOT);
  CHECK(b.request(h.v.dhw, 55, BOOT) == "pre-read");
  CHECK(b.request(h.v.a1_comfort, 21, BOOT) == "refused: another setpoint change is in progress");
  uint32_t t = run_until(b, BOOT, MIN, [&] { return !b.dhw.busy(); });
  CHECK(b.dhw.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK(b.request(h.v.a1_comfort, 21, t) == "refused: too soon after another setpoint write");
  b.run(t, SETPOINT_GLOBAL_SPACING_MS);
  t += SETPOINT_GLOBAL_SPACING_MS;
  CHECK(b.request(h.v.a1_comfort, 21, t) == "pre-read");
  // Pendant la consigne de chauffage, l'ECS attend à son tour.
  CHECK(b.request(h.v.dhw, 56, t + 100) == "refused: another setpoint change is in progress");
  t = run_until(b, t, MIN, [&] { return !b.writer_for(static_cast<int>(h.v.a1_comfort))->busy(); });
  CHECK_EQ(b.boiler_value(0x6300), 55);
  CHECK_EQ(b.boiler_value(0x2306), 21);
  CHECK_EQ(b.boiler.write_frames, 2);
  CHECK(read_back_follows_each_write(b.boiler));
}

TEST(sim_v07_heating_waits_for_circulation_restore) {
  // Même scénario que pour l'ECS (v0.6) avec la consigne confort de M2.
  auto scenario = [](int32_t offset_ms, bool priority, uint32_t a1_before_ms) {
    Bench b;
    BoostBench x(b);  // V03 avec écritures : consignes de chauffage autorisées
    const size_t m2 = x.modes.base.m2_comfort;
    if (priority)
      b.restore_priority = [&x] { return boost_restore_priority(x.glue.seq); };
    b.restore_observer = [&x] { return boost_restore_priority(x.glue.seq); };
    run_boost(b, x.glue, 0, BOOT);
    CHECK(x.glue.start(10, ha_now(BOOT), BOOT) == "accepted");
    const uint32_t confirmed =
        run_boost_until(b, x.glue, BOOT, MIN, [&] { return x.glue.seq.state() == BoostState::ACTIVE; });
    const uint32_t ask = confirmed + 10 * MIN + offset_ms;
    if (a1_before_ms != 0) {
      run_boost(b, x.glue, confirmed, ask - a1_before_ms - confirmed);
      CHECK(b.request_mode(x.modes.a1, ECS_ONLY, ask - a1_before_ms) == "queued");
      run_boost(b, x.glue, ask - a1_before_ms, a1_before_ms);
    } else {
      run_boost(b, x.glue, confirmed, ask - confirmed);
    }
    CHECK(b.request(m2, 20, ask) == "pre-read");
    run_boost(b, x.glue, ask, 3 * MIN);
    CHECK(x.glue.seq.idle());
    CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);
    CHECK_EQ(b.boiler_value(0x3306), 20);
    CHECK(read_back_follows_each_write(b.boiler));
    return b.setpoint_started_in_restore;
  };
  int without = 0;
  for (int32_t offset : {-6000, -3000, -1000, 0, 2000}) {
    for (uint32_t a1 : {0u, 4000u, 8000u, 12000u}) {
      CHECK_EQ(scenario(offset, true, a1), 0);
      without += scenario(offset, false, a1);
    }
  }
  CHECK(without > 0);  // contrôle : sans la priorité, le défaut est détecté
}

TEST(sim_v07_climate_follows_the_vitotronic_and_never_commands_by_itself) {
  Bench b;
  Heat h(b, true, true);
  b.enable_mode_writes = b.enable_m2_mode_write = true;  // tout autorisé, aucune commande
  ClimateGlue c(b, h);
  c.run(0, BOOT);
  CHECK(c.state.ready);
  CHECK_EQ(c.state.target, 21.0f);  // consigne confort M2
  CHECK_NEAR(c.state.current, 20.4, 1e-4);  // ambiance Vitotrol, jamais le départ 0x3900
  CHECK(c.state.hvac == CircuitHvac::HEAT);
  CHECK(c.state.action == CircuitAction::HEATING);
  // Panneau / Vitotrol : consigne, programme, circulateur.
  struct Change {
    uint16_t address;
    std::vector<uint8_t> value;
  };
  uint32_t t = BOOT;
  const Change changes[] = {{0x3306, {19}}, {0x3301, {0x00}}, {0x3301, {0x05}}, {0x3301, {0x07}},
                            {0x3301, {0x03}}, {0x3906, {0x00}}, {0x0898, {0xD2, 0x00}}};
  for (const Change &ch : changes) {
    b.boiler.registers[ch.address] = ch.value;
    c.run(t, 6 * MIN);
    t += 6 * MIN;
    CHECK_EQ(c.state.target, static_cast<float>(b.number_state[h.v.m2_comfort]));  // number et climate identiques
    const CircuitHvac expected = circuit_hvac_from_mode(b.boiler_value(0x3301));
    if (expected != CircuitHvac::UNKNOWN)
      CHECK(c.state.hvac == expected);
  }
  CHECK_EQ(c.state.target, 19.0f);
  CHECK(c.state.hvac == CircuitHvac::HEAT);  // 07 inconnu : resté OFF, puis 03 -> HEAT
  CHECK(c.state.action == CircuitAction::IDLE);  // circulateur arrêté
  CHECK_NEAR(c.state.current, 21.0, 1e-4);
  // Redémarrage : rien n'est commandé, l'état revient par lecture.
  b.reboot(t);
  c.state = CircuitClimateState();
  b.boiler_only(t, 5 * S);
  c.run(t + 5 * S, 6 * H);
  CHECK(c.state.ready);
  CHECK_EQ(c.state.target, 19.0f);
  CHECK_EQ(b.boiler.write_frames, 0);  // aucune commande intempestive
}

TEST(sim_v073_climate_m2_heating_inertia_stopped_and_stale_readings) {
  Bench b;
  Heat h(b, false, false);
  ClimateGlue c(b, h);
  c.run(0, BOOT);
  // Programme 03, circulateur 01, brûleur 01 : Chauffe.
  CHECK(c.state.status == CircuitStatus::HEATING);
  CHECK(c.state.action == CircuitAction::HEATING);
  uint32_t t = BOOT;
  // Brûleur arrêté, circulateur toujours en marche : Inertie (idle).
  b.boiler.registers[0x0842] = {0x00};
  c.run(t, MIN);
  t += MIN;
  CHECK(c.state.status == CircuitStatus::INERTIA);
  CHECK(c.state.action == CircuitAction::IDLE);
  b.boiler.registers[0x0842] = {0x01};
  c.run(t, MIN);
  t += MIN;
  CHECK(c.state.status == CircuitStatus::HEATING);
  // Brûleur plus lu (aucune réponse) : la dernière valeur reste fraîche
  // 3 intervalles + 30 s (2 min), puis Inconnu, jamais « chauffe ».
  b.boiler.register_faults[0x0842] = SimBoiler::Fault::NO_RESPONSE;
  const uint32_t last = b.last_success_ms[h.v.burner];
  const uint32_t limit = (last + 119 * S) / S * S;  // refresh du climate : secondes rondes
  c.run(t, limit - t);
  CHECK(c.state.status == CircuitStatus::HEATING);
  c.run(limit, 3 * S);
  CHECK(c.state.status == CircuitStatus::UNKNOWN);
  CHECK(c.state.action == CircuitAction::IDLE);
  CHECK(c.state.hvac == CircuitHvac::HEAT);  // mode lu conservé
  t = limit + 3 * S;
  b.boiler.register_faults.erase(0x0842);
  c.run(t, MIN);
  t += MIN;
  CHECK(c.state.status == CircuitStatus::HEATING);
  // Circulateur arrêté : Arrêt quel que soit le brûleur (idle, mode HEAT).
  b.boiler.registers[0x3906] = {0x00};
  c.run(t, 2 * MIN);
  t += 2 * MIN;
  CHECK(c.state.status == CircuitStatus::STOPPED);
  CHECK(c.state.action == CircuitAction::IDLE);
  // Chauffage désactivé (ECS seule) : Arrêt, action off.
  b.boiler.registers[0x3906] = {0x01};
  b.boiler.registers[0x3301] = {0x00};
  c.run(t, 6 * MIN);
  t += 6 * MIN;
  CHECK(c.state.status == CircuitStatus::STOPPED);
  CHECK(c.state.action == CircuitAction::OFF);
  CHECK(c.state.hvac == CircuitHvac::OFF);
  // Interface débranchée : tout devient périmé ou inconnu, jamais « chauffe ».
  b.boiler.registers[0x3301] = {0x03};
  c.run(t, 6 * MIN);
  t += 6 * MIN;
  CHECK(c.state.status == CircuitStatus::HEATING);
  b.boiler.connected = false;
  c.run(t, 20 * MIN);
  t += 20 * MIN;
  CHECK(c.state.status == CircuitStatus::UNKNOWN);
  CHECK(c.state.action != CircuitAction::HEATING);
  b.boiler.connected = true;
  c.run(t, 6 * MIN);
  CHECK(c.state.status == CircuitStatus::HEATING);
  CHECK_EQ(b.boiler.write_frames, 0);  // affichage seulement
}

TEST(sim_v073_climate_without_burner_never_reports_heating) {
  Bench b;
  Heat h(b, false, false);
  ClimateGlue c(b, h);
  c.with_burner = false;  // ancienne configuration, sans burner_id
  c.run(0, BOOT);
  CHECK(c.state.ready);
  CHECK(c.state.status == CircuitStatus::UNKNOWN);  // circulateur en marche, brûleur non déclaré
  CHECK(c.state.action == CircuitAction::IDLE);
  b.boiler.registers[0x3906] = {0x00};
  c.run(BOOT, 2 * MIN);
  CHECK(c.state.status == CircuitStatus::STOPPED);
}

TEST(sim_v07_climate_commands_use_the_secured_paths) {
  Bench b;
  Heat h(b, false, false);  // tout désactivé (défaut)
  ClimateGlue c(b, h);
  c.run(0, BOOT);
  CHECK(c.set_temperature(20, BOOT) == "refused: writes are disabled (enable_writes: false)");
  CHECK(c.set_hvac(false, BOOT) == "disabled");
  c.run(BOOT, 2 * MIN);
  CHECK_EQ(b.boiler.write_frames, 0);
  CHECK_EQ(c.state.target, 21.0f);  // l'état lu, jamais la demande
  // Écritures + consignes de chauffage : la consigne passe, le mode non.
  b.enable_writes = b.enable_heating_setpoint_writes = true;
  uint32_t t = BOOT + 2 * MIN;
  CHECK(c.set_temperature(20, t) == "pre-read");
  // Home Assistant / VTherm répète la même demande : jamais doublée.
  CHECK(c.set_temperature(20, t + 500) == "pending");
  CHECK(c.set_hvac(false, t) == "mode disabled");
  c.run(t, MIN);
  t += MIN;
  CHECK_EQ(b.boiler_value(0x3306), 20);
  CHECK_EQ(c.state.target, 20.0f);
  CHECK_EQ(writes_to(b.boiler, 0x3306), 1);
  const int reads = b.boiler.requests_by_address[0x3306];
  CHECK(c.set_temperature(20, t) == "unchanged");  // déjà la valeur lue : rien
  c.run(t, 10 * S);
  CHECK_EQ(b.boiler.requests_by_address[0x3306], reads);
  // Programmes autorisés (A1/M2) : HEAT/OFF -> 03/00, jamais 05.
  b.enable_mode_writes = b.enable_m2_mode_write = true;
  t += 10 * S;
  CHECK(c.set_hvac(false, t) == "queued");
  c.run(t, MIN);
  t += MIN;
  CHECK_EQ(b.boiler_value(0x3301), 0x00);  // ECS seule : l'eau chaude reste produite
  CHECK(c.state.hvac == CircuitHvac::OFF);
  CHECK(c.set_hvac(false, t) == "unchanged");
  CHECK(c.set_hvac(true, t) == "queued");
  c.run(t, MIN);
  CHECK_EQ(b.boiler_value(0x3301), 0x03);
  CHECK(c.state.hvac == CircuitHvac::HEAT);
  for (const auto &frame : b.boiler.write_log) {
    if (frame[2] == 0x33 && frame[3] == 0x01)
      CHECK(frame[5] != 0x05);
  }
  CHECK_EQ(writes_to(b.boiler, 0x3906), 0);  // circulateur : jamais commandé
  CHECK_EQ(writes_to(b.boiler, 0x0898), 0);
  CHECK(read_back_follows_each_write(b.boiler));  // le registre écrit est relu en premier
}

TEST(sim_v07_climate_link_loss_shows_unknown_never_commands) {
  Bench b;
  Heat h(b);
  ClimateGlue c(b, h);
  c.run(0, BOOT);
  b.boiler.connected = false;
  c.run(BOOT, 10 * MIN);
  CHECK(std::isnan(c.state.target) || c.state.target == 21.0f);  // jamais une valeur inventée
  CHECK_EQ(b.boiler.write_frames, 0);
  b.boiler.connected = true;
  c.run(BOOT + 10 * MIN, 10 * MIN);
  CHECK_EQ(c.state.target, 21.0f);
  CHECK_EQ(b.boiler.write_frames, 0);
}

// --- v0.8 : éditeur des programmations -----------------------------------------
//
// Chaudière SIMULÉE (blocs de 8 octets acceptés et accusés par 00, hypothèse
// vcontrold) : rien ici ne valide l'écriture des programmations sur la KW3.

namespace {

/// Même logique que VissLinkScheduleEditor (components/visslink_schedule_editor).
struct EditorGlue : BlockClient {
  Bench &b;
  ScheduleEditor seq;
  size_t index[EDITOR_CIRCUIT_COUNT];  ///< registre de chaque programmation
  bool flash_has{false};
  EditorRecord flash;
  int saves{0};
  std::string last_error;

  EditorGlue(Bench &bench, size_t a1, size_t m2, size_t dhw, size_t loop) : b(bench), index{a1, m2, dhw, loop} {
    boot(0);
  }
  void boot(uint32_t now) {
    seq = ScheduleEditor();
    seq.boot(flash_has ? &flash : nullptr, now);
    b.editor = this;
  }
  /// VissLinkScheduleEditor::context_ + submit_.
  bool submit(EditKind kind, const char *circuit, const std::string &days, const std::string &ranges, int source,
              const std::string &expected, uint32_t now) {
    last_error.clear();
    if (!b.schedule_writes_enabled())
      return refuse("disabled");
    if (!b.link_ok)
      return refuse("link");
    const uint8_t c = editor_circuit_from_key(circuit);
    if (c >= EDITOR_CIRCUIT_COUNT)
      return refuse("circuit");
    EditContext ctx;
    ctx.cache = b.raw_weeks[index[c]];
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      if (b.locked_by_other(WriteArbiter::EDITOR_OWNER, schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[c], d)))
        ctx.locked_mask |= static_cast<uint8_t>(1u << d);
    ctx.busy = seq.busy() || b.boost_busy();
    if (!seq.busy() && ctx.busy)
      return refuse("bouclage ECS temporaire en cours : réessayer après la restauration");
    DayList list;
    if (!parse_day_list(days, list.days, &list.count))
      return refuse("jours illisibles");
    EditRequest r;
    const std::string e = prepare_edit(kind, circuit, list, ranges, source, expected, ctx, &r);
    if (!e.empty())
      return refuse(e);
    CHECK(seq.start(r, now));
    dispatch(now);
    return true;
  }
  bool undo(uint32_t now) {
    EditRequest r;
    if (!seq.undo_request(&r))
      return refuse("rien");
    if (seq.busy() || b.boost_busy())
      return refuse("busy");
    CHECK(seq.start(r, now));
    dispatch(now);
    return true;
  }
  bool refresh(const char *circuit, uint32_t now) {
    const uint8_t c = editor_circuit_from_key(circuit);
    return c < EDITOR_CIRCUIT_COUNT && b.scheduler.request_cycle(index[c], now);
  }
  bool refuse(const std::string &why) {
    last_error = why;
    return false;
  }
  std::string expected(const char *circuit) {
    return format_expected_week(b.raw_weeks[index[editor_circuit_from_key(circuit)]]);
  }
  void dispatch(uint32_t now) {
    for (int guard = 0; guard < 8; guard++) {
      const EditorAction a = seq.poll(now);
      switch (a.type) {
        case EditorActionType::READ:
          if (!b.request_direct_read(a.address, 8, now, WriteArbiter::EDITOR_OWNER))
            seq.on_read_not_submitted();
          return;
        case EditorActionType::WRITE:
          if (!b.submit_block_write(a.address, a.data, now, WriteArbiter::EDITOR_OWNER)) {
            seq.on_write(false, false, now);
            break;
          }
          return;
        case EditorActionType::SAVE:
          saves++;
          flash = a.record;
          flash_has = true;
          seq.on_saved(true, now);
          break;
        default:
          return;
      }
    }
  }
  void on_direct_read(uint16_t address, bool ok, const uint8_t *data, uint8_t length, uint32_t now) override {
    if (seq.on_read(ok, data, length, now))
      b.publish_schedule_block(address, data, now);
    dispatch(now);
  }
  void on_block_write(uint16_t, bool sent, bool acked, uint32_t now) override {
    seq.on_write(sent, acked, now);
    dispatch(now);
  }
  bool expects_block_write(uint16_t a, const uint8_t *d) const override { return seq.expects_write(a, d); }
  bool block_operation_busy() const override { return seq.busy(); }
  bool locks_block(uint16_t a) const override { return seq.locks_block(a); }
};

/// Production v0.8 : registres actifs, quatre programmations (6 h), blocs
/// des quatre circuits acceptés en écriture par la chaudière simulée.
struct EditBench {
  Active act;
  Circuits circuits;
  EditorGlue ed;
  explicit EditBench(Bench &b)
      : act(b, 0), circuits(b, 0), ed(b, circuits.a1, circuits.m2, circuits.dhw, circuits.loop) {
    b.enable_writes = b.enable_schedule_writes = true;
    for (uint16_t monday : EDITOR_CIRCUIT_ADDRESSES)
      for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
        b.boiler.writable.insert(schedule_day_address(monday, d));
  }
};

void run_edit(Bench &b, EditorGlue &ed, uint32_t from, uint32_t duration_ms, BoostGlue *boost = nullptr) {
  for (uint32_t t = from; t - from < duration_ms; t += 5) {
    if (t % 1000 == 0) {
      set_vito_clock(b.boiler, t);
      b.clock_tick(t);
    }
    b.boiler.tick(t);
    b.step(t);
    ed.dispatch(t);
    if (boost != nullptr) {
      boost->dispatch(t);
      boost->refresh(t);
    }
  }
}

std::vector<uint8_t> enc(const std::string &ranges) {
  DaySchedule d;
  CHECK(parse_ranges(ranges, &d) == RangesError::NONE);
  uint8_t out[8];
  CHECK(encode_day_block(d, out));
  return std::vector<uint8_t>(out, out + 8);
}

constexpr uint32_t READ_ALL = 5 * MIN;  ///< 4 programmations lues (7 blocs, 30 s entre deux)

std::map<uint16_t, std::vector<uint8_t>> snapshot(const SimBoiler &boiler) {
  std::map<uint16_t, std::vector<uint8_t>> m;
  for (uint16_t monday : EDITOR_CIRCUIT_ADDRESSES)
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      m[schedule_day_address(monday, d)] = boiler.registers.at(schedule_day_address(monday, d));
  return m;
}

}  // namespace

TEST(sim_v08_four_schedules_read_then_one_day_edited) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  for (size_t i : {x.circuits.a1, x.circuits.m2, x.circuits.dhw, x.circuits.loop})
    CHECK_EQ(b.raw_weeks[i].known_mask, EDITOR_ALL_DAYS);
  CHECK(x.ed.expected("ecs").find("20B8FFFFFFFFFFFF") == 0);
  const auto before = snapshot(b.boiler);
  const int burner_before = b.successes[x.act.burner];
  const size_t frames = b.boiler.frame_log.size();
  CHECK(x.ed.submit(EditKind::SET, "a1", "2", "05:30-08:00,16:30-22:30", -1, x.ed.expected("a1"), READ_ALL));
  run_edit(b, x.ed, READ_ALL, MIN);
  CHECK(!x.ed.seq.busy());
  CHECK(x.ed.seq.status() == "Enregistré et relu : Radiateurs Me");
  CHECK(b.boiler.registers[0x2010] == enc("05:30-08:00,16:30-22:30"));
  // Trames : lecture préalable, écriture de 8 octets, relecture ; rien d'autre écrit.
  CHECK_EQ(b.boiler.write_frames, 1);
  CHECK(b.boiler.write_log[0] ==
        (std::vector<uint8_t>{0x01, 0xF4, 0x20, 0x10, 0x08, 0x2B, 0x40, 0x83, 0xB3, 0xFF, 0xFF, 0xFF, 0xFF}));
  std::vector<std::pair<uint8_t, uint16_t>> block_frames;
  for (size_t i = frames; i < b.boiler.frame_log.size(); i++)
    if (b.boiler.frame_log[i].second == 0x2010)
      block_frames.push_back(b.boiler.frame_log[i]);
  CHECK(block_frames == (std::vector<std::pair<uint8_t, uint16_t>>{{0xF7, 0x2010}, {0xF4, 0x2010}, {0xF7, 0x2010}}));
  // Affichage : le bloc relu, sans attendre le cycle de 6 h.
  CHECK(format_day(b.weeks[x.circuits.a1].days[2]) == "05:30-08:00, 16:30-22:30");
  // Autres jours et circuits intacts ; lectures ordinaires poursuivies.
  for (const auto &kv : before)
    if (kv.first != 0x2010)
      CHECK(b.boiler.registers[kv.first] == kv.second);
  CHECK(b.successes[x.act.burner] > burner_before);
  CHECK(b.failures.empty());
  CHECK_EQ(x.ed.saves, 1);
}

TEST(sim_v08_copy_monday_to_weekdays_block_by_block) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  CHECK(x.ed.submit(EditKind::SET, "m2", "0", "05:00-07:30,17:00-22:00", -1, x.ed.expected("m2"), READ_ALL));
  run_edit(b, x.ed, READ_ALL, MIN);
  uint32_t t = READ_ALL + MIN;
  CHECK(x.ed.submit(EditKind::COPY, "m2", "0,1,2,3,4", "", 0, x.ed.expected("m2"), t));
  // Pendant l'opération : jamais « semaine enregistrée » avant la fin.
  bool partial_seen = false;
  for (int i = 0; i < 120 && x.ed.seq.busy(); i++) {
    run_edit(b, x.ed, t, S);
    t += S;
    if (x.ed.seq.busy())
      CHECK(x.ed.seq.status().find("Enregistré et relu") == std::string::npos);
    partial_seen |= x.ed.seq.busy() && x.ed.seq.saved_mask() != 0;
  }
  CHECK(partial_seen);
  CHECK(!x.ed.seq.busy());
  CHECK(x.ed.seq.status() == "Enregistré et relu : Plancher Ma, Me, Je, Ve");
  for (uint8_t d = 0; d < 5; d++)
    CHECK(b.boiler.registers[schedule_day_address(M2, d)] == enc("05:00-07:30,17:00-22:00"));
  for (uint8_t d = 5; d < 7; d++)
    CHECK(b.boiler.registers[schedule_day_address(M2, d)] == (std::vector<uint8_t>{0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK_EQ(writes_to(b.boiler, 0x3000), 1);  // lundi déjà écrit, jamais réécrit
  for (uint8_t d = 1; d < 5; d++)
    CHECK_EQ(writes_to(b.boiler, schedule_day_address(M2, d)), 1);
  // 10 s au moins entre deux écritures (WriteArbiter::MIN_SPACING_MS).
  CHECK_EQ(b.boiler.write_frames, 5);
  CHECK(b.failures.empty());
}

TEST(sim_v08_panel_change_after_display_is_never_overwritten) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  const std::string shown = x.ed.expected("ecs");
  // Modification au panneau après l'affichage, avant « Enregistrer ».
  const std::vector<uint8_t> panel = {0x28, 0xA8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 05:00-21:00
  b.boiler.registers[schedule_day_address(DHW, 4)] = panel;
  CHECK(x.ed.submit(EditKind::SET, "ecs", "3,4,5", "04:00-22:00", -1, shown, READ_ALL));
  run_edit(b, x.ed, READ_ALL, 2 * MIN);
  CHECK(b.boiler.registers[schedule_day_address(DHW, 4)] == panel);  // conservé
  CHECK(x.ed.seq.result(3) == JobResult::SAVED);
  CHECK(x.ed.seq.result(4) == JobResult::FAILED);
  CHECK(x.ed.seq.result(5) == JobResult::NOT_DONE);
  CHECK(b.boiler.registers[schedule_day_address(DHW, 5)] == (std::vector<uint8_t>{0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(x.ed.seq.status() ==
        "Échec ECS Ve : modifié depuis la Vitotronic (05:00-21:00), non écrasé : programmation rechargée · "
        "enregistrés : Je · non traités : Sa");
  CHECK(format_day(b.weeks[x.circuits.dhw].days[4]) == "05:00-21:00");  // affichage resynchronisé
  // Nouvelle référence de la carte : la modification passe.
  CHECK(x.ed.submit(EditKind::SET, "ecs", "4", "04:00-22:00", -1, x.ed.expected("ecs"), READ_ALL + 2 * MIN));
  run_edit(b, x.ed, READ_ALL + 2 * MIN, MIN);
  CHECK(b.boiler.registers[schedule_day_address(DHW, 4)] == enc("04:00-22:00"));
  // Référence de la carte périmée alors que l'ESP connaît déjà le changement.
  b.boiler.registers[schedule_day_address(DHW, 6)] = panel;
  CHECK(x.ed.refresh("ecs", READ_ALL + 3 * MIN));
  run_edit(b, x.ed, READ_ALL + 3 * MIN, 4 * MIN);
  CHECK(!x.ed.submit(EditKind::SET, "ecs", "6", "04:00-22:00", -1, shown, READ_ALL + 7 * MIN));
  CHECK(x.ed.last_error.find("ECS Di modifié depuis l'affichage") == 0);
}

TEST(sim_v08_link_lost_after_the_write_then_recovered) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  CHECK(x.ed.submit(EditKind::SET, "bouclage", "1", "06:00-06:30", -1, "", READ_ALL));
  uint32_t t = READ_ALL;
  while (b.boiler.write_frames == 0 && t < READ_ALL + MIN) {
    run_edit(b, x.ed, t, 5);
    t += 5;
  }
  CHECK_EQ(b.boiler.write_frames, 1);
  run_edit(b, x.ed, t, 30);  // accusé
  t += 30;
  b.boiler.connected = false;  // interface débranchée juste après l'écriture
  run_edit(b, x.ed, t, 3 * MIN);
  t += 3 * MIN;
  CHECK(x.ed.seq.busy());
  CHECK(x.ed.seq.saved_mask() == 0);  // jamais « enregistré » sans relecture
  CHECK(x.ed.seq.status().find("nouvelle relecture dans 30 s") != std::string::npos);
  CHECK(x.ed.seq.locks_block(0x2208));
  CHECK(x.ed.block_operation_busy());
  b.boiler.connected = true;
  run_edit(b, x.ed, t, 2 * MIN);
  CHECK(!x.ed.seq.busy());
  CHECK(x.ed.seq.status() == "Enregistré et relu : Bouclage ECS Ma");
  CHECK_EQ(b.boiler.write_frames, 1);  // jamais réémise
}

TEST(sim_v08_esp_reboot_checks_the_last_write_without_writing) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  CHECK(x.ed.submit(EditKind::SET, "a1", "6", "07:00-23:00", -1, "", READ_ALL));
  uint32_t t = READ_ALL;
  while (b.boiler.write_frames == 0) {
    run_edit(b, x.ed, t, 5);
    t += 5;
  }
  // Coupure de l'ESP juste après l'émission (relecture jamais faite).
  b.reboot(t + 20);
  x.ed.boot(t + 20);
  CHECK(x.ed.seq.busy());  // vérification de la dernière écriture
  const int writes = b.boiler.write_frames;
  run_edit(b, x.ed, t + 20, 2 * MIN);
  CHECK(!x.ed.seq.busy());
  CHECK(x.ed.seq.status() == "Dernière modification (Radiateurs Di) confirmée par relecture");
  CHECK_EQ(b.boiler.write_frames, writes);  // aucune écriture au démarrage
  // L'annulation reste possible : bloc d'origine réécrit et relu.
  run_edit(b, x.ed, t + 20 + 2 * MIN, READ_ALL);
  t += 20 + 2 * MIN + READ_ALL;
  CHECK(x.ed.undo(t));
  run_edit(b, x.ed, t, MIN);
  CHECK(b.boiler.registers[0x2030] == (std::vector<uint8_t>{0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(x.ed.seq.status() == "Annulation enregistrée et relue : Radiateurs Di");
}

TEST(sim_v08_editor_and_boost_exclude_each_other) {
  Bench b;
  EditBench x(b);
  BoostGlue boost(b, 0x7F);
  run_edit(b, x.ed, 0, READ_ALL, &boost);
  uint32_t t = READ_ALL;
  // Bouclage temporaire (dimanche, 0x2230) : toute modification refusée
  // explicitement jusqu'à la fin de la restauration ; le jour est signalé.
  CHECK(boost.start(10, ha_now(t), t) == "accepted");
  run_edit(b, x.ed, t, MIN, &boost);
  t += MIN;
  CHECK(boost.seq.state() == BoostState::ACTIVE);
  CHECK(b.locked_by_other(WriteArbiter::EDITOR_OWNER, 0x2230));
  CHECK(!b.locked_by_other(WriteArbiter::EDITOR_OWNER, 0x2228));
  CHECK(!x.ed.submit(EditKind::SET, "bouclage", "6", "06:00-06:30", -1, "", t));
  CHECK(x.ed.last_error == "bouclage ECS temporaire en cours : réessayer après la restauration");
  CHECK(!x.ed.submit(EditKind::SET, "a1", "0", "06:00-21:00", -1, "", t));
  // Pendant la restauration aussi.
  CHECK(boost.restore(t));
  run_edit(b, x.ed, t, 50, &boost);
  t += 50;
  CHECK(!boost.seq.idle());
  CHECK(!x.ed.submit(EditKind::SET, "bouclage", "6", "06:00-06:30", -1, "", t));
  run_edit(b, x.ed, t, 2 * MIN, &boost);
  t += 2 * MIN;
  CHECK(boost.seq.idle());
  CHECK(b.boiler.registers[0x2230] == LOOP_SUNDAY);  // restauration intacte
  // Bouclage terminé : modification acceptée ; pendant celle-ci, le bouclage
  // refuse de démarrer.
  CHECK(x.ed.submit(EditKind::SET, "bouclage", "6", "06:00-06:30,21:40-21:50", -1, "", t));
  CHECK(boost.start(10, ha_now(t), t) == "editor");
  run_edit(b, x.ed, t, MIN, &boost);
  t += MIN;
  CHECK(x.ed.seq.status() == "Enregistré et relu : Bouclage ECS Di");
  // Le bouclage repart de la nouvelle programmation et la restaure à l'identique.
  t += 15 * MIN;  // intervalle minimal entre deux déclenchements
  run_edit(b, x.ed, t - 15 * MIN, 15 * MIN, &boost);
  CHECK(boost.start(10, ha_now(t), t) == "accepted");
  run_edit(b, x.ed, t, 15 * MIN, &boost);
  CHECK(boost.seq.idle());
  CHECK(b.boiler.registers[0x2230] == enc("06:00-06:30,21:40-21:50"));
  CHECK(b.failures.empty());
}

TEST(sim_v08_manual_refresh_reads_seven_blocks_spaced) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  // Modification au panneau : visible au prochain cycle (6 h) ou au
  // rafraîchissement manuel, qui relit les 7 jours espacés de 30 s.
  b.boiler.registers[schedule_day_address(M2, 3)] = {0x28, 0xA8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const size_t reads_before = b.schedule_reads.size();
  CHECK(x.ed.refresh("m2", READ_ALL));
  run_edit(b, x.ed, READ_ALL, 4 * MIN);
  std::vector<uint32_t> times;
  for (size_t i = reads_before; i < b.schedule_reads.size(); i++)
    if (b.schedule_reads[i].second >= M2 && b.schedule_reads[i].second < M2 + 56)
      times.push_back(b.schedule_reads[i].first);
  CHECK_EQ(times.size(), 7u);
  for (size_t i = 1; i < times.size(); i++)
    CHECK(times[i] - times[i - 1] >= RegisterScheduler::BLOCK_DELAY_MS);
  CHECK(format_day(b.weeks[x.circuits.m2].days[3]) == "05:00-21:00");
  CHECK(x.ed.refresh("m2", READ_ALL + 4 * MIN));
  CHECK(!x.ed.refresh("zz", READ_ALL));
}

TEST(sim_v08_editor_disabled_or_refused_without_link) {
  Bench b;
  EditBench x(b);
  run_edit(b, x.ed, 0, READ_ALL);
  b.enable_schedule_writes = false;
  CHECK(!x.ed.submit(EditKind::SET, "a1", "0", "06:00-21:00", -1, "", READ_ALL));
  CHECK(x.ed.last_error == "disabled");
  b.enable_schedule_writes = true;
  b.boiler.connected = false;
  run_edit(b, x.ed, READ_ALL, MIN);
  CHECK(!x.ed.submit(EditKind::SET, "a1", "0", "06:00-21:00", -1, "", READ_ALL + MIN));
  CHECK(x.ed.last_error == "link");
  CHECK_EQ(b.boiler.write_frames, 0);
  // Écriture du bouclage par l'éditeur ou d'un bloc A1 par le bouclage :
  // refusées par la liste blanche du propriétaire.
  const uint8_t block[8] = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!b.block_write_allowed(0x2000, block, WriteArbiter::BOOST_OWNER));
  CHECK(!b.block_write_allowed(0x2301, block, WriteArbiter::EDITOR_OWNER));  // pas un bloc
}
