// Owns the per-unit state table and the tasks that talk to the ACs.
//
// The C6 has a single high-performance core, so a blocking socket read on the
// Arduino loop() would stall HomeSpan's HAP polling. Every device call
// therefore happens on that unit's own task; REST and HomeKit only ever read a
// mutex-guarded snapshot and push onto a command queue. That also means two
// callers can never race on one device session — which matters, because both
// vendors' modules tolerate only one session at a time.

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "ac_driver.h"
#include "ac_types.h"

namespace acbridge {

// Called on the driver task after every state change. Used by the HomeKit layer
// to push updates into characteristics so Apple Home reflects changes made from
// the AC's own remote.
using StateListener = void (*)(size_t index, const AcState& state, void* ctx);

class AcRegistry {
 public:
  static constexpr size_t kMaxUnits = 4;
  static constexpr size_t kQueueDepth = 8;
  static constexpr uint32_t kPollIntervalMs = 20000;
  // After a failure, back off rather than hammering a unit whose app session is
  // holding the device.
  static constexpr uint32_t kMinBackoffMs = 5000;
  static constexpr uint32_t kMaxBackoffMs = 120000;

  // Register a driver. The registry does not take ownership — drivers are
  // expected to be long-lived (static or heap allocated for the process).
  bool add(AcDriver* driver);

  size_t count() const { return count_; }
  AcDriver* driverAt(size_t i) { return i < count_ ? units_[i].driver : nullptr; }
  int indexOf(const char* id) const;

  // Thread-safe copy of the current state. Returns false for an unknown id.
  bool snapshot(size_t index, AcState& out) const;
  bool snapshot(const char* id, AcState& out) const;

  // Queue a command for the unit's task. Returns false if unknown or the queue
  // is full.
  bool submit(size_t index, const AcCommand& cmd);
  bool submit(const char* id, const AcCommand& cmd);

  void setListener(StateListener listener, void* ctx) {
    listener_ = listener;
    listener_ctx_ = ctx;
  }

  // Spawn one task per registered unit. Call once, after WiFi is up.
  void start();

 private:
  struct Unit {
    AcDriver* driver = nullptr;
    AcState state;
    SemaphoreHandle_t lock = nullptr;
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    uint32_t backoff_ms = 0;
  };

  static void taskTrampoline(void* arg);
  void runUnit(size_t index);
  void publish(size_t index, const AcState& state);

  Unit units_[kMaxUnits];
  size_t count_ = 0;
  StateListener listener_ = nullptr;
  void* listener_ctx_ = nullptr;
};

extern AcRegistry g_registry;

}  // namespace acbridge
