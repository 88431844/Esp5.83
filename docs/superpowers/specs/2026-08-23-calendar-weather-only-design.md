# Calendar and Weather Only Design

## Goal

Temporarily disable all PVE and Synology NAS activity while keeping the
existing calendar and weather panels in the upper half of the 600x448 display.
The lower half remains blank for later restoration of PVE and NAS content.

## Scope

- Keep `connectWifi()`, `syncTime()`, and `fetchWeather()` active.
- Keep `drawCalendar()` and `drawWeather()` active at their current positions
  and dimensions.
- Disable calls to `fetchPVE()`, `fetchNAS()`, `drawPVM()`, `drawNAS()`, and
  `drawBottomBar()` by commenting them out.
- Keep the PVE and SNMP implementations, data structures, and library includes
  unchanged so the features can be restored by uncommenting their calls.
- Keep the horizontal divider at y=224 and limit the vertical divider to the
  upper half so the reserved lower area is visually blank.

## Verification

1. A source-level check confirms no PVE or NAS function is actively called
   from `setup()` or `renderAll()`.
2. The Arduino sketch compiles for the ESP8266 NodeMCU board.
3. The compiled sketch uploads to `/dev/cu.usbserial-1120`.
4. Serial output shows Wi-Fi, NTP, and weather activity without PVE or NAS
   requests before the device enters deep sleep.
