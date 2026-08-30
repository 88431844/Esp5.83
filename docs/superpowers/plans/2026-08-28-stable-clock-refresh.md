# Stable Clock Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the pre-high-speed `0x3A` clock refresh behavior and perform a full cleaning refresh after every 600 successful partial updates.

**Architecture:** Keep the repository-local low-memory driver and all complete-frame fixes. Change only the fast-mode PLL back to the previously working `0x3A`, and change the cleaning interval from 300 to 600 updates while preserving the post-clean return that prevents an all-unchanged differential frame.

**Tech Stack:** Arduino C++, ESP8266 Arduino Core 3.1.2, GxEPD2 1.6.9, shell source gates, C++17 host tests, Arduino CLI.

---

### Task 1: Restore Stable Refresh And Ten-Minute Cleaning

**Files:**
- Modify: `test/verify_partial_clock.sh`
- Modify: `partial_refresh/GxEPD2_583_FastPartial.cpp`
- Modify: `partial_refresh_clock/partial_refresh_clock.ino`

- [ ] **Step 1: Write failing source checks**

Require the stable PLL and 600-update interval:

```sh
rg -q 'FAST_PLL = 0x3A' partial_refresh/GxEPD2_583_FastPartial.cpp
rg -q 'FULL_CLEAN_INTERVAL = 600' partial_refresh_clock/partial_refresh_clock.ino
```

- [ ] **Step 2: Verify the checks fail**

Run: `sh test/verify_partial_clock.sh`

Expected: non-zero exit because the current source still uses `0x39` and 300 updates.

- [ ] **Step 3: Make the minimal implementation change**

```cpp
constexpr uint8_t FAST_PLL = 0x3A;
constexpr uint32_t FULL_CLEAN_INTERVAL = 600;
```

- [ ] **Step 4: Verify host checks and firmware build**

Run:

```sh
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
git diff --check
sh tools/build_partial_clock.sh
```

Expected: all checks pass; RAM, IRAM, and flash remain below ESP8266 limits.

- [ ] **Step 5: Flash and monitor the device**

Run:

```sh
arduino-cli upload --port /dev/cu.usbserial-1120 --fqbn esp8266:esp8266:nodemcuv2 --input-dir build/partial-clock-output build/partial_refresh_clock
arduino-cli monitor --port /dev/cu.usbserial-1120 --config baudrate=115200 --raw
```

Expected: current `HH:MM:SS` advances continuously, refresh calls remain stable, and no reset, exception, BUSY timeout, or heap decline appears.
