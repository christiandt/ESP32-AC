// Golden-vector tests for the Broadlink / Electrolux framing.
//
// Every expected value here was produced by running the *reference* Python
// (electrolux/cli.py::_send and python-broadlink's send_packet/scan) — not by
// running this C++ — so a mistake in the port fails the test instead of being
// baked into the fixture. See tools/gen_vectors.py to regenerate.

#include <string.h>
#include <unity.h>

#include "proto/broadlink_packet.h"

using namespace acbridge::proto;

namespace {

// Parses "0e00a5a5..." into bytes. Returns the length.
size_t unhex(const char* hex, uint8_t* out, size_t cap) {
  const size_t n = strlen(hex) / 2;
  if (n > cap) return 0;
  for (size_t i = 0; i < n; i++) {
    auto nib = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
      return static_cast<uint8_t>(c - 'A' + 10);
    };
    out[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
  }
  return n;
}

void assertFrameMatches(uint16_t command, const char* json, const char* expected_hex) {
  uint8_t expected[128];
  const size_t expected_len = unhex(expected_hex, expected, sizeof(expected));
  TEST_ASSERT_NOT_EQUAL(0, expected_len);

  uint8_t actual[128];
  const size_t actual_len = buildEluxFrame(actual, sizeof(actual), command, json, strlen(json));

  TEST_ASSERT_EQUAL_size_t(expected_len, actual_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, expected_len);
}

}  // namespace

// -- Electrolux inner frame --------------------------------------------------

void test_elux_status_frame(void) {
  // The one-byte-slice resize in the Python original makes the header 14 bytes,
  // not 13. This vector pins that down: payload "{}" starts at offset 0x0E.
  assertFrameMatches(kEluxCmdStatus, "{}", "0e00a5a55a5ab3c1010b020000007b7d");
}

void test_elux_temp_frame(void) {
  assertFrameMatches(kEluxCmdTemp, "{\"temp\":22}",
                     "1700a5a55a5a55c4020b0b0000007b2274656d70223a32327d");
}

void test_elux_mode_frame(void) {
  assertFrameMatches(kEluxCmdMode, "{\"ac_mode\":0}",
                     "1900a5a55a5a35c5020b0d0000007b2261635f6d6f6465223a307d");
}

void test_elux_short_payload_flag(void) {
  // packet[0x08] is 0x01 for payloads of 2 bytes or fewer, 0x02 otherwise.
  uint8_t buf[64];
  buildEluxFrame(buf, sizeof(buf), kEluxCmdStatus, "{}", 2);
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[0x08]);
  buildEluxFrame(buf, sizeof(buf), kEluxCmdTemp, "{\"temp\":22}", 11);
  TEST_ASSERT_EQUAL_UINT8(0x02, buf[0x08]);
}

void test_elux_frame_rejects_small_buffer(void) {
  uint8_t buf[8];
  TEST_ASSERT_EQUAL_size_t(0, buildEluxFrame(buf, sizeof(buf), kEluxCmdStatus, "{}", 2));
}

void test_elux_roundtrip(void) {
  uint8_t frame[128];
  const char* json = "{\"ac_mode\":0}";
  const size_t len = buildEluxFrame(frame, sizeof(frame), kEluxCmdMode, json, strlen(json));

  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  TEST_ASSERT_TRUE(parseEluxFrame(frame, len, &payload, &payload_len));
  TEST_ASSERT_EQUAL_size_t(strlen(json), payload_len);
  TEST_ASSERT_EQUAL_MEMORY(json, payload, payload_len);
}

void test_elux_parse_rejects_bad_checksum(void) {
  uint8_t frame[128];
  const size_t len = buildEluxFrame(frame, sizeof(frame), kEluxCmdStatus, "{}", 2);
  frame[0x0E] ^= 0xFF;  // corrupt the payload, leave the checksum

  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  TEST_ASSERT_FALSE(parseEluxFrame(frame, len, &payload, &payload_len));
}

void test_elux_parse_rejects_truncated(void) {
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  uint8_t frame[8] = {0};
  TEST_ASSERT_FALSE(parseEluxFrame(frame, sizeof(frame), &payload, &payload_len));
}

// -- Broadlink outer packet --------------------------------------------------

