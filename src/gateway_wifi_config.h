#pragma once
/**
 * gateway_wifi_config.h
 *
 * Non-blocking WiFi state machine for the Power Telemetry Gateway.
 *
 * ── Design principle ─────────────────────────────────────────────────────────
 *
 *   The AP is the permanent last-resort access route. It starts unconditionally
 *   at boot and is the only way to reach the gateway when no upstream WiFi
 *   network is available or configured. STA is an optional extension that, when
 *   connected, extends the range to any client on the same home network.
 *
 *   AP is disabled only while STA is actively connected. If STA drops for any
 *   reason, AP comes back on immediately — no manual intervention required.
 *
 * ── State machine ────────────────────────────────────────────────────────────
 *
 *   Boot → startAP() immediately → AP_ACTIVE
 *
 *   AP_ACTIVE ──(saved creds exist)──► AP_STA_CONNECTING
 *       ▲         or /api/connect             │
 *       │                           STA connects → stopAP()
 *       │                                     ▼
 *       └──── STA drops / timeout ── STA_CONNECTED (AP off)
 *
 *   Auto-reconnect (SDK): AP_ACTIVE → STA_CONNECTED when SDK reconnects
 *
 * ── Dashboard integration ────────────────────────────────────────────────────
 *
 *   The dashboard's "cfgBtn" (id="cfgBtn") navigates to /wifi_config.html.
 *   This page is served from LittleFS by the same AsyncWebServer as the
 *   dashboard — it is reachable on both the AP IP (192.168.4.1) and the STA IP.
 *   The dashboard does not need to know or care which interface the client is on.
 *
 * ── WiFi config API (on main server, port 80) ─────────────────────────────────
 *
 *   GET  /wifi_config.html   → serves /wifi_config.html from LittleFS
 *   GET  /api/info           → {"version","apSsid","apIp","apActive","staConnected","staSSID","staIP"}
 *   GET  /api/scan           → {"networks":[{"ssid","rssi","secure"},...]}
 *   POST /api/connect        → body: ssid=...&password=...   → {"status":"connecting"}
 *   GET  /api/wifistatus     → {"apActive","connecting","connected","ip","ssid","rssi"}
 *                              polled every 800 ms by wifi_config.html after POST /api/connect
 *   GET  /api/disconnect     → {"ok":true}  abort current attempt
 *   GET  /api/forget         → {"ok":true}  clear NVS credentials, restore AP
 *
 *   /api/wifistatus ≠ /api/status  (dashboard uses /api/status for gateway system info)
 *
 * ── Captive-portal convenience ───────────────────────────────────────────────
 *
 *   When AP is active, DNS redirects all queries → 192.168.4.1 so phones
 *   auto-open a browser. The catch-all handler (wifiHandleCatchAll) redirects
 *   to /wifi_config.html and must be wired into gateway_web.cpp's onNotFound.
 *
 *   OS probes automatically handled by the redirect:
 *     Android  : /generate_204, /gen_204
 *     iOS/macOS: /hotspot-detect.html, /library/test/success.html
 *     Windows  : /connecttest.txt, /ncsi.txt, /redirect
 *     Firefox  : /canonical.html
 *
 * ── NVS layout ───────────────────────────────────────────────────────────────
 *   namespace "wifi-cfg" · key "ssid" (≤32 B) · key "pass" (≤64 B, empty = open)
 *   Survives LittleFS format and firmware OTA.
 */

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// ─── Parameters ──────────────────────────────────────────────────────────────
#define CONFIG_PIN              0           // GPIO0 / BOOT — hold LOW at power-on
#define WIFI_CONNECT_TIMEOUT_MS 12000
#define CONFIG_AP_SSID          "PowerTelemetry-Setup"
#define FW_VERSION_STR          "v1.0.0"  // human-readable string for /api/info
                                           // protocol version integer: FW_VERSION in lora_tdma_protocol.h

// ─── Public API ──────────────────────────────────────────────────────────────

/** Called once at the top of setup(). Always returns promptly.
 *  Starts AP unconditionally, then begins STA attempt if creds exist. */
void wifiConfigBegin();

/** Register all WiFi config routes on the main server.
 *  Call from webServerSetup() before server.begin(). */
void wifiRegisterRoutes(AsyncWebServer &server);

/** Drive the state machine. Call every loop() iteration. */
void wifiConfigLoop();

/** For gateway_web.cpp's onNotFound — redirects to /wifi_config.html when AP active. */
void wifiHandleCatchAll(AsyncWebServerRequest *req);

/** Status accessors — used by /api/status to include apActive field. */
bool wifiIsApActive();
bool wifiIsStaConnected();
