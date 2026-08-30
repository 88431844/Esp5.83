#!/bin/sh
set -eu

project_root=$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd)
arduino_cli="${PARTIAL_CLOCK_ARDUINO_CLI:-arduino-cli}"
default_build_root="$project_root/build"

if [ "${PARTIAL_CLOCK_TEST_BUILD_ROOT+x}" = x ]; then
  requested_build_root=$PARTIAL_CLOCK_TEST_BUILD_ROOT
  case "$requested_build_root" in
    ''|'/'|"$project_root")
      echo "Unsafe clock test build root: $requested_build_root" >&2
      exit 1
      ;;
  esac
  if [ -L "$requested_build_root" ] || [ ! -d "$requested_build_root" ]; then
    echo "Clock test build root must be a real directory: $requested_build_root" >&2
    exit 1
  fi
  build_root=$(CDPATH= cd -P -- "$requested_build_root" && pwd)
  temp_root=$(CDPATH= cd -P -- "${TMPDIR:-/tmp}" && pwd)
  case "$build_root" in
    "$temp_root"/*) ;;
    *)
      echo "Clock test build root escapes TMPDIR: $build_root" >&2
      exit 1
      ;;
  esac
else
  if [ -L "$default_build_root" ]; then
    echo "Clock build directory must not be a symlink: $default_build_root" >&2
    exit 1
  fi
  mkdir -p "$default_build_root"
  build_root=$(CDPATH= cd -P -- "$default_build_root" && pwd)
  if [ "$build_root" != "$default_build_root" ]; then
    echo "Clock build directory resolves outside the project: $build_root" >&2
    exit 1
  fi
fi

if [ "$build_root" = / ] || [ "$build_root" = "$project_root" ]; then
  echo "Unsafe clock build root: $build_root" >&2
  exit 1
fi

source_dir="$project_root/partial_refresh_clock"
driver_dir="$project_root/partial_refresh"
sketch_dir="$build_root/partial_refresh_clock"
output_dir="$build_root/partial-clock-output"

for required_input in \
  "$source_dir/partial_refresh_clock.ino" \
  "$driver_dir/GxEPD2_583_FastPartial.h" \
  "$driver_dir/GxEPD2_583_FastPartial.cpp" \
  "$driver_dir/partial_refresh_model.h" \
  "$project_root/partial_clock_model.h" \
  "$project_root/secrets.h"; do
  if [ ! -f "$required_input" ]; then
    echo "Missing clock build input: $required_input" >&2
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
      echo "Clock cleanup target escapes build root: $cleanup_target" >&2
      exit 1
      ;;
  esac
  if [ -L "$cleanup_target" ] || \
      { [ -e "$cleanup_target" ] && [ ! -d "$cleanup_target" ]; }; then
    echo "Clock cleanup target must be a real directory: $cleanup_target" >&2
    exit 1
  fi
  if [ -d "$cleanup_target" ]; then
    resolved_cleanup_target=$(CDPATH= cd -P -- "$cleanup_target" && pwd)
    case "$resolved_cleanup_target" in
      "$build_root"/*) ;;
      *)
        echo "Clock cleanup target resolves outside build root: $cleanup_target" >&2
        exit 1
        ;;
    esac
  fi
done

rm -rf "$sketch_dir" "$output_dir"
mkdir -p "$sketch_dir" "$output_dir"
cp "$source_dir/partial_refresh_clock.ino" \
  "$sketch_dir/partial_refresh_clock.ino"
cp "$driver_dir/GxEPD2_583_FastPartial.h" \
  "$driver_dir/GxEPD2_583_FastPartial.cpp" \
  "$driver_dir/partial_refresh_model.h" \
  "$project_root/partial_clock_model.h" \
  "$project_root/secrets.h" \
  "$sketch_dir/"

"$arduino_cli" compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  --build-path "$output_dir" \
  "$sketch_dir"
