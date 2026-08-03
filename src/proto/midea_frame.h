// Midea appliance frame — the 0xAA envelope that every command and response
// travels in, inside the LAN packet.
//
// Ported from msmart/frame.py. Pure C++, host-testable.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace proto {

constexpr size_t kFrameHeaderLen = 10;
constexpr uint8_t kFrameStart = 0xAA;
constexpr uint8_t kDeviceTypeAc = 0xAC;  // const.py DeviceType.AIR_CONDITIONER

// const.py FrameType
enum : uint8_t {
  kFrameControl = 0x02,
  kFrameQuery = 0x03,
  kFrameReport = 0x04,
};

// (~sum + 1) & 0xFF — a two's-complement checksum, not a CRC.
uint8_t frameChecksum(const uint8_t* data, size_t len);

// Wraps `data` in a frame. Returns the total length, or 0 if `cap` is too small.
size_t buildFrame(uint8_t* out, size_t cap, uint8_t frame_type, const uint8_t* data,
                  size_t data_len);

// Checks length, checksum and device type.
bool validateFrame(const uint8_t* frame, size_t len);

}  // namespace proto
}  // namespace acbridge
