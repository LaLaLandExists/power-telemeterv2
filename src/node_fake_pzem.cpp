#ifdef PZEM_FAKE

#include "node_fake_pzem.h"
#include "node_tdma_task.h"
#include <Arduino.h>
#include <math.h>
#include <stdlib.h>

// --- Load profile (refrigerator-like, matches simulation.js Node 1) ----------
#define FAKE_NOMINAL_W      120.0f
#define FAKE_IDLE_W           5.0f
#define FAKE_CYCLE_MS      600000UL   // 10-minute ON/OFF cycle
#define FAKE_ON_FRAC          0.60f
#define FAKE_VOLT_NOM       220.0f
#define FAKE_FREQ_NOM        60.0f
#define FAKE_PF_NOM           0.85f
#define FAKE_PF_NOISE         0.04f

// Uniform random noise in [-amp, +amp], matching simulation.js noise(amp)
static inline float noise(float amp) {
  return ((float)(rand() % 1001) / 500.0f - 1.0f) * amp;
}

void fakePzemTask(void* /*params*/) {
  Serial.println("[PZEM-FAKE] Task started — net-test mode, no hardware required");

  uint32_t accumEnergyWh = 0;
  uint32_t lastMs        = millis();

  while (true) {
    uint32_t now   = millis();
    uint32_t dt_ms = now - lastMs;
    lastMs = now;

    // Determine cycle phase from absolute time so phase survives task restarts
    uint32_t phase_ms = now % FAKE_CYCLE_MS;
    bool     inOnPhase = phase_ms < (uint32_t)(FAKE_CYCLE_MS * FAKE_ON_FRAC);

    float v, i, p, pf, f;

    if (g_relayState == 0) {
      // Relay OFF: line voltage present, no current
      v  = FAKE_VOLT_NOM + noise(0.6f);
      i  = 0.0f;
      p  = 0.0f;
      pf = 0.0f;
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    } else if (inOnPhase) {
      // ON phase: sinusoidal power ripple + noise, voltage sag under load
      float t = (float)now / 1000.0f;
      p  = FAKE_NOMINAL_W
         + FAKE_NOMINAL_W * 0.12f * sinf(t * 0.8f)
         + noise(FAKE_NOMINAL_W * 0.05f);
      if (p < 0.0f) p = 0.0f;
      pf = fminf(1.0f, fmaxf(0.5f, FAKE_PF_NOM + noise(FAKE_PF_NOISE)));
      // First pass: estimate I, compute voltage sag, then recalculate I
      float i_est = p / (FAKE_VOLT_NOM * pf + 0.001f);
      v  = fmaxf(195.0f, FAKE_VOLT_NOM - i_est * 0.3f + noise(0.4f));
      i  = p / (v * pf + 0.001f);
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    } else {
      // Idle phase: standby draw, near-nominal voltage
      p  = FAKE_IDLE_W + noise(FAKE_IDLE_W * 0.1f + 0.5f);
      if (p < 0.0f) p = 0.0f;
      pf = fminf(1.0f, fmaxf(0.5f, FAKE_PF_NOM + noise(FAKE_PF_NOISE)));
      v  = FAKE_VOLT_NOM + noise(0.6f);
      i  = p / (v * pf + 0.001f);
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    }

    // Accumulate energy (Wh): P[W] × dt[ms] / 3 600 000
    accumEnergyWh += (uint32_t)(p * (float)dt_ms / 3600000.0f + 0.5f);

    // Write to shared g_pzem and consume any pending threshold command
    bool     doThreshold = false;
    uint16_t threshWatts = 0;

    if (xSemaphoreTake(g_pzemMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      g_pzem.voltage     = v;
      g_pzem.current     = i;
      g_pzem.power       = p;
      g_pzem.energy      = accumEnergyWh;
      g_pzem.frequency   = f;
      g_pzem.powerFactor = pf;
      g_pzem.valid       = true;
      g_pzem.readAt      = now;

      if (g_pzem.hasPendingThreshold) {
        doThreshold                = true;
        threshWatts                = g_pzem.pendingThresholdW;
        g_pzem.alarmThreshold      = threshWatts;
        g_pzem.hasPendingThreshold = false;
      }
      xSemaphoreGive(g_pzemMutex);
    }

    if (doThreshold) {
      Serial.printf("[PZEM-FAKE] Alarm threshold -> %d W\n", threshWatts);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

#endif // PZEM_FAKE
