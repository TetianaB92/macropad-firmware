#include "wifi_config.h"
#include <Preferences.h>
#include <string.h>

#if defined(MACROPAD_PUBLIC_BUILD)
#include "secrets.h.example"
#elif __has_include("secrets.h")
#include "secrets.h"
#elif __has_include("secrets.h.example")
#include "secrets.h.example"
#endif

#ifndef FACTORY_API_BASE_URL
#ifdef API_BASE_URL
#define FACTORY_API_BASE_URL API_BASE_URL
#else
#define FACTORY_API_BASE_URL "https://macropad-tawny.vercel.app"
#endif
#endif

static Preferences wifiPrefs;
static char ssid_[33] = {0};
static char pass_[65] = {0};
static char api_[128] = {0};

void WifiConfig::init() {
  wifiPrefs.begin("wifi", true);
  String s = wifiPrefs.getString("ssid", "");
  String p = wifiPrefs.getString("pass", "");
  String a = wifiPrefs.getString("api", "");
  wifiPrefs.end();

  strncpy(ssid_, s.c_str(), sizeof(ssid_) - 1);
  strncpy(pass_, p.c_str(), sizeof(pass_) - 1);
  if (a.length() > 0)
    strncpy(api_, a.c_str(), sizeof(api_) - 1);
  else
    strncpy(api_, FACTORY_API_BASE_URL, sizeof(api_) - 1);

  Serial.printf("WifiConfig: ssid=%s api=%s\n",
                ssid_[0] ? ssid_ : "(setup needed)", api_);
}

bool WifiConfig::hasWifi() { return ssid_[0] != 0; }
const char* WifiConfig::ssid() { return ssid_; }
const char* WifiConfig::password() { return pass_; }
const char* WifiConfig::apiBaseUrl() { return api_; }
const char* WifiConfig::factoryApiDefault() { return FACTORY_API_BASE_URL; }

void WifiConfig::save(const char* ssid, const char* password,
                      const char* apiUrl) {
  strncpy(ssid_, ssid ? ssid : "", sizeof(ssid_) - 1);
  strncpy(pass_, password ? password : "", sizeof(pass_) - 1);
  if (apiUrl && apiUrl[0])
    strncpy(api_, apiUrl, sizeof(api_) - 1);
  else if (api_[0] == 0)
    strncpy(api_, FACTORY_API_BASE_URL, sizeof(api_) - 1);

  // strip trailing slash
  size_t n = strlen(api_);
  while (n > 0 && api_[n - 1] == '/') {
    api_[--n] = 0;
  }

  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", ssid_);
  wifiPrefs.putString("pass", pass_);
  wifiPrefs.putString("api", api_);
  wifiPrefs.end();
  Serial.println("WifiConfig: saved to NVS");
}

void WifiConfig::clear() {
  ssid_[0] = pass_[0] = 0;
  strncpy(api_, FACTORY_API_BASE_URL, sizeof(api_) - 1);
  wifiPrefs.begin("wifi", false);
  wifiPrefs.clear();
  wifiPrefs.end();
}
