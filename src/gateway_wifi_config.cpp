/**
 * gateway_wifi_config.cpp
 *
 * Non-blocking WiFi state machine for the Power Telemetry Gateway.
 *
 * State machine:
 *
 *   AP_ACTIVE ──(creds exist at boot)──► AP_STA_CONNECTING
 *       ▲                                       │
 *       │ STA drops / timeout / forget          │ STA connects
 *       │                                       ▼
 *       └──────────────────────────── STA_CONNECTED (AP off)
 *
 * AP is always on except while STA is actively connected.
 * DNS catch-all redirects all queries to 192.168.4.1 while AP is active.
 */

#include "gateway_wifi_config.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// -- State -------------------------------------------------------------------------------------
enum WifiState
{
  WIFI_STATE_AP_ACTIVE,         // AP on, no STA
  WIFI_STATE_AP_STA_CONNECTING, // AP on + STA connecting in background
  WIFI_STATE_STA_CONNECTED,     // STA up, AP off
};

static WifiState s_state        = WIFI_STATE_AP_ACTIVE;
static uint32_t  s_connectStart = 0;
static bool      s_apActive     = false;
static bool      s_staConnected = false;
static bool      s_apEventRegistered = false; // guard: register AP_START handler once
static DNSServer s_dns;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const uint8_t   DNS_PORT = 53;

