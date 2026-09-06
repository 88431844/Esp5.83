#pragma once

#include <GxEPD2_BW.h>

// 600x448 Waveshare V1 / GDEW0583T7 driver variant.
// It keeps GxEPD2's proven RAM upload path and applies the manufacturer's
// slower full-refresh PLL and VCOM settings immediately before the update.
class GxEPD2_583_DeepBlack : public GxEPD2_583 {
 public:
  GxEPD2_583_DeepBlack(int16_t cs, int16_t dc, int16_t rst, int16_t busy);

  using GxEPD2_583::refresh;
  void refresh(bool partial_update_mode = false) override;

 private:
  void applyManufacturerFullWaveform();
};
