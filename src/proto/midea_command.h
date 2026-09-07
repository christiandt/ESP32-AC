// Midea AC commands and the state response.
//
// Ported from msmart/device/AC/command.py. Every byte position here is covered
// by a golden vector generated from msmart — see tools/gen_vectors.py.
//
// Pure C++, host-testable.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace proto {

constexpr uint8_t kControlSource = 0x02;  // "app control"
constexpr uint8_t kResponseIdState = 0xC0;

// device.py AC.OperationalMode
enum : uint8_t {
  kModeAuto = 1,
  kModeCool = 2,
  kModeDry = 3,
  kModeHeat = 4,
  kModeFanOnly = 5,
};

// device.py AC.SwingMode
enum : uint8_t {
  kSwingOff = 0x0,
  kSwingHorizontal = 0x3,
  kSwingVertical = 0xC,
  kSwingBoth = 0xF,
};

// device.py AC.FanSpeed — note these are percentages, with 102 meaning auto.
enum : uint8_t {
  kFanSilent = 20,
  kFanLow = 40,
  kFanMedium = 60,
  kFanHigh = 80,
  kFanMax = 100,
  kFanAuto = 102,
};

// Wraps a command body as `body + message_id + crc8` and frames it.
size_t buildCommandFrame(uint8_t* out, size_t cap, uint8_t frame_type, const uint8_t* body,
                         size_t body_len, uint8_t message_id);

// ---------------------------------------------------------------------------
// Properties
//
// A second command family, separate from GetState/SetState. Anything the state
// frame has no bit for lives here instead, addressed by a 16-bit id carrying a
// variable-length value. Ported from command.py's PropertyId,
// GetPropertiesCommand, SetPropertiesCommand and PropertiesResponse.
// ---------------------------------------------------------------------------

constexpr uint8_t kResponseIdPropertiesAck = 0xB0;  // reply to a set
constexpr uint8_t kResponseIdProperties = 0xB1;     // reply to a query

// command.py PropertyId. Only what this firmware actually drives.
constexpr uint16_t kPropOutSilent = 0x00CD;  // "Portasplit outdoor silent mode"

// OUT_SILENT is not a plain boolean: command.py encodes on as 3 and decodes
// with `data[0] == 3`. Writing 1 would read back as off.
constexpr uint8_t kOutSilentOn = 0x03;
constexpr uint8_t kOutSilentOff = 0x00;

// GetPropertiesCommand — a QUERY frame listing the ids to read.
size_t buildGetPropertiesFrame(uint8_t* out, size_t cap, uint8_t message_id,
                               const uint16_t* props, size_t count);

// SetPropertiesCommand — a CONTROL frame carrying one id and its value.
size_t buildSetPropertyFrame(uint8_t* out, size_t cap, uint8_t message_id, uint16_t prop,
                             const uint8_t* value, size_t value_len);

// Finds one property's value in a 0xB0/0xB1 response. Returns false if the
// frame isn't a properties response, if the property is absent or empty, or if
// the device flagged that property as failed.
bool findProperty(const uint8_t* frame, size_t len, uint16_t prop, const uint8_t** value,
                  size_t* value_len);

// GetStateCommand — a QUERY frame.
size_t buildGetStateFrame(uint8_t* out, size_t cap, uint8_t message_id);

// ToggleDisplayCommand. This *flips* the display rather than setting it, and
// (per msmart's comment) uses a QUERY frame type despite being an action.
size_t buildToggleDisplayFrame(uint8_t* out, size_t cap, uint8_t message_id, bool beep);

struct SetState {
  bool beep = true;
  bool power = false;
  float target_temp = 25.0f;
  uint8_t mode = kModeAuto;
  uint8_t fan_speed = kFanAuto;
  uint8_t swing_mode = kSwingOff;
  bool eco = false;
  bool turbo = false;
  bool sleep = false;
  bool fahrenheit = false;
  bool freeze_protection = false;
  bool follow_me = false;
  bool purifier = false;
  uint8_t target_humidity = 40;
};

// SetStateCommand — a CONTROL frame.
size_t buildSetStateFrame(uint8_t* out, size_t cap, const SetState& s, uint8_t message_id);

struct MideaState {
  bool power = false;
  float target_temp = 0;
  uint8_t mode = 0;
  uint8_t fan_speed = 0;
  uint8_t swing_mode = 0;

  bool has_indoor = false;
  float indoor = 0;
  bool has_outdoor = false;
  float outdoor = 0;

  bool turbo = false;
  bool eco = false;
  bool sleep = false;
  bool fahrenheit = false;
  bool follow_me = false;
  bool purifier = false;
  bool aux_heat = false;
  bool filter_alert = false;
  bool display_on = true;

  bool has_humidity = false;
  uint8_t target_humidity = 0;
  bool has_freeze = false;
  bool freeze_protection = false;

  uint8_t error_code = 0;
};

// Parses a complete 0xAA frame. Returns false if the frame is malformed, fails
// its checksum or payload CRC, or isn't a state response.
bool parseStateFrame(const uint8_t* frame, size_t len, MideaState* out);

}  // namespace proto
}  // namespace acbridge
