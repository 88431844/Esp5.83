#include "GxEPD2_583_FastClock.h"

#include <Arduino.h>
#include <string.h>

namespace {

const uint8_t lut_20_lutc_fast[] PROGMEM = {
    0x01, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_21_lutb_fast[] PROGMEM = {
    0x01, 0x10, 0x00, 0x00, 0x00, 0x1B, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_22_lutw_fast[] PROGMEM = {
    0x01, 0x20, 0x00, 0x00, 0x00, 0x25, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_23_lutg1[] PROGMEM = {
    0x01, 0x21, 0x21, 0x21, 0x21, 0x1B, 0x10,
    0x0F, 0x0A, 0x0A, 0x0F, 0x10, 0x08,
};

const uint8_t lut_24_lutg2[] PROGMEM = {
    0x01, 0x21, 0x21, 0x21, 0x21, 0x1B, 0x10,
    0x0F, 0x0A, 0x0A, 0x0F, 0x10, 0x10,
};

const uint8_t lut_25_lutr0[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_26_lutr1[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_27_lutr2[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_28_lutr3[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t lut_29_lutxon[] PROGMEM = {
    0x00, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

}  // namespace

GxEPD2_583_FastClock::GxEPD2_583_FastClock(
    int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : GxEPD2_583(cs, dc, rst, busy) {}

void GxEPD2_583_FastClock::clearToWhite() {
  writeScreenBuffer(0xFF);
  refresh(false);
}

void GxEPD2_583_FastClock::beginFastMode() {
  loadFastLut();
  if (!_power_is_on) {
    _writeCommand(0x04);
    _waitWhileBusy("FastClockPowerOn", power_on_time);
    _power_is_on = true;
  }
  _using_partial_mode = true;
  _initial_write = false;
}

bool GxEPD2_583_FastClock::refreshClockWindow(
    const uint8_t* image, uint8_t* previous, size_t length,
    int16_t x, int16_t y, int16_t width, int16_t height) {
  if (!image || !previous || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x % 8 != 0 || width % 8 != 0 || x + width > WIDTH ||
      y + height > HEIGHT) {
    return false;
  }

  const size_t expectedLength =
      static_cast<size_t>(width / 8) * static_cast<size_t>(height);
  if (length != expectedLength || !_using_partial_mode) return false;

  _writeCommand(0x91);
  setPartialRamArea(x, y, width, height);
  _writeCommand(0x10);
  _startTransfer();
  for (size_t i = 0; i < length; ++i) {
    transferDifferentialByte(image[i], previous[i]);
  }
  _endTransfer();
  _writeCommand(0x92);

  _writeCommand(0x91);
  setPartialRamArea(x, y, width, height);
  _writeCommand(0x12);
  _waitWhileBusy("FastClockPartial", partial_refresh_time);
  _writeCommand(0x92);

  memcpy(previous, image, length);
  return true;
}

void GxEPD2_583_FastClock::loadFastLut() {
  if (_hibernating) _reset();

  _writeCommand(0x01);
  _writeData(0x37);
  _writeData(0x00);
  _writeData(0x05);
  _writeData(0x05);
  _writeCommand(0x00);
  _writeData(0xCF);
  _writeData(0x88);
  _writeCommand(0x06);
  _writeData(0xC7);
  _writeData(0xCC);
  _writeData(0x28);
  _writeCommand(0x30);
  _writeData(0x3A);
  _writeCommand(0x41);
  _writeData(0x00);
  _writeCommand(0x50);
  _writeData(0x77);
  _writeCommand(0x60);
  _writeData(0x22);
  _writeCommand(0x61);
  _writeData(0x02);
  _writeData(0x58);
  _writeData(0x01);
  _writeData(0xC0);
  _writeCommand(0x82);
  _writeData(0x28);

  loadLut(0x20, lut_20_lutc_fast, sizeof(lut_20_lutc_fast), 220);
  loadLut(0x21, lut_21_lutb_fast, sizeof(lut_21_lutb_fast), 260);
  loadLut(0x22, lut_22_lutw_fast, sizeof(lut_22_lutw_fast), 260);
  loadLut(0x23, lut_23_lutg1, sizeof(lut_23_lutg1), 260);
  loadLut(0x24, lut_24_lutg2, sizeof(lut_24_lutg2), 260);
  loadLut(0x25, lut_25_lutr0, sizeof(lut_25_lutr0), 260);
  loadLut(0x26, lut_26_lutr1, sizeof(lut_26_lutr1), 260);
  loadLut(0x27, lut_27_lutr2, sizeof(lut_27_lutr2), 260);
  loadLut(0x28, lut_28_lutr3, sizeof(lut_28_lutr3), 260);
  loadLut(0x29, lut_29_lutxon, sizeof(lut_29_lutxon), 200);

  _writeCommand(0xE5);
  _writeData(0x03);
}

void GxEPD2_583_FastClock::loadLut(
    uint8_t command, const uint8_t* data,
    uint16_t length, uint16_t totalLength) {
  _writeCommand(command);
  _writeDataPGM(data, length, totalLength - length);
}

void GxEPD2_583_FastClock::setPartialRamArea(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
  const uint16_t xEnd = (x + width - 1) | 0x0007;
  const uint16_t yEnd = y + height - 1;
  x &= 0xFFF8;

  _writeCommand(0x90);
  _writeData(x >> 8);
  _writeData(x & 0xFF);
  _writeData(xEnd >> 8);
  _writeData(xEnd & 0xFF);
  _writeData(y >> 8);
  _writeData(y & 0xFF);
  _writeData(yEnd >> 8);
  _writeData(yEnd & 0xFF);
  _writeData(0x01);
}

void GxEPD2_583_FastClock::transferDifferentialByte(
    uint8_t current, uint8_t previous) {
  uint8_t changed = current ^ previous;
  for (uint8_t pixel = 0; pixel < 8; pixel += 2) {
    const uint8_t highMask = 0x80 >> pixel;
    const uint8_t lowMask = highMask >> 1;
    const uint8_t high = (changed & highMask) == 0
        ? 0x07
        : ((current & highMask) ? 0x03 : 0x00);
    const uint8_t low = (changed & lowMask) == 0
        ? 0x07
        : ((current & lowMask) ? 0x03 : 0x00);
    _transfer((high << 4) | low);
  }
}
