# PVE Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Add a memory-bounded PVE node and VM dashboard to the lower-left display area and verify it on the connected ESP8266.

**Architecture:** Put data-only structs and deterministic helpers in a host-testable header. Keep HTTPS/ArduinoJson and GxEPD2 integration in the existing sketch, using filtered sequential requests and fixed-capacity arrays.

**Tech Stack:** Arduino C++, ESP8266 Arduino core 3.1.2, ArduinoJson 7.4.3, GxEPD2 1.6.9, Arduino CLI 1.5.1, host C++17 tests.

---

### Task 1: Dashboard model

**Files:**
- Create: `dashboard_model.h`
- Create: `test/test_dashboard_model.cpp`
- Create: `test/run_dashboard_tests.sh`

- [x] Add a failing host test that constructs stopped/running VMs out of order,
  calls `sortVMs`, and expects running VMs first with each group ordered by VMID.
- [x] Add failing cases for rejecting loopback/link-local IPv4 and clamping node
  memory percentages to 0..100.
- [x] Run `sh test/run_dashboard_tests.sh`; require failure because
  `dashboard_model.h` does not exist.
- [x] Define fixed-size `PveNodeInfo` and `VMInfo` structs plus inline
  `sortVMs`, `isUsableGuestIPv4`, and `memoryPercent` helpers.
- [x] Re-run `sh test/run_dashboard_tests.sh`; require `PASS`.

### Task 2: PVE API integration

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`
- Replace: `test/verify_pve_disabled_nas_enabled.sh`
  with `test/verify_pve_dashboard.sh`

- [x] Add a source-level test requiring active `fetchPVE`, `drawPVE`, and both
  PVE/NAS footer calls, plus `/nodes`, `cluster/resources?type=vm`, and
  `agent/network-get-interfaces` endpoint strings.
- [x] Run the source test and confirm it fails on the disabled legacy PVE calls.
- [x] Replace the legacy PVE function with three sequential filtered requests:
  node status, streamed VM resources, and per-running-VM Guest Agent networking.
- [x] Validate the PVE TLS certificate fingerprint before adding the API token
  header, and retain only the globally preferred fixed-capacity VM entries.
- [x] Copy data into the fixed model, call `sortVMs`, and add heap diagnostics
  around every PVE phase.
- [x] Re-run both test scripts and require `PASS`.

### Task 3: PVE rendering

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino`

- [x] Draw a lower-left PVE panel with column labels `VM`, `IP`, `C`, and `MEM`.
- [x] Draw up to seven VM rows with filled/hollow status indicators and the
  fields fixed by Task 1.
- [x] Split the footer across both halves: PVE IP plus node memory on the left,
  existing Synology IP plus uptime on the right.
- [x] Enable `fetchPVE()` and PVE rendering in `setup()`/`renderAll()` and keep
  the existing NAS calls enabled.
- [x] Run model tests, source checks, and `git diff --check`; require success.

### Task 4: Firmware and hardware verification

**Files:**
- Source: `epd5in83-hanshow-arduino.ino`
- Temporary sketch: `/private/tmp/esp5.83-pve/epd5in83-hanshow-arduino/`
- Build output: `/private/tmp/esp5.83-pve/output/`

- [x] Compile with `arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2` and
  require exit 0 plus reported RAM/flash usage below board limits.
- [x] Identify the ESP8266 serial port from startup output, upload the compiled
  firmware, and require successful flash hash verification.
- [x] Monitor 115200-baud serial through one complete wake/fetch/render cycle.
- [x] If an exception, watchdog reset, JSON allocation failure, or unsafe heap
  floor occurs, capture the failing stage, add a reproducing check where
  possible, reduce the request/document lifetime, rebuild, upload, and repeat.
- [x] Require a final cycle with PVE, NAS, and display completion and no crash.

### Task 5: Documentation and final checks

**Files:**
- Modify: `README.md`

- [x] Document enabled PVE fields, VM ordering, Guest Agent IP behavior, and the
  official Waveshare V1 partial-refresh conclusion.
- [x] Run all tests, compile once more, inspect `git diff`, and confirm no
  live credentials remain in tracked files and no unrelated files were changed.
