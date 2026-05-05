/**
 * gateway_wifi_config.cpp
 *
 * Non-blocking WiFi state machine for the Power Telemetry Gateway.
 *
 * State machine:
 *
 *   AP_ACTIVE ──(creds exist at boot / POST /api/connect)──► AP_STA_CONNECTING
 *       ▲                                                           │
 *       │ STA drops / timeout / forget / disconnect                 │ WL_CONNECTED
 *       │                                                           ▼
 *       └──────────────────────────────────────── STA_CONNECTED (AP off)
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

// Static IP NVS — keys sip/sgw/ssn/sdns; all empty = DHCP
static void nvsLoadStaticIp(String &ip, String &gw, String &sn, String &dns1)
{
  Preferences p;
  p.begin("wifi-cfg", true);
  ip   = p.getString("sip",  "");
  gw   = p.getString("sgw",  "");
  sn   = p.getString("ssn",  "");
  dns1 = p.getString("sdns", "");
  p.end();
}

static void nvsSaveStaticIp(const String &ip, const String &gw,
                             const String &sn, const String &dns1)
{
  Preferences p;
  p.begin("wifi-cfg", false);
  p.putString("sip",  ip);
  p.putString("sgw",  gw);
  p.putString("ssn",  sn);
  p.putString("sdns", dns1);
  p.end();
}

static void nvsClearStaticIp()
{
  Preferences p;
  p.begin("wifi-cfg", false);
  p.remove("sip");
  p.remove("sgw");
  p.remove("ssn");
  p.remove("sdns");
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

  // Apply static IP if configured; fall through to DHCP otherwise.
  String sip, sgw, ssn, sdns;
  nvsLoadStaticIp(sip, sgw, ssn, sdns);
  if (!sip.isEmpty()) {
    IPAddress ip, gw, sn, dns1;
    if (ip.fromString(sip) && gw.fromString(sgw) && sn.fromString(ssn)) {
      if (sdns.isEmpty() || !dns1.fromString(sdns)) dns1 = gw; // fallback: gateway as DNS
      WiFi.config(ip, gw, sn, dns1);
      Serial.printf("[WIFI] Static IP: %s / %s via %s\n",
                    sip.c_str(), ssn.c_str(), sgw.c_str());
    }
  }

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
// 300 ms dwell per channel (vs default 120 ms) improves reliability in AP+STA
// mode where the radio shares time between serving AP clients and scanning.
// Auto-retries up to 2 times when scan completes with 0 networks, which is a
// common first-attempt failure in AP+STA mode.
static uint8_t s_scanRetries = 0;
static void startScan() {
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false,
                    /*passive=*/false, /*ms_per_chan=*/300);
}

static void handleScan(AsyncWebServerRequest *req)
{
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED)
  {
    s_scanRetries = 0;
    startScan();
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  if (n == WIFI_SCAN_RUNNING)
  {
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  // Retry up to 2 times when scan returns 0 networks — common in AP+STA mode
  // on the first attempt while the radio is busy serving AP clients.
  if (n == 0 && s_scanRetries < 2)
  {
    s_scanRetries++;
    WiFi.scanDelete();
    startScan();
    req->send(202, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }

  s_scanRetries = 0;
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

// GET /api/staticip — returns current static IP config
static void handleGetStaticIp(AsyncWebServerRequest *req)
{
  String ip, gw, sn, dns1;
  nvsLoadStaticIp(ip, gw, sn, dns1);
  JsonDocument doc;
  doc["enabled"] = !ip.isEmpty();
  doc["ip"]      = ip;
  doc["gateway"] = gw;
  doc["subnet"]  = sn;
  doc["dns"]     = dns1;
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// POST /api/staticip — body: ip=&gateway=&subnet=&dns=
static void handleSetStaticIp(AsyncWebServerRequest *req)
{
  String ip = "", gw = "", sn = "255.255.255.0", dns1 = "";
  if (req->hasParam("ip",      true)) ip   = req->getParam("ip",      true)->value();
  if (req->hasParam("gateway", true)) gw   = req->getParam("gateway", true)->value();
  if (req->hasParam("subnet",  true)) sn   = req->getParam("subnet",  true)->value();
  if (req->hasParam("dns",     true)) dns1 = req->getParam("dns",     true)->value();
  ip.trim(); gw.trim(); sn.trim(); dns1.trim();

  IPAddress vip, vgw, vsn;
  if (ip.isEmpty() || !vip.fromString(ip) || !vgw.fromString(gw) || !vsn.fromString(sn))
  {
    req->send(400, "application/json",
              "{\"ok\":false,\"message\":\"Invalid IP, gateway, or subnet\"}");
    return;
  }

  nvsSaveStaticIp(ip, gw, sn, dns1);
  Serial.printf("[WIFI] Static IP saved: %s / %s via %s\n",
                ip.c_str(), sn.c_str(), gw.c_str());
  req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/staticip/clear — reset to DHCP
static void handleClearStaticIp(AsyncWebServerRequest *req)
{
  nvsClearStaticIp();
  Serial.println("[WIFI] Static IP cleared — will use DHCP");
  req->send(200, "application/json", "{\"ok\":true}");
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
  server.on("/api/info",            HTTP_GET,  handleInfo);
  server.on("/api/scan",            HTTP_GET,  handleScan);
  server.on("/api/connect",         HTTP_POST, handleConnect);
  server.on("/api/wifistatus",      HTTP_GET,  handleWifiStatus);
  server.on("/api/disconnect",      HTTP_GET,  handleDisconnect);
  server.on("/api/forget",          HTTP_GET,  handleForget);
  server.on("/api/staticip",        HTTP_GET,  handleGetStaticIp);
  server.on("/api/staticip",        HTTP_POST, handleSetStaticIp);
  server.on("/api/staticip/clear",  HTTP_GET,  handleClearStaticIp);
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
