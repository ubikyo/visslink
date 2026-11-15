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

// --- v0.4.1 : réactivité des commandes ---------------------------------------

TEST(scheduler_release_keeps_due_and_failures) {
  RegisterScheduler s;
  s.add(60000, 0);
  s.add(60000, 50000);
  CHECK_EQ(s.acquire(1000), 0);
  s.release();  // préemptée avant émission
  CHECK_EQ(s.current(), RegisterScheduler::NONE);
  CHECK_EQ(s.entry(0).due_ms, 0u);  // toujours échue
  CHECK_EQ(s.entry(0).failures, 0);
  CHECK_EQ(s.acquire(6000), 0);  // la plus en retard repasse en premier
  CHECK(!s.finish(false, 7000));
  CHECK_EQ(s.acquire(8000), 0);
  s.release();
  CHECK_EQ(s.entry(0).failures, 1);  // tentative non comptée deux fois
}

TEST(scheduler_urgent_only_and_owner_first) {
  RegisterScheduler s;
  s.add(60000, 0);   // 0 : ordinaire, échu
  s.add(300000, 0);  // 1 : autre circuit
  s.add(300000, 0);  // 2 : registre écrit
  for (int i = 0; i < 3; i++) {
    const int idx = s.acquire(1000 + i);
    s.finish(true, 1500 + i);
    CHECK(idx >= 0);
  }
  CHECK(s.expedite(2, 70000));      // écrit : relu en premier
  CHECK(s.expedite(1, 70000 + 1));  // autre circuit ensuite
  CHECK_EQ(s.acquire(70001, true), 2);
  CHECK(!s.finish(false, 71000));        // échec : nouvelle tentative de 2 à 72 000
  CHECK_EQ(s.acquire(71100, true), 1);  // l'autre relecture passe pendant ce délai
  s.finish(true, 71500);
  CHECK_EQ(s.acquire(71600, true), RegisterScheduler::NONE);  // pas de lecture ordinaire intercalée…
  CHECK_EQ(s.acquire(71600, false), 0);                       // …alors que sans la restriction, 0 passerait
  s.release();
  CHECK_EQ(s.acquire(72000, true), 2);
  s.finish(true, 74000);
  CHECK_EQ(s.acquire(74000, true), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(74000), 0);  // relectures terminées : lectures ordinaires
}

// --- v0.5.1 : jour courant d'abord, relecture d'un bloc ----------------------

namespace {

/// Lit tout ce qui est échu entre `from` et `to` (lectures de `read_ms`) et
/// renvoie les (registre, bloc) lus dans l'ordre.
std::vector<std::pair<int, int>> trace(RegisterScheduler &s, uint32_t from, uint32_t to, uint32_t read_ms,
                                       uint32_t *now_out = nullptr) {
  std::vector<std::pair<int, int>> out;
  uint32_t now = from;
  while (now - from < to - from) {
    const int idx = s.acquire(now);
    if (idx == RegisterScheduler::NONE) {
      now += 100;
      continue;
    }
    out.push_back({idx, s.block(idx)});
    now += read_ms;
    s.finish(true, now);
  }
  if (now_out != nullptr)
    *now_out = now;
  return out;
}

std::vector<int> blocks_of(const std::vector<std::pair<int, int>> &t, int reg) {
  std::vector<int> out;
  for (const auto &p : t)
    if (p.first == reg)
      out.push_back(p.second);
  return out;
}

}  // namespace

TEST(scheduler_block_delay_is_configurable_default_30s) {
  RegisterScheduler s;
  CHECK_EQ(s.block_delay(), 30000u);
  CHECK_EQ(RegisterScheduler::BLOCK_DELAY_MS, 30000u);  // valeur de production inchangée
  s.set_block_delay(10000);
  s.add(3600000, 0, 7);
  CHECK_EQ(s.acquire(0), 0);
  s.finish(true, 500);
  CHECK_EQ(s.entry(0).due_ms, 10500u);
}

