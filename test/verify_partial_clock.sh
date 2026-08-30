#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
driver_header="$project_root/partial_refresh/GxEPD2_583_FastPartial.h"
driver_source="$project_root/partial_refresh/GxEPD2_583_FastPartial.cpp"
model_header="$project_root/partial_refresh/partial_refresh_model.h"
clock_model_header="$project_root/partial_clock_model.h"
clock_sketch="$project_root/partial_refresh_clock/partial_refresh_clock.ino"
build_script="$project_root/tools/build_partial_clock.sh"
flash_script="$project_root/tools/flash_partial_clock.sh"

test -f "$driver_header"
test -f "$driver_source"
test -f "$model_header"
test -f "$clock_model_header"
test -f "$clock_sketch"
test -f "$build_script"
test -f "$flash_script"

rg -q 'class GxEPD2_583_FastPartial : public GxEPD2_583' "$driver_header"
rg -q 'bool beginFastMode' "$driver_header"
rg -q 'refreshWindow' "$driver_header"
rg -q 'bool waitWhileBusyStatus' "$driver_header"
rg -q 'void prepareFullRefresh' "$driver_header"
rg -q 'bool lastFullRefreshSucceeded' "$driver_header"
rg -q 'void refresh\(bool partial_update_mode' "$driver_header"
rg -q 'void powerOff' "$driver_header"
rg -q 'lut_20_lutc_fast' "$driver_source"
rg -q 'lut_21_lutb_fast' "$driver_source"
rg -q 'lut_22_lutw_fast' "$driver_source"
rg -q 'FAST_PLL = 0x3A' "$driver_source"
rg -q 'setPartialRamArea\(0, 0, WIDTH, HEIGHT\)' "$driver_source"
rg -q 'writePattern' "$driver_source"
rg -q '_power_is_on' "$driver_source"
rg -q '_using_partial_mode' "$driver_source"
rg -q '_hibernating' "$driver_source"
rg -q 'initializeStandardFullMode' "$driver_source"
rg -q 'waitWhileBusyStatus\("StandardFull"' "$driver_source"
rg -q 'waitWhileBusyStatus\("StandardPowerOff"' "$driver_source"
rg -Uq 'if \(_busy >= 0 &&[[:space:]]*!waitWhileBusyStatus\("FastPartialReady", power_on_time\)\)[[:space:]]*\{[[:space:]]*invalidateFastMode\(\);[[:space:]]*return false;[[:space:]]*\}' \
  "$driver_source"
rg -Uq 'if \(!waitWhileBusyStatus\("FastPartial", partial_refresh_time\)\)[[:space:]]*\{[[:space:]]*invalidateFastMode\(\);[[:space:]]*return false;[[:space:]]*\}' \
  "$driver_source"

if rg -q '_waitWhileBusy' "$driver_source"; then
  echo "Fast partial driver must use a status-returning BUSY wait" >&2
  exit 1
fi

if rg -q 'loadNormalLut|drawInitialClock|writeFullClockFrame' "$driver_source"; then
  echo "Clock-specific normal-LUT or full-frame path is forbidden" >&2
  exit 1
fi

if rg -q 'uint8_t[^;]*(600|WIDTH)[^;]*/ *8[^;]*(448|HEIGHT)' "$driver_header"; then
  echo "Full-screen image buffer is forbidden in the fast clock driver" >&2
  exit 1
fi

rg -q 'configTime\(28800' "$clock_sketch"
rg -q 'u8g2_font_logisoso78_tn' "$clock_sketch"
rg -q 'formatClockTime' "$clock_sketch"
rg -q 'FULL_CLEAN_INTERVAL = 600' "$clock_sketch"
rg -q 'FULL_CLEAN_RETRY_INTERVAL_MS = 60000' "$clock_sketch"
rg -q 'lastFullCleanCount' "$clock_sketch"
rg -q 'shouldCleanClock' "$clock_sketch"
rg -q 'fullRefreshRetryReady' "$clock_sketch"
rg -q 'clockFullRecoveryAfterPartialFailure' "$clock_model_header"
rg -q 'clockFullRecoveryAfterPartialFailure' "$clock_sketch"
rg -q 'refreshWindow' "$clock_sketch"
rg -q 'if \(!display\.beginFastMode\(\)\)' "$clock_sketch"
rg -Uq 'display[[:space:]]*\.[[:space:]]*clearToWhite[[:space:]]*\([[:space:]]*\)[[:space:]]*;[[:space:]]*if[[:space:]]*\([[:space:]]*!display[[:space:]]*\.[[:space:]]*lastFullRefreshSucceeded[[:space:]]*\([[:space:]]*\)' \
  "$clock_sketch"
rg -Uq 'clockFullRecoveryAfterPartialFailure[[:space:]]*\([[:space:]]*millis[[:space:]]*\([[:space:]]*\)[[:space:]]*\)[[:space:]]*;[[:space:]]*displayReady[[:space:]]*=[[:space:]]*recoveryState[[:space:]]*\.[[:space:]]*display_ready[[:space:]]*;[[:space:]]*fullCleanRecoveryPending[[:space:]]*=[[:space:]]*recoveryState[[:space:]]*\.[[:space:]]*recovery_pending[[:space:]]*;[[:space:]]*lastFullCleanAttemptMs[[:space:]]*=[[:space:]]*recoveryState[[:space:]]*\.[[:space:]]*retry_anchor[[:space:]]*;[[:space:]]*fullCleanAttemptRecorded[[:space:]]*=[[:space:]]*recoveryState[[:space:]]*\.[[:space:]]*retry_anchor_recorded' \
  "$clock_sketch"
