#include "visslink_burner.h"

#include <cinttypes>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace visslink_burner {

static const char *const TAG = "visslink.burner";

void VissLinkBurnerCycles::setup() {
  this->tracker_.configure(this->burner_->get_interval(),
                           this->temperature_ != nullptr ? this->temperature_->get_interval() : 60000);
  // Lectures déjà faites avant setup() : ignorées (pas de borne datée).
  this->burner_seen_ = this->burner_->read_ok_count();
  this->set_interval("observe", 1000, [this]() { this->update_(); });
  this->publish_status_(visslink::BurnerCounterStats{});
}

void VissLinkBurnerCycles::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink burner cycles (no extra Optolink read):");
  ESP_LOGCONFIG(TAG, "  Burner 0x%04X every %" PRIu32 " ms (durations are estimates)", this->burner_->get_address(),
                this->burner_->get_interval());
  if (this->starts_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Starts counter 0x%04X every %" PRIu32 " ms (exact)", this->starts_->get_address(),
                  this->starts_->get_interval());
  if (this->runtime_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Runtime counter 0x%04X every %" PRIu32 " ms (exact)", this->runtime_->get_address(),
                  this->runtime_->get_interval());
  if (this->temperature_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Boiler temperature 0x%04X every %" PRIu32 " ms", this->temperature_->get_address(),
                  this->temperature_->get_interval());
}

void VissLinkBurnerCycles::update_() {
  // Dans l'ordre des lectures : la température lue juste avant un
  // changement d'état doit être connue au moment où il est observé.
  const bool temperature_first =
      this->temperature_ != nullptr && this->temperature_->read_ok_count() != this->temperature_seen_ &&
      static_cast<int32_t>(this->temperature_->last_read_ok_ms() - this->burner_->last_read_ok_ms()) <= 0;
  if (temperature_first)
    this->observe_temperature_();
  this->observe_burner_();
  if (!temperature_first)
    this->observe_temperature_();
  this->observe_counter_(this->starts_, &this->starts_seen_, &this->starts_history_, "starts");
  this->observe_counter_(this->runtime_, &this->runtime_seen_, &this->runtime_history_, "runtime");

  const uint32_t now = millis();
  bool on = false;
  float value = NAN;
  while (this->tracker_.poll_edge_temperature(now, &on, &value)) {
    ESP_LOGD(TAG, "Boiler temperature at burner %s: %.1f (nearest reading, estimate)", on ? "start" : "stop",
             value);
    publish_if_changed_(on ? this->start_temperature_ : this->stop_temperature_, value);
  }
  this->publish_counters_(now);
}

void VissLinkBurnerCycles::observe_burner_() {
  const uint32_t count = this->burner_->read_ok_count();
  if (count == this->burner_seen_)
    return;
  this->burner_seen_ = count;
  const uint32_t t = this->burner_->last_read_ok_ms();
  const visslink::BurnerUpdate u =
      this->tracker_.on_burner(t, this->burner_->has_state(), this->burner_->has_state() && this->burner_->state);
  if (!u.cycle && !u.stop)
    return;
  if (std::isnan(u.duration_s)) {
    ESP_LOGD(TAG, "Burner %s: duration unknown (missed or unknown reading)", u.cycle ? "cycle" : "stop");
  } else {
    ESP_LOGI(TAG, "Burner %s: %.0f s +/- %.0f s (estimate from %" PRIu32 " ms readings)", u.cycle ? "cycle" : "stop",
             u.duration_s, u.uncertainty_s, this->burner_->get_interval());
  }
  if (u.cycle) {
    this->last_uncertainty_s_ = u.uncertainty_s;
    publish_if_changed_(this->last_cycle_, u.duration_s);
  } else {
    publish_if_changed_(this->last_stop_, u.duration_s);
  }
}

void VissLinkBurnerCycles::observe_temperature_() {
  if (this->temperature_ == nullptr)
    return;
  const uint32_t count = this->temperature_->read_ok_count();
  if (count == this->temperature_seen_)
    return;
  this->temperature_seen_ = count;
  this->tracker_.on_temperature(this->temperature_->last_read_ok_ms(), this->temperature_->has_state(),
                                this->temperature_->state);
}

void VissLinkBurnerCycles::observe_counter_(visslink::VissLinkSensor *sensor, uint32_t *seen,
                                            visslink::CounterHistory *history, const char *what) {
  if (sensor == nullptr)
    return;
  const uint32_t count = sensor->read_ok_count();
  if (count == *seen)
    return;
  *seen = count;
  uint32_t raw = 0;
  if (!sensor->last_raw(&raw))
    return;  // valeur « non disponible » : rien d'ajouté
  if (!history->add(sensor->last_read_ok_ms(), raw))
    ESP_LOGW(TAG, "Burner %s counter went backwards (now %" PRIu32 "): history restarted", what, raw);
}

void VissLinkBurnerCycles::publish_counters_(uint32_t now) {
  if (this->starts_ == nullptr) {
    this->publish_status_(visslink::BurnerCounterStats{});
    return;
  }
  const visslink::BurnerCounterStats stats = visslink::burner_counter_stats(
      this->starts_history_, this->starts_->get_interval(),
      this->runtime_ != nullptr ? &this->runtime_history_ : nullptr,
      this->runtime_ != nullptr ? this->runtime_->get_interval() : 0, now);
  publish_if_changed_(this->starts_24h_, stats.starts_24h);
  publish_if_changed_(this->starts_per_hour_, stats.starts_per_hour);
  publish_if_changed_(this->average_runtime_, stats.average_run_s);
  this->publish_status_(stats);
}

void VissLinkBurnerCycles::publish_status_(const visslink::BurnerCounterStats &stats) {
  if (this->status_ == nullptr)
    return;
  std::string text;
  if (this->starts_ != nullptr) {
    if (stats.window_ms == 0) {
      text = "Compteurs : en attente (1 h de lectures minimum)";
    } else {
      char buf[96];
      snprintf(buf, sizeof(buf), "Compteurs (exact) : fenêtre de %.1f h%s",
               static_cast<double>(stats.window_ms) / 3600000.0,
               std::isnan(stats.starts_24h) ? ", 24 h pas encore atteintes" : "");
      text = buf;
    }
    text += " ; ";
  }
  char buf[128];
  if (std::isnan(this->last_uncertainty_s_)) {
    snprintf(buf, sizeof(buf), "Durées et températures : estimations (brûleur lu toutes les %" PRIu32 " s)",
             this->burner_->get_interval() / 1000);
  } else {
    snprintf(buf, sizeof(buf),
             "Durées et températures : estimations (brûleur lu toutes les %" PRIu32 " s, dernier cycle ± %.0f s)",
             this->burner_->get_interval() / 1000, static_cast<double>(this->last_uncertainty_s_));
  }
  text += buf;
  if (text == this->last_status_)
    return;
  this->last_status_ = text;
  visslink::truncate_state(text);
  this->status_->publish_state(text);
}

void VissLinkBurnerCycles::publish_if_changed_(sensor::Sensor *sensor, float value) {
  if (sensor == nullptr)
    return;
  if (sensor->has_state()) {
    const float old = sensor->get_raw_state();
    if ((std::isnan(old) && std::isnan(value)) || old == value)
      return;
  } else if (std::isnan(value)) {
    return;  // rien encore : reste « inconnu » sans publication
  }
  sensor->publish_state(value);
}

}  // namespace visslink_burner
}  // namespace esphome
