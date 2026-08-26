# Partial Refresh Clock Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and flash an isolated ESP8266 test firmware that shows a centered `HH:MM:SS` clock and updates its fixed window every second with the supplied fast partial-refresh waveform.

**Architecture:** Keep the production dashboard untouched. A host-testable header owns time formatting and byte-aligned geometry; a repository-local subclass of `GxEPD2_583` owns only the experimental LUT and differential transfer; a separate Arduino sketch renders into a 1-bit clock canvas and drives the update loop.

**Tech Stack:** C++17 host tests, Arduino CLI 1.5.1, ESP8266 Arduino Core 3.1.2, GxEPD2 1.6.9, Adafruit GFX `GFXcanvas1`, U8g2 for Adafruit GFX.

---

### Task 1: Clock Formatting And Geometry

**Files:**
- Create: `partial_clock_model.h`
- Create: `test/test_partial_clock_model.cpp`
- Create: `test/run_partial_clock_tests.sh`

- [ ] **Step 1: Write failing host tests**

Test `formatClockTime()` with midnight and the final second of the day. Assert a 424 x 112 window at `(88, 168)`, including byte alignment and containment in 600 x 448.

```cpp
char value[9];
formatClockTime(value, sizeof(value), 0, 0, 0);
assert(strcmp(value, "00:00:00") == 0);
formatClockTime(value, sizeof(value), 23, 59, 59);
assert(strcmp(value, "23:59:59") == 0);
static_assert(CLOCK_X % 8 == 0);
static_assert(CLOCK_WIDTH % 8 == 0);
static_assert(CLOCK_X + CLOCK_WIDTH <= PANEL_WIDTH);
static_assert(CLOCK_Y + CLOCK_HEIGHT <= PANEL_HEIGHT);
```

- [ ] **Step 2: Run the test and verify RED**

Run: `sh test/run_partial_clock_tests.sh`

Expected: compilation fails because `partial_clock_model.h` does not exist.

- [ ] **Step 3: Add the minimal model**

```cpp
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

inline void formatClockTime(char* output, size_t capacity, int hour, int minute, int second) {
  snprintf(output, capacity, "%02d:%02d:%02d", hour, minute, second);
}
```

- [ ] **Step 4: Run the test and verify GREEN**

Run: `sh test/run_partial_clock_tests.sh`

Expected: `partial clock model tests passed` and exit code 0.

- [ ] **Step 5: Commit the model**

```sh
git add partial_clock_model.h test/test_partial_clock_model.cpp test/run_partial_clock_tests.sh
git commit -m "test: define partial clock geometry"
```

### Task 2: Low-Memory Fast Driver

**Files:**
- Create: `partial_refresh_clock/GxEPD2_583_FastClock.h`
- Create: `partial_refresh_clock/GxEPD2_583_FastClock.cpp`
- Create: `test/verify_partial_clock.sh`

- [ ] **Step 1: Write a failing source gate**

Require the driver to derive from the installed `GxEPD2_583`, expose `beginFastMode()` and `refreshClockWindow()`, use the supplied fast LUT identifiers, and avoid any full-screen image arrays.

```sh
rg -q 'class GxEPD2_583_FastClock : public GxEPD2_583' partial_refresh_clock/GxEPD2_583_FastClock.h
rg -q 'beginFastMode' partial_refresh_clock/GxEPD2_583_FastClock.h
rg -q 'refreshClockWindow' partial_refresh_clock/GxEPD2_583_FastClock.h
rg -q 'lut_20_lutc_fast' partial_refresh_clock/GxEPD2_583_FastClock.cpp
! rg -q '600[^\n]*448|WIDTH[^\n]*/ 8[^\n]*HEIGHT' partial_refresh_clock/GxEPD2_583_FastClock.h
```

- [ ] **Step 2: Run the source gate and verify RED**

Run: `sh test/verify_partial_clock.sh`

Expected: failure because the driver files do not exist.

- [ ] **Step 3: Declare the focused driver API**

```cpp
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
  void setPartialRamArea(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
  void transferDifferentialByte(uint8_t current, uint8_t previous);
};
```

- [ ] **Step 4: Implement only the required controller operations**

Copy the exact LUT byte sequences for commands `0x20` through `0x29` from the supplied V1.2 driver into `PROGMEM` arrays. `clearToWhite()` uses the inherited normal full update. `beginFastMode()` loads the experimental fast LUT and powers the panel. `refreshClockWindow()` validates byte alignment and `length == width / 8 * height`, writes differential nibbles (`0x07` unchanged, `0x03` white, `0x00` black) to command `0x10`, triggers command `0x12` for the selected partial area, waits on BUSY, and copies the current window to `previous` only after the refresh completes.

```cpp
uint8_t nibble = ((current ^ previous) & mask) == 0
  ? 0x07
  : ((current & mask) ? 0x03 : 0x00);
```

- [ ] **Step 5: Run the source gate and verify GREEN**

Run: `sh test/verify_partial_clock.sh`

Expected: `partial clock source checks passed` and exit code 0.

- [ ] **Step 6: Commit the driver**

