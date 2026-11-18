#pragma once

// Diagnostic des cycles du brûleur (v0.8.1).
//
// Indépendant d'ESPHome (testé sur PC). AUCUNE lecture Optolink : tout vient
// des lectures déjà faites par les entités existantes :
//   - 0x0842 brûleur marche/arrêt (binary_sensor, typiquement 30 s) ;
//   - 0x088A compteur de démarrages (uint32, typiquement 1 h) ;
//   - 0x08A7 compteur de fonctionnement en secondes (uint32, 1 h) ;
//   - 0x0802 température chaudière (int16, typiquement 60 s).
//
// Deux familles de valeurs, à ne pas confondre :
//
// EXACTES (compteurs de la chaudière) : démarrages sur 24 h, démarrages par
// heure, durée moyenne de fonctionnement. Différences entre deux lectures
// des compteurs ; la fenêtre est celle qui sépare ces deux lectures (≈ 24 h,
// à l'intervalle de lecture du compteur près), jamais une extrapolation.
// Les démarrages sur 24 h ne sont publiés qu'avec 24 h d'historique ; les
// démarrages par heure et la durée moyenne dès 1 h (fenêtre disponible,
// au plus 24 h). L'historique est en RAM : il repart de zéro au redémarrage.
//
// ESTIMÉES (échantillonnage de 0x0842) : durée du dernier cycle, du dernier
// arrêt, température chaudière au démarrage et à l'arrêt. Un changement
// d'état se produit entre la dernière lecture de l'ancien état et la
// première lecture du nouveau : il est daté au milieu, à ± la moitié de cet
// écart. Une durée cumule l'incertitude de ses deux bornes. Un cycle plus
// court que l'intervalle de lecture peut passer inaperçu (le compteur de
// démarrages, lui, le compte). Après une lecture manquée (écart supérieur à
// 3 intervalles + 30 s) ou un état inconnu, la borne suivante est inconnue :
// la durée correspondante est publiée « inconnue », jamais devinée.
// Température : lecture de 0x0802 la plus proche de l'instant estimé du
// changement (celle d'avant ou la première d'après), si elle en est à moins
// de 3 intervalles + 30 s ; sinon inconnue.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "circuit_climate.h"  // stale_after_ms()

namespace esphome {
namespace visslink {

static constexpr uint32_t BURNER_HOUR_MS = 3600UL * 1000UL;
static constexpr uint32_t BURNER_DAY_MS = 24UL * BURNER_HOUR_MS;
/// Lecture du compteur acceptée comme début de fenêtre « 24 h » si elle a au
/// moins 24 h moins cette tolérance (dérive des échéances de lecture).
static constexpr uint32_t BURNER_DAY_TOLERANCE_MS = 5UL * 60UL * 1000UL;
/// Fenêtre plus longue (lectures du compteur manquées) : pas de valeur 24 h.
static constexpr uint32_t BURNER_DAY_MAX_MS = 26UL * BURNER_HOUR_MS;
/// Fenêtre minimale pour les démarrages par heure et la durée moyenne.
static constexpr uint32_t BURNER_RATE_MIN_MS = BURNER_HOUR_MS;

/// Âge de `t` à l'instant `now` (horloge millis(), débordement compris).
inline uint32_t burner_age(uint32_t now, uint32_t t) { return now - t; }

/// Historique d'un compteur croissant (lectures espacées d'au moins
/// MIN_SPACING_MS) sur un peu plus de 24 h. ~450 octets.
class CounterHistory {
 public:
  static constexpr size_t CAPACITY = 56;
  static constexpr uint32_t MIN_SPACING_MS = 30UL * 60UL * 1000UL;

  struct Sample {
    uint32_t t;
    uint32_t value;
  };

  /// Différence entre deux lectures du compteur.
  struct Delta {
    bool valid{false};
    uint32_t delta{0};
    uint32_t window_ms{0};
  };

