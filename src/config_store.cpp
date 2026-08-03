#include "config_store.h"

#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

namespace acbridge {

namespace {

constexpr const char* kNamespace = "esp32ac";
constexpr const char* kBlobKey = "config";

void generateBearer(char* out, size_t out_len) {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const size_t n = out_len - 1;
  for (size_t i = 0; i < n; i++) {
    out[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
  }
  out[n] = '\0';
}

}  // namespace

ConfigStore g_config;

bool ConfigStore::begin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
    // Namespace doesn't exist yet — first boot.
    cfg_ = Config{};
    loaded_ = false;
    generateBearer(cfg_.api.bearer, 33);
    save();
    return false;
  }

  Config stored{};
  const size_t read = prefs.getBytes(kBlobKey, &stored, sizeof(stored));
  prefs.end();

  if (read != sizeof(stored) || stored.version != kConfigVersion) {
    // Nothing usable stored (or a version we don't understand). Start clean
    // rather than half-applying a struct we can't interpret.
    cfg_ = Config{};
    loaded_ = false;
    generateBearer(cfg_.api.bearer, 33);
    save();
    return false;
  }

  cfg_ = stored;
  if (cfg_.api.bearer[0] == '\0') {
    generateBearer(cfg_.api.bearer, 33);
    save();
  }
  loaded_ = true;
  return true;
}

bool ConfigStore::save() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
  cfg_.version = kConfigVersion;
  const size_t written = prefs.putBytes(kBlobKey, &cfg_, sizeof(cfg_));
  prefs.end();
  return written == sizeof(cfg_);
}

bool ConfigStore::clear() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
  const bool ok = prefs.clear();
  prefs.end();
  cfg_ = Config{};
  loaded_ = false;
  return ok;
}

}  // namespace acbridge