// -- NVS -------------------------------------------------------
static void nvsLoad(String &ssid, String &pass)
{
  Preferences p;
  p.begin("wifi-cfg", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
}

static void nvsSave(const String &ssid, const String &pass)
{
  Preferences p;
  p.begin("wifi-cfg", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

static void nvsClear()
{
  Preferences p;
  p.begin("wifi-cfg", false);
  p.clear();
  p.end();
}

// -- AP helpers ----------------------------------------------------------------
static void startAP()
{
  if (s_apActive) return;

  // Register mDNS-on-AP-start handler exactly once across all startAP() calls.
  if (!s_apEventRegistered) {
    WiFi.onEvent([](WiFiEvent_t /*event*/, WiFiEventInfo_t /*info*/) {
      MDNS.begin("telemeter");
      Serial.println("[WIFI] mDNS: http://telemeter.local (AP mode)");
    }, ARDUINO_EVENT_WIFI_AP_START);
    s_apEventRegistered = true;
  }

  WiFi.enableAP(true);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  bool ok = WiFi.softAP(CONFIG_AP_SSID); // open network
  s_apActive = true;
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(DNS_PORT, "*", AP_IP);
  Serial.printf("[WIFI] AP %s · SSID: %s · IP: %s\n",
                ok ? "UP" : "FAIL", CONFIG_AP_SSID,
                WiFi.softAPIP().toString().c_str());
  configASSERT(ok);
}

static void stopAP()
{
  if (!s_apActive) return;
  MDNS.end();
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.enableAP(false);
  s_apActive = false;
  Serial.println("[WIFI] AP disabled — STA is up");
}

// -- STA helper -----------------------------------------------------
static void beginSTA(const String &ssid, const String &pass)
{
  WiFi.enableSTA(true);
  WiFi.setHostname("telemeter");
  WiFi.setAutoReconnect(true);

  if (pass.isEmpty())
    WiFi.begin(ssid.c_str());
  else
    WiFi.begin(ssid.c_str(), pass.c_str());

  s_connectStart = millis();
  s_staConnected = false;
  Serial.printf("[WIFI] STA connecting to \"%s\" ...\n", ssid.c_str());
}

// -- HTTP REST handlers ----------------------------------
static void handleInfo(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  doc["version"]      = FW_VERSION_STR;
  doc["apSsid"]       = CONFIG_AP_SSID;
  doc["apIp"]         = AP_IP.toString();
  doc["apActive"]     = s_apActive;
  doc["staConnected"] = s_staConnected;
  if (s_staConnected)
  {
    doc["staSSID"] = WiFi.SSID();
    doc["staIP"]   = WiFi.localIP().toString();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// Async scan: first call triggers the scan and returns {scanning:true}.
// Client retries until scanning:false with the full network list.
static void handleScan(AsyncWebServerRequest *req)
{
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED)
  {
    WiFi.scanNetworks(/*async=*/true);
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  if (n == WIFI_SCAN_RUNNING)
  {
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  JsonDocument doc;
  doc["scanning"] = false;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++)
  {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"]   = WiFi.SSID(i);
    net["rssi"]   = WiFi.RSSI(i);
    net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleConnect(AsyncWebServerRequest *req)
{
  String ssid = "", pass = "";
  if (req->hasParam("ssid", true))
    ssid = req->getParam("ssid", true)->value();
  if (req->hasParam("password", true))
    pass = req->getParam("password", true)->value();
  ssid.trim();

  if (ssid.isEmpty())
  {
    req->send(400, "application/json",
              "{\"status\":\"error\",\"message\":\"SSID cannot be empty\"}");
    return;
  }

  nvsSave(ssid, pass);

  // Restore AP before dropping STA so the device stays reachable throughout
  // the transition — even if the new credentials fail.
  if (s_staConnected)
  {
    MDNS.end();
    s_staConnected = false;
  }
  startAP(); // no-op if AP is already active

  WiFi.disconnect(false);
  beginSTA(ssid, pass);
  s_state = WIFI_STATE_AP_STA_CONNECTING;
  req->send(200, "application/json", "{\"status\":\"connecting\"}");
}

/** GET /api/wifistatus — polled by dashboard after POST /api/connect.
 *  Distinct from GET /api/status (dashboard gateway-info endpoint). */
static void handleWifiStatus(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  doc["apActive"]   = s_apActive;
  doc["connecting"] = (s_state == WIFI_STATE_AP_STA_CONNECTING);
  doc["connected"]  = s_staConnected;
  if (s_staConnected)
  {
    doc["ip"]   = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleDisconnect(AsyncWebServerRequest *req)
{
  WiFi.setAutoReconnect(false); // make disconnect sticky
  WiFi.disconnect(false);
  s_staConnected = false;

  if (s_state == WIFI_STATE_STA_CONNECTED)
  {
    // AP was off — restore it so the client isn't left stranded.
    MDNS.end();
    startAP();
    Serial.println("[WIFI] STA disconnected by user — AP restored");
  }
  else
  {
    Serial.println("[WIFI] STA connect attempt cancelled by user");
  }

  s_state = WIFI_STATE_AP_ACTIVE;
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleForget(AsyncWebServerRequest *req)
{
  nvsClear();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  s_staConnected = false;
  MDNS.end();
  startAP(); // no-op if AP is already active
  s_state = WIFI_STATE_AP_ACTIVE;
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[WIFI] Credentials cleared — AP restored");
}

/** Catch-all — redirects OS captive-portal probes to the config page.
 *  Only redirects when AP is active; falls through to 404 otherwise. */
static void handleCatchAll(AsyncWebServerRequest *req)
{
  if (s_apActive)
    req->redirect("http://telemeter.local");
  else
    req->send(404, "text/plain", "Not found");
}

// -- Public API -----------------------------------------------------------------------

void wifiConfigBegin()
{
  // Check force-AP pin (hold GPIO0 / BOOT button at power-on to skip saved creds)
  pinMode(CONFIG_PIN, INPUT);
  delay(50);
  bool forceAP = (digitalRead(CONFIG_PIN) == LOW);
  if (forceAP)
    Serial.printf("[WIFI] GPIO%d LOW — skipping saved credentials\n", CONFIG_PIN);

  WiFi.mode(WIFI_AP_STA);
  startAP();
  s_state = WIFI_STATE_AP_ACTIVE;

  if (!forceAP)
  {
    String ssid, pass;
    nvsLoad(ssid, pass);
    if (!ssid.isEmpty())
    {
      beginSTA(ssid, pass);
      s_state = WIFI_STATE_AP_STA_CONNECTING;
    }
    else
    {
      Serial.println("[WIFI] No saved credentials — staying in AP mode");
    }
  }
}

void wifiRegisterRoutes(AsyncWebServer &server)
{
  server.on("/api/info",       HTTP_GET,  handleInfo);
  server.on("/api/scan",       HTTP_GET,  handleScan);
  server.on("/api/connect",    HTTP_POST, handleConnect);
  server.on("/api/wifistatus", HTTP_GET,  handleWifiStatus);
  server.on("/api/disconnect", HTTP_GET,  handleDisconnect);
  server.on("/api/forget",     HTTP_GET,  handleForget);
  Serial.println("[WIFI] Config routes registered");
}

void wifiConfigLoop()
{
  if (s_apActive) s_dns.processNextRequest();

  switch (s_state)
  {
  case WIFI_STATE_AP_STA_CONNECTING:
    if (WiFi.status() == WL_CONNECTED)
    {
      s_staConnected = true;
      stopAP(); // stopAP() calls MDNS.end()
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter");
      Serial.printf("[WIFI] STA connected | IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      Serial.println("[WIFI] mDNS: http://telemeter.local");
    }
    else if (millis() - s_connectStart > WIFI_CONNECT_TIMEOUT_MS)
    {
      Serial.println("[WIFI] STA timed out - staying in AP mode");
      WiFi.disconnect(false);
      s_state = WIFI_STATE_AP_ACTIVE;
    }
    break;

  case WIFI_STATE_STA_CONNECTED:
    if (WiFi.status() != WL_CONNECTED)
    {
      s_staConnected = false;
      MDNS.end();
      startAP();
      s_state = WIFI_STATE_AP_ACTIVE;
      Serial.println("[WIFI] STA dropped - AP restored as fallback");
      // setAutoReconnect(true) is still set; SDK will reconnect in background
      // and AP_ACTIVE case below will promote back to STA_CONNECTED.
    }
    break;

  case WIFI_STATE_AP_ACTIVE:
    // SDK auto-reconnect may silently re-establish STA (e.g. after router reboot).
    if (WiFi.status() == WL_CONNECTED)
    {
      s_staConnected = true;
      stopAP(); // stopAP() calls MDNS.end()
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter");
      Serial.printf("[WIFI] Auto-reconnected | IP: %s\n",
                    WiFi.localIP().toString().c_str());
      Serial.println("[WIFI] mDNS: http://telemeter.local");
    }
    break;
  }
}

bool wifiIsApActive()     { return s_apActive; }
bool wifiIsStaConnected() { return s_staConnected; }

void wifiHandleCatchAll(AsyncWebServerRequest *req) { handleCatchAll(req); }
