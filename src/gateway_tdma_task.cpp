/**
 * gateway_tdma_task.cpp
 *
 * Core TDMA engine for the gateway.
 *
 * Superframe structure (~1845 ms total):
 *   [Beacon 40 ms] [Slot-1..8 each 210 ms] [Contention UL 60 ms + DL 40 ms] [Guard 20 ms]
 *
 * Each slot pair:
 *   [UL RX window 80 ms] [DL TX window 80 ms] [Guard 50 ms]
 *   Channel = hopChannel(sfCount, slotId)
 *   Phase guard (500 µs) applied between pairs where channel changes.
 *
 * Timing discipline:
 *   All waits are anchored to sfStart (millis() at beacon TX).
 *   waitUntilMs() is used everywhere to prevent accumulated drift.
 *
 * Thread safety:
 *   g_nodesMutex protects all g_nodes[] writes.
 *   The web task (Core 0) must also hold the mutex before reading g_nodes[].
 */

#include "gateway_tdma_task.h"
#include "gateway_web.h"          // webBroadcastTelemetry()
#include "fram_store.h"
#include "log_async.h"
#include <RadioLib.h>

// Dirty counters for deferred FRAM writes (n=10 policy)
#define FRAM_WRITE_N 10
static uint8_t s_energyDirty[MAX_NODES] = {};
static uint8_t s_histDirty[MAX_NODES]   = {};

// --- Radio instance (SPI pins defined in main.cpp) -----------------------
extern SX1278 radio;

// --- Shared state definitions (extern in gateway_state.h) -------------------
NodeState         g_nodes[MAX_NODES]  = {};
SemaphoreHandle_t g_nodesMutex        = nullptr;
uint16_t          g_sfCount           = 0;
uint8_t           g_slotMask          = 0;
uint8_t           g_gwHour            = 0;
uint8_t           g_gwMinute          = 0;
uint8_t           g_gwSecond          = 0;
uint32_t          g_gwTimeAt          = 0;
bool              g_timeSet           = false;

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

/** Change frequency and wait for PLL to re-lock (~500 µs). */
static void setChannel(uint8_t ch) {
  radio.setFrequency(LORA_CHANNELS[ch]);
  delayMicroseconds(PHASE_GUARD_US);
}

/**
 * Wait for a LoRa packet within a time window.
 * Radio must already be in startReceive() mode before calling.
 *
 * @param buf        Output buffer
 * @param maxLen     Buffer size
 * @param windowMs   How long to listen (ms)
 * @return           Bytes received, or -1 on timeout/error
 */
static int16_t rxWindow(uint8_t* buf, size_t maxLen, uint32_t windowMs) {
  uint32_t deadline = millis() + windowMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (radio.available()) {
      int len = radio.getPacketLength();
      if (len > 0 && (size_t)len <= maxLen) {
        int16_t st = radio.readData(buf, (size_t)len);
        if (st == RADIOLIB_ERR_NONE) return (int16_t)len;
      }
      return -1;  // CRC error or oversized - caller restarts receive
    }
    taskYIELD();
  }
  return -1;  // Timeout
}

// -----------------------------------------------------------------------------
// Energy accumulation
// -----------------------------------------------------------------------------
static void accumulateEnergy(NodeState* ns, uint32_t deltaWh) {
  // pkt.energy is now a Wh increment computed on the node, not a raw counter.
  // Rollover and power-cycle resets are handled node-side; the gateway just adds.
  ns->accumEnergy += deltaWh;

  uint8_t idx = (uint8_t)(ns - g_nodes);
  if (++s_energyDirty[idx] >= FRAM_WRITE_N) {
    s_energyDirty[idx] = 0;
    framSaveEnergy(idx);
  }
}

