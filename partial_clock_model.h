#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

constexpr int PANEL_WIDTH = 600;
constexpr int PANEL_HEIGHT = 448;

constexpr int CLOCK_X = 88;
constexpr int CLOCK_Y = 168;
constexpr int CLOCK_WIDTH = 424;
constexpr int CLOCK_HEIGHT = 112;
constexpr size_t PANEL_BUFFER_SIZE = PANEL_WIDTH / 8 * PANEL_HEIGHT;
constexpr size_t CLOCK_BUFFER_SIZE = CLOCK_WIDTH / 8 * CLOCK_HEIGHT;

inline uint8_t panelByteForClock(
    const uint8_t* clockBuffer, int panelByteX, int panelY) {
  constexpr int clockByteX = CLOCK_X / 8;
  constexpr int clockByteWidth = CLOCK_WIDTH / 8;
  if (panelY < CLOCK_Y || panelY >= CLOCK_Y + CLOCK_HEIGHT ||
      panelByteX < clockByteX ||
      panelByteX >= clockByteX + clockByteWidth) {
    return 0xFF;
  }

  const size_t index =
      static_cast<size_t>(panelY - CLOCK_Y) * clockByteWidth +
      static_cast<size_t>(panelByteX - clockByteX);
  return clockBuffer[index];
}

inline bool shouldCleanClock(
    uint32_t partialCount, uint32_t lastCleanCount, uint32_t interval) {
  return interval > 0 && partialCount - lastCleanCount >= interval;
}

inline bool fullRefreshRetryReady(
    uint32_t now, uint32_t lastAttempt, bool attemptRecorded,
    uint32_t retryInterval) {
  return !attemptRecorded ||
      static_cast<uint32_t>(now - lastAttempt) >= retryInterval;
}

struct ClockFullRecoveryState {
  bool display_ready;
  bool recovery_pending;
  uint32_t retry_anchor;
  bool retry_anchor_recorded;
};

inline ClockFullRecoveryState clockFullRecoveryAfterPartialFailure(
    uint32_t failedAt) {
  return {false, true, failedAt, true};
}

inline void formatClockTime(
    char* output, size_t capacity, int hour, int minute, int second) {
  snprintf(output, capacity, "%02d:%02d:%02d", hour, minute, second);
}
