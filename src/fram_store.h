/**
 * fram_store.h
 *
 * Gateway-side persistence for per-node accumEnergy, history[], and label via
 * MB85RC256V FRAM (32 KB, I²C) using the robtillaart/FRAM_I2C library.
 *
 * FRAM memory layout
 * ------------------
 *   0x0000  Magic     (uint32_t) = 0xFACECAFE
 *   0x0004  Version   (uint8_t)  = 2
 *   0x0005  Padding   (11 bytes) → align to 0x0010
 *   0x0010  Node[0] block (1968 bytes):
 *             +0   deviceUID   (uint16_t) — validation key for energy/history
 *             +2   _pad        (uint16_t)
 *             +4   accumEnergy (uint32_t)
 *             +8   histHead    (int32_t)
 *             +12  histCount   (int32_t)
 *             +16  label       (char[30]) — human-readable node name
 *             +46  _pad2       (uint16_t) — align history to 48
 *             +48  history[120] (1920 bytes, 120 × 16 bytes each)
 *   0x07C0  Node[1] block …
 *   …
 *   Total: 16 + 8×1968 = 15 760 bytes  (<32 768 bytes)
 *
 * Write policy
 * ------------
 *   framSaveEnergy / framSaveHistory: deferred via dirty-counter (n=10),
 *   managed in gateway_tdma_task.cpp.
 *   framSaveLabel: written immediately on every rename (low frequency).
 *
 * Version history
 * ---------------
 *   v1 — initial layout (no label field, block size 1936)
 *   v2 — added label[30] at +16, history shifted to +48, block size 1968
 */
#pragma once
#include <stdint.h>

/**
 * Initialise I²C bus and verify FRAM is reachable.
 * Writes magic + version on the very first boot (blank chip).
 * On version mismatch (layout upgrade), stamps the new version and skips
 * restore so stale data is never applied to the wrong offsets.
 *
 * @param sda   SDA GPIO (e.g. 32)
 * @param scl   SCL GPIO (e.g. 33)
 * @param addr  I²C address (default 0x50)
 * @return true  if FRAM responded and is ready
 *         false if device not found (persistence silently disabled)
 */
bool framInit(uint8_t sda, uint8_t scl, uint8_t addr = 0x50);

/**
 * Restore per-node state from FRAM. Call once at boot, before
 * gatewayTdmaTaskStart() and webServerSetup().
 *   - Labels are restored unconditionally for all slots.
 *   - accumEnergy and history[] are restored only when the stored deviceUID
 *     matches the active node (guards against slot reassignment after reboot).
 */
void framLoadAll();

/**
 * Persist accumEnergy (and deviceUID as validation key) for one node slot.
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveEnergy(uint8_t nodeIdx);

/**
 * Persist deviceUID, histHead, histCount, and the full history[] ring buffer
 * for one node slot.
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveHistory(uint8_t nodeIdx);

/**
 * Persist the human-readable label for one node slot.
 * Written immediately on rename (not deferred).
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveLabel(uint8_t nodeIdx);