  /// Nouvelle lecture réussie. Renvoie false si le compteur a reculé
  /// (remise à zéro, lecture aberrante) : l'historique repart de là.
  bool add(uint32_t now, uint32_t value) {
    bool monotonic = true;
    if (this->count_ > 0 && value < this->latest_.value) {
      this->count_ = 0;
      monotonic = false;
    }
    this->latest_ = {now, value};
    this->has_latest_ = true;
    if (this->count_ == 0 || burner_age(now, this->at_(this->count_ - 1).t) >= MIN_SPACING_MS)
      this->push_(this->latest_);
    this->prune_(now);
    return monotonic;
  }

  void clear() {
    this->count_ = 0;
    this->has_latest_ = false;
  }
  bool has_latest() const { return this->has_latest_; }
  const Sample &latest() const { return this->latest_; }
  size_t size() const { return this->count_; }

  /// Fenêtre « 24 h » : de la plus récente lecture ayant au moins
  /// 24 h - tolérance jusqu'à la dernière lecture. Invalide sans 24 h
  /// d'historique ou si la fenêtre dépasse BURNER_DAY_MAX_MS.
  Delta day() const {
    Delta d;
    const Sample *start = this->day_start_();
    if (start == nullptr)
      return d;
    d.window_ms = burner_age(this->latest_.t, start->t);
    if (d.window_ms > BURNER_DAY_MAX_MS)
      return Delta{};
    d.delta = this->latest_.value - start->value;
    d.valid = true;
    return d;
  }

  /// Fenêtre disponible, au plus « 24 h » : comme day() si 24 h d'historique,
  /// sinon depuis la plus ancienne lecture ; invalide sous BURNER_RATE_MIN_MS.
  Delta available() const {
    Delta d = this->day();
    if (d.valid || this->count_ == 0 || this->day_start_() != nullptr)
      return d;
    const Sample &start = this->at_(0);
    d.window_ms = burner_age(this->latest_.t, start.t);
    if (d.window_ms < BURNER_RATE_MIN_MS)
      return Delta{};
    d.delta = this->latest_.value - start.value;
    d.valid = true;
    return d;
  }

 protected:
  const Sample &at_(size_t i) const { return this->samples_[(this->head_ + i) % CAPACITY]; }

  const Sample *day_start_() const {
    if (!this->has_latest_)
      return nullptr;
    const Sample *found = nullptr;
    for (size_t i = 0; i < this->count_; i++) {
      const Sample &s = this->at_(i);
      if (burner_age(this->latest_.t, s.t) + BURNER_DAY_TOLERANCE_MS >= BURNER_DAY_MS)
        found = &s;  // la plus récente qui convient
    }
    return found;
  }

  void push_(const Sample &s) {
    if (this->count_ == CAPACITY) {
      this->head_ = (this->head_ + 1) % CAPACITY;
      this->count_--;
    }
    this->samples_[(this->head_ + this->count_) % CAPACITY] = s;
    this->count_++;
  }

  /// Garde une seule lecture de plus de 24 h (le début de fenêtre), et
  /// aucune au-delà de BURNER_DAY_MAX_MS.
  void prune_(uint32_t now) {
    while (this->count_ >= 2) {
      const uint32_t age_next = burner_age(now, this->at_(1).t);
      const uint32_t age_first = burner_age(now, this->at_(0).t);
      if (age_next + BURNER_DAY_TOLERANCE_MS >= BURNER_DAY_MS || age_first > BURNER_DAY_MAX_MS) {
        this->head_ = (this->head_ + 1) % CAPACITY;
        this->count_--;
      } else {
        break;
      }
    }
  }

