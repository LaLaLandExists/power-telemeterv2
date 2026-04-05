/**
 * lora_tdma_protocol.h
 * Shared header for gateway AND node firmware.
 * Copy this file into both the gateway/ and node/ sketch folders.
 *
 * Protocol v1.0 — SF7 / 125 kHz / 433 MHz / 8-node star topology
 */
#pragma once
#include <Arduino.h>
#include <esp_efuse.h>    // esp_efuse_mac_get_default()

// -----------------------------------------------------------------------------
// Network constants
// -----------------------------------------------------------------------------
#define FW_VERSION          1
#define MAX_NODES           8
#define N_CHANNELS          8
#define LORA_SYNC_WORD      0x12    // Private network (not LoRaWAN 0x34)
#define BROADCAST_ADDR      0xFF

// -----------------------------------------------------------------------------
// Radio parameters  (SX1278 via RadioLib)
// -----------------------------------------------------------------------------
#define LORA_FREQUENCY      433.05f  // Ch 0 start; setFrequency() called per hop
#define LORA_BANDWIDTH      125.0f   // kHz
#define LORA_SF             7
#define LORA_CR             5        // RadioLib value: 5 = 4/5 coding rate
#define LORA_TX_POWER       17       // dBm, PA_BOOST pin
#define LORA_PREAMBLE_LEN   8

// -----------------------------------------------------------------------------
// Channel plan — 433 MHz ISM, 200 kHz spacing
// Ch 0 is the fixed rendezvous channel (beacon + contention).
// -----------------------------------------------------------------------------
static const float LORA_CHANNELS[N_CHANNELS] = {
    433.050f,   // Ch 0 — Beacon + contention (FIXED)
    433.250f,   // Ch 1
    433.450f,   // Ch 2
    433.650f,   // Ch 3
    433.850f,   // Ch 4
    434.050f,   // Ch 5
    434.250f,   // Ch 6
    434.450f,   // Ch 7
};

// -----------------------------------------------------------------------------
// TDMA timing constants (milliseconds)
//
// Superframe = Beacon + N×SlotPair + PhaseGuards + CWin_UL + CWin_DL + EndGuard
// ≈ 40 + 8×210 + 4.5 + 60 + 40 + 20 ≈ 1845 ms
//
// SlotPair breakdown (from gateway perspective, node reads PZEM before slot):
//   UL window  : 80 ms  (node TX -> GW RX)
//   DL window  : 80 ms  (GW TX -> node RX, silence if no command)
//   Guard      : 50 ms  (absorbs jitter before next pair)
// -----------------------------------------------------------------------------
#define BEACON_MS               40
#define SLOT_UL_MS              80
#define SLOT_DL_MS              80
#define SLOT_GUARD_MS           50
#define SLOT_PAIR_MS            (SLOT_UL_MS + SLOT_DL_MS + SLOT_GUARD_MS)  // 210 ms
#define PHASE_GUARD_US          500     // us - PLL re-lock between hops
#define CONTENTION_UL_MS        60
#define CONTENTION_DL_MS        40
#define END_GUARD_MS            20

// Total superframe duration (conservative, includes all phase guards)
#define SUPERFRAME_MS \
    (BEACON_MS + MAX_NODES * SLOT_PAIR_MS + \
     CONTENTION_UL_MS + CONTENTION_DL_MS + END_GUARD_MS)  // ~1845 ms

// Node reads PZEM this many ms before its assigned TX time so data is ready.
// 110 ms = 100 ms PZEM read + 10 ms margin.
#define PZEM_PRE_READ_MS        110

// -----------------------------------------------------------------------------
// Application timeouts
// -----------------------------------------------------------------------------
#define NODE_TIMEOUT_MS         15000   // Node offline if silent for 15 s (~8 superframes)
#define PENDING_TIMEOUT_MS      15000   // Pending command clears after 15 s
#define RTC_CORRECTION_THRESHOLD_MS 2000  // Only correct schedule RTC if drift > 2 s

