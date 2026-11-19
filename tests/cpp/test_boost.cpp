// Bouclage ECS à la demande (v0.5) : codage des blocs, horloge, calcul de
// plage et machine à états (pilote synchrone, sans moteur KW).

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "circulation_boost.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

using Block = std::vector<uint8_t>;

Block bytes(const uint8_t *b) { return Block(b, b + SCHEDULE_BLOCK_LENGTH); }

/// Programmation réelle du bouclage (relevée sur la chaudière).
const Block REAL = {0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF};  // 06:10-08:30, 21:40-21:50

VitoClock at(uint8_t h, uint8_t m, uint8_t s = 0, uint8_t weekday = 6) {
  VitoClock c;
  c.year = 2026, c.month = 11, c.day = 15, c.weekday = weekday;
  c.hour = h, c.minute = m, c.second = s;
  return c;
}

PlanResult plan(const Block &block, VitoClock c, uint16_t duration, BoostPlan *p) {
  return plan_boost(c, block.data(), duration, p);
}

}  // namespace

// --- Codage des blocs ---------------------------------------------------------

TEST(schedule_time_encoding) {
  CHECK_EQ(encode_schedule_time(0), 0x00);
  CHECK_EQ(encode_schedule_time(6 * 60 + 10), 0x31);
  CHECK_EQ(encode_schedule_time(8 * 60 + 30), 0x43);
  CHECK_EQ(encode_schedule_time(14 * 60), 0x70);
  CHECK_EQ(encode_schedule_time(21 * 60 + 40), 0xAC);
  CHECK_EQ(encode_schedule_time(23 * 60 + 50), 0xBD);
  CHECK_EQ(encode_schedule_time(24 * 60), 0xC0);
  CHECK_EQ(encode_schedule_time(5), -1);         // pas un multiple de 10
  CHECK_EQ(encode_schedule_time(24 * 60 + 10), -1);
  for (int raw = 0; raw < 256; raw++) {
    const int minutes = decode_schedule_time(static_cast<uint8_t>(raw));
    if (minutes >= 0)
      CHECK_EQ(encode_schedule_time(static_cast<uint16_t>(minutes)), raw);  // codage unique
  }
}

