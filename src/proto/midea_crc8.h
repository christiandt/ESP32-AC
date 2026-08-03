// CRC8 used by the Midea appliance command payloads.
//
// Port of msmart/crc8.py. Table-driven, matching that implementation exactly so
// the golden vectors generated from msmart hold.
//
// Pure C++: no Arduino or ESP-IDF includes, so the host-native test env can
// compile it.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace proto {

uint8_t crc8(const uint8_t* data, size_t len);

}  // namespace proto
}  // namespace acbridge
