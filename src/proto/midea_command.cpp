#include "midea_command.h"

#include <math.h>
#include <string.h>

#include "midea_crc8.h"
#include "midea_frame.h"

namespace acbridge {
namespace proto {

namespace {

constexpr size_t kMaxBody = 32;

// Decodes a temperature byte, using the extra precision nibble when present.
// Mirrors StateResponse._parse_temperature, which msmart in turn lifted from
// dudanov/MideaUART.
bool parseTemperature(uint8_t data, float decimals, bool fahrenheit, float* out) {
  if (data == 0xFF) return false;  // unavailable

  const float temperature = static_cast<float>(static_cast<int>(data) - 50) / 2.0f;

  // int() in Python truncates toward zero, and so does a C++ cast.
  const float truncated = static_cast<float>(static_cast<int>(temperature));

  if (!fahrenheit && decimals != 0.0f) {
    *out = truncated + (temperature >= 0 ? decimals : -decimals);
    return true;
  }
  if (decimals >= 0.5f) {
    *out = truncated + (temperature >= 0 ? 0.5f : -0.5f);
    return true;
  }
  *out = temperature;
  return true;
}

}  // namespace

size_t buildCommandFrame(uint8_t* out, size_t cap, uint8_t frame_type, const uint8_t* body,
                         size_t body_len, uint8_t message_id) {
  uint8_t data[kMaxBody + 2];
  if (body_len + 2 > sizeof(data)) return 0;

  memcpy(data, body, body_len);
  data[body_len] = message_id;
  // The CRC covers the body *and* the message id.
  data[body_len + 1] = crc8(data, body_len + 1);

  return buildFrame(out, cap, frame_type, data, body_len + 2);
}

size_t buildGetPropertiesFrame(uint8_t* out, size_t cap, uint8_t message_id,
                               const uint16_t* props, size_t count) {
  // Two header bytes plus two per id, and the body has to stay inside the
  // command-frame budget.
  if (props == nullptr || count == 0 || 2 + count * 2 > kMaxBody) return 0;

  uint8_t body[kMaxBody];
  body[0] = kResponseIdProperties;
  body[1] = static_cast<uint8_t>(count);
  for (size_t i = 0; i < count; i++) {
    body[2 + i * 2] = static_cast<uint8_t>(props[i] & 0xFF);
    body[3 + i * 2] = static_cast<uint8_t>(props[i] >> 8);
  }
  return buildCommandFrame(out, cap, kFrameQuery, body, 2 + count * 2, message_id);
}

size_t buildSetPropertyFrame(uint8_t* out, size_t cap, uint8_t message_id, uint16_t prop,
                             const uint8_t* value, size_t value_len) {
  if (value == nullptr || value_len == 0 || 5 + value_len > kMaxBody) return 0;

  uint8_t body[kMaxBody];
  body[0] = kResponseIdPropertiesAck;
  body[1] = 1;  // one property per frame is all this firmware needs
  body[2] = static_cast<uint8_t>(prop & 0xFF);
  body[3] = static_cast<uint8_t>(prop >> 8);
  body[4] = static_cast<uint8_t>(value_len);
  memcpy(body + 5, value, value_len);
  return buildCommandFrame(out, cap, kFrameControl, body, 5 + value_len, message_id);
}

size_t encodeIEcoValue(uint8_t* out, size_t cap, uint8_t ieco_number, bool on) {
  if (out == nullptr || cap < kIEcoValueLen) return 0;
  memset(out, 0, kIEcoValueLen);
  out[0] = 0;  // ieco_frame, always zero in command.py
  out[1] = ieco_number;
  out[2] = on ? 1 : 0;
  return kIEcoValueLen;
}

bool findProperty(const uint8_t* frame, size_t len, uint16_t prop, const uint8_t** value,
                  size_t* value_len) {
  if (value == nullptr || value_len == nullptr) return false;
  if (!validateFrame(frame, len)) return false;
  if (len < kFrameHeaderLen + 2) return false;

  const uint8_t* payload = frame + kFrameHeaderLen;
  const size_t payload_len = len - kFrameHeaderLen - 2;
  if (payload_len < 2) return false;
  if (payload[0] != kResponseIdProperties && payload[0] != kResponseIdPropertiesAck) return false;

  // No payload CRC check here, unlike parseStateFrame. command.py skips it for
  // this response type too — "except for properties which certain devices send
  // invalid CRCs". The Porta Split is one of them: it sends 0x00. The frame
  // checksum, already verified above, is what actually protects this reply.

  // Entries are id(2) + result(1) + size(1) + value(size). An empty entry still
  // occupies its four header bytes.
  const size_t count = payload[1];
  size_t off = 2;
  for (size_t i = 0; i < count; i++) {
    if (off + 4 > payload_len) return false;
    const uint16_t id = static_cast<uint16_t>(payload[off] | (payload[off + 1] << 8));
    const uint8_t result = payload[off + 2];
    const size_t size = payload[off + 3];
    if (off + 4 + size > payload_len) return false;

    if (id == prop) {
      // Bit 0x10 means the device rejected this property.
      if (size == 0 || (result & 0x10) != 0) return false;
      *value = payload + off + 4;
      *value_len = size;
      return true;
    }
    off += 4 + size;
  }
  return false;
}

size_t buildGetStateFrame(uint8_t* out, size_t cap, uint8_t message_id) {
  const uint8_t body[] = {
      0x41,                                // get state
      0x81, 0x00, 0xFF, 0x03, 0xFF, 0x00,  // unknown
      0x02,                                // TemperatureType.INDOOR
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x03,  // unknown
  };
  return buildCommandFrame(out, cap, kFrameQuery, body, sizeof(body), message_id);
}

size_t buildToggleDisplayFrame(uint8_t* out, size_t cap, uint8_t message_id, bool beep) {
  const uint8_t body[] = {
      0x41,
      static_cast<uint8_t>(kControlSource | (beep ? 0x40 : 0x00)),
      0x00, 0xFF, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  return buildCommandFrame(out, cap, kFrameQuery, body, sizeof(body), message_id);
}

size_t buildSetStateFrame(uint8_t* out, size_t cap, const SetState& s, uint8_t message_id) {
  const uint8_t beep = s.beep ? 0x40 : 0x00;
  const uint8_t power = s.power ? 0x01 : 0x00;

  // Split the setpoint into whole degrees and a half-degree flag.
  float integral_f = 0;
  const float fractional = modff(s.target_temp, &integral_f);
  const int integral = static_cast<int>(integral_f);

  uint8_t temperature;
  uint8_t temperature_alt;
  if (integral >= 17 && integral <= 30) {
    temperature = static_cast<uint8_t>((integral - 16) & 0x0F);
    temperature_alt = 0;
  } else {
    // Outside the primary range the setpoint moves to the alternate byte.
    temperature = 0;
    temperature_alt = static_cast<uint8_t>((integral - 12) & 0x1F);
  }
  if (fractional > 0) temperature |= 0x10;

  const uint8_t mode = static_cast<uint8_t>((s.mode & 0x07) << 5);
  const uint8_t swing = static_cast<uint8_t>(0x30 | (s.swing_mode & 0x3F));

  const uint8_t eco = s.eco ? 0x80 : 0x00;
  const uint8_t purifier = s.purifier ? 0x20 : 0x00;
  const uint8_t sleep = s.sleep ? 0x01 : 0x00;
  const uint8_t turbo = s.turbo ? 0x02 : 0x00;
  const uint8_t fahrenheit = s.fahrenheit ? 0x04 : 0x00;
  // Turbo is signalled in two different bytes.
  const uint8_t turbo_alt = s.turbo ? 0x20 : 0x00;
  const uint8_t follow_me = s.follow_me ? 0x80 : 0x00;
  const uint8_t freeze = s.freeze_protection ? 0x80 : 0x00;

  const uint8_t body[] = {
      0x40,                                                          // set state
      static_cast<uint8_t>(kControlSource | beep | power),            // [1]
      static_cast<uint8_t>(temperature | mode),                       // [2]
      s.fan_speed,                                                    // [3]
      0x7F, 0x7F, 0x00,                                               // [4-6] timer
      swing,                                                          // [7]
      static_cast<uint8_t>(follow_me | turbo_alt),                    // [8]
      static_cast<uint8_t>(eco | purifier),                           // [9]
      static_cast<uint8_t>(sleep | turbo | fahrenheit),               // [10]
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                       // [11-17]
      temperature_alt,                                                // [18]
      static_cast<uint8_t>(s.target_humidity & 0x7F),                 // [19]
      0x00,                                                           // [20]
      freeze,                                                         // [21]
      0x00,                                                           // [22] independent aux heat
      0x00,                                                           // [23]
  };
  return buildCommandFrame(out, cap, kFrameControl, body, sizeof(body), message_id);
}

bool parseStateFrame(const uint8_t* frame, size_t len, MideaState* out) {
  if (out == nullptr) return false;
  if (!validateFrame(frame, len)) return false;

  // frame = header(10) + payload(N) + payload_crc(1) + frame_checksum(1)
  if (len < kFrameHeaderLen + 2) return false;
  const uint8_t* payload = frame + kFrameHeaderLen;
  const size_t payload_len = len - kFrameHeaderLen - 2;

  // Needs at least through the error code at [16].
  if (payload_len < 17) return false;
  if (payload[0] != kResponseIdState) return false;

  // Some devices sign the payload with a CRC8, others with the frame checksum.
  const uint8_t received = frame[len - 2];
  if (crc8(payload, payload_len) != received && frameChecksum(payload, payload_len) != received) {
    return false;
  }

  *out = MideaState{};

  out->power = (payload[1] & 0x01) != 0;

  out->target_temp = static_cast<float>(payload[2] & 0x0F) + 16.0f;
  if (payload[2] & 0x10) out->target_temp += 0.5f;
  out->mode = static_cast<uint8_t>((payload[2] >> 5) & 0x07);

  out->fan_speed = static_cast<uint8_t>(payload[3] & 0x7F);
  out->swing_mode = static_cast<uint8_t>(payload[7] & 0x0F);

  out->turbo = (payload[8] & 0x20) != 0;
  out->follow_me = (payload[8] & 0x80) != 0;

  out->eco = (payload[9] & 0x10) != 0;
  out->purifier = (payload[9] & 0x20) != 0;
  out->aux_heat = (payload[9] & 0x08) != 0;

  out->sleep = (payload[10] & 0x01) != 0;
  out->turbo = out->turbo || (payload[10] & 0x02) != 0;
  out->fahrenheit = (payload[10] & 0x04) != 0;

  out->has_indoor = parseTemperature(payload[11], static_cast<float>(payload[15] & 0x0F) / 10.0f,
                                     out->fahrenheit, &out->indoor);
  out->has_outdoor = parseTemperature(payload[12], static_cast<float>(payload[15] >> 4) / 10.0f,
                                      out->fahrenheit, &out->outdoor);

  // A non-zero alternate byte overrides the primary setpoint.
  const uint8_t target_alt = static_cast<uint8_t>(payload[13] & 0x1F);
  if (target_alt != 0) {
    out->target_temp = static_cast<float>(target_alt + 12);
    if (payload[2] & 0x10) out->target_temp += 0.5f;
  }
  out->filter_alert = (payload[13] & 0x20) != 0;

  out->display_on = payload[14] != 0x70;
  out->error_code = payload[16];

  if (payload_len >= 20) {
    out->has_humidity = true;
    out->target_humidity = static_cast<uint8_t>(payload[19] & 0x7F);
  }
  if (payload_len >= 22) {
    out->has_freeze = true;
    out->freeze_protection = (payload[21] & 0x80) != 0;
  }
  return true;
}

}  // namespace proto
}  // namespace acbridge
