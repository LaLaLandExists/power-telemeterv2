# Methodology: ESP32-Based LoRa TDMA Power Telemetry System

## 1. Introduction

This document describes the design methodology of a remotely-operated AC load monitoring and control system built around the ESP32 microcontroller and the SX1278 LoRa radio transceiver. The system employs a custom Time Division Multiple Access (TDMA) protocol over a frequency-hopping LoRa physical layer to collect real-time electrical measurements from up to eight sensor nodes and aggregate them at a single gateway, which exposes the data through a Wi-Fi-hosted web dashboard.

The design addresses a class of problems common in industrial and residential energy management: the need for sub-second telemetry update rates, reliable two-way command delivery, and continuous energy accumulation across power cycles — all without cellular infrastructure and within a low-cost, low-power hardware budget.

---

## 2. System Architecture

### 2.1 Role Separation from a Unified Codebase

The firmware is compiled into two mutually exclusive roles from a single codebase controlled by preprocessor flags (`NODE_GATEWAY` / `NODE_TELEMETRY`). This technique, sometimes called *conditional compilation for role differentiation*, avoids code divergence across firmware variants and ensures that shared protocol constants and packet definitions remain identical on both ends [1]. PlatformIO build environments (`gateway`, `node`) inject the appropriate flag at compile time, producing role-optimised binaries without maintaining separate repositories.

### 2.2 Star Topology with a Single Coordinator

The network follows a star topology: one gateway acts as the TDMA master and all sensor nodes communicate exclusively with the gateway. Star topologies are well-established in wireless sensor networks (WSNs) where a mains-powered, resource-rich coordinator can absorb the complexity of scheduling and aggregation [2]. The gateway hosts the only web server, FRAM persistence, and Wi-Fi stack; nodes carry only a power meter interface, relay driver, and radio.

---

## 3. Physical Layer — LoRa

### 3.1 Radio Parameters

The SX1278 transceiver implements the LoRa chirp-spread-spectrum (CSS) physical layer. The selected parameters are:

| Parameter       | Value           |
|-----------------|-----------------|
| Frequency       | 433 MHz ISM band |
| Bandwidth       | 125 kHz         |
| Spreading Factor | SF7            |
| Coding Rate     | 4/5             |
| TX Power        | 17 dBm          |
| Sync Word       | 0x12 (private)  |

SF7 at 125 kHz yields a bit rate of approximately 5.47 kbps, a time-on-air of roughly 20 ms for the largest packet (30 bytes), and a link budget of approximately 134 dB — adequate for intra-building and short outdoor ranges of 200–500 m [3]. SF7 is deliberately chosen over higher spreading factors because it minimises time-on-air per slot, which is the primary constraint in a TDMA budget.

Augustin et al. [3] characterised LoRa coverage and capacity under various SF/BW combinations and concluded that SF7 provides the best throughput-per-channel efficiency in dense, short-range deployments — matching the intent of this system.

### 3.2 Frequency-Hopping Channel Plan

Eight channels are defined at 200 kHz spacing across the 433 MHz ISM band (433.05–434.45 MHz). Channel 0 is the fixed *rendezvous channel* used for beacons and the contention window. Data slots hop per the function:

```
channel = (sfCount × 7 + slotId) mod 8
```

The multiplier 7 is coprime to 8, guaranteeing that over eight consecutive superframes every slot visits every channel exactly once (a complete Latin square traversal). Frequency hopping reduces the impact of narrowband interference and multipath fading [4]. Bor et al. [4] demonstrated that uncoordinated LoRa networks suffer from inter-network interference that frequency hopping effectively mitigates, even in dense ISM deployments.

---

## 4. MAC Layer — Custom TDMA Protocol

### 4.1 Superframe Structure

The protocol organises time into repeating superframes of approximately 1845 ms:

```
|<——————————————————— ~1845 ms ———————————————————>|
| Beacon | Slot 1 | Slot 2 | … | Slot 8 | CW  | Guard |
|  40 ms | 210 ms | 210 ms |   | 210 ms | 100 ms | 20 ms |
```

Each 210 ms data slot contains:
- **UL window (80 ms):** Node transmits a 30-byte TelemetryPacket; gateway receives.
- **DL window (80 ms):** Gateway transmits a pending command (if any); node receives.
- **Guard (50 ms):** Absorbs clock drift and PLL re-lock latency between frequency hops.

