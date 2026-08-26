#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 /dev/cu.usbserial-PORT" >&2
  exit 2
fi

port=$1
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sketch_dir="$project_root/build/partial_refresh_clock"
output_dir="$project_root/build/partial-clock-output"

sh "$project_root/tools/build_partial_clock.sh"
arduino-cli upload \
  --port "$port" \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --input-dir "$output_dir" \
  "$sketch_dir"

exec arduino-cli monitor \
  --port "$port" \
  --config baudrate=115200 \
  --raw
