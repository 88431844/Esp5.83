# NAS Speed Partial Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add five-second NAS RX/TX partial updates to the existing PVE/NAS dashboard while preserving its white background, static footer information, and ten-minute full refresh.

**Architecture:** Convert the clock-specific local display subclass into a reusable fixed-window differential driver, while leaving normal full-screen rendering on the installed GxEPD2 path. Add pure host-testable rate and scheduling helpers, register SNMP callbacks once for the persistent runtime, and keep only two 592-byte buffers for the aligned NAS speed line.

**Tech Stack:** Arduino C++, ESP8266 Arduino Core 3.1.2, GxEPD2 1.6.9, U8g2 for Adafruit GFX, Arduino SNMP Manager 1.1.13, C++17 host tests, shell source gates, Arduino CLI.

---

### Task 1: Add Network Rate And Scheduling Model

**Files:**
- Modify: `dashboard_model.h`
- Modify: `test/test_dashboard_model.cpp`

- [ ] **Step 1: Write failing model tests**

Add these cases to `test/test_dashboard_model.cpp`:

```cpp
  static_assert(NAS_SPEED_X == 304, "unexpected speed x");
  static_assert(NAS_SPEED_Y == 432, "unexpected speed y");
  static_assert(NAS_SPEED_WIDTH == 296, "unexpected speed width");
  static_assert(NAS_SPEED_HEIGHT == 16, "unexpected speed height");
  static_assert(NAS_SPEED_BUFFER_SIZE == 592, "unexpected speed buffer size");
  static_assert(NAS_SPEED_X % 8 == 0, "speed x must be byte aligned");
  static_assert(NAS_SPEED_WIDTH % 8 == 0, "speed width must be byte aligned");

  NetworkCounterSample first = {1000, 4000, 1000, true};
  NetworkCounterSample second = {6120, 14240, 6000, true};
  NetworkRates rates = {};
  assert(calculateNetworkRates(first, second, rates));
  assert(rates.rx_bytes_per_second == 1024);
  assert(rates.tx_bytes_per_second == 2048);
  assert(rates.valid);

  NetworkCounterSample reset = {900, 3000, 11000, true};
  assert(!calculateNetworkRates(second, reset, rates));
  assert(!rates.valid);

  NetworkCounterSample missing = {0, 0, 11000, false};
  assert(!calculateNetworkRates(second, missing, rates));
  assert(!rates.valid);

  char rateText[48];
  formatNetworkRates(rateText, sizeof(rateText), rates);
  assert(strcmp(rateText, "RX:-- TX:--") == 0);

  rates = {1024, 2097152, true};
  formatNetworkRates(rateText, sizeof(rateText), rates);
  assert(strcmp(rateText, "RX:1.0 KB/s TX:2.0 MB/s") == 0);

  assert(!intervalElapsed(4999, 0, 5000));
  assert(intervalElapsed(5000, 0, 5000));
  assert(intervalElapsed(0x00000020U, 0xFFFFFFF0U, 0x30U));
```

- [ ] **Step 2: Run the tests and verify RED**

Run: `sh test/run_dashboard_tests.sh`

Expected: compilation fails because the NAS speed constants and model types do not exist.

- [ ] **Step 3: Add the minimal model implementation**

Add `#include <stdio.h>` and these definitions to `dashboard_model.h`:

```cpp
static const int NAS_SPEED_X = 304;
static const int NAS_SPEED_Y = 432;
static const int NAS_SPEED_WIDTH = 296;
static const int NAS_SPEED_HEIGHT = 16;
static const size_t NAS_SPEED_BUFFER_SIZE =
    NAS_SPEED_WIDTH / 8 * NAS_SPEED_HEIGHT;

struct NetworkCounterSample {
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint32_t sampled_at_ms;
  bool valid;
};

struct NetworkRates {
  uint64_t rx_bytes_per_second;
  uint64_t tx_bytes_per_second;
  bool valid;
};

inline bool intervalElapsed(uint32_t now, uint32_t previous,
                            uint32_t interval) {
  return interval > 0 && static_cast<uint32_t>(now - previous) >= interval;
}

inline bool calculateNetworkRates(const NetworkCounterSample& previous,
                                  const NetworkCounterSample& current,
                                  NetworkRates& output) {
  output = {};
  if (!previous.valid || !current.valid ||
      current.rx_bytes < previous.rx_bytes ||
      current.tx_bytes < previous.tx_bytes) {
    return false;
  }
  const uint32_t elapsed = current.sampled_at_ms - previous.sampled_at_ms;
  if (elapsed == 0) return false;
  output.rx_bytes_per_second =
      ((current.rx_bytes - previous.rx_bytes) * 1000ULL) / elapsed;
  output.tx_bytes_per_second =
      ((current.tx_bytes - previous.tx_bytes) * 1000ULL) / elapsed;
  output.valid = true;
  return true;
}

inline void formatByteRate(char* output, size_t capacity, uint64_t value) {
  if (value >= 1048576ULL) {
    snprintf(output, capacity, "%.1f MB/s",
             static_cast<double>(value) / 1048576.0);
  } else if (value >= 1024ULL) {
    snprintf(output, capacity, "%.1f KB/s",
             static_cast<double>(value) / 1024.0);
  } else {
    snprintf(output, capacity, "%llu B/s",
             static_cast<unsigned long long>(value));
  }
}

inline void formatNetworkRates(char* output, size_t capacity,
                               const NetworkRates& rates) {
  if (!rates.valid) {
    snprintf(output, capacity, "RX:-- TX:--");
    return;
  }
  char rx[20];
  char tx[20];
  formatByteRate(rx, sizeof(rx), rates.rx_bytes_per_second);
  formatByteRate(tx, sizeof(tx), rates.tx_bytes_per_second);
  snprintf(output, capacity, "RX:%s TX:%s", rx, tx);
}
```

- [ ] **Step 4: Run the tests and verify GREEN**

Run: `sh test/run_dashboard_tests.sh`

Expected: `Dashboard model tests PASS`.

- [ ] **Step 5: Check the patch**

Run: `git diff --check -- dashboard_model.h test/test_dashboard_model.cpp`

Expected: exit 0 with no output. Keep implementation changes uncommitted until the physical display is accepted.

### Task 2: Generalize The Differential Display Driver

**Files:**
- Move: `partial_refresh_clock/GxEPD2_583_FastClock.h` to `partial_refresh/GxEPD2_583_FastPartial.h`
- Move: `partial_refresh_clock/GxEPD2_583_FastClock.cpp` to `partial_refresh/GxEPD2_583_FastPartial.cpp`
- Modify: `partial_refresh_clock/partial_refresh_clock.ino`
- Modify: `partial_clock_model.h`
- Modify: `test/test_partial_clock_model.cpp`
- Modify: `test/verify_partial_clock.sh`
- Modify: `tools/build_partial_clock.sh`

- [ ] **Step 1: Change the source gate first**

Update `test/verify_partial_clock.sh` to require the generic files and API:

```sh
driver_header="$project_root/partial_refresh/GxEPD2_583_FastPartial.h"
driver_source="$project_root/partial_refresh/GxEPD2_583_FastPartial.cpp"

rg -q 'class GxEPD2_583_FastPartial : public GxEPD2_583' "$driver_header"
rg -q 'beginFastMode' "$driver_header"
rg -q 'refreshWindow' "$driver_header"
rg -q 'FAST_PLL = 0x3A' "$driver_source"
rg -q 'setPartialRamArea\(0, 0, WIDTH, HEIGHT\)' "$driver_source"
rg -q 'writePattern' "$driver_source"

if rg -q 'loadNormalLut|drawInitialClock|writeFullClockFrame' "$driver_source"; then
  echo "Custom full-refresh path must not remain in the fast driver" >&2
  exit 1
fi
```

Retain the existing no-full-screen-buffer check and update the build-script
check to require `GxEPD2_583_FastPartial.cpp`.

- [ ] **Step 2: Run the source gate and verify RED**

Run: `sh test/verify_partial_clock.sh`

Expected: non-zero exit because the generic driver files do not exist.

- [ ] **Step 3: Move the files and rename the public class**

Run:

```sh
mkdir -p partial_refresh
git mv partial_refresh_clock/GxEPD2_583_FastClock.h partial_refresh/GxEPD2_583_FastPartial.h
git mv partial_refresh_clock/GxEPD2_583_FastClock.cpp partial_refresh/GxEPD2_583_FastPartial.cpp
```

Rename the class, constructor, include, and public method in both files:

```cpp
class GxEPD2_583_FastPartial : public GxEPD2_583 {
 public:
  GxEPD2_583_FastPartial(int16_t cs, int16_t dc, int16_t rst, int16_t busy);

  void clearToWhite();
  void beginFastMode();
  bool refreshWindow(const uint8_t* image, uint8_t* previous,
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
```

Keep `clearToWhite()` on the inherited standard GxEPD2 full-update path:

```cpp
void GxEPD2_583_FastPartial::clearToWhite() {
  writeScreenBuffer(0xFF);
  refresh(false);
}
```

Delete the custom normal LUT arrays and the `loadNormalLut()`,
`drawInitialClock()`, `writeFullClockFrame()`, and `transferAbsoluteByte()`
methods. Preserve all fast LUT byte arrays exactly and keep:

```cpp
constexpr uint8_t FAST_PLL = 0x3A;
```

- [ ] **Step 4: Make the window implementation geometry-independent**

Replace `refreshClockWindow()` with:

```cpp
bool GxEPD2_583_FastPartial::refreshWindow(
    const uint8_t* image, uint8_t* previous, size_t length,
    int16_t x, int16_t y, int16_t width, int16_t height) {
  if (!image || !previous || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x % 8 != 0 || width % 8 != 0 || x + width > WIDTH ||
      y + height > HEIGHT || !_using_partial_mode) {
    return false;
  }
  const size_t rowBytes = static_cast<size_t>(width / 8);
  if (length != rowBytes * static_cast<size_t>(height)) return false;

  _writeCommand(0x91);
  setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x10);
  _startTransfer();
  const uint8_t unchangedPair = 0x77;
  const uint32_t leftControllerBytes = x / 2;
  const uint32_t rightControllerBytes = (WIDTH - x - width) / 2;
  for (int16_t panelY = 0; panelY < HEIGHT; ++panelY) {
    if (panelY < y || panelY >= y + height) {
      _pSPIx->writePattern(&unchangedPair, 1, WIDTH / 2);
      continue;
    }
    _pSPIx->writePattern(&unchangedPair, 1, leftControllerBytes);
    const size_t row = static_cast<size_t>(panelY - y) * rowBytes;
    for (size_t byteX = 0; byteX < rowBytes; ++byteX) {
      transferDifferentialByte(image[row + byteX], previous[row + byteX]);
    }
    _pSPIx->writePattern(&unchangedPair, 1, rightControllerBytes);
  }
  _endTransfer();
  _writeCommand(0x92);

  _writeCommand(0x91);
  setPartialRamArea(x, y, width, height);
  _writeCommand(0x12);
  _waitWhileBusy("FastPartial", partial_refresh_time);
  _writeCommand(0x92);

  memcpy(previous, image, length);
  return true;
}
```

- [ ] **Step 5: Update the clock regression firmware**

In `partial_refresh_clock/partial_refresh_clock.ino`, include the generic
header, instantiate `GxEPD2_583_FastPartial`, call `refreshWindow()`, and return
to the proven white-clean startup/full-clean flow:

```cpp
void cleanPanelAndResumeFastMode() {
  Serial.println("Full clean refresh");
  display.clearToWhite();
  memset(previousClock, 0xFF, sizeof(previousClock));
  display.beginFastMode();
}
```

After cleaning, render the current clock and call `refreshWindow()` once so
the panel and `previousClock` agree. Remove all calls to `drawInitialClock()`.
Update `tools/build_partial_clock.sh` to copy the two files from
`partial_refresh/` into the generated sketch directory.

- [ ] **Step 6: Run driver and clock verification**

Run:

```sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
sh tools/build_partial_clock.sh
```

Expected: both test scripts report PASS and the ESP8266 build exits 0 without
RAM, IRAM, or flash overflow.

### Task 3: Add Production Source Gates And Build Wiring