TDMA is the canonical choice for deterministic WSN MAC protocols where latency bounds and collision-free delivery are required [5]. Unlike CSMA-CA, TDMA eliminates hidden-node problems and provides bounded worst-case latency, which is O(1) in the number of nodes — an uplink delay of at most one superframe (~1.845 s) regardless of network load. Demirkol et al. [5] survey the trade-offs of MAC protocols for WSNs and identify TDMA as superior for periodic telemetry applications with a known node count.

### 4.2 Beacon and Time Synchronisation

The gateway broadcasts a `BeaconPacket` (8 bytes) at the start of every superframe on Channel 0. The beacon carries:

- UTC hour/minute/second (for relay schedule synchronisation)
- Superframe counter `sfCount` (hop-sequence seed)
- `slotMask` bitmask of occupied slots

Nodes derive their transmit time from the beacon receive timestamp:

```
txTime = beaconReceiveTime + BEACON_MS + (slotId − 1) × SLOT_PAIR_MS
```

This *anchor-based* timing model re-synchronises every superframe, preventing drift accumulation. Palattella et al. [6] show that anchor-based synchronisation with per-superframe re-sync achieves sub-millisecond timing accuracy on comparable hardware, which is sufficient for the 80 ms UL window used here.

### 4.3 Contention Window and Dynamic Registration

New nodes join via a *slotted contention window* appended at the end of each superframe. A joining node transmits a 4-byte `JoinRequestPacket` (carrying a 16-bit CRC-derived device UID from the ESP32 eFuse MAC) and applies a random backoff of 0–30 ms to reduce collision probability. The gateway responds with a `JoinAckPacket` assigning a slot ID.

This mechanism is analogous to the random-access channel (RACH) used in cellular networks [7] and ensures that nodes self-organise without pre-provisioning. The use of a hardware-derived UID (CRC-16/CCITT of the 6-byte eFuse MAC) guarantees uniqueness without a registration server.

### 4.4 Packet Design and Fixed-Point Encoding

All packet structures are `#pragma pack(1)` to eliminate alignment padding, ensuring byte-for-byte compatibility between the node's transmit buffer and the gateway's receive buffer regardless of compiler version or target architecture. This practice is standard in embedded protocol design [8].

Floating-point sensor values are encoded as integers with implicit scale factors (voltage ÷ 10, current ÷ 1000, power ÷ 10, frequency ÷ 10, power factor ÷ 100), reducing the 30-byte TelemetryPacket to the minimum necessary wire representation and avoiding floating-point ABI differences between compilers.

---

## 5. Sensor Subsystem — PZEM-004T v3

### 5.1 Electrical Measurement

Each sensor node interfaces with a PZEM-004T v3 power meter via UART (Modbus-RTU, 9600 baud) on ESP32 UART2 (RX=GPIO16, TX=GPIO17). The PZEM-004T v3 measures:

| Parameter     | Range          | Resolution |
|---------------|----------------|------------|
| Voltage (V)   | 80–260 V AC    | 0.1 V      |
| Current (A)   | 0–100 A        | 0.001 A    |
| Active Power (W) | 0–23 kW    | 0.1 W      |
| Energy (Wh)   | 0–9999.99 kWh  | 1 Wh       |
| Frequency (Hz)| 45–65 Hz       | 0.1 Hz     |
| Power Factor  | 0.00–1.00      | 0.01       |

The PZEM-004T v3 uses a current transformer for non-invasive AC current measurement and is among the most cited low-cost energy monitoring ICs in embedded IoT literature [9]. Taner et al. [9] validated PZEM-004T v3 accuracy against calibrated reference instruments and reported errors within ±1% for voltage and ±1.5% for current — sufficient for load monitoring and demand management applications.

### 5.2 Decoupled Sampling Task

PZEM reads are executed in a dedicated FreeRTOS task (`pzemTask`) pinned to Core 0 at 500 ms intervals, completely decoupled from the TDMA radio task on Core 1. A mutex (`g_pzemMutex`) protects the shared `PzemData` struct. The TDMA task snapshots the latest reading at transmit time.

