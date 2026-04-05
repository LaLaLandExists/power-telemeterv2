/**
 * gateway_web.cpp
 *
 * ESPAsyncWebServer + AsyncWebSocket implementation.
 * Serves the dashboard SPA from LittleFS and exposes the full API
 * defined in dashboard-api-reference.md (v7).
 *
 * REST endpoints:
 *   GET  /api/nodes
 *   GET  /api/node/{id}/live
 *   GET  /api/node/{id}/history
 *   POST /api/node/{id}/relay
 *   POST /api/node/{id}/name
 *   POST /api/time
 *   GET  /api/status
 *
 * WebSocket ws://<ip>/ws
 *   Commands: relay_manual, relay_schedule, relay_clear, set_threshold,
 *             nudge, rename, set_time, clear_energy, clear_all_energy, get_nodes
 *   Push:     telemetry, nodes, name_changed, time_set, relay_ack, schedule_ack,
 *             clear_ack, threshold_ack, nudge_ack, energy_cleared, all_energy_cleared
 */

#include "gateway_web.h"
#include "gateway_state.h"
#include "gateway_tdma_task.h"
#include "gateway_wifi_config.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// -- Server and WebSocket instances -------------------------------------------
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// =================================================
// JSON helpers — build node objects from NodeState
// =================================================

/** Populate a JsonObject with the "summary" fields (used by /api/nodes and nodes push). */
static void nodeToSummaryJson(JsonObject obj, const NodeState &ns)
{
  char timeBuf[9];
  gwTimeString(timeBuf);

  bool online = ns.hasData && ((millis() - ns.lastSeen) < NODE_TIMEOUT_MS);

  obj["id"] = ns.slotId;
  obj["label"] = ns.label;
  obj["online"] = online;
  obj["rssi"] = ns.rssi;
  // The x / 10.0f * 10.0f may look useless, but this converts raw modbus register to floating point values
  obj["voltage"] = roundf(ns.latest.voltage / 10.0f * 10.0f) / 10.0f;
  obj["current"] = roundf(ns.latest.current / 1000.0f * 1000.0f) / 1000.0f;
  obj["power"] = roundf(ns.latest.power / 10.0f * 10.0f) / 10.0f;
  obj["energy"] = ns.accumEnergy;
  obj["frequency"] = roundf(ns.latest.frequency / 10.0f * 10.0f) / 10.0f;
  obj["powerFactor"] = roundf(ns.latest.powerFactor / 100.0f * 100.0f) / 100.0f;
  obj["relayState"] = ns.relayState;
  obj["relayMode"] = ns.relayMode;
  obj["schedState"] = ns.schedState;
  obj["alarmState"] = ns.alarmState;
  obj["alarmThreshold"] = ns.latest.alarmThreshold;
  obj["age"] = (uint32_t)((millis() - ns.lastSeen) / 1000UL);
  obj["pending"] = ns.pending;

  bool hasSched = (ns.relayMode == 1 && ns.schedState > 0);
  obj["hasSched"] = hasSched;
}

/** Populate a JsonObject with full detail fields (used by /api/node/{id}/live and telemetry push). */
static void nodeToDetailJson(JsonObject obj, const NodeState &ns)
{
  nodeToSummaryJson(obj, ns);

  // Schedule fields only when active
  bool hasSched = (ns.relayMode == 1 && ns.schedState > 0);
  if (hasSched)
  {
    char schedStart[6], schedEnd[6];
    snprintf(schedStart, sizeof(schedStart), "%02d:%02d",
             ns.latest.schedSH, ns.latest.schedSM);
    snprintf(schedEnd, sizeof(schedEnd), "%02d:%02d",
             ns.latest.schedEH, ns.latest.schedEM);
    obj["schedStart"] = schedStart;
    obj["schedEnd"] = schedEnd;
  }

  // Cost calculation
  float cost = (float)ns.accumEnergy / 1000.0f * g_costPerKwh;
  obj["energyCost"] = roundf(cost * 100.0f) / 100.0f;
  obj["costPerKwh"] = g_costPerKwh;

  // Gateway clock status
  char timeBuf[9];
  gwTimeString(timeBuf);
  obj["timeSet"] = g_timeSet;
  obj["time"] = timeBuf;
}

