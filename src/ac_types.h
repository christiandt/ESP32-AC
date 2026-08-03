// Vendor-neutral vocabulary shared by the drivers, the REST layer and the
// HomeKit layer.
//
// This header is deliberately free of Arduino and ESP-IDF includes so it can
// also be compiled by the host-native test environment.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace acbridge {

// ---------------------------------------------------------------------------
// Opt<T> — a value that may be absent.
//
// Two jobs that want the same shape:
//   * In AcState, absent means "this unit doesn't report it" and the REST layer
//     emits null.
//   * In AcCommand, present means "the caller asked for it", so a PATCH applies
//     only the keys it was actually given.
// ---------------------------------------------------------------------------
template <typename T>
struct Opt {
  T value{};
  bool present = false;

  Opt() = default;
  explicit Opt(T v) : value(v), present(true) {}

  void set(T v) {
    value = v;
    present = true;
  }
  void clear() {
    value = T{};
    present = false;
  }
  bool has() const { return present; }
  T get(T fallback = T{}) const { return present ? value : fallback; }
};

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class Vendor : uint8_t { Mock, Midea, Electrolux };

// Both vendors happen to expose exactly this set, so no vendor needs a mode
// that this vocabulary can't express.
enum class Mode : uint8_t { Auto, Cool, Dry, Heat, Fan };

enum class Swing : uint8_t { Off, Vertical, Horizontal, Both };

enum class FanPreset : uint8_t { Auto, Silent, Low, Medium, High, Turbo };

// Boolean extras. Not every unit has every one — AcDriver::supportsFeature()
// gates them, and unsupported ones stay absent in AcState.
enum class Feature : uint8_t {
  Eco,
  IEco,
  Boost,
  Sleep,
  OutSilent,
  Ion,
  Beep,
  Freeze,
  FollowMe,
  Led,
  SelfClean,
  Count,
};

constexpr size_t kFeatureCount = static_cast<size_t>(Feature::Count);

// One-shot operations that aren't a settable state. The Midea unit only exposes
// its display as a *flip* (msmart's toggle_display) rather than a set, and self
// clean is a job you start, not a flag you hold.
enum class Action : uint8_t { None, LedToggle, SelfClean, ClearTimer };

// ---------------------------------------------------------------------------
// Fan speed
//
// A speed is either one of the named presets or a raw percentage. Units with
// custom fan-speed support report the percentage form, which is what broke
// mpsac before its commit 4b1dda0 — so both forms are first-class here rather
// than being flattened into one.
// ---------------------------------------------------------------------------
struct FanSpeed {
  bool is_percent = false;
  FanPreset preset = FanPreset::Auto;
  uint8_t percent = 0;

  static FanSpeed ofPreset(FanPreset p) {
    FanSpeed f;
    f.is_percent = false;
    f.preset = p;
    return f;
  }
  static FanSpeed ofPercent(uint8_t pct) {
    FanSpeed f;
    f.is_percent = true;
    f.percent = pct;
    return f;
  }

