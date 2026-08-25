#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sketch_dir="$project_root/build/epd5in83-hanshow-arduino"
output_dir="$project_root/build/output"

if [ ! -f "$project_root/secrets.h" ]; then
  echo "Missing secrets.h; create it from secrets.example.h first" >&2
  exit 1
fi

mkdir -p "$sketch_dir" "$output_dir"
cp "$project_root/epd5in83-hanshow-arduino.ino" \
  "$sketch_dir/epd5in83-hanshow-arduino.ino"
cp "$project_root/dashboard_model.h" "$sketch_dir/dashboard_model.h"
cp "$project_root/secrets.h" "$sketch_dir/secrets.h"

arduino-cli compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --build-path "$output_dir" \
  "$sketch_dir"
