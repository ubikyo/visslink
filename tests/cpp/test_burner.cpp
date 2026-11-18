// Diagnostic des cycles du brûleur (v0.8.1) : compteurs exacts, durées et
// températures estimées.

#include <cmath>
#include <cstdint>

#include "burner_cycles.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

constexpr uint32_t S = 1000;
constexpr uint32_t MIN = 60 * S;
constexpr uint32_t H = 60 * MIN;

/// Compteur lu toutes les heures (+ `jitter` ms), `per_hour` démarrages/h.
void fill_hourly(CounterHistory &h, uint32_t t0, int hours, uint32_t start, uint32_t per_hour, uint32_t jitter = 0) {
  for (int i = 0; i <= hours; i++)
    h.add(t0 + static_cast<uint32_t>(i) * (H + jitter), start + static_cast<uint32_t>(i) * per_hour);
}

}  // namespace

TEST(burner_counter_needs_one_hour_then_24h) {
  CounterHistory h;
  h.add(1000, 500);
  CHECK(!h.available().valid);  // une seule lecture
  CHECK(!h.day().valid);
  h.add(1000 + 30 * MIN, 503);
  CHECK(!h.available().valid);  // < 1 h
  fill_hourly(h, 1000 + H, 10, 506, 4);
  const CounterHistory::Delta a = h.available();
  CHECK(a.valid);
  CHECK_EQ(a.window_ms, 11 * H);
  CHECK_EQ(a.delta, 46u);
  CHECK(!h.day().valid);  // 24 h pas encore atteintes
}

TEST(burner_counter_day_window_is_exact_difference) {
  CounterHistory h;
  fill_hourly(h, 5000, 30, 1000, 3);
  const CounterHistory::Delta d = h.day();
  CHECK(d.valid);
  CHECK_EQ(d.window_ms, 24 * H);
  CHECK_EQ(d.delta, 72u);
  CHECK(h.size() <= CounterHistory::CAPACITY);
  CHECK(h.size() <= 26u);  // anciennes lectures élaguées
}

TEST(burner_counter_day_tolerates_reading_drift) {
  // Lectures un peu en avance (59 min 50 s) : la fenêtre « 24 h » reste
  // celle de la lecture la plus proche, sans extrapolation.
  CounterHistory h;
  for (int i = 0; i <= 26; i++)
    h.add(static_cast<uint32_t>(i) * (H - 10 * S), 100u + static_cast<uint32_t>(i) * 2u);
  const CounterHistory::Delta d = h.day();
  CHECK(d.valid);
  CHECK_EQ(d.window_ms, 24 * (H - 10 * S));  // 23 h 56 min
  CHECK_EQ(d.delta, 48u);
}

TEST(burner_counter_gap_too_long_gives_no_day_value) {
  CounterHistory h;
  h.add(0, 10);
  h.add(27 * H, 60);  // 27 h sans lecture
  CHECK(!h.day().valid);
  CHECK(!h.available().valid);
}

TEST(burner_counter_backwards_restarts_history) {
  CounterHistory h;
  fill_hourly(h, 0, 5, 1000, 2);
  CHECK(!h.add(6 * H, 3));  // remise à zéro
  CHECK_EQ(h.size(), 1u);
  CHECK(!h.available().valid);
}

TEST(burner_counter_dense_readings_are_thinned) {
  // Compteur lu toutes les 10 min : une lecture gardée toutes les 30 min,
  // mais la dernière lecture sert toujours de fin de fenêtre.
  CounterHistory h;
  for (uint32_t i = 0; i <= 6 * 30; i++)
    h.add(i * 10 * MIN, i);
  CHECK(h.size() <= CounterHistory::CAPACITY);
  const CounterHistory::Delta d = h.day();
  CHECK(d.valid);
  CHECK_EQ(d.delta, 144u);  // 24 h = 144 lectures de 10 min
  CHECK_EQ(d.window_ms, 24 * H);
}

TEST(burner_counter_survives_millis_rollover) {
  CounterHistory h;
  const uint32_t t0 = 0xFFFFFFFFu - 3 * H;
  fill_hourly(h, t0, 25, 7, 1);
  const CounterHistory::Delta d = h.day();
  CHECK(d.valid);
  CHECK_EQ(d.window_ms, 24 * H);
  CHECK_EQ(d.delta, 24u);
}