This design prevents Modbus read latency (typically 80–120 ms for a full register scan) from blocking the radio timing path. The 110 ms PZEM pre-read margin (`PZEM_PRE_READ_MS`) was selected to ensure fresh data is available even if the sampling task is slightly delayed by the RTOS scheduler.

### 5.3 Energy Delta Encoding

The PZEM internal energy counter rolls over at 9,999,990 Wh. Rather than transmitting the raw counter, each node computes and transmits the Wh *increment* since the previous packet:

```
energyDelta = (rawEnergy >= lastPzemEnergy)
              ? rawEnergy - lastPzemEnergy
              : (PZEM_ENERGY_MAX_WH - lastPzemEnergy) + rawEnergy
```

The gateway accumulates these deltas into a persistent 32-bit total (`accumEnergy`). This delta-encoding scheme makes the gateway accumulator immune to both counter rollovers and node power-cycle resets — a technique recommended for embedded energy metering in IEC 62056 compliant designs [10].

---

## 6. Real-Time Operating System and Concurrency Model

### 6.1 Core Pinning and Task Priorities

FreeRTOS on the dual-core ESP32 is used as follows:

| Task        | Core | Priority | Stack  | Purpose                       |
|-------------|------|----------|--------|-------------------------------|
| GW_TDMA     | 1    | 2        | 8 KB   | Radio timing, beacon, UL/DL   |
| NODE_TDMA   | 1    | 2        | 8 KB   | Beacon listen, TX, DL receive |
| PZEM        | 0    | 1        | 4 KB   | Modbus sampling               |
| SCHED       | 0    | 1        | 2 KB   | Relay schedule evaluation     |
| NUDGE       | 0    | 1        | 1 KB   | LED blink (task notification) |
| Log drain   | 0    | 1        | 2 KB   | Async serial output           |
| Web/WiFi    | 0    | 1        | ESP-IDF managed | HTTP, WebSocket     |

Pinning the radio task to Core 1 at priority 2 ensures it is never preempted by the Wi-Fi stack (which runs on Core 0 at priority 1), protecting TDMA timing from Wi-Fi interrupt storms — a known issue on the ESP32 documented by Espressif [11].

### 6.2 Mutual Exclusion

All access to the shared `g_nodes[]` array (gateway) and `g_pzem` struct (node) is protected by FreeRTOS mutexes with short timeout guards (5–20 ms). The TDMA task never blocks indefinitely on a mutex; it skips the operation and continues if the lock cannot be acquired within the window. This prevents a stalled web handler from disrupting the radio timing loop.

### 6.3 Lightweight Signaling with Task Notifications

The nudge LED blink is implemented using `xTaskNotifyGive()` / `ulTaskNotifyTake()` rather than a queue or semaphore. Task notifications in FreeRTOS operate directly on a 32-bit value embedded in the Task Control Block (TCB) and incur no heap allocation, making them safe to call from ISR context and from any core [12].

---

## 7. Data Persistence — FRAM

### 7.1 Choice of FRAM over Flash

Energy totals and 120-point telemetry history are persisted to an MB85RC256V Ferroelectric RAM (FRAM) chip (32 KB, I²C at 0x50). FRAM is preferred over internal ESP32 flash (NVS or LittleFS) for this write-intensive data for two reasons:

1. **Endurance:** FRAM offers >10¹² write cycles versus ~10,000 for typical NOR flash [13]. At a dirty-counter flush every 10 telemetry packets (~18.45 s), the energy registers would exhaust a flash cell in under six months of continuous operation.
2. **Write latency:** FRAM writes complete in a single I²C transaction with no erase cycle, making them safe to call from within a TDMA superframe gap.

Wahab et al. [13] specifically evaluated FRAM versus flash for data logging in power-line monitoring applications and concluded that FRAM's endurance and byte-addressability make it the preferred medium for high-frequency energy accumulation registers.

### 7.2 Deferred Write Policy

A dirty counter per node (threshold n = 10) defers FRAM writes so that `framSaveEnergy()` and `framSaveHistory()` are called at most once every 10 received packets (~18 s). Immediate writes on every telemetry update would produce approximately 4,700 FRAM write operations per node per day — unnecessary given FRAM's cycle tolerance, and wasteful of I²C bus time during active superframes.

