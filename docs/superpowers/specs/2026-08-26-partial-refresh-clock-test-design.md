# 5.83-inch Partial Refresh Clock Test Design

## Goal

Validate the fast partial-refresh method from the supplied `GxEPD2_583` replacement on the existing ESP8266 and 600 x 448 monochrome panel. The test firmware displays a centered `HH:MM:SS` digital clock and refreshes only its fixed window once per second.

The work lives only on `codex/partial-refresh-clock-test`. It does not replace the dashboard firmware or modify the globally installed Arduino libraries.

## Driver Design

Vendor a test-specific `GxEPD2_583`-compatible driver in the repository. Reuse the supplied UC8159/IL0371 fast LUT and changed/unchanged pixel encoding, but do not copy its two 33,600-byte full-screen buffers. Keep previous and current image data only for the byte-aligned clock window, limiting additional RAM to the size of two small monochrome window buffers.

The driver supports the operations needed by this test: controller initialization, an initial full-screen white refresh, writing the clock window, fast window refresh, power-off, and hibernation. Fast mode is explicit and local to the test driver.

## Firmware Flow

1. Start serial output at 115200 baud and connect using the existing local Wi-Fi credentials.
2. Synchronize local time for `Asia/Shanghai` with the existing NTP sources.
3. Initialize the display and perform one slow full-screen white refresh.
4. Draw a large, centered, monospaced `HH:MM:SS` clock inside a fixed byte-aligned window.
5. On each new wall-clock second, redraw and fast-refresh only that window.
6. Log the displayed value, refresh duration, free heap, and running partial-refresh count.
7. Perform a full clean refresh periodically to limit accumulated ghosting, then resume partial refreshes.

The ESP8266 remains awake for this test. Wi-Fi reconnect and NTP resynchronization are retried without blocking display updates indefinitely.

## Verification

Host tests cover zero-padded `HH:MM:SS` formatting and the clock-window alignment and bounds. A source check ensures the test uses the vendored driver and partial window rather than the production dashboard path. The final checks are the full host test suite, an ESP8266 Arduino compile with memory usage, upload to `/dev/cu.usbserial-1120`, and serial evidence of repeated one-second partial refreshes without resets or heap collapse.

## Safety And Scope

The supplied driver warns that its waveform is experimental and panel damage is the user's responsibility. The test therefore includes periodic full refreshes and keeps all experimental code isolated on the test branch. No credentials are committed, no global Arduino library files are overwritten, and no production dashboard behavior is changed.