  Sample samples_[CAPACITY]{};
  size_t head_{0};
  size_t count_{0};
  Sample latest_{0, 0};
  bool has_latest_{false};
};

/// Valeurs exactes calculées à partir des deux compteurs.
struct BurnerCounterStats {
  float starts_24h{NAN};       ///< démarrages sur ≈ 24 h
  float starts_per_hour{NAN};  ///< démarrages / h, fenêtre disponible (≤ 24 h)
  float average_run_s{NAN};    ///< secondes de fonctionnement / démarrage
  uint32_t window_ms{0};       ///< fenêtre des démarrages (0 : aucune)
};

/// `starts` et `runtime` : historiques des deux compteurs ; une valeur reste
/// inconnue si la dernière lecture du compteur est périmée à `now`.
inline BurnerCounterStats burner_counter_stats(const CounterHistory &starts, uint32_t starts_interval_ms,
                                               const CounterHistory *runtime, uint32_t runtime_interval_ms,
                                               uint32_t now) {
  BurnerCounterStats out;
  if (!starts.has_latest() || burner_age(now, starts.latest().t) > stale_after_ms(starts_interval_ms))
    return out;
  const CounterHistory::Delta day = starts.day();
  if (day.valid)
    out.starts_24h = static_cast<float>(day.delta);
  const CounterHistory::Delta avail = starts.available();
  if (!avail.valid)
    return out;
  out.window_ms = avail.window_ms;
  out.starts_per_hour = static_cast<float>(avail.delta) * static_cast<float>(BURNER_HOUR_MS) /
                        static_cast<float>(avail.window_ms);
  if (runtime == nullptr || !runtime->has_latest() ||
      burner_age(now, runtime->latest().t) > stale_after_ms(runtime_interval_ms) || avail.delta == 0)
    return out;
  const CounterHistory::Delta run = runtime->available();
  if (!run.valid)
    return out;
  // Les deux compteurs sont lus séparément : fenêtres voisines mais pas
  // identiques. Écart de plus de 10 % : pas de moyenne.
  const uint32_t a = run.window_ms, b = avail.window_ms;
  const uint32_t diff = a > b ? a - b : b - a;
  if (diff > b / 10)
    return out;
  out.average_run_s = static_cast<float>(run.delta) / static_cast<float>(avail.delta);
  return out;
}

/// Lecture de température datée.
struct TimedValue {
  bool valid{false};
  uint32_t t{0};
  float value{NAN};
};

/// Changement d'état estimé du brûleur, en attente de sa température.
struct BurnerEdge {
  bool pending{false};
  bool on{false};        ///< true : démarrage, false : arrêt
  uint32_t t{0};         ///< instant estimé (milieu de l'écart)
  uint32_t seen_ms{0};   ///< première lecture du nouvel état
  TimedValue before;     ///< température connue à ce moment-là
};

/// Résultat d'une lecture du brûleur.
struct BurnerUpdate {
  bool cycle{false};      ///< durée du dernier cycle à publier
  bool stop{false};       ///< durée du dernier arrêt à publier
  float duration_s{NAN};  ///< NAN : borne inconnue
  float uncertainty_s{NAN};
};

/// Écart absolu entre deux instants millis() proches (< 24 jours).
inline uint32_t burner_distance(uint32_t a, uint32_t b) {
  const int32_t d = static_cast<int32_t>(a - b);
  return d < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(d)) : static_cast<uint32_t>(d);
}

/// Suivi des marches/arrêts lus sur 0x0842 et des températures associées.
class BurnerCycleTracker {
 public:
  /// Intervalles de lecture du brûleur et de la température (ms).
  void configure(uint32_t burner_interval_ms, uint32_t temperature_interval_ms) {
    this->burner_interval_ms_ = burner_interval_ms;
    this->temperature_interval_ms_ = temperature_interval_ms;
  }