// -----------------------------------------------------------------------------
// Packet type IDs
// -----------------------------------------------------------------------------
#define PKT_TELEMETRY           0x01    // Node -> GW,   30 bytes
#define PKT_RELAY_MANUAL        0x02    // GW   -> Node,  3 bytes
#define PKT_RELAY_SCHEDULE      0x03    // GW   -> Node,  7 bytes
#define PKT_BEACON              0x04    // GW   -> All,   8 bytes
#define PKT_RELAY_CLEAR         0x05    // GW   -> Node,  2 bytes
#define PKT_THRESHOLD           0x06    // GW   -> Node,  4 bytes
#define PKT_NUDGE               0x07    // GW   -> Node,  2 bytes
#define PKT_JOIN_REQUEST        0xA0    // Node -> GW,    4 bytes
#define PKT_JOIN_ACK            0xA1    // GW   -> Node,  4 bytes

// -----------------------------------------------------------------------------
// Packet structures — all packed to avoid alignment padding
// -----------------------------------------------------------------------------
#pragma pack(push, 1)

/**
 * BeaconPacket (8 bytes) — broadcast on Ch 0 at superframe start.
 * Provides: time sync, superframe counter (hop seed), occupied slot bitmask.
 */
struct BeaconPacket {
    uint8_t  pktType;    // 0x04
    uint8_t  addr;       // 0xFF (broadcast)
    uint8_t  hour;       // 0–23
    uint8_t  minute;     // 0–59
    uint8_t  second;     // 0–59
    uint16_t sfCount;    // Superframe counter — hop sequence seed
    uint8_t  slotMask;   // Bit N = slot (N+1) is occupied (bit 0 = slot 1)
};
static_assert(sizeof(BeaconPacket) == 8, "BeaconPacket must be 8 bytes");

/**
 * TelemetryPacket (30 bytes) — node uplink every superframe.
 * Status byte packs relayState, relayMode, schedState, alarmState.
 *
 * statusByte layout:
 *   bit 0   : relayState  (0=OFF, 1=ON)
 *   bit 1   : relayMode   (0=MANUAL, 1=SCHEDULED)
 *   bits 3:2: schedState  (00=NONE, 01=WAITING, 10=ACTIVE)
 *   bit 4   : alarmState  (0=OK, 1=ALARM)
 *   bits 7:5: reserved
 */
struct TelemetryPacket {
    uint8_t  pktType;          // 0x01
    uint8_t  nodeId;           // 1–8 (assigned slot ID)
    uint16_t voltage;          // ÷10  -> volts     (2204 = 220.4 V)
    uint32_t current;          // ÷1000 -> amps     (2345 = 2.345 A)
    uint32_t power;            // ÷10  -> watts     (5163 = 516.3 W)
    uint32_t energy;           // Wh increment since last packet (node-side delta; handles rollover)
    uint16_t frequency;        // ÷10  -> Hz        (600  = 60.0 Hz)
    uint16_t powerFactor;      // ÷100 -> 0.00–1.00 (98   = 0.98)
    uint8_t  statusByte;       // packed bitfield (see above)
    uint8_t  schedSH;          // Schedule start hour
    uint8_t  schedSM;          // Schedule start minute
    uint8_t  schedEH;          // Schedule end hour
    uint8_t  schedEM;          // Schedule end minute
    uint16_t alarmThreshold;   // watts
    uint8_t  seqCounter;       // Rolling 0–255, for packet-loss detection
    uint8_t  beaconRSSI;       // int8 cast to uint8 — RSSI of last beacon (dBm)
    uint8_t  fwVersion;        // Firmware version
};
static_assert(sizeof(TelemetryPacket) == 30, "TelemetryPacket must be 30 bytes");

