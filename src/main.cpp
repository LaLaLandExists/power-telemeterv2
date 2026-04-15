/**
 * main.cpp
 *
 * Unified entry point for the Power Telemetry firmware.
 * Compiles as either a Gateway or a Sensor Node depending on the
 * build flag defined in platformio.ini.
 *
 * -- Usage ---------------------------------------------------------------------
 *
 *   In platformio.ini, set exactly ONE of:
 *
 *     build_flags = -D NODE_GATEWAY
 *     build_flags = -D NODE_TELEMETRY
 *
 *   If BOTH are defined, the build resolves to NODE_GATEWAY (with a warning).
 *   If NEITHER is defined, the build fails with a descriptive #error.
 *
 * -- platformio.ini example ----------------------------------------------------
 *
 *   [env:gateway]
 *   platform  = espressif32
 *   board     = esp32dev
 *   framework = arduino
 *   build_flags =
 *       -D NODE_GATEWAY
 *       -D ARDUINO_USB_CDC_ON_BOOT=0
 *   board_build.filesystem = littlefs
 *   lib_deps =
 *       jgromes/RadioLib @ ^6.0.0
 *       me-no-dev/ESPAsyncWebServer
 *       me-no-dev/AsyncTCP
 *       bblanchon/ArduinoJson @ ^7.0.0
 *
 *   [env:node]
 *   platform  = espressif32
 *   board     = esp32dev
 *   framework = arduino
 *   build_flags =
 *       -D NODE_TELEMETRY
 *       -D ARDUINO_USB_CDC_ON_BOOT=0
 *   lib_deps =
 *       jgromes/RadioLib @ ^6.0.0
 *       olehs/PZEM004Tv30
 *
 * -- Hardware ------------------------------------------------------------------
 *
 *   SX1278 (VSPI - identical on both roles):
 *     NSS  -> GPIO5    SCK  -> GPIO22
 *     MISO -> GPIO19   MOSI -> GPIO18
 *     DIO0 -> GPIO26   RST  -> GPIO14
 *     DIO1 -> GPIO25 (tie to GND if unused)
 *
 *   Gateway only:
 *     WiFi + LittleFS (index.html + wifi_config.html in data/ folder)
 *     Dashboard: http://telemeter.local  or  http://192.168.4.1
 *
 *   Node only:
 *     PZEM-004T v3  -> Serial2  RX2=GPIO16, TX2=GPIO17
 *     Relay signal  -> GPIO4  (active HIGH)
 *     LED           -> GPIO2  (built-in, nudge blink)
 */

// --- Macro resolution ---------------------------------------------------------
#if defined(NODE_GATEWAY) && defined(NODE_TELEMETRY)
  // Both defined - resolve to gateway with a compile-time warning.
  // #warning is a GCC extension supported by the ESP32 toolchain.
  #warning "Both NODE_GATEWAY and NODE_TELEMETRY are defined - building as NODE_GATEWAY."
  #undef NODE_TELEMETRY
#elif !defined(NODE_GATEWAY) && !defined(NODE_TELEMETRY)
  #error "No build role defined. Set exactly one of: -D NODE_GATEWAY  or  -D NODE_TELEMETRY"
#endif
// After resolution: exactly one of NODE_GATEWAY or NODE_TELEMETRY is defined.

// --- Common includes (both roles) --------------------------------------------
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "lora_tdma_protocol.h"
#include "log_async.h"

// --- Role-specific includes ---------------------------------------------------
#ifdef NODE_GATEWAY
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include <LittleFS.h>
  #include <Wire.h>
  #include "gateway_state.h"
  #include "gateway_tdma_task.h"
  #include "gateway_web.h"
  #include "gateway_wifi_config.h"
  #include "fram_store.h"
#endif

#ifdef NODE_TELEMETRY
  #include <PZEM004Tv30.h>
  #include "node_tdma_task.h"
#endif

// --- Pin definitions (shared) ------------------------------------------------
#define LORA_PIN_NSS    5
#define LORA_PIN_DIO0   26
#define LORA_PIN_RST    14
#define LORA_PIN_DIO1   25   // Tie to GND if unused
#define LORA_PIN_SCK    22   // Remapped from GPIO18 (routing)
#define LORA_PIN_MISO   19
#define LORA_PIN_MOSI   18

// --- Role-specific pin definitions -------------------------------------------
#ifdef NODE_TELEMETRY
  #define PZEM_RX_PIN  16   // ESP32 RX2 <- PZEM TX
  #define PZEM_TX_PIN  17   // ESP32 TX2 -> PZEM RX
  #define RELAY_PIN_    4   // Relay signal (active HIGH)
  #define LED_PIN_      2   // Built-in LED

  // Exported - referenced by node_tdma_task.cpp
  uint8_t RELAY_PIN = RELAY_PIN_;
  uint8_t LED_PIN   = LED_PIN_;
#endif

