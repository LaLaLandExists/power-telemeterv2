/**
 * log_async.h
 *
 * Non-blocking asynchronous serial logger for timing-critical tasks.
 *
 * logAsync() formats a message and enqueues it into a static ring buffer
 * under a short spinlock, then returns immediately — it never waits for
 * UART TX to drain.  A low-priority Core 0 drain task flushes the buffer
 * to Serial.write().
 *
 * Usage:
 *   1. Call logDrainTaskStart() once from setup() after Serial.begin().
 *   2. Replace Serial.printf/println in TDMA tasks with logAsync().
 *   3. Non-timing-critical tasks (PZEM, web, nudge) may still use Serial directly.
 *
 * Drop behaviour:
 *   If the ring buffer is full, the message is silently discarded.
 *   This is acceptable for a debug log — timing integrity takes priority.
 */
#pragma once
#include <Arduino.h>

/** Start the background drain task. Call once from setup() after Serial.begin(). */
void logDrainTaskStart();

/**
 * Enqueue a printf-formatted log message for asynchronous serial output.
 * Non-blocking: returns immediately even if UART TX is busy.
 * Safe to call from any task or core.
 * Messages longer than 127 characters are truncated.
 */
void logAsync(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