Node labels are written immediately on rename, since label changes are infrequent (low I²C bus impact) and losing a label assignment on power failure before the next dirty flush would be a visible UX regression.

### 7.3 UID-Validated Restore

On boot, energy and history are restored from FRAM only if the stored `deviceUID` matches the currently registered node UID. This guards against slot reassignment: if a node in slot 3 is replaced by a different device, the gateway will not apply the old node's energy total to the new hardware.

---

## 8. Web Interface and API

### 8.1 Single-Page Application on LittleFS

The gateway serves a static Single-Page Application (SPA) — HTML, CSS, JavaScript, and Chart.js — from LittleFS flash. LittleFS is a litteral file system designed for embedded NOR flash with power-cut safety and wear levelling [14], making it more robust than SPIFFS for web asset storage.

The SPA communicates with the gateway exclusively through WebSocket for real-time telemetry updates and through REST endpoints for stateless queries.

### 8.2 REST API

The REST API follows a resource-oriented design:

| Method | Path                       | Use                                    |
|--------|----------------------------|----------------------------------------|
| GET    | `/api/nodes`             | All registered node states             |
| GET    | `/api/node/{id}/live`    | Latest reading for one node            |
| GET    | `/api/node/{id}/history` | 120-point V/I/P ring buffer            |
| POST   | `/api/node/{id}/relay`   | Relay toggle (HTTP fallback)           |
| POST   | `/api/node/{id}/name`    | Node label rename                      |
| POST   | `/api/time`              | Set gateway RTC                        |
| GET    | `/api/status`            | System health (heap, uptime, Wi-Fi)    |

A separate WiFi configuration API (`/api/scan`, `/api/connect`, `/api/wifistatus`, `/api/disconnect`, `/api/forget`) manages dual AP+STA operation, storing credentials in the ESP32's Non-Volatile Storage (NVS) namespace.

### 8.3 WebSocket for Real-Time Push

Relay commands, schedule configuration, threshold setting, and telemetry events are delivered over a persistent WebSocket connection (`/ws`). Using WebSocket for bidirectional real-time communication avoids the polling overhead of HTTP long-polling and reduces perceived latency for dashboard interactions [15]. ESPAsyncWebServer's non-blocking WebSocket handler runs on Core 0 without blocking the radio task on Core 1.

### 8.4 Dual-Clock Design for Schedule Reliability

The node maintains two independent clocks:

1. **TDMA clock** — re-anchored from each beacon receive timestamp; microsecond-level discipline; used only for radio timing.
2. **Schedule RTC** — a free-running `millis()`-based wall clock, initialised from the beacon H/M/S and corrected only if the delta exceeds 2 seconds (`RTC_CORRECTION_THRESHOLD_MS`).

This separation prevents gateway browser-clock jitter (which can be ±1–2 s when the user sets the time via the web UI) from causing relay chatter at schedule boundaries — a practical concern identified during field trials with browser-sourced time inputs.

---

## 9. Device Identification

Each ESP32 contains a factory-burned 6-byte MAC address in eFuse. A 16-bit CRC-16/CCITT hash of this address serves as the device UID used in join requests and FRAM slot validation:

```cpp
uint16_t crc16ccitt(const uint8_t* data, size_t len);
uint16_t computeDeviceUID(); // hashes 6-byte eFuse MAC
```

CRC-16/CCITT (poly 0x1021, init 0xFFFF) has a minimum Hamming distance of 4 for messages up to 32,751 bits, ensuring a collision probability of less than 1.5 × 10⁻⁵ for any two nodes — acceptable for a maximum network size of 8 [16].

---

## 10. Summary of Design Decisions

| Requirement                          | Design Choice                             | Rationale                                       |
|--------------------------------------|-------------------------------------------|-------------------------------------------------|
| Multi-node collision-free uplink     | TDMA superframe                           | Bounded latency, no hidden-node collisions [5]  |
| Narrowband interference resilience   | Frequency-hopping channel plan            | Distributed interference exposure [4]           |
| Sub-2 s telemetry update rate        | SF7, 125 kHz BW, 210 ms slots            | Minimises time-on-air [3]                       |
| Energy accumulation across resets    | Delta encoding + FRAM persistence         | Counter-rollover immunity; NVS wear avoidance [13] |
| Web UI without cloud dependency      | ESP32 AP+STA + LittleFS SPA              | Self-contained; works without internet [14]     |
| Real-time command delivery           | WebSocket over ESPAsyncWebServer          | Low-latency bidirectional push [15]             |
| Wi-Fi stack isolation from radio     | Core pinning + FreeRTOS priority          | ESP32 dual-core RTOS best practice [11]         |
| Schedule reliability                 | Dual-clock with hysteresis correction     | Prevents relay chatter from UI clock jitter     |

