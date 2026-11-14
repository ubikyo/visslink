#include "scheduler.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

/// Simule des lectures qui réussissent toutes en `duration_ms`.
void run(RegisterScheduler &s, uint32_t start, uint32_t end, uint32_t duration_ms, std::vector<int> &counts) {
  counts.assign(s.size(), 0);
  uint32_t now = start;
  while (static_cast<int32_t>(end - now) > 0) {
    const int idx = s.acquire_next(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    now += duration_ms;
    s.on_success(idx, now);
    counts[idx]++;
  }
}

}  // namespace

TEST(scheduler_individual_intervals) {
  RegisterScheduler s;
  s.add(0x0800, 2, 60000);
  s.add(0x0802, 2, 30000);
  s.add(0x0804, 2, 60000);
  s.start(0);
  std::vector<int> counts;
  run(s, 0, 600000, 500, counts);  // 10 minutes, 0,5 s par lecture
  CHECK(counts[0] >= 10 && counts[0] <= 11);
  CHECK(counts[1] >= 20 && counts[1] <= 21);
  CHECK(counts[2] >= 10 && counts[2] <= 11);
}

TEST(scheduler_long_intervals) {
  RegisterScheduler s;
  s.add(0x0001, 2, 6UL * 3600 * 1000);  // 6 h
  s.add(0x0002, 2, 3600UL * 1000);      // 1 h
  s.add(0x0003, 2, 30000);
  s.start(0);
  std::vector<int> counts;
  run(s, 0, 12UL * 3600 * 1000, 500, counts);  // 12 h
  CHECK(counts[0] >= 2 && counts[0] <= 3);
  CHECK(counts[1] >= 12 && counts[1] <= 13);
  CHECK(counts[2] >= 1400);
}

TEST(scheduler_no_duplicate_while_in_flight) {
  RegisterScheduler s;
  s.add(0x0800, 2, 1000);
  s.add(0x0802, 2, 1000);
  s.start(0);
  const int first = s.acquire_next(0);
  CHECK_EQ(first, 0);
  CHECK(s.busy());
  // Tant que la lecture est en cours, rien d'autre n'est délivré, même très tard.
  CHECK_EQ(s.acquire_next(10), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire_next(50000), RegisterScheduler::NONE);
  s.on_success(first, 50000);
  CHECK_EQ(s.acquire_next(50000), 1);
}

TEST(scheduler_no_catch_up_after_stall) {
  RegisterScheduler s;
  s.add(0x0800, 2, 10000);
  s.start(0);
  CHECK_EQ(s.acquire_next(0), 0);
  // La lecture a pris 95 s : 9 échéances manquées ne doivent pas être rejouées.
  s.on_success(0, 95000);
  CHECK_EQ(s.slot(0).next_due_ms, 105000u);
  CHECK_EQ(s.acquire_next(95000), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire_next(104999), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire_next(105000), 0);
}

TEST(scheduler_keeps_cadence_without_drift) {
  RegisterScheduler s;
  s.add(0x0800, 2, 30000);
  s.start(0);
  CHECK_EQ(s.acquire_next(0), 0);
  s.on_success(0, 4000);  // 4 s de délai de synchro
  CHECK_EQ(s.slot(0).next_due_ms, 30000u);  // pas 34000 : pas de dérive
  CHECK_EQ(s.slot(0).last_success_ms, 4000u);
  CHECK(s.slot(0).has_succeeded);
}

TEST(scheduler_most_overdue_first_and_no_starvation) {
  RegisterScheduler s;
  // Registre 0 très fréquent, registre 3 rare : il doit quand même passer.
  s.add(0x0001, 2, 1000);
  s.add(0x0002, 2, 1000);
  s.add(0x0003, 2, 1000);
  s.add(0x0004, 2, 3600000);
  s.start(0);
  // Chaque lecture dure 5 s, bien plus que l'intervalle de 1 s : saturation.
  uint32_t now = 0;
  int rare_reads = 0;
  int wait_for_rare = 0;
  for (int i = 0; i < 8; i++) {
    const int idx = s.acquire_next(now);
    CHECK(idx != RegisterScheduler::NONE);
    if (idx == 3)
      rare_reads++;
    else if (rare_reads == 0)
      wait_for_rare++;
    now += 5000;
    s.on_success(idx, now);
  }
  CHECK_EQ(rare_reads, 1);
  CHECK(wait_for_rare <= 3);  // attente bornée à N-1 transactions
}

TEST(scheduler_tie_respects_declaration_order) {
  RegisterScheduler s;
  s.add(0x0800, 2, 60000);
  s.add(0x0802, 2, 30000);
  s.add(0x0804, 2, 60000);
  s.start(0);
  int order[3];
  uint32_t now = 0;
  for (int &o : order) {
    o = s.acquire_next(now);
    now += 100;
    s.on_success(o, now);
  }
  CHECK_EQ(order[0], 0);
  CHECK_EQ(order[1], 1);
  CHECK_EQ(order[2], 2);
}

TEST(scheduler_limited_retries_then_next_cycle) {
  RegisterScheduler s;
  s.set_max_retries(2);
  s.set_retry_delay(1000);
  s.add(0x0800, 2, 60000);
  s.start(0);
  CHECK_EQ(s.acquire_next(0), 0);
  CHECK(!s.on_failure(0, 100));  // tentative 1
  CHECK_EQ(s.slot(0).next_due_ms, 1100u);
  CHECK_EQ(s.acquire_next(1000), RegisterScheduler::NONE);  // délai de nouvelle tentative
  CHECK_EQ(s.acquire_next(1100), 0);
  CHECK(!s.on_failure(0, 1200));  // tentative 2
  CHECK_EQ(s.acquire_next(2200), 0);
  CHECK(s.on_failure(0, 2300));  // tentative 3 : abandon pour ce cycle
  CHECK_EQ(s.slot(0).failures, 0);
  CHECK_EQ(s.slot(0).next_due_ms, 60000u);  // cycle suivant normal
  CHECK(!s.busy());
}

TEST(scheduler_failing_register_does_not_block_others) {
  RegisterScheduler s;
  s.set_max_retries(1);
  s.add(0x0800, 2, 10000);  // toujours en échec
  s.add(0x0802, 2, 10000);
  s.start(0);
  std::vector<int> ok(2, 0);
  uint32_t now = 0;
  while (now < 100000) {
    const int idx = s.acquire_next(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    now += 1000;
    if (idx == 0) {
      s.on_failure(idx, now);
    } else {
      s.on_success(idx, now);
      ok[1]++;
    }
  }
  CHECK(ok[1] >= 9);
}

TEST(scheduler_millis_overflow) {
  RegisterScheduler s;
  s.add(0x0800, 2, 60000);
  s.add(0x0802, 2, 30000);
  const uint32_t start = 0xFFFFFFFFu - 120000;  // 2 minutes avant le débordement
  s.start(start);
  std::vector<int> counts;
  run(s, start, start + 600000, 500, counts);  // 10 minutes à cheval
  CHECK(counts[0] >= 10 && counts[0] <= 11);
  CHECK(counts[1] >= 20 && counts[1] <= 21);
}
