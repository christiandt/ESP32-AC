#include "crypto_util.h"

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace acbridge {
namespace crypto {

namespace {

bool validKeyLen(size_t key_len) { return key_len == 16 || key_len == 24 || key_len == 32; }

bool aesSetup(mbedtls_aes_context* ctx, int mode, const uint8_t* key, size_t key_len) {
  const unsigned bits = static_cast<unsigned>(key_len * 8);
  return (mode == MBEDTLS_AES_ENCRYPT ? mbedtls_aes_setkey_enc(ctx, key, bits)
                                      : mbedtls_aes_setkey_dec(ctx, key, bits)) == 0;
}

bool aesCbc(int mode, const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in,
            size_t len, uint8_t* out) {
  if (!validKeyLen(key_len) || len % 16 != 0) return false;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (!aesSetup(&ctx, mode, key, key_len)) {
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

bool aesEcb(int mode, const uint8_t* key, size_t key_len, const uint8_t* in, size_t len,
            uint8_t* out) {
  if (!validKeyLen(key_len) || len % 16 != 0) return false;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (!aesSetup(&ctx, mode, key, key_len)) {
    mbedtls_aes_free(&ctx);
    return false;
  }

  bool ok = true;
  for (size_t off = 0; off < len && ok; off += 16) {
    ok = mbedtls_aes_crypt_ecb(&ctx, mode, in + off, out + off) == 0;
  }
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

bool aesCbcEncrypt(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in,
                   size_t len, uint8_t* out) {
  return aesCbc(MBEDTLS_AES_ENCRYPT, key, key_len, iv, in, len, out);
}

bool aesCbcDecrypt(const uint8_t* key, size_t key_len, const uint8_t iv[16], const uint8_t* in,
                   size_t len, uint8_t* out) {
  return aesCbc(MBEDTLS_AES_DECRYPT, key, key_len, iv, in, len, out);
}

bool aesEcbEncrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len,
                   uint8_t* out) {
  return aesEcb(MBEDTLS_AES_ENCRYPT, key, key_len, in, len, out);
}

bool aesEcbDecrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len,
                   uint8_t* out) {
  return aesEcb(MBEDTLS_AES_DECRYPT, key, key_len, in, len, out);
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  mbedtls_sha256(data, len, out, /*is224=*/0);
}

void md5(const uint8_t* data, size_t len, uint8_t out[16]) { mbedtls_md5(data, len, out); }

void sha256Pair(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, /*is224=*/0);
  mbedtls_sha256_update(&ctx, a, a_len);
  mbedtls_sha256_update(&ctx, b, b_len);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

void md5Pair(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len, uint8_t out[16]) {
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, a, a_len);
  mbedtls_md5_update(&ctx, b, b_len);
  mbedtls_md5_finish(&ctx, out);
  mbedtls_md5_free(&ctx);
}

void xorBytes(const uint8_t* a, const uint8_t* b, size_t len, uint8_t* out) {
  for (size_t i = 0; i < len; i++) out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

bool equals(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

size_t pkcs7Pad(uint8_t* buf, size_t len, size_t cap) {
  const size_t pad = 16 - (len % 16);  // always 1..16, never 0
  if (len + pad > cap) return 0;
  memset(buf + len, static_cast<int>(pad), pad);
  return len + pad;
}

size_t pkcs7Unpad(const uint8_t* buf, size_t len) {
  if (len == 0 || len % 16 != 0) return 0;
  const uint8_t pad = buf[len - 1];
  if (pad == 0 || pad > 16 || pad > len) return 0;
  for (size_t i = len - pad; i < len; i++) {
    if (buf[i] != pad) return 0;
  }
  return len - pad;
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
