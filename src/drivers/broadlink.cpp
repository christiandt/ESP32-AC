#include "broadlink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "../crypto_util.h"

namespace acbridge {

using namespace acbridge::proto;

namespace {
constexpr uint16_t kAuthPacketType = 0x65;
constexpr size_t kAuthPayloadLen = 0x50;
}  // namespace

void BroadlinkTransport::setError(const char* msg) {
  strncpy(error_, msg, sizeof(error_) - 1);
  error_[sizeof(error_) - 1] = '\0';
}

void BroadlinkTransport::configure(const char* ip) {
  strncpy(ip_, ip, sizeof(ip_) - 1);
  ip_[sizeof(ip_) - 1] = '\0';
  reset();
}

void BroadlinkTransport::reset() {
  ready_ = false;
  discovered_ = false;
  device_id_ = 0;
  count_ = 0;
  udp_.stop();
  local_port_ = 0;
}

bool BroadlinkTransport::transact(const uint8_t* packet, size_t len, uint8_t* resp, size_t cap,
                                  size_t* resp_len) {
  if (local_port_ == 0) {
    // Bind a stable ephemeral port; the discovery packet has to carry it.
    local_port_ = static_cast<uint16_t>(20000 + (esp_random() % 20000));
    if (!udp_.begin(local_port_)) {
      setError("could not open UDP socket");
      local_port_ = 0;
      return false;
    }
  }

  IPAddress addr;
  if (!addr.fromString(ip_)) {
    setError("device IP is not a valid address");
    return false;
  }

  for (int attempt = 0; attempt < kAttempts; attempt++) {
    // Drain anything stale so a late reply to a previous attempt isn't mistaken
    // for this one's.
    while (udp_.parsePacket() > 0) udp_.clear();

    if (!udp_.beginPacket(addr, kBroadlinkPort)) {
      setError("could not address the device");
      continue;
    }
    udp_.write(packet, len);
    if (!udp_.endPacket()) {
      setError("UDP send failed");
      continue;
    }

    bool timed_out = true;
    const uint32_t deadline = millis() + kTimeoutMs;
    while (millis() < deadline) {
      const int size = udp_.parsePacket();
      if (size <= 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (static_cast<size_t>(size) > cap) {
        setError("reply too large");
        udp_.clear();
        timed_out = false;
        break;
      }
      const int got = udp_.read(resp, cap);
      if (got <= 0) {
        timed_out = false;
        break;
      }
      *resp_len = static_cast<size_t>(got);
      return true;
    }
    // Don't clobber a more specific reason from the loop above.
    if (timed_out) setError("device did not reply");
  }
  return false;
}

bool BroadlinkTransport::hello() {
  const IPAddress local = WiFi.localIP();
  const uint8_t local_ip[4] = {local[0], local[1], local[2], local[3]};

  // The device does not validate the timestamp block, and the ESP32 has no
  // reliable clock at this point, so send zeros.
  const uint8_t datetime[12] = {0};

  uint8_t packet[kDiscoveryLen];
  // transact() binds the socket, but the discovery packet has to carry the
  // source port, so bind first if we haven't.
  if (local_port_ == 0) {
    local_port_ = static_cast<uint16_t>(20000 + (esp_random() % 20000));
    if (!udp_.begin(local_port_)) {
      setError("could not open UDP socket");
      local_port_ = 0;
      return false;
    }
  }
  buildDiscovery(packet, local_ip, local_port_, datetime);

  size_t resp_len = 0;
  if (!transact(packet, sizeof(packet), rx_, sizeof(rx_), &resp_len)) return false;

  if (!parseDiscovery(rx_, resp_len, &info_)) {
    setError("discovery reply was too short");
    return false;
  }
  discovered_ = true;
  return true;
}

bool BroadlinkTransport::authenticate() {
  // auth() starts from id 0 and the fixed bootstrap key.
  device_id_ = 0;
  memcpy(key_, kInitKey, sizeof(key_));

  uint8_t payload[kAuthPayloadLen];
  memset(payload, 0, sizeof(payload));
  memset(payload + 0x04, 0x31, 16);
  payload[0x1E] = 0x01;
  payload[0x2D] = 0x01;
  memcpy(payload + 0x30, "Test 1", 6);

  // The auth reply payload is ~0x50 bytes; no need for a full-size buffer on a
  // task stack.
  uint8_t out[256];
  size_t out_len = 0;
  if (!sendPacketLocked(kAuthPacketType, payload, sizeof(payload), out, sizeof(out), &out_len)) {
    return false;
  }
  if (out_len < 0x14) {
    setError("auth reply was too short");
    return false;
  }

  device_id_ = static_cast<uint32_t>(out[0]) | (static_cast<uint32_t>(out[1]) << 8) |
               (static_cast<uint32_t>(out[2]) << 16) | (static_cast<uint32_t>(out[3]) << 24);
  memcpy(key_, out + 0x04, 16);  // payload[0x04:0x14]
  ready_ = true;
  return true;
}

bool BroadlinkTransport::ensureSession() {
  if (ready_) return true;
  if (ip_[0] == '\0') {
    setError("no device IP configured");
    return false;
  }
  if (!discovered_ && !hello()) return false;
  return authenticate();
}

bool BroadlinkTransport::sendPacket(uint16_t packet_type, const uint8_t* payload,
                                    size_t payload_len, uint8_t* out, size_t out_cap,
                                    size_t* out_len) {
  if (!ensureSession()) return false;
  if (sendPacketLocked(packet_type, payload, payload_len, out, out_cap, out_len)) return true;

  // A session can lapse (the vendor app grabs the device, the module reboots).
  // Re-handshake once before giving up.
  ready_ = false;
  discovered_ = false;
  if (!ensureSession()) return false;
  return sendPacketLocked(packet_type, payload, payload_len, out, out_cap, out_len);
}

bool BroadlinkTransport::sendPacketLocked(uint16_t packet_type, const uint8_t* payload,
                                          size_t payload_len, uint8_t* out, size_t out_cap,
                                          size_t* out_len) {
  const size_t padded = paddedLen(payload_len);
  if (kOuterHeaderLen + padded > sizeof(tx_)) {
    setError("payload too large");
    return false;
  }

  count_ = nextCount(count_);
  buildOuterHeader(tx_, info_.devtype, packet_type, count_, info_.mac, device_id_, payload,
                   payload_len);

  // Zero padding, not PKCS7 — broadlink appends plain zero bytes.
  memset(scratch_, 0, padded);
  memcpy(scratch_, payload, payload_len);
  if (!crypto::aesCbcEncrypt(key_, kInitVect, scratch_, padded, tx_ + kOuterHeaderLen)) {
    setError("AES encrypt failed");
    return false;
  }

  const size_t total = kOuterHeaderLen + padded;
  finalizeOuter(tx_, total);

  size_t resp_len = 0;
  if (!transact(tx_, total, rx_, sizeof(rx_), &resp_len)) return false;

  const int16_t err = outerErrorCode(rx_, resp_len);
  if (err != 0) {
    snprintf(error_, sizeof(error_), "device returned error %d", err);
    return false;
  }

  if (resp_len <= kOuterHeaderLen) {
    setError("reply had no payload");
    return false;
  }
  const size_t cipher_len = resp_len - kOuterHeaderLen;
  if (cipher_len % 16 != 0) {
    setError("reply payload was not block-aligned");
    return false;
  }
  if (cipher_len > out_cap) {
    setError("reply payload too large");
    return false;
  }

  if (!crypto::aesCbcDecrypt(key_, kInitVect, rx_ + kOuterHeaderLen, cipher_len, out)) {
    setError("AES decrypt failed");
    return false;
  }
  *out_len = cipher_len;
  error_[0] = '\0';
  return true;
}

}  // namespace acbridge
