#include "selftest.h"

#include <Arduino.h>
#include <string.h>

#include "crypto_util.h"
#include "drivers/midea.h"
#include "proto/broadlink_packet.h"
#include "proto/midea_packet.h"

namespace acbridge {

namespace {

SelfTestResult g_last;

size_t unhex(const char* hex, uint8_t* out, size_t cap) {
  size_t n = 0;
  crypto::hexDecode(hex, out, cap, &n);
  return n;
}

}  // namespace

bool MideaDriver::selfTest(char* err, size_t err_len) {
  // A V2 packet built by msmart for the GetState frame below, with the same
  // device id and the same fixed timestamp this firmware sends. Covers
  // AES-ECB, PKCS7 padding, the header layout and the MD5 signature in one go.
  static const char kFrameHex[] =
      "aa21ac00000000000003418100ff03ff0002000000000000000000000000030169fe";
  static const char kPacketHex[] =
      "5a5a011168002000000000000000000c01011914896745230100000000000000000000000000000065cd8728"
      "fc6a5d31e6464235e81fa3b3e10c552981b23022cb71ea0fc54dc25e29ff4bed361a3a2fbe593a9d1a721035"
      "9ca2748936eefa0fe46f6e1884e80085";

  uint8_t frame[64];
  uint8_t expected[160];
  const size_t frame_len = unhex(kFrameHex, frame, sizeof(frame));
  const size_t expected_len = unhex(kPacketHex, expected, sizeof(expected));
  if (frame_len == 0 || expected_len == 0) {
    snprintf(err, err_len, "self-test vectors failed to decode");
    return false;
  }

  MideaConfig cfg{};
  cfg.device_id = 0x0123456789ULL;  // 8-byte id, as real units have
  cfg.port = 6444;

  // ~2 KB of buffers; keep it off the caller's stack.
  auto* drv = new MideaDriver(cfg);
  uint8_t actual[192];
  size_t actual_len = 0;
  const bool encoded = drv->encodeV2(frame, frame_len, actual, sizeof(actual), &actual_len);

  bool ok = encoded && actual_len == expected_len && memcmp(actual, expected, expected_len) == 0;
  if (!ok) {
    snprintf(err, err_len, "V2 encode mismatch (len %u vs %u)", static_cast<unsigned>(actual_len),
             static_cast<unsigned>(expected_len));
    delete drv;
    return false;
  }

  // And back the other way.
  uint8_t decoded[128];
  size_t decoded_len = 0;
  ok = drv->decodeV2(expected, expected_len, decoded, sizeof(decoded), &decoded_len) &&
       decoded_len == frame_len && memcmp(decoded, frame, frame_len) == 0;
  delete drv;

  if (!ok) {
    snprintf(err, err_len, "V2 decode mismatch");
    return false;
  }
  return true;
}

SelfTestResult runSelfTest() {
  SelfTestResult result;
  auto fail = [&result](const char* msg) {
    result.passed = false;
    strncpy(result.detail, msg, sizeof(result.detail) - 1);
    result.detail[sizeof(result.detail) - 1] = '\0';
    Serial.printf("selftest: FAILED — %s\n", result.detail);
    g_last = result;
    return result;
  };

  // 1. MD5 of the Midea sign key, which is the AES-ECB key for the V2 layer.
  {
    uint8_t digest[16];
    crypto::md5(reinterpret_cast<const uint8_t*>(proto::kMideaSignKey), proto::kMideaSignKeyLen,
                digest);
    uint8_t expected[16];
    unhex("6a92ef406bad2f0359baad994171ea6d", expected, sizeof(expected));
    if (!crypto::equals(digest, expected, sizeof(expected))) return fail("MD5 of sign key");
  }

  // 2. PKCS7, which the V2 layer depends on.
  {
    uint8_t buf[32] = {1, 2, 3};
    const size_t padded = crypto::pkcs7Pad(buf, 3, sizeof(buf));
    if (padded != 16 || buf[15] != 13) return fail("PKCS7 pad");
    if (crypto::pkcs7Unpad(buf, padded) != 3) return fail("PKCS7 unpad");
    // An exact multiple must gain a whole block.
    uint8_t full[32] = {0};
    if (crypto::pkcs7Pad(full, 16, sizeof(full)) != 32) return fail("PKCS7 pad of full block");
    // Malformed padding must be rejected.
    uint8_t bad[16] = {0};
    bad[15] = 0x20;
    if (crypto::pkcs7Unpad(bad, 16) != 0) return fail("PKCS7 unpad accepted bad padding");
  }

  // 3. AES-128-CBC against the Broadlink bootstrap key and IV.
  {
    const uint8_t plain[16] = {0};
    uint8_t cipher[16];
    uint8_t back[16];
    if (!crypto::aesCbcEncrypt(proto::kInitKey, 16, proto::kInitVect, plain, 16, cipher)) {
      return fail("AES-128-CBC encrypt");
    }
    if (!crypto::aesCbcDecrypt(proto::kInitKey, 16, proto::kInitVect, cipher, 16, back)) {
      return fail("AES-128-CBC decrypt");
    }
    if (!crypto::equals(plain, back, 16)) return fail("AES-128-CBC roundtrip");
    // The IV must survive: mbedtls advances it in place, so a second encrypt
    // with the same inputs has to produce the same bytes.
    uint8_t again[16];
    crypto::aesCbcEncrypt(proto::kInitKey, 16, proto::kInitVect, plain, 16, again);
    if (!crypto::equals(cipher, again, 16)) return fail("AES-CBC mutated the caller's IV");
  }

  // 4. The V3 handshake: derive a session key from a reply built by msmart's
  //    own crypto and check it against the expected XOR result.
  {
    uint8_t cloud_key[32];
    uint8_t response[64];
    uint8_t expected[32];
    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", cloud_key,
          sizeof(cloud_key));
    unhex(
        "dc9f01fd76b87c0d0f4be8850b0bece0444c933419e85dcf3cfdf552ec2bc09e"
        "00e988677eecf94c0bb9233371c7c0d6f4db8ebdcdecb7c5ebaa666f17249227",
        response, sizeof(response));
    unhex("a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0", expected,
          sizeof(expected));

    uint8_t local_key[48];
    size_t local_key_len = 0;
    if (!MideaDriver::deriveLocalKey(response, sizeof(response), cloud_key, sizeof(cloud_key),
                                     local_key, &local_key_len)) {
      return fail("V3 handshake key derivation");
    }
    if (local_key_len != 32 || !crypto::equals(local_key, expected, 32)) {
      return fail("V3 session key mismatch");
    }

    // A tampered digest must be rejected.
    uint8_t tampered[64];
    memcpy(tampered, response, sizeof(tampered));
    tampered[40] ^= 0xFF;
    if (MideaDriver::deriveLocalKey(tampered, sizeof(tampered), cloud_key, sizeof(cloud_key),
                                    local_key, &local_key_len)) {
      return fail("V3 handshake accepted a bad digest");
    }
  }

  // 5. The full Midea V2 encode/decode path against a msmart-built packet.
  {
    char err[96] = {0};
    if (!MideaDriver::selfTest(err, sizeof(err))) return fail(err);
  }

  result.passed = true;
  strncpy(result.detail, "all checks passed", sizeof(result.detail) - 1);
  Serial.println("selftest: all checks passed");
  g_last = result;
  return result;
}

const SelfTestResult& lastSelfTest() { return g_last; }

}  // namespace acbridge
