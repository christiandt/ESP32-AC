// An in-memory AC, so the REST and HomeKit layers can be brought up and tested
// end-to-end before either real protocol exists.

#pragma once

#include "../ac_driver.h"

namespace acbridge {

class MockDriver : public AcDriver {
 public:
  MockDriver(const char* id, const char* name) : id_(id), name_(name) {}

  const char* id() const override { return id_; }
  const char* name() const override { return name_; }
  Vendor vendor() const override { return Vendor::Mock; }

  bool supportsFeature(Feature f) const override;
  bool supportsAction(Action a) const override;
  bool supportsSwing(Swing) const override { return true; }

  bool poll(AcState& state) override;
  bool apply(const AcCommand& cmd, AcState& state) override;

 private:
  void seed(AcState& state);

  const char* id_;
  const char* name_;
  bool seeded_ = false;
};

}  // namespace acbridge
