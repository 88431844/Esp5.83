# PVE Disabled and NAS Enabled Design

## Goal

Keep PVE monitoring disabled while displaying the calendar, weather, and
Synology NAS information on the 600x448 e-paper display.

## Scope

- Keep Wi-Fi, NTP, weather, and NAS data fetching active.
- Keep the calendar and weather panels in the upper half.
- Keep the lower-left PVE panel blank by commenting out `fetchPVE()` and
  `drawPVM()`.
- Enable `fetchNAS()`, `drawNAS()`, and `drawBottomBar()` in the lower-right
  area.
- Keep the large `WiFiUDP` and `SNMPManager` objects in static storage. Keeping
  them on the ESP8266's 4096-byte continuation stack causes a Soft WDT reset
  while SNMP responses are processed.
- Print the NAS pool count and heap usage to the serial console for runtime
  verification.
- Keep all PVE implementation code available for later restoration.

## Verification

1. A source-level check confirms PVE calls are inactive and required NAS calls
   are active.
2. The sketch compiles for ESP8266 NodeMCU.
3. The firmware uploads to `/dev/cu.usbserial-1120`.
4. Startup logs show Wi-Fi, NTP, weather, four NAS pools, and a completed full
   display refresh without a PVE request or watchdog reset.