**Files:**
- Modify: `test/verify_pve_dashboard.sh`
- Modify: `tools/build_firmware.sh`

- [ ] **Step 1: Write failing production source checks**

Add requirements to `test/verify_pve_dashboard.sh` for:

```sh
for source_text in \
  '#include "GxEPD2_583_FastPartial.h"' \
  'GxEPD2_BW<GxEPD2_583_FastPartial, 32>' \
  'NAS_SPEED_REFRESH_INTERVAL_MS = 5000' \
  'FULL_REFRESH_INTERVAL_MS = 600000' \
  'ifHCInOctets' \
  'ifHCOutOctets' \
  'ipAdEntIfIndex' \
  'addCounter64Handler' \
  'display.epd2.refreshWindow' \
  'display.epd2.beginFastMode'; do
  rg -qF "$source_text" "$sketch" || {
    echo "Required NAS speed implementation is missing: $source_text" >&2
    exit 1
  }
done

if rg -q 'ESP\.deepSleep' "$sketch"; then
  echo "Persistent partial-refresh dashboard must not enter deep sleep" >&2
  exit 1
fi
```

Add build-script checks requiring both generic driver files to be copied into
the production sketch directory.

Replace the old function-local SNMP ownership gate with checks for the new
persistent global declarations:

```sh
for declaration in \
  'static WiFiUDP nasUdp;' \
  'static SNMPManager nasSnmp(SNMP_COMMUNITY);'; do
  rg -qF "$declaration" "$sketch" || {
    echo "Required persistent NAS declaration is missing: $declaration" >&2
    exit 1
  }
done
```

- [ ] **Step 2: Run the source gate and verify RED**

Run: `sh test/verify_pve_dashboard.sh`

Expected: non-zero exit because the production sketch still uses the standard
driver and deep sleep.

- [ ] **Step 3: Wire the generic driver into the build**

Extend `tools/build_firmware.sh`:

```sh
cp "$project_root/partial_refresh/GxEPD2_583_FastPartial.h" \
  "$project_root/partial_refresh/GxEPD2_583_FastPartial.cpp" \
  "$project_root/dashboard_model.h" \
  "$project_root/secrets.h" \
  "$sketch_dir/"
```

Do not copy or modify the globally installed GxEPD2 library.

### Task 4: Make NAS SNMP Monitoring Persistent

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`

- [ ] **Step 1: Replace one-shot SNMP ownership with persistent state**

Add these globals beside the existing NAS buffers:

```cpp
static WiFiUDP nasUdp;
static SNMPManager nasSnmp(SNMP_COMMUNITY);
bool nasCallbacksReady = false;
bool nasCountersReady = false;
int nasInterfaceIndex = -1;
uint64_t nasRxOctets = UINT64_MAX;
uint64_t nasTxOctets = UINT64_MAX;
NetworkCounterSample previousNetworkSample = {};
NetworkRates currentNetworkRates = {};

ValueCallback* cbUptime = nullptr;
ValueCallback* cbInterfaceIndex = nullptr;
ValueCallback* cbRxOctets = nullptr;
ValueCallback* cbTxOctets = nullptr;
ValueCallback* cbName[4] = {};
ValueCallback* cbAlloc[4] = {};
ValueCallback* cbTotal[4] = {};
ValueCallback* cbUsed[4] = {};

