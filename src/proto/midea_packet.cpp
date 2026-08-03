#include "midea_packet.h"

#include <string.h>

namespace acbridge {
namespace proto {

const char kMideaSignKey[] = "xhdiwjnchekd4d512chdjx5d8e4c394D2D7S";

void buildV2Header(uint8_t out[kV2HeaderLen], uint64_t device_id, size_t cipher_len,
                   const uint8_t timestamp[8]) {
  memset(out, 0, kV2HeaderLen);

  out[0] = 0x5A;
  out[1] = 0x5A;
  out[2] = 0x01;  // message type 0x0111
  out[3] = 0x11;

  const uint16_t length = static_cast<uint16_t>(kV2HeaderLen + cipher_len + kV2SignLen);
  out[4] = static_cast<uint8_t>(length & 0xFF);
  out[5] = static_cast<uint8_t>((length >> 8) & 0xFF);

  out[6] = 0x20;  // magic
  out[7] = 0x00;

  // [8..11] message id stays zero, matching msmart.
  if (timestamp != nullptr) memcpy(out + 12, timestamp, 8);

  for (size_t i = 0; i < 8; i++) {
    out[20 + i] = static_cast<uint8_t>((device_id >> (8 * i)) & 0xFF);
  }
  // [28..39] stay zero.
}

bool parseV2(const uint8_t* packet, size_t len, V2Layout* out) {
  if (packet == nullptr || out == nullptr || len < 6) return false;
  if (packet[0] != 0x5A || packet[1] != 0x5A) return false;

  const size_t declared = static_cast<size_t>(packet[4]) | (static_cast<size_t>(packet[5]) << 8);
  if (declared < kV2HeaderLen + kV2SignLen) return false;
  if (len < declared) return false;

  out->total_len = declared;
  out->cipher_off = kV2HeaderLen;
  out->cipher_len = declared - kV2HeaderLen - kV2SignLen;
  out->signed_len = declared - kV2SignLen;
  out->sign_off = declared - kV2SignLen;
  return true;
}

size_t v3PadFor(size_t data_len) {
  // The 2-byte packet id counts toward alignment but not toward the size field.
  const size_t remainder = (data_len + 2) % 16;
  return remainder != 0 ? 16 - remainder : 0;
}

void buildV3Header(uint8_t out[kV3HeaderLen], uint16_t length, uint8_t pad, uint8_t type) {
  out[0] = 0x83;
  out[1] = 0x70;
  // Big endian, unlike the V2 header's little-endian length.
  out[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(length & 0xFF);
  out[4] = 0x20;
  out[5] = static_cast<uint8_t>((pad << 4) | (type & 0x0F));
}

bool parseV3Header(const uint8_t* packet, size_t len, V3Header* out) {
  if (packet == nullptr || out == nullptr || len < kV3HeaderLen) return false;
  if (packet[0] != 0x83 || packet[1] != 0x70) return false;
  if (packet[4] != 0x20) return false;

  out->size_field = static_cast<uint16_t>((static_cast<uint16_t>(packet[2]) << 8) | packet[3]);
  out->total_len = static_cast<size_t>(out->size_field) + 8;
  out->pad = static_cast<uint8_t>(packet[5] >> 4);
  out->type = static_cast<uint8_t>(packet[5] & 0x0F);
  return true;
}

}  // namespace proto
}  // namespace acbridge
