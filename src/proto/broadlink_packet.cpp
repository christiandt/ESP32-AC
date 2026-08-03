#include "broadlink_packet.h"

#include <string.h>

namespace acbridge {
namespace proto {

// "097628343fe99e23765c1513accf8b02"
const uint8_t kInitKey[16] = {0x09, 0x76, 0x28, 0x34, 0x3f, 0xe9, 0x9e, 0x23,
                              0x76, 0x5c, 0x15, 0x13, 0xac, 0xcf, 0x8b, 0x02};
// "562e17996d093d28ddb3ba695a2e6f58"
const uint8_t kInitVect[16] = {0x56, 0x2e, 0x17, 0x99, 0x6d, 0x09, 0x3d, 0x28,
                               0xdd, 0xb3, 0xba, 0x69, 0x5a, 0x2e, 0x6f, 0x58};

namespace {

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

uint16_t getLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint16_t sumSeeded(const uint8_t* data, size_t len, uint16_t seed) {
  uint32_t acc = seed;
  for (size_t i = 0; i < len; i++) acc += data[i];
  return static_cast<uint16_t>(acc & 0xFFFF);
}

}  // namespace

void buildDiscovery(uint8_t out[kDiscoveryLen], const uint8_t local_ip[4], uint16_t src_port,
                    const uint8_t datetime[12]) {
  memset(out, 0, kDiscoveryLen);
  if (datetime != nullptr) memcpy(out + 0x08, datetime, 12);

  // inet_aton(ip)[::-1] — the octets go in reversed.
  out[0x18] = local_ip[3];
  out[0x19] = local_ip[2];
  out[0x1A] = local_ip[1];
  out[0x1B] = local_ip[0];

  putLe16(out + 0x1C, src_port);
  out[0x26] = 6;  // hello

  putLe16(out + 0x20, sumSeeded(out, kDiscoveryLen, kChecksumSeed));
}

bool parseDiscovery(const uint8_t* resp, size_t len, DiscoveryInfo* out) {
  if (resp == nullptr || out == nullptr || len < 0x80) return false;
  out->devtype = getLe16(resp + 0x34);
  // resp[0x3A:0x40][::-1]
  for (size_t i = 0; i < 6; i++) out->mac[i] = resp[0x3F - i];
  out->is_locked = resp[0x7F] != 0;
  return true;
}

void buildOuterHeader(uint8_t out[kOuterHeaderLen], uint16_t devtype, uint16_t packet_type,
                      uint16_t count, const uint8_t mac[6], uint32_t device_id,
                      const uint8_t* payload, size_t payload_len) {
  memset(out, 0, kOuterHeaderLen);

  static const uint8_t kMagic[8] = {0x5a, 0xa5, 0xaa, 0x55, 0x5a, 0xa5, 0xaa, 0x55};
  memcpy(out, kMagic, sizeof(kMagic));

  putLe16(out + 0x24, devtype);
  putLe16(out + 0x26, packet_type);
  putLe16(out + 0x28, count);

  // self.mac[::-1]
  for (size_t i = 0; i < 6; i++) out[0x2A + i] = mac[5 - i];

  out[0x30] = static_cast<uint8_t>(device_id & 0xFF);
  out[0x31] = static_cast<uint8_t>((device_id >> 8) & 0xFF);
  out[0x32] = static_cast<uint8_t>((device_id >> 16) & 0xFF);
  out[0x33] = static_cast<uint8_t>((device_id >> 24) & 0xFF);

  // Checksum of the *plaintext* payload, before padding and encryption.
  putLe16(out + 0x34, sumSeeded(payload, payload_len, kChecksumSeed));
}

void finalizeOuter(uint8_t* packet, size_t total_len) {
  packet[0x20] = 0;
  packet[0x21] = 0;
  putLe16(packet + 0x20, sumSeeded(packet, total_len, kChecksumSeed));
}

int16_t outerErrorCode(const uint8_t* resp, size_t len) {
  if (len < 0x24) return -1;
  return static_cast<int16_t>(getLe16(resp + 0x22));
}

size_t buildEluxFrame(uint8_t* out, size_t cap, uint16_t command, const char* json,
                      size_t json_len) {
  const size_t total = kEluxHeaderLen + json_len;
  if (out == nullptr || cap < total) return 0;

  memset(out, 0, kEluxHeaderLen);
  putLe16(out + 0x00, command);
  out[0x02] = 0xa5;
  out[0x03] = 0xa5;
  out[0x04] = 0x5a;
  out[0x05] = 0x5a;
  out[0x08] = json_len <= 2 ? 0x01 : 0x02;
  out[0x09] = 0x0b;
  putLe16(out + 0x0A, static_cast<uint16_t>(json_len));
  // 0x0C-0x0D stay zero.

  memcpy(out + kEluxHeaderLen, json, json_len);

  // Checksum covers 0x08 to the end, including the payload.
  putLe16(out + 0x06, sumSeeded(out + 0x08, total - 0x08, kEluxChecksumSeed));
  return total;
}

bool parseEluxFrame(const uint8_t* dec, size_t len, const uint8_t** payload, size_t* payload_len) {
  if (dec == nullptr || len < kEluxHeaderLen) return false;

  const uint16_t expected = getLe16(dec + 0x06);
  const uint16_t actual = sumSeeded(dec + 0x08, len - 0x08, kEluxChecksumSeed);
  if (expected != actual) return false;

  const uint16_t n = getLe16(dec + 0x0A);
  if (kEluxHeaderLen + n > len) return false;

  *payload = dec + kEluxHeaderLen;
  *payload_len = n;
  return true;
}

}  // namespace proto
}  // namespace acbridge
