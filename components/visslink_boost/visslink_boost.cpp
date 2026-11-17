#include "visslink_boost.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace visslink_boost {

static const char *const TAG = "visslink.boost";

void BoostButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->press(this->kind_);
}

void VissLinkBoost::setup() {
  // Sauvegarde en FLASH (pas en RTC) : elle survit à une coupure secteur.
  this->pref_ = global_preferences->make_preference<visslink::BoostRecord>(fnv1_hash("visslink_boost_v1"), true);
  visslink::BoostRecord record;
  const bool loaded = this->pref_.load(&record) && visslink::boost_record_valid(record);
  this->sequencer_.configure(this->config_);
  this->sequencer_.boot(loaded ? &record : nullptr);
  if (loaded) {
    // Aucune écriture automatique au démarrage (cf. docs/bouclage-ecs-v0.5.md).
    ESP_LOGW(TAG, "%s", this->sequencer_.status().c_str());
  }
  if (this->parent_ != nullptr)
    this->parent_->set_block_client(this);
  if (this->duration_ != nullptr)
    this->duration_->publish_state(visslink::BOOST_DEFAULT_DURATION_INDEX);  // 10 min après chaque démarrage
  this->publish_(millis());
}

void VissLinkBoost::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink circulation boost (experimental):");
  ESP_LOGCONFIG(TAG, "  Schedule writes: %s",
                this->parent_ != nullptr && this->parent_->schedule_writes_enabled() ? "ENABLED" : "disabled");
  ESP_LOGCONFIG(TAG, "  Validated days mask: 0x%02X (bit 0 = Monday)", this->config_.validated_days_mask);
  ESP_LOGCONFIG(TAG, "  State: %s", visslink::boost_state_to_string(this->sequencer_.state()));
}

void VissLinkBoost::loop() {
  const uint32_t now = millis();
  this->dispatch_(now);
  this->refresh_display_(now);
  this->publish_(now);
}

void VissLinkBoost::refresh_display_(uint32_t now) {
  if (this->parent_ == nullptr)
    return;
  // Affichage seulement : la relecture de vérification a déjà décidé.
  switch (this->refresh_.poll(this->sequencer_, this->port_, now)) {
    case visslink::BoostRefreshRelay::Event::SUBMITTED:
      ESP_LOGD(TAG, "0x%04X: schedule display refresh requested", this->refresh_.address());
      break;
    case visslink::BoostRefreshRelay::Event::NOT_DECLARED:
      ESP_LOGD(TAG, "0x%04X: no schedule entity for this block, display not refreshed", this->refresh_.address());
      break;
    case visslink::BoostRefreshRelay::Event::CANCELLED:
      ESP_LOGD(TAG, "0x%04X: display refresh cancelled (new operation)", this->refresh_.address());
      break;
    case visslink::BoostRefreshRelay::Event::GAVE_UP:
      ESP_LOGW(TAG, "0x%04X: display refresh not possible, next periodic read will update it",
               this->refresh_.address());
      break;
    case visslink::BoostRefreshRelay::Event::NONE:
      break;
  }
}

void VissLinkBoost::refuse_(const std::string &why, uint32_t now) {
  ESP_LOGW(TAG, "Request refused: %s", why.c_str());
  this->refusal_ = VL_TEXT("Refusé : ") + why;
  this->refusal_until_ms_ = now + 30000;
  this->refusal_base_ = this->sequencer_.status();
  this->publish_(now);
}