TEST(schedule_slots_round_trip_exact) {
  const std::vector<Block> samples = {
      REAL,
      {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
      {0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
      {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
      {0xFF, 0xFF, 0x31, 0x43, 0xFF, 0xFF, 0xAC, 0xAD},  // emplacements libres au milieu
      {0xAC, 0xAD, 0x31, 0x43, 0xFF, 0xFF, 0xFF, 0xFF},  // non trié
      {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0xB8, 0xC0},  // 4 plages, fin 24:00
  };
  for (const Block &b : samples) {
    ScheduleSlots s;
    CHECK(decode_schedule_slots(b.data(), b.size(), &s) == ScheduleError::NONE);
    uint8_t out[8];
    CHECK(encode_schedule_slots(s, out));
    CHECK(bytes(out) == b);
  }
  // Recherche large : 100 000 blocs valides aléatoires (emplacements libres
  // n'importe où, ordre quelconque, fin à 24:00 possible) se réencodent à
  // l'identique, octet pour octet.
  uint32_t x = 12345;
  auto rnd = [&x](uint32_t n) {
    x = x * 1103515245u + 12345u;
    return (x >> 8) % n;
  };
  auto raw = [](uint16_t minutes) { return static_cast<uint8_t>(minutes / 60 * 8 + minutes % 60 / 10); };
  int valid = 0;
  for (int n = 0; n < 100000; n++) {
    Block b(8, 0xFF);
    for (int slot = 0; slot < 4; slot++) {
      if (rnd(3) == 0)
        continue;
      const uint16_t start = static_cast<uint16_t>(rnd(144) * 10);
      const uint16_t end = static_cast<uint16_t>(start + (1 + rnd((1440 - start) / 10)) * 10);
      b[2 * slot] = raw(start);
      b[2 * slot + 1] = raw(end);
    }
    ScheduleSlots s;
    if (decode_schedule_slots(b.data(), b.size(), &s) != ScheduleError::NONE)
      continue;
    valid++;
    uint8_t out[8];
    CHECK(encode_schedule_slots(s, out));
    CHECK(bytes(out) == b);
  }
  CHECK_EQ(valid, 100000);
}

TEST(schedule_slots_rejects_invalid) {
  ScheduleSlots s;
  for (const Block &b : std::vector<Block>{{0x31, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                                           {0x36, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                                           {0x43, 0x31, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                                           {0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}})
    CHECK(decode_schedule_slots(b.data(), b.size(), &s) != ScheduleError::NONE);
  CHECK(decode_schedule_slots(REAL.data(), 7, &s) == ScheduleError::BAD_LENGTH);
  ScheduleSlots bad;
  bad.used[0] = true, bad.start[0] = 605, bad.end[0] = 700;  // 10:05
  uint8_t out[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(!encode_schedule_slots(bad, out));
  CHECK_EQ(out[0], 1);  // rien d'écrit
  bad.start[0] = 700, bad.end[0] = 700;
  CHECK(!encode_schedule_slots(bad, out));
}

// --- Horloge 0x088E -----------------------------------------------------------

TEST(vito_clock_decoding) {
  VitoClock c;
  const uint8_t sunday[8] = {0x20, 0x26, 0x11, 0x15, 0x07, 0x14, 0x03, 0x27};  // 2026-11-15 14:03:27
  CHECK(decode_vito_clock(sunday, 8, &c) == ClockError::NONE);
  CHECK_EQ(c.year, 2026);
  CHECK_EQ(c.weekday, 6);  // dimanche -> bloc 0x2230
  CHECK_EQ(c.minute_of_day(), 14 * 60 + 3);
  CHECK_EQ(c.second_of_day(), 14u * 3600 + 3 * 60 + 27);
  CHECK(format_vito_clock(c) == "2026-11-15 14:03:27 (dimanche)");
  CHECK_EQ(loop_block_address(c.weekday), 0x2230);
  const uint8_t sunday0[8] = {0x20, 0x26, 0x11, 0x15, 0x00, 0x14, 0x03, 0x27};  // dimanche codé 0
  CHECK(decode_vito_clock(sunday0, 8, &c) == ClockError::NONE);
  const uint8_t wrong_day[8] = {0x20, 0x26, 0x11, 0x15, 0x01, 0x14, 0x03, 0x27};  // lundi ?
  CHECK(decode_vito_clock(wrong_day, 8, &c) == ClockError::WEEKDAY_MISMATCH);
  const uint8_t not_bcd[8] = {0x20, 0x26, 0x1A, 0x15, 0x07, 0x14, 0x03, 0x27};
  CHECK(decode_vito_clock(not_bcd, 8, &c) == ClockError::NOT_BCD);
  const uint8_t month13[8] = {0x20, 0x26, 0x13, 0x15, 0x07, 0x14, 0x03, 0x27};
  CHECK(decode_vito_clock(month13, 8, &c) == ClockError::OUT_OF_RANGE);
  const uint8_t hour24[8] = {0x20, 0x26, 0x11, 0x15, 0x07, 0x24, 0x03, 0x27};
  CHECK(decode_vito_clock(hour24, 8, &c) == ClockError::OUT_OF_RANGE);
  const uint8_t feb29_2027[8] = {0x20, 0x27, 0x02, 0x29, 0x01, 0x10, 0x00, 0x00};
  CHECK(decode_vito_clock(feb29_2027, 8, &c) == ClockError::OUT_OF_RANGE);
  const uint8_t feb29_2028[8] = {0x20, 0x28, 0x02, 0x29, 0x02, 0x10, 0x00, 0x00};  // mardi
  CHECK(decode_vito_clock(feb29_2028, 8, &c) == ClockError::NONE);
  CHECK(decode_vito_clock(sunday, 7, &c) == ClockError::BAD_LENGTH);
  CHECK_EQ(weekday_of_date(2024, 1, 1), 0);
  CHECK_EQ(weekday_of_date(2000, 2, 29), 1);
  CHECK_EQ(weekday_of_date(2026, 11, 15), 6);
}

// --- Calcul de la plage --------------------------------------------------------

TEST(boost_plan_required_times) {
  BoostPlan p;
  struct Case {
    uint8_t h, m, s;
    uint16_t start, end;
  };
  // Durée 10 min, marge 2 min : fin = plafond(t + 12 min) à la dizaine.
  for (const Case &c : {Case{14, 0, 0, 14 * 60, 14 * 60 + 20}, Case{14, 1, 0, 14 * 60, 14 * 60 + 20},
                        Case{14, 9, 0, 14 * 60, 14 * 60 + 30}, Case{14, 9, 59, 14 * 60, 14 * 60 + 30},
                        Case{14, 10, 0, 14 * 60 + 10, 14 * 60 + 30}, Case{14, 59, 0, 14 * 60 + 50, 15 * 60 + 20}}) {
    CHECK(plan(REAL, at(c.h, c.m, c.s), 10, &p) == PlanResult::OK);
    CHECK_EQ(p.start_min, c.start);
    CHECK_EQ(p.end_min, c.end);
    CHECK(p.end_min * 60u >= (c.h * 3600u + c.m * 60u + c.s) + 10 * 60 + BOOST_MARGIN_S);
  }
  CHECK(plan(REAL, at(23, 50), 10, &p) == PlanResult::CROSSES_MIDNIGHT);
  CHECK(plan(REAL, at(23, 59), 10, &p) == PlanResult::CROSSES_MIDNIGHT);
  CHECK(plan(REAL, at(23, 38, 0), 10, &p) == PlanResult::OK);  // fin 23:50
  CHECK_EQ(p.end_min, BOOST_LAST_END_MIN);
  CHECK(plan(REAL, at(23, 38, 1), 10, &p) == PlanResult::CROSSES_MIDNIGHT);  // 24:00 refusé
}

TEST(boost_plan_inserts_sorted_and_preserves_ranges) {
  BoostPlan p;
  CHECK(plan(REAL, at(14, 3), 10, &p) == PlanResult::OK);
  // 06:10-08:30, 14:00-14:20, 21:40-21:50 : la plage 21:40 avance d'un
  // emplacement, ses octets sont conservés.
  CHECK(bytes(p.temporary) == (Block{0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF}));
  CHECK(bytes(p.original) == REAL);
  CHECK_EQ(p.slot, 1);
  // Avant la première plage, puis après la dernière.
  CHECK(plan(REAL, at(5, 0), 20, &p) == PlanResult::OK);
  CHECK(bytes(p.temporary) == (Block{0x28, 0x2B, 0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF}));  // 05:00-05:30
  CHECK(plan(REAL, at(22, 0), 30, &p) == PlanResult::OK);
  CHECK(bytes(p.temporary) == (Block{0x31, 0x43, 0xAC, 0xAD, 0xB0, 0xB4, 0xFF, 0xFF}));  // 22:00-22:40
  // Juste après une plage : contiguë, acceptée.
  CHECK(plan(REAL, at(8, 30), 10, &p) == PlanResult::OK);
  CHECK(bytes(p.temporary) == (Block{0x31, 0x43, 0x43, 0x45, 0xAC, 0xAD, 0xFF, 0xFF}));
}

TEST(boost_plan_property_over_a_day) {
  // Toute heure, toute durée : plage contenant l'heure, durée restante >=
  // demandée + marge, résolution 10 min, plages d'origine intactes.
  int ok = 0;
  for (uint32_t t = 0; t < 24 * 3600; t += 37) {
    for (uint16_t d : BOOST_DURATIONS) {
      BoostPlan p;
      const PlanResult r = plan(REAL, at(t / 3600, t / 60 % 60, t % 60), d, &p);
      if (r != PlanResult::OK)
        continue;
      ok++;
      CHECK(p.start_min * 60u <= t && t < p.end_min * 60u);
      CHECK(p.end_min * 60u >= t + d * 60u + BOOST_MARGIN_S);
      CHECK(p.end_min * 60u < t + d * 60u + BOOST_MARGIN_S + 600u);  // pas plus que nécessaire
      CHECK(p.start_min % 10 == 0 && p.end_min % 10 == 0 && p.end_min <= BOOST_LAST_END_MIN);
      ScheduleSlots before, after;
      CHECK(decode_schedule_slots(p.original, 8, &before) == ScheduleError::NONE);
      CHECK(decode_schedule_slots(p.temporary, 8, &after) == ScheduleError::NONE);
      CHECK(schedule_slots_canonical(after));
      CHECK_EQ(after.used_count(), before.used_count() + 1);
      for (uint8_t i = 0; i < 4; i++) {
        if (!before.used[i])
          continue;
        bool kept = false;
        for (uint8_t j = 0; j < 4; j++)
          kept |= after.used[j] && after.start[j] == before.start[i] && after.end[j] == before.end[i];
        CHECK(kept);
      }
    }
  }
  CHECK(ok > 5000);
}

TEST(boost_plan_refusals) {
  BoostPlan p;
  CHECK(plan(REAL, at(7, 0), 10, &p) == PlanResult::ALREADY_ACTIVE);
  CHECK_EQ(p.remaining_min, 90);
  CHECK(bytes(p.temporary) == REAL);  // rien de modifié
  CHECK(plan(REAL, at(8, 29, 30), 10, &p) == PlanResult::ALREADY_ACTIVE);
  CHECK_EQ(p.remaining_min, 0);
  CHECK(plan(REAL, at(21, 35), 10, &p) == PlanResult::OVERLAP);  // 21:30-21:50 chevaucherait 21:40
  const Block full = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0xB8, 0xBA};
  CHECK(plan(full, at(14, 0), 10, &p) == PlanResult::NO_FREE_SLOT);
  const Block gap = {0xFF, 0xFF, 0x31, 0x43, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(plan(gap, at(14, 0), 10, &p) == PlanResult::UNSUPPORTED_LAYOUT);
  const Block unsorted = {0xAC, 0xAD, 0x31, 0x43, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(plan(unsorted, at(14, 0), 10, &p) == PlanResult::UNSUPPORTED_LAYOUT);
  const Block invalid = {0x31, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(plan(invalid, at(14, 0), 10, &p) == PlanResult::INVALID_BLOCK);
  CHECK(plan(REAL, at(14, 0), 15, &p) == PlanResult::INVALID_DURATION);
  CHECK(plan(REAL, at(14, 0), 0, &p) == PlanResult::INVALID_DURATION);
  const Block empty = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(plan(empty, at(14, 0), 10, &p) == PlanResult::OK);
  CHECK(bytes(p.temporary) == (Block{0x70, 0x72, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(boost_plan_vs_manual_test_of_15_november) {
  // Essai depuis le panneau, 2026-11-15 (dimanche) vers 14:03 : la
  // régulation a relu « 06:10-08:30, 14:00-14:10 » (la plage 21:40-21:50 a
  // disparu). VissLink, lui, conserve 21:40-21:50 et garantit 10 min.
  BoostPlan p;
  CHECK(plan(REAL, at(14, 3), 10, &p) == PlanResult::OK);
  CHECK_EQ(p.start_min, 14 * 60);
  CHECK_EQ(p.end_min, 14 * 60 + 20);
  const Block panel = {0x31, 0x43, 0x70, 0x71, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(bytes(p.temporary) != panel);
}

TEST(schedule_write_whitelist_is_loop_only) {
  for (uint16_t a : {0x2200, 0x2208, 0x2210, 0x2218, 0x2220, 0x2228, 0x2230})
    CHECK(is_schedule_write_whitelisted(a));
  for (uint16_t a : {0x2000, 0x2100, 0x3000, 0x2238, 0x2201, 0x21F8, 0x0846, 0x088E, 0x2301, 0x0000, 0x2302, 0x2303,
                     0x3302, 0x3303})
    CHECK(!is_schedule_write_whitelisted(a));
}

// --- Machine à états (pilote synchrone) ---------------------------------------

namespace {

/// Régulation et flash simulées, exécution synchrone des actions.
struct Rig {
  std::map<uint16_t, Block> regs;
  bool flash_has = false;
  BoostRecord flash;
  bool save_fails = false, read_fails = false, write_lost = false, write_ignored = false, write_unsent = false;
  int writes = 0, saves = 0;
  std::vector<std::string> trace;
  uint32_t now = 1000;
  /// Horloge de la régulation : clock_base_s (seconde du jour) à now = t0.
  uint32_t clock_base_s = 14u * 3600 + 3 * 60;
  uint32_t t0 = 1000;
  /// Instants (millis) des écritures émises.
  std::vector<uint32_t> write_times;
  /// Date de la régulation : 2026-11-<day>, jour KW 1 = lundi ... 7 = dimanche
  /// (v0.7.1 : les 7 jours ; dimanche 15 par défaut).
  uint8_t day = 15, weekday_kw = 7;

  Rig() {
    regs[0x2230] = REAL;
    set_clock(14, 3, 0);
  }
  /// v0.5.2 : démarre à une autre heure et/ou à une autre valeur de millis().
  Rig(uint32_t clock_s, uint32_t start_ms) : now(start_ms), clock_base_s(clock_s), t0(start_ms) {
    regs[0x2230] = REAL;
    tick_clock();
  }
  void tick_clock() {
    const uint32_t t = clock_base_s + (now - t0) / 1000;  // différence modulo 2^32
    set_clock(t / 3600 % 24, t / 60 % 60, t % 60);
  }
  void set_clock(uint8_t h, uint8_t m, uint8_t s) {
    auto bcd = [](uint8_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); };
    regs[VITO_CLOCK_ADDRESS] = {0x20, 0x26, 0x11, bcd(day), bcd(weekday_kw), bcd(h), bcd(m), bcd(s)};
  }
  /// Exécute une action ; renvoie false si aucune.
  bool step(BoostSequencer &q) {
    const BoostAction a = q.poll(now);
    switch (a.type) {
      case BoostActionType::NONE:
        return false;
      case BoostActionType::READ:
        trace.push_back("R" + std::to_string(a.address));
        if (read_fails)
          q.on_read(false, nullptr, 0, now);
        else
          q.on_read(true, regs[a.address].data(), static_cast<uint8_t>(regs[a.address].size()), now);
        return true;
      case BoostActionType::WRITE:
        trace.push_back("W" + std::to_string(a.address));
        if (write_unsent) {
          q.on_write(false, false, now);
          return true;
        }
        writes++;
        write_times.push_back(now);
        if (!write_ignored)
          regs[a.address] = Block(a.data, a.data + a.length);
        q.on_write(true, !write_lost, now);
        return true;
      case BoostActionType::SAVE:
        trace.push_back("S");
        saves++;
        if (!save_fails) {
          flash = a.record;
          flash_has = true;
        }
        q.on_saved(!save_fails, now);
        return true;
      case BoostActionType::CLEAR:
        trace.push_back("C");
        saves++;
        flash_has = false;
        q.on_saved(true, now);
        return true;
    }
    return false;
  }
  /// Pas de 1 s ; correct au débordement de millis().
  void run(BoostSequencer &q, uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 1000) {
      while (step(q)) {
      }
      now += 1000;
      tick_clock();  // l'horloge de la régulation avance avec le temps simulé
    }
  }
  /// Pas de 1 s jusqu'à ce que `done` soit vrai (au plus `limit_ms`).
  template<typename F> bool run_until(BoostSequencer &q, F done, uint32_t limit_ms) {
    for (uint32_t elapsed = 0; elapsed <= limit_ms; elapsed += 1000) {
      while (!done() && step(q)) {
      }
      if (done())
        return true;
      now += 1000;
      tick_clock();
    }
    return false;
  }
};

BoostConfig boost_config() { return BoostConfig(); }

HaTime ha_at(uint16_t minute, uint8_t weekday = 6) {
  HaTime h;
  h.valid = true, h.weekday = weekday, h.minute_of_day = minute;
  return h;
}

}  // namespace

TEST(boost_sequence_nominal) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.start(10, ha_at(14 * 60 + 3), r.now) == BoostStart::ACCEPTED);
  CHECK(q.start(10, ha_at(14 * 60 + 3), r.now) == BoostStart::BUSY);
  r.run(q, 2000);
  CHECK(q.state() == BoostState::ACTIVE);
  CHECK(q.active());
  CHECK(r.flash_has);  // sauvegarde présente pendant la plage
  CHECK(bytes(r.flash.original) == REAL);
  CHECK(r.regs[0x2230] == (Block{0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF}));
  // Ordre imposé : horloge, bloc, SAUVEGARDE, écriture, relecture.
  CHECK((std::vector<std::string>(r.trace.begin(), r.trace.begin() + 5) ==
         std::vector<std::string>{"R2190", "R8752", "S", "W8752", "R8752"}));
  r.run(q, 20 * 60 * 1000);  // jusqu'à 14:23 et au-delà
  CHECK(q.idle());
  CHECK(r.regs[0x2230] == REAL);
  CHECK(!r.flash_has);
  CHECK(q.status() == "Programmation restaurée (vérifiée par relecture), après 10 min 00 s (durée demandée : 10 min)");
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(r.saves, 2);  // une sauvegarde, un effacement : usure minimale
}

TEST(boost_refusals_before_any_write) {
  struct Case {
    const char *name;
    HaTime ha;
    bool read_fails;
    Block clock;
  };
  const Block good_clock = {0x20, 0x26, 0x11, 0x15, 0x07, 0x14, 0x03, 0x00};
  for (const Case &c : {Case{"HA indisponible", HaTime(), false, good_clock},
                        Case{"mauvais jour HA", ha_at(843, 5), false, good_clock},
                        Case{"horloges décalées", ha_at(843 + 6), false, good_clock},
                        Case{"horloge illisible", ha_at(843), true, good_clock},
                        Case{"jour incohérent", ha_at(843), false,
                             {0x20, 0x26, 0x11, 0x15, 0x03, 0x14, 0x03, 0x00}},
                        Case{"non BCD", ha_at(843), false, {0x20, 0x26, 0x11, 0x15, 0x07, 0x1F, 0x03, 0x00}}}) {
    Rig r;
    r.regs[VITO_CLOCK_ADDRESS] = c.clock;
    r.read_fails = c.read_fails;
    BoostSequencer q;
    q.configure(BoostConfig());
    q.boot(nullptr);
    CHECK(q.start(10, c.ha, r.now) == BoostStart::ACCEPTED);
    while (r.step(q)) {
    }
    CHECK(q.idle());
    CHECK_EQ(r.writes, 0);
    CHECK_EQ(r.saves, 0);
    CHECK(q.status().rfind("Refusé", 0) == 0);
    if (vltest::failures() > 0)
      std::printf("    %s: %s\n", c.name, q.status().c_str());
  }
  // Déjà dans une plage : rien n'est écrit.
  Rig r;
  r.set_clock(7, 0, 0);
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.start(10, ha_at(7 * 60), r.now) == BoostStart::ACCEPTED);
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK_EQ(r.writes, 0);
  CHECK(q.status().rfind("Déjà programmé", 0) == 0);
  CHECK(q.start(15, ha_at(7 * 60), r.now) == BoostStart::BAD_DURATION);
}

TEST(boost_backup_failure_prevents_write) {
  Rig r;
  r.save_fails = true;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(10, ha_at(843), r.now);
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK_EQ(r.writes, 0);
  CHECK(r.regs[0x2230] == REAL);
}

TEST(boost_write_not_applied_or_unsent) {
  {  // accusé, mais bloc inchangé : refus constaté, sauvegarde effacée
    Rig r;
    r.write_ignored = true;
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    while (r.step(q)) {
    }
    CHECK(q.idle());
    CHECK_EQ(r.writes, 1);
    CHECK(!r.flash_has);
    CHECK(q.status() == "Refusé par la régulation : bloc inchangé après écriture");
  }
  {  // trame jamais émise : rien n'a été écrit
    Rig r;
    r.write_unsent = true;
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    while (r.step(q)) {
    }
    CHECK(q.idle());
    CHECK(!r.flash_has);
    CHECK(r.regs[0x2230] == REAL);
  }
}

TEST(boost_unexpected_read_back_blocks_everything) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(10, ha_at(843), r.now);
  // Exécution pas à pas : la régulation « corrige » le bloc après l'écriture.
  while (q.state() != BoostState::VERIFY_PENDING)
    r.step(q);
  r.regs[0x2230] = {0x31, 0x43, 0x70, 0x71, 0xAC, 0xAD, 0xFF, 0xFF};
  r.step(q);
  CHECK(q.recovery_required());
  CHECK(r.flash_has);  // sauvegarde conservée
  CHECK(q.start(10, ha_at(843), r.now) == BoostStart::RECOVERY_PENDING);
  r.run(q, 60 * 60 * 1000);
  CHECK_EQ(r.writes, 1);  // aucune écriture automatique
  CHECK(q.recovery_required());
}

TEST(boost_panel_change_is_never_overwritten) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(10, ha_at(843), r.now);
  r.run(q, 2000);
  CHECK(q.state() == BoostState::ACTIVE);
  const Block edited = {0x31, 0x43, 0x70, 0x72, 0xA8, 0xAD, 0xFF, 0xFF};  // modifié sur le panneau
  r.regs[0x2230] = edited;
  r.run(q, 25 * 60 * 1000);
  CHECK(q.recovery_required());
  CHECK(r.regs[0x2230] == edited);  // non écrasé
  CHECK_EQ(r.writes, 1);
  CHECK(r.flash_has);
  // Bouton « restaurer » : refuse toujours (ni origine ni temporaire).
  CHECK(q.request_restore(r.now));
  while (r.step(q)) {
  }
  CHECK(q.recovery_required());
  CHECK(r.regs[0x2230] == edited);
  // « Oublier » : efface la sauvegarde, n'écrit rien.
  CHECK(q.forget());
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK(!r.flash_has);
  CHECK(r.regs[0x2230] == edited);
}

TEST(boost_early_restore_button) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(30, ha_at(843), r.now);
  r.run(q, 2000);
  CHECK(q.state() == BoostState::ACTIVE);
  CHECK(q.request_restore(r.now));
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK(r.regs[0x2230] == REAL);
  CHECK(!r.flash_has);
}

TEST(boost_restore_failures_lead_to_recovery) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(10, ha_at(843), r.now);
  r.run(q, 2000);
  r.write_ignored = true;  // la restauration n'est jamais appliquée
  r.run(q, 40 * 60 * 1000);
  CHECK(q.recovery_required());
  CHECK_EQ(r.writes, 1 + 3);  // 3 tentatives, chacune précédée d'une relecture
  CHECK(r.flash_has);
}

TEST(boost_power_cut_after_every_action) {
  // Coupure de l'ESP après la k-ième action, pour chaque k : au redémarrage,
  // jamais d'écriture automatique ; si une sauvegarde existe, récupération
  // requise ; le bouton « restaurer » ramène toujours le bloc d'origine, ou
  // constate qu'il l'est déjà ; sans sauvegarde, le bloc est intact.
  int cuts = 0;
  for (int k = 1; k <= 12; k++) {
    Rig r;
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    int done = 0;
    while (done < k) {
      if (!r.step(q)) {
        r.now += 60000;  // attendre la fin de la durée si besoin
        const uint32_t t = 14u * 3600 + 3 * 60 + (r.now - 1000) / 1000;
        r.set_clock(t / 3600 % 24, t / 60 % 60, t % 60);
        if (q.idle())
          break;
        continue;
      }
      done++;
    }
    if (q.idle() && done < k)
      break;  // séquence terminée avant la k-ième action
    cuts++;
    // --- coupure : RAM perdue, flash conservée ---
    BoostSequencer after;
    after.configure(boost_config());
    after.boot(r.flash_has ? &r.flash : nullptr);
    const int writes_before = r.writes;
    r.run(after, 60 * 60 * 1000);
    CHECK_EQ(r.writes, writes_before);  // rien d'automatique
    if (r.flash_has) {
      CHECK(after.recovery_required());
      CHECK(after.start(10, ha_at(843), r.now) == BoostStart::RECOVERY_PENDING);
      CHECK(after.request_restore(r.now));
      while (r.step(after)) {
      }
      CHECK(after.idle());
      CHECK(!r.flash_has);
    } else {
      CHECK(after.idle());
    }
    CHECK(r.regs[0x2230] == REAL);
    if (vltest::failures() > 0)
      std::printf("    coupure après l'action %d\n", k);
  }
  // 9 actions (v0.5.2 : plus de lecture de l'horloge avant la restauration).
  CHECK(cuts >= 9);
}

TEST(boost_record_integrity) {
  BoostRecord r;
  CHECK(!boost_record_valid(r));
  r.magic = BOOST_RECORD_MAGIC;
  r.version = BOOST_RECORD_VERSION;
  r.weekday = 6;
  r.address = 0x2230;
  std::memcpy(r.original, REAL.data(), 8);
  r.check = boost_record_checksum(r);
  CHECK(boost_record_valid(r));
  BoostRecord bad = r;
  bad.original[3] ^= 1;
  CHECK(!boost_record_valid(bad));  // contenu altéré
  bad = r;
  bad.address = 0x2000;
  bad.check = boost_record_checksum(bad);
  CHECK(!boost_record_valid(bad));  // jamais un autre programme
  BoostSequencer q;
  q.boot(&bad);
  CHECK(q.idle());
}

TEST(boost_rate_limits) {
  Rig r;
  BoostSequencer q;
  BoostConfig cfg = boost_config();
  cfg.max_per_day = 2;
  q.configure(cfg);
  q.boot(nullptr);
  CHECK(q.start(10, ha_at(843), r.now) == BoostStart::ACCEPTED);
  r.run(q, 30 * 60 * 1000);
  CHECK(q.idle());
  CHECK(q.start(10, ha_at(873), r.now) == BoostStart::ACCEPTED);  // 30 min plus tard
  r.run(q, 30 * 60 * 1000);
  CHECK(q.start(10, ha_at(903), r.now) == BoostStart::ACCEPTED);
  while (r.step(q)) {
  }
  CHECK(q.status() == "Refusé : nombre maximal de déclenchements atteint pour aujourd'hui");
  Rig s;
  BoostSequencer p;
  p.configure(boost_config());
  p.boot(nullptr);
  CHECK(p.start(10, ha_at(843), s.now) == BoostStart::ACCEPTED);
  s.run(p, 2000);
  CHECK(p.state() == BoostState::ACTIVE);
  CHECK(p.request_restore(s.now));  // fin anticipée
  s.run(p, 2000);
  CHECK(p.idle());
  CHECK(p.start(10, ha_at(843), s.now) == BoostStart::RATE_LIMITED);  // 15 min minimum
}

// --- v0.5.1 : actualisation de l'affichage après une modification confirmée ----

TEST(boost_take_refresh_only_after_confirmed_change) {
  Rig r;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK_EQ(q.take_refresh(), 0);
  q.start(10, ha_at(14 * 60 + 3), r.now);
  // Pendant toute la préparation, l'écriture et la vérification : rien.
  while (q.state() != BoostState::ACTIVE) {
    CHECK_EQ(q.take_refresh(), 0);
    if (!r.step(q))
      break;
  }
  CHECK(q.state() == BoostState::ACTIVE);
  CHECK_EQ(q.take_refresh(), 0x2230);  // plage temporaire confirmée par la relecture
  CHECK_EQ(q.take_refresh(), 0);       // une seule fois
  // Restauration : rien pendant RESTORE_*, une fois revenu au repos.
  q.request_restore(r.now);
  while (r.step(q)) {
    if (!q.idle())
      CHECK_EQ(q.take_refresh(), 0);
  }
  CHECK(q.idle());
  CHECK(q.status().rfind("Programmation restaurée (vérifiée par relecture)", 0) == 0);
  CHECK_EQ(q.take_refresh(), 0x2230);
  CHECK_EQ(q.take_refresh(), 0);
}

TEST(boost_no_refresh_without_confirmed_change) {
  {
    Rig r;  // écriture ignorée par la régulation : rien n'a changé
    r.write_ignored = true;
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    r.run(q, 2000);
    CHECK(q.idle());
    CHECK(q.status() == "Refusé par la régulation : bloc inchangé après écriture");
    CHECK_EQ(q.take_refresh(), 0);
  }
  {
    Rig r;  // relecture illisible : état indéterminé, aucune lecture de plus
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    while (q.state() != BoostState::VERIFY_PENDING && r.step(q)) {
    }
    r.read_fails = true;
    r.run(q, 2000);
    CHECK(q.recovery_required());
    CHECK_EQ(q.take_refresh(), 0);
  }
  {
    Rig r;  // refus avant toute écriture (heure de Home Assistant indisponible)
    BoostSequencer q;
    q.configure(BoostConfig());
    q.boot(nullptr);
    q.start(10, HaTime(), r.now);
    r.run(q, 2000);
    CHECK(q.idle());
    CHECK_EQ(q.take_refresh(), 0);
  }
  {
    Rig r;  // sauvegarde oubliée : aucune relecture confirmée
    BoostSequencer q;
    q.configure(boost_config());
    BoostRecord rec;
    rec.magic = BOOST_RECORD_MAGIC;
    rec.version = BOOST_RECORD_VERSION;
    rec.weekday = 6;
    rec.address = 0x2230;
    std::memcpy(rec.original, REAL.data(), 8);
    const Block temp = {0x31, 0x43, 0x70, 0x72, 0xAC, 0xAD, 0xFF, 0xFF};
    std::memcpy(rec.temporary, temp.data(), 8);
    rec.check = boost_record_checksum(rec);
    q.boot(&rec);
    CHECK(q.forget());
    r.run(q, 2000);
    CHECK(q.idle());
    CHECK_EQ(q.take_refresh(), 0);
  }
}

TEST(boost_refresh_after_recovery) {
  for (bool already_original : {false, true}) {
    Rig r;
    BoostSequencer q;
    q.configure(boost_config());
    q.boot(nullptr);
    q.start(10, ha_at(843), r.now);
    r.run(q, 2000);
    CHECK(q.state() == BoostState::ACTIVE);
    // Coupure pendant la plage : nouvelle instance, sauvegarde relue.
    BoostSequencer after;
    after.configure(boost_config());
    after.boot(&r.flash);
    CHECK(after.recovery_required());
    CHECK_EQ(after.take_refresh(), 0);
    if (already_original)
      r.regs[0x2230] = REAL;  // corrigé depuis le panneau
    CHECK(after.request_restore(r.now));
    r.run(after, 2000);
    CHECK(after.idle());
    CHECK_EQ(after.take_refresh(), 0x2230);
    CHECK(r.regs[0x2230] == REAL);
  }
}

namespace {

struct FakePort : BoostRefreshPort {
  bool declared = true, idle = true, accept = true;
  std::vector<uint16_t> pending;
  int requests = 0, cancels = 0;
  bool is_schedule_block(uint16_t a) const override { return declared && a == 0x2230; }
  bool request_refresh(uint16_t a) override {
    requests++;
    if (!accept)
      return false;
    pending.push_back(a);
    return true;
  }
  bool cancel_refresh(uint16_t a) override {
    for (auto it = pending.begin(); it != pending.end(); ++it) {
      if (*it == a) {
        pending.erase(it);
        cancels++;
        return true;
      }
    }
    return false;
  }
  bool refresh_pending(uint16_t a) const override {
    for (uint16_t p : pending)
      if (p == a)
        return true;
    return false;
  }
  bool writes_idle() const override { return idle; }
};

/// Séquenceur amené en ACTIVE (plage confirmée).
void to_active(Rig &r, BoostSequencer &q) {
  q.configure(boost_config());
  q.boot(nullptr);
  q.start(10, ha_at(843), r.now);
  r.run(q, 2000);
}

}  // namespace

TEST(boost_relay_submits_once_when_quiescent) {
  Rig r;
  BoostSequencer q;
  FakePort port;
  BoostRefreshRelay relay;
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::NONE);
  to_active(r, q);
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::SUBMITTED);
  CHECK_EQ(relay.address(), 0x2230);
  CHECK(relay.poll(q, port, r.now + 5000) == BoostRefreshRelay::Event::NONE);
  CHECK_EQ(port.requests, 1);
}

TEST(boost_relay_waits_for_other_commands_then_gives_up) {
  Rig r;
  BoostSequencer q;
  FakePort port;
  BoostRefreshRelay relay;
  to_active(r, q);
  port.idle = false;  // commande A1/M2 en attente
  uint32_t t = r.now;
  for (; t < r.now + 10000; t += 100)
    CHECK(relay.poll(q, port, t) == BoostRefreshRelay::Event::NONE);
  CHECK_EQ(port.requests, 0);  // jamais demandée pendant une autre commande
  port.idle = true;
  CHECK(relay.poll(q, port, t) == BoostRefreshRelay::Event::SUBMITTED);

  // Registre occupé en continu : un essai par seconde, abandon après 60 s.
  Rig r2;
  BoostSequencer q2;
  FakePort busy;
  busy.accept = false;
  BoostRefreshRelay relay2;
  to_active(r2, q2);
  int gave_up = 0;
  for (uint32_t u = r2.now; u < r2.now + 70000; u += 50)
    gave_up += relay2.poll(q2, busy, u) == BoostRefreshRelay::Event::GAVE_UP;
  CHECK_EQ(gave_up, 1);
  CHECK(busy.requests >= 60 && busy.requests <= 62);
}

TEST(boost_relay_not_declared_reads_nothing) {
  Rig r;
  BoostSequencer q;
  FakePort port;
  port.declared = false;
  BoostRefreshRelay relay;
  to_active(r, q);
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::NOT_DECLARED);
  CHECK(relay.poll(q, port, r.now + 2000) == BoostRefreshRelay::Event::NONE);
  CHECK_EQ(port.requests, 0);
}

TEST(boost_relay_cancelled_when_a_new_operation_starts) {
  // Restauration demandée avant que l'actualisation soit servie : elle est
  // retirée (rien d'émis), la vérification de la restauration reste seule.
  Rig r;
  BoostSequencer q;
  FakePort port;
  BoostRefreshRelay relay;
  to_active(r, q);
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::SUBMITTED);
  CHECK(q.request_restore(r.now));
  const BoostAction a = q.poll(r.now);  // relecture avant restauration émise
  CHECK(a.type == BoostActionType::READ);
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::CANCELLED);
  CHECK(port.pending.empty());
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::NONE);
  // Fin de la restauration : une nouvelle actualisation.
  q.on_read(true, r.regs[0x2230].data(), 8, r.now);
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK(relay.poll(q, port, r.now) == BoostRefreshRelay::Event::SUBMITTED);
  CHECK_EQ(port.cancels, 1);

  // Déjà servie : rien à retirer.
  Rig r2;
  BoostSequencer q2;
  FakePort port2;
  BoostRefreshRelay relay2;
  to_active(r2, q2);
  relay2.poll(q2, port2, r2.now);
  port2.pending.clear();  // lue par l'ordonnanceur
  relay2.poll(q2, port2, r2.now + 1000);
  q2.request_restore(r2.now);
  q2.poll(r2.now);
  CHECK(relay2.poll(q2, port2, r2.now + 2000) == BoostRefreshRelay::Event::NONE);
  CHECK_EQ(port2.cancels, 0);
}

