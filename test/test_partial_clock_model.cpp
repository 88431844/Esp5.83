#include <cassert>
#include <cstring>
#include <iostream>

#include "../partial_clock_model.h"

int main() {
  char value[9] = {};

  formatClockTime(value, sizeof(value), 0, 0, 0);
  assert(std::strcmp(value, "00:00:00") == 0);

  formatClockTime(value, sizeof(value), 23, 59, 59);
  assert(std::strcmp(value, "23:59:59") == 0);

  static_assert(CLOCK_X % 8 == 0, "clock x must be byte aligned");
  static_assert(CLOCK_WIDTH % 8 == 0, "clock width must be byte aligned");
  static_assert(CLOCK_X >= 0 && CLOCK_Y >= 0, "clock window must start on panel");
  static_assert(CLOCK_X + CLOCK_WIDTH <= PANEL_WIDTH, "clock window exceeds panel width");
  static_assert(CLOCK_Y + CLOCK_HEIGHT <= PANEL_HEIGHT, "clock window exceeds panel height");
  static_assert(CLOCK_BUFFER_SIZE == 5936, "unexpected clock buffer size");

  std::cout << "Partial clock model tests PASS\n";
  return 0;
}
