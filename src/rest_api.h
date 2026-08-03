// LAN REST API.
//
// Runs on esp_http_server, which owns its own task. A synchronous Arduino
// WebServer polled from loop() would fight HomeSpan for the single core, so
// don't swap it for one.
//
//   GET   /api/health
//   GET   /api/units                      -> [{id, vendor, name, online}]
//   GET   /api/units/{id}                 -> unified state
//   PATCH /api/units/{id}                 -> partial state; only present keys
//   POST  /api/units/{id}/actions/{name}  -> led_toggle | self_clean | clear_timer
//   GET   /api/config                     -> redacted config
//   POST  /api/config                     -> provisioning
//
// Every route requires `Authorization: Bearer <token>` except while the device
// is in provisioning mode, where it is serving its own private soft-AP and no
// token has been handed out yet.

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace acbridge {

bool restApiStart(uint16_t port = 80);
void restApiStop();

// In provisioning mode POST /api/config is unauthenticated, because the caller
// is already on the device's private soft-AP and has no token to present.
void restApiSetProvisioning(bool provisioning);

}  // namespace acbridge