/** RelayCommandPacket (3 bytes) — immediate relay toggle */
struct RelayCommandPacket {
    uint8_t pktType;    // 0x02
    uint8_t nodeId;
    uint8_t relayState; // 0=OFF, 1=ON
};

/** RelaySchedulePacket (7 bytes) — daily recurring window */
struct RelaySchedulePacket {
    uint8_t pktType;    // 0x03
    uint8_t nodeId;
    uint8_t onState;    // Relay state INSIDE the window (1=ON is the normal case)
    uint8_t startH;
    uint8_t startM;
    uint8_t endH;
    uint8_t endM;
};

/** RelayClearPacket (2 bytes) — cancel active schedule, revert to manual */
struct RelayClearPacket {
    uint8_t pktType;    // 0x05
    uint8_t nodeId;
};

/** ThresholdPacket (4 bytes) — set PZEM over-power alarm */
struct ThresholdPacket {
    uint8_t  pktType;    // 0x06
    uint8_t  nodeId;
    uint16_t threshold;  // watts, little-endian (struct already packed)
};

/** NudgePacket (2 bytes) — blink LED for physical identification */
struct NudgePacket {
    uint8_t pktType;    // 0x07
    uint8_t nodeId;
};

/** JoinRequestPacket (4 bytes) — contention uplink from new node */
struct JoinRequestPacket {
    uint8_t  pktType;    // 0xA0
    uint8_t  uid_lo;
    uint8_t  uid_hi;
    uint8_t  fwVersion;
};

/** JoinAckPacket (4 bytes) — contention downlink from gateway */
struct JoinAckPacket {
    uint8_t pktType;    // 0xA1
    uint8_t uid_lo;
    uint8_t uid_hi;
    uint8_t slotId;     // 1–8
};

#pragma pack(pop)

// -----------------------------------------------------------------------------
// Status byte helpers
// -----------------------------------------------------------------------------
inline uint8_t encodeStatus(uint8_t relayState, uint8_t relayMode,
                              uint8_t schedState, uint8_t alarmState) {
    return ((alarmState & 0x01) << 4) |
           ((schedState & 0x03) << 2) |
           ((relayMode  & 0x01) << 1) |
            (relayState & 0x01);
}
inline uint8_t decodeRelayState(uint8_t s)  { return  s        & 0x01; }
inline uint8_t decodeRelayMode (uint8_t s)  { return (s >> 1)  & 0x01; }
inline uint8_t decodeSchedState(uint8_t s)  { return (s >> 2)  & 0x03; }
inline uint8_t decodeAlarmState(uint8_t s)  { return (s >> 4)  & 0x01; }

// -----------------------------------------------------------------------------
// Hop sequence
//
// channel = (sfCount × 7 + slotId) % N_CHANNELS
// Multiplier 7 is coprime to 8 -> over 8 superframes each slot visits all channels.
// Beacon and contention always use Ch 0, regardless.
// -----------------------------------------------------------------------------
inline uint8_t hopChannel(uint16_t sfCount, uint8_t slotId) {
    return (uint8_t)(((uint32_t)sfCount * 7u + slotId) % N_CHANNELS);
}

// -----------------------------------------------------------------------------
// CRC-16/CCITT (initial value 0xFFFF, polynomial 0x1021)
// Used to derive a 2-byte deviceUID from the 6-byte ESP32 MAC address.
// -----------------------------------------------------------------------------
inline uint16_t crc16ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

/** Compute 2-byte device UID from eFuse MAC. Called once at boot. */
inline uint16_t computeDeviceUID() {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return crc16ccitt(mac, 6);
}

// -----------------------------------------------------------------------------
// Utility: spin-wait until millis() reaches a target
// Safe for 32-bit millis() rollover as long as gap < 2^31 ms.
// -----------------------------------------------------------------------------
inline void waitUntilMs(uint32_t target) {
    while ((int32_t)(target - millis()) > 0) {
        taskYIELD();
    }
}