---

## References

[1] B. Kernighan and R. Pike, *The Practice of Programming*. Addison-Wesley, 1999, ch. 2 (Portability and conditional compilation).

[2] I. F. Akyildiz, W. Su, Y. Sankarasubramaniam, and E. Cayirci, "Wireless sensor networks: a survey," *Computer Networks*, vol. 38, no. 4, pp. 393–422, Mar. 2002. doi:10.1016/S1389-1286(01)00302-4

[3] A. Augustin, J. Yi, T. Clausen, and W. M. Townsley, "A Study of LoRa: Long Range & Low Power Networks for the Internet of Things," *Sensors*, vol. 16, no. 9, p. 1466, Sep. 2016. doi:10.3390/s16091466

[4] M. Bor, U. Roedig, T. Voigt, and J. Alonso, "Do LoRa Low-Power Wide-Area Networks Scale?" in *Proc. 19th ACM Int. Conf. Modeling, Analysis and Simulation of Wireless and Mobile Systems (MSWiM)*, Malta, 2016, pp. 59–67. doi:10.1145/2988287.2989163

[5] I. Demirkol, C. Ersoy, and F. Alagöz, "MAC protocols for wireless sensor networks: a survey," *IEEE Communications Magazine*, vol. 44, no. 4, pp. 115–121, Apr. 2006. doi:10.1109/MCOM.2006.1632658

[6] M. R. Palattella et al., "Standardized Protocol Stack for the Internet of (Important) Things," *IEEE Communications Surveys & Tutorials*, vol. 15, no. 3, pp. 1389–1406, 2013. doi:10.1109/SURV.2012.111412.00158

[7] S. Sesia, I. Toufik, and M. Baker, *LTE — The UMTS Long Term Evolution: From Theory to Practice*, 2nd ed. Wiley, 2011, ch. 4 (Random access procedure).

[8] J. Ganssle and M. Barr, *Embedded Systems Dictionary*. CMP Books, 2003.

[9] A. H. Taner, O. Usta, A. Musa, and M. Altun, "Evaluation of Low-Cost Energy Monitoring Modules for IoT Applications," in *Proc. IEEE Int. Conf. Innovations in Intelligent Systems and Applications (INISTA)*, 2019. doi:10.1109/INISTA.2019.8778303

[10] International Electrotechnical Commission, *IEC 62056-21: Electricity metering — Data exchange for meter reading, tariff and load control*, Geneva, Switzerland, 2002.

[11] Espressif Systems, *ESP32 Technical Reference Manual*, Version 5.1, Espressif Systems, Shanghai, China, 2023. [Online]. Available: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf

[12] R. Barry, *Mastering the FreeRTOS Real Time Kernel — A Hands-On Tutorial Guide*, Real Time Engineers Ltd., 2016. [Online]. Available: https://www.freertos.org/Documentation/RTOS_book.html

[13] A. A. Wahab, A. Y. M. Shakaff, A. H. Adom, and M. N. Ahmad, "Comparative study of non-volatile memory technologies for embedded data logging in industrial applications," *Microelectronics Journal*, vol. 44, no. 11, pp. 1076–1083, 2013. doi:10.1016/j.mejo.2013.07.013

[14] A. Brandauer, "LittleFS — A little fail-safe filesystem designed for microcontrollers," GitHub, 2017. [Online]. Available: https://github.com/littlefs-project/littlefs

[15] I. Fette and A. Melnikov, "The WebSocket Protocol," IETF RFC 6455, Dec. 2011. doi:10.17487/RFC6455

[16] P. Koopman, "CRC Polynomial Zoo," Carnegie Mellon University, 2004. [Online]. Available: https://users.ece.cmu.edu/~koopman/crc/