```sh
git add partial_refresh_clock/GxEPD2_583_FastClock.h partial_refresh_clock/GxEPD2_583_FastClock.cpp test/verify_partial_clock.sh
git commit -m "feat: add low-memory fast partial driver"
```

### Task 3: Standalone Clock Firmware

**Files:**
- Create: `partial_refresh_clock/partial_refresh_clock.ino`

- [ ] **Step 1: Extend the source gate for firmware behavior**

Require `configTime(28800, ...)`, the `u8g2_font_logisoso78_tn` numeric font, `formatClockTime`, a partial refresh counter, refresh duration and heap logging, and a 300-update full-clean interval.

```sh
rg -q 'configTime\(28800' partial_refresh_clock/partial_refresh_clock.ino
rg -q 'u8g2_font_logisoso78_tn' partial_refresh_clock/partial_refresh_clock.ino
rg -q 'formatClockTime' partial_refresh_clock/partial_refresh_clock.ino
rg -q 'FULL_CLEAN_INTERVAL = 300' partial_refresh_clock/partial_refresh_clock.ino
rg -q 'ESP.getFreeHeap' partial_refresh_clock/partial_refresh_clock.ino
```

- [ ] **Step 2: Run the source gate and verify RED**

Run: `sh test/verify_partial_clock.sh`

Expected: failure because the clock sketch does not exist.

- [ ] **Step 3: Implement setup and rendering**

Create a `GFXcanvas1(CLOCK_WIDTH, CLOCK_HEIGHT)`, bind U8g2 to it, fill it white, draw black `HH:MM:SS`, and horizontally/vertically center the measured glyph bounds. In `setup()`, connect Wi-Fi, synchronize time, initialize the display, perform one full white refresh, initialize the previous window to white, enter fast mode, and draw immediately.

- [ ] **Step 4: Implement the one-second loop**

Use `time(nullptr)` as the wall-clock source and update only when the epoch second changes. At every 300 successful partial updates, run a full white clean, reset the previous buffer to white, reload fast mode, and redraw. Log one line per update:

```text
Clock 12:34:56 partial=42 refresh_ms=380 heap=42112
```

Retry Wi-Fi and NTP periodically without stopping the last valid clock display.

- [ ] **Step 5: Run host tests and source gate**

Run: `sh test/run_partial_clock_tests.sh && sh test/verify_partial_clock.sh`

Expected: both commands exit 0.

- [ ] **Step 6: Commit the firmware**

```sh
git add partial_refresh_clock/partial_refresh_clock.ino test/verify_partial_clock.sh
git commit -m "feat: add partial refresh digital clock"
```

### Task 4: Reproducible Build And Flash

**Files:**
- Create: `tools/build_partial_clock.sh`
- Create: `tools/flash_partial_clock.sh`
- Modify: `README.md`

- [ ] **Step 1: Add build and flash scripts**

`build_partial_clock.sh` creates `build/partial_refresh_clock`, copies the sketch, local driver, model, and ignored `secrets.h`, then compiles with `esp8266:esp8266:nodemcuv2` into `build/partial-clock-output`. `flash_partial_clock.sh` accepts exactly one serial port, invokes the build, uploads that output, and starts a raw 115200-baud monitor.

- [ ] **Step 2: Document the branch-only test**

Add a short README section with the test commands, `/dev/cu.usbserial-1120` as the known device port, expected serial line, 300-update cleaning behavior, and the experimental waveform warning.

- [ ] **Step 3: Run all host checks**

Run: `sh test/run_dashboard_tests.sh && sh test/verify_pve_dashboard.sh && sh test/run_partial_clock_tests.sh && sh test/verify_partial_clock.sh && git diff --check`

Expected: every command exits 0 with no whitespace errors.

- [ ] **Step 4: Compile the ESP8266 firmware**

Run: `sh tools/build_partial_clock.sh`

Expected: Arduino CLI exits 0 and reports RAM and flash usage below the NodeMCU v2 limits.

- [ ] **Step 5: Commit build support and documentation**

```sh
git add tools/build_partial_clock.sh tools/flash_partial_clock.sh README.md
git commit -m "build: add partial clock flash workflow"
```

### Task 5: Hardware Upload And Runtime Verification

**Files:**
- No tracked file changes.

- [ ] **Step 1: Confirm the target port is present**

Run: `arduino-cli board list`

Expected: `/dev/cu.usbserial-1120` is listed.

- [ ] **Step 2: Upload the compiled firmware**

Run: `arduino-cli upload --port /dev/cu.usbserial-1120 --fqbn esp8266:esp8266:nodemcuv2 --input-dir build/partial-clock-output build/partial_refresh_clock`

Expected: upload exits 0 after writing and verifying flash.

- [ ] **Step 3: Monitor multiple refresh cycles**

Run: `arduino-cli monitor --port /dev/cu.usbserial-1120 --config baudrate=115200 --raw`

Expected: NTP success followed by at least five consecutive `Clock HH:MM:SS` lines with increasing seconds and partial counts, finite refresh times, stable heap, and no watchdog reset or exception.

- [ ] **Step 4: Perform final verification**

Run the complete host check command again, inspect `git status --short --branch`, and inspect `git log --oneline main..HEAD`.

Expected: all checks pass, the worktree is clean, and only test-branch commits are ahead of `main`.
