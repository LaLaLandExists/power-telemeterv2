/**
 * node_tdma_task.cpp
 *
 * State machine:
 *   BOOT -> LISTEN -> CONTENDING -> REGISTERED -> (repeat via LISTEN each superframe)
 *
 * Timing model:
 *   - The node records beaconReceiveTime = millis() when a beacon lands.
 *   - Its TX time = beaconReceiveTime + BEACON_MS + (slotId-1) × SLOT_PAIR_MS
 *   - PZEM read starts PZEM_PRE_READ_MS (110 ms) before TX time.
 *   - After TX + DL window, node returns to LISTEN on Ch 0 for the next beacon.
 *
 * Dual clock:
 *   Clock 1 (TDMA timing) - re-derived from each beacon.  µs-level discipline.
 *   Clock 2 (Schedule RTC) - free-running millis() based wall clock.
 *                             Updated only when |delta| > RTC_CORRECTION_THRESHOLD_MS.
 *
 * Relay schedule evaluation:
 *   Uses Clock 2 (not the TDMA clock) to decide ON/OFF.  This prevents
 *   gateway browser-clock jitter from causing relay flicker at schedule boundaries.
 */

#include "node_tdma_task.h"
#include "log_async.h"
#include <RadioLib.h>
#include <PZEM004Tv30.h>

// --- Relay GPIO --------------------------------------------------------------
extern uint8_t RELAY_PIN;       // Defined in main.cpp (NODE_TELEMETRY build)
extern uint8_t LED_PIN;         // Defined in main.cpp (NODE_TELEMETRY build)

// --- Radio instance (defined in main.cpp) ------------------------------------
extern SX1278 radio;

// --- PZEM instance (defined in main.cpp) -------------------------------------
extern PZEM004Tv30 pzem;

// --- Global state definitions -------------------------------------------------
PzemData         g_pzem        = {};
SemaphoreHandle_t g_pzemMutex  = nullptr;

bool     g_nodeRegistered = false;
uint8_t  g_nodeSlotId     = 0;
uint16_t g_nodeUID        = 0;

uint8_t  g_relayState = 0;
uint8_t  g_relayMode  = 0;
uint8_t  g_schedState = 0;
uint8_t  g_schedSH    = 0;
uint8_t  g_schedSM    = 0;
uint8_t  g_schedEH    = 8;
uint8_t  g_schedEM    = 0;

uint32_t g_rtcBaseSec = 0;
uint32_t g_rtcBaseMs  = 0;
bool     g_rtcSet     = false;

static uint8_t g_seqCounter  = 0;
static int8_t  g_beaconRSSI  = -128;

// -----------------------------------------------------------------------------
// Relay control
// -----------------------------------------------------------------------------
void setRelay(uint8_t state) {
  g_relayState = state & 1;
  digitalWrite(RELAY_PIN, g_relayState ? HIGH : LOW);
}

// -----------------------------------------------------------------------------
// Schedule clock helpers
// -----------------------------------------------------------------------------

/** Set the schedule RTC from H/M/S (beacon time). */
static void rtcSet(uint8_t h, uint8_t m, uint8_t s) {
  g_rtcBaseSec = (uint32_t)h * 3600u + (uint32_t)m * 60u + s;
  g_rtcBaseMs  = millis();
  g_rtcSet     = true;
}

/**
 * Conditionally update the RTC from the beacon.
 * Only corrects if the delta exceeds RTC_CORRECTION_THRESHOLD_MS.
 * This prevents gateway browser-clock jitter from flickering relays.
 */
static void rtcConditionalSync(uint8_t beaconH, uint8_t beaconM, uint8_t beaconS) {
  uint32_t beaconSec = (uint32_t)beaconH * 3600u + beaconM * 60u + beaconS;
  if (!g_rtcSet) {
    rtcSet(beaconH, beaconM, beaconS);
    return;
  }
  uint32_t localSec  = rtcGetSec();
  int32_t  delta_ms  = (int32_t)(localSec  - beaconSec) * 1000;

  // Handle midnight wrap (±12 h window)
  if (delta_ms >  43200000L) delta_ms -= 86400000L;
  if (delta_ms < -43200000L) delta_ms += 86400000L;

  if (abs(delta_ms) > RTC_CORRECTION_THRESHOLD_MS) {
    rtcSet(beaconH, beaconM, beaconS);
    logAsync("[NODE-RTC] Corrected %+d ms\n", delta_ms);
  }
}