// --- v0.5.2 : durée demandée = durée de la plage temporaire --------------------

namespace {

/// Bloc du dimanche sans la plage 21:40-21:50 (cas du constat du 15/11 au soir).
const Block MORNING_ONLY = {0x31, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/// Instant de la n-ième écriture émise (0 si absente : le test échoue sans
/// lecture hors limites).
uint32_t write_at(const Rig &r, size_t n) {
  CHECK(r.write_times.size() > n);
  return r.write_times.size() > n ? r.write_times[n] : 0;
}

/// Amène le séquenceur en ACTIVE ; renvoie l'instant de la confirmation.
uint32_t confirm(Rig &r, BoostSequencer &q, uint16_t duration, uint16_t ha_minute) {
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.start(duration, ha_at(ha_minute), r.now) == BoostStart::ACCEPTED);
  CHECK(r.run_until(q, [&] { return q.state() == BoostState::ACTIVE; }, 60000));
  return r.now;
}

}  // namespace

TEST(boost_ten_minutes_restore_after_ten_minutes_not_at_range_end) {
  // Constat matériel : 10 min demandées vers 21:28 (heure Vitotronic) ->
  // plage 21:20-21:50, restauration prévue à 21:52 (v0.5). v0.5.2 :
  // restauration 10 min après la confirmation, vers 21:38:30.
  Rig r(21u * 3600 + 28 * 60 + 30, 5000);
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 21 * 60 + 28);
  CHECK_EQ(q.plan().start_min, 21 * 60 + 20);
  CHECK_EQ(q.plan().end_min, 21 * 60 + 50);  // fenêtre inchangée (arrondie)
  CHECK(r.regs[0x2230] == (Block{0x31, 0x43, 0xAA, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(q.status() == "Bouclage en cours : 10 min, arrêt vers 21:39 (plage 21:20-21:50 programmée)");
  // Une seconde avant l'expiration : rien.
  r.run(q, 599 * 1000);
  CHECK(q.state() == BoostState::ACTIVE);
  CHECK_EQ(r.writes, 1);
  CHECK(r.regs[0x2230] != MORNING_ONLY);
  // Expiration : relecture, restauration, vérification, sans attendre 21:52.
  CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
  CHECK(r.regs[0x2230] == MORNING_ONLY);
  CHECK(!r.flash_has);
  CHECK(q.status() == "Programmation restaurée (vérifiée par relecture), après 10 min 00 s (durée demandée : 10 min)");
  // Heure de la régulation à la restauration : 21:38:30, avant la fin de plage.
  CHECK(r.regs[VITO_CLOCK_ADDRESS] == (Block{0x20, 0x26, 0x11, 0x15, 0x07, 0x21, 0x38, 0x30}));
}

TEST(boost_every_duration_and_start_time_restores_on_time) {
  // Toute durée, plusieurs heures de départ (début, milieu, fin de dizaine) :
  // restauration à durée exacte après la confirmation, toujours avant la fin
  // de la plage programmée.
  int cases = 0;
  for (uint16_t d : BOOST_DURATIONS) {
    for (uint32_t m : {14u * 60 + 0, 14u * 60 + 3, 14u * 60 + 9}) {
      for (uint32_t s : {0u, 59u}) {
        Rig r(m * 60 + s, 1000);
        r.regs[0x2230] = MORNING_ONLY;
        BoostSequencer q;
        const uint32_t confirmed = confirm(r, q, d, static_cast<uint16_t>(m));
        const uint16_t end_min = q.plan().end_min;
        CHECK(r.run_until(q, [&] { return q.idle(); }, 60u * 60 * 1000));
        CHECK_EQ(r.writes, 2);
        CHECK_EQ(write_at(r, 1) - confirmed, d * 60000u);
        CHECK(m * 60 + s + d * 60u < end_min * 60u);  // avant la fin de plage
        CHECK(r.regs[0x2230] == MORNING_ONLY);
        cases++;
      }
    }
  }
  CHECK_EQ(cases, 18);
}

TEST(boost_timer_correct_across_millis_overflow) {
  // millis() déborde pendant la plage, ou exactement à l'expiration.
  for (uint32_t before_wrap : {4u * 60 * 1000, 10u * 60 * 1000, 10u * 60 * 1000 - 1000, 1000u}) {
    Rig r(14u * 3600 + 3 * 60, 0u - before_wrap);
    BoostSequencer q;
    const uint32_t confirmed = confirm(r, q, 10, 14 * 60 + 3);
    r.run(q, 599 * 1000);
    CHECK(q.state() == BoostState::ACTIVE);  // pas de restauration prématurée
    CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
    CHECK_EQ(r.write_times.size(), 2u);
    CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
    CHECK(r.regs[0x2230] == REAL);
    if (vltest::failures() > 0)
      std::printf("    débordement %u ms après le départ\n", before_wrap);
  }
}

TEST(boost_link_lost_retries_15_min_then_manual_recovery) {
  // Liaison perdue avant l'expiration et jamais rétablie dans la fenêtre :
  // relectures toutes les 30 s pendant 15 min, aucune écriture, puis
  // récupération manuelle ; la sauvegarde est conservée.
  Rig r;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 843);
  r.run(q, 9 * 60 * 1000);
  r.read_fails = true;
  r.run(q, 60 * 1000 + 1000);  // expiration à +10 min, première relecture échouée
  const uint32_t expired = confirmed + 10u * 60 * 1000;
  CHECK(q.state() == BoostState::ACTIVE);
  CHECK(q.restoring());       // restauration en cours...
  CHECK(!q.restore_alert());  // ... sans fausse alerte
  CHECK(q.status().rfind("Restauration en cours (relecture impossible) : nouvel essai", 0) == 0);
  CHECK(r.run_until(q, [&] { return q.recovery_required(); }, 20 * 60 * 1000));
  CHECK_EQ(r.now - expired, 15u * 60 * 1000);  // fin de la fenêtre automatique
  size_t reads = 0;
  for (const std::string &t : r.trace)
    reads += t == "R8752";
  CHECK_EQ(reads, 2u + 31u);  // bloc + vérification, puis 31 relectures (0 à 15 min)
  CHECK_EQ(r.writes, 1);
  CHECK(r.flash_has);  // jamais effacée sans restauration confirmée
  CHECK(q.restore_alert());
  CHECK(!q.restoring());
  CHECK(q.status() ==
        "Récupération nécessaire : liaison indisponible (relecture impossible) 15 min après la fin de la durée ; "
        "sinon, supprimer sur le panneau la plage 14:00-14:20 du dimanche");
  // Retour de la liaison APRÈS la fenêtre : aucune écriture automatique.
  r.read_fails = false;
  r.run(q, 60 * 60 * 1000);
  CHECK_EQ(r.writes, 1);
  CHECK(q.recovery_required());
  CHECK(r.regs[0x2230] != REAL);
  // Récupération manuelle : restaurée, alerte retombée, sauvegarde effacée.
  CHECK(q.request_restore(r.now));
  while (r.step(q)) {
  }
  CHECK(q.idle());
  CHECK(!q.restore_alert());
  CHECK(r.regs[0x2230] == REAL);
  CHECK(!r.flash_has);
}

TEST(boost_link_back_within_window_restores_without_alert) {
  // Retour de la liaison 7 min, puis 14 min 50 s après l'expiration (dernière
  // relecture à 15 min) : restauration automatique, après relecture exacte,
  // sans que l'alerte ne soit jamais levée.
  for (uint32_t back_s : {7u * 60, 14u * 60 + 50}) {
    Rig r;
    BoostSequencer q;
    const uint32_t confirmed = confirm(r, q, 10, 843);
    const uint32_t expired = confirmed + 10u * 60 * 1000;
    r.run(q, 9 * 60 * 1000);
    r.read_fails = true;
    bool alert = false, restoring = false;
    while (r.now - expired < back_s * 1000u || r.now < expired) {
      r.run(q, 1000);
      alert |= q.restore_alert();
      restoring |= q.restoring();
    }
    CHECK(restoring);
    CHECK_EQ(r.writes, 1);
    r.read_fails = false;
    CHECK(r.run_until(
        q,
        [&] {
          alert |= q.restore_alert();
          return q.idle();
        },
        31 * 1000));
    CHECK(!alert);
    CHECK_EQ(r.writes, 2);
    CHECK(r.regs[0x2230] == REAL);
    CHECK(!r.flash_has);
    CHECK(!q.restoring());
    CHECK(!q.restore_alert());
    CHECK(q.status().rfind("Programmation restaurée (vérifiée par relecture), après ", 0) == 0);
    if (vltest::failures() > 0)
      std::printf("    retour après %u s\n", back_s);
  }
}

TEST(boost_link_back_just_after_window_needs_manual_recovery) {
  Rig r;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 843);
  const uint32_t expired = confirmed + 10u * 60 * 1000;
  r.run(q, 9 * 60 * 1000);
  r.read_fails = true;
  while (r.now - expired < 15u * 60 * 1000 + 10 * 1000 || r.now < expired)
    r.run(q, 1000);
  CHECK(q.recovery_required());
  r.read_fails = false;
  r.run(q, 30 * 60 * 1000);
  CHECK_EQ(r.writes, 1);
  CHECK(r.flash_has);
}

