#pragma once

#include <stddef.h>
#include <stdint.h>

#include <epd/GxEPD2_583.h>

class GxEPD2_583_FastClock : public GxEPD2_583 {
 public:
  GxEPD2_583_FastClock(int16_t cs, int16_t dc, int16_t rst, int16_t busy);

  void clearToWhite();
  void beginFastMode();
  bool refreshClockWindow(const uint8_t* image, uint8_t* previous,
                          size_t length, int16_t x, int16_t y,
                          int16_t width, int16_t height);

 private:
  void loadFastLut();
  void loadLut(uint8_t command, const uint8_t* data,
               uint16_t length, uint16_t totalLength);
  void setPartialRamArea(uint16_t x, uint16_t y,
                         uint16_t width, uint16_t height);
  void transferDifferentialByte(uint8_t current, uint8_t previous);
};
