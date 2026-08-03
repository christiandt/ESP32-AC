#include "ac_registry.h"

#include <Arduino.h>

namespace acbridge {

AcRegistry g_registry;

namespace {
struct TaskArg {
  AcRegistry* registry;
  size_t index;
};
}  // namespace

bool AcRegistry::add(AcDriver* driver) {
  if (driver == nullptr || count_ >= kMaxUnits) return false;

  Unit& u = units_[count_];
  u.driver = driver;
  u.lock = xSemaphoreCreateMutex();
  u.queue = xQueueCreate(kQueueDepth, sizeof(AcCommand));
  if (u.lock == nullptr || u.queue == nullptr) return false;

  u.state = AcState{};
  u.state.setError("not polled yet");
  count_++;
  return true;
}

int AcRegistry::indexOf(const char* id) const {
  if (id == nullptr) return -1;
  for (size_t i = 0; i < count_; i++) {
    if (strcmp(units_[i].driver->id(), id) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool AcRegistry::snapshot(size_t index, AcState& out) const {
  if (index >= count_) return false;
  const Unit& u = units_[index];
  if (xSemaphoreTake(u.lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  out = u.state;
  xSemaphoreGive(u.lock);
  return true;
}

bool AcRegistry::snapshot(const char* id, AcState& out) const {
  const int i = indexOf(id);
  return i >= 0 && snapshot(static_cast<size_t>(i), out);
}

bool AcRegistry::submit(size_t index, const AcCommand& cmd) {
  if (index >= count_) return false;
  return xQueueSend(units_[index].queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool AcRegistry::submit(const char* id, const AcCommand& cmd) {
  const int i = indexOf(id);
  return i >= 0 && submit(static_cast<size_t>(i), cmd);
}

void AcRegistry::publish(size_t index, const AcState& state) {
  Unit& u = units_[index];
  if (xSemaphoreTake(u.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
    u.state = state;
    xSemaphoreGive(u.lock);
  }
  // Deliberately outside the lock: a listener is free to call snapshot().
  if (listener_ != nullptr) listener_(index, state, listener_ctx_);
}

void AcRegistry::taskTrampoline(void* arg) {
  auto* ta = static_cast<TaskArg*>(arg);
  AcRegistry* self = ta->registry;
  const size_t index = ta->index;
  delete ta;
  self->runUnit(index);
  vTaskDelete(nullptr);
}

void AcRegistry::runUnit(size_t index) {
  Unit& u = units_[index];

  // Task-local working copy. Readings survive a failed poll so the REST API can
  // still show the last known values alongside online=false.
  AcState working = u.state;
  working.clearError();

  for (;;) {
    AcCommand cmd;
    const uint32_t wait_ms = u.backoff_ms > 0 ? u.backoff_ms : kPollIntervalMs;

    // The timeout only applies when the queue is empty, so a burst of queued
    // commands drains back-to-back before the next scheduled poll.
    const bool have_cmd = xQueueReceive(u.queue, &cmd, pdMS_TO_TICKS(wait_ms)) == pdTRUE;

    const bool ok = have_cmd ? u.driver->apply(cmd, working) : u.driver->poll(working);

    if (ok) {
      working.online = true;
      working.clearError();
      working.updated_ms = millis();
      u.backoff_ms = 0;
    } else {
      working.online = false;
      if (working.error[0] == '\0') working.setError("device did not respond");
      // Drop the session; both vendors need a fresh handshake after a drop.
      u.driver->reset();
      u.backoff_ms = u.backoff_ms == 0
                         ? kMinBackoffMs
                         : (u.backoff_ms >= kMaxBackoffMs / 2 ? kMaxBackoffMs : u.backoff_ms * 2);
      log_w("[%s] %s (retrying in %ums)", u.driver->id(), working.error, u.backoff_ms);
    }

    publish(index, working);
  }
}

void AcRegistry::start() {
  for (size_t i = 0; i < count_; i++) {
    auto* arg = new TaskArg{this, i};
    char name[16];
    snprintf(name, sizeof(name), "ac_%s", units_[i].driver->id());
    // 6 KB: the Midea driver runs mbedtls AES/SHA over stack buffers.
    xTaskCreate(&AcRegistry::taskTrampoline, name, 6144, arg, 4, &units_[i].task);
  }
}

}  // namespace acbridge
