/**
 * log_async.cpp
 *
 * Async serial log queue implementation.
 * See log_async.h for design notes.
 */

#include "log_async.h"
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- Ring buffer -------------------------------------------------------------
// Power-of-2 size enables cheap index masking (& LOG_BUF_MASK).
#define LOG_BUF_SIZE  1024u
#define LOG_BUF_MASK  (LOG_BUF_SIZE - 1u)
#define LOG_MSG_MAX   128u   // max formatted message length (stack tmp)

static char           s_logBuf[LOG_BUF_SIZE];
static volatile uint16_t s_logHead = 0;   // write index (producer)
static volatile uint16_t s_logTail = 0;   // read  index (consumer)
static portMUX_TYPE   s_logMux = portMUX_INITIALIZER_UNLOCKED;

// --- Producer ---------------------------------------------------------------

void logAsync(const char* fmt, ...) {
  char tmp[LOG_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  if (n <= 0) return;
  if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;  // truncated

  portENTER_CRITICAL(&s_logMux);
  uint16_t free_space = (uint16_t)(LOG_BUF_SIZE - ((s_logHead - s_logTail) & LOG_BUF_MASK) - 1u);
  if ((uint16_t)n <= free_space) {
    for (int i = 0; i < n; i++) {
      s_logBuf[s_logHead & LOG_BUF_MASK] = tmp[i];
      s_logHead++;
    }
  }
  // else: buffer full — drop silently
  portEXIT_CRITICAL(&s_logMux);
}

// --- Consumer (drain task, Core 0, priority 0) ------------------------------

static void logDrainTask(void* /*params*/) {
  uint8_t buf[64];
  while (true) {
    uint16_t count = 0;

    portENTER_CRITICAL(&s_logMux);
    while (s_logHead != s_logTail && count < sizeof(buf)) {
      buf[count++] = (uint8_t)s_logBuf[s_logTail & LOG_BUF_MASK];
      s_logTail++;
    }
    portEXIT_CRITICAL(&s_logMux);

    if (count > 0) {
      Serial.write(buf, count);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// --- Task launcher ----------------------------------------------------------

void logDrainTaskStart() {
  xTaskCreatePinnedToCore(
    logDrainTask,
    "LOG_DRAIN",
    2048,
    nullptr,
    0,       // lowest priority
    nullptr,
    0        // Core 0 alongside web/wifi
  );
}
