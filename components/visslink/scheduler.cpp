#include "scheduler.h"
#include "time_utils.h"

namespace esphome {
namespace visslink {

size_t RegisterScheduler::add(uint16_t address, uint8_t length, uint32_t interval_ms) {
  RegisterSlot slot;
  slot.address = address;
  slot.length = length;
  slot.interval_ms = interval_ms;
  this->slots_.push_back(slot);
  return this->slots_.size() - 1;
}

void RegisterScheduler::start(uint32_t now) {
  for (auto &slot : this->slots_) {
    slot.cycle_due_ms = now;
    slot.next_due_ms = now;
    slot.failures = 0;
    slot.in_flight = false;
  }
  this->in_flight_ = NONE;
}

int RegisterScheduler::acquire_next(uint32_t now) {
  if (this->in_flight_ != NONE)
    return NONE;

  int best = NONE;
  uint32_t best_lateness = 0;
  for (size_t i = 0; i < this->slots_.size(); i++) {
    const RegisterSlot &slot = this->slots_[i];
    if (!time_reached(now, slot.next_due_ms))
      continue;
    const uint32_t lateness = now - slot.next_due_ms;
    // Inégalité stricte : à retard égal, le premier déclaré l'emporte.
    if (best == NONE || lateness > best_lateness) {
      best = static_cast<int>(i);
      best_lateness = lateness;
    }
  }
  if (best != NONE) {
    this->slots_[best].in_flight = true;
    this->in_flight_ = best;
  }
  return best;
}

void RegisterScheduler::schedule_next_cycle_(RegisterSlot &slot, uint32_t now) {
  uint32_t next = slot.cycle_due_ms + slot.interval_ms;
  // Cycle déjà dépassé (protocole lent ou nombreuses tentatives) : on ne
  // rattrape pas, on repart de maintenant.
  if (time_reached(now, next))
    next = now + slot.interval_ms;
  slot.cycle_due_ms = next;
  slot.next_due_ms = next;
  slot.failures = 0;
}

void RegisterScheduler::on_success(size_t index, uint32_t now) {
  if (index >= this->slots_.size())
    return;
  RegisterSlot &slot = this->slots_[index];
  slot.in_flight = false;
  slot.has_succeeded = true;
  slot.last_success_ms = now;
  this->schedule_next_cycle_(slot, now);
  this->in_flight_ = NONE;
}

bool RegisterScheduler::on_failure(size_t index, uint32_t now) {
  if (index >= this->slots_.size())
    return false;
  RegisterSlot &slot = this->slots_[index];
  slot.in_flight = false;
  this->in_flight_ = NONE;
  slot.failures++;
  if (slot.failures > this->max_retries_) {
    this->schedule_next_cycle_(slot, now);
    return true;
  }
  slot.next_due_ms = now + this->retry_delay_ms_;
  return false;
}

}  // namespace visslink
}  // namespace esphome
