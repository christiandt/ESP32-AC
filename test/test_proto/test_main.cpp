// Host-native tests for the pure protocol code under src/proto/.
//
//   pio test -e native
//
// Expected values come from msmart (see tools/gen_vectors.py) rather than from
// this implementation, so a mistake in the port shows up as a failure instead
// of being baked into the fixture.

#include <unity.h>

#include "proto/midea_crc8.h"

using acbridge::proto::crc8;

void test_crc8_empty(void) { TEST_ASSERT_EQUAL_UINT8(0x00, crc8(nullptr, 0)); }

void test_crc8_single_bytes(void) {
  // Directly the table entries: crc8([n]) == table[n].
  const uint8_t zero[] = {0x00};
  const uint8_t one[] = {0x01};
  const uint8_t ff[] = {0xFF};
  TEST_ASSERT_EQUAL_UINT8(0x00, crc8(zero, 1));
  TEST_ASSERT_EQUAL_UINT8(0x5E, crc8(one, 1));
  TEST_ASSERT_EQUAL_UINT8(0x35, crc8(ff, 1));
}

void test_crc8_query_payload(void) {
  // The 21-byte GetState query payload from msmart's command.py.
  const uint8_t query[] = {0x41, 0x81, 0x00, 0xFF, 0x03, 0xFF, 0x00, 0x02,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8(0x2F, crc8(query, sizeof(query)));

  // A single changed byte must change the CRC.
  uint8_t mutated[sizeof(query)];
  for (size_t i = 0; i < sizeof(query); i++) mutated[i] = query[i];
  mutated[2] = 0x01;
  TEST_ASSERT_EQUAL_UINT8(0xF6, crc8(mutated, sizeof(mutated)));
}

// The table must be a permutation of 0..255. A single mistyped entry — the most
// likely porting error in 256 hand-copied hex values — breaks this.
void test_crc8_table_is_a_permutation(void) {
  bool seen[256] = {false};
  for (int i = 0; i < 256; i++) {
    const uint8_t b = static_cast<uint8_t>(i);
    const uint8_t v = crc8(&b, 1);  // crc8([n]) == table[n]
    TEST_ASSERT_FALSE_MESSAGE(seen[v], "duplicate value in CRC8 table");
    seen[v] = true;
  }
}

// Defined in test_broadlink.cpp. Unity has no auto-registration, so every test
// has to be named here or it silently never runs.
void test_elux_status_frame(void);
void test_elux_temp_frame(void);
void test_elux_mode_frame(void);
void test_elux_short_payload_flag(void);
void test_elux_frame_rejects_small_buffer(void);
void test_elux_roundtrip(void);
void test_elux_parse_rejects_bad_checksum(void);
void test_elux_parse_rejects_truncated(void);
void test_outer_header(void);
void test_outer_final_checksum(void);
void test_next_count_keeps_high_bit(void);
void test_padded_len(void);
void test_discovery_packet(void);
void test_discovery_response_parsing(void);
void test_discovery_rejects_short_response(void);
void test_mac_byte_order_roundtrip(void);

// Defined in test_midea.cpp.
void test_frame_checksum(void);
void test_frame_roundtrip_validates(void);
void test_frame_rejects_corruption(void);
void test_get_state_frame(void);
void test_toggle_display_frame(void);
void test_set_state_cool_22(void);
void test_set_state_heat_25_5_all_flags(void);
void test_set_state_uses_alternate_temperature(void);
void test_command_frame_rejects_small_buffer(void);
void test_parse_state_frame(void);
void test_parse_state_rejects_bad_payload_crc(void);
void test_parse_state_rejects_wrong_response_id(void);
void test_parse_state_rejects_truncated(void);
void test_get_out_silent_frame(void);
void test_set_out_silent_on(void);
void test_set_out_silent_off(void);
void test_find_property_reads_out_silent(void);
void test_find_property_rejects_absent_and_failed(void);
void test_property_frames_reject_small_buffer(void);
void test_find_property_accepts_real_device_reply(void);
void test_set_self_clean_frames(void);
void test_set_ieco_frames(void);
void test_get_all_properties_frame(void);
void test_find_property_walks_multiple_entries(void);
void test_v2_header(void);
void test_v2_parse(void);
void test_v2_parse_rejects_truncated_and_bad_start(void);
void test_v3_pad_for(void);
void test_v3_encrypted_request_header(void);
void test_v3_handshake_request_header(void);
void test_v3_header_is_big_endian(void);
void test_v3_parse_header(void);
void test_v3_parse_header_rejects_bad_magic(void);

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_crc8_empty);
  RUN_TEST(test_crc8_single_bytes);
  RUN_TEST(test_crc8_query_payload);
  RUN_TEST(test_crc8_table_is_a_permutation);

  RUN_TEST(test_elux_status_frame);
  RUN_TEST(test_elux_temp_frame);
  RUN_TEST(test_elux_mode_frame);
  RUN_TEST(test_elux_short_payload_flag);
  RUN_TEST(test_elux_frame_rejects_small_buffer);
  RUN_TEST(test_elux_roundtrip);
  RUN_TEST(test_elux_parse_rejects_bad_checksum);
  RUN_TEST(test_elux_parse_rejects_truncated);

  RUN_TEST(test_outer_header);
  RUN_TEST(test_outer_final_checksum);
  RUN_TEST(test_next_count_keeps_high_bit);
  RUN_TEST(test_padded_len);

  RUN_TEST(test_discovery_packet);
  RUN_TEST(test_discovery_response_parsing);
  RUN_TEST(test_discovery_rejects_short_response);
  RUN_TEST(test_mac_byte_order_roundtrip);

  RUN_TEST(test_frame_checksum);
  RUN_TEST(test_frame_roundtrip_validates);
  RUN_TEST(test_frame_rejects_corruption);

  RUN_TEST(test_get_state_frame);
  RUN_TEST(test_toggle_display_frame);
  RUN_TEST(test_set_state_cool_22);
  RUN_TEST(test_set_state_heat_25_5_all_flags);
  RUN_TEST(test_set_state_uses_alternate_temperature);
  RUN_TEST(test_command_frame_rejects_small_buffer);

  RUN_TEST(test_parse_state_frame);
  RUN_TEST(test_parse_state_rejects_bad_payload_crc);
  RUN_TEST(test_parse_state_rejects_wrong_response_id);
  RUN_TEST(test_parse_state_rejects_truncated);

  RUN_TEST(test_get_out_silent_frame);
  RUN_TEST(test_set_out_silent_on);
  RUN_TEST(test_set_out_silent_off);
  RUN_TEST(test_find_property_reads_out_silent);
  RUN_TEST(test_find_property_rejects_absent_and_failed);
  RUN_TEST(test_property_frames_reject_small_buffer);
  RUN_TEST(test_find_property_accepts_real_device_reply);
  RUN_TEST(test_set_self_clean_frames);
  RUN_TEST(test_set_ieco_frames);
  RUN_TEST(test_get_all_properties_frame);
  RUN_TEST(test_find_property_walks_multiple_entries);

  RUN_TEST(test_v2_header);
  RUN_TEST(test_v2_parse);
  RUN_TEST(test_v2_parse_rejects_truncated_and_bad_start);

  RUN_TEST(test_v3_pad_for);
  RUN_TEST(test_v3_encrypted_request_header);
  RUN_TEST(test_v3_handshake_request_header);
  RUN_TEST(test_v3_header_is_big_endian);
  RUN_TEST(test_v3_parse_header);
  RUN_TEST(test_v3_parse_header_rejects_bad_magic);

  return UNITY_END();
}
