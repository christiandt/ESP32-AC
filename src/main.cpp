// ESP32-AC — a XIAO ESP32C6 that holds the credentials for both AC units,
// polls them over the LAN, and re-exposes them as a REST API (and, from
// phase 4, as HomeKit accessories).

#include <Arduino.h>
#include <WiFi.h>

#include "ac_registry.h"
#include "config_store.h"
#include "drivers/electrolux.h"
#include "drivers/midea.h"
#include "drivers/mock.h"
#include "homekit.h"
#include "rest_api.h"
#include "selftest.h"

using namespace acbridge;

namespace {

constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr const char* kApSsid = "esp32-ac-setup";
constexpr const char* kApPassword = "acsetup123";

// Drivers are built from stored config, so they outlive setup() and are
// allocated once here rather than being globals that can't see the config.
void registerDrivers(const Config& cfg) {
  if (cfg.midea.enabled && cfg.midea.ip[0] != '\0') {
    g_registry.add(new MideaDriver(cfg.midea));
    Serial.printf("Midea at %s (%s)\n", cfg.midea.ip, cfg.midea.isV3() ? "V3" : "V2");
  } else {
    // Keep the unit present so the REST surface is stable, just obviously fake.
    g_registry.add(new MockDriver("midea", "Midea (unconfigured)"));
    Serial.println("Midea not configured — using a mock");
  }

  if (cfg.electrolux.enabled && cfg.electrolux.ip[0] != '\0') {
    g_registry.add(new ElectroluxDriver(cfg.electrolux));
    Serial.printf("Electrolux at %s\n", cfg.electrolux.ip);
  } else {
    // Keep the unit present so the REST surface is stable, just obviously fake.
    g_registry.add(new MockDriver("electrolux", "Electrolux (unconfigured)"));
    Serial.println("Electrolux not configured — using a mock");
  }
}

void startProvisioningAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  restApiSetProvisioning(true);
  Serial.println();
  Serial.println("== Provisioning mode ==");
  Serial.printf("  Join WiFi '%s' (password '%s')\n", kApSsid, kApPassword);
  Serial.printf("  Then POST your config to http://%s/api/config\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println("  Generate it with: python3 tools/export_config.py");
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32-AC starting");

  runSelfTest();

  const bool provisioned = g_config.begin();
  const Config& cfg = g_config.get();

  if (!provisioned) {
    Serial.println("No stored config — first boot.");
  }
  Serial.printf("REST bearer token: %s\n", cfg.api.bearer);

  registerDrivers(cfg);

  if (cfg.wifi.valid()) {
    restApiSetProvisioning(false);

    // HomeSpan owns the station — we deliberately do not connect it ourselves.
    //
    // HomeSpan starts mDNS and the HAP server from Span::configureNetwork(),
    // which runs only when its own `connected` counter first reaches 1: the
    // first GOT_IP event it sees, and never again. If the station is already up
    // when HomeSpan starts, its counter is still 0, so it calls WiFi.begin()
    // regardless — and the event that races out of that transition arrives
    // while localIP() is still 0.0.0.0. mDNS and the HAP server are then bound
    // to nothing for the rest of the uptime, which shows up as an accessory
    // stuck on "No response" in the Home app while the REST API carries on
    // working perfectly. Disconnecting first isn't enough; only letting
    // HomeSpan make the one and only connection avoids the race.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // the driver tasks poll on a timer; latency > power here
    homekitBegin(cfg);

    Serial.printf("Waiting for HomeSpan to join '%s' ...\n", cfg.wifi.ssid);
    const uint32_t deadline = millis() + kWifiTimeoutMs;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
      homekitLoop();  // HomeSpan connects from its own poll loop
      delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      // Not fatal: HomeSpan keeps retrying, and the drivers back off until
      // there is a route. Bad credentials no longer fall back to the setup AP,
      // so recover by clearing NVS or re-provisioning over serial.
      Serial.println("Still not connected — HomeSpan will keep retrying.");
    }

    g_registry.start();
  } else {
    startProvisioningAp();
    // Deliberately no driver tasks in provisioning mode: there is no route to
    // the ACs yet, so they would just log timeouts.
  }

  if (restApiStart()) {
    Serial.println("REST API listening on port 80");
  } else {
    Serial.println("REST API failed to start");
  }
}

void loop() {
  // The AC drivers each have their own task; this one belongs to HomeSpan,
  // which is not thread-safe and owns every characteristic read and write.
  if (homekitStarted()) {
    homekitLoop();
  } else {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
