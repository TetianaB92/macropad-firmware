#include "provisioning.h"
#include "wifi_config.h"
#include "device_state.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

static DNSServer dns;
static WebServer server(80);
static bool active = false;
static char apNameBuf[24] = "Macropad-Setup";

static const char* PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Macropad setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#0c0c0e;color:#f4f4f5;
margin:0;padding:24px}
h1{color:#d97736;font-size:1.4rem}
label{display:block;margin:14px 0 6px;color:#a1a1aa;font-size:.85rem}
input{width:100%;box-sizing:border-box;padding:12px;border-radius:12px;
border:1px solid #2a2a30;background:#16161a;color:#fff;font-size:1rem}
button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:14px;
background:#d97736;color:#111;font-weight:700;font-size:1rem}
p{color:#71717a;line-height:1.45;font-size:.9rem}
</style>
</head>
<body>
<h1>Macropad setup</h1>
<p>Enter your home Wi‑Fi. Then open the Macropad website and type the code
shown on the device screen.</p>
<form method="POST" action="/save">
<label>Wi‑Fi name (SSID)</label>
<input name="ssid" required placeholder="HomeWiFi" autocomplete="off">
<label>Wi‑Fi password</label>
<input name="pass" type="password" placeholder="••••••••">
<label>App URL (usually leave as is)</label>
<input name="api" value="%API%" placeholder="https://your-app.vercel.app">
<button type="submit">Save &amp; connect</button>
</form>
</body>
</html>
)HTML";

static String htmlPage() {
  String page = PAGE_HTML;
  page.replace("%API%", WifiConfig::apiBaseUrl());
  return page;
}

static void handleRoot() { server.send(200, "text/html", htmlPage()); }

static void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String api = server.arg("api");
  ssid.trim();
  api.trim();
  if (ssid.length() < 1) {
    server.send(400, "text/plain", "SSID required");
    return;
  }
  WifiConfig::save(ssid.c_str(), pass.c_str(),
                   api.length() ? api.c_str() : WifiConfig::factoryApiDefault());
  server.send(200, "text/html",
              "<html><body style='background:#0c0c0e;color:#f4f4f5;font-family:"
              "system-ui;padding:24px'><h1 style='color:#d97736'>Saved</h1>"
              "<p>Device is rebooting and joining your Wi‑Fi…</p>"
              "<p>Then open the Macropad site → Devices and enter the code on "
              "screen.</p></body></html>");
  delay(800);
  ESP.restart();
}

static void handleCaptive() { server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void Provisioning::begin() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apNameBuf, sizeof(apNameBuf), "Macropad-%02X%02X", mac[4], mac[5]);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apNameBuf);
  delay(100);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Provisioning AP %s IP %s\n", apNameBuf, ip.toString().c_str());

  dns.start(53, "*", ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/generate_204", handleCaptive);       // Android
  server.on("/hotspot-detect.html", handleCaptive); // Apple
  server.on("/fwlink", handleCaptive);
  server.onNotFound(handleRoot);
  server.begin();

  active = true;
  DeviceState::setPhase(DevicePhase::Provisioning);
  Serial.printf("Provisioning UI: connect to %s\n", apNameBuf);
}

void Provisioning::loop() {
  if (!active) return;
  dns.processNextRequest();
  server.handleClient();
}

bool Provisioning::isActive() { return active; }
const char* Provisioning::apName() { return apNameBuf; }