char oidInterfaceIndex[64];
char oidRxOctets[64];
char oidTxOctets[64];
```

Create a bounded request helper that reuses the manager and UDP socket:

```cpp
bool requestNAS(ValueCallback* const* callbacks, size_t count,
                int requestId, uint32_t timeoutMs) {
  SNMPGet request(SNMP_COMMUNITY, 1);
  request.setUDP(&nasUdp);
  request.setRequestID(requestId);
  for (size_t i = 0; i < count; ++i) request.addOIDPointer(callbacks[i]);
  if (!request.sendTo(nas_ip)) return false;
  const uint32_t started = millis();
  while (!intervalElapsed(millis(), started, timeoutMs)) {
    nasSnmp.loop();
    delay(10);
  }
  return true;
}
```

- [ ] **Step 2: Register all static callbacks exactly once**

Extract the existing uptime and volume callback registration into
`initializeNASCallbacks()`. Build the interface-index OID from the configured
IPv4 address:

```cpp
void initializeNASCallbacks() {
  if (nasCallbacksReady) return;
  nasSnmp._udp = nullptr;
  nasSnmp.setUDP(&nasUdp);

  cbUptime = nasSnmp.addTimestampHandler(
      nas_ip, ".1.3.6.1.2.1.25.1.1.0", &g_sysUptime);
  const int volumeIndices[4] = {59, 57, 56, 58};
  for (int i = 0; i < 4; ++i) {
    snprintf(g_oidName[i], sizeof(g_oidName[i]),
        ".1.3.6.1.2.1.25.2.3.1.3.%d", volumeIndices[i]);
    snprintf(g_oidAlloc[i], sizeof(g_oidAlloc[i]),
        ".1.3.6.1.2.1.25.2.3.1.4.%d", volumeIndices[i]);
    snprintf(g_oidTotal[i], sizeof(g_oidTotal[i]),
        ".1.3.6.1.2.1.25.2.3.1.5.%d", volumeIndices[i]);
    snprintf(g_oidUsed[i], sizeof(g_oidUsed[i]),
        ".1.3.6.1.2.1.25.2.3.1.6.%d", volumeIndices[i]);
    cbName[i] = nasSnmp.addStringHandler(
        nas_ip, g_oidName[i], &g_volNames[i]);
    cbAlloc[i] = nasSnmp.addIntegerHandler(
        nas_ip, g_oidAlloc[i], &g_volAlloc[i]);
    cbTotal[i] = nasSnmp.addIntegerHandler(
        nas_ip, g_oidTotal[i], &g_volTotal[i]);
    cbUsed[i] = nasSnmp.addIntegerHandler(
        nas_ip, g_oidUsed[i], &g_volUsed[i]);
  }

  snprintf(oidInterfaceIndex, sizeof(oidInterfaceIndex),
      ".1.3.6.1.2.1.4.20.1.2.%u.%u.%u.%u",
      nas_ip[0], nas_ip[1], nas_ip[2], nas_ip[3]);
  cbInterfaceIndex = nasSnmp.addIntegerHandler(
      nas_ip, oidInterfaceIndex, &nasInterfaceIndex);
  nasCallbacksReady = true;
}
```

Keep the existing volume OIDs and global destination buffers, but never call
`addTimestampHandler()`, `addStringHandler()`, or `addIntegerHandler()` again
after `nasCallbacksReady` becomes true.

- [ ] **Step 3: Discover the interface and register counter callbacks once**

Add:

```cpp
bool discoverNASInterface() {
  initializeNASCallbacks();
  nasInterfaceIndex = -1;
  ValueCallback* request[] = {cbInterfaceIndex};
  if (!requestNAS(request, 1, 2000, 1000) || nasInterfaceIndex <= 0) {
    Serial.println("NAS interface discovery FAILED");
    return false;
  }
  if (!nasCountersReady) {
    snprintf(oidRxOctets, sizeof(oidRxOctets),
        ".1.3.6.1.2.1.31.1.1.1.6.%d", nasInterfaceIndex);
    snprintf(oidTxOctets, sizeof(oidTxOctets),
        ".1.3.6.1.2.1.31.1.1.1.10.%d", nasInterfaceIndex);
    cbRxOctets = nasSnmp.addCounter64Handler(
        nas_ip, oidRxOctets, &nasRxOctets);
    cbTxOctets = nasSnmp.addCounter64Handler(
        nas_ip, oidTxOctets, &nasTxOctets);
    nasCountersReady = true;
  }
  Serial.printf("NAS interface index=%d\n", nasInterfaceIndex);
  return true;
}
```

- [ ] **Step 4: Add counter sampling and reset-safe rate calculation**

Add:

```cpp
bool sampleNASNetwork() {
  if (!nasCountersReady && !discoverNASInterface()) {
    currentNetworkRates = {};
    return false;
  }
  nasRxOctets = UINT64_MAX;
  nasTxOctets = UINT64_MAX;
  ValueCallback* request[] = {cbRxOctets, cbTxOctets};
  const uint32_t sampledAt = millis();
  if (!requestNAS(request, 2, 2100, 1000) ||
      nasRxOctets == UINT64_MAX || nasTxOctets == UINT64_MAX) {
    currentNetworkRates = {};
    Serial.println("NAS network sample FAILED");
    return false;
  }

  const NetworkCounterSample current = {
      nasRxOctets, nasTxOctets, sampledAt, true};
  NetworkRates nextRates = {};
  const bool calculated =
      calculateNetworkRates(previousNetworkSample, current, nextRates);
  previousNetworkSample = current;
  currentNetworkRates = nextRates;
  Serial.printf("NAS counters rx=%llu tx=%llu valid=%d\n",
      static_cast<unsigned long long>(nasRxOctets),
      static_cast<unsigned long long>(nasTxOctets), calculated);
  return calculated;
}
```

On request failure, do not replace `previousNetworkSample`, so the next valid
delta uses its actual longer elapsed time. On a valid counter decrease,
`calculateNetworkRates()` returns false and the current sample becomes the new
baseline.

- [ ] **Step 5: Refactor storage and uptime fetching to reuse callbacks**

Change `fetchNAS()` so it calls `initializeNASCallbacks()`, sends one uptime
request, then sends the same four bounded volume requests as today using the
already registered callback pointers. Preserve the current four small packets
and pool parsing; remove all callback registration and local/static SNMP
objects from `fetchNAS()`.

The request portion becomes:

```cpp
initializeNASCallbacks();
ValueCallback* uptimeRequest[] = {cbUptime};
requestNAS(uptimeRequest, 1, 1000, 1000);

