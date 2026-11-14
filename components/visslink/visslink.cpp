#include "visslink.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "decode.h"

namespace esphome {
namespace visslink {

static const char *const TAG = "visslink";

void VissLinkComponent::setup() {
  // Toutes les allocations ont lieu ici, jamais dans loop().
  RegisterScheduler &scheduler = this->engine_.scheduler();
  scheduler.reserve(this->sensors_.size());
  for (auto *sensor : this->sensors_)
    scheduler.add(sensor->get_address(), sensor->get_length(), sensor->get_interval());
  scheduler.start(millis());
}

void VissLinkComponent::loop() {
  if (this->engine_.loop(millis(), &this->event_))
    this->handle_event_(this->event_);

  // Boucle rapide uniquement pendant une transaction, pour répondre au 0x05
  // sans attendre les ~16 ms du cycle normal d'ESPHome.
  if (this->engine_.transaction_active()) {
    this->high_freq_.start();
  } else {
    this->high_freq_.stop();
  }
}

void VissLinkComponent::handle_event_(const ReadEvent &event) {
  VissLinkSensor *sensor = this->sensors_[event.index];
  const uint16_t address = sensor->get_address();

  if (event.result == KwResult::SUCCESS) {
    const float value = decode_register(event.data, event.length, sensor->is_signed());
    if (event.length == 2) {
      ESP_LOGD(TAG, "0x%04X <- %02X %02X = %.0f", address, event.data[0], event.data[1], value);
    } else {
      ESP_LOGD(TAG, "0x%04X <- %.0f (%u bytes)", address, value, event.length);
    }
    if (!this->link_ok_) {
      ESP_LOGI(TAG, "Optolink communication established");
      this->link_ok_ = true;
      this->status_clear_warning();
    }
    sensor->publish_state(value);
    return;
  }

  ESP_LOGW(TAG, "0x%04X: %s (attempt %u/%u)", address, kw_result_to_string(event.result), event.attempt,
           this->engine_.scheduler().max_retries() + 1u);

  if (event.result == KwResult::SYNC_TIMEOUT) {
    if (this->link_ok_)
      ESP_LOGW(TAG, "No sync byte (0x05) from the boiler: optical link lost?");
    this->link_ok_ = false;
    this->status_set_warning();
  }

  // Après épuisement des tentatives, la valeur précédente n'est plus fiable :
  // l'état devient « inconnu » plutôt que de laisser croire qu'elle est à jour.
  if (event.exhausted && sensor->has_state() && !std::isnan(sensor->get_raw_state())) {
    ESP_LOGW(TAG, "0x%04X: giving up for this cycle, state set to unknown", address);
    sensor->publish_state(NAN);
  }
}

void VissLinkComponent::dump_config() {
  const KwStats &stats = this->engine_.protocol().stats();
  ESP_LOGCONFIG(TAG,
                "VissLink (Optolink KW, read-only):\n"
                "  Reads OK: %u, sync timeouts: %u, response timeouts: %u, too short: %u, too long: %u",
                static_cast<unsigned>(stats.success), static_cast<unsigned>(stats.sync_timeouts),
                static_cast<unsigned>(stats.response_timeouts), static_cast<unsigned>(stats.too_short),
                static_cast<unsigned>(stats.too_long));
  for (auto *sensor : this->sensors_) {
    LOG_SENSOR("  ", "Register", sensor);
    ESP_LOGCONFIG(TAG, "    Address: 0x%04X, length: %u, %s, interval: %u ms", sensor->get_address(),
                  sensor->get_length(), sensor->is_signed() ? "signed" : "unsigned",
                  static_cast<unsigned>(sensor->get_interval()));
  }
  // Paramètres UART (4800 8E2) vérifiés à la compilation par
  // uart.final_validate_device_schema() dans __init__.py.
}

}  // namespace visslink
}  // namespace esphome
