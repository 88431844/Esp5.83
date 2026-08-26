#pragma once

#include <stddef.h>
#include <stdio.h>

constexpr int PANEL_WIDTH = 600;
constexpr int PANEL_HEIGHT = 448;

constexpr int CLOCK_X = 88;
constexpr int CLOCK_Y = 168;
constexpr int CLOCK_WIDTH = 424;
constexpr int CLOCK_HEIGHT = 112;
constexpr size_t CLOCK_BUFFER_SIZE = CLOCK_WIDTH / 8 * CLOCK_HEIGHT;

inline void formatClockTime(
    char* output, size_t capacity, int hour, int minute, int second) {
  snprintf(output, capacity, "%02d:%02d:%02d", hour, minute, second);
}