TEST(boost_restore_frame_not_sent_is_retried_without_counting) {
  Rig r;
  BoostSequencer q;
  confirm(r, q, 10, 843);
  r.write_unsent = true;  // arbitre ou liaison indisponible à l'émission
  r.run(q, 13 * 60 * 1000);
  CHECK(q.restoring());
  CHECK(!q.restore_alert());  // attente normale : pas d'alerte
  CHECK_EQ(r.writes, 1);
  r.write_unsent = false;
  CHECK(r.run_until(q, [&] { return q.idle(); }, 31 * 1000));
  CHECK(r.regs[0x2230] == REAL);
  CHECK(!q.restore_alert());
}

TEST(boost_restore_refused_alerts_with_panel_instructions) {
  Rig r;
  BoostSequencer q;
  confirm(r, q, 10, 843);
  r.write_ignored = true;
  CHECK(r.run_until(q, [&] { return q.restoring() && q.state() == BoostState::ACTIVE; }, 11 * 60 * 1000));
  CHECK(!q.restore_alert());  // tentatives en cours : pas encore d'alerte
  r.run(q, 30 * 60 * 1000);
  CHECK(q.recovery_required());
  CHECK(q.restore_alert());
  CHECK_EQ(r.writes, 1 + 3);  // écritures bornées, chacune après relecture
  CHECK(r.flash_has);
  CHECK(q.status() ==
        "Récupération nécessaire : restauration non appliquée après 3 écritures ; sinon, supprimer sur le panneau "
        "la plage 14:00-14:20 du dimanche");
  // Plus rien d'automatique ensuite.
  r.run(q, 2 * 60 * 60 * 1000);
  CHECK_EQ(r.writes, 4);
}

