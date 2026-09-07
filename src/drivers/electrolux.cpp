#include "electrolux.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace acbridge {

using namespace acbridge::proto;

namespace {

constexpr uint16_t kBroadlinkCmdPacket = 0x6A;

// electrolux/cli.py:118 — {"auto": 4, "cool": 0, "heat": 1, "dry": 2, "fan": 3, "heat_8": 6}
int modeCode(Mode m) {
  switch (m) {
    case Mode::Cool: return 0;
    case Mode::Heat: return 1;
    case Mode::Dry: return 2;
    case Mode::Fan: return 3;
    case Mode::Auto: return 4;
  }
  return 4;
}

bool modeFromCode(int code, Mode& out) {
  switch (code) {
    case 0: out = Mode::Cool; return true;
    case 1: out = Mode::Heat; return true;
    case 2: out = Mode::Dry; return true;
    case 3: out = Mode::Fan; return true;
    case 4: out = Mode::Auto; return true;
    // 6 is the unit's "heat_8" frost-watch mode, which has no HomeKit or REST
    // equivalent; report it as heat rather than inventing vocabulary.
    case 6: out = Mode::Heat; return true;
  }
  return false;
}

// electrolux/cli.py:132 — {"auto": 0, "low": 1, "mid": 2, "high": 3, "turbo": 4, "quiet": 5}
int fanCode(FanPreset p) {
  switch (p) {
    case FanPreset::Auto: return 0;
    case FanPreset::Low: return 1;
    case FanPreset::Medium: return 2;
    case FanPreset::High: return 3;
    case FanPreset::Turbo: return 4;
    case FanPreset::Silent: return 5;  // the unit calls this "quiet"
  }
  return 0;
}

bool fanFromCode(int code, FanPreset& out) {
  switch (code) {
    case 0: out = FanPreset::Auto; return true;
    case 1: out = FanPreset::Low; return true;
    case 2: out = FanPreset::Medium; return true;
    case 3: out = FanPreset::High; return true;
    case 4: out = FanPreset::Turbo; return true;
    case 5: out = FanPreset::Silent; return true;
  }
  return false;
}

// This module has no custom percentage speed, so a percentage (which is what
// HomeKit's RotationSpeed gives us) snaps to the nearest step.
FanPreset presetForPercent(uint8_t pct) {
  if (pct <= 20) return FanPreset::Silent;
  if (pct <= 40) return FanPreset::Low;
  if (pct <= 60) return FanPreset::Medium;
  if (pct <= 80) return FanPreset::High;
  return FanPreset::Turbo;
}

// Reads an integer field that the device may encode as a number or a string.
bool readInt(JsonObjectConst o, const char* key, int& out) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return false;
  if (v.is<int>()) {
    out = v.as<int>();
    return true;
  }
  if (v.is<const char*>()) {
    out = atoi(v.as<const char*>());
    return true;
  }
  return false;
}

bool readFloat(JsonObjectConst o, const char* key, float& out) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return false;
  if (v.is<float>()) {
    out = v.as<float>();
    return true;
  }
  if (v.is<const char*>()) {
    out = static_cast<float>(atof(v.as<const char*>()));
    return true;
  }
  return false;
}

}  // namespace

ElectroluxDriver::ElectroluxDriver(const ElectroluxConfig& cfg) {
  strncpy(name_, cfg.name[0] != '\0' ? cfg.name : "Electrolux", sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = '\0';
  tx_.configure(cfg.ip);
}

bool ElectroluxDriver::supportsFeature(Feature f) const {
  // Only the three the CLI exposes: ac_slp, scrdisp, mldprf.
  return f == Feature::Sleep || f == Feature::Led || f == Feature::SelfClean;
}

bool ElectroluxDriver::supportsAction(Action a) const { return a == Action::ClearTimer; }

bool ElectroluxDriver::sendField(uint16_t command, const char* field, int value, AcState& state) {
  char json[64];
  const int n = snprintf(json, sizeof(json), "{\"%s\":%d}", field, value);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
    state.setError("command payload too long");
    return false;
  }

  uint8_t frame[128];
  const size_t frame_len =
      buildEluxFrame(frame, sizeof(frame), command, json, static_cast<size_t>(n));
  if (frame_len == 0) {
    state.setError("could not build command frame");
    return false;
  }

  size_t resp_len = 0;
  if (!tx_.sendPacket(kBroadlinkCmdPacket, frame, frame_len, resp_, sizeof(resp_), &resp_len)) {
    state.setError(tx_.lastError());
    return false;
  }
  return true;
}

bool ElectroluxDriver::requestStatus(AcState& state) {
  uint8_t frame[32];
  const size_t frame_len = buildEluxFrame(frame, sizeof(frame), kEluxCmdStatus, "{}", 2);

  size_t resp_len = 0;
  if (!tx_.sendPacket(kBroadlinkCmdPacket, frame, frame_len, resp_, sizeof(resp_), &resp_len)) {
    state.setError(tx_.lastError());
    return false;
  }

  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  if (!parseEluxFrame(resp_, resp_len, &payload, &payload_len)) {
    state.setError("status frame failed validation");
    return false;
  }

  // Keep a copy for GET /api/units/electrolux/raw. Truncating it only costs
  // some debug output; parsing below reads the untruncated payload, so a reply
  // longer than raw_ no longer fails as malformed JSON.
  const size_t copy = payload_len < sizeof(raw_) - 1 ? payload_len : sizeof(raw_) - 1;
  memcpy(raw_, payload, copy);
  raw_[copy] = '\0';

  return parseStatus(reinterpret_cast<const char*>(payload), payload_len, state);
}

