#include "rest_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <string.h>

#include "ac_registry.h"
#include "config_store.h"

namespace acbridge {
namespace {

httpd_handle_t g_server = nullptr;
bool g_provisioning = false;

constexpr size_t kMaxBody = 2048;
constexpr const char* kUnitPrefix = "/api/units/";

// --------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------

void copyStr(char* dst, size_t cap, const char* src) {
  if (src == nullptr) return;
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void sendJson(httpd_req_t* req, const char* status, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, out.c_str());
}

esp_err_t sendError(httpd_req_t* req, const char* status, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(req, status, doc);
  return ESP_OK;
}

// Length-independent compare, so a token can't be recovered a byte at a time.
bool secureEquals(const char* a, const char* b) {
  const size_t la = strlen(a);
  const size_t lb = strlen(b);
  unsigned char diff = (la == lb) ? 0 : 1;
  const size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

bool authorized(httpd_req_t* req) {
  const Config& cfg = g_config.get();
  char hdr[160];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
  if (strncmp(hdr, "Bearer ", 7) != 0) return false;
  return secureEquals(hdr + 7, cfg.api.bearer);
}

// Guard for every route. Returns true when the request may proceed.
bool guard(httpd_req_t* req, bool allow_in_provisioning = false) {
  if (g_provisioning && allow_in_provisioning) return true;
  if (authorized(req)) return true;
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
  sendError(req, "401 Unauthorized", "missing or invalid bearer token");
  return false;
}

bool readBody(httpd_req_t* req, char* buf, size_t cap, size_t* out_len) {
  const size_t total = req->content_len;
  if (total >= cap) return false;
  size_t got = 0;
  while (got < total) {
    const int r = httpd_req_recv(req, buf + got, total - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  buf[got] = '\0';
  *out_len = got;
  return true;
}

// Copies the path segment after /api/units/, minus any query string.
bool unitPath(httpd_req_t* req, char* buf, size_t cap) {
  const size_t prefix = strlen(kUnitPrefix);
  if (strncmp(req->uri, kUnitPrefix, prefix) != 0) return false;
  const char* tail = req->uri + prefix;
  const char* q = strchr(tail, '?');
  const size_t n = q ? static_cast<size_t>(q - tail) : strlen(tail);
  if (n == 0 || n >= cap) return false;
  memcpy(buf, tail, n);
  buf[n] = '\0';
  return true;
}

// --------------------------------------------------------------------------
// Serialization
// --------------------------------------------------------------------------

template <typename T>
void putOpt(JsonObject o, const char* key, const Opt<T>& v) {
  if (v.has()) {
    o[key] = v.value;
  } else {
    o[key] = nullptr;
  }
}

void putFan(JsonObject o, const Opt<FanSpeed>& v) {
  if (!v.has()) {
    o["fan"] = nullptr;
  } else if (v.value.is_percent) {
    o["fan"] = v.value.percent;
  } else {
    o["fan"] = fanPresetName(v.value.preset);
  }
}

void stateToJson(const AcDriver& drv, const AcState& st, JsonObject o) {
  o["id"] = drv.id();
  o["vendor"] = vendorName(drv.vendor());
  o["name"] = drv.name();
  o["online"] = st.online;

  putOpt(o, "power", st.power);
  if (st.mode.has()) {
    o["mode"] = modeName(st.mode.value);
  } else {
    o["mode"] = nullptr;
  }
  putOpt(o, "target_temp", st.target_temp);
  putOpt(o, "indoor_temp", st.indoor_temp);
  putOpt(o, "outdoor_temp", st.outdoor_temp);
  putOpt(o, "humidity", st.humidity);
  putFan(o, st.fan);
  if (st.swing.has()) {
    o["swing"] = swingName(st.swing.value);
  } else {
    o["swing"] = nullptr;
  }

  JsonObject feats = o["features"].to<JsonObject>();
  for (size_t i = 0; i < kFeatureCount; i++) {
    const Feature f = static_cast<Feature>(i);
    if (!drv.supportsFeature(f)) continue;
    putOpt(feats, featureName(f), st.features[i]);
  }

  o["min_target"] = st.min_target;
  o["max_target"] = st.max_target;
  o["age_ms"] = st.updated_ms == 0 ? 0 : (millis() - st.updated_ms);
  if (st.error[0] != '\0') {
    o["error"] = st.error;
  } else {
    o["error"] = nullptr;
  }
}

// --------------------------------------------------------------------------
// Command parsing
//
// Unknown enum values are rejected rather than silently coerced. The Python
// original had the opposite behaviour: an unrecognised name made dict.get()
// return None and sent a literal {"ac_mode":None} to the device
// (electrolux/cli.py:108-148).
// --------------------------------------------------------------------------

bool parseCommand(JsonObjectConst body, const AcDriver& drv, const AcState& st, AcCommand& cmd,
                  char* err, size_t err_len) {
  JsonVariantConst v;

  v = body["power"];
  if (!v.isNull()) {
    if (!v.is<bool>()) {
      snprintf(err, err_len, "power must be a boolean");
      return false;
    }
    cmd.power.set(v.as<bool>());
  }

  v = body["mode"];
  if (!v.isNull()) {
    Mode m;
    if (!v.is<const char*>() || !modeFromName(v.as<const char*>(), m)) {
      snprintf(err, err_len, "mode must be one of auto|cool|dry|heat|fan");
      return false;
    }
    cmd.mode.set(m);
  }

  v = body["target_temp"];
  if (!v.isNull()) {
    if (!v.is<float>()) {
      snprintf(err, err_len, "target_temp must be a number");
      return false;
    }
    const float t = v.as<float>();
    if (t < st.min_target || t > st.max_target) {
      snprintf(err, err_len, "target_temp must be between %g and %g", st.min_target, st.max_target);
      return false;
    }
    cmd.target_temp.set(t);
  }

  v = body["fan"];
  if (!v.isNull()) {
    if (v.is<const char*>()) {
      FanPreset p;
      if (!fanPresetFromName(v.as<const char*>(), p)) {
        snprintf(err, err_len, "fan must be auto|silent|low|medium|high|turbo or 1-100");
        return false;
      }
      cmd.fan.set(FanSpeed::ofPreset(p));
    } else if (v.is<int>()) {
      const int pct = v.as<int>();
      if (pct < 1 || pct > 100) {
        snprintf(err, err_len, "fan percentage must be between 1 and 100");
        return false;
      }
      cmd.fan.set(FanSpeed::ofPercent(static_cast<uint8_t>(pct)));
    } else {
      snprintf(err, err_len, "fan must be a name or a percentage");
      return false;
    }
  }

  v = body["swing"];
  if (!v.isNull()) {
    Swing s;
    if (!v.is<const char*>() || !swingFromName(v.as<const char*>(), s)) {
      snprintf(err, err_len, "swing must be one of off|vertical|horizontal|both");
      return false;
    }
    cmd.swing.set(s);
  }

  v = body["features"];
  if (!v.isNull()) {
    if (!v.is<JsonObjectConst>()) {
      snprintf(err, err_len, "features must be an object");
      return false;
    }
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      Feature f;
      if (!featureFromName(kv.key().c_str(), f)) {
        snprintf(err, err_len, "unknown feature '%s'", kv.key().c_str());
        return false;
      }
      if (!drv.supportsFeature(f)) {
        snprintf(err, err_len, "this unit does not support '%s'", kv.key().c_str());
        return false;
      }
      if (!kv.value().is<bool>()) {
        snprintf(err, err_len, "feature '%s' must be a boolean", kv.key().c_str());
        return false;
      }
      cmd.feature(f).set(kv.value().as<bool>());
    }
  }

  return true;
}

// --------------------------------------------------------------------------
// Handlers
// --------------------------------------------------------------------------

esp_err_t handleHealth(httpd_req_t* req) {
  if (!guard(req, /*allow_in_provisioning=*/true)) return ESP_OK;
  JsonDocument doc;
  doc["ok"] = true;
  doc["provisioning"] = g_provisioning;
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["units"] = g_registry.count();
  sendJson(req, "200 OK", doc);
  return ESP_OK;
}

esp_err_t handleUnitList(httpd_req_t* req) {
  if (!guard(req)) return ESP_OK;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < g_registry.count(); i++) {
    AcDriver* drv = g_registry.driverAt(i);
    AcState st;
    g_registry.snapshot(i, st);
    JsonObject o = arr.add<JsonObject>();
    o["id"] = drv->id();
    o["vendor"] = vendorName(drv->vendor());
    o["name"] = drv->name();
    o["online"] = st.online;
  }
  sendJson(req, "200 OK", doc);
  return ESP_OK;
}

esp_err_t handleUnitGet(httpd_req_t* req) {
  if (!guard(req)) return ESP_OK;
  char path[64];
  if (!unitPath(req, path, sizeof(path))) return sendError(req, "404 Not Found", "unknown unit");
  // GET only serves the unit itself, never a sub-resource.
  if (strchr(path, '/') != nullptr) return sendError(req, "404 Not Found", "unknown resource");

  const int idx = g_registry.indexOf(path);
  if (idx < 0) return sendError(req, "404 Not Found", "unknown unit");

  AcState st;
  g_registry.snapshot(static_cast<size_t>(idx), st);

  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  stateToJson(*g_registry.driverAt(static_cast<size_t>(idx)), st, o);
  sendJson(req, "200 OK", doc);
  return ESP_OK;
}

esp_err_t handleUnitPatch(httpd_req_t* req) {
  if (!guard(req)) return ESP_OK;
  char path[64];
  if (!unitPath(req, path, sizeof(path)) || strchr(path, '/') != nullptr) {
    return sendError(req, "404 Not Found", "unknown unit");
  }
  const int idx = g_registry.indexOf(path);
  if (idx < 0) return sendError(req, "404 Not Found", "unknown unit");

  char body[kMaxBody];
  size_t len = 0;
  if (!readBody(req, body, sizeof(body), &len)) {
    return sendError(req, "413 Payload Too Large", "request body too large or truncated");
  }

  JsonDocument doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) {
    return sendError(req, "400 Bad Request", "body is not valid JSON");
  }
  if (!doc.is<JsonObjectConst>()) {
    return sendError(req, "400 Bad Request", "body must be a JSON object");
  }

  AcDriver* drv = g_registry.driverAt(static_cast<size_t>(idx));
  AcState st;
  g_registry.snapshot(static_cast<size_t>(idx), st);

  AcCommand cmd;
  char err[96] = {0};
  if (!parseCommand(doc.as<JsonObjectConst>(), *drv, st, cmd, err, sizeof(err))) {
    return sendError(req, "400 Bad Request", err);
  }
  if (cmd.empty()) return sendError(req, "400 Bad Request", "no recognised fields to apply");

  if (!g_registry.submit(static_cast<size_t>(idx), cmd)) {
    return sendError(req, "503 Service Unavailable", "command queue is full");
  }

  JsonDocument out;
  out["accepted"] = true;
  out["id"] = drv->id();
  sendJson(req, "202 Accepted", out);
  return ESP_OK;
}

esp_err_t handleUnitAction(httpd_req_t* req) {
  if (!guard(req)) return ESP_OK;
  char path[80];
  if (!unitPath(req, path, sizeof(path))) return sendError(req, "404 Not Found", "unknown unit");

  // Expect "<id>/actions/<name>".
  char* slash = strchr(path, '/');
  if (slash == nullptr) return sendError(req, "404 Not Found", "expected /actions/<name>");
  *slash = '\0';
  const char* rest = slash + 1;
  if (strncmp(rest, "actions/", 8) != 0) {
    return sendError(req, "404 Not Found", "expected /actions/<name>");
  }
  const char* action_name = rest + 8;

  const int idx = g_registry.indexOf(path);
  if (idx < 0) return sendError(req, "404 Not Found", "unknown unit");

  Action action;
  if (!actionFromName(action_name, action)) {
    return sendError(req, "400 Bad Request", "unknown action");
  }

  AcDriver* drv = g_registry.driverAt(static_cast<size_t>(idx));
  if (!drv->supportsAction(action)) {
    return sendError(req, "400 Bad Request", "this unit does not support that action");
  }

  AcCommand cmd;
  cmd.action = action;
  if (!g_registry.submit(static_cast<size_t>(idx), cmd)) {
    return sendError(req, "503 Service Unavailable", "command queue is full");
  }

  JsonDocument out;
  out["accepted"] = true;
  out["id"] = drv->id();
  out["action"] = actionName(action);
  sendJson(req, "202 Accepted", out);
  return ESP_OK;
}

esp_err_t handleConfigGet(httpd_req_t* req) {
  if (!guard(req, /*allow_in_provisioning=*/true)) return ESP_OK;
  const Config& cfg = g_config.get();

  JsonDocument doc;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = cfg.wifi.ssid;
  wifi["configured"] = cfg.wifi.valid();

  // Secrets are never echoed back — only whether they are set.
  JsonObject api = doc["api"].to<JsonObject>();
  api["bearer_set"] = cfg.api.bearer[0] != '\0';

  JsonObject midea = doc["midea"].to<JsonObject>();
  midea["enabled"] = cfg.midea.enabled;
  midea["name"] = cfg.midea.name;
  midea["ip"] = cfg.midea.ip;
  midea["device_id"] = cfg.midea.device_id;
  midea["port"] = cfg.midea.port;
  midea["v3"] = cfg.midea.isV3();
  midea["min_target"] = cfg.midea.min_target;
  midea["max_target"] = cfg.midea.max_target;
  JsonArray caps = midea["caps"].to<JsonArray>();
  for (size_t i = 0; i < kFeatureCount; i++) {
    const Feature f = static_cast<Feature>(i);
    if (capsHas(cfg.midea.caps, f)) caps.add(featureName(f));
  }

  JsonObject elux = doc["electrolux"].to<JsonObject>();
  elux["enabled"] = cfg.electrolux.enabled;
  elux["name"] = cfg.electrolux.name;
  elux["ip"] = cfg.electrolux.ip;

  sendJson(req, "200 OK", doc);
  return ESP_OK;
}

void deferredRestart(void*) {
  vTaskDelay(pdMS_TO_TICKS(750));
  ESP.restart();
}

esp_err_t handleConfigPost(httpd_req_t* req) {
  if (!guard(req, /*allow_in_provisioning=*/true)) return ESP_OK;

  char body[kMaxBody];
  size_t len = 0;
  if (!readBody(req, body, sizeof(body), &len)) {
    return sendError(req, "413 Payload Too Large", "request body too large or truncated");
  }

  JsonDocument doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) {
    return sendError(req, "400 Bad Request", "body is not valid JSON");
  }

