// HomeKit, via HomeSpan.
//
// The device pairs as a *bridge* carrying one accessory per AC. The HAP
// category is device-wide and only sets the icon shown while pairing, so
// Category::AirConditioners would describe the box rather than the units;
// Bridges is what lets each accessory be renamed independently and puts the
// device on the Home app's Hubs & Bridges page. The air-conditioner look and
// controls come from each accessory's HeaterCooler service, not the category.
//
// Threading: HomeSpan is not thread-safe, so everything here runs on the
// Arduino loop task. Reads take a registry snapshot; writes go onto the unit's
// command queue. Neither touches a driver directly.

#pragma once

#include "config_store.h"

namespace acbridge {

// Builds the accessory tree from whatever the registry holds. Call after the
// registry is populated and WiFi is up.
void homekitBegin(const Config& cfg);

// Call from loop(). Drives HomeSpan and refreshes characteristics from state.
void homekitLoop();

bool homekitStarted();

}  // namespace acbridge
