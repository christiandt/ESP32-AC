// Broadlink session transport: discovery, the AES handshake, and encrypted
// request/response over UDP.
//
// Ported from mjg59/python-broadlink (device.py). The framing itself lives in
// proto/broadlink_packet.h, which is host-testable; this file adds only the
// socket and the crypto.

#pragma once

#include <WiFiUdp.h>
#include <stdint.h>

#include "../proto/broadlink_packet.h"

namespace acbridge {

class BroadlinkTransport {
 public:
  static constexpr size_t kMaxPacket = 1024;
  static constexpr uint32_t kTimeoutMs = 3000;
  static constexpr int kAttempts = 3;  // UDP; a dropped packet is normal

  void configure(const char* ip);

  // Discovers the device and authenticates. Cheap to call repeatedly: returns
  // immediately once a session exists.
  bool ensureSession();

  // Drops the session so the next call re-handshakes.
  void reset();

  bool ready() const { return ready_; }
  const char* lastError() const { return error_; }

  // Sends a plaintext payload and returns the decrypted response payload
  // (everything from offset 0x38 of the reply). Establishes a session first if
  // needed.
  bool sendPacket(uint16_t packet_type, const uint8_t* payload, size_t payload_len, uint8_t* out,
                  size_t out_cap, size_t* out_len);

 private:
  bool hello();
  bool authenticate();
  // Sends an already-built packet and returns the raw reply.
  bool transact(const uint8_t* packet, size_t len, uint8_t* resp, size_t cap, size_t* resp_len);
  bool sendPacketLocked(uint16_t packet_type, const uint8_t* payload, size_t payload_len,
                        uint8_t* out, size_t out_cap, size_t* out_len);
  void setError(const char* msg);

  char ip_[16] = {0};
  WiFiUDP udp_;
  uint16_t local_port_ = 0;

  proto::DiscoveryInfo info_;
  uint32_t device_id_ = 0;
  uint8_t key_[16] = {0};
  uint16_t count_ = 0;
  bool ready_ = false;
  bool discovered_ = false;

  char error_[64] = {0};
  uint8_t tx_[kMaxPacket];
  uint8_t rx_[kMaxPacket];
  uint8_t scratch_[kMaxPacket];
};

}  // namespace acbridge