  /// Lecture du brûleur à `t` : `known` faux si l'état lu est inconnu.
  BurnerUpdate on_burner(uint32_t t, bool known, bool on) {
    BurnerUpdate u;
    if (!known) {
      this->has_obs_ = false;  // borne suivante inconnue
      return u;
    }
    if (!this->has_obs_ || burner_age(t, this->last_obs_) > stale_after_ms(this->burner_interval_ms_)) {
      // Premier état connu, ou lectures manquées / inconnues : rien ne dit
      // ce qui s'est passé entre-temps. Un changement d'état clôt la phase
      // précédente avec une durée inconnue.
      if (this->has_state_ && on != this->on_) {
        u.cycle = this->on_;
        u.stop = !this->on_;
      }
      this->restart_(t, on);
      return u;
    }
    if (on == this->on_) {
      this->last_obs_ = t;
      return u;
    }
    // Changement observé : entre last_obs_ et t, daté au milieu.
    const uint32_t gap = burner_age(t, this->last_obs_);
    const uint32_t half = gap / 2;
    const uint32_t edge = this->last_obs_ + half;
    if (this->phase_known_) {
      u.duration_s = static_cast<float>(burner_age(edge, this->phase_start_)) / 1000.0f;
      u.uncertainty_s = static_cast<float>(half + this->phase_half_) / 1000.0f;
    }
    u.cycle = this->on_;
    u.stop = !this->on_;
    this->on_ = on;
    this->phase_known_ = true;
    this->phase_start_ = edge;
    this->phase_half_ = half;
    this->last_obs_ = t;
    BurnerEdge &e = this->edges_[on ? 1 : 0];
    e.pending = true;
    e.on = on;
    e.t = edge;
    e.seen_ms = t;
    e.before = this->temperature_;
    return u;
  }

  /// Lecture de température (`valid` faux : lecture inconnue).
  void on_temperature(uint32_t t, bool valid, float value) {
    this->temperature_.valid = valid && std::isfinite(value);
    this->temperature_.t = t;
    this->temperature_.value = value;
  }

  /// Température d'un changement d'état, dès qu'elle est décidée : renvoie
  /// true et remplit `on` (démarrage ?) et `value` (NAN si inconnue).
  bool poll_edge_temperature(uint32_t now, bool *on, float *value) {
    for (BurnerEdge &e : this->edges_) {
      if (this->decide_(e, now, value)) {
        *on = e.on;
        return true;
      }
    }
    return false;
  }

  /// Demi-écart de lecture de la dernière borne (incertitude d'un instant).
  float last_edge_half_s() const {
    return this->phase_known_ ? static_cast<float>(this->phase_half_) / 1000.0f : NAN;
  }

 protected:
  bool decide_(BurnerEdge &e, uint32_t now, float *value) {
    if (!e.pending)
      return false;
    const uint64_t limit = stale_after_ms(this->temperature_interval_ms_);
    const TimedValue &cur = this->temperature_;
    // Première lecture faite après l'observation du changement ?
    const bool after = cur.valid && static_cast<int32_t>(cur.t - e.seen_ms) > 0;
    if (!after && burner_age(now, e.seen_ms) <= limit)
      return false;  // on l'attend encore
    const uint32_t d_before = e.before.valid ? burner_distance(e.t, e.before.t) : UINT32_MAX;
    const uint32_t d_after = after ? burner_distance(cur.t, e.t) : UINT32_MAX;
    float v = NAN;
    if (d_after <= d_before && d_after <= limit) {
      v = cur.value;
    } else if (d_before < d_after && d_before <= limit) {
      v = e.before.value;
    }
    *value = v;
    e.pending = false;
    return true;
  }

  void restart_(uint32_t t, bool on) {
    this->has_obs_ = true;
    this->has_state_ = true;
    this->on_ = on;
    this->last_obs_ = t;
    this->phase_known_ = false;
  }

  uint32_t burner_interval_ms_{30000};
  uint32_t temperature_interval_ms_{60000};
  bool has_obs_{false};    ///< dernière lecture connue et enchaînable
  bool has_state_{false};  ///< un état a déjà été lu (on_ significatif)
  bool on_{false};
  uint32_t last_obs_{0};
  bool phase_known_{false};
  uint32_t phase_start_{0};
  uint32_t phase_half_{0};
  BurnerEdge edges_[2];  ///< [0] arrêt, [1] démarrage
  TimedValue temperature_;
};

}  // namespace visslink
}  // namespace esphome
