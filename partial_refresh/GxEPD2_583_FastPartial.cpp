#include "GxEPD2_583_FastPartial.h"
#include "partial_refresh_model.h"

#include <Arduino.h>
#include <string.h>

namespace {

constexpr uint8_t FAST_PLL = 0x3A;

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

GxEPD2_583_FastPartial::GxEPD2_583_FastPartial(
    int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : GxEPD2_583(cs, dc, rst, busy) {}

void GxEPD2_583_FastPartial::clearToWhite() {
  writeScreenBuffer(0xFF);
  refresh(false);
}

void GxEPD2_583_FastPartial::prepareFullRefresh() {
  _last_full_refresh_succeeded = false;
}

bool GxEPD2_583_FastPartial::lastFullRefreshSucceeded() const {
  return _last_full_refresh_succeeded;
}

void GxEPD2_583_FastPartial::refresh(bool partial_update_mode) {
  if (partial_update_mode) {
    _last_full_refresh_succeeded = false;
    GxEPD2_583::refresh(true);
    return;
  }

  _last_full_refresh_succeeded = false;
  if (_using_partial_mode && !initializeStandardFullMode()) return;

  _writeCommand(0x12);
  if (!waitWhileBusyStatus("StandardFull", full_refresh_time)) {
    invalidateFastMode();
    return;
  }
  _last_full_refresh_succeeded = true;
}

void GxEPD2_583_FastPartial::powerOff() {
  _writeCommand(0x02);
  const bool poweredOff =
    waitWhileBusyStatus("StandardPowerOff", power_off_time);
  _power_is_on = false;
  _using_partial_mode = false;
  if (!poweredOff) {
    _last_full_refresh_succeeded = false;
    if (_rst >= 0) _hibernating = true;
  }
}

bool GxEPD2_583_FastPartial::initializeStandardFullMode() {
  if (_hibernating) _reset();

  _writeCommand(0x01);
  _writeData(0x37);
  _writeData(0x00);
  _writeCommand(0x00);
  _writeData(0xCF);
  _writeData(0x08);
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
  _writeCommand(0xE5);
  _writeData(0x03);

  if (!_power_is_on) {
    _writeCommand(0x04);
    if (!waitWhileBusyStatus("StandardPowerOn", power_on_time)) {
      invalidateFastMode();
      return false;
    }
    _power_is_on = true;
  }
  _using_partial_mode = false;
  return true;
}

bool GxEPD2_583_FastPartial::beginFastMode() {
  _using_partial_mode = false;
  if (_busy >= 0 &&
      !waitWhileBusyStatus("FastPartialReady", power_on_time)) {
    invalidateFastMode();
    return false;
  }
  loadFastLut();
  if (!_power_is_on) {
    _writeCommand(0x04);
    if (!waitWhileBusyStatus("FastPartialPowerOn", power_on_time)) {
      invalidateFastMode();
      return false;
    }
    _power_is_on = true;
  }
  _using_partial_mode = true;
  _initial_write = false;
  return true;
}

bool GxEPD2_583_FastPartial::refreshWindow(
    const uint8_t* image, uint8_t* previous, size_t length,
    int16_t x, int16_t y, int16_t width, int16_t height) {
  if (!image || !previous || !_power_is_on || !_using_partial_mode ||
      !partial_refresh::isValidWindow(
          WIDTH, HEIGHT, x, y, width, height, length)) {
    return false;
  }

  _writeCommand(0x91);
  setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x10);
  _startTransfer();
  const uint8_t unchangedPair = 0x77;
  const uint32_t leftControllerBytes = partial_refresh::leftControllerBytes(x);
  const uint32_t rightControllerBytes =
      partial_refresh::rightControllerBytes(WIDTH, x, width);
  for (int16_t panelY = 0; panelY < HEIGHT; ++panelY) {
    if (panelY < y || panelY >= y + height) {
      _pSPIx->writePattern(&unchangedPair, 1, WIDTH / 2);
      continue;
    }

    _pSPIx->writePattern(&unchangedPair, 1, leftControllerBytes);
    const size_t rowOffset =
        partial_refresh::windowImageIndex(panelY, y, width, 0);
    for (int16_t panelByteX = 0; panelByteX < width / 8; ++panelByteX) {
      const size_t index = rowOffset + static_cast<size_t>(panelByteX);
      transferDifferentialByte(image[index], previous[index]);
    }
    _pSPIx->writePattern(&unchangedPair, 1, rightControllerBytes);
  }
  _endTransfer();
  _writeCommand(0x92);

  _writeCommand(0x91);
  setPartialRamArea(x, y, width, height);
  _writeCommand(0x12);
  if (!waitWhileBusyStatus("FastPartial", partial_refresh_time)) {
    invalidateFastMode();
    return false;
  }
  _writeCommand(0x92);

  memcpy(previous, image, length);
  return true;
}

bool GxEPD2_583_FastPartial::waitWhileBusyStatus(
    const char* comment, uint16_t busyTime) {
  if (_busy < 0) {
    delay(busyTime);
    return true;
  }

  delay(1);
  const uint32_t startedAt = millis();
  const uint32_t startedMicros = micros();
  const uint32_t timeoutMs =
      _busy_timeout / 1000U + (_busy_timeout % 1000U != 0U ? 1U : 0U);
  bool ready = false;
  while (true) {
    if (digitalRead(_busy) != _busy_level) {
      ready = true;
      break;
    }
    if (_busy_callback) {
      _busy_callback(_busy_callback_parameter);
    } else {
      delay(1);
    }
    if (digitalRead(_busy) != _busy_level) {
      ready = true;
      break;
    }
    if (static_cast<uint32_t>(millis() - startedAt) >= timeoutMs) {
      Serial.println("Busy Timeout!");
      break;
    }
#if defined(ESP8266) || defined(ESP32)
    yield();
#endif
  }

#if !defined(DISABLE_DIAGNOSTIC_OUTPUT)
  if (comment && _diag_enabled) {
    Serial.print(comment);
    Serial.print(" : ");
    Serial.println(static_cast<uint32_t>(micros() - startedMicros));
  }
#endif
  (void)comment;
  (void)startedMicros;
  return ready;
}

void GxEPD2_583_FastPartial::invalidateFastMode() {
  _power_is_on = false;
  _using_partial_mode = false;
  // Make the inherited normal initialization reset before full recovery.
  if (_rst >= 0) _hibernating = true;
}

void GxEPD2_583_FastPartial::loadFastLut() {
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
  _writeData(FAST_PLL);
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

void GxEPD2_583_FastPartial::loadLut(
    uint8_t command, const uint8_t* data,
    uint16_t length, uint16_t totalLength) {
  _writeCommand(command);
  _writeDataPGM(data, length, totalLength - length);
}

void GxEPD2_583_FastPartial::setPartialRamArea(
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

void GxEPD2_583_FastPartial::transferDifferentialByte(
    uint8_t current, uint8_t previous) {
  for (uint8_t pixel = 0; pixel < 8; pixel += 2) {
    _transfer(partial_refresh::differentialControllerPair(
        current, previous, pixel));
  }
}
