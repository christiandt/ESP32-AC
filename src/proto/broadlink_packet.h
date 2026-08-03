// Broadlink wire framing, and the Electrolux OEM frame that rides inside it.
//
// Ported from mjg59/python-broadlink (device.py: scan, send_packet, auth) and
// from christiandt/electrolux-ac-cli (electrolux/cli.py: _send).
//
// Pure C++: no Arduino, no ESP-IDF, no crypto. Encryption is the caller's job,
// so this whole layer is exercisable by the host-native tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace acbridge {
namespace proto {

// -- Broadlink outer packet -------------------------------------------------

constexpr size_t kOuterHeaderLen = 0x38;
constexpr size_t kDiscoveryLen = 0x30;
constexpr uint16_t kChecksumSeed = 0xBEAF;
constexpr uint16_t kBroadlinkPort = 80;

// Broadlink's fixed bootstrap key/IV, used only until auth() hands back a
// per-device key. Both are AES-128-CBC.
extern const uint8_t kInitKey[16];
extern const uint8_t kInitVect[16];

// Fills the 0x30-byte discovery ("hello") packet. `local_ip` is the four
// octets a.b.c.d in order; the packet stores them reversed, as inet_aton()[::-1]
// does. `datetime` is the 12-byte timestamp block — the device does not
// validate it, so zeros are fine when the clock isn't set yet.
void buildDiscovery(uint8_t out[kDiscoveryLen], const uint8_t local_ip[4], uint16_t src_port,
                    const uint8_t datetime[12]);

struct DiscoveryInfo {
  uint16_t devtype = 0;
  uint8_t mac[6] = {0};
  bool is_locked = false;
};

// Parses a discovery reply. Needs at least 0x80 bytes.
bool parseDiscovery(const uint8_t* resp, size_t len, DiscoveryInfo* out);

// Writes the 0x38-byte outer header for a packet whose plaintext payload is
// `payload`. The caller then appends the ciphertext (payload zero-padded to a
// 16-byte multiple, AES-128-CBC) and calls finalizeOuter over the whole thing.
void buildOuterHeader(uint8_t out[kOuterHeaderLen], uint16_t devtype, uint16_t packet_type,
                      uint16_t count, const uint8_t mac[6], uint32_t device_id,
                      const uint8_t* payload, size_t payload_len);

// Computes the whole-packet checksum and writes it to 0x20. Call once the
// ciphertext has been appended; bytes 0x20-0x21 must still be zero.
void finalizeOuter(uint8_t* packet, size_t total_len);

// Broadlink advances a 16-bit counter per packet, always with the top bit set.
inline uint16_t nextCount(uint16_t count) {
  return static_cast<uint16_t>(((count + 1) | 0x8000) & 0xFFFF);
}

// Zero-padding to the AES block size. Note this is *not* PKCS7 — broadlink
// appends plain zero bytes (`payload + bytes(padding)`).
inline size_t paddedLen(size_t len) { return (len + 15) & ~static_cast<size_t>(15); }

// The device's error code lives at 0x22 of every reply. Zero means success.
int16_t outerErrorCode(const uint8_t* resp, size_t len);

// -- Electrolux OEM inner frame ---------------------------------------------
//
// Layout, in both directions:
//   0x00-0x01  command (LE16)
//   0x02-0x05  a5 a5 5a 5a
//   0x06-0x07  checksum (LE16) over bytes 0x08..end, seeded 0xC0AD
//   0x08       0x01 when the payload is <= 2 bytes, else 0x02
//   0x09       0x0b
//   0x0A-0x0B  payload length (LE16)
//   0x0C-0x0D  zero
//   0x0E...    payload (ASCII JSON)
//
// The Python original builds this as a 13-byte bytearray and then writes two
// bytes into the one-byte slice [0x0A:0x0B], relying on bytearray slice
// assignment to resize it to 14. Same bytes on the wire; spelled out here.

constexpr size_t kEluxHeaderLen = 0x0E;
constexpr uint16_t kEluxChecksumSeed = 0xC0AD;

// Electrolux sub-commands, from electrolux/cli.py.
enum : uint16_t {
  kEluxCmdStatus = 0x0E,    // {}
  kEluxCmdTemp = 0x17,      // {"temp":N}
  kEluxCmdPower = 0x18,     // {"ac_pwr":N} / {"ac_slp":N} / {"mldprf":N}
  kEluxCmdMode = 0x19,      // {"ac_mode":N} / {"ac_mark":N} / {"ac_vdir":N} / {"scrdisp":N}
  kEluxCmdTimer = 0x1F,     // {"timer":"HHMM|0X"}
};

// Builds an Electrolux frame into `out`. Returns the total length, or 0 if the
// buffer is too small.
size_t buildEluxFrame(uint8_t* out, size_t cap, uint16_t command, const char* json,
                      size_t json_len);

// Validates a decrypted Electrolux frame and locates its payload. `payload`
// points into `dec`; it is not NUL-terminated.
bool parseEluxFrame(const uint8_t* dec, size_t len, const uint8_t** payload, size_t* payload_len);

}  // namespace proto
}  // namespace acbridge