TEST(boost_reboot_during_restore_window_writes_nothing) {
  // Coupure de l'ESP pendant la fenêtre automatique (liaison perdue) : au
  // redémarrage, ni minuterie ni nouvelle tentative ; récupération manuelle.
  Rig r;
  BoostSequencer q;
  confirm(r, q, 10, 843);
  r.run(q, 9 * 60 * 1000);
  r.read_fails = true;
  r.run(q, 4 * 60 * 1000);
  CHECK(q.restoring());
  BoostSequencer after;
  after.configure(boost_config());
  after.boot(r.flash_has ? &r.flash : nullptr);
  r.read_fails = false;
  r.run(after, 60 * 60 * 1000);
  CHECK(after.recovery_required());
  CHECK(after.restore_alert());
  CHECK(!after.restoring());
  CHECK_EQ(r.writes, 1);
  CHECK(r.flash_has);
}

TEST(boost_default_duration_is_ten_minutes) {
  CHECK(BOOST_DEFAULT_DURATION_INDEX < BOOST_DURATION_COUNT);
  CHECK_EQ(BOOST_DURATIONS[BOOST_DEFAULT_DURATION_INDEX], 10);
}

TEST(boost_panel_change_during_timer_detected_at_expiry) {
  Rig r;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 843);
  const Block edited = {0x31, 0x43, 0x70, 0x72, 0xA8, 0xAD, 0xFF, 0xFF};
  r.run(q, 3 * 60 * 1000);
  r.regs[0x2230] = edited;  // modifié sur le panneau pendant la minuterie
  CHECK(r.run_until(q, [&] { return q.recovery_required(); }, 20 * 60 * 1000));
  CHECK_EQ(r.now - confirmed, 10u * 60 * 1000);  // constaté à l'expiration
  CHECK(r.regs[0x2230] == edited);              // jamais écrasé
  CHECK_EQ(r.writes, 1);
}

