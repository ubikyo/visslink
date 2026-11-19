#include "visslink.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_CONTROLLER_REGISTRY
#include "esphome/core/controller_registry.h"
#endif

namespace esphome {
namespace visslink {

static const char *const TAG = "visslink";

// ---------------------------------------------------------------------------
// Plateformes

#ifdef USE_SENSOR
bool VissLinkSensor::publish_data(uint8_t /*block*/, const uint8_t *data) {
  const uint8_t length = this->get_length();
  if (this->invalid_raw_.contains(decode_raw_le(data, length))) {
    // Réponse bien reçue (pas de nouvelle tentative), mais la régulation
    // n'a pas de valeur : « inconnu », jamais une mesure.
    if (!this->invalid_logged_) {
      ESP_LOGW(TAG, "0x%04X: raw value 0x%0*" PRIX32 " means 'not available', published as unknown", this->address_,
               2 * length, decode_raw_le(data, length));
      this->invalid_logged_ = true;
    }
    this->raw_valid_ = false;
    if (!this->has_state() || !std::isnan(this->get_raw_state()))
      this->publish_state(NAN);
    return true;
  }
  if (this->invalid_logged_) {
    ESP_LOGI(TAG, "0x%04X: value available again", this->address_);
    this->invalid_logged_ = false;
  }
  this->raw_ = decode_raw_le(data, length);
  this->raw_valid_ = true;
  this->publish_state(static_cast<float>(decode_value(this->data_type_, data)));
  return true;
}

void VissLinkSensor::publish_unknown(uint8_t /*block*/) {
  this->raw_valid_ = false;
  if (this->has_state() && !std::isnan(this->get_raw_state()))
    this->publish_state(NAN);
}

void VissLinkSensor::dump_register_config() {
  LOG_SENSOR("  ", "Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: %s, interval: %" PRIu32 " ms", this->address_,
                data_type_to_string(this->data_type_), this->interval_ms_);
  for (uint8_t i = 0; i < this->invalid_raw_.count; i++)
    ESP_LOGCONFIG(TAG, "    Not available: 0x%0*" PRIX32 " (published as unknown)", 2 * this->get_length(),
                  this->invalid_raw_.values[i]);
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
  if (this->raw_ != nullptr) {
    std::memcpy(this->raw_->week.raw[block], data, SCHEDULE_BLOCK_LENGTH);
    this->raw_->week.known_mask |= static_cast<uint8_t>(1u << block);
    this->raw_->read_ms[block] = millis();
  }
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

void VissLinkSchedule::enable_raw_cache() {
  if (this->raw_ == nullptr)
    this->raw_.reset(new RawCache());
}

void VissLinkSchedule::raw_week(WeekBlocks *out) const {
  *out = this->raw_ != nullptr ? this->raw_->week : WeekBlocks();
}

bool VissLinkSchedule::day_read_ms(uint8_t day, uint32_t *ms) const {
  if (this->raw_ == nullptr || !this->raw_->week.known(day))
    return false;
  *ms = this->raw_->read_ms[day];
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

int VissLinkSchedule::preferred_first_block() const {
#ifdef USE_TIME
  if (this->clock_ == nullptr)
    return -1;
  const ESPTime now = this->clock_->now();
  return now.is_valid() ? weekday_from_esphome(now.day_of_week) : -1;
#else
  return -1;
#endif
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

#ifdef USE_NUMBER
bool VissLinkNumber::publish_data(uint8_t /*block*/, const uint8_t *data) {
  const uint8_t value = data[0];
  if (this->verification_.pending()) {
    const uint8_t expected = this->verification_.expected();
    const VerifyResult result = this->verification_.on_read(value);
    if (result == VerifyResult::CONFIRMED) {
      ESP_LOGI(TAG, "0x%04X: write confirmed by read-back (%u)", this->address_, value);
    } else {
      ESP_LOGW(TAG, "0x%04X: write %s: read back %u, requested %u", this->address_, verify_result_to_string(result),
               value, expected);
    }
  }
  // Toujours la valeur lue, jamais la valeur demandée.
  this->publish_state(value);
  return true;
}

void VissLinkNumber::publish_unknown(uint8_t /*block*/) {
  if (this->verification_.pending()) {
    ESP_LOGW(TAG, "0x%04X: write %s: read-back failed, requested %u", this->address_,
             verify_result_to_string(this->verification_.on_give_up()), this->verification_.expected());
    uint8_t lo, hi;
    if (setpoint_write_limits(this->address_, &lo, &hi)) {
      // v0.6/v0.7 : consigne ECS ou de chauffage, issue inconnue : la
      // dernière valeur confirmée reste affichée (jamais la valeur demandée).
      this->restore_read_state();
      return;
    }
  }
  if (this->has_read_value())
    this->publish_state(NAN);
}

void VissLinkNumber::restore_read_state() {
  // Republier l'état actuel (lu, ou inconnu) : Home Assistant abandonne la
  // valeur demandée.
  this->publish_state(this->has_state() ? this->state : NAN);
}

bool VissLinkNumber::write_allowed(uint8_t value) const {
  uint8_t encoded;
  return check_write_value(this->address_, this->get_length(), value, this->traits.get_min_value(),
                           this->traits.get_max_value(), &encoded) == WriteCheck::OK &&
         encoded == value;
}

void VissLinkNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->submit_write(this, value);
}

void VissLinkNumber::dump_register_config() {
  LOG_NUMBER("  ", "Number", this);
  const WritableRegister *reg = find_writable(this->address_);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: uint8, interval: %" PRIu32 " ms, range: %.0f..%.0f (documented %d..%d)",
                this->address_, this->interval_ms_, this->traits.get_min_value(), this->traits.get_max_value(),
                reg != nullptr ? reg->min : 0, reg != nullptr ? reg->max : 0);
}
#endif

#ifdef USE_TEXT_SENSOR
bool VissLinkModeText::publish_data(uint8_t /*block*/, const uint8_t *data) {
  const int index = operating_mode_index(data[0]);
  if (index < 0) {
    ESP_LOGW(TAG, "0x%04X: unexpected operating mode 0x%02X (expected 00, 03 or 05), shown as unknown",
             this->address_, data[0]);
    this->raw_ = -1;
  } else {
    this->raw_ = data[0];
  }
  this->publish_text_(operating_mode_text(this->raw_));
  if (this->parent_ != nullptr)
    this->parent_->on_mode_state_changed();
  return true;
}

void VissLinkModeText::publish_unknown(uint8_t /*block*/) {
  this->raw_ = -1;
  this->publish_text_(operating_mode_text(this->raw_));
  if (this->parent_ != nullptr)
    this->parent_->on_mode_state_changed();
}

void VissLinkModeText::publish_text_(const char *text) {
  if (!this->has_state() || this->get_state() != text)
    this->publish_state(text);
}

void VissLinkModeText::dump_register_config() {
  LOG_TEXT_SENSOR("  ", "Operating mode (read-only)", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: uint8 (00/03/05), interval: %" PRIu32 " ms, write: never",
                this->address_, this->interval_ms_);
}
#endif

#ifdef USE_TEXT_SENSOR
bool VissLinkClockText::publish_data(uint8_t /*block*/, const uint8_t *data) {
  VitoClock clock;
  const ClockError error = decode_vito_clock(data, VITO_CLOCK_LENGTH, &clock);
  if (error == ClockError::NONE) {
    this->publish_state(format_vito_clock(clock));
  } else {
    ESP_LOGW(TAG, "0x%04X: clock %s (%s)", this->address_, clock_error_to_string(error), hex_block(data).c_str());
    this->publish_state(std::string("invalide (") + clock_error_to_string(error) + ") : " + hex_block(data));
  }
  return true;
}

void VissLinkClockText::publish_unknown(uint8_t /*block*/) { this->publish_state("Inconnu"); }

void VissLinkClockText::dump_register_config() {
  LOG_TEXT_SENSOR("  ", "Vitotronic clock (read-only)", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, 8 bytes BCD, interval: %" PRIu32 " ms", this->address_,
                this->interval_ms_);
}
#endif

#ifdef USE_SELECT
bool VissLinkModeSelect::publish_data(uint8_t /*block*/, const uint8_t *data) {
  const uint8_t raw = data[0];
  if (this->verification_.pending()) {
    const uint8_t expected = this->verification_.expected();
    const VerifyResult result = this->verification_.on_read(raw);
    if (result == VerifyResult::CONFIRMED) {
      ESP_LOGI(TAG, "0x%04X: operating mode write confirmed by read-back (%02X)", this->address_, raw);
    } else {
      ESP_LOGW(TAG, "0x%04X: operating mode write %s: read back %02X, requested %02X", this->address_,
               verify_result_to_string(result), raw, expected);
    }
  }
  const int index = operating_mode_index(raw);
  if (index < 0) {
    // Valeur non documentée : ne pas la deviner (cf. binary_sensor).
    ESP_LOGW(TAG, "0x%04X: unexpected operating mode 0x%02X (expected 00, 03 or 05), state set to unknown",
             this->address_, raw);
    this->publish_unknown_state_();
  } else {
    this->publish_state(static_cast<size_t>(index));
  }
  if (this->parent_ != nullptr)
    this->parent_->on_mode_state_changed();
  return true;
}

void VissLinkModeSelect::publish_unknown(uint8_t /*block*/) {
  if (this->verification_.pending()) {
    ESP_LOGW(TAG, "0x%04X: operating mode write %s: read-back failed, requested %02X", this->address_,
             verify_result_to_string(this->verification_.on_give_up()), this->verification_.expected());
  }
  this->publish_unknown_state_();
  if (this->parent_ != nullptr)
    this->parent_->on_mode_state_changed();
}

void VissLinkModeSelect::publish_unknown_state_() {
  if (!this->has_state())
    return;
  this->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
  ControllerRegistry::notify_select_update(this);  // Home Assistant : état inconnu
#endif
}

void VissLinkModeSelect::restore_read_state() {
  // Republier le programme lu pour que Home Assistant abandonne l'option
  // demandée.
  const auto index = this->active_index();
  if (this->has_state() && index.has_value()) {
    this->publish_state(index.value());
  } else {
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_select_update(this);
#endif
  }
}

bool VissLinkModeSelect::write_allowed(uint8_t value) const {
  return is_mode_register(this->address_) && check_mode_write(this->address_, this->get_length(), value) == WriteCheck::OK;
}

int VissLinkModeSelect::mode_raw() const {
  const auto index = this->active_index();
  if (!this->has_state() || !index.has_value() || index.value() >= OPERATING_MODE_COUNT)
    return -1;
  return OPERATING_MODE_VALUES[index.value()];
}

void VissLinkModeSelect::control(size_t index) {
  if (this->parent_ != nullptr)
    this->parent_->submit_mode_write(this, index);
}

void VissLinkModeSelect::dump_register_config() {
  LOG_SELECT("  ", "Operating mode", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: uint8 (00/03/05), interval: %" PRIu32 " ms, write: %s", this->address_,
                this->interval_ms_, is_mode_write_whitelisted(this->address_) ? "documented (experimental)" : "never");
}
#endif

#ifdef USE_SWITCH
bool VissLinkFlagSwitch::publish_data(uint8_t /*block*/, const uint8_t *data) {
  const uint8_t raw = data[0];
  if (this->verification_.pending()) {
    const uint8_t expected = this->verification_.expected();
    const VerifyResult result = this->verification_.on_read(raw);
    if (result == VerifyResult::CONFIRMED) {
      ESP_LOGI(TAG, "0x%04X: Eco/Party write confirmed by read-back (%02X)", this->address_, raw);
    } else {
      ESP_LOGW(TAG, "0x%04X: Eco/Party write %s: read back %02X, requested %02X", this->address_,
               verify_result_to_string(result), raw, expected);
    }
  }
  if (raw > 1) {
    // Valeur non documentée : jamais interprétée (cf. binary_sensor).
    ESP_LOGW(TAG, "0x%04X: unexpected Eco/Party value 0x%02X (expected 00 or 01), state not updated", this->address_,
             raw);
    return true;
  }
  this->raw_ = raw;
  // Toujours la valeur lue, jamais la commande.
  this->publish_state(raw == 1);
  return true;
}

void VissLinkFlagSwitch::publish_unknown(uint8_t /*block*/) {
  if (this->verification_.pending()) {
    ESP_LOGW(TAG, "0x%04X: Eco/Party write %s: read-back failed, requested %02X", this->address_,
             verify_result_to_string(this->verification_.on_give_up()), this->verification_.expected());
  }
  // Un switch n'a pas d'état « inconnu » dans Home Assistant : le dernier
  // état lu reste affiché ; le texte d'état et le journal signalent l'échec.
}

void VissLinkFlagSwitch::restore_read_state() {
  // Republier l'état lu : Home Assistant abandonne la commande (publish_state
  // ignore une valeur identique, d'où la notification directe).
#ifdef USE_CONTROLLER_REGISTRY
  ControllerRegistry::notify_switch_update(this);
#endif
}

bool VissLinkFlagSwitch::write_allowed(uint8_t value) const {
  return check_flag_write(this->address_, this->get_length(), value) == WriteCheck::OK;
}

void VissLinkFlagSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) {
    this->parent_->submit_flag_write(this, state);
  } else {
    this->restore_read_state();
  }
}

void VissLinkFlagSwitch::dump_register_config() {
  LOG_SWITCH("  ", "Eco/Party", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X, type: uint8 (00/01), interval: %" PRIu32 " ms", this->address_,
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
    this->scheduler_.add(this->registers_[i]->get_interval(), now + i * STARTUP_STAGGER_MS,
                         this->registers_[i]->get_block_count());
  for (auto &ch : this->setpoints_)
    ch.owner = this->index_of_(ch.reg);
  for (auto &ch : this->flags_)
    ch.owner = this->index_of_(ch.reg);
  // Timer natif d'ESPHome, hors du chemin des lectures.
  this->set_interval("clock", CLOCK_TICK_MS, [this]() {
    for (auto *reg : this->registers_)
      reg->on_clock_tick();
    this->current_day_first_(millis());
  });
}

void VissLinkComponent::current_day_first_(uint32_t now) {
  // Programmations (v0.5.1) : chaque cycle commence par le jour courant, et
  // ce jour est relu sans attendre son tour tant qu'il n'est pas connu (au
  // démarrage, l'heure arrive souvent après le début du premier cycle).
  for (size_t i = 0; i < this->registers_.size(); i++) {
    VissLinkRegister *reg = this->registers_[i];
    const int first = reg->preferred_first_block();
    if (first < 0 || first >= reg->get_block_count())
      continue;
    const uint8_t day = static_cast<uint8_t>(first);
    this->scheduler_.set_cycle_start(i, day);  // sans effet sur un cycle commencé
    const RegisterScheduler::Entry &e = this->scheduler_.entry(i);
    const bool next_in_cycle = e.urgent == RegisterScheduler::URGENT_NONE && e.done == 0 && e.block == day;
    if (next_in_cycle || !reg->wants_block_refresh(day) || !this->scheduler_.expedite_block(i, day, now))
      continue;
    reg->on_block_refresh_requested(day);
    ESP_LOGD(TAG, "0x%04X: current day read first", reg->get_block_address(day));
  }
}

bool VissLinkComponent::find_schedule_block_(uint16_t address, size_t *index, uint8_t *block) const {
  for (size_t i = 0; i < this->registers_.size(); i++) {
    const VissLinkRegister *reg = this->registers_[i];
    if (reg->get_block_count() < 2)
      continue;
    for (uint8_t b = 0; b < reg->get_block_count(); b++) {
      if (reg->get_block_address(b) == address) {
        *index = i;
        *block = b;
        return true;
      }
    }
  }
  return false;
}

bool VissLinkComponent::is_schedule_block(uint16_t address) const {
  size_t index;
  uint8_t block;
  return this->find_schedule_block_(address, &index, &block);
}

bool VissLinkComponent::request_block_refresh(uint16_t address) {
  size_t index;
  uint8_t block;
  if (!this->find_schedule_block_(address, &index, &block)) {
    ESP_LOGW(TAG, "0x%04X: block refresh refused: not a declared schedule block", address);
    return false;
  }
  const bool ok = this->scheduler_.expedite_block(index, block, millis());
  ESP_LOGD(TAG, "0x%04X: block refresh %s", address, ok ? "requested" : "deferred (busy)");
  return ok;
}

bool VissLinkComponent::cancel_block_refresh(uint16_t address) {
  size_t index;
  uint8_t block;
  if (!this->find_schedule_block_(address, &index, &block) || !this->scheduler_.cancel_refresh(index, block))
    return false;
  ESP_LOGD(TAG, "0x%04X: pending block refresh cancelled", address);
  return true;
}

bool VissLinkComponent::block_refresh_pending(uint16_t address) const {
  size_t index;
  uint8_t block;
  return this->find_schedule_block_(address, &index, &block) && this->scheduler_.refresh_pending(index, block);
}

void VissLinkComponent::loop() {
  const uint32_t now = millis();

  const KwResult result = this->kw_.loop(now, *this);
  if (result != KwResult::NONE) {
    if (this->write_in_flight_) {
      this->handle_write_result_(result, now);
    } else if (this->direct_.in_flight) {
      this->handle_direct_result_(result, now);
    } else {
      this->handle_result_(result, now);
    }
  }

  for (auto &ch : this->setpoints_) {
    if (ch.writer.poll(now))
      ESP_LOGW(TAG, "0x%04X: setpoint change dropped: no read of the current value", ch.writer.address());
    this->setpoint_publish_(ch);
  }
  for (auto &ch : this->flags_) {
    if (ch.writer.poll(now))
      ESP_LOGW(TAG, "0x%04X: Eco/Party command dropped: no read of the current state", ch.writer.address());
    this->flag_publish_(ch);
  }

  this->preempt_unsent_read_(now);
  if (this->kw_.is_idle() && this->scheduler_.current() == RegisterScheduler::NONE && !this->direct_.in_flight) {
    this->drop_expired_writes_(now);
    // Une écriture en attente passe avant les lectures (commande manuelle),
    // dans les limites de WriteArbiter : une à la fois, espacées. Ensuite,
    // une lecture ponctuelle de bloc horaire (v0.5).
    if (!this->start_write_(now))
      this->start_direct_read_(now);
  }

  if (this->kw_.is_idle()) {
    // Pendant les relectures de vérification (y compris celles d'un bloc
    // horaire), aucune lecture ordinaire intercalée.
    const int index = this->scheduler_.acquire(
        now, !this->read_back_.idle() || this->direct_.pending || this->block_verify_pending_);
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
    // v0.6 : relecture de vérification d'une écriture de ce registre ?
    const bool verify = this->writes_.active() && this->writes_.current().owner == index &&
                        this->read_back_.waiting(index);
    // v0.9.1 : relecture de cette commande après l'écriture de l'autre
    // commande Éco/Party du circuit.
    const bool peer_read_back = this->writes_.active() && this->writes_.current().owner != index &&
                                this->read_back_.waiting(index);
    if (reg->publish_data(block, data)) {
      reg->mark_read_ok(now);  // v0.7.3 : fraîcheur (climate M2)
      this->scheduler_.finish(true, now);
      if (SetpointChannel *ch = this->setpoint_channel_(index))
        this->setpoint_on_read_(*ch, true, data[0], verify, now);
      if (FlagChannel *ch = this->flag_channel_(index))
        this->flag_on_read_(*ch, true, data[0], verify, peer_read_back, now);
      this->read_back_done_(index, now);  // relecture de vérification terminée ?
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
    const bool verify = this->writes_.active() && this->writes_.current().owner == index &&
                        this->read_back_.waiting(index);
    reg->publish_unknown(block);
    if (SetpointChannel *ch = this->setpoint_channel_(index))
      this->setpoint_on_read_(*ch, false, 0, verify, now);
    if (FlagChannel *ch = this->flag_channel_(index))
      this->flag_on_read_(*ch, false, 0, verify, false, now);
    this->read_back_done_(index, now);  // relecture de vérification abandonnée ?
  }
}

// ---------------------------------------------------------------------------
// Écritures (v0.3)

int VissLinkComponent::index_of_(const VissLinkRegister *reg) const {
  for (size_t i = 0; i < this->registers_.size(); i++) {
    if (this->registers_[i] == reg)
      return static_cast<int>(i);
  }
  return WriteArbiter::NONE;
}

#ifdef USE_NUMBER
void VissLinkComponent::submit_write(VissLinkNumber *number, float value) {
  // v0.6/v0.7 : toute consigne passe par sa transaction dédiée (autorisation
  // propre, relecture avant et après). Aucun autre chemin d'écriture.
  SetpointChannel *ch = this->setpoint_channel_(this->index_of_(number));
  if (ch == nullptr) {
    uint8_t byte = 0;
    const WriteCheck check = check_write_value(number->get_address(), number->get_length(), value,
                                               number->traits.get_min_value(), number->traits.get_max_value(), &byte);
    ESP_LOGW(TAG, "0x%04X: write of %.1f refused: %s; nothing sent", number->get_address(), value,
             check != WriteCheck::OK ? write_check_to_string(check) : "not a managed setpoint");
    number->restore_read_state();
    return;
  }
  this->submit_setpoint_(*ch, number, value);
}
#endif

void VissLinkComponent::drop_expired_writes_(uint32_t now) {
  WriteArbiter::Request request;
  while (this->writes_.pop_expired(now, &request)) {
    ESP_LOGW(TAG, "0x%04X: write of %u dropped: not sent within %" PRIu32 " s", request.address, request.value,
             WriteArbiter::REQUEST_EXPIRY_MS / 1000);
    if (SetpointChannel *ch = this->setpoint_channel_(request.owner))
      ch->writer.on_dropped(VL_TEXT("délai de 120 s dépassé"));
    if (FlagChannel *ch = this->flag_channel_(request.owner))
      ch->writer.on_dropped(VL_TEXT("délai de 120 s dépassé"));
    if (BlockClients::is_block_owner(request.owner)) {
      if (VissLinkBlockClient *client = this->block_clients_.get(request.owner))
        client->on_block_write(request.address, false, false, now);  // rien d'émis
    } else {
      this->registers_[request.owner]->restore_read_state();
    }
  }
}

bool VissLinkComponent::start_write_(uint32_t now) {
  WriteArbiter::Request request;
  if (!this->writes_.next(now, this->link_ok_, &request, this->deferred_setpoints_()))
    return false;
  // Dernier contrôle avant émission : la demande a été validée au dépôt,
  // mais rien ne part sans un nouveau contrôle complet (adresse de la liste
  // blanche, longueur, bornes configurées et documentées).
  if (BlockClients::is_block_owner(request.owner)) {
    // Bloc horaire (v0.5, v0.8) : liste blanche du propriétaire, validité du bloc, verrou de l'autre client et attente
    // effective du client, revérifiés juste avant l'émission.
    if (request.length != KwProtocol::BLOCK_WRITE_LENGTH ||
        !this->block_write_allowed_(request.owner, request.address, request.data) ||
        !this->kw_.request_write(request.address, request.data, KwProtocol::BLOCK_WRITE_LENGTH, now)) {
      ESP_LOGE(TAG, "0x%04X: schedule block write refused at send time", request.address);
      this->writes_.complete(now);
      if (VissLinkBlockClient *client = this->block_clients_.get(request.owner))
        client->on_block_write(request.address, false, false, now);
      return false;
    }
    ESP_LOGI(TAG, "0x%04X -> %s (schedule block write, experimental)", request.address,
             hex_block(request.data).c_str());
    this->write_in_flight_ = true;
    this->timing_ = WriteTiming();
    this->timing_.requested = request.requested_ms;
    this->timing_.started = now;
    return true;
  }
  // Programme : liste blanche des programmes et valeur 00/03/05.
  VissLinkRegister *reg = this->registers_[request.owner];
  const bool mode_ok = !is_mode_register(request.address) ||
                       check_mode_write(request.address, 1, request.value) == WriteCheck::OK;
  // Consigne : transaction en attente de exactement cette valeur. Toute
  // adresse de consigne l'exige.
  SetpointChannel *ch = this->setpoint_channel_(request.owner);
  const bool setpoint = find_writable(request.address) != nullptr;
  const bool setpoint_ok = !setpoint || (ch != nullptr && ch->writer.address() == request.address &&
                                         ch->writer.expects_write(request.value));
  // v0.9.1 : commande Éco/Party : transaction en attente de exactement
  // cette valeur, 00 ou 01.
  FlagChannel *fc = this->flag_channel_(request.owner);
  const bool flag = is_eco_party_flag_whitelisted(request.address);
  const bool flag_ok = !flag || (fc != nullptr && fc->writer.address() == request.address &&
                                 fc->writer.expects_write(request.value) &&
                                 check_flag_write(request.address, 1, request.value) == WriteCheck::OK);
  if (reg->get_address() != request.address || !is_write_whitelisted(request.address) || !mode_ok || !setpoint_ok ||
      !flag_ok || !reg->write_allowed(request.value) ||
      !this->kw_.request_write(request.address, &request.value, 1, now)) {
    ESP_LOGE(TAG, "0x%04X: write of %u refused at send time", request.address, request.value);
    this->writes_.complete(now);
    if (ch != nullptr)
      ch->writer.on_dropped(VL_TEXT("refusée au moment de l'émission"));
    if (fc != nullptr)
      fc->writer.on_dropped(VL_TEXT("refusée au moment de l'émission"));
    reg->restore_read_state();
    return false;
  }
  ESP_LOGI(TAG, "0x%04X -> %02X (write)", request.address, request.value);
  this->write_in_flight_ = true;
  this->timing_ = WriteTiming();
  this->timing_.requested = request.requested_ms;
  this->timing_.started = now;
  return true;
}

void VissLinkComponent::preempt_unsent_read_(uint32_t now) {
  // Une commande manuelle prête prend la place d'une lecture ordinaire
  // acquise mais encore en attente du 0x05 : aucun octet n'a été émis, rien
  // n'est interrompu sur la ligne. La lecture garde son échéance et repasse
  // après. Une transaction dont la trame est partie n'est jamais touchée,
  // ni une relecture de vérification (un rafraîchissement de bloc, si).
  const int current = this->scheduler_.current();
  const bool direct_ready = this->direct_.pending && !this->direct_.in_flight && time_reached(now, this->direct_.due_ms);
  if (this->write_in_flight_ || this->direct_.in_flight || current == RegisterScheduler::NONE ||
      this->scheduler_.entry(current).urgent == RegisterScheduler::URGENT_VERIFY || !this->kw_.is_waiting_sync() ||
      !(this->writes_.ready(now, this->link_ok_, this->deferred_setpoints_()) || direct_ready))
    return;
  if (this->kw_.cancel_before_send()) {
    this->scheduler_.release();
    ESP_LOGD(TAG, "0x%04X: periodic read deferred for a manual command (nothing sent yet)",
             this->registers_[current]->get_block_address(this->scheduler_.block(current)));
  }
}

void VissLinkComponent::log_write_timing_(uint16_t address) {
  if (!this->timing_valid_)
    return;
  const WriteTiming &t = this->timing_;
  ESP_LOGI(TAG,
           "0x%04X: write timing: line wait %" PRIu32 " ms, sync wait %" PRIu32 " ms, send+ack %" PRIu32
           " ms, read-back %" PRIu32 " ms; state published %" PRIu32 " ms after the request, all read-backs %" PRIu32
           " ms",
           address, t.queue_ms(), t.sync_ms(), t.ack_ms(), t.read_back_ms(), t.applied_ms(), t.total_ms());
}

void VissLinkComponent::handle_write_result_(KwResult result, uint32_t now) {
  this->write_in_flight_ = false;
  this->timing_.sent = this->kw_.sent_ms();
  this->timing_.acked = now;
  this->timing_valid_ = result != KwResult::SYNC_TIMEOUT;  // sinon, rien n'a été émis
  const WriteArbiter::Request &request = this->writes_.current();
  if (BlockClients::is_block_owner(request.owner)) {
    this->handle_block_write_result_(result, now);
    return;
  }
  VissLinkRegister *reg = this->registers_[request.owner];
  if (SetpointChannel *ch = this->setpoint_channel_(request.owner)) {
    ch->writer.on_ack(result == KwResult::SUCCESS          ? DhwAck::RECEIVED
                      : result == KwResult::WRITE_REJECTED ? DhwAck::REJECTED
                      : result == KwResult::SYNC_TIMEOUT   ? DhwAck::NOT_SENT
                                                           : DhwAck::INDETERMINATE);
  }
  if (FlagChannel *ch = this->flag_channel_(request.owner)) {
    ch->writer.on_ack(result == KwResult::SUCCESS          ? DhwAck::RECEIVED
                      : result == KwResult::WRITE_REJECTED ? DhwAck::REJECTED
                      : result == KwResult::SYNC_TIMEOUT   ? DhwAck::NOT_SENT
                                                           : DhwAck::INDETERMINATE);
  }
  switch (result) {
    case KwResult::SUCCESS:
      ESP_LOGD(TAG, "0x%04X <- %02X (write acknowledged)", request.address, this->kw_.data()[0]);
      break;
    case KwResult::WRITE_REJECTED:
      ESP_LOGW(TAG, "0x%04X: write of %u rejected by the boiler (reply %02X)", request.address, request.value,
               this->kw_.data()[0]);
      break;
    case KwResult::SYNC_TIMEOUT:
      // Trame jamais émise : rien n'a atteint la régulation.
      ESP_LOGW(TAG, "0x%04X: write of %u not sent: sync timeout", request.address, request.value);
      if (this->link_ok_)
        ESP_LOGW(TAG, "No sync byte (0x05) from the boiler: optical link lost?");
      this->link_ok_ = false;
      this->status_set_warning();
      break;
    default:
      // Trame émise, accusé absent ou mal cadré : l'écriture a pu être
      // appliquée ou non. Résultat INDÉTERMINÉ jusqu'à la relecture ; aucune
      // nouvelle émission.
      ESP_LOGW(TAG, "0x%04X: write of %u: %s, outcome indeterminate until read-back (not resent)", request.address,
               request.value, kw_result_to_string(result));
      break;
  }
  // Dans tous les cas : relecture immédiate et prioritaire du registre et,
  // pour un programme, du programme de l'autre circuit (jamais écrit).
  reg->expect_written_value(request.value);
  // Programme : l'autre circuit ; v0.9.1, commande Éco/Party : l'autre
  // commande du même circuit (Éco <-> Party).
  int peer = this->mode_peer_of_(request.owner);
  if (peer == WriteArbiter::NONE)
    peer = this->flag_peer_of_(request.owner);
  this->mode_write_checked_ = is_mode_register(request.address);
  this->read_back_.start(request.owner, peer);
  // Le registre écrit est relu en premier (état publié au plus tôt), puis
  // l'autre circuit.
  if (peer != WriteArbiter::NONE && !this->scheduler_.expedite(peer, now + 1))
    this->read_back_.done(peer);
  if (!this->scheduler_.expedite(request.owner, now)) {
    ESP_LOGE(TAG, "0x%04X: read-back could not be scheduled", request.address);
    this->read_back_.done(request.owner);
    if (this->read_back_.idle())
      this->writes_.complete(now);
  }
}

// ---------------------------------------------------------------------------
// Blocs horaires du bouclage (v0.5, expérimental)

bool VissLinkComponent::block_write_allowed_(int owner, uint16_t address, const uint8_t *data) const {
  // Liste blanche propre à chaque client : le bouclage ne touche que 0x2200,
  // l'éditeur que les 28 blocs des quatre programmations.
  const bool whitelisted = owner == BLOCK_OWNER_BOOST    ? is_schedule_write_whitelisted(address)
                           : owner == BLOCK_OWNER_EDITOR ? is_schedule_editor_whitelisted(address)
                                                         : false;
  const VissLinkBlockClient *client = this->block_clients_.get(owner);
  ScheduleSlots slots;
  return whitelisted &&
         decode_schedule_slots(data, SCHEDULE_BLOCK_LENGTH, &slots) == ScheduleError::NONE && client != nullptr &&
         !this->block_clients_.locked_by_other(owner, address) && client->expects_block_write(address, data);
}

bool VissLinkComponent::submit_block_write(uint16_t address, const uint8_t *data, uint32_t now, int owner) {
  if (!this->block_write_allowed_(owner, address, data)) {
    ESP_LOGW(TAG, "0x%04X: schedule block write refused (not allowed)", address);
    return false;
  }
  return this->writes_.submit_block(owner, address, data, now) == WriteArbiter::Submit::QUEUED;
}

bool VissLinkComponent::request_register_cycle(const VissLinkRegister *reg) {
  const int index = this->index_of_(reg);
  if (index < 0)
    return false;
  return this->scheduler_.request_cycle(static_cast<size_t>(index), millis());
}

bool VissLinkComponent::request_direct_read(uint16_t address, uint8_t length, int owner) {
  const bool block_ok = owner == BLOCK_OWNER_EDITOR ? is_schedule_editor_whitelisted(address)
                                                    : is_schedule_write_whitelisted(address);
  const bool allowed = length == 8 && this->block_clients_.get(owner) != nullptr &&
                       ((owner == BLOCK_OWNER_BOOST && address == VITO_CLOCK_ADDRESS) || block_ok);
  if (!allowed || this->direct_.pending)
    return false;
  this->direct_.owner = owner;
  this->direct_.pending = true;
  this->direct_.in_flight = false;
  this->direct_.address = address;
  this->direct_.length = length;
  this->direct_.failures = 0;
  this->direct_.due_ms = millis();
  return true;
}

bool VissLinkComponent::start_direct_read_(uint32_t now) {
  if (!this->direct_.pending || !time_reached(now, this->direct_.due_ms) || !this->read_back_.idle())
    return false;
  if (!this->kw_.request_read(this->direct_.address, this->direct_.length, now))
    return false;
  this->direct_.in_flight = true;
  return true;
}

void VissLinkComponent::handle_direct_result_(KwResult result, uint32_t now) {
  this->direct_.in_flight = false;
  const uint16_t address = this->direct_.address;
  if (result == KwResult::SUCCESS) {
    const uint8_t *data = this->kw_.data();
    ESP_LOGD(TAG, "0x%04X <- %s", address, hex_block(data).c_str());
    if (!this->link_ok_) {
      this->link_ok_ = true;
      this->status_clear_warning();
    }
    this->direct_.pending = false;
    if (this->block_verify_pending_) {
      this->block_verify_pending_ = false;
      this->writes_.complete(now);
    }
    if (VissLinkBlockClient *client = this->block_clients_.get(this->direct_.owner))
      client->on_direct_read(address, true, data, this->kw_.data_length(), now);
    return;
  }
  if (result == KwResult::SYNC_TIMEOUT) {
    this->link_ok_ = false;
    this->status_set_warning();
  }
  ESP_LOGW(TAG, "0x%04X: %s (attempt %u/%u)", address, kw_result_to_string(result), this->direct_.failures + 1u,
           this->scheduler_.max_retries() + 1u);
  if (this->direct_.failures < this->scheduler_.max_retries()) {
    this->direct_.failures++;
    this->direct_.due_ms = now + RegisterScheduler::RETRY_DELAY_MS;
    return;
  }
  this->direct_.pending = false;
  if (this->block_verify_pending_) {
    this->block_verify_pending_ = false;
    this->writes_.complete(now);
  }
  if (VissLinkBlockClient *client = this->block_clients_.get(this->direct_.owner))
    client->on_direct_read(address, false, nullptr, 0, now);
}

void VissLinkComponent::handle_block_write_result_(KwResult result, uint32_t now) {
  const WriteArbiter::Request request = this->writes_.current();
  const bool sent = result != KwResult::SYNC_TIMEOUT;
  this->timing_valid_ = false;
  switch (result) {
    case KwResult::SUCCESS:
      ESP_LOGI(TAG, "0x%04X <- 00 (schedule block write acknowledged; NOT a proof, read-back follows)",
               request.address);
      break;
    case KwResult::SYNC_TIMEOUT:
      ESP_LOGW(TAG, "0x%04X: schedule block write not sent: sync timeout", request.address);
      this->link_ok_ = false;
      this->status_set_warning();
      break;
    default:
      ESP_LOGW(TAG, "0x%04X: schedule block write: %s, outcome indeterminate until read-back (not resent)",
               request.address, kw_result_to_string(result));
      break;
  }
  if (sent) {
    // L'arbitre reste occupé jusqu'à la relecture du bloc par le client.
    this->block_verify_pending_ = true;
  } else {
    this->writes_.complete(now);
  }
  if (VissLinkBlockClient *client = this->block_clients_.get(request.owner))
    client->on_block_write(request.address, sent, result == KwResult::SUCCESS, now);
  if (sent && !this->direct_.pending) {
    // Le client n'a pas demandé de relecture : ne jamais bloquer l'arbitre.
    this->block_verify_pending_ = false;
    this->writes_.complete(now);
  }
}

void VissLinkComponent::read_back_done_(int index, uint32_t now) {
  if (this->writes_.active() && this->writes_.current().owner == index && this->read_back_.waiting(index))
    this->timing_.read_back = now;  // état du registre écrit publié
  if (!this->writes_.active() || !this->read_back_.done(index))
    return;
  this->timing_.done = now;
  this->log_write_timing_(this->writes_.current().address);
  this->writes_.complete(now);
  if (this->mode_write_checked_) {
    this->mode_write_checked_ = false;
    this->log_mode_divergence_after_write_();
  }
}

int VissLinkComponent::mode_peer_of_(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= this->registers_.size())
    return WriteArbiter::NONE;
  const VissLinkRegister *reg = this->registers_[index];
  if (!is_mode_register(reg->get_address()))
    return WriteArbiter::NONE;
  for (auto *other : this->mode_registers_) {
    if (other != nullptr && other != reg)
      return this->index_of_(other);
  }
  return WriteArbiter::NONE;
}

void VissLinkComponent::on_mode_state_changed() {
#ifdef USE_BINARY_SENSOR
  if (this->mode_divergence_ == nullptr || this->mode_registers_[0] == nullptr || this->mode_registers_[1] == nullptr)
    return;
  switch (mode_divergence(this->mode_registers_[0]->mode_raw(), this->mode_registers_[1]->mode_raw())) {
    case ModeDivergence::SAME:
      this->mode_divergence_->publish_state(false);
      break;
    case ModeDivergence::DIFFERENT:
      this->mode_divergence_->publish_state(true);
      break;
    case ModeDivergence::UNKNOWN:
      if (this->mode_divergence_->has_state())
        this->mode_divergence_->invalidate_state();
      break;
  }
#endif
}

void VissLinkComponent::log_mode_divergence_after_write_() {
  if (this->mode_registers_[0] == nullptr || this->mode_registers_[1] == nullptr)
    return;
  const int a = this->mode_registers_[0]->mode_raw();
  const int b = this->mode_registers_[1]->mode_raw();
  switch (mode_divergence(a, b)) {
    case ModeDivergence::DIFFERENT:
      ESP_LOGW(TAG, "Operating modes differ after the write: 0x%04X = %02X, 0x%04X = %02X (not corrected automatically)",
               this->mode_registers_[0]->get_address(), a, this->mode_registers_[1]->get_address(), b);
      break;
    case ModeDivergence::SAME:
      ESP_LOGI(TAG, "Operating modes after the write: both %02X", a);
      break;
    case ModeDivergence::UNKNOWN:
      ESP_LOGW(TAG, "Operating modes after the write: at least one could not be read");
      break;
  }
}

void VissLinkComponent::add_mode_register(VissLinkRegister *reg) {
  for (auto &slot : this->mode_registers_) {
    if (slot == nullptr) {
      slot = reg;
      return;
    }
  }
}

#ifdef USE_SELECT

void VissLinkComponent::submit_mode_write(VissLinkModeSelect *select, size_t index) {
  const uint16_t address = select->get_address();
  const int owner = this->index_of_(select);
  const uint8_t value = index < OPERATING_MODE_COUNT ? OPERATING_MODE_VALUES[index] : 0xFF;
  const char *refused = nullptr;
  bool mode_busy = false;
  for (auto *reg : this->mode_registers_) {
    if (reg != nullptr && this->writes_.has_request(this->index_of_(reg)))
      mode_busy = true;
  }
  if (!is_mode_write_whitelisted(address)) {
    refused = write_check_to_string(WriteCheck::NOT_WHITELISTED);
  } else if (index >= OPERATING_MODE_COUNT) {
    refused = write_check_to_string(WriteCheck::VALUE_NOT_ALLOWED);
  } else if (check_mode_write(address, select->get_length(), value) != WriteCheck::OK || owner == WriteArbiter::NONE) {
    refused = write_check_to_string(check_mode_write(address, select->get_length(), value));
  } else if (select->mode_raw() < 0) {
    refused = "current operating mode not read yet";
  } else if (!this->link_ok_) {
    refused = "Optolink link not synchronized";
  } else if (mode_busy || this->read_back_.waiting(owner)) {
    refused = "another operating mode write is in progress";
  }
  if (refused != nullptr) {
    ESP_LOGW(TAG, "0x%04X: operating mode %02X refused: %s; nothing sent", address, value, refused);
    select->restore_read_state();
    return;
  }
  if (select->mode_raw() == value) {
    ESP_LOGI(TAG, "0x%04X: operating mode already %02X, nothing written", address, value);
    select->restore_read_state();
    return;
  }
  if (this->writes_.submit(owner, address, value, millis()) == WriteArbiter::Submit::FULL) {
    ESP_LOGW(TAG, "0x%04X: operating mode %02X refused: too many pending writes", address, value);
    select->restore_read_state();
    return;
  }
  ESP_LOGI(TAG, "0x%04X: operating mode %02X queued (experimental)", address, value);
  // L'entité continue d'afficher le programme LU, jamais la commande en
  // attente : il ne change qu'après la relecture.
  select->restore_read_state();
}
#endif

// ---------------------------------------------------------------------------
// Consignes : ECS (v0.6) et chauffage A1/M2 (v0.7), expérimental

#ifdef USE_NUMBER
void VissLinkComponent::add_setpoint_number(VissLinkNumber *number) {
  uint8_t lo, hi;
  if (!setpoint_write_limits(number->get_address(), &lo, &hi))
    return;
  this->setpoints_.emplace_back(number, number->get_address());
}

void VissLinkComponent::submit_setpoint_(SetpointChannel &ch, VissLinkNumber *number, float value) {
  const uint32_t now = millis();
  const uint16_t address = ch.writer.address();
  DhwContext ctx;
  ctx.link_ok = this->link_ok_;
  ctx.has_value = number->has_read_value() && ch.owner != WriteArbiter::NONE;
  ctx.config_min = number->traits.get_min_value();
  ctx.config_max = number->traits.get_max_value();
  if (const HeatingSetpoint *h = find_heating_setpoint(address)) {
    // Confort >= réduite : dernière valeur lue de l'autre consigne du circuit.
    for (auto &other : this->setpoints_) {
      if (other.writer.address() == h->peer && other.reg->setpoint_value() >= 0) {
        ctx.peer_known = true;
        ctx.peer_value = static_cast<uint8_t>(other.reg->setpoint_value());
      }
    }
  }
  {
    // Garde commune : une modification de consigne à la fois, espacées.
    std::vector<const SetpointWriter *> writers;
    writers.reserve(this->setpoints_.size());
    for (const auto &other : this->setpoints_)
      writers.push_back(&other.writer);
    setpoint_gate(writers, &ch.writer, now, &ctx);
  }
  ESP_LOGI(TAG, "0x%04X: setpoint change requested: %.1f (current %.0f)", address, value,
           number->has_read_value() ? number->state : NAN);
  switch (ch.writer.request(value, ctx, now)) {
    case DhwAction::PRE_READ:
      ESP_LOGI(TAG, "0x%04X: reading the current value first", address);
      // Relecture prioritaire sur les lectures ordinaires, mais jamais
      // intercalée entre une écriture et sa vérification (URGENT_REFRESH) ;
      // si le registre est déjà en cours de lecture, c'est elle qui sert.
      if (!this->scheduler_.expedite_block(ch.owner, 0, now))
        ESP_LOGD(TAG, "0x%04X: read already in progress, used as the current value", address);
      break;
    default:
      ESP_LOGW(TAG, "0x%04X: setpoint %.1f refused: %s; nothing sent", address, value, ch.writer.reason());
      break;
  }
  // L'entité affiche toujours la valeur LUE, jamais la demande.
  number->restore_read_state();
  this->setpoint_publish_(ch);
}
#endif

void VissLinkComponent::setpoint_on_read_(SetpointChannel &ch, bool ok, uint8_t value, bool verify, uint32_t now) {
  const uint16_t address = ch.writer.address();
  const uint32_t panel_changes = ch.writer.panel_changes();
  switch (ch.writer.on_read(ok, value, verify, now)) {
    case DhwAction::SUBMIT: {
      const uint8_t target = ch.writer.target();
      ESP_LOGI(TAG, "0x%04X: setpoint: current %u, requested %u: write queued", address, value, target);
      if (this->writes_.submit(ch.owner, address, target, now) == WriteArbiter::Submit::FULL) {
        ESP_LOGW(TAG, "0x%04X: setpoint write refused: too many pending writes", address);
        ch.writer.on_dropped(VL_TEXT("trop d'écritures en attente"));
      } else if (this->deferred_setpoints_().count != 0) {
        ESP_LOGI(TAG, "0x%04X: setpoint write waits for the circulation schedule restore", address);
      }
      break;
    }
    case DhwAction::UNCHANGED:
      ESP_LOGI(TAG, "0x%04X: setpoint already %u, nothing written", address, value);
      break;
    case DhwAction::REFUSED:
      ESP_LOGW(TAG, "0x%04X: setpoint change dropped: current value unreadable, nothing written", address);
      break;
    default:
      if (verify && ch.writer.last_outcome() == DhwOutcome::CONFIRMED)
        ESP_LOGI(TAG, "0x%04X: setpoint %u confirmed %" PRIu32 " ms after the request", address, value,
                 ch.writer.last_duration_ms());
      if (ch.writer.panel_changes() != panel_changes)
        ESP_LOGI(TAG, "0x%04X: setpoint changed outside VissLink (panel/Vitotrol?): now %u", address, value);
      break;
  }
}

void VissLinkComponent::setpoint_publish_(SetpointChannel &ch) {
  if (ch.writer.version() == ch.published)
    return;
  ch.published = ch.writer.version();
  // État de la commande : journal seulement (aucune entité Home Assistant).
  ESP_LOGI(TAG, "0x%04X: setpoint: %s", ch.writer.address(), ch.writer.status().c_str());
}

VissLinkComponent::SetpointChannel *VissLinkComponent::setpoint_channel_(int index) {
  if (index == WriteArbiter::NONE)
    return nullptr;
  for (auto &ch : this->setpoints_) {
    if (ch.owner == index)
      return &ch;
  }
  return nullptr;
}

bool VissLinkComponent::setpoint_pending(const VissLinkRegister *reg, uint8_t *target) const {
  for (const auto &ch : this->setpoints_) {
    if (ch.reg == reg && ch.writer.busy()) {
      *target = ch.writer.target();
      return true;
    }
  }
  return false;
}

OwnerSet VissLinkComponent::deferred_setpoints_() const {
  OwnerSet set;
  if (!this->block_clients_.restore_priority())
    return set;
  for (const auto &ch : this->setpoints_) {
    if (ch.owner != WriteArbiter::NONE)
      set.add(ch.owner);
  }
  for (const auto &ch : this->flags_) {
    if (ch.owner != WriteArbiter::NONE)
      set.add(ch.owner);
  }
  return set;
}

// ---------------------------------------------------------------------------
// Commandes Éco / Party A1 et M2 (v0.9.1, expérimental)

#ifdef USE_SWITCH
void VissLinkComponent::add_flag_switch(VissLinkFlagSwitch *sw) {
  if (!is_eco_party_flag_whitelisted(sw->get_address()))
    return;
  this->flags_.emplace_back(sw, sw->get_address());
}

void VissLinkComponent::submit_flag_write(VissLinkFlagSwitch *sw, bool on) {
  const uint32_t now = millis();
  FlagChannel *ch = this->flag_channel_(this->index_of_(sw));
  if (ch == nullptr) {
    ESP_LOGW(TAG, "0x%04X: Eco/Party command refused: not a managed Eco/Party register; nothing sent",
             sw->get_address());
    sw->restore_read_state();
    return;
  }
  FlagContext ctx;
  ctx.link_ok = this->link_ok_;
  ctx.has_value = sw->flag_raw() >= 0;
  {
    std::vector<const FlagWriter *> writers;
    writers.reserve(this->flags_.size());
    for (const auto &other : this->flags_)
      writers.push_back(&other.writer);
    flag_gate(writers, &ch->writer, now, &ctx);
  }
  const uint16_t address = ch->writer.address();
  ESP_LOGI(TAG, "0x%04X: Eco/Party command requested: %s (current %d)", address, on ? "ON" : "OFF", sw->flag_raw());
  if (ch->writer.request(on, ctx, now) == DhwAction::PRE_READ) {
    ESP_LOGI(TAG, "0x%04X: reading the current state first", address);
    if (!this->scheduler_.expedite_block(ch->owner, 0, now))
      ESP_LOGD(TAG, "0x%04X: read already in progress, used as the current state", address);
  } else {
    ESP_LOGW(TAG, "0x%04X: Eco/Party command %s refused: %s; nothing sent", address, on ? "ON" : "OFF",
             ch->writer.reason());
  }
  // L'entité affiche toujours l'état LU, jamais la commande.
  sw->restore_read_state();
  this->flag_publish_(*ch);
}
#endif

void VissLinkComponent::flag_on_read_(FlagChannel &ch, bool ok, uint8_t value, bool verify, bool after_peer_write,
                                      uint32_t now) {
  const uint16_t address = ch.writer.address();
  const uint32_t panel_changes = ch.writer.panel_changes();
  const uint32_t peer_changes = ch.writer.peer_changes();
  switch (ch.writer.on_read(ok, value, verify, now, after_peer_write)) {
    case DhwAction::SUBMIT: {
      const uint8_t target = ch.writer.target();
      ESP_LOGI(TAG, "0x%04X: Eco/Party: current %02X, requested %02X: write queued (experimental)", address, value,
               target);
      if (this->writes_.submit(ch.owner, address, target, now) == WriteArbiter::Submit::FULL) {
        ESP_LOGW(TAG, "0x%04X: Eco/Party write refused: too many pending writes", address);
        ch.writer.on_dropped(VL_TEXT("trop d'écritures en attente"));
      } else if (this->deferred_setpoints_().count != 0) {
        ESP_LOGI(TAG, "0x%04X: Eco/Party write waits for the circulation schedule restore", address);
      }
      break;
    }
    case DhwAction::UNCHANGED:
      ESP_LOGI(TAG, "0x%04X: Eco/Party already %02X, nothing written", address, value);
      break;
    case DhwAction::REFUSED:
      ESP_LOGW(TAG, "0x%04X: Eco/Party command dropped: %s, nothing written", address,
               ok ? "unexpected current value" : "current state unreadable");
      break;
    default:
      if (verify && ch.writer.last_outcome() == DhwOutcome::CONFIRMED)
        ESP_LOGI(TAG, "0x%04X: Eco/Party %02X confirmed %" PRIu32 " ms after the request", address, value,
                 ch.writer.last_duration_ms());
      if (ch.writer.panel_changes() != panel_changes)
        ESP_LOGI(TAG, "0x%04X: Eco/Party changed outside VissLink (automatic end, panel or Vitotrol?): now %02X",
                 address, value);
      if (ch.writer.peer_changes() != peer_changes)
        ESP_LOGI(TAG, "0x%04X: Eco/Party changed by the boiler after the other command of the circuit: now %02X",
                 address, value);
      break;
  }
}

void VissLinkComponent::flag_publish_(FlagChannel &ch) {
  if (ch.writer.version() == ch.published)
    return;
  ch.published = ch.writer.version();
  // État de la commande : journal seulement (aucune entité Home Assistant).
  ESP_LOGI(TAG, "0x%04X: Eco/Party: %s", ch.writer.address(), ch.writer.status().c_str());
}

VissLinkComponent::FlagChannel *VissLinkComponent::flag_channel_(int index) {
  if (index == WriteArbiter::NONE)
    return nullptr;
  for (auto &ch : this->flags_) {
    if (ch.owner == index)
      return &ch;
  }
  return nullptr;
}

int VissLinkComponent::flag_peer_of_(int index) const {
  for (const auto &ch : this->flags_) {
    if (ch.owner != index || index == WriteArbiter::NONE)
      continue;
    for (const auto &other : this->flags_) {
      if (other.writer.address() == ch.writer.peer_address())
        return other.owner;
    }
  }
  return WriteArbiter::NONE;
}

void VissLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink (Optolink KW):");
  ESP_LOGCONFIG(TAG, "  Writes: whitelisted registers only (setpoints, operating modes, Eco/Party, schedules)");
  ESP_LOGCONFIG(TAG, "  DHW setpoint internal limits: %u..%u", DHW_WRITE_MIN, DHW_WRITE_MAX);
  for (auto *reg : this->registers_)
    reg->dump_register_config();
}

// ---------------------------------------------------------------------------
// Climate du circuit plancher M2 (v0.7, expérimental)

#if defined(USE_CLIMATE) && defined(USE_NUMBER) && defined(USE_SELECT)
static const char *const CLIMATE_TAG = "visslink.climate";

void VissLinkCircuitClimate::setup() {
  // Aucune commande au démarrage : seul l'état lu est publié, dès que la
  // consigne et le programme sont connus.
  this->set_interval("refresh", 1000, [this]() { this->refresh_(false); });
}

int VissLinkCircuitClimate::mode_raw_() const {
  return this->mode_select_ != nullptr ? this->mode_select_->mode_raw() : -1;
}

int VissLinkCircuitClimate::fresh_mode_raw_(uint32_t now) const {
  if (this->mode_select_ == nullptr || !this->mode_select_->read_fresh(now))
    return -1;
  return this->mode_select_->mode_raw();
}

#ifdef USE_BINARY_SENSOR
int VissLinkCircuitClimate::fresh_binary_(const VissLinkBinarySensor *sensor, uint32_t now) {
  if (sensor == nullptr || !sensor->has_state() || !sensor->read_fresh(now))
    return -1;
  return sensor->state ? 1 : 0;
}
#endif

void VissLinkCircuitClimate::publish_status_() {
  // v0.7.3 : publié dès le démarrage (« Inconnu »), puis à chaque changement.
  if (this->status_published_ && this->published_status_ == this->state_.status)
    return;
  this->status_published_ = true;
  this->published_status_ = this->state_.status;
  ESP_LOGD(CLIMATE_TAG, "M2 state: %s", circuit_status_text(this->state_.status));
#ifdef USE_TEXT_SENSOR
  if (this->status_sensor_ != nullptr)
    this->status_sensor_->publish_state(circuit_status_text(this->state_.status));
#endif
}

void VissLinkCircuitClimate::refresh_(bool force) {
  const uint32_t now = millis();
  float room = NAN;
#ifdef USE_SENSOR
  if (this->room_ != nullptr && this->room_->has_state())
    room = this->room_->state;
#endif
  int pump = -1, burner = -1;
#ifdef USE_BINARY_SENSOR
  pump = fresh_binary_(this->pump_, now);
  burner = fresh_binary_(this->burner_, now);
#endif
  // Données périmées = inconnues : l'état et l'action passent à Inconnu /
  // idle ; le mode affiché garde la dernière valeur lue (jamais deviné).
  const CircuitClimateState next = circuit_climate_state(this->state_, room, this->comfort_->setpoint_value(),
                                                         this->fresh_mode_raw_(now), pump, burner);
  if (!next.ready) {
    // Climate pas encore publié : seul l'état texte l'est.
    this->state_.status = next.status;
    this->publish_status_();
    return;
  }
  if (!force && next.same_as(this->state_))
    return;
  this->state_ = next;
  this->publish_status_();
  this->current_temperature = next.current;
  this->target_temperature = next.target;
  this->mode = next.hvac == CircuitHvac::HEAT ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_OFF;
  switch (next.action) {
    case CircuitAction::HEATING:
      this->action = climate::CLIMATE_ACTION_HEATING;
      break;
    case CircuitAction::IDLE:
      this->action = climate::CLIMATE_ACTION_IDLE;
      break;
    case CircuitAction::OFF:
      this->action = climate::CLIMATE_ACTION_OFF;
      break;
  }
  this->publish_state();
}

climate::ClimateTraits VissLinkCircuitClimate::traits() {
  climate::ClimateTraits traits;
  uint32_t flags = 0;
#ifdef USE_SENSOR
  if (this->room_ != nullptr)
    flags |= climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE;
#endif
#ifdef USE_BINARY_SENSOR
  if (this->pump_ != nullptr)
    flags |= climate::CLIMATE_SUPPORTS_ACTION;
#endif
  traits.add_feature_flags(flags);
  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  // Bornes : number de la consigne confort ET limites internes d'écriture.
  const HeatingSetpoint *h = find_heating_setpoint(M2_COMFORT_SETPOINT);
  float lo = this->comfort_->traits.get_min_value(), hi = this->comfort_->traits.get_max_value();
  if (h != nullptr) {
    lo = std::max(lo, static_cast<float>(h->min));
    hi = std::min(hi, static_cast<float>(h->max));
  }
  traits.set_visual_min_temperature(lo);
  traits.set_visual_max_temperature(hi);
  traits.set_visual_target_temperature_step(1.0f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void VissLinkCircuitClimate::control(const climate::ClimateCall &call) {
  // Aucun chemin propre : la consigne passe par la transaction de 0x3306,
  // le mode par le sélecteur de M2. L'état publié reste lu.
  if (call.get_mode().has_value()) {
    const climate::ClimateMode requested = *call.get_mode();
    if (requested != climate::CLIMATE_MODE_HEAT && requested != climate::CLIMATE_MODE_OFF) {
      ESP_LOGW(CLIMATE_TAG, "Mode %u refused: only HEAT (03) and OFF (00, DHW only)", static_cast<unsigned>(requested));
    } else {
      const bool heat = requested == climate::CLIMATE_MODE_HEAT;
      const CircuitHvac current = circuit_hvac_from_mode(this->mode_raw_());
      if (!hvac_command_needed(heat ? CircuitHvac::HEAT : CircuitHvac::OFF, current)) {
        ESP_LOGD(CLIMATE_TAG, "Mode %s already read, nothing sent", heat ? "HEAT" : "OFF");
      } else {
        ESP_LOGI(CLIMATE_TAG, "Mode %s requested -> operating mode %02X", heat ? "HEAT" : "OFF",
                 mode_value_for_hvac(heat));
        this->parent_->submit_mode_write(this->mode_select_, mode_option_for_hvac(heat));
      }
    }
  }
  if (call.get_target_temperature().has_value()) {
    const float value = *call.get_target_temperature();
    uint8_t pending_target = 0;
    const bool pending = this->parent_->setpoint_pending(this->comfort_, &pending_target);
    switch (classify_temperature_command(value, this->comfort_->setpoint_value(), pending, pending_target)) {
      case TemperatureCommand::FORWARD:
        ESP_LOGI(CLIMATE_TAG, "Target %.1f requested -> comfort setpoint 0x%04X", value, M2_COMFORT_SETPOINT);
        this->parent_->submit_write(this->comfort_, value);
        break;
      case TemperatureCommand::UNCHANGED:
        ESP_LOGD(CLIMATE_TAG, "Target %.1f equals the read comfort setpoint, nothing sent", value);
        break;
      case TemperatureCommand::PENDING:
        ESP_LOGD(CLIMATE_TAG, "Target %.1f already in progress, not repeated", value);
        break;
      case TemperatureCommand::INVALID:
        ESP_LOGW(CLIMATE_TAG, "Target refused: not a finite number");
        break;
    }
  }
  // Home Assistant revient à l'état LU (jamais la demande).
  this->refresh_(true);
}

void VissLinkCircuitClimate::dump_config() {
  LOG_CLIMATE("", "VissLink circuit climate (M2, experimental)", this);
  ESP_LOGCONFIG(CLIMATE_TAG, "  Target: comfort setpoint 0x%04X; mode: 0x%04X (HEAT=03, OFF=00 DHW only)",
                this->comfort_->get_address(),
                this->mode_select_ != nullptr ? this->mode_select_->get_address() : 0);
#ifdef USE_BINARY_SENSOR
  ESP_LOGCONFIG(CLIMATE_TAG, "  State: pump 0x3906 %s, burner 0x0842 %s (stale after 3 intervals + 30 s)",
                this->pump_ != nullptr ? "yes" : "no", this->burner_ != nullptr ? "yes" : "no");
  if (this->pump_ != nullptr && this->burner_ == nullptr)
    ESP_LOGCONFIG(CLIMATE_TAG, "  No burner_id: heating vs inertia cannot be told apart, action never 'heating'");
#endif
}
#endif

}  // namespace visslink
}  // namespace esphome