// -----------------------------------------------------------------------------
// History buffer
// -----------------------------------------------------------------------------
static void addHistory(NodeState* ns, const TelemetryPacket& pkt) {
  HistoryPoint& hp = ns->history[ns->histHead];
  hp.t = millis();
  hp.v = pkt.voltage     / 10.0f;
  hp.i = pkt.current     / 1000.0f;
  hp.p = pkt.power       / 10.0f;

  ns->histHead = (ns->histHead + 1) % HISTORY_MAX_POINTS;
  if (ns->histCount < HISTORY_MAX_POINTS) ns->histCount++;

  uint8_t idx = (uint8_t)(ns - g_nodes);
  if (++s_histDirty[idx] >= FRAM_WRITE_N) {
    s_histDirty[idx] = 0;
    framSaveHistory(idx);
  }
}

// -----------------------------------------------------------------------------
// Process a received TelemetryPacket
// -----------------------------------------------------------------------------
static void processUplink(uint8_t slotIdx, const uint8_t* buf, int16_t rssi) {
  if ((size_t)sizeof(TelemetryPacket) > MAX_NODES * 32) return; // sanity
  const TelemetryPacket* pkt = reinterpret_cast<const TelemetryPacket*>(buf);

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  NodeState* ns = &g_nodes[slotIdx];
  memcpy(&ns->latest, pkt, sizeof(TelemetryPacket));
  ns->hasData    = true;
  ns->rssi       = rssi;
  ns->lastSeen   = millis();

  // Decode status byte
  ns->relayState = decodeRelayState(pkt->statusByte);
  ns->relayMode  = decodeRelayMode (pkt->statusByte);
  ns->schedState = decodeSchedState(pkt->statusByte);
  ns->alarmState = decodeAlarmState(pkt->statusByte);
  ns->fwVersion  = pkt->fwVersion;
  ns->beaconRSSI = pkt->beaconRSSI;
  ns->seqLast    = pkt->seqCounter;

  accumulateEnergy(ns, pkt->energy);
  addHistory(ns, *pkt);

  // Resolve pending flag: check if node state matches what was commanded
  if (ns->pending) {
    bool confirmed = false;
    if (ns->queuedCmd.len >= 3 && ns->queuedCmd.data[0] == PKT_RELAY_MANUAL) {
      confirmed = (ns->relayState == ns->queuedCmd.data[2]);
    } else if (ns->queuedCmd.len >= 7 && ns->queuedCmd.data[0] == PKT_RELAY_SCHEDULE) {
      confirmed = (ns->relayMode == 1 && ns->schedState > 0);
    } else if (ns->queuedCmd.len >= 2 && ns->queuedCmd.data[0] == PKT_RELAY_CLEAR) {
      confirmed = (ns->relayMode == 0 && ns->schedState == 0);
    } else {
      // Threshold, nudge - no confirmation from telemetry, rely on timeout
      if ((millis() - ns->pendingSentAt) >= PENDING_TIMEOUT_MS) {
        ns->pending = false;
      }
    }
    if (confirmed) ns->pending = false;
  }
  // Also clear pending on timeout regardless of command type
  if (ns->pending && (millis() - ns->pendingSentAt) >= PENDING_TIMEOUT_MS) {
    ns->pending = false;
  }

  xSemaphoreGive(g_nodesMutex);

  // Push to WebSocket clients on Core 0 (non-blocking, safe from Core 1)
  webBroadcastTelemetry(slotIdx);

  logAsync("[GW-UL] Slot%d V=%.1f A=%.3f W=%.1f Wh=%lu RSSI=%d\n",
           pkt->nodeId,
           pkt->voltage / 10.0f,
           pkt->current / 1000.0f,
           pkt->power   / 10.0f,
           (unsigned long)ns->accumEnergy,
           rssi);
}

// -----------------------------------------------------------------------------
// Build beacon and transmit on Ch 0
// -----------------------------------------------------------------------------
static void sendBeacon() {
  BeaconPacket beacon;
  beacon.pktType  = PKT_BEACON;
  beacon.addr     = BROADCAST_ADDR;
  beacon.sfCount  = g_sfCount;
  beacon.slotMask = g_slotMask;
  getGwHMS(beacon.hour, beacon.minute, beacon.second);

  setChannel(0);
  int16_t st = radio.transmit(reinterpret_cast<uint8_t*>(&beacon), sizeof(beacon));
  if (st != RADIOLIB_ERR_NONE) {
    logAsync("[GW-BCN] TX error %d\n", st);
  }
}

