#include "scheduler.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

/// Simule des lectures qui réussissent toutes en `duration_ms`.
std::vector<int> run(RegisterScheduler &s, size_t n, uint32_t start, uint32_t duration, uint32_t read_ms) {
  std::vector<int> counts(n, 0);
  uint32_t now = start;
  while (now - start < duration) {
    const int idx = s.acquire(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    now += read_ms;
    s.finish(true, now);
    counts[idx]++;
  }
  return counts;
}

}  // namespace

TEST(scheduler_individual_intervals) {
  RegisterScheduler s;
  s.add(60000, 0);
  s.add(30000, 0);
  s.add(60000, 0);
  const auto c = run(s, 3, 0, 600000, 500);  // 10 min, 0,5 s par lecture
  CHECK(c[0] >= 10 && c[0] <= 11);
  CHECK(c[1] >= 20 && c[1] <= 21);
  CHECK(c[2] >= 10 && c[2] <= 11);
}

TEST(scheduler_long_intervals) {
  RegisterScheduler s;
  s.add(6UL * 3600 * 1000, 0);  // 6 h
  s.add(3600UL * 1000, 0);      // 1 h
  s.add(30000, 0);
  const auto c = run(s, 3, 0, 12UL * 3600 * 1000, 500);
  CHECK(c[0] >= 2 && c[0] <= 3);
  CHECK(c[1] >= 12 && c[1] <= 13);
  CHECK(c[2] >= 1400);
}

TEST(scheduler_no_duplicate_while_in_flight) {
  RegisterScheduler s;
  s.add(1000, 0);
  s.add(1000, 0);
  CHECK_EQ(s.acquire(0), 0);
  CHECK_EQ(s.current(), 0);
  CHECK_EQ(s.acquire(10), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(50000), RegisterScheduler::NONE);
  s.finish(true, 50000);
  CHECK_EQ(s.current(), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(50000), 1);
}

TEST(scheduler_finish_without_acquire_is_ignored) {
  RegisterScheduler s;
  s.add(1000, 0);
  CHECK(!s.finish(false, 0));
  CHECK_EQ(s.entry(0).failures, 0);
}

TEST(scheduler_no_catch_up_after_stall) {
  RegisterScheduler s;
  s.add(10000, 0);
  CHECK_EQ(s.acquire(0), 0);
  s.finish(true, 95000);  // lecture de 95 s : 9 échéances manquées non rejouées
  CHECK_EQ(s.entry(0).due_ms, 105000u);
  CHECK_EQ(s.acquire(104999), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(105000), 0);
}

TEST(scheduler_keeps_cadence_without_drift) {
  RegisterScheduler s;
  s.add(30000, 0);
  CHECK_EQ(s.acquire(0), 0);
  s.finish(true, 4000);  // 4 s d'attente de synchro
  CHECK_EQ(s.entry(0).due_ms, 30000u);
}

TEST(scheduler_most_overdue_first_and_no_starvation) {
  RegisterScheduler s;
  s.add(1000, 0);
  s.add(1000, 0);
  s.add(1000, 0);
  s.add(3600000, 0);  // registre rare
  // Lectures de 5 s pour des intervalles de 1 s : saturation.
  uint32_t now = 0;
  int waited = 0;
  bool rare_read = false;
  for (int i = 0; i < 8 && !rare_read; i++) {
    const int idx = s.acquire(now);
    CHECK(idx != RegisterScheduler::NONE);
    rare_read = (idx == 3);
    if (!rare_read)
      waited++;
    now += 5000;
    s.finish(true, now);
  }
  CHECK(rare_read);
  CHECK(waited <= 3);  // au plus N-1 lectures d'attente
}

TEST(scheduler_tie_respects_declaration_order) {
  RegisterScheduler s;
  s.add(60000, 0);
  s.add(30000, 0);
  s.add(60000, 0);
  for (int expected = 0; expected < 3; expected++) {
    CHECK_EQ(s.acquire(100 * expected), expected);
    s.finish(true, 100 * expected + 50);
  }
}