/** Extract node ID from path like "/api/node/3/live" → 3 */
// TODO implement this using regex if possible
static int parseNodeIdFromPath(const String &path)
{
  // path format: /api/node/<id>/<action>
  int first = path.indexOf('/', 10); // skip "/api/node"
  if (first < 0)
    return -1;
  int second = path.indexOf('/', first + 1);
  if (second < 0)
    return -1;
  return path.substring(first + 1, second).toInt();
}

// -----------------------------------------------------------------------------
// LittleFS — node name persistence
// -----------------------------------------------------------------------------

static void saveNodeNames()
{
  JsonDocument doc;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    for (uint8_t i = 0; i < MAX_NODES; i++)
    {
      if (g_nodes[i].active)
      {
        doc[String(g_nodes[i].slotId)] = g_nodes[i].label;
      }
    }
    xSemaphoreGive(g_nodesMutex);
  }
  File f = LittleFS.open("/node_names.json", "w");
  if (!f)
  {
    // First attempt failed. LFS_ERR_NOPERM can occur if a prior partial
    // write left a corrupt inode. Remove and retry once.
    Serial.println("[WEB] WARN: node_names.json open failed, removing and retrying");
    LittleFS.remove("/node_names.json");
    f = LittleFS.open("/node_names.json", "w");
  }
  if (f)
  {
    serializeJson(doc, f);
    f.close();
  }
  else
  {
    Serial.println("[WEB] ERROR: could not save node_names.json after retry");
  }
}

static void loadNodeNames()
{
  if (!LittleFS.exists("/node_names.json"))
  {
    // File does not exist yet (fresh filesystem) — create it now with an
    // empty object so subsequent saveNodeNames() calls never need to create
    // a new file. LittleFS LFS_ERR_NOPERM on create can occur when a prior
    // partial write / power-cycle left a corrupt inode at the path.
    File f = LittleFS.open("/node_names.json", "w");
    if (f)
    {
      f.print("{}");
      f.close();
    }
    else
    {
      Serial.println("[WEB] WARN: could not create node_names.json");
    }
    return; // nothing to load
  }

  File f = LittleFS.open("/node_names.json", "r");
  if (!f)
  {
    Serial.println("[WEB] WARN: could not open node_names.json for read");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    Serial.printf("[WEB] WARN: node_names.json parse error: %s\n", err.c_str());
    return;
  }

  // Pre-populate labels so they're ready when nodes join
  // (nodes may have been registered in a previous session)
  for (JsonPair kv : doc.as<JsonObject>())
  {
    int id = String(kv.key().c_str()).toInt();
    if (id < 1 || id > MAX_NODES)
      continue;
    int idx = id - 1;
    // Only set label - active flag is set when the node actually joins
    // Sets g_nodes[idx].label <- Json value
    strlcpy(g_nodes[idx].label, kv.value().as<const char *>(),
            sizeof(g_nodes[idx].label));
  }
  Serial.println("[WEB] Loaded node_names.json");
}

// -----------------------------------------------------------------------------
// WebSocket broadcast helpers
// -----------------------------------------------------------------------------

void webBroadcastTelemetry(uint8_t slotIdx)
{
  if (ws.count() == 0)
    return;

  JsonDocument doc;
  doc["type"] = "telemetry";

  JsonObject nodeObj = doc["node"].to<JsonObject>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    if (g_nodes[slotIdx].active)
    {
      nodeToDetailJson(nodeObj, g_nodes[slotIdx]);
    }
    xSemaphoreGive(g_nodesMutex);
  }

  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

static void wsSendToClient(AsyncWebSocketClient *client, const JsonDocument &doc)
{
  String json;
  serializeJson(doc, json);
  client->text(json);
}

