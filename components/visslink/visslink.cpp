#include "visslink.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace visslink {

static const char *const TAG = "visslink";

void VissLinkComponent::setup() {
  const uint32_t now = millis();
  for (auto *sensor : this->sensors_)
    this->scheduler_.add(sensor->get_interval(), now);
}

void VissLinkComponent::loop() {
  const uint32_t now = millis();

  const KwResult result = this->kw_.loop(now, *this);
  if (result != KwResult::NONE)
    this->handle_result_(result, now);

  if (this->kw_.is_idle()) {
    const int index = this->scheduler_.acquire(now);
    if (index != RegisterScheduler::NONE &&
        !this->kw_.request_read(this->sensors_[index]->get_address(), VissLinkSensor::LENGTH, now))
      this->scheduler_.finish(false, now);  // impossible en pratique : ne jamais bloquer l'ordonnanceur
  }

  // Boucle rapide pendant une transaction, pour répondre au 0x05 sans
  // attendre les ~16 ms du cycle normal d'ESPHome.
  if (this->kw_.is_idle()) {
    this->high_freq_.stop();
  } else {
    this->high_freq_.start();
  }
}

void VissLinkComponent::handle_result_(KwResult result, uint32_t now) {
  const int index = this->scheduler_.current();
  if (index == RegisterScheduler::NONE)
    return;
  VissLinkSensor *sensor = this->sensors_[index];
  const uint16_t address = sensor->get_address();

  if (result == KwResult::SUCCESS) {
    this->scheduler_.finish(true, now);
    const uint8_t *data = this->kw_.data();
    const int16_t raw = decode_int16_le(data);
    ESP_LOGD(TAG, "0x%04X <- %02X %02X = %d", address, data[0], data[1], raw);
    if (!this->link_ok_) {
      ESP_LOGI(TAG, "Optolink communication established");
      this->link_ok_ = true;
      this->status_clear_warning();
    }
    sensor->publish_state(raw);
    return;
  }

  const unsigned attempt = this->scheduler_.entry(index).failures + 1u;
  const bool gave_up = this->scheduler_.finish(false, now);
  ESP_LOGW(TAG, "0x%04X: %s (attempt %u/%u)", address, kw_result_to_string(result), attempt,
           this->scheduler_.max_retries() + 1u);

  if (result == KwResult::SYNC_TIMEOUT) {
    if (this->link_ok_)
      ESP_LOGW(TAG, "No sync byte (0x05) from the boiler: optical link lost?");
    this->link_ok_ = false;
    this->status_set_warning();
  }

  // Tentatives épuisées : l'ancienne valeur n'est plus fiable, l'état devient
  // « inconnu » plutôt que de la laisser passer pour actuelle.
  if (gave_up && sensor->has_state() && !std::isnan(sensor->get_raw_state())) {
    ESP_LOGW(TAG, "0x%04X: giving up for this cycle, state set to unknown", address);
    sensor->publish_state(NAN);
  }
}

void VissLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink (Optolink KW, read-only):");
  for (auto *sensor : this->sensors_) {
    LOG_SENSOR("  ", "Register", sensor);
    ESP_LOGCONFIG(TAG, "    Address: 0x%04X, interval: %u ms", sensor->get_address(),
                  static_cast<unsigned>(sensor->get_interval()));
  }
}

}  // namespace visslink
}  // namespace esphome
