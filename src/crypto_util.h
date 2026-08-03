// Thin wrappers over mbedtls, which ships with ESP-IDF — no extra dependency.
//
// Between them the two vendors need: AES-CBC (Broadlink at 128-bit, Midea's V3
// session at whatever the cloud key length implies — usually 256), AES-ECB for
// the Midea V2 payload, plus SHA256 and MD5.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace crypto {

// AES-CBC. `key_len` must be 16, 24 or 32; `len` must be a multiple of 16.
// The caller's IV is not modified.
bool aesCbcEncrypt(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in,
                   size_t len, uint8_t* out);
bool aesCbcDecrypt(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in,
                   size_t len, uint8_t* out);

// AES-ECB over `len` bytes, which must be a multiple of 16.
bool aesEcbEncrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len, uint8_t* out);
bool aesEcbDecrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len, uint8_t* out);

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void md5(const uint8_t* data, size_t len, uint8_t out[16]);

// Two-part variants, so a signature over header+payload doesn't need the two
// copied into one buffer first.
void sha256Pair(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len, uint8_t out[32]);
void md5Pair(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len, uint8_t out[16]);

void xorBytes(const uint8_t* a, const uint8_t* b, size_t len, uint8_t* out);

// Constant-time compare, for signature checks.
bool equals(const uint8_t* a, const uint8_t* b, size_t len);

// PKCS7. `pad` appends in place and returns the new length, or 0 if it would
// overflow `cap`. `unpad` returns the trimmed length, or 0 if the padding is
// malformed.
size_t pkcs7Pad(uint8_t* buf, size_t len, size_t cap);
size_t pkcs7Unpad(const uint8_t* buf, size_t len);

bool hexDecode(const char* hex, uint8_t* out, size_t out_cap, size_t* out_len);

}  // namespace crypto
}  // namespace acbridge