void VissLinkBoost::press(BoostButton::Kind kind) {
  const uint32_t now = millis();
  this->refusal_.clear();
  if (kind == BoostButton::Kind::FORGET) {
    if (!this->sequencer_.forget())
      this->refuse_(VL_TEXT("aucune sauvegarde à oublier"), now);
    return;
  }
  // Démarrer et restaurer écrivent dans la régulation : mêmes autorisations.
  if (this->parent_ == nullptr || !this->parent_->schedule_writes_enabled()) {
    this->refuse_(VL_TEXT("écritures de programmation désactivées (enable_writes / enable_schedule_writes)"), now);
    return;
  }
  if (kind == BoostButton::Kind::RESTORE) {
    if (!this->sequencer_.request_restore(now))
      this->refuse_(VL_TEXT("aucune plage temporaire ni récupération en cours"), now);
    return;
  }
  if (!this->parent_->link_ok()) {
    this->refuse_(VL_TEXT("liaison Optolink non synchronisée"), now);
    return;
  }
  if (!this->parent_->writes_idle() && this->sequencer_.idle()) {
    this->refuse_(VL_TEXT("une autre commande est en cours"), now);
    return;
  }
  // v0.8 : verrou mutuel avec l'éditeur des programmations.
  if (this->sequencer_.idle() && this->parent_->block_client_busy_other(visslink::BLOCK_OWNER_BOOST)) {
    this->refuse_(VL_TEXT("modification de programmation en cours"), now);
    return;
  }
  visslink::HaTime ha;
  if (this->clock_ != nullptr) {
    const ESPTime t = this->clock_->now();
    const int weekday = visslink::weekday_from_esphome(t.day_of_week);
    if (t.is_valid() && weekday >= 0) {
      ha.valid = true;
      ha.weekday = static_cast<uint8_t>(weekday);
      ha.minute_of_day = static_cast<uint16_t>(t.hour * 60 + t.minute);
    }
  }
  const auto index = this->duration_ != nullptr ? this->duration_->active_index() : optional<size_t>();
  const uint16_t duration = index.has_value() && index.value() < visslink::BOOST_DURATION_COUNT
                                ? visslink::BOOST_DURATIONS[index.value()]
                                : 0;
  switch (this->sequencer_.start(duration, ha, now)) {
    case visslink::BoostStart::ACCEPTED:
      ESP_LOGI(TAG, "Circulation boost requested (%u min)", duration);
      break;
    case visslink::BoostStart::BUSY:
      this->refuse_(VL_TEXT("opération déjà en cours (minuterie non prolongée)"), now);
      break;
    case visslink::BoostStart::RECOVERY_PENDING:
      this->refuse_(VL_TEXT("récupération nécessaire d'abord"), now);
      break;
    case visslink::BoostStart::BAD_DURATION:
      this->refuse_(VL_TEXT("durée invalide"), now);
      break;
    case visslink::BoostStart::RATE_LIMITED:
      this->refuse_(VL_TEXT("trop tôt après le déclenchement précédent"), now);
      break;
  }
  this->dispatch_(now);
}

bool VissLinkBoost::persist_(const visslink::BoostRecord &record) {
  // Écriture, synchronisation immédiate de la flash, puis relecture.
  if (!this->pref_.save(&record) || !global_preferences->sync())
    return false;
  visslink::BoostRecord check;
  return this->pref_.load(&check) && std::memcmp(&check, &record, sizeof(record)) == 0;
}

void VissLinkBoost::dispatch_(uint32_t now) {
  for (int guard = 0; guard < 8; guard++) {
    if (!this->has_pending_) {
      this->pending_ = this->sequencer_.poll(now);
      if (this->pending_.type == visslink::BoostActionType::NONE)
        return;
      this->has_pending_ = true;
    }
    const BoostAction a = this->pending_;
    switch (a.type) {
      case visslink::BoostActionType::READ:
        if (!this->parent_->request_direct_read(a.address, a.length))
          return;  // ligne occupée : nouvel essai au prochain passage
        this->has_pending_ = false;
        return;  // résultat via on_direct_read
      case visslink::BoostActionType::WRITE:
        this->has_pending_ = false;
        if (!this->parent_->submit_block_write(a.address, a.data, now))
          this->sequencer_.on_write(false, false, now);  // rien d'émis
        else
          return;  // résultat via on_block_write
        break;
      case visslink::BoostActionType::SAVE: {
        this->has_pending_ = false;
        const bool ok = this->persist_(a.record);
        ESP_LOGI(TAG, "Backup of 0x%04X (%s): %s", a.record.address, visslink::hex_block(a.record.original).c_str(),
                 ok ? "saved to flash" : "FAILED");
        this->sequencer_.on_saved(ok, now);
        break;
      }
      case visslink::BoostActionType::CLEAR: {
        this->has_pending_ = false;
        const bool ok = this->persist_(visslink::BoostRecord());
        ESP_LOGI(TAG, "Backup cleared: %s", ok ? "ok" : "FAILED");
        this->sequencer_.on_saved(ok, now);
        break;
      }
      default:
        this->has_pending_ = false;
        return;
    }
  }
}