TEST(boost_reboot_never_restarts_a_full_duration) {
  // Redémarrage de l'ESP 4 min après la confirmation : le temps écoulé est
  // inconnu. Aucune minuterie relancée, aucune écriture automatique ; la
  // récupération manuelle restaure immédiatement, sans attendre 10 min.
  Rig r;
  BoostSequencer q;
  confirm(r, q, 10, 843);
  r.run(q, 4 * 60 * 1000);
  BoostSequencer after;
  after.configure(boost_config());
  after.boot(r.flash_has ? &r.flash : nullptr);
  CHECK(after.recovery_required());
  CHECK(after.restore_alert());
  CHECK(after.status().find("supprimer sur le panneau la plage 14:00-14:20 du dimanche") != std::string::npos);
  bool active_again = false;
  for (int i = 0; i < 60; i++) {
    r.run(after, 60 * 1000);
    active_again |= after.state() == BoostState::ACTIVE;
  }
  CHECK(!active_again);
  CHECK_EQ(r.writes, 1);
  CHECK(after.start(10, ha_at(843 + 64), r.now) == BoostStart::RECOVERY_PENDING);
  const uint32_t pressed = r.now;
  CHECK(after.request_restore(pressed));
  while (r.step(after)) {
  }
  CHECK(after.idle());
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(write_at(r, 1), pressed);  // restauration immédiate
  CHECK(r.regs[0x2230] == REAL);
  CHECK(after.status() == "Récupération terminée : programmation restaurée (vérifiée)");
}

