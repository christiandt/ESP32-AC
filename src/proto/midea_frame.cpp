#include "midea_frame.h"

#include <string.h>

namespace acbridge {
namespace proto {

uint8_t frameChecksum(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return static_cast<uint8_t>((~sum + 1) & 0xFF);
}

size_t buildFrame(uint8_t* out, size_t cap, uint8_t frame_type, const uint8_t* data,
                  size_t data_len) {
  const size_t total = kFrameHeaderLen + data_len + 1;
  if (out == nullptr || cap < total) return 0;
  // The length field is a single byte, so an oversized frame can't be expressed.
  if (data_len + kFrameHeaderLen > 0xFF) return 0;

  memset(out, 0, kFrameHeaderLen);
  out[0] = kFrameStart;
  out[1] = static_cast<uint8_t>(data_len + kFrameHeaderLen);
  out[2] = kDeviceTypeAc;
  out[8] = 0x00;  // protocol version
  out[9] = frame_type;

  memcpy(out + kFrameHeaderLen, data, data_len);

  // Checksum covers everything after the start byte, excluding itself.
  out[total - 1] = frameChecksum(out + 1, total - 2);
  return total;
}

bool validateFrame(const uint8_t* frame, size_t len) {
  if (frame == nullptr || len < kFrameHeaderLen + 1) return false;
  if (frame[0] != kFrameStart) return false;
  if (frame[2] != kDeviceTypeAc) return false;
  return frameChecksum(frame + 1, len - 2) == frame[len - 1];
}

}  // namespace proto
}  // namespace acbridge