  bool operator==(const FanSpeed& o) const {
    if (is_percent != o.is_percent) return false;
    return is_percent ? percent == o.percent : preset == o.preset;
  }
  bool operator!=(const FanSpeed& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Name <-> enum. Table-driven so the REST vocabulary has exactly one
// definition and round-trips by construction.
// ---------------------------------------------------------------------------

namespace detail {

template <typename E>
struct NameEntry {
  E value;
  const char* name;
};

template <typename E, size_t N>
inline const char* nameOf(const NameEntry<E> (&table)[N], E v, const char* fallback) {
  for (size_t i = 0; i < N; i++) {
    if (table[i].value == v) return table[i].name;
  }
  return fallback;
}

template <typename E, size_t N>
inline bool valueOf(const NameEntry<E> (&table)[N], const char* name, E& out) {
  if (name == nullptr) return false;
  for (size_t i = 0; i < N; i++) {
    if (strcmp(table[i].name, name) == 0) {
      out = table[i].value;
      return true;
    }
  }
  return false;
}

inline const NameEntry<Mode> kModes[] = {
    {Mode::Auto, "auto"}, {Mode::Cool, "cool"}, {Mode::Dry, "dry"},
    {Mode::Heat, "heat"}, {Mode::Fan, "fan"},
};

inline const NameEntry<Swing> kSwings[] = {
    {Swing::Off, "off"},
    {Swing::Vertical, "vertical"},
    {Swing::Horizontal, "horizontal"},
    {Swing::Both, "both"},
};

inline const NameEntry<FanPreset> kFanPresets[] = {
    {FanPreset::Auto, "auto"},   {FanPreset::Silent, "silent"},
    {FanPreset::Low, "low"},     {FanPreset::Medium, "medium"},
    {FanPreset::High, "high"},   {FanPreset::Turbo, "turbo"},
};

inline const NameEntry<Feature> kFeatures[] = {
    {Feature::Eco, "eco"},         {Feature::IEco, "ieco"},
    {Feature::Boost, "boost"},     {Feature::Sleep, "sleep"},
    {Feature::OutSilent, "out_silent"}, {Feature::Ion, "ion"},
    {Feature::Beep, "beep"},       {Feature::Freeze, "freeze"},
    {Feature::FollowMe, "follow_me"}, {Feature::Led, "led"},
    {Feature::SelfClean, "self_clean"},
};

inline const NameEntry<Action> kActions[] = {
    {Action::LedToggle, "led_toggle"},
    {Action::SelfClean, "self_clean"},
    {Action::ClearTimer, "clear_timer"},
};

inline const NameEntry<Vendor> kVendors[] = {
    {Vendor::Mock, "mock"},
    {Vendor::Midea, "midea"},
    {Vendor::Electrolux, "electrolux"},
};

}  // namespace detail

inline const char* modeName(Mode v) { return detail::nameOf(detail::kModes, v, "auto"); }
inline bool modeFromName(const char* s, Mode& out) { return detail::valueOf(detail::kModes, s, out); }

inline const char* swingName(Swing v) { return detail::nameOf(detail::kSwings, v, "off"); }
inline bool swingFromName(const char* s, Swing& out) { return detail::valueOf(detail::kSwings, s, out); }

inline const char* fanPresetName(FanPreset v) { return detail::nameOf(detail::kFanPresets, v, "auto"); }
inline bool fanPresetFromName(const char* s, FanPreset& out) {
  return detail::valueOf(detail::kFanPresets, s, out);
}

inline const char* featureName(Feature v) { return detail::nameOf(detail::kFeatures, v, "?"); }
inline bool featureFromName(const char* s, Feature& out) {
  return detail::valueOf(detail::kFeatures, s, out);
}

inline const char* actionName(Action v) { return detail::nameOf(detail::kActions, v, "none"); }
inline bool actionFromName(const char* s, Action& out) {
  return detail::valueOf(detail::kActions, s, out);
}

inline const char* vendorName(Vendor v) { return detail::nameOf(detail::kVendors, v, "mock"); }

// ---------------------------------------------------------------------------
// State and commands
// ---------------------------------------------------------------------------

struct AcState {
  bool online = false;

  Opt<bool> power;
  Opt<Mode> mode;
  Opt<float> target_temp;
  Opt<float> indoor_temp;
  Opt<float> outdoor_temp;
  Opt<uint8_t> humidity;
  Opt<FanSpeed> fan;
  Opt<Swing> swing;
  Opt<bool> features[kFeatureCount];

  // Device-reported setpoint limits. HomeKit needs these to widen its default
  // characteristic ranges, and PATCH validates against them.
  float min_target = 16.0f;
  float max_target = 30.0f;

  uint32_t updated_ms = 0;  // millis() of the last successful poll
  char error[64] = {0};     // last failure, empty when healthy

  Opt<bool>& feature(Feature f) { return features[static_cast<size_t>(f)]; }
  const Opt<bool>& feature(Feature f) const { return features[static_cast<size_t>(f)]; }

  void setError(const char* msg) {
    if (msg == nullptr) {
      error[0] = '\0';
      return;
    }
    strncpy(error, msg, sizeof(error) - 1);
    error[sizeof(error) - 1] = '\0';
  }
  void clearError() { error[0] = '\0'; }
};

struct AcCommand {
  Opt<bool> power;
  Opt<Mode> mode;
  Opt<float> target_temp;
  Opt<FanSpeed> fan;
  Opt<Swing> swing;
  Opt<bool> features[kFeatureCount];
  Action action = Action::None;

  Opt<bool>& feature(Feature f) { return features[static_cast<size_t>(f)]; }
  const Opt<bool>& feature(Feature f) const { return features[static_cast<size_t>(f)]; }

  bool empty() const {
    if (power.has() || mode.has() || target_temp.has() || fan.has() || swing.has()) return false;
    if (action != Action::None) return false;
    for (size_t i = 0; i < kFeatureCount; i++) {
      if (features[i].has()) return false;
    }
    return true;
  }
};

}  // namespace acbridge
