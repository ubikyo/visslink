#include "visslink_schedule_editor.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace visslink_schedule_editor {

static const char *const TAG = "visslink.editor";

using visslink::EDITOR_CIRCUIT_ADDRESSES;
using visslink::EDITOR_CIRCUIT_COUNT;
using visslink::EDITOR_CIRCUIT_KEYS;
using visslink::EDITOR_CIRCUIT_LABELS;
using visslink::EDITOR_NO_CIRCUIT;
using visslink::EditorAction;
using visslink::EditorActionType;
using visslink::SCHEDULE_DAY_LABELS;
using visslink::SCHEDULE_DAYS;

void VissLinkScheduleEditor::add_schedule(VissLinkSchedule *schedule) {
  const uint8_t circuit = visslink::editor_circuit_of_monday(schedule->get_address());
  if (circuit != EDITOR_NO_CIRCUIT)
    this->schedules_[circuit] = schedule;
}

void VissLinkScheduleEditor::setup() {
  for (auto *schedule : this->schedules_) {
    if (schedule != nullptr)
      schedule->enable_raw_cache();  // octets lus : références de l'éditeur
  }
  // Sauvegarde en FLASH : la dernière écriture tentée est vérifiée (une
  // seule lecture) après un redémarrage, jamais réécrite automatiquement.
  this->pref_ = global_preferences->make_preference<visslink::EditorRecord>(fnv1_hash("visslink_editor_v1"), true);
  visslink::EditorRecord record;
  const bool loaded = this->pref_.load(&record) && visslink::editor_record_valid(record);
  this->sequencer_.boot(loaded ? &record : nullptr, millis());
  if (this->parent_ != nullptr)
    this->parent_->set_editor_client(this);
  this->publish_(millis());
}

void VissLinkScheduleEditor::dump_config() {
  ESP_LOGCONFIG(TAG, "VissLink schedule editor (experimental):");
  for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++) {
    if (this->schedules_[c] != nullptr)
      ESP_LOGCONFIG(TAG, "  %s (%s): 0x%04X + 8 x day", EDITOR_CIRCUIT_KEYS[c], EDITOR_CIRCUIT_LABELS[c],
                    EDITOR_CIRCUIT_ADDRESSES[c]);
  }
  ESP_LOGCONFIG(TAG, "  Ranges: 10 min steps, 4 per day, end <= 23:50; read-back of the 8 bytes required");
}

void VissLinkScheduleEditor::loop() {
  const uint32_t now = millis();
  this->dispatch_(now);
  this->publish_(now);
}

bool VissLinkScheduleEditor::refuse_(const std::string &why) {
  ESP_LOGW(TAG, "Request refused: %s", why.c_str());
  this->last_error_ = why;
  this->refusal_ = VL_TEXT("Refusé : ") + why;
  this->refusal_until_ms_ = millis() + 30000;
  this->publish_(millis());
  return false;
}

bool VissLinkScheduleEditor::context_(const std::string &circuit, visslink::EditContext *ctx) {
  this->last_error_.clear();
  if (this->parent_ == nullptr)
    return this->refuse_(VL_TEXT("composant visslink absent"));
  if (!this->parent_->link_ok())
    return this->refuse_(VL_TEXT("liaison Optolink non synchronisée"));
  const uint8_t c = visslink::editor_circuit_from_key(circuit);
  VissLinkSchedule *schedule = this->schedule_for_(c);
  if (schedule == nullptr)
    return this->refuse_(VL_TEXT("circuit non déclaré dans l'éditeur : « ") + circuit + VL_TEXT(" »"));
  schedule->raw_week(&ctx->cache);
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    const uint16_t address = visslink::schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[c], d);
    if (this->parent_->block_locked_by_other(visslink::BLOCK_OWNER_EDITOR, address))
      ctx->locked_mask |= static_cast<uint8_t>(1u << d);
  }
  ctx->busy = this->sequencer_.busy() || this->parent_->block_client_busy_other(visslink::BLOCK_OWNER_EDITOR);
  if (!this->sequencer_.busy() && ctx->busy)
    return this->refuse_(VL_TEXT("bouclage ECS temporaire en cours : réessayer après la restauration"));
  return true;
}

bool VissLinkScheduleEditor::submit_(EditKind kind, const std::string &circuit, const std::string &days,
                                     const std::string &ranges, int source, const std::string &expected) {
  visslink::EditContext ctx;
  if (!this->context_(circuit, &ctx))
    return false;
  visslink::DayList list;
  if (!visslink::parse_day_list(days, list.days, &list.count))
    return this->refuse_(VL_TEXT("jours illisibles « ") + days + VL_TEXT(" » (ex. \"0,1,2\", 0 = lundi, ou \"*\")"));
  EditRequest request;
  return this->start_(request, visslink::prepare_edit(kind, circuit, list, ranges, source, expected, ctx, &request));
}