pool_count = 0;
for (int i = 0; i < 4; ++i) {
  ValueCallback* volumeRequest[] = {
      cbName[i], cbAlloc[i], cbTotal[i], cbUsed[i]};
  requestNAS(volumeRequest, 4, 1100 + i, 1000);
  if (g_volNames[i][0] == '\0') continue;

  if (strncmp(g_volNames[i], "/volume", 7) == 0) {
    snprintf(pools[pool_count].name, sizeof(pools[pool_count].name),
             "Vol %s", g_volNames[i] + 7);
  } else {
    copyText(pools[pool_count].name, sizeof(pools[pool_count].name),
             g_volNames[i]);
  }
  const double allocation = g_volAlloc[i];
  pools[pool_count].total_tb =
      (g_volTotal[i] * allocation) / 1099511627776.0;
  pools[pool_count].used_tb =
      (g_volUsed[i] * allocation) / 1099511627776.0;
  pools[pool_count].status = 1;
  pools[pool_count].pct = pools[pool_count].total_tb > 0
      ? static_cast<int>(100.0 * pools[pool_count].used_tb /
                         pools[pool_count].total_tb)
      : 0;
  ++pool_count;
}
```

- [ ] **Step 6: Run the host checks and inspect the patch**

Run:

```sh
sh test/run_dashboard_tests.sh
git diff --check -- epd5in83-hanshow-arduino.ino
```

Expected: model tests pass and the formatting check exits 0. The production
source gate intentionally remains RED until Tasks 5 and 6 are complete.

### Task 5: Render And Partially Refresh Only The Speed Line

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`

- [ ] **Step 1: Switch the display type and add the two small buffers**

Replace the display declaration and add the speed canvas:

```cpp
#include "GxEPD2_583_FastPartial.h"

GxEPD2_BW<GxEPD2_583_FastPartial, 32> display(
    GxEPD2_583_FastPartial(15, 0, 2, 4));
GFXcanvas1 nasSpeedCanvas(NAS_SPEED_WIDTH, NAS_SPEED_HEIGHT);
U8G2_FOR_ADAFRUIT_GFX nasSpeedFont;
uint8_t previousNasSpeed[NAS_SPEED_BUFFER_SIZE];
```

- [ ] **Step 2: Render the two-line footer**

Keep the IP and uptime on the upper line and add the formatted speed text on
the lower line:

```cpp
void drawNASBottomBar() {
  display.fillRect(301, 416, 299, 32, GxEPD_WHITE);
  display.drawLine(300, 416, 300, 447, GxEPD_BLACK);
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  u8g2Fonts.setCursor(308, 428);
  u8g2Fonts.print("IP:192.168.31.105");
  const uint32_t days = g_sysUptime / (100UL * 60 * 60 * 24);
  u8g2Fonts.setCursor(520, 428);
  u8g2Fonts.printf("Up:%lu d", static_cast<unsigned long>(days));

  char rates[48];
  formatNetworkRates(rates, sizeof(rates), currentNetworkRates);
  u8g2Fonts.setCursor(NAS_SPEED_X + 4, 445);
  u8g2Fonts.print(rates);
}
```

- [ ] **Step 3: Add isolated speed-canvas rendering**

Add:

```cpp
void renderNASSpeedCanvas() {
  nasSpeedCanvas.fillScreen(1);
  nasSpeedFont.setFontMode(0);
  nasSpeedFont.setFontDirection(0);
  nasSpeedFont.setForegroundColor(0);
  nasSpeedFont.setBackgroundColor(1);
  nasSpeedFont.setFont(u8g2_font_helvR08_tf);
  char rates[48];
  formatNetworkRates(rates, sizeof(rates), currentNetworkRates);
  nasSpeedFont.setCursor(4, 13);
  nasSpeedFont.print(rates);
}
```

Initialize `nasSpeedFont` with `nasSpeedCanvas` in `setup()`.

- [ ] **Step 4: Add partial refresh and full-refresh resynchronization**

Add:

```cpp
bool refreshNASSpeedWindow() {
  renderNASSpeedCanvas();
  const uint32_t started = millis();
  const bool ok = display.epd2.refreshWindow(
      nasSpeedCanvas.getBuffer(), previousNasSpeed,
      sizeof(previousNasSpeed), NAS_SPEED_X, NAS_SPEED_Y,
      NAS_SPEED_WIDTH, NAS_SPEED_HEIGHT);
  Serial.printf("NAS speed partial ok=%d ms=%lu heap=%u\n", ok,
      static_cast<unsigned long>(millis() - started), ESP.getFreeHeap());
  return ok;
}

void resumePartialModeAfterFullRefresh() {
  renderNASSpeedCanvas();
  memcpy(previousNasSpeed, nasSpeedCanvas.getBuffer(),
         sizeof(previousNasSpeed));
  display.epd2.beginFastMode();
}
```

Call `resumePartialModeAfterFullRefresh()` only after `renderAll()` completes.
This keeps normal full updates on the inherited standard GxEPD2 path and avoids
the gray custom full-frame clock path.

- [ ] **Step 5: Compile the production sketch**

Run:

```sh
sh tools/build_firmware.sh
```

Expected: the build exits 0 and remains within RAM, IRAM, and flash limits.
The production source gate intentionally remains RED until Task 6 removes
deep sleep and adds the final scheduler.

### Task 6: Replace Deep Sleep With Five-Second And Ten-Minute Scheduling

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`
- Modify: `README.md`

- [ ] **Step 1: Add explicit elapsed-time constants and state**

Add:

```cpp
constexpr uint32_t NAS_SPEED_REFRESH_INTERVAL_MS = 5000;
constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 600000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;