// --- v0.7.1 : sept jours, plage inscrite, temps restant, appuis répétés -------

TEST(boost_all_seven_days) {
  // v1.0 : tous les jours, sans option. Pour chaque jour (2026-11-09 lundi ...
  // 2026-11-15 dimanche), seul le bloc du jour est écrit, sauvegardé avant,
  // relu, puis restauré à l'identique ; les autres blocs ne bougent pas.
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    Rig r;
    r.day = static_cast<uint8_t>(9 + d);
    r.weekday_kw = static_cast<uint8_t>(d + 1);
    for (uint8_t o = 0; o < SCHEDULE_DAYS; o++)
      r.regs[loop_block_address(o)] = REAL;
    r.tick_clock();
    BoostSequencer q;
    q.configure(BoostConfig());
    q.boot(nullptr);
    CHECK(q.start(10, ha_at(843, d), r.now) == BoostStart::ACCEPTED);
    CHECK(r.run_until(q, [&] { return q.state() == BoostState::ACTIVE; }, 60000));
    const uint16_t block = loop_block_address(d);
    CHECK_EQ(q.record().address, block);
    CHECK(r.flash_has && bytes(r.flash.original) == REAL);  // sauvegarde avant l'écriture
    CHECK(r.trace[2] == "S" && r.trace[3] == "W" + std::to_string(block) && r.trace[4] == "R" + std::to_string(block));
    CHECK(r.regs[block] != REAL);
    CHECK(r.run_until(q, [&] { return q.idle(); }, 11 * 60 * 1000));
    for (uint8_t o = 0; o < SCHEDULE_DAYS; o++)
      CHECK(r.regs[loop_block_address(o)] == REAL);
    CHECK_EQ(r.writes, 2);
    CHECK(!r.flash_has);
  }
}

TEST(boost_range_and_remaining_are_distinct_displays) {
  // 10 min demandées vers 21:28:30 : plage inscrite 21:20-21:50 (30 min),
  // temps restant 10 min à la confirmation, restauration au bout de 10 min.
  Rig r(21u * 3600 + 28 * 60 + 30, 5000);
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.range_text() == "Aucune");
  CHECK_EQ(q.remaining_s(r.now), 0u);
  CHECK(q.start(10, ha_at(21 * 60 + 28), r.now) == BoostStart::ACCEPTED);
  const uint32_t confirmed = r.now;  // synchrone : confirmée dans la même seconde
  CHECK(r.run_until(q, [&] { return q.state() == BoostState::ACTIVE; }, 60000));
  CHECK_EQ(r.now, confirmed);
  CHECK(q.range_text() == "21:20-21:50 (dimanche)");
  CHECK_EQ(q.remaining_s(r.now), 600u);
  r.run(q, 5 * 60 * 1000);
  CHECK_EQ(q.remaining_s(r.now), 300u);
  CHECK(q.range_text() == "21:20-21:50 (dimanche)");  // la plage, elle, ne change pas
  r.run(q, 299 * 1000);
  CHECK_EQ(q.remaining_s(r.now), 1u);
  CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
  CHECK(q.range_text() == "Aucune");
  CHECK_EQ(q.remaining_s(r.now), 0u);
  // Fin anticipée (bouton) : temps restant à 0 immédiatement.
  Rig s;
  BoostSequencer p;
  confirm(s, p, 10, 843);
  CHECK_EQ(p.remaining_s(s.now), 600u);
  CHECK(p.request_restore(s.now));
  CHECK_EQ(p.remaining_s(s.now), 0u);
  p.poll(s.now);  // prise en compte au passage suivant : restauration en cours
  CHECK(p.restoring());
  CHECK(p.range_text() == "14:00-14:20 (dimanche)");  // encore inscrite jusqu'à la relecture
  // Récupération (redémarrage pendant la plage) : plage signalée « peut-être
  // encore inscrite », aucun temps restant.
  Rig t;
  BoostSequencer before;
  confirm(t, before, 10, 843);
  BoostSequencer after;
  after.configure(boost_config());
  after.boot(&t.flash);
  CHECK(after.range_text() == "14:00-14:20 (dimanche) peut-être encore inscrite (récupération)");
  CHECK_EQ(after.remaining_s(t.now), 0u);
}

