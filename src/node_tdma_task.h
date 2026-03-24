/**
 * node_tdma_task.h
 * Node-side TDMA FreeRTOS task public API.
 */
#pragma once
#include <Arduino.h>
#include "lora_tdma_protocol.h"

// ─── PZEM data shared between the PZEM task and the TDMA task ────────────────
struct PzemData {
    float    voltage;       // V
    float    current;       // A
    float    power;         // W
    uint32_t energy;        // Wh (raw from PZEM register × scale)
    float    frequency;     // Hz
    float    powerFactor;   // 0.00–1.00
    uint16_t alarmThreshold; // watts, last value written to PZEM
    bool     valid;          // At least one successful read
    uint32_t readAt;         // millis() of last successful read

    // Pending threshold write — set by TDMA task, consumed by pzemTask.
    // Both fields are protected by g_pzemMutex.
    uint16_t pendingThresholdW;    // Watts to write (valid when hasPendingThreshold)
    bool     hasPendingThreshold;  // true = pzemTask should write threshold on next cycle
};

extern PzemData         g_pzem;
extern SemaphoreHandle_t g_pzemMutex;

// ─── Node persistent state ────────────────────────────────────────────────────
extern bool     g_nodeRegistered;
extern uint8_t  g_nodeSlotId;
extern uint16_t g_nodeUID;

// ─── Relay / schedule state ──────────────────────────────────────────────────
extern uint8_t  g_relayState;   // 0=OFF, 1=ON
extern uint8_t  g_relayMode;    // 0=MANUAL, 1=SCHEDULED
extern uint8_t  g_schedState;   // 0=NONE, 1=WAITING, 2=ACTIVE
extern uint8_t  g_schedSH, g_schedSM, g_schedEH, g_schedEM;

// ─── Schedule / RTC clock domain ─────────────────────────────────────────────
// Runs autonomously on millis(); only corrected when delta vs beacon > 2 s.
extern uint32_t g_rtcBaseSec;    // Seconds-since-midnight base
extern uint32_t g_rtcBaseMs;     // millis() when rtcBaseSec was set
extern bool     g_rtcSet;

/** Get current RTC time in seconds-since-midnight. */
inline uint32_t rtcGetSec() {
    if (!g_rtcSet) return 0;
    return (g_rtcBaseSec + (millis() - g_rtcBaseMs) / 1000UL) % 86400UL;
}

/**
 * Launch the TDMA task (Core 1, priority 2).
 * Also launches the PZEM sampling task (Core 0, priority 1).
 * Call after radio and PZEM hardware are initialised.
 */
void nodeTdmaTaskStart();

/**
 * Set relay GPIO and update g_relayState.
 * Thread-safe — may be called from any task.
 */
void setRelay(uint8_t state);