// -----------------------------------------------------------------------------
// Schedule evaluation
// Evaluates whether current RTC time falls inside the scheduled ON window.
// Supports midnight-wrap: endH:endM < startH:startM is allowed.
// Updates g_schedState and calls setRelay() when g_relayMode == 1.
// -----------------------------------------------------------------------------
static void evaluateSchedule() {
  if (g_relayMode != 1 || g_schedState == 0) return;

  uint16_t nowMins   = (uint16_t)(rtcGetSec() / 60);
  uint16_t startMins = (uint16_t)g_schedSH * 60 + g_schedSM;
  uint16_t endMins   = (uint16_t)g_schedEH * 60 + g_schedEM;

  bool inside;
  if (startMins <= endMins) {
    // Normal window: 08:00–17:00
    inside = (nowMins >= startMins && nowMins < endMins);
  } else {
    // Midnight-wrap: 22:00–06:00 -> inside if >=22:00 OR <06:00
    inside = (nowMins >= startMins || nowMins < endMins);
  }

  uint8_t newSchedState = inside ? 2 : 1;  // 2=ACTIVE, 1=WAITING
  if (newSchedState != g_schedState) {
    g_schedState = newSchedState;
    setRelay(inside ? 1 : 0);
    logAsync("[NODE-SCHED] %s (now=%02d:%02d)\n",
             inside ? "ACTIVE" : "WAITING",
             nowMins / 60, nowMins % 60);
  }
}

// -----------------------------------------------------------------------------
// PZEM_ENERGY_MAX_WH -- counter rolls over to 0 after this value.
// PZEM-004T v3 display limit: 9999.99 kWh = 9,999,990 Wh.
#define PZEM_ENERGY_MAX_WH 9999990UL

// Node-side energy delta state -- persists across superframes.
// pkt.energy carries Wh increments, not the raw counter, so the gateway
// accumulator is immune to PZEM counter rollovers and power-cycle resets.
static uint32_t s_lastPzemEnergy    = 0;
static bool     s_pzemEnergyBaseSet = false;

// Nudge - non-blocking LED blink via task notification
// -----------------------------------------------------------------------------
// nudgeTask() is a persistent task created once in nodeTdmaTaskStart(). It
// spends its entire life blocked in ulTaskNotifyTake(), consuming no CPU.
//
// nudge() - called from handleDownlink() on the TDMA task (Core 1) - simply
// calls xTaskNotifyGive(), which is an atomic increment of a counter already
// inside the TCB. No heap allocation, no scheduler overhead, returns in < 1 us.
//
// On wake, the task blinks for 3 seconds, then calls xTaskNotifyStateClear()
// before going back to wait. This drains any notifications that arrived during
// the blink (rapid repeated nudges), preventing a backlog of queued blinks.
// -----------------------------------------------------------------------------
static TaskHandle_t s_nudgeTaskHandle = nullptr;