TEST(burner_stats_exact_values_from_counters) {
  CounterHistory starts, runtime;
  // 3 démarrages/h, 1800 s de fonctionnement/h : 600 s par démarrage.
  fill_hourly(starts, 0, 25, 100, 3);
  fill_hourly(runtime, 2 * S, 25, 50000, 1800);  // lu 2 s après
  const uint32_t now = 25 * H + 10 * S;
  BurnerCounterStats st = burner_counter_stats(starts, H, &runtime, H, now);
  CHECK_EQ(st.starts_24h, 72.0f);
  CHECK(std::fabs(st.starts_per_hour - 3.0f) < 1e-4f);
  CHECK(std::fabs(st.average_run_s - 600.0f) < 1e-3f);
  CHECK_EQ(st.window_ms, 24 * H);
}

TEST(burner_stats_partial_window_before_24h) {
  CounterHistory starts, runtime;
  fill_hourly(starts, 0, 4, 100, 5);
  fill_hourly(runtime, 0, 4, 0, 900);
  BurnerCounterStats st = burner_counter_stats(starts, H, &runtime, H, 4 * H);
  CHECK(std::isnan(st.starts_24h));  // jamais extrapolé
  CHECK(std::fabs(st.starts_per_hour - 5.0f) < 1e-4f);
  CHECK(std::fabs(st.average_run_s - 180.0f) < 1e-3f);
  CHECK_EQ(st.window_ms, 4 * H);
}

TEST(burner_stats_unknown_when_counter_stale_or_no_start) {
  CounterHistory starts, runtime;
  fill_hourly(starts, 0, 25, 100, 0);  // aucun démarrage
  fill_hourly(runtime, 0, 25, 0, 3600);  // brûleur en marche continue
  BurnerCounterStats st = burner_counter_stats(starts, H, &runtime, H, 25 * H);
  CHECK_EQ(st.starts_24h, 0.0f);
  CHECK_EQ(st.starts_per_hour, 0.0f);
  CHECK(std::isnan(st.average_run_s));  // pas de division par zéro
  // Dernière lecture du compteur trop ancienne (3 intervalles + 30 s) :
  st = burner_counter_stats(starts, H, &runtime, H, 25 * H + 3 * H + 31 * S);
  CHECK(std::isnan(st.starts_24h));
  CHECK(std::isnan(st.starts_per_hour));
  // Sans compteur de fonctionnement : pas de durée moyenne.
  st = burner_counter_stats(starts, H, nullptr, 0, 25 * H);
  CHECK(std::isnan(st.average_run_s));
}

TEST(burner_stats_mismatched_windows_give_no_average) {
  CounterHistory starts, runtime;
  fill_hourly(starts, 0, 10, 0, 2);
  fill_hourly(runtime, 8 * H, 2, 0, 600);  // 2 h seulement
  BurnerCounterStats st = burner_counter_stats(starts, H, &runtime, H, 10 * H);
  CHECK(!std::isnan(st.starts_per_hour));
  CHECK(std::isnan(st.average_run_s));
}

TEST(burner_cycle_duration_is_midpoint_estimate) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  CHECK(!t.on_burner(0, true, false).cycle);
  CHECK(!t.on_burner(30 * S, true, false).cycle);
  BurnerUpdate u = t.on_burner(60 * S, true, true);  // démarrage entre 30 et 60 s
  CHECK(u.stop);  // l'arrêt d'avant n'a pas de début connu
  CHECK(std::isnan(u.duration_s));
  for (uint32_t r = 90 * S; r <= 10 * MIN + 30 * S; r += 30 * S)
    CHECK(!t.on_burner(r, true, true).cycle);  // en marche, lu toutes les 30 s
  u = t.on_burner(11 * MIN, true, false);  // arrêt entre 10:30 et 11:00
  CHECK(u.cycle);
  CHECK(!u.stop);
  // (10:45) - (0:45) = 10 min, ± (15 + 15) s
  CHECK_EQ(u.duration_s, 600.0f);
  CHECK_EQ(u.uncertainty_s, 30.0f);
  u = t.on_burner(14 * MIN, false, false);  // lecture inconnue
  CHECK(!u.cycle && !u.stop);
  u = t.on_burner(14 * MIN + 30 * S, true, true);  // redémarre : borne inconnue
  CHECK(u.stop);
  CHECK(std::isnan(u.duration_s));
}

