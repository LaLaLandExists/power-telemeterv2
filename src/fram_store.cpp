/**
 * fram_store.cpp
 *
 * FRAM persistence for gateway-side node state.
 * See fram_store.h for memory layout documentation.
 */
#ifdef NODE_GATEWAY

#include "fram_store.h"
#include <Wire.h>
#include <FRAM.h>
#include "gateway_state.h"

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr uint32_t FRAM_MAGIC        = 0xDEADF00DUL;
static constexpr uint8_t  FRAM_VERSION      = 2;
static constexpr uint16_t FRAM_HEADER_BASE  = 0x0000;   // magic(4) + ver(1) + pad(11)
static constexpr uint16_t FRAM_NODES_BASE   = 0x0010;   // first node block

// Per-node block offsets (relative to node block start)
static constexpr uint16_t OFF_UID           = 0;    // uint16_t
static constexpr uint16_t OFF_PAD           = 2;    // uint16_t (alignment pad)
static constexpr uint16_t OFF_ENERGY        = 4;    // uint32_t
static constexpr uint16_t OFF_HIST_HEAD     = 8;    // int32_t
static constexpr uint16_t OFF_HIST_COUNT    = 12;   // int32_t
static constexpr uint16_t OFF_LABEL         = 16;   // char[30]
static constexpr uint16_t OFF_PAD2          = 46;   // uint16_t (align history to 48)
static constexpr uint16_t OFF_HISTORY       = 48;   // HistoryPoint[120]

static constexpr uint16_t LABEL_BYTES       = 30;   // sizeof(NodeState::label)
static constexpr uint16_t HISTORY_BYTES     = HISTORY_MAX_POINTS * sizeof(HistoryPoint);
static constexpr uint16_t NODE_BLOCK_SIZE   = OFF_HISTORY + HISTORY_BYTES;  // 1968

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static FRAM s_fram(&Wire);
static bool s_framOk = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline uint16_t nodeBase(uint8_t idx)
{
  return FRAM_NODES_BASE + (uint16_t)idx * NODE_BLOCK_SIZE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool framInit(uint8_t sda, uint8_t scl, uint8_t addr)
{
  Wire.begin((int)sda, (int)scl);

  int rc = s_fram.begin(addr);
  if (rc != FRAM_OK || !s_fram.isConnected()) {
    Serial.printf("[FRAM] begin() rc=%d  isConnected=%d\n",
                  rc, (int)s_fram.isConnected());
    return false;
  }

  uint32_t magic = s_fram.read32(FRAM_HEADER_BASE);
  if (magic != FRAM_MAGIC) {
  // Blank or corrupted chip — stamp header
  Serial.println("[FRAM] Blank chip detected - writing header");
  s_fram.write32(FRAM_HEADER_BASE, FRAM_MAGIC);
  s_fram.write8(FRAM_HEADER_BASE + 4, FRAM_VERSION);
  } else {
    uint8_t ver = s_fram.read8(FRAM_HEADER_BASE + 4);
    if (ver != FRAM_VERSION) {
      // Layout changed — stamp new version, skip restore to avoid
      // applying old data to wrong offsets.
      Serial.printf("[FRAM] Layout v%u -> v%u: skipping restore, "
                    "data will repopulate\n", ver, FRAM_VERSION);
      s_fram.write8(FRAM_HEADER_BASE + 4, FRAM_VERSION);
    // Return true so persistence is active for future writes
     s_framOk = true;
     return true;
    }
  }

  s_framOk = true;
  return true;
}

void framLoadAll()
{
  if (!s_framOk) return;

  for (uint8_t i = 0; i < MAX_NODES; i++) {
    uint16_t base = nodeBase(i);
    NodeState& ns = g_nodes[i];

    // --- Labels: restore unconditionally (user-assigned, slot-bound) ---
    char label[LABEL_BYTES];
    s_fram.read(base + OFF_LABEL, reinterpret_cast<uint8_t*>(label), LABEL_BYTES);
    label[LABEL_BYTES - 1] = '\0';  // safety null-terminate
    if (label[0] != '\0') {
      strlcpy(ns.label, label, sizeof(ns.label));
      Serial.printf("[FRAM] Slot %u: restored label \"%s\"\n", i + 1, ns.label);
    }

    // --- Energy + history: UID-validated ---
    uint16_t framUID = s_fram.read16(base + OFF_UID);
    if (framUID == 0x0000 || framUID == 0xFFFF) continue;
    if (!ns.active || ns.deviceUID != framUID) continue;

    ns.accumEnergy = s_fram.read32(base + OFF_ENERGY);
    ns.histHead    = (int)s_fram.read32(base + OFF_HIST_HEAD);
    ns.histCount   = (int)s_fram.read32(base + OFF_HIST_COUNT);
    s_fram.read(base + OFF_HISTORY,
                reinterpret_cast<uint8_t*>(ns.history),
                HISTORY_BYTES);

    Serial.printf("[FRAM] Slot %u (UID=0x%04X): restored energy=%lu Wh, "
                  "histCount=%d\n",
                  i + 1, framUID,
                  (unsigned long)ns.accumEnergy, ns.histCount);
  }
}

void framSaveEnergy(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  uint16_t base = nodeBase(nodeIdx);

  s_fram.write16(base + OFF_UID,    ns.deviceUID);
  s_fram.write32(base + OFF_ENERGY, ns.accumEnergy);
}

void framSaveHistory(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  uint16_t base = nodeBase(nodeIdx);

  s_fram.write16(base + OFF_UID,        ns.deviceUID);
  s_fram.write32(base + OFF_HIST_HEAD,  (uint32_t)ns.histHead);
  s_fram.write32(base + OFF_HIST_COUNT, (uint32_t)ns.histCount);
  s_fram.write(base + OFF_HISTORY,
               reinterpret_cast<uint8_t*>(
               const_cast<HistoryPoint*>(ns.history)),
               HISTORY_BYTES);
}

void framSaveLabel(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  uint16_t base = nodeBase(nodeIdx);

  s_fram.write(base + OFF_LABEL,
               reinterpret_cast<uint8_t*>(const_cast<char*>(ns.label)),
               LABEL_BYTES);

  Serial.printf("[FRAM] Slot %u: saved label \"%s\"\n", nodeIdx + 1, ns.label);
}

#endif // NODE_GATEWAY
