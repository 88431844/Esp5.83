#include "GxEPD2_583_DeepBlack.h"

GxEPD2_583_DeepBlack::GxEPD2_583_DeepBlack(
    int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : GxEPD2_583(cs, dc, rst, busy) {}

void GxEPD2_583_DeepBlack::refresh(bool partial_update_mode) {
  if (partial_update_mode) {
    GxEPD2_583::refresh(true);
    return;
  }

  applyManufacturerFullWaveform();
  _writeCommand(0x12);
  _waitWhileBusy("_Update_DeepBlack", full_refresh_time);
  _using_partial_mode = false;
}

void GxEPD2_583_DeepBlack::applyManufacturerFullWaveform() {
  if (_hibernating) _reset();

  // Waveshare's official 600x448 V1 initialization values. GxEPD2_583 uses
  // PLL 0x3A and VCOM 0x28; the V1 reference uses the slower 0x3C waveform
  // and VCOM 0x1E.
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
  _writeData(0x3C);
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
  _writeData(0x1E);
  _writeCommand(0xE5);
  _writeData(0x03);

  if (!_power_is_on) {
    _writeCommand(0x04);
    _waitWhileBusy("_PowerOn_DeepBlack", power_on_time);
    _power_is_on = true;
  }
  _using_partial_mode = false;
}
