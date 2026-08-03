// Thin wrappers over mbedtls, which ships with ESP-IDF — no extra dependency.
//
// Both vendors need the same small set: Broadlink is AES-128-CBC throughout,
// and the Midea V3 handshake adds SHA256 and MD5.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace crypto {

// AES-128-CBC. `len` must be a multiple of 16. `in` and `out` may alias.
// The IV is not modified (mbedtls consumes it, so a copy is made internally).
bool aesCbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                   uint8_t* out);
bool aesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                   uint8_t* out);

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void md5(const uint8_t* data, size_t len, uint8_t out[16]);

void xorBytes(const uint8_t* a, const uint8_t* b, size_t len, uint8_t* out);

// Decodes a hex string into bytes. Returns false on odd length, a non-hex
// character, or a buffer that is too small.
bool hexDecode(const char* hex, uint8_t* out, size_t out_cap, size_t* out_len);

}  // namespace crypto
}  // namespace acbridge
