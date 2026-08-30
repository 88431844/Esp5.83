#!/bin/sh
set -eu

project_root=$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd)
arduino_cli="${FIRMWARE_ARDUINO_CLI:-arduino-cli}"
default_build_root="$project_root/build"

if [ "${FIRMWARE_TEST_BUILD_ROOT+x}" = x ]; then
  requested_build_root=$FIRMWARE_TEST_BUILD_ROOT
  case "$requested_build_root" in
    ''|'/'|"$project_root")
      echo "Unsafe firmware test build root: $requested_build_root" >&2
      exit 1
      ;;
  esac
  if [ ! -d "$requested_build_root" ]; then
    echo "Firmware test build root must already exist: $requested_build_root" >&2
    exit 1
  fi
  build_root=$(CDPATH= cd -P -- "$requested_build_root" && pwd)
  temp_root=$(CDPATH= cd -P -- /tmp && pwd)
  case "$build_root" in
    "$temp_root"/*) ;;
    *)
      echo "Firmware test build root escapes the temp directory: $build_root" >&2
      exit 1
      ;;
  esac
else
  mkdir -p "$default_build_root"
  build_root=$(CDPATH= cd -P -- "$default_build_root" && pwd)
  if [ "$build_root" != "$default_build_root" ]; then
    echo "Firmware build directory resolves outside the project: $build_root" >&2
    exit 1
  fi
fi

if [ "$build_root" = / ] || [ "$build_root" = "$project_root" ]; then
  echo "Unsafe firmware build root: $build_root" >&2
  exit 1
fi

sketch_dir="$build_root/epd5in83-hanshow-arduino"
output_dir="$build_root/output"

for required_input in \
  "$project_root/epd5in83-hanshow-arduino.ino" \
  "$project_root/partial_refresh/GxEPD2_583_FastPartial.h" \
  "$project_root/partial_refresh/GxEPD2_583_FastPartial.cpp" \
  "$project_root/partial_refresh/partial_refresh_model.h" \
  "$project_root/dashboard_model.h" \
  "$project_root/secrets.h"; do
  if [ ! -f "$required_input" ]; then
    echo "Missing firmware build input: $required_input" >&2
    exit 1
  fi
done

if ! command -v "$arduino_cli" >/dev/null 2>&1; then
  echo "Arduino CLI is unavailable: $arduino_cli" >&2
  exit 1
fi

for cleanup_target in "$sketch_dir" "$output_dir"; do
  case "$cleanup_target" in
    "$build_root"/*) ;;
    *)
      echo "Firmware cleanup target escapes build root: $cleanup_target" >&2
      exit 1
      ;;
  esac
  if [ -L "$cleanup_target" ] || \
      { [ -e "$cleanup_target" ] && [ ! -d "$cleanup_target" ]; }; then
    echo "Firmware cleanup target must be a real directory: $cleanup_target" >&2
    exit 1
  fi
  if [ -d "$cleanup_target" ]; then
    resolved_cleanup_target=$(CDPATH= cd -P -- "$cleanup_target" && pwd)
    case "$resolved_cleanup_target" in
      "$build_root"/*) ;;
      *)
        echo "Firmware cleanup target resolves outside build root: $cleanup_target" >&2
        exit 1
        ;;
    esac
  fi
done

rm -rf "$sketch_dir" "$output_dir"
mkdir -p "$sketch_dir" "$output_dir"
cp "$project_root/epd5in83-hanshow-arduino.ino" \
  "$sketch_dir/epd5in83-hanshow-arduino.ino"
cp "$project_root/partial_refresh/GxEPD2_583_FastPartial.h" \
  "$project_root/partial_refresh/GxEPD2_583_FastPartial.cpp" \
  "$project_root/partial_refresh/partial_refresh_model.h" \
  "$project_root/dashboard_model.h" \
  "$project_root/secrets.h" \
  "$sketch_dir/"

"$arduino_cli" compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --build-path "$output_dir" \
  --output-dir "$output_dir" \
  "$sketch_dir"