if rg -q 'drawInitialClock' "$clock_sketch"; then
  echo "Clock sketch must use the generic refresh path" >&2
  exit 1
fi
rg -q 'ESP.getFreeHeap' "$clock_sketch"

rg -q 'partial-clock-output' "$build_script"
rg -q 'GxEPD2_583_FastPartial.h' "$build_script"
rg -q 'GxEPD2_583_FastPartial.cpp' "$build_script"
rg -q 'partial_refresh_model.h' "$build_script"
rg -q 'secrets.h' "$build_script"
rg -q 'PARTIAL_CLOCK_TEST_BUILD_ROOT' "$build_script"
rg -q 'PARTIAL_CLOCK_ARDUINO_CLI' "$build_script"

fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/verify-partial-clock.XXXXXX")
trap 'rm -rf "$fixture_dir"' EXIT HUP INT TERM
build_fixture="$fixture_dir/build"
mkdir -p "$build_fixture/partial_refresh_clock" \
  "$build_fixture/partial-clock-output"
build_fixture=$(CDPATH= cd -P -- "$build_fixture" && pwd)
touch "$build_fixture/partial_refresh_clock/GxEPD2_583_FastClock.cpp" \
  "$build_fixture/partial-clock-output/stale-object.o"

cli_stub="$fixture_dir/arduino-cli-stub"
cli_log="$fixture_dir/arduino-cli.log"
expected_cli_log="$fixture_dir/expected-arduino-cli.log"
printf '%s\n' \
  '#!/bin/sh' \
  ': "${PARTIAL_CLOCK_CLI_LOG:?}"' \
  'printf '\''%s\n'\'' "$@" > "$PARTIAL_CLOCK_CLI_LOG"' \
  > "$cli_stub"
chmod +x "$cli_stub"
if ! PARTIAL_CLOCK_TEST_BUILD_ROOT="$build_fixture" \
    PARTIAL_CLOCK_ARDUINO_CLI="$cli_stub" \
    PARTIAL_CLOCK_CLI_LOG="$cli_log" \
    sh "$build_script" >/dev/null; then
  echo "Clock build staging fixture failed" >&2
  exit 1
fi
printf '%s\n' \
  compile \
  --fqbn \
  esp8266:esp8266:nodemcuv2 \
  --build-path \
  "$build_fixture/partial-clock-output" \
  "$build_fixture/partial_refresh_clock" \
  > "$expected_cli_log"
if ! cmp -s "$expected_cli_log" "$cli_log"; then
  echo "Clock build invoked Arduino CLI with unexpected arguments" >&2
  diff -u "$expected_cli_log" "$cli_log" >&2 || true
  exit 1
fi
for staged_source in \
  partial_refresh_clock/partial_refresh_clock.ino \
  partial_refresh/GxEPD2_583_FastPartial.h \
  partial_refresh/GxEPD2_583_FastPartial.cpp \
  partial_refresh/partial_refresh_model.h \
  partial_clock_model.h \
  secrets.h; do
  staged_name=$(basename "$staged_source")
  if ! cmp -s "$project_root/$staged_source" \
      "$build_fixture/partial_refresh_clock/$staged_name"; then
    echo "Clock build did not stage current source: $staged_source" >&2
    exit 1
  fi
done
if [ -e "$build_fixture/partial_refresh_clock/GxEPD2_583_FastClock.cpp" ] || \
    [ -e "$build_fixture/partial-clock-output/stale-object.o" ]; then
  echo "Clock build must clear stale staged sources and output" >&2
  exit 1
fi

outside_dir="$fixture_dir/outside"
mkdir -p "$outside_dir"
printf '%s\n' preserved > "$outside_dir/sentinel"
rm -rf "$build_fixture/partial_refresh_clock"
ln -s "$outside_dir" "$build_fixture/partial_refresh_clock"
if PARTIAL_CLOCK_TEST_BUILD_ROOT="$build_fixture" \
    PARTIAL_CLOCK_ARDUINO_CLI="$cli_stub" \
    PARTIAL_CLOCK_CLI_LOG="$cli_log" \
    sh "$build_script" >/dev/null 2>&1; then
  echo "Clock build accepted a symlink cleanup target" >&2
  exit 1
fi
if [ ! -f "$outside_dir/sentinel" ]; then
  echo "Clock build removed data through a symlink cleanup target" >&2
  exit 1
fi

unsafe_project="$fixture_dir/unsafe-project"
unsafe_outside="$fixture_dir/unsafe-outside"
mkdir -p "$unsafe_project/tools" "$unsafe_outside/partial_refresh_clock"
cp "$build_script" "$unsafe_project/tools/build_partial_clock.sh"
touch "$unsafe_project/secrets.h"
printf '%s\n' preserved > "$unsafe_outside/partial_refresh_clock/sentinel"
ln -s "$unsafe_outside" "$unsafe_project/build"
if sh "$unsafe_project/tools/build_partial_clock.sh" >/dev/null 2>&1; then
  echo "Clock build accepted a symlink production build root" >&2
  exit 1
fi
if [ ! -f "$unsafe_outside/partial_refresh_clock/sentinel" ]; then
  echo "Clock build removed data through a symlink production build root" >&2
  exit 1
fi

if PARTIAL_CLOCK_TEST_BUILD_ROOT=/ \
    PARTIAL_CLOCK_ARDUINO_CLI="$cli_stub" \
    PARTIAL_CLOCK_CLI_LOG="$cli_log" \
    sh "$build_script" >/dev/null 2>&1; then
  echo "Clock build accepted an unsafe test build root" >&2
  exit 1
fi

rg -q 'arduino-cli upload' "$flash_script"
rg -q 'arduino-cli monitor' "$flash_script"

echo "Partial clock source checks PASS"