uint32_t lastNetworkRefreshMs = 0;
uint32_t lastFullRefreshMs = 0;
uint32_t lastWifiRetryMs = 0;
bool displayReady = false;
bool offlineRatesDisplayed = false;
```

- [ ] **Step 2: Extract a complete dashboard cycle**

Add:

```cpp
void refreshFullDashboard(const char* reason) {
  Serial.printf("Full dashboard refresh reason=%s\n", reason);
  syncTime();
  fetchWeather();
  fetchPVE();
  fetchNAS();
  if (!previousNetworkSample.valid) sampleNASNetwork();
  renderAll();
  resumePartialModeAfterFullRefresh();
  lastFullRefreshMs = millis();
  lastNetworkRefreshMs = lastFullRefreshMs;
  displayReady = true;
  logHeap("after full refresh");
}
```

In `setup()`, initialize Wi-Fi, display, both font adapters, NAS callbacks, and
the first network sample, then call `refreshFullDashboard("startup")`.

```cpp
void setup() {
  Serial.begin(115200);
  connectWifi();
  display.init(115200, true, 2, false);
  u8g2Fonts.begin(display);
  nasSpeedFont.begin(nasSpeedCanvas);
  initializeNASCallbacks();
  sampleNASNetwork();
  refreshFullDashboard("startup");
}
```

- [ ] **Step 3: Implement the persistent loop**

Replace deep sleep and the empty loop with:

```cpp
void loop() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (displayReady && !offlineRatesDisplayed) {
      currentNetworkRates = {};
      refreshNASSpeedWindow();
      offlineRatesDisplayed = true;
    }
    if (intervalElapsed(now, lastWifiRetryMs, WIFI_RETRY_INTERVAL_MS)) {
      lastWifiRetryMs = now;
      connectWifi();
    }
    delay(20);
    return;
  }
  offlineRatesDisplayed = false;

  if (intervalElapsed(now, lastFullRefreshMs, FULL_REFRESH_INTERVAL_MS)) {
    refreshFullDashboard("periodic");
    return;
  }

  if (intervalElapsed(
          now, lastNetworkRefreshMs, NAS_SPEED_REFRESH_INTERVAL_MS)) {
    lastNetworkRefreshMs = now;
    sampleNASNetwork();
    refreshNASSpeedWindow();
  }
  delay(20);
}
```

Delete `display.hibernate()` and `ESP.deepSleep(600e6)`.

- [ ] **Step 4: Update operational documentation**

Change `README.md` to state that the device stays awake, RX/TX is sampled and
partially refreshed every five seconds, and the whole dashboard refreshes every
ten minutes. Document the runtime-discovered IF-MIB interface and the
experimental waveform risk. Update the example serial output accordingly.

- [ ] **Step 5: Run all host, source, formatting, and build checks**

Run:

```sh
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
git diff --check
sh tools/build_firmware.sh
sh tools/build_partial_clock.sh
```

Expected: all four test/source scripts report PASS, `git diff --check` is
silent, and both firmware builds exit 0 within ESP8266 memory limits.

### Task 7: Flash And Verify The Physical Dashboard

**Files:**
- Verify only: `build/output/`
- Device: `/dev/cu.usbserial-1120`

- [ ] **Step 1: Confirm the serial device**

Run: `ls -l /dev/cu.usbserial-1120`

Expected: the character device exists.

- [ ] **Step 2: Upload the production dashboard firmware**

Run:

```sh
arduino-cli upload \
  --port /dev/cu.usbserial-1120 \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --input-dir build/output \
  build/epd5in83-hanshow-arduino
```

Expected: flash write reaches 100%, hash verification succeeds, and the board
hard-resets.

- [ ] **Step 3: Monitor startup and at least six partial samples**

Run:

```sh
arduino-cli monitor \
  --port /dev/cu.usbserial-1120 \
  --config baudrate=115200 \
  --raw
```

Require evidence of:

```text
NAS interface index=15
NAS counters rx=<increasing> tx=<increasing> valid=1
NAS speed partial ok=1 ms=<finite> heap=<stable>
```

Observe at least six five-second cycles. Reject the firmware if the log shows a
watchdog reset, exception, BUSY timeout, failed partial refresh, or progressive
heap decline. Stop the monitor with `Ctrl-C` before reporting.

- [ ] **Step 4: Request physical acceptance**

Ask the user to verify that the background is white, calendar/weather/PVE/NAS
content is present, IP and uptime do not flicker, only the RX/TX line changes,
and residual shadowing remains acceptable.

- [ ] **Step 5: Commit implementation only after physical acceptance**

After the user confirms the screen, inspect `git status` and stage only the
generic driver, production/clock sketches, models, tests, scripts, README, and
this plan. Do not stage `secrets.h` or build output.

```sh
git add README.md dashboard_model.h partial_clock_model.h \
  epd5in83-hanshow-arduino.ino partial_refresh partial_refresh_clock \
  test tools docs/superpowers/plans/2026-08-29-nas-speed-partial-refresh.md
git commit -m "feat: add NAS speed partial refresh dashboard"
```

Expected: one implementation commit on `codex/partial-refresh-clock-test` with
no credentials or generated build artifacts.
