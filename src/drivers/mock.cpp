#include "mock.h"

#include <Arduino.h>

namespace acbridge {

namespace {
constexpr Feature kSupported[] = {Feature::Eco, Feature::Boost, Feature::Sleep, Feature::Led};
}

bool MockDriver::supportsFeature(Feature f) const {
  for (Feature s : kSupported) {
    if (s == f) return true;
  }
  return false;
}

bool MockDriver::supportsAction(Action a) const {
  return a == Action::LedToggle || a == Action::SelfClean;
}

void MockDriver::seed(AcState& state) {
  state.power.set(true);
  state.mode.set(Mode::Cool);
  state.target_temp.set(22.0f);
  state.indoor_temp.set(25.5f);
  state.outdoor_temp.set(29.0f);
  state.humidity.set(48);
  state.fan.set(FanSpeed::ofPreset(FanPreset::Auto));
  state.swing.set(Swing::Vertical);
  for (Feature f : kSupported) state.feature(f).set(false);
  state.feature(Feature::Led).set(true);
  state.min_target = 16.0f;
  state.max_target = 30.0f;
  seeded_ = true;
}

bool MockDriver::poll(AcState& state) {
  if (!seeded_) seed(state);

  // Drift the room toward the setpoint so polling shows something moving and
  // HomeKit's CurrentHeaterCoolerState has a reason to change.
  if (state.power.get(false) && state.indoor_temp.has() && state.target_temp.has()) {
    const float indoor = state.indoor_temp.value;
    const float target = state.target_temp.value;
    const float delta = target - indoor;
    if (delta > 0.05f || delta < -0.05f) {
      state.indoor_temp.set(indoor + (delta > 0 ? 0.2f : -0.2f));
    }
  }
  return true;
}

bool MockDriver::apply(const AcCommand& cmd, AcState& state) {
  if (!seeded_) seed(state);

  if (cmd.power.has()) state.power.set(cmd.power.value);
  if (cmd.mode.has()) {
    state.mode.set(cmd.mode.value);
    state.power.set(true);  // mirrors mpsac: selecting a mode powers the unit on
  }
  if (cmd.target_temp.has()) state.target_temp.set(cmd.target_temp.value);
  if (cmd.fan.has()) state.fan.set(cmd.fan.value);
  if (cmd.swing.has()) state.swing.set(cmd.swing.value);

  for (size_t i = 0; i < kFeatureCount; i++) {
    if (cmd.features[i].has() && supportsFeature(static_cast<Feature>(i))) {
      state.features[i].set(cmd.features[i].value);
    }
  }

  switch (cmd.action) {
    case Action::LedToggle:
      state.feature(Feature::Led).set(!state.feature(Feature::Led).get(false));
      break;
    case Action::SelfClean:
      state.feature(Feature::SelfClean).set(true);
      break;
    default:
      break;
  }
  return true;
}

}  // namespace acbridge
