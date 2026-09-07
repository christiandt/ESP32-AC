#include "midea.h"

#include <Arduino.h>
#include <string.h>

#include "../crypto_util.h"
#include "../proto/midea_frame.h"

namespace acbridge {

using namespace acbridge::proto;

namespace {

// The V3 session uses a zero IV throughout (msmart's Security.decrypt_aes_cbc).
const uint8_t kZeroIv[16] = {0};

// command.py ResponseId.CAPABILITIES. Capabilities are baked into NVS at
// provisioning time (see tools/export_config.py --probe), so this firmware
// never asks for them — which makes any 0xB5 reply one the unit volunteered.
constexpr uint8_t kResponseCapabilities = 0xB5;

// How many volunteered frames to step over before treating the exchange as
// lost. In practice the unit sends at most one.
constexpr int kMaxUnsolicitedFrames = 3;

// Some units interleave unsolicited CAPABILITIES frames (frame type 0x05) with
// the reply to whatever was actually asked. msmart carries the same allowance
// in Response.construct.
bool isUnsolicited(const uint8_t* frame, size_t len) {
  return len > kFrameHeaderLen && frame[kFrameHeaderLen] == kResponseCapabilities;
}

// _Packet._timestamp packs the current time as BCD-ish bytes. The device does
// not appear to validate it and the ESP32 has no clock, so send something
// well-formed and fixed rather than zeros (which would encode month 0, day 0).
const uint8_t kTimestamp[8] = {0x00, 0x00, 0x00, 0x0C, 0x01, 0x01, 0x19, 0x14};

uint8_t modeCode(Mode m) {
  switch (m) {
    case Mode::Auto: return kModeAuto;
    case Mode::Cool: return kModeCool;
    case Mode::Dry: return kModeDry;
    case Mode::Heat: return kModeHeat;
    case Mode::Fan: return kModeFanOnly;
  }
  return kModeAuto;
}

bool modeFromCode(uint8_t code, Mode& out) {
  switch (code) {
    case kModeAuto: out = Mode::Auto; return true;
    case kModeCool: out = Mode::Cool; return true;
    case kModeDry: out = Mode::Dry; return true;
    case kModeHeat: out = Mode::Heat; return true;
    case kModeFanOnly: out = Mode::Fan; return true;
    // SMART_DRY (6) has no vocabulary of its own; report it as dry.
    case 6: out = Mode::Dry; return true;
  }
  return false;
}

uint8_t swingCode(Swing s) {
  switch (s) {
    case Swing::Off: return kSwingOff;
    case Swing::Vertical: return kSwingVertical;
    case Swing::Horizontal: return kSwingHorizontal;
    case Swing::Both: return kSwingBoth;
  }
  return kSwingOff;
}

Swing swingFromCode(uint8_t code) {
  switch (code) {
    case kSwingVertical: return Swing::Vertical;
    case kSwingHorizontal: return Swing::Horizontal;
    case kSwingBoth: return Swing::Both;
    default: return Swing::Off;
  }
}

uint8_t fanCode(const FanSpeed& f) {
  if (f.is_percent) return f.percent;
  switch (f.preset) {
    case FanPreset::Auto: return kFanAuto;
    case FanPreset::Silent: return kFanSilent;
    case FanPreset::Low: return kFanLow;
    case FanPreset::Medium: return kFanMedium;
    case FanPreset::High: return kFanHigh;
    case FanPreset::Turbo: return kFanMax;
  }
  return kFanAuto;
}

// Named steps are percentages on this protocol, so a reported speed is a preset
// only when it lands exactly on one. Anything else is a genuine custom
// percentage — the case that broke mpsac before its commit 4b1dda0.
FanSpeed fanFromCode(uint8_t code) {
  switch (code) {
    case kFanAuto: return FanSpeed::ofPreset(FanPreset::Auto);
    case kFanSilent: return FanSpeed::ofPreset(FanPreset::Silent);
    case kFanLow: return FanSpeed::ofPreset(FanPreset::Low);
    case kFanMedium: return FanSpeed::ofPreset(FanPreset::Medium);
    case kFanHigh: return FanSpeed::ofPreset(FanPreset::High);
    case kFanMax: return FanSpeed::ofPreset(FanPreset::Turbo);
    default: return FanSpeed::ofPercent(code);
  }
}

// The features this driver can actually set through SetStateCommand. Others
// (iECO, out-silent, self clean) go through msmart's property commands, which
// aren't ported.
bool featureImplemented(Feature f) {
  switch (f) {
    case Feature::Eco:
    case Feature::Boost:
    case Feature::Sleep:
    case Feature::Ion:
    case Feature::Beep:
    case Feature::Freeze:
    case Feature::FollowMe:
    case Feature::Led:
      return true;
    default:
      return false;
  }
}

}  // namespace

MideaDriver::MideaDriver(const MideaConfig& cfg) {
  strncpy(name_, cfg.name[0] != '\0' ? cfg.name : "Midea", sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = '\0';
  strncpy(ip_, cfg.ip, sizeof(ip_) - 1);
  ip_[sizeof(ip_) - 1] = '\0';

  port_ = cfg.port != 0 ? cfg.port : 6444;
  device_id_ = cfg.device_id;
  caps_ = cfg.caps;
  min_target_ = cfg.min_target;
  max_target_ = cfg.max_target;

  crypto::hexDecode(cfg.token, token_, sizeof(token_), &token_len_);
  crypto::hexDecode(cfg.key, cloud_key_, sizeof(cloud_key_), &cloud_key_len_);

  crypto::md5(reinterpret_cast<const uint8_t*>(kMideaSignKey), kMideaSignKeyLen, enc_key_);
}

bool MideaDriver::supportsFeature(Feature f) const {
  return featureImplemented(f) && capsHas(caps_, f);
}

bool MideaDriver::supportsAction(Action a) const {
  // The display is a flip, not a settable flag (msmart's toggle_display).
  return a == Action::LedToggle && capsHas(caps_, Feature::Led);
}

void MideaDriver::reset() {
  sock_.stop();
  authenticated_ = false;
  local_key_len_ = 0;
}

// -- Socket ------------------------------------------------------------------

bool MideaDriver::writeAll(const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const size_t n = sock_.write(data + sent, len - sent);
    if (n == 0) return false;
    sent += n;
  }
  return true;
}

bool MideaDriver::readExact(uint8_t* buf, size_t n) {
  size_t got = 0;
  const uint32_t deadline = millis() + kReadTimeoutMs;
  while (got < n) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    const int avail = sock_.available();
    if (avail <= 0) {
      if (!sock_.connected()) return false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const int r = sock_.read(buf + got, n - got);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

bool MideaDriver::connectSocket(AcState& state) {
  sock_.stop();
  if (ip_[0] == '\0') {
    state.setError("no device IP configured");
    return false;
  }
  if (!sock_.connect(ip_, port_, kConnectTimeoutMs)) {
    state.setError("could not connect on port 6444");
    return false;
  }
  sock_.setNoDelay(true);
  packet_id_ = 0;
  return true;
}

// -- V3 layer ----------------------------------------------------------------

bool MideaDriver::sendV3(const uint8_t* data, size_t len, uint8_t type) {
  if (type == kV3HandshakeRequest) {
    // Handshake packets are neither encrypted nor signed.
    if (kV3HeaderLen + 2 + len > sizeof(tx_)) return false;
    buildV3Header(tx_, static_cast<uint16_t>(len), 0, kV3HandshakeRequest);
    tx_[kV3HeaderLen] = static_cast<uint8_t>((packet_id_ >> 8) & 0xFF);
    tx_[kV3HeaderLen + 1] = static_cast<uint8_t>(packet_id_ & 0xFF);
    memcpy(tx_ + kV3HeaderLen + 2, data, len);
    packet_id_ = (packet_id_ + 1) & 0x0FFF;
    return writeAll(tx_, kV3HeaderLen + 2 + len);
  }

  if (local_key_len_ == 0) return false;

  const size_t pad = v3PadFor(len);
  const size_t payload_len = 2 + len + pad;  // block aligned by construction
  const size_t total = kV3HeaderLen + payload_len + kV3SignLen;
  if (total > sizeof(tx_) || payload_len > sizeof(scratch_)) return false;

  buildV3Header(tx_, static_cast<uint16_t>(len + pad + kV3SignLen), static_cast<uint8_t>(pad),
                kV3EncryptedRequest);

  // msmart fills the pad with random bytes; the receiver strips them by count,
  // so zeros are equivalent.
  memset(scratch_, 0, payload_len);
  scratch_[0] = static_cast<uint8_t>((packet_id_ >> 8) & 0xFF);
  scratch_[1] = static_cast<uint8_t>(packet_id_ & 0xFF);
  memcpy(scratch_ + 2, data, len);
  packet_id_ = (packet_id_ + 1) & 0x0FFF;

  // The signature covers the header and the *plaintext* payload.
  crypto::sha256Pair(tx_, kV3HeaderLen, scratch_, payload_len, tx_ + kV3HeaderLen + payload_len);

  if (!crypto::aesCbcEncrypt(local_key_, local_key_len_, kZeroIv, scratch_, payload_len,
                             tx_ + kV3HeaderLen)) {
    return false;
  }
  return writeAll(tx_, total);
}

bool MideaDriver::recvV3(uint8_t* out, size_t cap, size_t* out_len, uint8_t* type) {
  uint8_t header[kV3HeaderLen];
  if (!readExact(header, sizeof(header))) return false;

  V3Header h;
  if (!parseV3Header(header, sizeof(header), &h)) return false;
  if (h.total_len < kV3HeaderLen || h.total_len - kV3HeaderLen > sizeof(rx_)) return false;

  const size_t body_len = h.total_len - kV3HeaderLen;
  if (!readExact(rx_, body_len)) return false;

  *type = h.type;

  if (h.type == kV3HandshakeResponse) {
    // 2-byte packet id, then the raw payload.
    if (body_len < 2) return false;
    const size_t n = body_len - 2;
    if (n > cap) return false;
    memcpy(out, rx_ + 2, n);
    *out_len = n;
    return true;
  }

  if (h.type != kV3EncryptedResponse) return false;
  if (body_len < kV3SignLen) return false;

  const size_t cipher_len = body_len - kV3SignLen;
  if (cipher_len == 0 || cipher_len % 16 != 0 || cipher_len > sizeof(scratch_)) return false;

  if (!crypto::aesCbcDecrypt(local_key_, local_key_len_, kZeroIv, rx_, cipher_len, scratch_)) {
    return false;
  }
  uint8_t digest[32];
  crypto::sha256Pair(header, kV3HeaderLen, scratch_, cipher_len, digest);
  if (!crypto::equals(digest, rx_ + cipher_len, kV3SignLen)) return false;

  // Strip the 2-byte packet id and the padding. msmart writes this as
  // payload[2:-pad], which silently yields nothing when pad is 0; computing the
  // end offset avoids that.
  if (cipher_len < 2 + h.pad) return false;
  const size_t n = cipher_len - 2 - h.pad;
  if (n > cap) return false;
  memcpy(out, scratch_ + 2, n);
  *out_len = n;
  return true;
}

// -- V2 layer ----------------------------------------------------------------

bool MideaDriver::encodeV2(const uint8_t* frame, size_t len, uint8_t* out, size_t cap,
                           size_t* out_len) {
  uint8_t padded[128];
  if (len > sizeof(padded)) return false;
  memcpy(padded, frame, len);
  const size_t padded_len = crypto::pkcs7Pad(padded, len, sizeof(padded));
  if (padded_len == 0) return false;

  const size_t total = kV2HeaderLen + padded_len + kV2SignLen;
  if (total > cap) return false;

  buildV2Header(out, device_id_, padded_len, kTimestamp);
  if (!crypto::aesEcbEncrypt(enc_key_, sizeof(enc_key_), padded, padded_len, out + kV2HeaderLen)) {
    return false;
  }
  // MD5 over the packet followed by the fixed sign key.
  crypto::md5Pair(out, kV2HeaderLen + padded_len,
                  reinterpret_cast<const uint8_t*>(kMideaSignKey), kMideaSignKeyLen,
                  out + kV2HeaderLen + padded_len);
  *out_len = total;
  return true;
}

bool MideaDriver::decodeV2(const uint8_t* packet, size_t len, uint8_t* out, size_t cap,
                           size_t* out_len) {
  V2Layout layout;
  if (!parseV2(packet, len, &layout)) return false;

  uint8_t digest[16];
  crypto::md5Pair(packet, layout.signed_len, reinterpret_cast<const uint8_t*>(kMideaSignKey),
                  kMideaSignKeyLen, digest);
  if (!crypto::equals(digest, packet + layout.sign_off, kV2SignLen)) return false;

  if (layout.cipher_len == 0 || layout.cipher_len % 16 != 0 || layout.cipher_len > cap) {
    return false;
  }
  if (!crypto::aesEcbDecrypt(enc_key_, sizeof(enc_key_), packet + layout.cipher_off,
                             layout.cipher_len, out)) {
    return false;
  }
  const size_t unpadded = crypto::pkcs7Unpad(out, layout.cipher_len);
  if (unpadded == 0) return false;
  *out_len = unpadded;
  return true;
}

bool MideaDriver::recvV2Raw(uint8_t* out, size_t cap, size_t* out_len) {
  // V2 devices speak the inner packet straight over the socket.
  uint8_t header[6];
  if (!readExact(header, sizeof(header))) return false;
  if (header[0] != 0x5A || header[1] != 0x5A) return false;

  const size_t declared = static_cast<size_t>(header[4]) | (static_cast<size_t>(header[5]) << 8);
  if (declared <= sizeof(header) || declared > cap) return false;

  memcpy(out, header, sizeof(header));
  if (!readExact(out + sizeof(header), declared - sizeof(header))) return false;
  *out_len = declared;
  return true;
}

// -- Session -----------------------------------------------------------------

bool MideaDriver::deriveLocalKey(const uint8_t* response, size_t response_len,
                                 const uint8_t* cloud_key, size_t cloud_key_len, uint8_t* out,
                                 size_t* out_len) {
  // 32 bytes of encrypted payload followed by its SHA256.
  if (response_len != 64 || cloud_key_len != 32) return false;

  uint8_t decrypted[32];
  if (!crypto::aesCbcDecrypt(cloud_key, cloud_key_len, kZeroIv, response, 32, decrypted)) {
    return false;
  }

  uint8_t digest[32];
  crypto::sha256(decrypted, sizeof(decrypted), digest);
  if (!crypto::equals(digest, response + 32, 32)) return false;

  // The session key is the decrypted payload XORed with the cloud key.
  crypto::xorBytes(decrypted, cloud_key, sizeof(decrypted), out);
  *out_len = sizeof(decrypted);
  return true;
}

bool MideaDriver::handshake(AcState& state) {
  if (token_len_ == 0 || cloud_key_len_ == 0) {
    state.setError("V3 device but no token/key stored");
    return false;
  }

  if (!sendV3(token_, token_len_, kV3HandshakeRequest)) {
    state.setError("handshake write failed");
    return false;
  }

  uint8_t response[96];
  size_t response_len = 0;
  uint8_t type = 0;
  if (!recvV3(response, sizeof(response), &response_len, &type)) {
    state.setError("no handshake reply");
    return false;
  }
  if (type != kV3HandshakeResponse) {
    state.setError("unexpected handshake reply type");
    return false;
  }
  if (!deriveLocalKey(response, response_len, cloud_key_, cloud_key_len_, local_key_,
                      &local_key_len_)) {
    state.setError("handshake failed — token/key may be stale, re-run `mpsac setup`");
    return false;
  }

  authenticated_ = true;
  auth_deadline_ = millis() + kReauthIntervalMs;
  return true;
}

bool MideaDriver::ensureSession(AcState& state) {
  const bool v3 = token_len_ > 0 && cloud_key_len_ > 0;

  // Comparing by subtraction keeps this correct across the millis() wrap.
  const bool expired = v3 && authenticated_ &&
                       static_cast<int32_t>(millis() - auth_deadline_) >= 0;
  if (expired) {
    reset();
  }

  if (sock_.connected() && (!v3 || authenticated_)) return true;

  if (!connectSocket(state)) return false;
  if (!v3) {
    authenticated_ = false;
    return true;
  }
  return handshake(state);
}

bool MideaDriver::transact(const uint8_t* frame, size_t frame_len, uint8_t* out, size_t cap,
                           size_t* out_len, AcState& state) {
  const bool v3 = token_len_ > 0 && cloud_key_len_ > 0;

  for (int attempt = 0; attempt < 2; attempt++) {
    if (!ensureSession(state)) return false;

    uint8_t packet[256];
    size_t packet_len = 0;
    if (!encodeV2(frame, frame_len, packet, sizeof(packet), &packet_len)) {
      state.setError("could not encode packet");
      return false;
    }

    bool ok = false;
    if (v3 ? sendV3(packet, packet_len, kV3EncryptedRequest) : writeAll(packet, packet_len)) {
      // Step over anything the unit volunteered. Those frames arrive where our
      // reply should be, but the reply is still queued behind them, so read on
      // rather than re-sending the query.
      for (int i = 0; i <= kMaxUnsolicitedFrames; i++) {
        size_t reply_len = 0;
        bool got;
        if (v3) {
          uint8_t type = 0;
          got = recvV3(scratch_, sizeof(scratch_), &reply_len, &type) &&
                type == kV3EncryptedResponse;
        } else {
          got = recvV2Raw(scratch_, sizeof(scratch_), &reply_len);
        }
        if (!got || !decodeV2(scratch_, reply_len, out, cap, out_len)) break;
        if (!isUnsolicited(out, *out_len)) {
          ok = true;
          break;
        }
      }
    }
    if (ok) return true;

    // A stale session is the common cause here: the vendor app took the
    // device, or the unit dropped the connection. Reconnect once and retry.
    state.setError("no usable reply from the unit");
    reset();
  }
  return false;
}

// -- Driver ------------------------------------------------------------------

void MideaDriver::publish(const MideaState& st, AcState& out) const {
  out.power.set(st.power);

  Mode m;
  if (modeFromCode(st.mode, m)) out.mode.set(m);

  out.target_temp.set(st.target_temp);
  if (st.has_indoor) out.indoor_temp.set(st.indoor);
  if (st.has_outdoor) out.outdoor_temp.set(st.outdoor);

  out.fan.set(fanFromCode(st.fan_speed));
  out.swing.set(swingFromCode(st.swing_mode));

  if (supportsFeature(Feature::Eco)) out.feature(Feature::Eco).set(st.eco);
  if (supportsFeature(Feature::Boost)) out.feature(Feature::Boost).set(st.turbo);
  if (supportsFeature(Feature::Sleep)) out.feature(Feature::Sleep).set(st.sleep);
  if (supportsFeature(Feature::Ion)) out.feature(Feature::Ion).set(st.purifier);
  if (supportsFeature(Feature::FollowMe)) out.feature(Feature::FollowMe).set(st.follow_me);
  if (supportsFeature(Feature::Led)) out.feature(Feature::Led).set(st.display_on);
  if (supportsFeature(Feature::Beep)) out.feature(Feature::Beep).set(beep_);
  if (supportsFeature(Feature::Freeze) && st.has_freeze) {
    out.feature(Feature::Freeze).set(st.freeze_protection);
  }

  out.min_target = min_target_;
  out.max_target = max_target_;

  if (st.error_code != 0) {
    char buf[48];
    snprintf(buf, sizeof(buf), "unit reported error code %u", st.error_code);
    out.setError(buf);
  }
}

bool MideaDriver::poll(AcState& state) {
  uint8_t frame[64];
  const size_t len = buildGetStateFrame(frame, sizeof(frame), ++message_id_);
  if (len == 0) {
    state.setError("could not build query frame");
    return false;
  }

  uint8_t reply[128];
  size_t reply_len = 0;
  if (!transact(frame, len, reply, sizeof(reply), &reply_len, state)) return false;

  MideaState st;
  if (!parseStateFrame(reply, reply_len, &st)) {
    state.setError("state reply failed validation");
    return false;
  }

  cached_ = st;
  has_cached_ = true;
  publish(st, state);
  return true;
}

bool MideaDriver::apply(const AcCommand& cmd, AcState& state) {
  // The set command carries the whole state, so anything the caller didn't
  // mention has to come from the unit rather than from defaults.
  if (!has_cached_ && !poll(state)) return false;

  if (cmd.action == Action::LedToggle) {
    uint8_t frame[64];
    const size_t len = buildToggleDisplayFrame(frame, sizeof(frame), ++message_id_, beep_);
    uint8_t reply[128];
    size_t reply_len = 0;
    if (!transact(frame, len, reply, sizeof(reply), &reply_len, state)) return false;
    // The reply is a state frame, but the display bit lags the flip; a fresh
    // poll is more trustworthy than parsing it.
    return poll(state);
  }

  SetState s;
  s.beep = beep_;
  s.power = cached_.power;
  s.target_temp = cached_.target_temp;
  s.mode = cached_.mode == 0 ? kModeAuto : cached_.mode;
  s.fan_speed = cached_.fan_speed;
  s.swing_mode = cached_.swing_mode;
  s.eco = cached_.eco;
  s.turbo = cached_.turbo;
  s.sleep = cached_.sleep;
  s.fahrenheit = cached_.fahrenheit;
  s.freeze_protection = cached_.freeze_protection;
  s.follow_me = cached_.follow_me;
  s.purifier = cached_.purifier;
  s.target_humidity = cached_.has_humidity ? cached_.target_humidity : 40;

  if (cmd.power.has()) s.power = cmd.power.value;
  if (cmd.mode.has()) {
    s.mode = modeCode(cmd.mode.value);
    s.power = true;  // mirrors mpsac: selecting a mode powers the unit on
  }
  if (cmd.target_temp.has()) s.target_temp = cmd.target_temp.value;
  if (cmd.fan.has()) s.fan_speed = fanCode(cmd.fan.value);
  if (cmd.swing.has()) s.swing_mode = swingCode(cmd.swing.value);

  if (cmd.feature(Feature::Eco).has()) s.eco = cmd.feature(Feature::Eco).value;
  if (cmd.feature(Feature::Boost).has()) s.turbo = cmd.feature(Feature::Boost).value;
  if (cmd.feature(Feature::Sleep).has()) s.sleep = cmd.feature(Feature::Sleep).value;
  if (cmd.feature(Feature::Ion).has()) s.purifier = cmd.feature(Feature::Ion).value;
  if (cmd.feature(Feature::FollowMe).has()) s.follow_me = cmd.feature(Feature::FollowMe).value;
  if (cmd.feature(Feature::Freeze).has()) {
    s.freeze_protection = cmd.feature(Feature::Freeze).value;
  }
  if (cmd.feature(Feature::Beep).has()) {
    beep_ = cmd.feature(Feature::Beep).value;
    s.beep = beep_;
  }
  // Feature::Led is read-only here — the protocol only offers a flip, which is
  // exposed as the led_toggle action.

  uint8_t frame[64];
  const size_t len = buildSetStateFrame(frame, sizeof(frame), s, ++message_id_);
  if (len == 0) {
    state.setError("could not build set frame");
    return false;
  }

  uint8_t reply[128];
  size_t reply_len = 0;
  if (!transact(frame, len, reply, sizeof(reply), &reply_len, state)) return false;

  MideaState st;
  if (parseStateFrame(reply, reply_len, &st)) {
    cached_ = st;
    publish(st, state);
    return true;
  }
  // Some units acknowledge with something other than a state frame; fall back
  // to reading the unit rather than reporting a failure.
  return poll(state);
}

}  // namespace acbridge
