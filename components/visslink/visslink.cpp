#include "visslink.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

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
  if (result != KwResult::NONE) {
    if (this->write_in_flight_) {
      this->handle_write_result_(result, now);
    } else {
      this->handle_result_(result, now);
    }
  }

  if (this->kw_.is_idle() && this->scheduler_.current() == RegisterScheduler::NONE) {
    this->drop_expired_writes_(now);
    // Une écriture en attente passe avant les lectures (commande manuelle),
    // dans les limites de WriteArbiter : une à la fois, espacées.
    this->start_write_(now);
  }

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
    reg->publish_unknown(block);
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
  const uint16_t address = number->get_address();
  uint8_t byte = 0;
  const char *refused = nullptr;
  const WriteCheck check = check_write_value(address, number->get_length(), value, number->traits.get_min_value(),
                                             number->traits.get_max_value(), &byte);
  const int owner = this->index_of_(number);
  if (!this->enable_writes_) {
    refused = "writes are disabled (enable_writes: false)";
  } else if (check != WriteCheck::OK) {
    refused = write_check_to_string(check);
  } else if (owner == WriteArbiter::NONE) {
    refused = "unknown register";
  } else if (!number->has_read_value()) {
    refused = "current value not read yet";
  } else if (!this->link_ok_) {
    refused = "Optolink link not synchronized";
  }
  if (refused != nullptr) {
    ESP_LOGW(TAG, "0x%04X: write of %.1f refused: %s; nothing sent", address, value, refused);
    number->restore_read_state();
    return;
  }
  const bool busy = this->writes_.has_request(owner);
  if (!busy && byte == static_cast<uint8_t>(number->state)) {
    ESP_LOGI(TAG, "0x%04X: already %u, nothing written", address, byte);
    number->restore_read_state();
    return;
  }
  switch (this->writes_.submit(owner, address, byte, millis())) {
    case WriteArbiter::Submit::QUEUED:
      ESP_LOGI(TAG, "0x%04X: write of %u queued", address, byte);
      break;
    case WriteArbiter::Submit::REPLACED:
      ESP_LOGI(TAG, "0x%04X: write of %u replaces the pending request", address, byte);
      break;
    case WriteArbiter::Submit::FULL:
      ESP_LOGW(TAG, "0x%04X: write of %u refused: too many pending writes", address, byte);
      number->restore_read_state();
      break;
  }
}
#endif

void VissLinkComponent::drop_expired_writes_(uint32_t now) {
  WriteArbiter::Request request;
  while (this->writes_.pop_expired(now, &request)) {
    ESP_LOGW(TAG, "0x%04X: write of %u dropped: not sent within %" PRIu32 " s", request.address, request.value,
             WriteArbiter::REQUEST_EXPIRY_MS / 1000);
    this->registers_[request.owner]->restore_read_state();
  }
}

bool VissLinkComponent::start_write_(uint32_t now) {
  WriteArbiter::Request request;
  if (!this->enable_writes_ || !this->writes_.next(now, this->link_ok_, &request))
    return false;
  // Dernier contrôle avant émission : la demande a été validée au dépôt,
  // mais rien ne part sans un nouveau contrôle complet (adresse de la liste
  // blanche, longueur, bornes configurées et documentées).
  // Programme : les autorisations de configuration sont revérifiées ici
  // (M2 : enable_m2_mode_write).
  VissLinkRegister *reg = this->registers_[request.owner];
  const bool mode_ok = !is_mode_register(request.address) ||
                       mode_write_permitted(request.address, this->enable_writes_, this->enable_mode_writes_,
                                            this->enable_m2_mode_write_) == ModePermission::OK;
  if (reg->get_address() != request.address || !is_write_whitelisted(request.address) || !mode_ok ||
      !reg->write_allowed(request.value) || !this->kw_.request_write(request.address, &request.value, 1, now)) {
    ESP_LOGE(TAG, "0x%04X: write of %u refused at send time", request.address, request.value);
    this->writes_.complete(now);
    reg->restore_read_state();
    return false;
  }
  ESP_LOGI(TAG, "0x%04X -> %02X (write)", request.address, request.value);
  this->write_in_flight_ = true;
  return true;
}

void VissLinkComponent::handle_write_result_(KwResult result, uint32_t now) {
  this->write_in_flight_ = false;
  const WriteArbiter::Request &request = this->writes_.current();
  VissLinkRegister *reg = this->registers_[request.owner];
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
  const int peer = this->mode_peer_of_(request.owner);
  this->mode_write_checked_ = is_mode_register(request.address);
  this->read_back_.start(request.owner, peer);
  if (peer != WriteArbiter::NONE && !this->scheduler_.expedite(peer, now))
    this->read_back_.done(peer);
  if (!this->scheduler_.expedite(request.owner, now)) {
    ESP_LOGE(TAG, "0x%04X: read-back could not be scheduled", request.address);
    this->read_back_.done(request.owner);
    if (this->read_back_.idle())
      this->writes_.complete(now);
  }
}

void VissLinkComponent::read_back_done_(int index, uint32_t now) {
  if (!this->writes_.active() || !this->read_back_.done(index))
    return;
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
  const ModePermission permission =
      mode_write_permitted(address, this->enable_writes_, this->enable_mode_writes_, this->enable_m2_mode_write_);
  if (permission != ModePermission::OK) {
    refused = mode_permission_to_string(permission);
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

void VissLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink (Optolink KW):");
  ESP_LOGCONFIG(TAG, "  Writes: %s", this->enable_writes_ ? "ENABLED (whitelisted setpoints only)" : "disabled (read-only)");
  ESP_LOGCONFIG(TAG, "  Operating mode writes: %s",
                this->enable_writes_ && this->enable_mode_writes_ ? "ENABLED (experimental)" : "disabled");
  ESP_LOGCONFIG(TAG, "  M2 operating mode write (0x3301): %s",
                mode_write_permitted(M2_MODE_REGISTER, this->enable_writes_, this->enable_mode_writes_,
                                     this->enable_m2_mode_write_) == ModePermission::OK
                    ? "ENABLED (experimental, undocumented)"
                    : "disabled");
  for (auto *reg : this->registers_)
    reg->dump_register_config();
}

}  // namespace visslink
}  // namespace esphome