static void nudgeTask(void* /*params*/) {
  while (true) {
    // Block indefinitely until nudge() sends a notification
    ulTaskNotifyTake(pdTRUE,          // clear notification count to 0 on wake
                      portMAX_DELAY);  // wait forever

    Serial.println("[NODE] Nudge!");
    for (int i = 0; i < 30; i++) {
      digitalWrite(LED_PIN, i & 1);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    digitalWrite(LED_PIN, LOW);

    // Drain any notifications that stacked up while we were blinking.
    // Without this, rapid nudge commands would queue up and blink N times.
    xTaskNotifyStateClear(nullptr);   // nullptr = clear own notification
  }
}

static void nudge() {
  if (s_nudgeTaskHandle == nullptr) return;   // task not yet created
  xTaskNotifyGive(s_nudgeTaskHandle);         // atomic; safe from any core/ISR
}

// -----------------------------------------------------------------------------
// Process downlink command received in DL window
// -----------------------------------------------------------------------------
static void handleDownlink(const uint8_t* buf, int16_t len) {
  if (len < 2) return;
  uint8_t type   = buf[0];
  uint8_t nodeId = buf[1];

  if (nodeId != g_nodeSlotId && nodeId != BROADCAST_ADDR) return;

  switch (type) {
  case PKT_RELAY_MANUAL:   // 3 bytes: type, nodeId, state
    if (len >= 3) {
      g_relayMode  = 0;       // Switch to MANUAL
      g_schedState = 0;       // Clear schedule
      setRelay(buf[2]);
      logAsync("[NODE-DL] Relay manual -> %s\n", buf[2] ? "ON" : "OFF");
    }
    break;

  case PKT_RELAY_SCHEDULE: // 7 bytes: type, nodeId, onState, sH, sM, eH, eM
    if (len >= 7) {
      g_relayMode = 1;
      g_schedSH   = buf[3];
      g_schedSM   = buf[4];
      g_schedEH   = buf[5];
      g_schedEM   = buf[6];
      g_schedState = 1;       // Start as WAITING - evaluateSchedule() will flip
      evaluateSchedule();
      logAsync("[NODE-DL] Schedule set %02d:%02d - %02d:%02d\n",
               g_schedSH, g_schedSM, g_schedEH, g_schedEM);
    }
    break;

  case PKT_RELAY_CLEAR:    // 2 bytes: type, nodeId
    g_relayMode  = 0;
    g_schedState = 0;
    // Relay stays in its current state; do not force ON or OFF
    logAsync("[NODE-DL] Schedule cleared\n");
    break;

  case PKT_THRESHOLD:      // 4 bytes: type, nodeId, threshW_lo, threshW_hi
    if (len >= 4) {
      uint16_t watts = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
      // Update cached value and queue the write for pzemTask.
      // pzemTask is the sole owner of Serial2; writing from the TDMA
      // task here would race with active Modbus read transactions.
      if (xSemaphoreTake(g_pzemMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_pzem.alarmThreshold      = watts;  // cached: reflected in next TX
        g_pzem.pendingThresholdW   = watts;  // bus write: deferred to pzemTask
        g_pzem.hasPendingThreshold = true;
        xSemaphoreGive(g_pzemMutex);
      }
      logAsync("[NODE-DL] Alarm threshold queued -> %d W\n", watts);
    }
    break;

  case PKT_NUDGE:          // 2 bytes: type, nodeId
    nudge();
    break;

  default:
    logAsync("[NODE-DL] Unknown packet type 0x%02X\n", type);
    break;
  }
}

// -----------------------------------------------------------------------------
// Build and transmit TelemetryPacket
// -----------------------------------------------------------------------------
static void transmitTelemetry() {
  TelemetryPacket pkt = {};

  if (xSemaphoreTake(g_pzemMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    // Convert float readings to fixed-point wire format
    pkt.voltage       = (uint16_t)(g_pzem.voltage      * 10.0f);
    pkt.current       = (uint32_t)(g_pzem.current      * 1000.0f);
    pkt.power         = (uint32_t)(g_pzem.power        * 10.0f);
    // Compute Wh increment since last packet (node-side delta encoding).
    // Sending deltas makes the gateway accumulator immune to PZEM counter
    // rollovers (at PZEM_ENERGY_MAX_WH) and power-cycle resets.
    uint32_t rawEnergy = g_pzem.energy;
    uint32_t energyDelta;
    if (!s_pzemEnergyBaseSet) {
      s_lastPzemEnergy    = rawEnergy;
      s_pzemEnergyBaseSet = true;
      energyDelta = 0;  // first packet -- no increment yet
    } else if (rawEnergy >= s_lastPzemEnergy) {
      energyDelta = rawEnergy - s_lastPzemEnergy;  // normal increment
    } else {
      // Counter rolled over: Wh consumed = (max - last) + post-rollover
      energyDelta = (PZEM_ENERGY_MAX_WH - s_lastPzemEnergy) + rawEnergy;
    }
    s_lastPzemEnergy = rawEnergy;
    pkt.energy = energyDelta;   // Wh increment since last packet
    pkt.frequency     = (uint16_t)(g_pzem.frequency    * 10.0f);
    pkt.powerFactor   = (uint16_t)(g_pzem.powerFactor  * 100.0f);
    pkt.alarmThreshold = g_pzem.alarmThreshold;
    xSemaphoreGive(g_pzemMutex);
  }

  // Determine alarm state from PZEM data (power > threshold)
  uint8_t alarmState = 0;
  if (pkt.alarmThreshold > 0 &&
    (pkt.power / 10) >= pkt.alarmThreshold) {
    alarmState = 1;
  }

  pkt.pktType    = PKT_TELEMETRY;
  pkt.nodeId     = g_nodeSlotId;
  pkt.statusByte = encodeStatus(g_relayState, g_relayMode, g_schedState, alarmState);
  pkt.schedSH    = g_schedSH;
  pkt.schedSM    = g_schedSM;
  pkt.schedEH    = g_schedEH;
  pkt.schedEM    = g_schedEM;
  pkt.seqCounter = ++g_seqCounter;
  pkt.beaconRSSI = (uint8_t)(int8_t)g_beaconRSSI;
  pkt.fwVersion  = FW_VERSION;

  int16_t st = radio.transmit(reinterpret_cast<uint8_t*>(&pkt), sizeof(pkt));
  if (st == RADIOLIB_ERR_NONE) {
    logAsync("[NODE-TX] V=%.1f A=%.3f W=%.1f seq=%d\n",
                 g_pzem.voltage, g_pzem.current, g_pzem.power, g_seqCounter);
  } else {
    logAsync("[NODE-TX] Error %d\n", st);
  }
}

// -----------------------------------------------------------------------------
// Receive with timeout helper
// Radio must be in startReceive() before calling.
// Returns bytes received, or -1 on timeout.
// -----------------------------------------------------------------------------
static int16_t rxWindow(uint8_t* buf, size_t maxLen, uint32_t windowMs) {
  uint32_t deadline = millis() + windowMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (radio.available()) {
      int len = radio.getPacketLength();
      if (len > 0 && (size_t)len <= maxLen) {
        int16_t st = radio.readData(buf, (size_t)len);
        if (st == RADIOLIB_ERR_NONE) return (int16_t)len;
      }
      return -1;
    }
    taskYIELD();
  }
  return -1;
}

// -----------------------------------------------------------------------------
// TDMA node state machine
// -----------------------------------------------------------------------------
typedef enum { ST_LISTEN, ST_CONTENDING, ST_REGISTERED } NodeState_t;

static void nodeTdmaTask(void* /*params*/) {
  logAsync("[NODE-TDMA] Task started on Core 1\n");

  NodeState_t state            = ST_LISTEN;
  uint32_t    beaconReceiveTime = 0;
  uint16_t    sfCount           = 0;

  // Tune to Ch 0 and start continuous receive
  radio.setFrequency(LORA_CHANNELS[0]);
  radio.startReceive();

  while (true) {

    // -- LISTEN: wait for a beacon on Ch 0 --------------------------------
    if (state == ST_LISTEN || state == ST_CONTENDING || state == ST_REGISTERED) {
      // Wait up to (SUPERFRAME_MS + 200ms margin) for the next beacon
      uint8_t buf[64];
      radio.setFrequency(LORA_CHANNELS[0]);
      radio.startReceive();

      uint32_t listenTimeout = (state == ST_LISTEN) ? 10000 : SUPERFRAME_MS + 200;
      int16_t  len           = rxWindow(buf, sizeof(buf), listenTimeout);

      if (len == (int16_t)sizeof(BeaconPacket) && buf[0] == PKT_BEACON) {
        beaconReceiveTime = millis();
        const BeaconPacket* bcn = reinterpret_cast<const BeaconPacket*>(buf);
        sfCount           = bcn->sfCount;
        g_beaconRSSI      = (int8_t)radio.getRSSI();

        // Update schedule RTC (conditional - see dual clock design)
        rtcConditionalSync(bcn->hour, bcn->minute, bcn->second);

        // Evaluate relay schedule
        evaluateSchedule();

        if (g_nodeRegistered) {
          // Verify our slot is still in the mask
          if (bcn->slotMask & (1u << (g_nodeSlotId - 1))) {
            state = ST_REGISTERED;
          } else {
            // Gateway dropped our slot (edge case - re-register)
            g_nodeRegistered = false;
            state = ST_CONTENDING;
          }
        } else {
          state = ST_CONTENDING;
        }

      } else if (state != ST_LISTEN) {
        // Missed beacon - go back to basic listen mode
        logAsync("[NODE-TDMA] Beacon timeout - re-listening\n");
        state = ST_LISTEN;
        continue;
      } else {
        // Still in initial LISTEN and no beacon yet - retry
        continue;
      }
    }

    // -- CONTENDING: transmit join request in contention window -----------
    if (state == ST_CONTENDING) {
      uint32_t cwStart = beaconReceiveTime
                             + BEACON_MS
                             + (uint32_t)MAX_NODES * SLOT_PAIR_MS;

      waitUntilMs(cwStart);

      // Random backoff (0–30 ms) to reduce collision probability
      vTaskDelay((esp_random() % 30) / portTICK_PERIOD_MS);

      JoinRequestPacket req;
      req.pktType   = PKT_JOIN_REQUEST;
      req.uid_lo    = (uint8_t)(g_nodeUID & 0xFF);
      req.uid_hi    = (uint8_t)(g_nodeUID >> 8);
      req.fwVersion = FW_VERSION;

      radio.setFrequency(LORA_CHANNELS[0]);
      radio.transmit(reinterpret_cast<uint8_t*>(&req), sizeof(req));
      logAsync("[NODE-JOIN] Sent JoinReq UID=0x%04X\n", g_nodeUID);

      // Listen for ACK during contention DL window
      radio.startReceive();
      uint8_t ackBuf[8];
      uint32_t ackDeadline = cwStart + CONTENTION_UL_MS + CONTENTION_DL_MS;
      int16_t  ackLen      = rxWindow(ackBuf, sizeof(ackBuf),
                      ackDeadline - millis());

      if (ackLen == (int16_t)sizeof(JoinAckPacket) &&
        ackBuf[0] == PKT_JOIN_ACK) {
        const JoinAckPacket* ack = reinterpret_cast<const JoinAckPacket*>(ackBuf);
        uint16_t echoed = ((uint16_t)ack->uid_hi << 8) | ack->uid_lo;

        if (echoed == g_nodeUID && ack->slotId >= 1 && ack->slotId <= MAX_NODES) {
          g_nodeSlotId     = ack->slotId;
          g_nodeRegistered = true;
          logAsync("[NODE-JOIN] Registered! slotId=%d\n", g_nodeSlotId);
          // Fall through to REGISTERED processing on next iteration
          state = ST_LISTEN;  // re-sync with next beacon before first TX
          continue;
        }
      }

      // No ACK or UID mismatch (collision) - retry next superframe
      logAsync("[NODE-JOIN] No ACK - retrying next superframe\n");
      state = ST_LISTEN;
      continue;
    }

    // -- REGISTERED: wait for our TX slot, transmit, listen for DL --------
    if (state == ST_REGISTERED) {
      // TX time = beaconReceiveTime + beacon zone + preceding slot pairs
      uint32_t txTime = beaconReceiveTime
              + BEACON_MS
              + (uint32_t)(g_nodeSlotId - 1) * SLOT_PAIR_MS;

      // Start PZEM read early so data is ready at TX time
      uint32_t pzemReadStart = (txTime > PZEM_PRE_READ_MS)
                                   ? (txTime - PZEM_PRE_READ_MS) : 0;

      // Wait until PZEM pre-read window opens (if still in the future)
      if ((int32_t)(pzemReadStart - millis()) > 0) {
        waitUntilMs(pzemReadStart);
      }
      // The PZEM task runs asynchronously and continuously refreshes g_pzem.
      // If we're here, g_pzem already has a fresh-enough reading.
      // (The PZEM task samples every ~500 ms on Core 0.)

      // Wait until actual TX time
      waitUntilMs(txTime);

      // Set correct hop channel and transmit
      uint8_t ch = hopChannel(sfCount, g_nodeSlotId);
      radio.setFrequency(LORA_CHANNELS[ch]);
      delayMicroseconds(PHASE_GUARD_US);  // PLL settle

      transmitTelemetry();

      // -- DL receive window -----------------------------------------
      uint32_t dlStart = txTime + SLOT_UL_MS;
      waitUntilMs(dlStart);

      // Radio is now in TX idle - switch to RX
      radio.setFrequency(LORA_CHANNELS[ch]);
      radio.startReceive();

      uint8_t dlBuf[16];
      int16_t dlLen = rxWindow(dlBuf, sizeof(dlBuf), SLOT_DL_MS);
      if (dlLen > 0) {
        handleDownlink(dlBuf, dlLen);
      }

      waitUntilMs(dlStart + SLOT_DL_MS + SLOT_GUARD_MS);

      // Re-enter LISTEN to sync with next beacon
      state = ST_LISTEN;
    }
  }
}

// -----------------------------------------------------------------------------
// PZEM sampling task (Core 0, priority 1)
// Continuously reads all PZEM registers every ~500 ms.
// Stores results in g_pzem under g_pzemMutex.
// -----------------------------------------------------------------------------
static void pzemTask(void* /*params*/) {
  Serial.println("[PZEM] Sampling task started on Core 0");

  while (true) {
    float v  = pzem.voltage();
    float i  = pzem.current();
    float p  = pzem.power();
    float e  = pzem.energy();      // Library returns Wh (raw register value)
    float f  = pzem.frequency();
    float pf = pzem.pf();

    bool valid = !isnan(v) && !isnan(i) && !isnan(p) && !isnan(f);

    if (valid) {
      if (xSemaphoreTake(g_pzemMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_pzem.voltage     = v;
        g_pzem.current     = i;
        g_pzem.power       = p;
        g_pzem.energy      = isnan(e) ? g_pzem.energy : (uint32_t)e;
        g_pzem.frequency   = f;
        g_pzem.powerFactor = isnan(pf) ? 0.0f : pf;
        g_pzem.valid       = true;
        g_pzem.readAt      = millis();
        xSemaphoreGive(g_pzemMutex);
      }
    } else {
      // PZEM read error - log and continue with cached values
      static uint32_t lastErrorLog = 0;
      if (millis() - lastErrorLog > 5000) {
        Serial.println("[PZEM] Read error - check wiring/baud");
        lastErrorLog = millis();
      }
    }

    // -- Pending threshold write -----------------------------------------
    // The TDMA task cannot safely touch Serial2 (races with read transactions).
    // It sets hasPendingThreshold under g_pzemMutex; we consume it here as
    // the sole bus owner, using the library's setPowerAlarm().
    bool doThreshold = false;
    uint16_t threshWatts = 0;
    if (xSemaphoreTake(g_pzemMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (g_pzem.hasPendingThreshold) {
        doThreshold                = true;
        threshWatts                = g_pzem.pendingThresholdW;
        g_pzem.hasPendingThreshold = false;
      }
      xSemaphoreGive(g_pzemMutex);
    }
    if (doThreshold) {
      bool ok = pzem.setPowerAlarm(threshWatts);
      Serial.printf("[PZEM] Alarm threshold -> %d W (%s)\n",
                          threshWatts, ok ? "OK" : "FAIL");
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// -----------------------------------------------------------------------------
// Schedule evaluation periodic task (Core 0, low frequency)
// Evaluates relay schedule every 10 s so the TDMA task doesn't have to.
// -----------------------------------------------------------------------------
static void schedTask(void* /*params*/) {
  while (true) {
    evaluateSchedule();
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

// -----------------------------------------------------------------------------
// Task launcher
// -----------------------------------------------------------------------------
void nodeTdmaTaskStart() {
  g_pzemMutex = xSemaphoreCreateMutex();
  configASSERT(g_pzemMutex);

  g_nodeUID = computeDeviceUID();
  Serial.printf("[NODE] Device UID = 0x%04X\n", g_nodeUID);

  // PZEM sampling - Core 0, priority 1 (below WiFi if present)
  xTaskCreatePinnedToCore(pzemTask, "PZEM", 4096, nullptr, 1, nullptr, 0);

  // Schedule evaluator - Core 0, very low priority
  xTaskCreatePinnedToCore(schedTask, "SCHED", 2048, nullptr, 1, nullptr, 0);

  // Nudge blink task - Core 0, priority 1; blocks on task notification
  xTaskCreatePinnedToCore(nudgeTask, "NUDGE", 1024, nullptr, 1,
                          &s_nudgeTaskHandle, 0);

  // TDMA radio task - Core 1, priority 2
  xTaskCreatePinnedToCore(nodeTdmaTask, "NODE_TDMA", 8192, nullptr, 2, nullptr, 1);
}
