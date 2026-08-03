#include "crypto_util.h"

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace acbridge {
namespace crypto {

namespace {

bool aesCbc(int mode, const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
            uint8_t* out) {
  if (len % 16 != 0) return false;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);

  const int rc = (mode == MBEDTLS_AES_ENCRYPT) ? mbedtls_aes_setkey_enc(&ctx, key, 128)
                                               : mbedtls_aes_setkey_dec(&ctx, key, 128);
  if (rc != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }

  // mbedtls advances the IV in place, so work on a copy and leave the caller's
  // constant IV alone.
  uint8_t iv_copy[16];
  memcpy(iv_copy, iv, sizeof(iv_copy));

  const bool ok = mbedtls_aes_crypt_cbc(&ctx, mode, len, iv_copy, in, out) == 0;
  mbedtls_aes_free(&ctx);
  return ok;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool aesCbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                   uint8_t* out) {
  return aesCbc(MBEDTLS_AES_ENCRYPT, key, iv, in, len, out);
}

bool aesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                   uint8_t* out) {
  return aesCbc(MBEDTLS_AES_DECRYPT, key, iv, in, len, out);
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  mbedtls_sha256(data, len, out, /*is224=*/0);
}

void md5(const uint8_t* data, size_t len, uint8_t out[16]) { mbedtls_md5(data, len, out); }

void xorBytes(const uint8_t* a, const uint8_t* b, size_t len, uint8_t* out) {
  for (size_t i = 0; i < len; i++) out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

bool hexDecode(const char* hex, uint8_t* out, size_t out_cap, size_t* out_len) {
  if (hex == nullptr) return false;
  const size_t chars = strlen(hex);
  if (chars % 2 != 0) return false;

  const size_t bytes = chars / 2;
  if (bytes > out_cap) return false;

  for (size_t i = 0; i < bytes; i++) {
    const int hi = hexNibble(hex[i * 2]);
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  if (out_len != nullptr) *out_len = bytes;
  return true;
}

}  // namespace crypto
}  // namespace acbridge
