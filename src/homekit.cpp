#include "homekit.h"

#include <HomeSpan.h>

#include "ac_registry.h"

namespace acbridge {

namespace {

bool g_started = false;

// How often characteristics are refreshed from the registry snapshot. The
// drivers poll every 20 s, so this only needs to be fast enough that a change
// made on the AC's own remote shows up promptly once the driver has seen it.
constexpr uint32_t kSyncIntervalMs = 1000;

// HAP enum values.
constexpr uint8_t kActiveInactive = 0;
constexpr uint8_t kActiveActive = 1;
constexpr uint8_t kCurrentInactive = 0;
constexpr uint8_t kCurrentIdle = 1;
constexpr uint8_t kCurrentHeating = 2;
constexpr uint8_t kCurrentCooling = 3;
constexpr uint8_t kTargetAuto = 0;
constexpr uint8_t kTargetHeat = 1;
constexpr uint8_t kTargetCool = 2;

void submit(size_t index, const AcCommand& cmd) { g_registry.submit(index, cmd); }

// Named fan steps as percentages, so HomeKit's slider lands on something the
// unit actually has.
uint8_t presetToPercent(FanPreset p) {
  switch (p) {
    case FanPreset::Silent: return 20;
    case FanPreset::Low: return 40;
    case FanPreset::Medium: return 60;
    case FanPreset::High: return 80;
    case FanPreset::Turbo: return 100;
    case FanPreset::Auto: return 100;  // shown at full; the Fan Auto switch owns the real state
  }
  return 100;
}

// ---------------------------------------------------------------------------
// The AC itself
// ---------------------------------------------------------------------------

struct AcHeaterCooler : Service::HeaterCooler {
  size_t index;
  uint32_t next_sync = 0;

  SpanCharacteristic* active;
  SpanCharacteristic* current_temp;
  SpanCharacteristic* current_state;
  SpanCharacteristic* target_state;
  SpanCharacteristic* cool_threshold;
  SpanCharacteristic* heat_threshold;
  SpanCharacteristic* rotation_speed;
  SpanCharacteristic* swing_mode;

  AcHeaterCooler(size_t i, float min_target, float max_target) : Service::HeaterCooler(), index(i) {
    active = new Characteristic::Active(kActiveInactive);
    current_temp = new Characteristic::CurrentTemperature(20);
    current_state = new Characteristic::CurrentHeaterCoolerState(kCurrentInactive);
    target_state = new Characteristic::TargetHeaterCoolerState(kTargetAuto);

    // These units have a single setpoint, but HomeKit's AUTO mode shows two
    // thresholds. Both are kept equal, and writing either one sets the target.
    cool_threshold = new Characteristic::CoolingThresholdTemperature(min_target);
    heat_threshold = new Characteristic::HeatingThresholdTemperature(min_target);
    // HomeKit's defaults (cooling 10-35, heating 0-25) don't match what the
    // unit accepts, so widen both to what the device reported to mpsac.
    cool_threshold->setRange(min_target, max_target, 0.5);
    heat_threshold->setRange(min_target, max_target, 0.5);

    rotation_speed = new Characteristic::RotationSpeed(100);
    swing_mode = new Characteristic::SwingMode(0);
  }

  boolean update() override {
    AcCommand cmd;

    if (active->updated()) {
      cmd.power.set(active->getNewVal<uint8_t>() == kActiveActive);
    }

    if (target_state->updated()) {
      switch (target_state->getNewVal<uint8_t>()) {
        case kTargetHeat: cmd.mode.set(Mode::Heat); break;
        case kTargetCool: cmd.mode.set(Mode::Cool); break;
        default: cmd.mode.set(Mode::Auto); break;
      }
    }

    // Whichever threshold moved is the new setpoint; the other follows on the
    // next sync.
    if (cool_threshold->updated()) {
      cmd.target_temp.set(cool_threshold->getNewVal<float>());
    } else if (heat_threshold->updated()) {
      cmd.target_temp.set(heat_threshold->getNewVal<float>());
    }

    if (rotation_speed->updated()) {
      const int pct = rotation_speed->getNewVal<int>();
      if (pct >= 1) cmd.fan.set(FanSpeed::ofPercent(static_cast<uint8_t>(pct)));
    }

    if (swing_mode->updated()) {
      // Vertical is the axis both units have.
      cmd.swing.set(swing_mode->getNewVal<uint8_t>() == 1 ? Swing::Vertical : Swing::Off);
    }

    if (!cmd.empty()) submit(index, cmd);
    return true;
  }

