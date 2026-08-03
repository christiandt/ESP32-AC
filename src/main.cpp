// ESP32-AC — a XIAO ESP32C6 that holds the credentials for both AC units,
// polls them over the LAN, and re-exposes them as a REST API (and, from
// phase 4, as HomeKit accessories).

#include <Arduino.h>
#include <WiFi.h>

#include "ac_registry.h"
#include "config_store.h"
#include "drivers/mock.h"
#include "rest_api.h"

using namespace acbridge;

namespace {

// Phase 1 stands both units up as mocks so the REST shape, the task model and
// the HomeKit mapping can be exercised before either protocol exists. Phases 2
// and 3 swap these for MideaDriver and ElectroluxDriver; the ids and names are
// already the final ones.
MockDriver g_midea("midea", "Midea Porta Split");
MockDriver g_electrolux("electrolux", "Electrolux");

constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr const char* kApSsid = "esp32-ac-setup";
constexpr const char* kApPassword = "acsetup123";

bool connectWifi(const WifiConfig& wifi) {
  Serial.printf("Connecting to '%s' ...\n", wifi.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // the driver tasks poll on a timer; latency > power here
  WiFi.begin(wifi.ssid, wifi.psk);

  const uint32_t deadline = millis() + kWifiTimeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect timed out.");
    return false;
  }
  Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
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

  const bool provisioned = g_config.begin();
  const Config& cfg = g_config.get();

  if (!provisioned) {
    Serial.println("No stored config — first boot.");
  }
  Serial.printf("REST bearer token: %s\n", cfg.api.bearer);

  g_registry.add(&g_midea);
  g_registry.add(&g_electrolux);

  if (cfg.wifi.valid() && connectWifi(cfg.wifi)) {
    restApiSetProvisioning(false);
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
  // Everything lives on its own task. Phase 4 puts homeSpan.poll() here.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