TEST(scheduler_cycle_starts_at_chosen_block_and_wraps) {
  RegisterScheduler s;
  s.add(6UL * 3600 * 1000, 0, 7);
  CHECK(s.set_cycle_start(0, 6));  // dimanche
  const auto t = trace(s, 0, 10 * 60 * 1000, 500);
  CHECK(blocks_of(t, 0) == (std::vector<int>{6, 0, 1, 2, 3, 4, 5}));
  // Cadence calée sur le début du cycle, cycle suivant au même jour.
  CHECK_EQ(s.entry(0).due_ms, 6UL * 3600 * 1000);
  CHECK_EQ(s.block(0), 6);
  const auto t2 = trace(s, 6UL * 3600 * 1000, 6UL * 3600 * 1000 + 10 * 60 * 1000, 500);
  CHECK(blocks_of(t2, 0) == (std::vector<int>{6, 0, 1, 2, 3, 4, 5}));
}

TEST(scheduler_cycle_start_refused_once_cycle_started_or_invalid) {
  RegisterScheduler s;
  s.add(60000, 0);        // registre simple
  s.add(3600000, 0, 7);   // programmation
  CHECK(!s.set_cycle_start(0, 0));
  CHECK(!s.set_cycle_start(1, 7));
  CHECK(!s.set_cycle_start(9, 0));
  CHECK_EQ(s.acquire(0), 0);
  s.finish(true, 100);
  CHECK_EQ(s.acquire(100), 1);
  CHECK(!s.set_cycle_start(1, 3));  // en cours de lecture
  s.finish(true, 600);
  CHECK(!s.set_cycle_start(1, 3));  // cycle commencé : ordre inchangé
  CHECK_EQ(s.block(1), 1);
}

TEST(scheduler_expedite_block_mid_cycle_then_cycle_resumes) {
  RegisterScheduler s;
  s.add(30000, 0);            // brûleur
  s.add(6UL * 3600 * 1000, 0, 7);
  uint32_t now = 0;
  // Lundi et mardi lus.
  auto t = trace(s, 0, 32000, 500, &now);
  CHECK(blocks_of(t, 1) == (std::vector<int>{0, 1}));
  CHECK(s.expedite_block(1, 6, now));
  CHECK_EQ(s.entry(1).urgent, RegisterScheduler::URGENT_REFRESH);
  // Servi avant le brûleur échu.
  CHECK_EQ(s.acquire(now + 30000), 1);
  CHECK_EQ(s.block(1), 6);
  s.finish(true, now + 30500);
  CHECK_EQ(s.entry(1).urgent, RegisterScheduler::URGENT_NONE);
  CHECK_EQ(s.block(1), 2);                           // reprise au mercredi
  CHECK(s.entry(1).due_ms - (now + 30500) >= 30000);  // jamais deux blocs à la suite
  // Le cycle se termine normalement : chaque jour lu une fois de plus.
  t = trace(s, now + 30500, now + 10 * 60 * 1000, 500);
  CHECK(blocks_of(t, 1) == (std::vector<int>{2, 3, 4, 5, 6}));
  CHECK_EQ(s.entry(1).due_ms, 6UL * 3600 * 1000);  // cadence du cycle conservée
}

TEST(scheduler_expedite_block_while_waiting_for_next_cycle) {
  RegisterScheduler s;
  s.add(6UL * 3600 * 1000, 0, 7);
  trace(s, 0, 10 * 60 * 1000, 500);  // cycle complet
  const uint32_t next = s.entry(0).due_ms;
  CHECK(s.expedite_block(0, 4, 20 * 60 * 1000));
  CHECK_EQ(s.acquire(20 * 60 * 1000), 0);
  CHECK_EQ(s.block(0), 4);
  s.finish(true, 20 * 60 * 1000 + 500);
  CHECK_EQ(s.entry(0).due_ms, next);  // prochain cycle inchangé
  CHECK_EQ(s.block(0), 0);
  CHECK_EQ(s.entry(0).done, 0);
}

