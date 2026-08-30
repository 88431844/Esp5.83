#pragma once

#include <stddef.h>
#include <stdint.h>

namespace partial_refresh {

inline size_t windowRowBytes(int width) {
  return static_cast<size_t>(width / 8);
}

inline bool isValidWindow(
    int panelWidth, int panelHeight, int x, int y, int width, int height,
    size_t length) {
  if (panelWidth <= 0 || panelHeight <= 0 || x < 0 || y < 0 ||
      width <= 0 || height <= 0 || x % 8 != 0 || width % 8 != 0 ||
      width > panelWidth - x || height > panelHeight - y) {
    return false;
  }

  return length == windowRowBytes(width) * static_cast<size_t>(height);
}

inline size_t windowImageIndex(
    int panelY, int y, int width, int windowByteX) {
  return static_cast<size_t>(panelY - y) * windowRowBytes(width) +
         static_cast<size_t>(windowByteX);
}

inline uint32_t leftControllerBytes(int x) {
  return static_cast<uint32_t>(x / 2);
}

inline uint32_t windowControllerBytes(int width) {
  return static_cast<uint32_t>(width / 2);
}

inline uint32_t rightControllerBytes(int panelWidth, int x, int width) {
  return static_cast<uint32_t>((panelWidth - x - width) / 2);
}

inline size_t controllerFrameBytes(int panelWidth, int panelHeight) {
  return static_cast<size_t>(panelWidth / 2) *
         static_cast<size_t>(panelHeight);
}

inline uint8_t differentialPixelNibble(
    uint8_t current, uint8_t previous, uint8_t mask) {
  if ((current ^ previous) & mask) return (current & mask) ? 0x03 : 0x00;
  return 0x07;
}

inline uint8_t differentialControllerPair(
    uint8_t current, uint8_t previous, uint8_t pixel) {
  const uint8_t highMask = 0x80 >> pixel;
  const uint8_t lowMask = highMask >> 1;
  const uint8_t high = differentialPixelNibble(current, previous, highMask);
  const uint8_t low = differentialPixelNibble(current, previous, lowMask);
  return (high << 4) | low;
}

}  // namespace partial_refresh