bool VissLinkScheduleEditor::start_(const EditRequest &request, const std::string &error) {
  if (!error.empty())
    return this->refuse_(error);
  if (!this->sequencer_.start(request, millis()))
    return this->refuse_(VL_TEXT("une opération sur les programmations est déjà en cours"));
  ESP_LOGI(TAG, "%s: %s requested (%s)", EDITOR_CIRCUIT_LABELS[request.circuit], request.undo ? "undo" : "edit",
           visslink::editor_days_text(request.days_mask).c_str());
  this->refusal_.clear();
  this->dispatch_(millis());
  this->publish_(millis());
  return true;
}

bool VissLinkScheduleEditor::undo() {
  this->last_error_.clear();
  EditRequest request;
  if (!this->sequencer_.undo_request(&request))
    return this->refuse_(VL_TEXT("aucune modification à annuler"));
  visslink::EditContext ctx;
  if (!this->context_(EDITOR_CIRCUIT_KEYS[request.circuit], &ctx))
    return false;
  if (ctx.busy)
    return this->refuse_(VL_TEXT("une opération sur les programmations est déjà en cours"));
  if (ctx.locked_mask & request.days_mask)
    return this->refuse_(VL_TEXT("plage temporaire du bouclage en cours, réessayer après la restauration"));
  const uint8_t day = this->sequencer_.record().day;
  if (ctx.cache.known(day) &&
      std::memcmp(ctx.cache.raw[day], this->sequencer_.record().target, visslink::SCHEDULE_BLOCK_LENGTH) != 0)
    return this->refuse_(VL_TEXT("le jour a été modifié depuis (panneau ?) : annulation impossible sans écraser"));
  return this->start_(request, "");
}

bool VissLinkScheduleEditor::refresh_(const std::string &circuit) {
  this->last_error_.clear();
  const uint8_t c = visslink::editor_circuit_from_key(circuit);
  VissLinkSchedule *schedule = this->schedule_for_(c);
  if (schedule == nullptr || this->parent_ == nullptr)
    return this->refuse_(VL_TEXT("circuit non déclaré dans l'éditeur : « ") + circuit + VL_TEXT(" »"));
  if (!this->parent_->request_register_cycle(schedule))
    return this->refuse_(VL_TEXT("relecture déjà en cours pour ce circuit, réessayer dans une minute"));
  ESP_LOGI(TAG, "%s: refresh of the 7 days requested", EDITOR_CIRCUIT_LABELS[c]);
  return true;
}

void VissLinkScheduleEditor::read_(const std::string &circuit, JsonObject root) {
  const uint32_t now = millis();
  uint8_t c = visslink::editor_circuit_from_key(circuit);
  if (this->schedule_for_(c) == nullptr) {
    // Circuit absent : le premier déclaré.
    c = EDITOR_NO_CIRCUIT;
    for (uint8_t i = 0; i < EDITOR_CIRCUIT_COUNT && c == EDITOR_NO_CIRCUIT; i++) {
      if (this->schedules_[i] != nullptr)
        c = i;
    }
  }
  JsonArray circuits = root["circuits"].to<JsonArray>();
  for (uint8_t i = 0; i < EDITOR_CIRCUIT_COUNT; i++) {
    if (this->schedules_[i] == nullptr)
      continue;
    JsonObject o = circuits.add<JsonObject>();
    o["key"] = EDITOR_CIRCUIT_KEYS[i];
    o["label"] = EDITOR_CIRCUIT_LABELS[i];
  }
  root["max_end"] = "23:50";
  root["max_ranges"] = visslink::SCHEDULE_SLOTS;
  root["link_ok"] = this->parent_ != nullptr && this->parent_->link_ok();
  root["busy"] = this->sequencer_.busy();
  root["status"] = this->refusal_.empty() ? this->sequencer_.status() : this->refusal_;
  const uint8_t op = this->sequencer_.circuit();
  if (op < EDITOR_CIRCUIT_COUNT) {
    JsonObject o = root["operation"].to<JsonObject>();
    o["circuit"] = EDITOR_CIRCUIT_KEYS[op];
    o["busy"] = this->sequencer_.busy();
    o["current_day"] = this->sequencer_.current_day();
    o["requested"] = this->sequencer_.requested_mask();
    o["saved"] = this->sequencer_.saved_mask();
    o["unchanged"] = this->sequencer_.unchanged_mask();
    o["not_done"] = this->sequencer_.not_done_mask();
    o["failed_day"] = this->sequencer_.failed_day();
    o["reason"] = this->sequencer_.failure_reason();
  }
  if (this->sequencer_.has_record()) {
    const visslink::EditorRecord &r = this->sequencer_.record();
    JsonObject u = root["undo"].to<JsonObject>();
    u["circuit"] = EDITOR_CIRCUIT_KEYS[r.circuit];
    u["day"] = r.day;
    u["original"] = visslink::editor_block_hex(r.original);
    u["target"] = visslink::editor_block_hex(r.target);
  }
  if (c == EDITOR_NO_CIRCUIT)
    return;
  VissLinkSchedule *schedule = this->schedules_[c];
  root["circuit"] = EDITOR_CIRCUIT_KEYS[c];
  root["label"] = EDITOR_CIRCUIT_LABELS[c];
  visslink::WeekBlocks week;
  schedule->raw_week(&week);
  root["expected"] = visslink::format_expected_week(week);
  JsonArray days = root["days"].to<JsonArray>();
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    JsonObject o = days.add<JsonObject>();
    o["day"] = d;
    o["label"] = SCHEDULE_DAY_LABELS[d];
    uint32_t read_ms = 0;
    const bool known = schedule->day_read_ms(d, &read_ms);
    o["known"] = known;
    const uint16_t address = visslink::schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[c], d);
    o["locked"] = this->parent_ != nullptr && this->parent_->block_locked_by_other(visslink::BLOCK_OWNER_EDITOR, address);
    if (!known)
      continue;
    o["raw"] = visslink::editor_block_hex(week.raw[d]);
    o["age_s"] = (now - read_ms) / 1000u;
    visslink::DaySchedule day;
    JsonArray ranges = o["ranges"].to<JsonArray>();
    if (visslink::decode_schedule_day(week.raw[d], visslink::SCHEDULE_BLOCK_LENGTH, &day) ==
        visslink::ScheduleError::NONE) {
      for (uint8_t i = 0; i < day.count; i++) {
        visslink::DaySchedule one;
        one.count = 1;
        one.start[0] = day.start[i];
        one.end[0] = day.end[i];
        ranges.add(visslink::format_day(one));
      }
    }
  }
}

