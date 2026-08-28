# NAS Speed Partial Refresh Dashboard Design

## Goal

Integrate the tested differential partial-refresh capability into the existing
PVE and Synology NAS dashboard. Keep the current dashboard layout and static
NAS footer information, sample NAS network traffic every five seconds, and
refresh only the network-speed line. Perform a normal full dashboard refresh
every ten minutes to update all other data and clear accumulated ghosting.

## Scope

- Preserve the calendar, weather, PVE node and VM list, NAS storage pools, PVE
  footer, NAS IP, and NAS uptime.
- Add NAS receive and transmit rates to the lower line of the NAS footer.
- Keep the ESP8266 awake and connected; remove the production deep-sleep step.
- Reuse the repository-local experimental partial-refresh driver without
  modifying the globally installed GxEPD2 library.
- Do not add partial refresh to any region other than the NAS speed line.

## Footer Layout

The NAS footer remains within `x=301..599`, `y=416..447`.

- The upper line displays the existing NAS IP and uptime and changes only
  during a full dashboard refresh.
- The lower line displays `RX:<rate> TX:<rate>`.
- The partial-refresh window is byte-aligned at `x=304`, `y=432`,
  `width=296`, `height=16`.
- A monochrome window buffer therefore occupies 592 bytes. Keep one current
  canvas and one previous-frame buffer, avoiding a full-screen RAM buffer.

The speed canvas is filled white before every redraw, so shorter new values
fully erase longer old values inside the refresh window.

## SNMP Data Flow

At startup, request `ipAdEntIfIndex.<NAS IPv4>` from IP-MIB to discover the
interface associated with the configured NAS management address. The current
NAS reports interface index 15 (`ovs_eth5`), but runtime discovery avoids
depending on that value.

Build the two IF-MIB counter OIDs from the discovered index:

- `ifHCInOctets.<index>` for receive/download bytes.
- `ifHCOutOctets.<index>` for transmit/upload bytes.

Take an initial counter sample as the baseline. Every five seconds, request
both 64-bit counters in one small SNMP GET and calculate each rate from the
counter delta and actual elapsed milliseconds. Format values using B/s, KB/s,
or MB/s so the line remains within its fixed width.

If interface discovery or a sample fails, display `RX:-- TX:--` and retain the
last valid baseline. If either counter decreases because the interface or NAS
restarted, discard that delta and establish a new baseline instead of showing
an invalid spike.

## Display Refresh Flow

The production dashboard continues to use the existing 32-row GxEPD2 page
buffer for the full screen. The display type uses the repository-local driver
subclass so the sketch can switch explicitly between normal full refresh and
differential partial refresh.

1. Connect Wi-Fi, synchronize time, and fetch weather, PVE, NAS storage, NAS
   uptime, interface index, and the first network-counter baseline.
2. Render the complete dashboard with the existing standard GxEPD2 full-update
   path. This is the known white-background path; do not use the clock test's
   custom normal-LUT full-frame routine.
3. Initialize the previous speed buffer from the speed line rendered in the
   full dashboard, then enter stable fast mode with PLL `0x3A`.
4. Every five seconds, sample the two counters, render the speed canvas, and
   issue a differential refresh for only the 296 x 16 speed window.
5. Every ten minutes, fetch all dashboard data again and perform a standard
   full-screen render. Reset the partial-refresh baseline from the newly drawn
   speed line and resume five-second updates.

The local driver must stream a complete controller data frame with unchanged
pixels encoded as `0x77` outside the selected window, while command `0x12`
targets only the aligned speed region. This preserves the complete-frame fix
that prevented blank output without allocating a full-screen image buffer.

## Scheduling And Recovery

- Five-second sampling and ten-minute full refreshes use elapsed `millis()`
  deadlines and remain correct across refresh duration and timer wraparound.
- A Wi-Fi disconnect pauses SNMP sampling, displays unavailable rates when the
  display can be updated, and retries the connection without resetting the
  display state.
- A failed full data fetch keeps the last valid values where possible and
  still performs the periodic normal full refresh to clean the panel.
- Serial logs include interface discovery, counter values, formatted rates,
  partial-refresh duration, full-refresh reason, free heap, and failures.

## Testing And Device Verification

Host tests cover rate calculation, unit formatting, counter reset handling,
five-second scheduling, ten-minute scheduling, and window alignment/bounds.
Source gates require active PVE and NAS calls, 64-bit IF-MIB counters, the
fixed NAS speed window, the stable `0x3A` partial mode, and the absence of deep
sleep.

Verification consists of the existing dashboard and partial-refresh tests,
an ESP8266 Arduino build with memory reporting, upload to
`/dev/cu.usbserial-1120`, and serial monitoring through multiple five-second
samples. Physical acceptance requires a white dashboard background, unchanged
static footer text, only the speed line visibly updating, readable RX/TX
values, and no resets, BUSY timeouts, or progressive heap loss.

## Risk Boundary

The supplied fast LUT remains experimental. Limiting partial refresh to one
small line, using the slower `0x3A` timing, and forcing a normal full refresh
every ten minutes reduce ghosting and panel stress but cannot eliminate the
hardware risk. The implementation remains on the existing test branch until
physical behavior is accepted.