// -----------------------------------------------------------------------------
// Downlink TX - send queued command to node
// Returns true if TX succeeded.
// -----------------------------------------------------------------------------
static bool sendDownlink(uint8_t slotIdx) {
  NodeState* ns = &g_nodes[slotIdx];
  if (!ns->queuedCmd.active || ns->queuedCmd.len == 0) return false;

  int16_t st = radio.transmit(ns->queuedCmd.data, ns->queuedCmd.len);
  bool ok = (st == RADIOLIB_ERR_NONE);

  if (ok) {
    ns->pending       = true;
    ns->pendingSentAt = millis();
    ns->queuedCmd.active = false;
    logAsync("[GW-DL] Slot%d type=0x%02X len=%d\n",
             slotIdx + 1, ns->queuedCmd.data[0], ns->queuedCmd.len);
  } else {
    logAsync("[GW-DL] TX error %d for slot%d\n", st, slotIdx + 1);
  }
  return ok;
}

// -----------------------------------------------------------------------------
// Contention window - register new nodes
// -----------------------------------------------------------------------------
static void handleContentionWindow(uint32_t sfStart) {
  uint32_t cwStart = sfStart + BEACON_MS + (uint32_t)MAX_NODES * SLOT_PAIR_MS;

  // Contention UL: listen for join requests on Ch 0
  setChannel(0);
  radio.startReceive();

  uint8_t buf[16];
  waitUntilMs(cwStart);
  int16_t len = rxWindow(buf, sizeof(buf), CONTENTION_UL_MS);

  uint32_t dlStart = cwStart + CONTENTION_UL_MS;
  waitUntilMs(dlStart);

  if (len == (int16_t)sizeof(JoinRequestPacket) && buf[0] == PKT_JOIN_REQUEST) {
    const JoinRequestPacket* req = reinterpret_cast<const JoinRequestPacket*>(buf);
    uint16_t uid = ((uint16_t)req->uid_hi << 8) | req->uid_lo;

    logAsync("[GW-CW] JoinReq UID=0x%04X fw=%d\n", uid, req->fwVersion);

    // Find existing slot for this UID, or allocate a new one
    uint8_t targetSlot = 0;
    if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      // Check if already registered (re-boot case)
      for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].active && g_nodes[i].deviceUID == uid) {
          targetSlot = g_nodes[i].slotId;
          break;
        }
      }
      // Allocate new slot
      if (targetSlot == 0) {
        for (uint8_t i = 0; i < MAX_NODES; i++) {
          if (!g_nodes[i].active) {
            g_nodes[i].active    = true;
            g_nodes[i].slotId    = i + 1;
            g_nodes[i].deviceUID = uid;
            snprintf(g_nodes[i].label, sizeof(g_nodes[i].label),
                      "Node %d", i + 1);
            g_slotMask |= (1u << i);
            targetSlot  = i + 1;
            logAsync("[GW-CW] Assigned slot %d to UID=0x%04X\n",
                     targetSlot, uid);
            break;
          }
        }
      }
      xSemaphoreGive(g_nodesMutex);
    }

    if (targetSlot > 0) {
      // Send ACK on Ch 0 during contention DL window
      JoinAckPacket ack;
      ack.pktType = PKT_JOIN_ACK;
      ack.uid_lo  = req->uid_lo;
      ack.uid_hi  = req->uid_hi;
      ack.slotId  = targetSlot;
      radio.transmit(reinterpret_cast<uint8_t*>(&ack), sizeof(ack));
      logAsync("[GW-CW] JoinACK slot=%d\n", targetSlot);
    }
  }

  waitUntilMs(dlStart + CONTENTION_DL_MS);
}

