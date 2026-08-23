# PVE Disabled and NAS Enabled Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep PVE disabled, restore Synology NAS fetching and rendering, and upload the firmware to the ESP8266.

**Architecture:** Preserve all monitoring implementations and control the active behavior at the `setup()` and `renderAll()` call sites. Verify the intended call state before compiling a correctly named temporary Arduino sketch and uploading it through the USB serial adapter.

**Tech Stack:** Arduino C++, ESP8266 Arduino core 3.1.2, Arduino CLI 1.5.1, GxEPD2, U8g2, ArduinoJson, Arduino SNMP Manager.

---

### Task 1: Verify call-site state

**Files:**
- Create: `test/verify_pve_disabled_nas_enabled.sh`
- Modify: `epd5in83-hanshow-arduino.ino:585-606`

- [ ] Add a check that rejects active `fetchPVE()` or `drawPVM()` calls and
  requires active `fetchNAS()`, `drawNAS()`, and `drawBottomBar()` calls.
- [ ] Run `sh test/verify_pve_disabled_nas_enabled.sh` before the change and
  confirm it fails because NAS is inactive.
- [ ] Restore the three NAS calls and right-side layout lines while keeping the
  PVE calls commented.
- [ ] Move the function-local `WiFiUDP` and `SNMPManager` objects to static
  storage by adding `static` to their declarations.
- [ ] Add serial diagnostics for the NAS pool count and heap usage.
- [ ] Re-run the check and confirm it exits 0.

### Task 2: Compile and upload

**Files:**
- Source: `epd5in83-hanshow-arduino.ino`
- Temporary sketch: `/private/tmp/esp5.83-arduino/epd5in83-hanshow-arduino/epd5in83-hanshow-arduino.ino`
- Build output: `/private/tmp/esp5.83-arduino/output`

- [ ] Copy the updated sketch to the correctly named temporary directory.
- [ ] Run `arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 --output-dir /private/tmp/esp5.83-arduino/output /private/tmp/esp5.83-arduino/epd5in83-hanshow-arduino` and require exit 0.
- [ ] Run `arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 --port /dev/cu.usbserial-1120 --input-dir /private/tmp/esp5.83-arduino/output /private/tmp/esp5.83-arduino/epd5in83-hanshow-arduino` and require successful hash verification.
- [ ] Reset the board and capture the 115200 baud startup log; require four NAS
  pools, a completed full refresh, and no WDT/Exception output.

### Task 3: Final verification

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`
- Create: `test/verify_pve_disabled_nas_enabled.sh`

- [ ] Run the source-level check and `git diff --check`.
- [ ] Review the final diff and repository status.
- [ ] Commit the verified implementation on a `codex/` feature branch.
