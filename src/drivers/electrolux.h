// Electrolux AC over its Broadlink OEM module.
//
// Ported from christiandt/electrolux-ac-cli (electrolux/cli.py). Every
// sub-command and JSON field name below comes from that file.

#pragma once

#include "../ac_driver.h"
#include "../config_store.h"
#include "broadlink.h"

namespace acbridge {

class ElectroluxDriver : public AcDriver {
 public:
  explicit ElectroluxDriver(const ElectroluxConfig& cfg);

  const char* id() const override { return "electrolux"; }
  const char* name() const override { return name_; }
  Vendor vendor() const override { return Vendor::Electrolux; }

  bool supportsFeature(Feature f) const override;
  bool supportsAction(Action a) const override;
  // The module exposes the vertical vane only (ac_vdir); there is no ac_hdir.
  bool supportsSwing(Swing s) const override { return s == Swing::Off || s == Swing::Vertical; }

  const char* rawStatus() const override { return raw_[0] != '\0' ? raw_ : nullptr; }

  bool poll(AcState& state) override;
  bool apply(const AcCommand& cmd, AcState& state) override;
  void reset() override { tx_.reset(); }

 private:
  // Sends one `{"field":value}` command and discards the echoed state; poll()
  // reads it back afterwards.
  bool sendField(uint16_t command, const char* field, int value, AcState& state);
  bool requestStatus(AcState& state);
  bool parseStatus(const char* json, size_t len, AcState& state);

  char name_[33];
  BroadlinkTransport tx_;
  // The real WP71-265WT status reply is 532 bytes across 34 fields, so 512 was
  // not enough; sized to match resp_ now. This is only the debug mirror served
  // by GET /api/units/electrolux/raw — parsing reads the full payload.
  char raw_[768] = {0};
  // Response scratch lives here rather than on the driver task's stack.
  uint8_t resp_[768] = {0};
};

}  // namespace acbridge