TEST(burner_stop_duration_and_missed_reading) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  t.on_burner(0, true, true);
  t.on_burner(30 * S, true, false);  // arrêt vers 15 s
  BurnerUpdate u = t.on_burner(5 * MIN + 30 * S, true, true);  // > 2 min sans lecture
  CHECK(u.stop);
  CHECK(std::isnan(u.duration_s));  // jamais deviné
  t.on_burner(6 * MIN, true, true);
  t.on_burner(7 * MIN, true, false);  // une lecture manquée (60 s) : acceptée
  t.on_burner(7 * MIN + 30 * S, true, false);
  u = t.on_burner(8 * MIN, true, true);
  CHECK(u.stop);
  // arrêt vers 6:30 (±30 s), démarrage vers 7:45 (±15 s)
  CHECK_EQ(u.duration_s, 75.0f);
  CHECK_EQ(u.uncertainty_s, 45.0f);
}

TEST(burner_short_cycle_between_readings_is_not_seen) {
  // Limite assumée : marche puis arrêt entre deux lectures, aucun cycle.
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  t.on_burner(0, true, false);
  const BurnerUpdate u = t.on_burner(30 * S, true, false);
  CHECK(!u.cycle && !u.stop);
}

TEST(burner_edge_temperature_picks_nearest_reading) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  bool on = false;
  float v = NAN;
  t.on_temperature(0, true, 45.0f);
  t.on_burner(10 * S, true, false);
  t.on_temperature(40 * S, true, 44.8f);
  t.on_burner(40 * S, true, false);
  t.on_burner(70 * S, true, true);  // démarrage vers 55 s
  CHECK(!t.poll_edge_temperature(71 * S, &on, &v));  // attend la lecture suivante
  t.on_temperature(100 * S, true, 47.0f);  // 45 s après, celle d'avant : 15 s
  CHECK(t.poll_edge_temperature(101 * S, &on, &v));
  CHECK(on);
  CHECK_EQ(v, 44.8f);
  CHECK(!t.poll_edge_temperature(102 * S, &on, &v));  // décidée une seule fois
}

TEST(burner_edge_temperature_after_is_nearer) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  bool on = true;
  float v = NAN;
  t.on_temperature(0, true, 70.0f);
  t.on_burner(30 * S, true, true);
  t.on_burner(60 * S, true, false);  // arrêt vers 45 s
  t.on_temperature(62 * S, true, 72.5f);
  CHECK(t.poll_edge_temperature(63 * S, &on, &v));
  CHECK(!on);
  CHECK_EQ(v, 72.5f);
}

TEST(burner_edge_temperature_unknown_when_too_old) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  bool on = false;
  float v = 0.0f;
  t.on_temperature(0, true, 50.0f);
  t.on_burner(5 * MIN, true, false);
  t.on_burner(5 * MIN + 30 * S, true, true);
  // Aucune lecture de température ensuite : décision à l'échéance
  // (3 intervalles + 30 s), celle d'avant est trop ancienne.
  CHECK(!t.poll_edge_temperature(5 * MIN + 31 * S, &on, &v));
  CHECK(t.poll_edge_temperature(5 * MIN + 30 * S + 211 * S, &on, &v));
  CHECK(on);
  CHECK(std::isnan(v));
}

TEST(burner_short_cycle_keeps_both_edge_temperatures) {
  BurnerCycleTracker t;
  t.configure(30 * S, 60 * S);
  bool on = false;
  float v = NAN;
  t.on_temperature(0, true, 50.0f);
  t.on_burner(0, true, false);
  t.on_burner(30 * S, true, true);   // démarrage vers 15 s
  t.on_burner(60 * S, true, false);  // arrêt vers 45 s
  t.on_temperature(61 * S, true, 53.0f);
  int seen = 0;
  bool start_seen = false, stop_seen = false;
  while (t.poll_edge_temperature(62 * S, &on, &v)) {
    seen++;
    if (on) {
      start_seen = true;
      CHECK_EQ(v, 50.0f);  // 15 s avant, contre 46 s après
    } else {
      stop_seen = true;
      CHECK_EQ(v, 53.0f);  // 16 s après, contre 45 s avant
    }
  }
  CHECK_EQ(seen, 2);
  CHECK(start_seen && stop_seen);
}