void VissLinkBoost::on_direct_read(uint16_t /*address*/, bool ok, const uint8_t *data, uint8_t length,
                                   uint32_t now) {
  this->sequencer_.on_read(ok, data, length, now);
  this->dispatch_(now);
}

void VissLinkBoost::on_block_write(uint16_t /*address*/, bool sent, bool acked, uint32_t now) {
  this->sequencer_.on_write(sent, acked, now);
  this->dispatch_(now);  // relecture demandée aussitôt : l'arbitre reste occupé jusque-là
}

void VissLinkBoost::publish_(uint32_t now) {
  if (!this->refusal_.empty() &&
      (visslink::time_reached(now, this->refusal_until_ms_) || this->sequencer_.status() != this->refusal_base_))
    this->refusal_.clear();
  const std::string status = this->refusal_.empty() ? this->sequencer_.status() : this->refusal_;
  if (status != this->last_status_) {
    this->last_status_ = status;
    ESP_LOGI(TAG, "%s [%s]", status.c_str(), visslink::boost_state_to_string(this->sequencer_.state()));
    if (this->status_sensor_ != nullptr) {
      std::string text = status;
      visslink::truncate_state(text);
      this->status_sensor_->publish_state(text);
    }
  }
  if (this->active_sensor_ != nullptr &&
      (!this->active_sensor_->has_state() || this->active_sensor_->state != this->sequencer_.active()))
    this->active_sensor_->publish_state(this->sequencer_.active());
  // v0.5.2 : « restauration en cours » (attente normale du bus, fenêtre
  // automatique) distincte de l'alerte, levée seulement en récupération.
  if (this->restoring_sensor_ != nullptr &&
      (!this->restoring_sensor_->has_state() || this->restoring_sensor_->state != this->sequencer_.restoring()))
    this->restoring_sensor_->publish_state(this->sequencer_.restoring());
  // v0.7.1 : plage inscrite et temps restant, distincts de la pompe, lue à
  // part (0x0846 : entité visslink séparée, jamais commandée ici).
  if (this->range_sensor_ != nullptr) {
    const std::string range = this->sequencer_.range_text();
    if (range != this->last_range_ || !this->range_sensor_->has_state()) {
      this->last_range_ = range;
      this->range_sensor_->publish_state(range);
    }
  }
  if (this->remaining_sensor_ != nullptr) {
    // Minutes entières arrondies au-dessus : 10 juste après la confirmation,
    // 1 pendant la dernière minute, 0 à la restauration.
    const int32_t minutes = static_cast<int32_t>((this->sequencer_.remaining_s(now) + 59u) / 60u);
    if (minutes != this->last_remaining_min_) {
      this->last_remaining_min_ = minutes;
      this->remaining_sensor_->publish_state(minutes);
    }
  }
  const bool alert = this->sequencer_.restore_alert();
  if (this->restore_sensor_ != nullptr && (!this->restore_sensor_->has_state() || this->restore_sensor_->state != alert))
    this->restore_sensor_->publish_state(alert);
}

}  // namespace visslink_boost
}  // namespace esphome
