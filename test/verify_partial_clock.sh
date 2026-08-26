#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
driver_header="$project_root/partial_refresh_clock/GxEPD2_583_FastClock.h"
driver_source="$project_root/partial_refresh_clock/GxEPD2_583_FastClock.cpp"
clock_sketch="$project_root/partial_refresh_clock/partial_refresh_clock.ino"

test -f "$driver_header"
test -f "$driver_source"
test -f "$clock_sketch"

rg -q 'class GxEPD2_583_FastClock : public GxEPD2_583' "$driver_header"
rg -q 'beginFastMode' "$driver_header"
rg -q 'refreshClockWindow' "$driver_header"
rg -q 'lut_20_lutc_fast' "$driver_source"

if rg -q 'uint8_t[^;]*(600|WIDTH)[^;]*/ *8[^;]*(448|HEIGHT)' "$driver_header"; then
  echo "Full-screen image buffer is forbidden in the fast clock driver" >&2
  exit 1
fi

rg -q 'configTime\(28800' "$clock_sketch"
rg -q 'u8g2_font_logisoso78_tn' "$clock_sketch"
rg -q 'formatClockTime' "$clock_sketch"
rg -q 'FULL_CLEAN_INTERVAL = 300' "$clock_sketch"
rg -q 'refreshClockWindow' "$clock_sketch"
rg -q 'ESP.getFreeHeap' "$clock_sketch"

echo "Partial clock source checks PASS"