  void loop() override {
    if (millis() < next_sync) return;
    next_sync = millis() + kSyncIntervalMs;

    AcState st;
    if (!g_registry.snapshot(index, st)) return;

    const bool on = st.power.get(false);
    active->setVal(on ? kActiveActive : kActiveInactive);

    if (st.indoor_temp.has()) current_temp->setVal(st.indoor_temp.value);
    if (st.target_temp.has()) {
      cool_threshold->setVal(st.target_temp.value);
      heat_threshold->setVal(st.target_temp.value);
    }

    if (st.mode.has()) {
      switch (st.mode.value) {
        case Mode::Heat: target_state->setVal(kTargetHeat); break;
        case Mode::Cool: target_state->setVal(kTargetCool); break;
        // Dry and Fan have no HomeKit equivalent; leave the dial where it is
        // and let the aux switches show what the unit is really doing.
        default: target_state->setVal(kTargetAuto); break;
      }
    }

    // HomeKit wants to know whether the unit is actively working right now,
    // which the protocol doesn't report — derive it from setpoint vs. room.
    uint8_t cur = kCurrentInactive;
    if (on) {
      cur = kCurrentIdle;
      if (st.indoor_temp.has() && st.target_temp.has()) {
        const float indoor = st.indoor_temp.value;
        const float target = st.target_temp.value;
        const Mode m = st.mode.get(Mode::Auto);
        if ((m == Mode::Cool || m == Mode::Auto) && indoor > target + 0.5f) {
          cur = kCurrentCooling;
        } else if ((m == Mode::Heat || m == Mode::Auto) && indoor < target - 0.5f) {
          cur = kCurrentHeating;
        }
      }
    }
    current_state->setVal(cur);

    if (st.fan.has()) {
      rotation_speed->setVal(st.fan.value.is_percent ? st.fan.value.percent
                                                     : presetToPercent(st.fan.value.preset));
    }
    if (st.swing.has()) swing_mode->setVal(st.swing.value == Swing::Off ? 0 : 1);
  }
};

// ---------------------------------------------------------------------------
// Aux switches, for everything HomeKit has no vocabulary for
// ---------------------------------------------------------------------------

// Selects an operating mode HeaterCooler can't express (dry, fan only).
struct ModeSwitch : Service::Switch {
  size_t index;
  Mode mode;
  uint32_t next_sync = 0;
  SpanCharacteristic* on;

  ModeSwitch(size_t i, Mode m) : Service::Switch(), index(i), mode(m) {
    on = new Characteristic::On(false);
  }

  boolean update() override {
    AcCommand cmd;
    if (on->getNewVal<bool>()) {
      cmd.mode.set(mode);
      cmd.power.set(true);
    } else {
      // Turning a mode switch off has no natural target, so fall back to auto
      // rather than guessing or silently doing nothing.
      cmd.mode.set(Mode::Auto);
    }
    submit(index, cmd);
    return true;
  }

  void loop() override {
    if (millis() < next_sync) return;
    next_sync = millis() + kSyncIntervalMs;

    AcState st;
    if (!g_registry.snapshot(index, st)) return;
    on->setVal(st.power.get(false) && st.mode.has() && st.mode.value == mode);
  }
};

struct FeatureSwitch : Service::Switch {
  size_t index;
  Feature feature;
  uint32_t next_sync = 0;
  SpanCharacteristic* on;

  FeatureSwitch(size_t i, Feature f) : Service::Switch(), index(i), feature(f) {
    on = new Characteristic::On(false);
  }

  boolean update() override {
    AcCommand cmd;
    cmd.feature(feature).set(on->getNewVal<bool>());
    submit(index, cmd);
    return true;
  }

  void loop() override {
    if (millis() < next_sync) return;
    next_sync = millis() + kSyncIntervalMs;

    AcState st;
    if (!g_registry.snapshot(index, st)) return;
    if (st.feature(feature).has()) on->setVal(st.feature(feature).value);
  }
};

// The Midea display is a flip rather than a settable flag, so it can only be
// driven through the toggle action.
struct ActionSwitch : Service::Switch {
  size_t index;
  Action action;
  Feature mirrors;
  uint32_t next_sync = 0;
  SpanCharacteristic* on;

  ActionSwitch(size_t i, Action a, Feature m)
      : Service::Switch(), index(i), action(a), mirrors(m) {
    on = new Characteristic::On(false);
  }

  boolean update() override {
    AcState st;
    if (!g_registry.snapshot(index, st)) return true;

    const bool want = on->getNewVal<bool>();
    // Only send the flip when it would actually change something.
    if (!st.feature(mirrors).has() || st.feature(mirrors).value != want) {
      AcCommand cmd;
      cmd.action = action;
      submit(index, cmd);
    }
    return true;
  }