TEST(scheduler_expedite_block_refusals) {
  RegisterScheduler s;
  s.add(3600000, 0, 7);
  CHECK(!s.expedite_block(0, 7, 0));   // bloc inexistant
  CHECK(!s.expedite_block(1, 0, 0));   // registre inexistant
  CHECK(!s.expedite_block(0, 0, 0, RegisterScheduler::URGENT_NONE));
  CHECK(!s.expedite(0, 0));            // expedite() reste réservé aux registres simples
  CHECK(s.expedite_block(0, 3, 0));
  CHECK(s.expedite_block(0, 3, 0));    // même bloc : accepté, sans effet
  CHECK(!s.expedite_block(0, 5, 0));   // une seule relecture hors cycle à la fois
  CHECK_EQ(s.acquire(0), 0);
  CHECK(!s.expedite_block(0, 3, 0));   // en cours de lecture
}

TEST(scheduler_refresh_never_served_during_verification) {
  RegisterScheduler s;
  s.add(60000, 0);        // registre écrit
  s.add(3600000, 0, 7);   // programmation
  trace(s, 0, 31000, 500);
  CHECK(s.expedite_block(1, 6, 40000));
  CHECK(s.expedite(0, 40000));
  // Relecture de vérification d'abord, même plus récente.
  CHECK_EQ(s.acquire(40001), 0);
  s.finish(true, 40500);
  // Pendant une vérification (urgent_only), jamais le rafraîchissement.
  CHECK_EQ(s.acquire(40600, true), RegisterScheduler::NONE);
  CHECK_EQ(s.acquire(40600), 1);
  CHECK_EQ(s.block(1), 6);
}

TEST(scheduler_expedite_block_retries_then_gives_up_and_resumes) {
  RegisterScheduler s;
  s.add(3600000, 0, 7);
  trace(s, 0, 31000, 500);  // lundi et mardi
  CHECK(s.expedite_block(0, 5, 40000));
  uint32_t now = 40000;
  bool gave_up = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    CHECK_EQ(s.acquire(now), 0);
    CHECK_EQ(s.block(0), 5);  // la tentative suivante relit le même bloc
    gave_up = s.finish(false, now + 500);
    now += 1500;
  }
  CHECK(gave_up);
  CHECK_EQ(s.entry(0).urgent, RegisterScheduler::URGENT_NONE);
  CHECK_EQ(s.block(0), 2);
  CHECK_EQ(s.entry(0).failures, 0);
}

TEST(scheduler_cancel_refresh_restores_cycle_untouched) {
  RegisterScheduler s;
  s.add(60000, 0);
  s.add(6UL * 3600 * 1000, 0, 7);
  uint32_t now = 0;
  trace(s, 0, 32000, 500, &now);  // lundi, mardi
  const RegisterScheduler::Entry before = s.entry(1);
  CHECK(s.expedite_block(1, 6, now));
  CHECK(s.refresh_pending(1, 6));
  CHECK(!s.cancel_refresh(1, 5));  // autre bloc
  CHECK(s.cancel_refresh(1, 6));
  CHECK(!s.refresh_pending(1, 6));
  CHECK_EQ(s.entry(1).urgent, RegisterScheduler::URGENT_NONE);
  CHECK_EQ(s.entry(1).block, before.block);
  CHECK_EQ(s.entry(1).due_ms, before.due_ms);
  CHECK_EQ(s.entry(1).done, before.done);
  // Vérification (expedite) et lecture en cours : jamais retirées.
  CHECK(s.expedite(0, now));
  CHECK(!s.cancel_refresh(0, 0));
  CHECK(s.expedite_block(1, 6, now));
  CHECK_EQ(s.acquire(now + 1), 0);
  s.finish(true, now + 500);
  CHECK_EQ(s.acquire(now + 600), 1);
  CHECK(!s.cancel_refresh(1, 6));  // en cours de lecture
}
