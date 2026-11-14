#include "read_engine.h"

namespace esphome {
namespace visslink {

bool ReadEngine::loop(uint32_t now, ReadEvent *event) {
  bool produced = false;
  const KwResult result = this->protocol_.loop(now);

  if (result != KwResult::NONE && this->current_ != RegisterScheduler::NONE) {
    const size_t index = static_cast<size_t>(this->current_);
    this->current_ = RegisterScheduler::NONE;
    const RegisterSlot &slot = this->scheduler_.slot(index);

    event->index = index;
    event->result = result;
    event->attempt = slot.failures + 1;
    event->exhausted = false;
    event->length = 0;
    if (result == KwResult::SUCCESS) {
      event->length = this->protocol_.data_length();
      for (uint8_t i = 0; i < event->length; i++)
        event->data[i] = this->protocol_.data()[i];
      this->scheduler_.on_success(index, now);
    } else {
      event->exhausted = this->scheduler_.on_failure(index, now);
    }
    produced = true;
  }

  this->start_next_(now);
  return produced;
}

void ReadEngine::start_next_(uint32_t now) {
  if (this->current_ != RegisterScheduler::NONE || !this->protocol_.is_idle())
    return;
  const int index = this->scheduler_.acquire_next(now);
  if (index == RegisterScheduler::NONE)
    return;
  const RegisterSlot &slot = this->scheduler_.slot(index);
  if (this->protocol_.request_read(slot.address, slot.length, now)) {
    this->current_ = index;
  } else {
    // Longueur invalide (normalement impossible : validée dans le YAML).
    this->scheduler_.on_failure(index, now);
  }
}

}  // namespace visslink
}  // namespace esphome
