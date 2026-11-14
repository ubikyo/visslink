#include "visslink.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace visslink {

static const char *const TAG = "visslink";

// ---------------------------------------------------------------------------
// Plateformes

#ifdef USE_SENSOR
void VissLinkSensor::publish_data(const uint8_t *data) {
  this->publish_state(static_cast<float>(decode_value(this->data_type_, data)));
}

void VissLinkSensor::publish_unknown() {
  if (this->has_state() && !std::isnan(this->get_raw_state()))
    this->publish_state(NAN);
}

void VissLinkSensor::dump_register_config() {
  LOG_SENSOR("  ", "Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: %s, interval: %" PRIu32 " ms", this->address_,
                data_type_to_string(this->data_type_), this->interval_ms_);
}
#endif

#ifdef USE_BINARY_SENSOR
void VissLinkBinarySensor::publish_data(const uint8_t *data) {
  switch (decode_binary(data[0])) {
    case BinaryValue::OFF:
      this->publish_state(false);
      break;
    case BinaryValue::ON:
      this->publish_state(true);
      break;
    case BinaryValue::INVALID:
      // Valeur non documentée : ne pas la deviner (cf. README).
      ESP_LOGW(TAG, "0x%04X: unexpected binary value 0x%02X (expected 00 or 01), state set to unknown",
               this->address_, data[0]);
      this->publish_unknown();
      break;
  }
}

void VissLinkBinarySensor::publish_unknown() {
  if (this->has_state())
    this->invalidate_state();
}

void VissLinkBinarySensor::dump_register_config() {
  LOG_BINARY_SENSOR("  ", "Binary sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: binary (1 byte), interval: %" PRIu32 " ms", this->address_,
                this->interval_ms_);
}
#endif

// ---------------------------------------------------------------------------
// Composant

void VissLinkComponent::setup() {
  // Premières échéances décalées d'une période de synchro : sinon tous les
  // registres restent calés sur le même instant (échéance + intervalle) et
  // reviennent en rafale, au point de faire sauter des cycles aux registres
  // rapides (cf. test sim_v02_simultaneous_start_loses_cycles).
  const uint32_t now = millis();
  for (size_t i = 0; i < this->registers_.size(); i++)
    this->scheduler_.add(this->registers_[i]->get_interval(), now + i * STARTUP_STAGGER_MS);
}

void VissLinkComponent::loop() {
  const uint32_t now = millis();

  const KwResult result = this->kw_.loop(now, *this);
  if (result != KwResult::NONE)
    this->handle_result_(result, now);

  if (this->kw_.is_idle()) {
    const int index = this->scheduler_.acquire(now);
    if (index != RegisterScheduler::NONE) {
      const VissLinkRegister *reg = this->registers_[index];
      if (!this->kw_.request_read(reg->get_address(), reg->get_length(), now))
        this->scheduler_.finish(false, now);  // impossible en pratique : ne jamais bloquer l'ordonnanceur
    }
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
  VissLinkRegister *reg = this->registers_[index];
  const uint16_t address = reg->get_address();

  if (result == KwResult::SUCCESS) {
    this->scheduler_.finish(true, now);
    const uint8_t *data = this->kw_.data();
    const uint8_t length = this->kw_.data_length();
    // Octets bruts tels que reçus : permet de vérifier un registre
    // expérimental avant de se fier à son interprétation.
    char hex[3 * KwProtocol::MAX_READ_LENGTH + 1];
    for (uint8_t i = 0; i < length; i++)
      snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    hex[length > 0 ? 3 * length - 1 : 0] = '\0';
    ESP_LOGD(TAG, "0x%04X <- %s", address, hex);
    if (!this->link_ok_) {
      ESP_LOGI(TAG, "Optolink communication established");
      this->link_ok_ = true;
      this->status_clear_warning();
    }
    reg->publish_data(data);
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
  if (gave_up) {
    ESP_LOGW(TAG, "0x%04X: giving up for this cycle", address);
    reg->publish_unknown();
  }
}

void VissLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink (Optolink KW, read-only):");
  for (auto *reg : this->registers_)
    reg->dump_register_config();
}

}  // namespace visslink
}  // namespace esphome