// --- Hardware instances -------------------------------------------------------
// Radio instance - extern-referenced by both gateway_tdma_task.cpp and node_tdma_task.cpp
SX1278 radio = new Module(LORA_PIN_NSS, LORA_PIN_DIO0, LORA_PIN_RST, LORA_PIN_DIO1);

#ifdef NODE_TELEMETRY
  // PZEM-004T v3 on Serial2 - extern-referenced by node_tdma_task.cpp
  PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
#endif

// =============================================================================
// GATEWAY
// =============================================================================
#ifdef NODE_GATEWAY

void setup() {
  Serial.begin(115200);
  logDrainTaskStart();
  delay(500);
  Serial.println("\n===== Power Telemetry Gateway =====");

  // -- LittleFS --------------------------------------------------------------
  // Mounts the web SPA (index.html, app.js, etc.) served by the web task.
  // LittleFS.begin(true) formats on mount failure.
  // NVS (WiFi credentials) is unaffected by format.
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount FAILED - dashboard SPA not available");
  } else {
    Serial.println("[FS] LittleFS OK");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) {
      Serial.printf("  /%-30s  %lu B\n", f.name(), (unsigned long)f.size());
      f = root.openNextFile();
    }
  }

  // -- FRAM (MB85RC256V, SDA=32, SCL=33, addr=0x50) -------------------------
  // framLoadAll() is called before the TDMA task starts so no mutex is needed.
  // framInit() internally calls Wire.begin(32, 33).
  if (!framInit(32, 33, 0x50)) {
    Serial.println("[FRAM] Init FAILED - energy/history persistence disabled");
  } else {
    Serial.println("[FRAM] OK");
    framLoadAll();
  }

  // -- WiFi - non-blocking state machine ------------------------------------
  // AP starts immediately (last-resort access route). STA attempted in
  // parallel if credentials are saved in NVS.
  //   AP on  -> while STA unavailable (no creds, connecting, or dropped)
  //   AP off -> only while STA actively connected
  // wifiConfigLoop() drives all transitions from loop().
  wifiConfigBegin();

  // -- SX1278 radio ----------------------------------------------------------
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  Serial.print("[LORA] Initialising SX1278 ... ");
  int16_t loraState = radio.begin(
    LORA_CHANNELS[0],   // Ch 0 - beacon channel
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_TX_POWER,
    LORA_PREAMBLE_LEN
  );
  if (loraState == RADIOLIB_ERR_NONE) {
    Serial.println("OK");
  } else {
    Serial.printf("FAILED (err=%d) - check wiring!\n", loraState);
  }
  radio.setCRC(true);

  // -- Web server ------------------------------------------------------------
  webServerSetup();

  // -- TDMA task (Core 1, priority 2) ---------------------------------------
  gatewayTdmaTaskStart();

  Serial.println("[GW] Setup complete.\n");
}

void loop() {
  // Drive the WiFi state machine and DNS server.
  wifiConfigLoop();

  // Periodic 5 s "nodes" WebSocket push + ws.cleanupClients().
  webBroadcastAllNodes();

  delay(10);
}

#endif // NODE_GATEWAY

// =============================================================================
// SENSOR NODE
// =============================================================================
#ifdef NODE_TELEMETRY

void setup() {
  Serial.begin(115200);
  logDrainTaskStart();
  delay(500);
  Serial.println("\n===== Power Telemetry Node =====");

  // -- GPIO init -------------------------------------------------------------
  pinMode(RELAY_PIN_, OUTPUT);
  digitalWrite(RELAY_PIN_, LOW);   // Relay OFF at boot
  pinMode(LED_PIN_,   OUTPUT);
  digitalWrite(LED_PIN_,   LOW);

  // 3 fast blinks - boot indicator
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN_, i & 1);
    delay(120);
  }
  digitalWrite(LED_PIN_, LOW);

  // -- SX1278 radio ----------------------------------------------------------
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  Serial.print("[LORA] Initialising SX1278 ... ");
  int16_t loraState = radio.begin(
    LORA_CHANNELS[0],   // Ch 0 - listen for beacon
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_TX_POWER,
    LORA_PREAMBLE_LEN
  );
  if (loraState == RADIOLIB_ERR_NONE) {
    Serial.println("OK");
  } else {
    Serial.printf("FAILED (err=%d)\n", loraState);
  }
  radio.setCRC(true);

  // -- PZEM sanity check -----------------------------------------------------
  Serial.print("[PZEM] Checking connection ... ");
  delay(200);
  float testV = pzem.voltage();
  if (!isnan(testV)) {
    Serial.printf("OK (%.1f V)\n", testV);
  } else {
    Serial.println("No response - check wiring (TX/RX swapped?)");
    // pzemTask will keep retrying - non-fatal
  }

  // -- Launch FreeRTOS tasks -------------------------------------------------
  nodeTdmaTaskStart();

  Serial.println("[NODE] Setup complete - waiting for beacon.\n");
}

void loop() {
  // All work is in FreeRTOS tasks on Core 1 (TDMA) and Core 0 (PZEM, sched, nudge).
  // loop() runs as the idle context - just yield.
  vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif // NODE_TELEMETRY