// -----------------------------------------------------------------------------
// Main TDMA task
// -----------------------------------------------------------------------------
static void gatewayTdmaTask(void* /*params*/) {
  logAsync("[GW-TDMA] Task started on Core 1\n");

  while (true) {
    uint32_t sfStart = millis();
    g_sfCount++;

    // -- Zone 1: Beacon --------------------------------------------------
    sendBeacon();
    waitUntilMs(sfStart + BEACON_MS);

    // -- Zone 2: Slot pairs ----------------------------------------------
    for (uint8_t s = 0; s < MAX_NODES; s++) {
      uint32_t slotBase = sfStart + BEACON_MS + (uint32_t)s * SLOT_PAIR_MS;

      waitUntilMs(slotBase);

      // Skip unoccupied slots - advance time but don't touch radio
      if (!(g_slotMask & (1u << s))) {
        waitUntilMs(slotBase + SLOT_PAIR_MS);
        continue;
      }

      // Frequency hop
      uint8_t ch = hopChannel(g_sfCount, s + 1);
      setChannel(ch);

      // -- UL receive window -----------------------------------------
      radio.startReceive();
      uint8_t ulBuf[64];
      int16_t ulLen = rxWindow(ulBuf, sizeof(ulBuf), SLOT_UL_MS);

      if (ulLen == (int16_t)sizeof(TelemetryPacket) &&
        ulBuf[0] == PKT_TELEMETRY &&
        ulBuf[1] == (s + 1))        // nodeId sanity check
      {
        int16_t rssi = radio.getRSSI();
        processUplink(s, ulBuf, rssi);
      }

      waitUntilMs(slotBase + SLOT_UL_MS);

      // -- DL transmit window ----------------------------------------
      // No channel change between UL and DL of the same pair.
      bool hasDL = false;
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        hasDL = g_nodes[s].queuedCmd.active;
        xSemaphoreGive(g_nodesMutex);
      }

      if (hasDL) {
        // Small gap to ensure node has switched to RX
        vTaskDelay(pdMS_TO_TICKS(5));
        if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sendDownlink(s);
          xSemaphoreGive(g_nodesMutex);
        }
      }

      waitUntilMs(slotBase + SLOT_UL_MS + SLOT_DL_MS);

      // -- Intra-pair guard ------------------------------------------
      waitUntilMs(slotBase + SLOT_PAIR_MS);
    }

    // -- Zone 3: Contention window ----------------------------------------
    handleContentionWindow(sfStart);

    // -- Zone 4: End guard ------------------------------------------------
    uint32_t sfEnd = sfStart + SUPERFRAME_MS;
    waitUntilMs(sfEnd);
  }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void gatewayTdmaTaskStart() {
  g_nodesMutex = xSemaphoreCreateMutex();
  configASSERT(g_nodesMutex);

  xTaskCreatePinnedToCore(
    gatewayTdmaTask,
    "GW_TDMA",
    8192,        // Stack in bytes (6k for tx/rx + JSON + Serial)
    nullptr,
    2,           // Priority 2 (higher than WiFi stack at 1)
    nullptr,
    1            // Core 1
  );
}

bool tdmaQueueCommand(uint8_t slotIdx, const uint8_t* data, uint8_t len) {
  if (slotIdx >= MAX_NODES || len == 0 || len > 8) return false;

  bool ok = false;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    NodeState* ns = &g_nodes[slotIdx];
    if (ns->active) {
      memcpy(ns->queuedCmd.data, data, len);
      ns->queuedCmd.len    = len;
      ns->queuedCmd.active = true;
      ok = true;
    }
    xSemaphoreGive(g_nodesMutex);
  }
  return ok;
}

uint8_t tdmaFindSlotByNodeId(uint8_t nodeId) {
  for (uint8_t i = 0; i < MAX_NODES; i++) {
    if (g_nodes[i].active && g_nodes[i].slotId == nodeId) return i;
  }
  return 0xFF;
}
