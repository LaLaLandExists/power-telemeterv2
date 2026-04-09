/**
 * gateway_web.h
 * ESPAsyncWebServer + WebSocket interface for the power telemetry gateway.
 * All REST endpoints and WS push events are handled here.
 */
#pragma once
#include <Arduino.h>

/**
 * Initialize web server and register all REST + WebSocket handlers.
 * Node labels are loaded from FRAM via framLoadAll() in main.cpp before
 * this is called — no file I/O happens here.
 */
void webServerSetup();

/**
 * Called from the TDMA task (Core 1) after a telemetry packet is processed.
 * Serialises the node's current state and pushes a "telemetry" event to all
 * connected WebSocket clients.
 *
 * @param slotIdx  0-based slot index into g_nodes[]
 */
void webBroadcastTelemetry(uint8_t slotIdx);

/**
 * Push a "nodes" event to all WS clients (full node list).
 * Called every 5 s from the gateway loop (Core 0) and on WS connect.
 */
void webBroadcastAllNodes();