// ---------------------------------------------------------------------------------
// Shared nodes-list document builder
// Populates doc with {type:"nodes", nodes:[...], count, costPerKwh, timeSet, time}.
// Used by get_nodes command, WS_EVT_CONNECT, and webBroadcastAllNodes().
static void buildNodesDoc(JsonDocument &doc)
{
  doc["type"] = "nodes";
  JsonArray arr = doc["nodes"].to<JsonArray>();
  int count = 0;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    for (uint8_t i = 0; i < MAX_NODES; i++)
    {
      if (g_nodes[i].active)
      {
        JsonObject obj = arr.add<JsonObject>();
        nodeToSummaryJson(obj, g_nodes[i]);
        count++;
      }
    }
    xSemaphoreGive(g_nodesMutex);
  }
  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["count"] = count;
  doc["costPerKwh"] = g_costPerKwh;
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;
}

// WebSocket command handler
// ----------------------------------------------------------------------------
static void handleWsMessage(AsyncWebSocketClient *client, const String &payload)
{
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok)
    return;

  const char *cmd = doc["cmd"] | "";
  uint8_t nodeId = (uint8_t)(doc["node"] | 0);

  // -- relay_manual --------------------------------------------------------
  if (strcmp(cmd, "relay_manual") == 0)
  {
    uint8_t state = (uint8_t)(doc["state"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[3] = {PKT_RELAY_MANUAL, nodeId, state};
      ok = tdmaQueueCommand(idx, pkt, 3);
    }
    JsonDocument ack;
    ack["type"] = "relay_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- relay_schedule -------------------------------------------------------
  if (strcmp(cmd, "relay_schedule") == 0)
  {
    uint8_t startH = (uint8_t)(doc["startH"] | 0);
    uint8_t startM = (uint8_t)(doc["startM"] | 0);
    uint8_t endH = (uint8_t)(doc["endH"] | 0);
    uint8_t endM = (uint8_t)(doc["endM"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[7] = {PKT_RELAY_SCHEDULE, nodeId, 1,
                        startH, startM, endH, endM};
      ok = tdmaQueueCommand(idx, pkt, 7);
    }
    JsonDocument ack;
    ack["type"] = "schedule_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- relay_clear -----------------------------------------------------------
  if (strcmp(cmd, "relay_clear") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[2] = {PKT_RELAY_CLEAR, nodeId};
      ok = tdmaQueueCommand(idx, pkt, 2);
    }
    JsonDocument ack;
    ack["type"] = "clear_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- set_threshold ----------------------------------------------------------
  if (strcmp(cmd, "set_threshold") == 0)
  {
    uint16_t watts = (uint16_t)(doc["watts"] | 0);
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF && watts > 0 && watts <= 23000)
    {
      uint8_t pkt[4] = {PKT_THRESHOLD, nodeId,
                        (uint8_t)(watts & 0xFF), (uint8_t)(watts >> 8)};
      ok = tdmaQueueCommand(idx, pkt, 4);
    }
    JsonDocument ack;
    ack["type"] = "threshold_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- nudge ------------------------------------------------------------------
  if (strcmp(cmd, "nudge") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    bool ok = false;
    if (idx != 0xFF)
    {
      uint8_t pkt[2] = {PKT_NUDGE, nodeId};
      ok = tdmaQueueCommand(idx, pkt, 2);
    }
    JsonDocument ack;
    ack["type"] = "nudge_ack";
    ack["node"] = nodeId;
    ack["success"] = ok;
    wsSendToClient(client, ack);
    return;
  }

  // -- rename -----------------------------------------------------------------
  if (strcmp(cmd, "rename") == 0)
  {
    const char *name = doc["name"] | "";
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    if (idx != 0xFF && strlen(name) >= 1 && strlen(name) <= 29)
    {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
      {
        strlcpy(g_nodes[idx].label, name, sizeof(g_nodes[idx].label));
        xSemaphoreGive(g_nodesMutex);
      }
      saveNodeNames();
      // Broadcast name_changed to ALL clients
      JsonDocument bcast;
      bcast["type"] = "name_changed";
      bcast["node"] = nodeId;
      bcast["name"] = name;
      String json;
      serializeJson(bcast, json);
      ws.textAll(json);
    }
    return;
  }

  // -- set_time -----------------------------------------------------------------
  if (strcmp(cmd, "set_time") == 0)
  {
    uint8_t h = (uint8_t)(doc["hour"] | 0);
    uint8_t m = (uint8_t)(doc["minute"] | 0);
    uint8_t s = (uint8_t)(doc["second"] | 0);
    setGatewayTime(h, m, s);
    char timeBuf[9];
    gwTimeString(timeBuf);
    JsonDocument bcast;
    bcast["type"] = "time_set";
    bcast["timeSet"] = true;
    bcast["time"] = timeBuf;
    String json;
    serializeJson(bcast, json);
    ws.textAll(json);
    Serial.printf("[WEB] Time set to %s\n", timeBuf);
    return;
  }

  // -- clear_energy -------------------------------------------------------------
  if (strcmp(cmd, "clear_energy") == 0)
  {
    uint8_t idx = tdmaFindSlotByNodeId(nodeId);
    if (idx != 0xFF)
    {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
      {
        g_nodes[idx].accumEnergy = 0;
        xSemaphoreGive(g_nodesMutex);
      }
    }
    JsonDocument ack;
    ack["type"] = "energy_cleared";
    ack["node"] = nodeId;
    ack["success"] = true;
    wsSendToClient(client, ack);
    return;
  }

  // -- clear_all_energy --------------------------------------------------------
  if (strcmp(cmd, "clear_all_energy") == 0)
  {
    if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      for (uint8_t i = 0; i < MAX_NODES; i++)
      {
        g_nodes[i].accumEnergy = 0;
      }
      xSemaphoreGive(g_nodesMutex);
    }
    JsonDocument ack;
    ack["type"] = "all_energy_cleared";
    ack["node"] = 0;
    ack["success"] = true;
    wsSendToClient(client, ack);
    return;
  }

  // -- get_nodes ---------------------------------------------------------------
  if (strcmp(cmd, "get_nodes") == 0)
  {
    JsonDocument resp;
    buildNodesDoc(resp);
    wsSendToClient(client, resp);
    return;
  }
}

// -----------------------------------------------------------------------------
// WebSocket event handler
// -----------------------------------------------------------------------------
static void onWsEvent(AsyncWebSocket * /*server*/, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.printf("[WS] Client #%u connected from %s\n",
                  client->id(), client->remoteIP().toString().c_str());
    // Push full node list immediately on connect
    JsonDocument doc;
    buildNodesDoc(doc);
    wsSendToClient(client, doc);
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    Serial.printf("[WS] Client #%u disconnected\n", client->id());
  }
  else if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
      String msg((char *)data, len);
      handleWsMessage(client, msg);
    }
  }
}

