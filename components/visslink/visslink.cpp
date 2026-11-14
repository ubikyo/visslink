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
bool VissLinkSensor::publish_data(uint8_t /*block*/, const uint8_t *data) {
  this->publish_state(static_cast<float>(decode_value(this->data_type_, data)));
  return true;
}

void VissLinkSensor::publish_unknown(uint8_t /*block*/) {
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
bool VissLinkBinarySensor::publish_data(uint8_t /*block*/, const uint8_t *data) {
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
      this->publish_unknown(0);
      break;
  }
  return true;  // comportement v0.2 : valeur inattendue publiée « inconnu », sans nouvelle tentative
}

void VissLinkBinarySensor::publish_unknown(uint8_t /*block*/) {
  if (this->has_state())
    this->invalidate_state();
}

void VissLinkBinarySensor::dump_register_config() {
  LOG_BINARY_SENSOR("  ", "Binary sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: binary (1 byte), interval: %" PRIu32 " ms", this->address_,
                this->interval_ms_);
}
#endif

#ifdef USE_TEXT_SENSOR
bool VissLinkSchedule::publish_data(uint8_t block, const uint8_t *data) {
  DaySchedule day;
  const ScheduleError error = decode_schedule_day(data, SCHEDULE_BLOCK_LENGTH, &day);
  if (error != ScheduleError::NONE) {
    // Rien n'est publié : la journée garde sa dernière valeur valide.
    ESP_LOGW(TAG, "0x%04X (%s): schedule rejected, %s", this->get_block_address(block), SCHEDULE_DAY_LABELS[block],
             schedule_error_to_string(error));
    return false;
  }
  const bool changed = this->week_.set_day(block, day);
  ESP_LOGD(TAG, "%s %s: %s", this->get_name().c_str(), SCHEDULE_DAY_LABELS[block], format_day(day).c_str());
  if (block + 1 == SCHEDULE_DAYS)
    this->first_pass_done_ = true;
  if (changed && this->day_sensors_[block] != nullptr)
    publish_text_(this->day_sensors_[block], format_day(day));
  this->publish_week_();
  if (changed)
    this->refresh_clock_();
  return true;
}

void VissLinkSchedule::publish_unknown(uint8_t block) {
  // Une programmation ne périme pas comme une mesure : la dernière valeur
  // valide reste affichée (cf. README). Le résumé est publié dès que les 7
  // jours ont été tentés, avec « non lu » pour ceux qui manquent.
  if (block + 1 == SCHEDULE_DAYS) {
    this->first_pass_done_ = true;
    this->publish_week_();
  }
}

void VissLinkSchedule::publish_week_() {
  if (!this->first_pass_done_ && !this->week_.all_known())
    return;  // pas de résumé partiel pendant la première lecture
  publish_text_(this, format_week(this->week_));
}

void VissLinkSchedule::publish_text_(text_sensor::TextSensor *sensor, const std::string &text) {
  if (!sensor->has_state() || sensor->get_state() != text)
    sensor->publish_state(text);
}

void VissLinkSchedule::refresh_clock_() {
#ifdef USE_TIME
  if (this->clock_ == nullptr)
    return;
  const ESPTime now = this->clock_->now();
  const bool valid = now.is_valid();
  const int weekday = valid ? weekday_from_esphome(now.day_of_week) : -1;
  const int minute = valid ? now.hour * 60 + now.minute : -1;

  // Heure inconnue : rien n'est publié (l'état reste inconnu).
  if (this->today_sensor_ != nullptr && weekday >= 0)
    publish_text_(this->today_sensor_, this->week_.format_day_or_unknown(static_cast<uint8_t>(weekday)));

#ifdef USE_BINARY_SENSOR
  if (this->active_sensor_ != nullptr) {
    switch (schedule_active_state(this->week_, valid, weekday, minute)) {
      case ActiveState::ACTIVE:
        this->active_sensor_->publish_state(true);
        break;
      case ActiveState::INACTIVE:
        this->active_sensor_->publish_state(false);
        break;
      case ActiveState::UNKNOWN:
        if (this->active_sensor_->has_state())
          this->active_sensor_->invalidate_state();
        break;
    }
  }
#endif
#endif
}

void VissLinkSchedule::dump_register_config() {
  LOG_TEXT_SENSOR("  ", "Schedule", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X-0x%04X (7 x 8 bytes, Monday first), interval: %" PRIu32 " ms",
                this->address_, this->get_block_address(SCHEDULE_DAYS - 1) + SCHEDULE_BLOCK_LENGTH - 1,
                this->interval_ms_);
  for (uint8_t day = 0; day < SCHEDULE_DAYS; day++) {
    if (this->day_sensors_[day] != nullptr)
      ESP_LOGCONFIG(TAG, "    %s: '%s'", SCHEDULE_DAY_LABELS[day], this->day_sensors_[day]->get_name().c_str());
  }
#ifdef USE_TIME
  if (this->today_sensor_ != nullptr)
    LOG_TEXT_SENSOR("    ", "Today", this->today_sensor_);
#ifdef USE_BINARY_SENSOR
  if (this->active_sensor_ != nullptr)
    LOG_BINARY_SENSOR("    ", "Active", this->active_sensor_);
#endif
#endif
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
    this->scheduler_.add(this->registers_[i]->get_interval(), now + i * STARTUP_STAGGER_MS,
                         this->registers_[i]->get_block_count());
  // Timer natif d'ESPHome, hors du chemin des lectures.
  this->set_interval("clock", CLOCK_TICK_MS, [this]() {
    for (auto *reg : this->registers_)
      reg->on_clock_tick();
  });
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
      const uint16_t address = reg->get_block_address(this->scheduler_.block(index));
      if (!this->kw_.request_read(address, reg->get_length(), now))
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
  const uint8_t block = this->scheduler_.block(index);
  const uint16_t address = reg->get_block_address(block);
  const char *reason = kw_result_to_string(result);

  if (result == KwResult::SUCCESS) {
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
    if (reg->publish_data(block, data)) {
      this->scheduler_.finish(true, now);
      return;
    }
    reason = "invalid data";  // trame bien cadrée mais contenu rejeté : nouvelle tentative
  }

  const unsigned attempt = this->scheduler_.entry(index).failures + 1u;
  const bool gave_up = this->scheduler_.finish(false, now);
  ESP_LOGW(TAG, "0x%04X: %s (attempt %u/%u)", address, reason, attempt,
           this->scheduler_.max_retries() + 1u);

  if (result == KwResult::SYNC_TIMEOUT) {
    if (this->link_ok_)
      ESP_LOGW(TAG, "No sync byte (0x05) from the boiler: optical link lost?");
    this->link_ok_ = false;
    this->status_set_warning();
  }

  // Tentatives épuisées : la plateforme décide du sort de l'ancienne valeur
  // (mesure : « inconnu » plutôt que de la laisser passer pour actuelle ;
  // programmation : conservée).
  if (gave_up) {
    ESP_LOGW(TAG, "0x%04X: giving up for this cycle", address);
    reg->publish_unknown(block);
  }
}

void VissLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink (Optolink KW, read-only):");
  for (auto *reg : this->registers_)
    reg->dump_register_config();
}

}  // namespace visslink
}  // namespace esphome