  Config& cfg = g_config.get();

  JsonVariantConst wifi = doc["wifi"];
  if (!wifi.isNull()) {
    if (wifi["ssid"].is<const char*>()) copyStr(cfg.wifi.ssid, sizeof(cfg.wifi.ssid), wifi["ssid"]);
    if (wifi["psk"].is<const char*>()) copyStr(cfg.wifi.psk, sizeof(cfg.wifi.psk), wifi["psk"]);
  }

  JsonVariantConst api = doc["api"];
  if (!api.isNull() && api["bearer"].is<const char*>()) {
    copyStr(cfg.api.bearer, sizeof(cfg.api.bearer), api["bearer"]);
  }

  JsonVariantConst midea = doc["midea"];
  if (!midea.isNull()) {
    MideaConfig& m = cfg.midea;
    if (midea["enabled"].is<bool>()) m.enabled = midea["enabled"];
    if (midea["name"].is<const char*>()) copyStr(m.name, sizeof(m.name), midea["name"]);
    if (midea["ip"].is<const char*>()) copyStr(m.ip, sizeof(m.ip), midea["ip"]);
    if (midea["device_id"].is<uint32_t>()) m.device_id = midea["device_id"];
    if (midea["port"].is<uint16_t>()) m.port = midea["port"];
    if (midea["token"].is<const char*>()) copyStr(m.token, sizeof(m.token), midea["token"]);
    if (midea["key"].is<const char*>()) copyStr(m.key, sizeof(m.key), midea["key"]);
    if (midea["min_target"].is<float>()) m.min_target = midea["min_target"];
    if (midea["max_target"].is<float>()) m.max_target = midea["max_target"];
    if (midea["caps"].is<JsonArrayConst>()) {
      m.caps = 0;
      for (JsonVariantConst c : midea["caps"].as<JsonArrayConst>()) {
        Feature f;
        if (c.is<const char*>() && featureFromName(c.as<const char*>(), f)) {
          m.caps = capsSet(m.caps, f, true);
        }
      }
    }
  }