TEST(scheduler_limited_retries_then_next_cycle) {
  RegisterScheduler s;
  s.set_max_retries(2);
  s.add(60000, 0);
  CHECK_EQ(s.acquire(0), 0);
  CHECK(!s.finish(false, 100));  // tentative 1
  CHECK_EQ(s.entry(0).due_ms, 100 + RegisterScheduler::RETRY_DELAY_MS);
  CHECK_EQ(s.acquire(1000), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(1100), 0);
  CHECK(!s.finish(false, 1200));  // tentative 2
  CHECK_EQ(s.acquire(2200), 0);
  CHECK(s.finish(false, 2300));  // tentative 3 : abandon pour ce cycle
  CHECK_EQ(s.entry(0).failures, 0);
  CHECK_EQ(s.entry(0).due_ms, 2200u + 60000u);  // cycle suivant
}

TEST(scheduler_failing_register_does_not_block_others) {
  RegisterScheduler s;
  s.set_max_retries(1);
  s.add(10000, 0);  // toujours en échec
  s.add(10000, 0);
  int ok = 0;
  for (uint32_t now = 0; now < 100000;) {
    const int idx = s.acquire(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    now += 1000;
    s.finish(idx == 1, now);
    ok += (idx == 1);
  }
  CHECK(ok >= 9);
}

TEST(scheduler_millis_overflow) {
  RegisterScheduler s;
  const uint32_t start = 0xFFFFFFFFu - 120000;  // 2 min avant le débordement
  s.add(60000, start);
  s.add(30000, start);
  const auto c = run(s, 2, start, 600000, 500);
  CHECK(c[0] >= 10 && c[0] <= 11);
  CHECK(c[1] >= 20 && c[1] <= 21);
}

// --- v0.2.1 : registres multi-blocs (programmations horaires) ----------------

TEST(scheduler_single_block_is_default) {
  RegisterScheduler s;
  s.add(60000, 0);
  CHECK_EQ(s.entry(0).blocks, 1);
  CHECK_EQ(s.block(0), 0);
  s.add(60000, 0, 0);  // 0 bloc : ramené à 1
  CHECK_EQ(s.entry(1).blocks, 1);
}

TEST(scheduler_blocks_spaced_then_interval_from_cycle_start) {
  const uint32_t H6 = 6UL * 3600 * 1000;
  const uint32_t D = RegisterScheduler::BLOCK_DELAY_MS;
  RegisterScheduler s;
  s.add(H6, 1000, 7);
  uint32_t now = 1000;
  for (uint8_t b = 0; b < 7; b++) {
    CHECK_EQ(s.acquire(now), 0);
    CHECK_EQ(s.block(0), b);
    now += 3000;  // lecture de 3 s
    CHECK(!s.finish(true, now));
    if (b < 6) {
      CHECK_EQ(s.entry(0).due_ms, now + D);  // bloc suivant après la pause
      CHECK_EQ(s.acquire(now + D - 1), RegisterScheduler::NONE);
      now += D;
    }
  }
  CHECK_EQ(s.block(0), 0);
  CHECK_EQ(s.entry(0).due_ms, 1000 + H6);  // calé sur le début du cycle, sans dérive
}

TEST(scheduler_block_retries_stay_on_same_block) {
  RegisterScheduler s;
  s.set_max_retries(2);
  s.add(60000, 0, 7);
  CHECK_EQ(s.acquire(0), 0);
  CHECK(!s.finish(true, 100));  // bloc 0 lu
  CHECK_EQ(s.acquire(RegisterScheduler::BLOCK_DELAY_MS + 100), 0);
  CHECK_EQ(s.block(0), 1);
  uint32_t now = RegisterScheduler::BLOCK_DELAY_MS + 200;
  CHECK(!s.finish(false, now));  // tentative 1 du bloc 1
  CHECK_EQ(s.block(0), 1);
  CHECK_EQ(s.entry(0).due_ms, now + RegisterScheduler::RETRY_DELAY_MS);
  now += RegisterScheduler::RETRY_DELAY_MS;
  CHECK_EQ(s.acquire(now), 0);
  CHECK(!s.finish(true, now + 100));  // succès à la 2e tentative
  CHECK_EQ(s.block(0), 2);
  CHECK_EQ(s.entry(0).failures, 0);
}

TEST(scheduler_block_given_up_moves_to_next_block) {
  RegisterScheduler s;
  s.set_max_retries(1);
  s.add(60000, 0, 3);
  CHECK_EQ(s.acquire(0), 0);
  CHECK(!s.finish(false, 100));
  CHECK_EQ(s.acquire(1100), 0);
  CHECK(s.finish(false, 1200));  // abandon du bloc 0
  CHECK_EQ(s.block(0), 1);       // le bloc 1 n'en est pas privé
  CHECK_EQ(s.entry(0).due_ms, 1200 + RegisterScheduler::BLOCK_DELAY_MS);
}

TEST(scheduler_blocks_do_not_starve_others) {
  // Saturation : un registre rapide et quatre programmations. Le registre
  // rapide attend au plus N-1 = 4 lectures, comme avec 5 registres simples.
  RegisterScheduler s;
  s.add(1000, 0);
  for (int i = 0; i < 4; i++)
    s.add(6UL * 3600 * 1000, 0, 7);
  uint32_t now = 0;
  int since_fast = 0, worst = 0;
  for (int i = 0; i < 200; i++) {
    const int idx = s.acquire(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    since_fast = (idx == 0) ? 0 : since_fast + 1;
    if (since_fast > worst)
      worst = since_fast;
    now += 3000;
    s.finish(true, now);
  }
  CHECK(worst <= 4);
}

// --- v0.3 : relecture de vérification après une écriture ---------------------

TEST(scheduler_expedite_reads_first_then_resumes_cadence) {
  RegisterScheduler s;
  s.add(60000, 0);   // 0 : très en retard
  s.add(300000, 0);  // 1 : consigne, 5 min
  s.add(30000, 0);   // 2
  CHECK_EQ(s.acquire(0), 0);
  s.finish(true, 2000);
  CHECK_EQ(s.acquire(2000), 1);
  s.finish(true, 4000);  // prochaine échéance de 1 : 300 000
  CHECK_EQ(s.acquire(4000), 2);
  s.finish(true, 6000);

  // À 100 s : 0 et 2 sont échus (et en retard), 1 ne l'est pas.
  CHECK(s.expedite(1, 100000));
  CHECK_EQ(s.acquire(100000), 1);  // servi avant les registres plus en retard
  s.finish(true, 102000);
  CHECK(!s.entry(1).urgent);
  CHECK_EQ(s.entry(1).due_ms, 300000u);  // cadence d'origine conservée
  CHECK_EQ(s.acquire(102000), 2);         // puis les autres, par retard décroissant
}

TEST(scheduler_expedite_keeps_priority_through_retries) {
  RegisterScheduler s;
  s.add(60000, 0);
  s.add(300000, 0);
  s.acquire(0);
  s.finish(true, 1000);
  s.acquire(1000);
  s.finish(true, 2000);
  CHECK(s.expedite(1, 70000));  // 0 est échu aussi
  CHECK_EQ(s.acquire(70000), 1);
  CHECK(!s.finish(false, 71000));
  CHECK_EQ(s.acquire(72000), 1);  // nouvelle tentative, prioritaire sur 0 (échu)
  CHECK(!s.finish(false, 73000));
  CHECK_EQ(s.acquire(74000), 1);
  CHECK(s.finish(false, 75000));  // abandon : la cadence normale reprend
  CHECK(!s.entry(1).urgent);
  CHECK_EQ(s.acquire(75000), 0);
}

TEST(scheduler_expedite_refused_for_schedules_and_current) {
  RegisterScheduler s;
  s.add(6UL * 3600 * 1000, 0, 7);
  s.add(60000, 0);
  CHECK(!s.expedite(0, 10));  // registre multi-blocs
  CHECK(!s.expedite(5, 10));  // index inconnu
  CHECK_EQ(s.acquire(0), 0);
  CHECK(!s.expedite(0, 10));
  CHECK(!s.expedite(5, 10));
  s.finish(true, 100);
  CHECK_EQ(s.acquire(100), 1);
  CHECK(!s.expedite(1, 200));  // lecture en cours
}
