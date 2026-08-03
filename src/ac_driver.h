// The seam every AC implementation sits behind.
//
// Drivers are only ever called from their own FreeRTOS task (see AcRegistry),
// so implementations do not need to be thread-safe and may block on sockets.

#pragma once

#include "ac_types.h"

namespace acbridge {

class AcDriver {
 public:
  virtual ~AcDriver() = default;

  // Stable identifier used in REST paths, e.g. "midea".
  virtual const char* id() const = 0;
  virtual const char* name() const = 0;
  virtual Vendor vendor() const = 0;

  virtual bool supportsFeature(Feature) const { return false; }
  virtual bool supportsAction(Action) const { return false; }

  // Read the unit. On failure return false with state.error set; the registry
  // marks the unit offline and keeps the previous readings visible.
  virtual bool poll(AcState& state) = 0;

  // Push a command and leave `state` as current as the vendor's reply allows.
  // Both vendors echo state back from a write, so a successful apply usually
  // saves a follow-up poll.
  virtual bool apply(const AcCommand& cmd, AcState& state) = 0;

  // Drop any cached session so the next poll reconnects from scratch.
  virtual void reset() {}
};

}  // namespace acbridge