  void loop() override {
    if (millis() < next_sync) return;
    next_sync = millis() + kSyncIntervalMs;

    AcState st;
    if (!g_registry.snapshot(index, st)) return;
    if (st.feature(mirrors).has()) on->setVal(st.feature(mirrors).value);
  }
};

// RotationSpeed is a percentage, so automatic fan needs its own control.
struct FanAutoSwitch : Service::Switch {
  size_t index;
  uint32_t next_sync = 0;
  SpanCharacteristic* on;

  explicit FanAutoSwitch(size_t i) : Service::Switch(), index(i) {
    on = new Characteristic::On(false);
  }

  boolean update() override {
    if (!on->getNewVal<bool>()) return true;  // turning it off is the slider's job
    AcCommand cmd;
    cmd.fan.set(FanSpeed::ofPreset(FanPreset::Auto));
    submit(index, cmd);
    return true;
  }

  void loop() override {
    if (millis() < next_sync) return;
    next_sync = millis() + kSyncIntervalMs;

    AcState st;
    if (!g_registry.snapshot(index, st)) return;
    on->setVal(st.fan.has() && !st.fan.value.is_percent && st.fan.value.preset == FanPreset::Auto);
  }
};

}  // namespace

void homekitBegin(const Config& cfg) {
  if (g_started) return;

  homeSpan.setLogLevel(0);
  // Hand HomeSpan the credentials too, so it can re-establish the connection on
  // its own if WiFi drops. main.cpp has already made the initial connection.
  if (cfg.wifi.valid()) homeSpan.setWifiCredentials(cfg.wifi.ssid, cfg.wifi.psk);
  // The category is device-wide and only picks the pairing icon; see the header.
  homeSpan.begin(Category::Bridges, "ESP32-AC", "esp32-ac");

  // Bridge accessory: information only, no functional services.
  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("ESP32-AC");
  new Characteristic::Manufacturer("christiandt");
  new Characteristic::Model("XIAO ESP32C6");
  new Characteristic::FirmwareRevision("0.1.0");

  for (size_t i = 0; i < g_registry.count(); i++) {
    AcDriver* drv = g_registry.driverAt(i);

    // Setpoint limits have to be known before pairing, so take them from the
    // config mpsac exported rather than waiting for the first poll.
    float min_target = 16.0f;
    float max_target = 30.0f;
    if (drv->vendor() == Vendor::Midea) {
      min_target = cfg.midea.min_target;
      max_target = cfg.midea.max_target;
    }
    if (max_target <= min_target) {
      min_target = 16.0f;
      max_target = 30.0f;
    }

    new SpanAccessory();
    new Service::AccessoryInformation();
    new Characteristic::Identify();
    new Characteristic::Name(drv->name());
    new Characteristic::Manufacturer(vendorName(drv->vendor()));
    new Characteristic::Model(drv->id());
    new Characteristic::FirmwareRevision("0.1.0");

    // setPrimary() makes the Home app render the accessory tile as an air
    // conditioner rather than picking one of the aux switches.
    (new AcHeaterCooler(i, min_target, max_target))->setPrimary();
    new Characteristic::ConfiguredName(drv->name());

    // Modes HeaterCooler can't express.
    new ModeSwitch(i, Mode::Dry);
    new Characteristic::ConfiguredName("Dry");
    new ModeSwitch(i, Mode::Fan);
    new Characteristic::ConfiguredName("Fan Only");

    new FanAutoSwitch(i);
    new Characteristic::ConfiguredName("Fan Auto");

    // Feature switches, only for what this unit reports it supports.
    struct FeatureLabel {
      Feature feature;
      const char* label;
    };
    static const FeatureLabel kLabels[] = {
        {Feature::Eco, "ECO"},         {Feature::Boost, "Boost"},
        {Feature::Sleep, "Sleep"},     {Feature::Ion, "Ion"},
        {Feature::Freeze, "Freeze Protection"}, {Feature::FollowMe, "Follow Me"},
        {Feature::SelfClean, "Self Clean"},
    };
    for (const FeatureLabel& fl : kLabels) {
      if (!drv->supportsFeature(fl.feature)) continue;
      new FeatureSwitch(i, fl.feature);
      new Characteristic::ConfiguredName(fl.label);
    }

    // The display: settable on the Electrolux, a flip on the Midea.
    if (drv->supportsFeature(Feature::Led)) {
      if (drv->supportsAction(Action::LedToggle)) {
        new ActionSwitch(i, Action::LedToggle, Feature::Led);
      } else {
        new FeatureSwitch(i, Feature::Led);
      }
      new Characteristic::ConfiguredName("Display");
    }
  }

  g_started = true;
  Serial.println("HomeKit: bridge started");
}

void homekitLoop() {
  if (!g_started) return;
  homeSpan.poll();
}

bool homekitStarted() { return g_started; }

}  // namespace acbridge
