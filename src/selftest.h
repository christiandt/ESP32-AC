// Boot-time verification of the crypto paths.
//
// The framing is covered by `pio test -e native`, but that environment has no
// mbedtls, so the encryption, hashing and key derivation are only exercisable
// on the device. This runs a handful of vectors generated from msmart and the
// Broadlink library at startup — cheap insurance that a key length, an IV or a
// signed byte range didn't get wired up wrong.

#pragma once

#include <stdbool.h>
#include <stddef.h>

namespace acbridge {

struct SelfTestResult {
  bool passed = false;
  char detail[96] = {0};
};

// Runs every check and logs to serial. Safe to call before WiFi is up.
SelfTestResult runSelfTest();

// Result of the run performed at boot, for GET /api/health.
const SelfTestResult& lastSelfTest();

}  // namespace acbridge
