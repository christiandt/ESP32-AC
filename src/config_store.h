// Persistent configuration in NVS.
//
// Everything here is provisioned once from `tools/export_config.py`, which
// reads the credentials the existing Python CLIs already cached on your
// laptop. The ESP32 never talks to the Midea cloud.

#pragma once

#include <stdint.h>

#include "ac_types.h"

namespace acbridge {

constexpr uint16_t kConfigVersion = 2;

struct WifiConfig {
  char ssid[33] = {0};
  char psk[65] = {0};

  bool valid() const { return ssid[0] != '\0'; }
};

struct ApiConfig {
  // Bearer token for the REST API. Generated from esp_random() on first boot
  // and printed to serial, so the device is never unauthenticated on the LAN.
  char bearer[65] = {0};
};

struct MideaConfig {
  bool enabled = false;
  char name[33] = {0};
  char ip[16] = {0};
  // Midea device ids are ~15 digits, well beyond 32 bits, and the protocol
  // packs the id as 8 bytes.
  uint64_t device_id = 0;
  uint16_t port = 6444;
  char token[161] = {0};  // 64 bytes, hex encoded
  char key[97] = {0};     // 32 bytes, hex encoded

  // Capability flags exported from mpsac, one bit per Feature. Baked in so the
  // firmware never has to parse a capabilities response on-device — mpsac
  // already treats capabilities as best-effort (mpsac/device.py:69).
  uint32_t caps = 0;

  float min_target = 16.0f;
  float max_target = 30.0f;

  bool isV3() const { return token[0] != '\0' && key[0] != '\0'; }
};

struct ElectroluxConfig {
  bool enabled = false;
  char name[33] = {0};
  char ip[16] = {0};
};

struct Config {
  uint16_t version = kConfigVersion;
  WifiConfig wifi;
  ApiConfig api;
  MideaConfig midea;
  ElectroluxConfig electrolux;
};

// Helpers for the capability bitmask.
inline bool capsHas(uint32_t caps, Feature f) {
  return (caps & (1u << static_cast<uint32_t>(f))) != 0;
}
inline uint32_t capsSet(uint32_t caps, Feature f, bool on) {
  const uint32_t bit = 1u << static_cast<uint32_t>(f);
  return on ? (caps | bit) : (caps & ~bit);
}

class ConfigStore {
 public:
  // Loads from NVS. Generates and persists a bearer token on first boot.
  // Returns false if nothing was stored yet (caller should provision).
  bool begin();

  Config& get() { return cfg_; }
  const Config& get() const { return cfg_; }

  bool save();
  bool clear();

  bool loaded() const { return loaded_; }

 private:
  Config cfg_;
  bool loaded_ = false;
};

extern ConfigStore g_config;

}  // namespace acbridge