// -----------------------------------------------------------------------------
// REST route handlers
// -----------------------------------------------------------------------------

/** GET /api/nodes */
static void handleGetNodes(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  JsonArray arr = doc["nodes"].to<JsonArray>();
  int count = 0;

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    for (uint8_t i = 0; i < MAX_NODES; i++)
    {
      if (g_nodes[i].active)
      {
        JsonObject obj = arr.add<JsonObject>();
        nodeToSummaryJson(obj, g_nodes[i]);
        count++;
      }
    }
    xSemaphoreGive(g_nodesMutex);
  }

  char timeBuf[9];
  gwTimeString(timeBuf);
  doc["count"] = count;
  doc["costPerKwh"] = g_costPerKwh;
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** GET /api/node/{id}/live */
static void handleGetNodeLive(AsyncWebServerRequest *req)
{
  int nodeId = parseNodeIdFromPath(req->url());
  if (nodeId < 1 || nodeId > MAX_NODES)
  {
    req->send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    nodeToDetailJson(obj, g_nodes[idx]);
    xSemaphoreGive(g_nodesMutex);
  }

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** GET /api/node/{id}/history */
static void handleGetNodeHistory(AsyncWebServerRequest *req)
{
  int nodeId = parseNodeIdFromPath(req->url());
  if (nodeId < 1 || nodeId > MAX_NODES)
  {
    req->send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "[]");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    NodeState *ns = &g_nodes[idx];
    // Walk the circular buffer in chronological order
    int start = (ns->histCount < HISTORY_MAX_POINTS)
                    ? 0
                    : ns->histHead;
    for (int i = 0; i < ns->histCount; i++)
    {
      int cidx = (start + i) % HISTORY_MAX_POINTS;
      JsonObject pt = arr.add<JsonObject>();
      pt["t"] = ns->history[cidx].t;
      pt["v"] = ns->history[cidx].v;
      pt["i"] = ns->history[cidx].i;
      pt["p"] = ns->history[cidx].p;
    }
    xSemaphoreGive(g_nodesMutex);
  }

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

/** POST /api/node/{id}/relay  body: state=0|1 */
static void handlePostNodeRelay(AsyncWebServerRequest *req)
{
  int nodeId = parseNodeIdFromPath(req->url());
  if (!req->hasParam("state", true))
  {
    req->send(400, "application/json", "{\"error\":\"missing state\"}");
    return;
  }
  uint8_t state = (uint8_t)req->getParam("state", true)->value().toInt();
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  bool ok = false;
  if (idx != 0xFF)
  {
    uint8_t pkt[3] = {PKT_RELAY_MANUAL, (uint8_t)nodeId, state};
    ok = tdmaQueueCommand(idx, pkt, 3);
  }
  req->send(200, "application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
}

/** POST /api/node/{id}/name  body: name=<string> */
static void handlePostNodeName(AsyncWebServerRequest *req)
{
  int nodeId = parseNodeIdFromPath(req->url());
  if (!req->hasParam("name", true))
  {
    req->send(400, "application/json", "{\"error\":\"missing name\"}");
    return;
  }
  String name = req->getParam("name", true)->value();
  if (name.length() < 1 || name.length() > 29)
  {
    req->send(400, "application/json", "{\"error\":\"name length 1-29\"}");
    return;
  }
  uint8_t idx = tdmaFindSlotByNodeId((uint8_t)nodeId);
  if (idx == 0xFF)
  {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    strlcpy(g_nodes[idx].label, name.c_str(), sizeof(g_nodes[idx].label));
    xSemaphoreGive(g_nodesMutex);
  }
  saveNodeNames();

  JsonDocument bcast;
  bcast["type"] = "name_changed";
  bcast["node"] = nodeId;
  bcast["name"] = name;
  String json;
  serializeJson(bcast, json);
  ws.textAll(json);

  req->send(200, "application/json", "{\"ok\":true}");
}

/** POST /api/time  body: hour=H&minute=M&second=S */
static void handlePostTime(AsyncWebServerRequest *req)
{
  uint8_t h = req->hasParam("hour", true) ? (uint8_t)req->getParam("hour", true)->value().toInt() : 0;
  uint8_t m = req->hasParam("minute", true) ? (uint8_t)req->getParam("minute", true)->value().toInt() : 0;
  uint8_t s = req->hasParam("second", true) ? (uint8_t)req->getParam("second", true)->value().toInt() : 0;
  setGatewayTime(h, m, s);

  char timeBuf[9];
  gwTimeString(timeBuf);

  JsonDocument bcast;
  bcast["type"] = "time_set";
  bcast["timeSet"] = true;
  bcast["time"] = timeBuf;
  String json;
  serializeJson(bcast, json);
  ws.textAll(json);

  Serial.printf("[WEB] POST /api/time → %s\n", timeBuf);
  req->send(200, "application/json", "{\"ok\":true}");
}

/** GET /api/status */
static void handleGetStatus(AsyncWebServerRequest *req)
{
  JsonDocument doc;
  char timeBuf[9];
  gwTimeString(timeBuf);

  int nodeCount = 0;
  for (uint8_t i = 0; i < MAX_NODES; i++)
    if (g_nodes[i].active)
      nodeCount++;

  doc["uptime"] = millis() / 1000UL;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["wifiRSSI"] = WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["loraFreq"] = LORA_CHANNELS[0];
  doc["nodeCount"] = nodeCount;
  doc["maxNodes"] = MAX_NODES;
  doc["wsClients"] = ws.count();
  doc["timeSet"] = g_timeSet;
  doc["time"] = timeBuf;
  doc["apActive"] = wifiIsApActive(); // true when AP is on (STA unavailable)

  String json;
  serializeJson(doc, json);
  req->send(200, "application/json", json);
}

// -----------------------------------------------------------------------------
// 5-second periodic nodes broadcast (called from loop())
// -----------------------------------------------------------------------------
static uint32_t lastBcastMs = 0;

// Called from main.cpp loop() on Core 0
void webBroadcastAllNodes()
{
  uint32_t now = millis();
  if (now - lastBcastMs >= 5000)
  {
    lastBcastMs = now;
    if (ws.count() > 0)
    {
      JsonDocument doc;
      buildNodesDoc(doc);
      String json;
      serializeJson(doc, json);
      ws.textAll(json);
    }
  }
  ws.cleanupClients();
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void webServerSetup()
{
  // Load persisted node names
  loadNodeNames();

  // WebSocket handler
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // REST routes
  server.on("/api/nodes", HTTP_GET, handleGetNodes);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/time", HTTP_POST, handlePostTime);

  // Per-node routes — match on prefix, dispatch on action suffix
  server.on("/api/node", HTTP_ANY, [](AsyncWebServerRequest *req) {
    String path = req->url();
    // Determine action from suffix
    bool isPost = (req->method() == HTTP_POST);

    if      (path.endsWith("/live"))    handleGetNodeLive(req);
    else if (path.endsWith("/history")) handleGetNodeHistory(req);
    else if (path.endsWith("/relay") && isPost)  handlePostNodeRelay(req);
    else if (path.endsWith("/name")  && isPost)  handlePostNodeName(req);
    else req->send(404, "application/json", "{\"error\":\"not found\"}"); 
  });

  // -- WiFi config routes (scan, connect, status, etc.) ---------------------
  // Registers /api/info, /api/scan, /api/connect, /api/wifistatus, /api/disconnect, /api/forget.
  wifiRegisterRoutes(server);

  // Static files from LittleFS (dashboard SPA + wifi_config.html)
  // Serve static files from LittleFS.
  // ESPAsyncWebServer probes for filename.gz before filename on every
  // request. The "file not found" log lines for .gz are benign — the
  // library falls back to the uncompressed file automatically.
  // To silence the log: add -D ASYNCWEBSERVER_REGEX=0 and
  // -D CONFIG_ASYNC_TCP_RUNNING_CORE=0 to platformio.ini build_flags,
  // or gzip the HTML files before uploading (recommended for index.html).
  // To gzip: gzip -k index.html && gzip -k wifi_config.html
  // Place the .gz files alongside the originals in the data/ folder.
  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("/index.html")
        .setCacheControl("public max-age=86400");

  // CORS headers on all responses
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  // Catch-all: OPTIONS pre-flight + captive-portal redirect when AP is active
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *resp = req->beginResponse(204);
      resp->addHeader("Access-Control-Allow-Origin",  "*");
      resp->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
      resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
      req->send(resp);
    } else {
      // Redirect OS captive-portal probes to dashboard when AP on
      wifiHandleCatchAll(req);
    }
  });

  server.begin();
  Serial.println("[WEB] Server started on port 80");
}