#include <cassert>
#include <cstring>
#include <iostream>

#include "../partial_clock_model.h"
#include "../partial_refresh/partial_refresh_model.h"

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
  static_assert(PANEL_BUFFER_SIZE == 33600, "unexpected full panel buffer size");

  uint8_t clockBuffer[CLOCK_BUFFER_SIZE];
  std::memset(clockBuffer, 0xFF, sizeof(clockBuffer));
  clockBuffer[0] = 0x12;
  clockBuffer[CLOCK_BUFFER_SIZE - 1] = 0x34;

  assert(panelByteForClock(clockBuffer, 0, 0) == 0xFF);
  assert(panelByteForClock(clockBuffer, CLOCK_X / 8, CLOCK_Y) == 0x12);
  assert(panelByteForClock(
      clockBuffer,
      (CLOCK_X + CLOCK_WIDTH) / 8 - 1,
      CLOCK_Y + CLOCK_HEIGHT - 1) == 0x34);
  assert(panelByteForClock(clockBuffer, PANEL_WIDTH / 8 - 1, PANEL_HEIGHT - 1) == 0xFF);

  assert(!shouldCleanClock(299, 0, 300));
  assert(shouldCleanClock(300, 0, 300));
  assert(!shouldCleanClock(300, 300, 300));
  assert(!shouldCleanClock(599, 300, 300));
  assert(shouldCleanClock(600, 300, 300));
  assert(!shouldCleanClock(600, 300, 0));

  assert(fullRefreshRetryReady(1000, 0, false, 60000));
  assert(!fullRefreshRetryReady(60999, 1000, true, 60000));
  assert(fullRefreshRetryReady(61000, 1000, true, 60000));
  assert(fullRefreshRetryReady(0x20, 0xFFFFFFF0, true, 0x30));

  const ClockFullRecoveryState partialFailure =
      clockFullRecoveryAfterPartialFailure(1234);
  assert(!partialFailure.display_ready);
  assert(partialFailure.recovery_pending);
  assert(partialFailure.retry_anchor == 1234);
  assert(partialFailure.retry_anchor_recorded);
  assert(!fullRefreshRetryReady(
      61233, partialFailure.retry_anchor,
      partialFailure.retry_anchor_recorded, 60000));
  assert(fullRefreshRetryReady(
      61234, partialFailure.retry_anchor,
      partialFailure.retry_anchor_recorded, 60000));

  constexpr int panelWidth = 600;
  constexpr int panelHeight = 448;
  assert(partial_refresh::isValidWindow(
      panelWidth, panelHeight, 0, 0, 8, 1, 1));
  assert(partial_refresh::isValidWindow(
      panelWidth, panelHeight, 592, 447, 8, 1, 1));
  assert(partial_refresh::isValidWindow(
      panelWidth, panelHeight, 0, 0, panelWidth, panelHeight,
      PANEL_BUFFER_SIZE));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, -8, 0, 8, 1, 1));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, 1, 0, 8, 1, 1));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, 0, 0, 7, 1, 1));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, 592, 0, 16, 1, 2));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, 0, 448, 8, 1, 1));
  assert(!partial_refresh::isValidWindow(
      panelWidth, panelHeight, 0, 0, 8, 1, 2));

  assert(partial_refresh::windowRowBytes(CLOCK_WIDTH) == 53);
  assert(partial_refresh::windowImageIndex(
      CLOCK_Y, CLOCK_Y, CLOCK_WIDTH, 0) == 0);
  assert(partial_refresh::windowImageIndex(
      CLOCK_Y + CLOCK_HEIGHT - 1, CLOCK_Y, CLOCK_WIDTH,
      CLOCK_WIDTH / 8 - 1) == CLOCK_BUFFER_SIZE - 1);

  assert(partial_refresh::leftControllerBytes(0) == 0);
  assert(partial_refresh::rightControllerBytes(panelWidth, 0, 8) == 296);
  assert(partial_refresh::windowControllerBytes(8) == 4);
  assert(partial_refresh::leftControllerBytes(592) == 296);
  assert(partial_refresh::rightControllerBytes(panelWidth, 592, 8) == 0);
  assert(partial_refresh::windowControllerBytes(panelWidth) == 300);
  assert(partial_refresh::leftControllerBytes(0) +
             partial_refresh::windowControllerBytes(8) +
             partial_refresh::rightControllerBytes(panelWidth, 0, 8) ==
         panelWidth / 2);
  assert(partial_refresh::leftControllerBytes(592) +
             partial_refresh::windowControllerBytes(8) +
             partial_refresh::rightControllerBytes(panelWidth, 592, 8) ==
         panelWidth / 2);
  assert(partial_refresh::leftControllerBytes(0) +
             partial_refresh::windowControllerBytes(panelWidth) +
             partial_refresh::rightControllerBytes(
                 panelWidth, 0, panelWidth) == panelWidth / 2);
  assert(partial_refresh::controllerFrameBytes(panelWidth, panelHeight) ==
         134400);

  assert(partial_refresh::differentialPixelNibble(0x80, 0x80, 0x80) == 0x7);
  assert(partial_refresh::differentialPixelNibble(0x80, 0x00, 0x80) == 0x3);
  assert(partial_refresh::differentialPixelNibble(0x00, 0x80, 0x80) == 0x0);
  assert(partial_refresh::differentialControllerPair(0x00, 0x00, 0) == 0x77);
  assert(partial_refresh::differentialControllerPair(0x80, 0x00, 0) == 0x37);
  assert(partial_refresh::differentialControllerPair(0x00, 0x80, 0) == 0x07);

  std::cout << "Partial clock model tests PASS\n";
  return 0;
}