void test_outer_header(void) {
  const uint8_t mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

  uint8_t payload[128];
  const size_t payload_len = buildEluxFrame(payload, sizeof(payload), kEluxCmdStatus, "{}", 2);

  uint8_t header[kOuterHeaderLen];
  buildOuterHeader(header, 0x4f9b, 0x6a, 0x8001, mac, 0x12345678, payload, payload_len);

  uint8_t expected[kOuterHeaderLen];
  unhex(
      "5aa5aa555aa5aa5500000000000000000000000000000000000000000000000000000000"
      "9b4f6a000180ffeeddccbbaa7856341235c30000",
      expected, sizeof(expected));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, header, kOuterHeaderLen);
}

void test_outer_final_checksum(void) {
  const uint8_t mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

  uint8_t payload[128];
  const size_t payload_len = buildEluxFrame(payload, sizeof(payload), kEluxCmdStatus, "{}", 2);
  const size_t padded = paddedLen(payload_len);

  uint8_t packet[kOuterHeaderLen + 128];
  memset(packet, 0, sizeof(packet));
  buildOuterHeader(packet, 0x4f9b, 0x6a, 0x8001, mac, 0x12345678, payload, payload_len);
  // Stand in the plaintext for the ciphertext: the checksum doesn't care what
  // the bytes mean, only that it covers the padded payload.
  memcpy(packet + kOuterHeaderLen, payload, payload_len);
  finalizeOuter(packet, kOuterHeaderLen + padded);

  TEST_ASSERT_EQUAL_size_t(16, padded);
  TEST_ASSERT_EQUAL_UINT8(0x0D, packet[0x20]);
  TEST_ASSERT_EQUAL_UINT8(0xD0, packet[0x21]);  // 0xD00D, little endian
}

void test_next_count_keeps_high_bit(void) {
  TEST_ASSERT_EQUAL_UINT16(0x8001, nextCount(0x0000));
  TEST_ASSERT_EQUAL_UINT16(0x8002, nextCount(0x8001));
  TEST_ASSERT_EQUAL_UINT16(0x8000, nextCount(0xFFFF));
}

void test_padded_len(void) {
  TEST_ASSERT_EQUAL_size_t(0, paddedLen(0));
  TEST_ASSERT_EQUAL_size_t(16, paddedLen(1));
  TEST_ASSERT_EQUAL_size_t(16, paddedLen(16));
  TEST_ASSERT_EQUAL_size_t(32, paddedLen(17));
}

// -- Discovery ---------------------------------------------------------------

void test_discovery_packet(void) {
  const uint8_t ip[4] = {192, 168, 1, 5};
  uint8_t packet[kDiscoveryLen];
  const uint8_t zero_dt[12] = {0};
  buildDiscovery(packet, ip, 12345, zero_dt);

  uint8_t expected[kDiscoveryLen];
  unhex(
      "0000000000000000000000000000000000000000000000000501a8c0393000008cc00000"
      "000006000000000000000000",
      expected, sizeof(expected));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, packet, kDiscoveryLen);
}

void test_discovery_response_parsing(void) {
  uint8_t resp[0x80] = {0};
  resp[0x34] = 0x9b;
  resp[0x35] = 0x4f;
  // mac is stored reversed at 0x3A..0x3F
  const uint8_t stored[6] = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa};
  memcpy(resp + 0x3A, stored, 6);
  resp[0x7F] = 1;

  DiscoveryInfo info;
  TEST_ASSERT_TRUE(parseDiscovery(resp, sizeof(resp), &info));
  TEST_ASSERT_EQUAL_UINT16(0x4f9b, info.devtype);

  const uint8_t want_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want_mac, info.mac, 6);
  TEST_ASSERT_TRUE(info.is_locked);
}

void test_discovery_rejects_short_response(void) {
  uint8_t resp[0x40] = {0};
  DiscoveryInfo info;
  TEST_ASSERT_FALSE(parseDiscovery(resp, sizeof(resp), &info));
}

// The mac round-trips: what buildOuterHeader writes is what parseDiscovery read.
void test_mac_byte_order_roundtrip(void) {
  uint8_t resp[0x80] = {0};
  const uint8_t stored[6] = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa};
  memcpy(resp + 0x3A, stored, 6);

  DiscoveryInfo info;
  parseDiscovery(resp, sizeof(resp), &info);

  uint8_t header[kOuterHeaderLen];
  const uint8_t payload[2] = {0x7b, 0x7d};
  buildOuterHeader(header, 0, 0, 0, info.mac, 0, payload, sizeof(payload));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(stored, header + 0x2A, 6);
}
