// Midea LAN packet framing — the two envelopes that wrap an appliance frame.
//
// V2 (inner, always present): 40-byte header + AES-ECB encrypted frame +
//   16-byte MD5 signature.
// V3 (outer, on token/key devices): 6-byte header + AES-CBC encrypted payload +
//   32-byte SHA256 signature.
//
// Ported from msmart/lan.py (_Packet, _LanProtocolV3). Crypto stays with the
// caller so this layer is pure C++ and host-testable.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace proto {

// -- V2 packet --------------------------------------------------------------

constexpr size_t kV2HeaderLen = 40;
constexpr size_t kV2SignLen = 16;

// The fixed key msmart signs V2 packets with. The AES-ECB key used for the
// payload is MD5 of this.
extern const char kMideaSignKey[];
constexpr size_t kMideaSignKeyLen = 36;

// Writes the 40-byte header. `cipher_len` is the length of the encrypted frame
// that will follow. `timestamp` is the 8-byte block described in
// _Packet._timestamp; the device does not appear to validate it.
void buildV2Header(uint8_t out[kV2HeaderLen], uint64_t device_id, size_t cipher_len,
                   const uint8_t timestamp[8]);

struct V2Layout {
  size_t total_len = 0;    // the declared packet length
  size_t cipher_off = 0;   // offset of the encrypted frame
  size_t cipher_len = 0;   // its length
  size_t signed_len = 0;   // bytes covered by the MD5 (everything before the sign)
  size_t sign_off = 0;     // offset of the received 16-byte MD5
};

// Locates the parts of a received V2 packet. Does not verify the signature —
// the caller has the crypto.
bool parseV2(const uint8_t* packet, size_t len, V2Layout* out);

// -- V3 packet --------------------------------------------------------------

constexpr size_t kV3HeaderLen = 6;
constexpr size_t kV3SignLen = 32;

enum : uint8_t {
  kV3HandshakeRequest = 0x0,
  kV3HandshakeResponse = 0x1,
  kV3EncryptedResponse = 0x3,
  kV3EncryptedRequest = 0x6,
  kV3Error = 0xF,
};

// Padding needed so that (2-byte packet id + data + pad) is block-aligned.
size_t v3PadFor(size_t data_len);

// `length` is the size field: data + pad + sign for an encrypted request, or
// just the data length for a handshake.
void buildV3Header(uint8_t out[kV3HeaderLen], uint16_t length, uint8_t pad, uint8_t type);

struct V3Header {
  uint16_t size_field = 0;
  size_t total_len = 0;  // size_field + 8: the 6-byte header plus the 2-byte packet id
  uint8_t pad = 0;
  uint8_t type = 0;
};

// Reads and sanity-checks a V3 header (start bytes and the 0x20 magic byte).
// Does not require the whole packet to be present — total_len tells the caller
// how much more to read.
bool parseV3Header(const uint8_t* packet, size_t len, V3Header* out);

}  // namespace proto
}  // namespace acbridge
