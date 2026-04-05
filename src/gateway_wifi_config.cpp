/**
 * gateway_wifi_config.cpp
 *
 * Non-blocking WiFi state machine for the Power Telemetry Gateway.
 *
 * AP is always the starting state — it is the permanent last-resort access
 * point. STA is an optional upgrade. AP turns off only while STA is connected,
 * and comes back on the moment STA drops.
 *
 * Boot flow:
 *   1. startAP() — immediately, unconditionally
 *   2. If NVS credentials exist -> beginSTA() alongside the running AP
 *   3. wifiConfigLoop() drives the rest
 *
 * State machine:
 *
 *   AP_ACTIVE ──(creds exist at boot)──► AP_STA_CONNECTING
 *       ▲                                       │
 *       │ STA drops / timeout / forget          │ STA connects
 *       │                                       ▼
 *       └──────────────────────────── STA_CONNECTED (AP off)
 *
 * Notes:
 *   - AP is open (no password) so OS captive-portal detection fires and
 *     auto-opens a browser when a device first joins the AP.
 *   - DNS catch-all redirects all queries to 192.168.4.1 while AP is active.
 *   - The config page (/wifi_config.html) is served from LittleFS on the
 *     same AsyncWebServer as the dashboard. It is reachable on either the AP IP
 *     or the STA IP — the cfgBtn on the dashboard navigates to /wifi_config.html
 *     regardless of which interface the client is currently using.
 *   - /api/wifistatus is distinct from /api/status (dashboard gateway-info).
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
  WIFI_STATE_AP_ACTIVE,         // AP on, no STA (waiting for provisioning or auto-reconnect)
  WIFI_STATE_AP_STA_CONNECTING, // AP on + STA connecting in background
  WIFI_STATE_STA_CONNECTED,     // STA up, AP off
};

static WifiState s_state = WIFI_STATE_AP_ACTIVE;
static uint32_t s_connectStart = 0;
static bool s_apActive = false;
static bool s_staConnected = false;
static DNSServer s_dns;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const uint8_t DNS_PORT = 53;

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
  // Register a one-shot event handler so MDNS.begin() fires only after
  // ARDUINO_EVENT_WIFI_AP_START, i.e. once the AP interface is fully up
  // and has assigned 192.168.4.1. Calling it synchronously here would race
  // with the async AP bring-up and may bind to the wrong interface.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    MDNS.begin("telemeter");
    Serial.println("[WIFI] mDNS: http://telemeter.local (AP mode)");
  }, ARDUINO_EVENT_WIFI_AP_START);

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
  MDNS.end(); // stop AP-mode mDNS before STA-mode mDNS starts
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.enableAP(false); // disable AP without touching STA
  s_apActive = false;
  Serial.println("[WIFI] AP disabled — STA is up");
}

// -- STA helper -----------------------------------------------------
static void beginSTA(const String &ssid, const String &pass)
{
  WiFi.enableSTA(true);
  WiFi.setHostname("telemeter");
  WiFi.setAutoReconnect(true); // SDK auto-reconnect on transient drops

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
  doc["version"] = FW_VERSION_STR;
  doc["apSsid"] = CONFIG_AP_SSID;
  doc["apIp"] = AP_IP.toString();
  doc["apActive"] = s_apActive;
  doc["staConnected"] = s_staConnected;
  if (s_staConnected)
  {
    doc["staSSID"] = WiFi.SSID();
    doc["staIP"] = WiFi.localIP().toString();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleScan(AsyncWebServerRequest *req)
{
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++)
  {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
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
  doc["apActive"] = s_apActive;
  doc["connecting"] = (s_state == WIFI_STATE_AP_STA_CONNECTING);
  doc["connected"] = s_staConnected;
  if (s_staConnected)
  {
    doc["ip"] = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
  }
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

static void handleDisconnect(AsyncWebServerRequest *req)
{
  WiFi.disconnect(false);
  s_staConnected = false;
  if (s_state == WIFI_STATE_AP_STA_CONNECTING)
    s_state = WIFI_STATE_AP_ACTIVE; // back to AP-only, AP already running
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[WIFI] STA cancelled by user");
}

static void handleForget(AsyncWebServerRequest *req)
{
  nvsClear();
  WiFi.disconnect(false);
  WiFi.setAutoReconnect(false);
  s_staConnected = false;
  // Ensure AP is back on if STA was connected
  startAP();
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
  pinMode(CONFIG_PIN, INPUT_PULLUP);
  delay(50);
  bool forceAP = (digitalRead(CONFIG_PIN) == LOW);
  if (forceAP)
    Serial.printf("[WIFI] GPIO%d LOW — skipping saved credentials\n", CONFIG_PIN);

  // -- Step 1: AP always starts immediately ----------------------------------
  // AP is the unconditional last-resort route. It is on from the first
  // millisecond of operation. stopAP() is only called once STA connects.
  WiFi.mode(WIFI_AP_STA); // both interfaces active; STA can be started later
  startAP();
  s_state = WIFI_STATE_AP_ACTIVE;

  // -- Step 2: Attempt STA if credentials are saved ---------------------------
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
  // Returns immediately in all cases. loop() drives the rest.
}

void wifiRegisterRoutes(AsyncWebServer &server)
{
  // WiFi config API - all distinct from dashboard /api/* routes
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/connect", HTTP_POST, handleConnect);
  server.on("/api/wifistatus", HTTP_GET, handleWifiStatus);
  server.on("/api/disconnect", HTTP_GET, handleDisconnect);
  server.on("/api/forget", HTTP_GET, handleForget);

  // Captive-portal catch-all (must be last — registered via onNotFound in gateway_web.cpp)
  // gateway_web.cpp calls wifiRegisterRoutes() then sets its own onNotFound;
  // handleCatchAll is stored here and invoked from gateway_web's onNotFound handler.
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
      stopAP(); // STA is up — AP no longer needed
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter"); // telemeter.local
      Serial.printf("[WIFI] STA connected | IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      Serial.println("[WIFI] mDNS: http://telemeter.local");
    }
    else if (millis() - s_connectStart > WIFI_CONNECT_TIMEOUT_MS)
    {
      Serial.println("[WIFI] STA timed out - staying in AP mode");
      WiFi.disconnect(false);
      s_state = WIFI_STATE_AP_ACTIVE; // AP is still running, nothing else to do
    }
    break;

  case WIFI_STATE_STA_CONNECTED:
    if (WiFi.status() != WL_CONNECTED)
    {
      // Runtime drop — bring AP back as immediate fallback
      s_staConnected = false;
      startAP(); // stopAP not needed — STA dropped, AP was already off
      s_state = WIFI_STATE_AP_ACTIVE;
      Serial.println("[WIFI] STA dropped - AP restored as fallback");
      // WiFi.setAutoReconnect(true) means SDK will attempt to reconnect;
      // loop will catch it in AP_ACTIVE and promote to STA_CONNECTED.
    }
    break;

  case WIFI_STATE_AP_ACTIVE:
    // SDK auto-reconnect may re-establish STA (e.g., after router reboot)
    if (WiFi.status() == WL_CONNECTED)
    {
      s_staConnected = true;
      stopAP();
      s_state = WIFI_STATE_STA_CONNECTED;
      MDNS.begin("telemeter"); // telemeter.local
      Serial.printf("[WIFI] Auto-reconnected | IP: %s\n",
                    WiFi.localIP().toString().c_str());
      Serial.println("[WIFI] mDNS: http://telemeter.local");
    }
    break;
  }
}

bool wifiIsApActive() { return s_apActive; }
bool wifiIsStaConnected() { return s_staConnected; }

// Exposed so gateway_web.cpp's onNotFound can invoke the captive redirect
void wifiHandleCatchAll(AsyncWebServerRequest *req) { handleCatchAll(req); }
