#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
driver_header="$project_root/partial_refresh_clock/GxEPD2_583_FastClock.h"
driver_source="$project_root/partial_refresh_clock/GxEPD2_583_FastClock.cpp"

test -f "$driver_header"
test -f "$driver_source"

rg -q 'class GxEPD2_583_FastClock : public GxEPD2_583' "$driver_header"
rg -q 'beginFastMode' "$driver_header"
rg -q 'refreshClockWindow' "$driver_header"
rg -q 'lut_20_lutc_fast' "$driver_source"

if rg -q 'uint8_t[^;]*(600|WIDTH)[^;]*/ *8[^;]*(448|HEIGHT)' "$driver_header"; then
  echo "Full-screen image buffer is forbidden in the fast clock driver" >&2
  exit 1
fi

echo "Partial clock source checks PASS"
