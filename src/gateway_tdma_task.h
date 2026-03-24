/**
 * gateway_tdma_task.h
 * Public API of the gateway TDMA FreeRTOS task.
 * Runs on Core 1 at priority 2 so radio timing is never preempted by WiFi/HTTP.
 */
#pragma once
#include "gateway_state.h"

/**
 * Launch the TDMA task.  Call from setup() AFTER radio is initialised.
 * Pins the task to Core 1, priority 2.
 */
void gatewayTdmaTaskStart();

/**
 * Queue a downlink command for a node.
 * Thread-safe — may be called from the web server (Core 0).
 * The command is transmitted in the node's next DL slot (~1–2 superframes later).
 *
 * @param slotIdx  0-based index (slotId-1)
 * @param data     Pointer to raw command bytes
 * @param len      Byte count (2–8)
 * @return true if queued, false if slot inactive or another command pending
 */
bool tdmaQueueCommand(uint8_t slotIdx, const uint8_t* data, uint8_t len);

/**
 * Find the slot index (0-based) for a given 1-based nodeId.
 * Returns 0xFF if not found.
 */
uint8_t tdmaFindSlotByNodeId(uint8_t nodeId);