bool VissLinkScheduleEditor::persist_(const visslink::EditorRecord &record) {
  // Écriture, synchronisation immédiate de la flash, puis relecture.
  if (!this->pref_.save(&record) || !global_preferences->sync())
    return false;
  visslink::EditorRecord check;
  return this->pref_.load(&check) && std::memcmp(&check, &record, sizeof(record)) == 0;
}

void VissLinkScheduleEditor::dispatch_(uint32_t now) {
  if (this->parent_ == nullptr)
    return;
  for (int guard = 0; guard < 8; guard++) {
    const EditorAction a = this->sequencer_.poll(now);
    switch (a.type) {
      case EditorActionType::READ:
        if (!this->parent_->request_direct_read(a.address, visslink::SCHEDULE_BLOCK_LENGTH,
                                                visslink::BLOCK_OWNER_EDITOR))
          this->sequencer_.on_read_not_submitted();  // ligne occupée : nouvel essai
        return;  // résultat via on_direct_read
      case EditorActionType::WRITE:
        if (!this->parent_->submit_block_write(a.address, a.data, now, visslink::BLOCK_OWNER_EDITOR)) {
          this->sequencer_.on_write(false, false, now);  // rien d'émis
          break;
        }
        return;  // résultat via on_block_write
      case EditorActionType::SAVE: {
        const bool ok = this->persist_(a.record);
        ESP_LOGI(TAG, "Backup of 0x%04X (%s -> %s): %s", a.record.address,
                 visslink::hex_block(a.record.original).c_str(), visslink::hex_block(a.record.target).c_str(),
                 ok ? "saved to flash" : "FAILED");
        this->sequencer_.on_saved(ok, now);
        break;
      }
      default:
        return;
    }
  }
}

void VissLinkScheduleEditor::on_direct_read(uint16_t address, bool ok, const uint8_t *data, uint8_t length,
                                            uint32_t now) {
  if (this->sequencer_.on_read(ok, data, length, now)) {
    // Bloc valide relu : publié comme une lecture normale (la Vitotronic
    // reste la source de vérité de l'affichage).
    uint8_t circuit, day;
    if (visslink::editor_block_position(address, &circuit, &day) && this->schedule_for_(circuit) != nullptr)
      this->schedule_for_(circuit)->publish_data(day, data);
  }
  this->dispatch_(now);
}

void VissLinkScheduleEditor::on_block_write(uint16_t /*address*/, bool sent, bool acked, uint32_t now) {
  this->sequencer_.on_write(sent, acked, now);
  this->dispatch_(now);  // relecture demandée aussitôt : l'arbitre reste occupé jusque-là
}

void VissLinkScheduleEditor::publish_(uint32_t now) {
  if (!this->refusal_.empty() && visslink::time_reached(now, this->refusal_until_ms_))
    this->refusal_.clear();
  const std::string status = this->refusal_.empty() ? this->sequencer_.status() : this->refusal_;
  if (status == this->last_status_)
    return;
  this->last_status_ = status;
  ESP_LOGI(TAG, "%s", status.c_str());
  if (this->status_sensor_ != nullptr) {
    std::string text = status;
    visslink::truncate_state(text);
    this->status_sensor_->publish_state(text);
  }
}

}  // namespace visslink_schedule_editor
}  // namespace esphome
