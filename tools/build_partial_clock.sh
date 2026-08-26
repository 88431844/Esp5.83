#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$project_root/partial_refresh_clock"
sketch_dir="$project_root/build/partial_refresh_clock"
output_dir="$project_root/build/partial-clock-output"

if [ ! -f "$project_root/secrets.h" ]; then
  echo "Missing secrets.h; create it from secrets.example.h first" >&2
  exit 1
fi

mkdir -p "$sketch_dir" "$output_dir"
cp "$source_dir/partial_refresh_clock.ino" \
  "$sketch_dir/partial_refresh_clock.ino"
cp "$source_dir/GxEPD2_583_FastClock.h" \
  "$source_dir/GxEPD2_583_FastClock.cpp" \
  "$project_root/partial_clock_model.h" \
  "$project_root/secrets.h" \
  "$sketch_dir/"

arduino-cli compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --build-path "$output_dir" \
  "$sketch_dir"