TEST(boost_repeated_presses_never_extend_the_timer) {
  // Appuis répétés pendant la plage (et juste avant l'expiration) : refusés,
  // la minuterie n'est ni relancée ni prolongée, aucune écriture de plus.
  Rig r;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 843);
  const BoostRecord record = q.record();
  for (uint32_t at : {1u, 60u, 5u * 60, 9u * 60 + 59}) {
    r.run(q, (confirmed + at * 1000) - r.now);
    for (uint16_t d : BOOST_DURATIONS)  // même avec une durée plus longue
      CHECK(q.start(d, ha_at(843 + at / 60), r.now) == BoostStart::BUSY);
    CHECK_EQ(q.remaining_s(r.now), 600u - at);
    CHECK(std::memcmp(&q.record(), &record, sizeof(record)) == 0);  // même plage, même sauvegarde
  }
  CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
  // Juste après : 15 min minimum entre deux déclenchements.
  CHECK(q.start(10, ha_at(853), r.now) == BoostStart::RATE_LIMITED);
  CHECK_EQ(r.writes, 2);
}

// --- v0.8.1 : durée sélectionnée dans Home Assistant --------------------------
// Défaut constaté : 10 min sélectionnées, « Programmation restaurée après
// 30 min 07 s ». La minuterie suit la durée figée à l'appui (ici 30 min,
// sélection précédente encore affichée) ; une sélection de 10 min faite
// ensuite était ignorée en silence.

TEST(boost_v081_ten_minutes_selected_restores_at_ten_not_thirty) {
  // Sélection de 10 min avant l'appui : restauration à 10 min, jamais à 30,
  // quelle que soit la fin de la plage inscrite.
  Rig r(21u * 3600 + 28 * 60 + 30, 5000);
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, BOOST_DURATIONS[0], 21 * 60 + 28);
  CHECK_EQ(q.duration(), 10);
  CHECK_EQ(q.remaining_s(r.now), 600u);
  CHECK(r.run_until(q, [&] { return q.idle(); }, 31u * 60 * 1000));
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);  // pas 30 min
  CHECK(q.status() == "Programmation restaurée (vérifiée par relecture), après 10 min 00 s (durée demandée : 10 min)");
}

TEST(boost_v081_reproduces_thirty_minutes_then_ten_selected_during_boost) {
  // Reproduction exacte : la sélection affichait encore 30 min à l'appui,
  // puis 10 min est choisi pendant le bouclage. Avant v0.8.1 : restauration
  // à 30 min (« après 30 min 0x s »). Désormais : à 10 min.
  Rig r;
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 30, 843);
  CHECK(q.status().find("Bouclage en cours : 30 min") == 0);
  r.run(q, 60 * 1000);  // 1 min plus tard, 10 min sélectionnées
  CHECK(q.change_duration(10, r.now) == BoostDurationChange::SHORTENED);
  CHECK_EQ(q.duration(), 10);
  CHECK(q.status().find("Bouclage en cours : 10 min") == 0);
  CHECK_EQ(q.remaining_s(r.now), 9u * 60);
  r.run(q, 9u * 60 * 1000 - 1000);
  CHECK(q.state() == BoostState::ACTIVE);  // une seconde avant : rien
  CHECK_EQ(r.writes, 1);
  CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
  CHECK_EQ(r.writes, 2);
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
  CHECK(r.regs[0x2230] == MORNING_ONLY);
  CHECK(q.status() == "Programmation restaurée (vérifiée par relecture), après 10 min 00 s (durée demandée : 10 min)");
}

TEST(boost_v081_shorter_selection_after_elapsed_restores_now) {
  // 30 min en cours depuis 12 min, 10 min sélectionnées : déjà écoulée,
  // restauration immédiate.
  Rig r;
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 30, 843);
  r.run(q, 12u * 60 * 1000);
  CHECK(q.change_duration(10, r.now) == BoostDurationChange::SHORTENED);
  const uint32_t selected = r.now;
  CHECK(r.run_until(q, [&] { return q.idle(); }, 5000));
  CHECK_EQ(write_at(r, 1), selected);
  CHECK(write_at(r, 1) - confirmed >= 12u * 60 * 1000);
  CHECK(q.status().find("(durée demandée : 10 min)") != std::string::npos);
}

TEST(boost_v081_selection_during_preparation_is_used) {
  // Sélection changée entre l'appui et la confirmation : la durée la plus
  // courte est retenue pour la minuterie.
  Rig r;
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.start(30, ha_at(843), r.now) == BoostStart::ACCEPTED);
  CHECK(q.change_duration(10, r.now) == BoostDurationChange::SHORTENED);
  CHECK(r.run_until(q, [&] { return q.state() == BoostState::ACTIVE; }, 60000));
  const uint32_t confirmed = r.now;
  CHECK_EQ(q.remaining_s(r.now), 600u);
  CHECK(r.run_until(q, [&] { return q.idle(); }, 31u * 60 * 1000));
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
}

TEST(boost_v081_longer_selection_never_extends) {
  Rig r;
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  const uint32_t confirmed = confirm(r, q, 10, 843);
  r.run(q, 2u * 60 * 1000);
  CHECK(q.change_duration(30, r.now) == BoostDurationChange::LONGER_REFUSED);
  CHECK(q.change_duration(20, r.now) == BoostDurationChange::LONGER_REFUSED);
  CHECK(q.change_duration(10, r.now) == BoostDurationChange::UNCHANGED);
  CHECK_EQ(q.duration(), 10);
  CHECK(r.run_until(q, [&] { return q.idle(); }, 31u * 60 * 1000));
  CHECK_EQ(write_at(r, 1) - confirmed, 10u * 60 * 1000);
}

TEST(boost_v081_selection_when_idle_or_restoring) {
  Rig r;
  r.regs[0x2230] = MORNING_ONLY;
  BoostSequencer q;
  q.configure(boost_config());
  q.boot(nullptr);
  CHECK(q.change_duration(30, r.now) == BoostDurationChange::SELECTED);  // prochain démarrage
  CHECK_EQ(q.duration(), 0);
  CHECK(q.change_duration(15, r.now) == BoostDurationChange::INVALID);
  // Restauration demandée : plus aucun changement.
  BoostSequencer q2;
  confirm(r, q2, 20, 843);
  CHECK(q2.request_restore(r.now));
  CHECK(q2.change_duration(10, r.now) == BoostDurationChange::TOO_LATE);
  CHECK_EQ(q2.duration(), 20);
  CHECK(r.run_until(q2, [&] { return q2.idle(); }, 5000));
  CHECK_EQ(q2.duration(), 0);
}

TEST(boost_v081_duration_index_matches_select_options) {
  // Options du select : « 10 min », « 20 min », « 30 min » (même ordre).
  CHECK_EQ(boost_duration_index(10), 0u);
  CHECK_EQ(boost_duration_index(20), 1u);
  CHECK_EQ(boost_duration_index(30), 2u);
  CHECK_EQ(boost_duration_index(15), BOOST_DURATION_COUNT);
  CHECK_EQ(BOOST_DURATIONS[BOOST_DEFAULT_DURATION_INDEX], 10);
}