// The status schema is not documented anywhere: electrolux-ac-cli never parses
// this response, and only `envtemp` is named (in its README). The field names
// below mirror the setters, which is the obvious reading but is inference until
// a real reply is seen. Anything missing simply stays absent in AcState, and
// the raw JSON is served from GET /api/units/electrolux/raw so the real names
// can be confirmed in one request.
bool ElectroluxDriver::parseStatus(const char* json, size_t len, AcState& state) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    state.setError("status was not valid JSON");
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();

  int i = 0;
  float f = 0;

  if (readFloat(o, "envtemp", f)) state.indoor_temp.set(f);
  if (readFloat(o, "temp", f)) state.target_temp.set(f);
  if (readInt(o, "ac_pwr", i)) state.power.set(i != 0);

  if (readInt(o, "ac_mode", i)) {
    Mode m;
    if (modeFromCode(i, m)) state.mode.set(m);
  }
  if (readInt(o, "ac_mark", i)) {
    FanPreset p;
    if (fanFromCode(i, p)) state.fan.set(FanSpeed::ofPreset(p));
  }
  if (readInt(o, "ac_vdir", i)) state.swing.set(i != 0 ? Swing::Vertical : Swing::Off);

  if (readInt(o, "ac_slp", i)) state.feature(Feature::Sleep).set(i != 0);
  if (readInt(o, "scrdisp", i)) state.feature(Feature::Led).set(i != 0);
  if (readInt(o, "mldprf", i)) state.feature(Feature::SelfClean).set(i != 0);

  // electrolux/cli.py:88 clamps every setpoint to this range.
  state.min_target = 16.0f;
  state.max_target = 30.0f;
  return true;
}

bool ElectroluxDriver::poll(AcState& state) { return requestStatus(state); }

bool ElectroluxDriver::apply(const AcCommand& cmd, AcState& state) {
  if (cmd.power.has()) {
    if (!sendField(kEluxCmdPower, "ac_pwr", cmd.power.value ? 1 : 0, state)) return false;
  }
  if (cmd.mode.has()) {
    if (!sendField(kEluxCmdMode, "ac_mode", modeCode(cmd.mode.value), state)) return false;
  }
  if (cmd.target_temp.has()) {
    // The unit takes whole degrees only, clamped to 16-30 (cli.py:88).
    int t = static_cast<int>(cmd.target_temp.value + 0.5f);
    if (t < 16) t = 16;
    if (t > 30) t = 30;
    if (!sendField(kEluxCmdTemp, "temp", t, state)) return false;
  }
  if (cmd.fan.has()) {
    const FanPreset p = cmd.fan.value.is_percent ? presetForPercent(cmd.fan.value.percent)
                                                 : cmd.fan.value.preset;
    if (!sendField(kEluxCmdMode, "ac_mark", fanCode(p), state)) return false;
  }
  if (cmd.swing.has()) {
    if (!sendField(kEluxCmdMode, "ac_vdir", cmd.swing.value == Swing::Off ? 0 : 1, state)) {
      return false;
    }
  }

  if (cmd.feature(Feature::Sleep).has()) {
    if (!sendField(kEluxCmdPower, "ac_slp", cmd.feature(Feature::Sleep).value ? 1 : 0, state)) {
      return false;
    }
  }
  if (cmd.feature(Feature::Led).has()) {
    if (!sendField(kEluxCmdMode, "scrdisp", cmd.feature(Feature::Led).value ? 1 : 0, state)) {
      return false;
    }
  }
  if (cmd.feature(Feature::SelfClean).has()) {
    if (!sendField(kEluxCmdPower, "mldprf", cmd.feature(Feature::SelfClean).value ? 1 : 0, state)) {
      return false;
    }
  }

  if (cmd.action == Action::ClearTimer) {
    // clear_timer(on) is timer(on, 0, 0) — cli.py:209.
    char json[32];
    const int n = snprintf(json, sizeof(json), "{\"timer\":\"0000|01\"}");
    uint8_t frame[64];
    const size_t frame_len =
        buildEluxFrame(frame, sizeof(frame), kEluxCmdTimer, json, static_cast<size_t>(n));
    size_t resp_len = 0;
    if (!tx_.sendPacket(kBroadlinkCmdPacket, frame, frame_len, resp_, sizeof(resp_), &resp_len)) {
      state.setError(tx_.lastError());
      return false;
    }
  }

  // Setters echo state back, but each covers only its own field. One status
  // read is simpler and keeps the whole snapshot consistent.
  return requestStatus(state);
}

}  // namespace acbridge