  JsonVariantConst elux = doc["electrolux"];
  if (!elux.isNull()) {
    ElectroluxConfig& e = cfg.electrolux;
    if (elux["enabled"].is<bool>()) e.enabled = elux["enabled"];
    if (elux["name"].is<const char*>()) copyStr(e.name, sizeof(e.name), elux["name"]);
    if (elux["ip"].is<const char*>()) copyStr(e.ip, sizeof(e.ip), elux["ip"]);
  }

  if (!g_config.save()) {
    return sendError(req, "500 Internal Server Error", "could not write config to NVS");
  }

  JsonDocument out;
  out["ok"] = true;
  out["rebooting"] = true;
  // Hand the token back exactly once, while still on the private soft-AP —
  // otherwise there is no way to learn it without a serial console.
  if (g_provisioning) out["bearer"] = cfg.api.bearer;
  sendJson(req, "200 OK", out);

  // Drivers read their config at construction, so a restart is the honest way
  // to apply it.
  xTaskCreate(deferredRestart, "cfg_restart", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

}  // namespace

void restApiSetProvisioning(bool provisioning) { g_provisioning = provisioning; }

bool restApiStart(uint16_t port) {
  if (g_server != nullptr) return true;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_uri_handlers = 12;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  // Needed so "/api/units/*" matches sub-paths.
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&g_server, &config) != ESP_OK) {
    g_server = nullptr;
    return false;
  }

  const httpd_uri_t routes[] = {
      {"/api/health", HTTP_GET, handleHealth, nullptr},
      {"/api/units", HTTP_GET, handleUnitList, nullptr},
      {"/api/units/*", HTTP_GET, handleUnitGet, nullptr},
      {"/api/units/*", HTTP_PATCH, handleUnitPatch, nullptr},
      {"/api/units/*", HTTP_POST, handleUnitAction, nullptr},
      {"/api/config", HTTP_GET, handleConfigGet, nullptr},
      {"/api/config", HTTP_POST, handleConfigPost, nullptr},
  };
  for (const httpd_uri_t& r : routes) httpd_register_uri_handler(g_server, &r);

  return true;
}

void restApiStop() {
  if (g_server == nullptr) return;
  httpd_stop(g_server);
  g_server = nullptr;
}

}  // namespace acbridge
