#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sketch="${1:-epd5in83-hanshow-arduino.ino}"
build_script="$project_root/tools/build_firmware.sh"

if [ ! -f "$sketch" ]; then
  echo "Sketch source is missing: $sketch" >&2
  exit 1
fi

fixture_dir=$(mktemp -d /tmp/verify-pve-dashboard.XXXXXX)
trap 'rm -rf "$fixture_dir"' EXIT HUP INT TERM
active_source="$fixture_dir/active-source.cpp"
code_source="$fixture_dir/code-source.cpp"

strip_cpp_source() {
  strip_strings=$1
  awk -v strip_strings="$strip_strings" '
    function filtered(text) {
      return strip_strings ? sprintf("%*s", length(text), "") : text
    }
    BEGIN {
      in_block_comment = 0
      in_line_comment = 0
      in_raw_string = 0
      raw_terminator = ""
      quote = ""
      escaped = 0
      single_quote = sprintf("%c", 39)
    }
    {
      output = ""
      if (in_line_comment) {
        in_line_comment = length($0) > 0 &&
            substr($0, length($0), 1) == "\\"
        print output
        next
      }
      for (i = 1; i <= length($0); ++i) {
        character = substr($0, i, 1)
        next_character = substr($0, i + 1, 1)

        if (in_raw_string) {
          remaining = substr($0, i)
          terminator_position = index(remaining, raw_terminator)
          if (terminator_position == 0) {
            output = output filtered(remaining)
            i = length($0)
            continue
          }
          consumed = terminator_position + length(raw_terminator) - 1
          output = output filtered(substr(remaining, 1, consumed))
          i += consumed - 1
          in_raw_string = 0
          raw_terminator = ""
          continue
        }

        if (in_block_comment) {
          if (character == "*" && next_character == "/") {
            in_block_comment = 0
            ++i
          }
          continue
        }

        if (quote != "") {
          output = output (strip_strings ? " " : character)
          if (escaped) {
            escaped = 0
          } else if (character == "\\") {
            escaped = 1
          } else if (character == quote) {
            quote = ""
          }
          continue
        }

        if (character == "/" && next_character == "*") {
          in_block_comment = 1
          ++i
          continue
        }
        if (character == "/" && next_character == "/") {
          in_line_comment = substr($0, length($0), 1) == "\\"
          break
        }

        if (character == "R" && next_character == "\"") {
          delimiter = ""
          valid_delimiter = 1
          for (j = i + 2; j <= length($0); ++j) {
            delimiter_character = substr($0, j, 1)
            if (delimiter_character == "(") break
            if (delimiter_character ~ /[[:space:]()\\]/ ||
                length(delimiter) >= 16) {
              valid_delimiter = 0
              break
            }
            delimiter = delimiter delimiter_character
          }
          if (valid_delimiter && j <= length($0) &&
              delimiter_character == "(") {
            output = output filtered(substr($0, i, j - i + 1))
            raw_terminator = ")" delimiter "\""
            in_raw_string = 1
            i = j
            continue
          }
        }

        if (character == "\"" || character == single_quote) {
          quote = character
          escaped = 0
          output = output (strip_strings ? " " : character)
          continue
        }

        output = output character
      }
      print output
      if (quote != "" &&
          !(length($0) > 0 && substr($0, length($0), 1) == "\\")) {
        quote = ""
      }
      if (quote != "") escaped = 0
    }
  ' "$sketch"
}

strip_cpp_source 0 > "$active_source"
strip_cpp_source 1 > "$code_source"

build_fixture="$fixture_dir/build"
mkdir -p "$build_fixture/epd5in83-hanshow-arduino" \
  "$build_fixture/output"
build_fixture=$(CDPATH= cd -P -- "$build_fixture" && pwd)
touch "$build_fixture/epd5in83-hanshow-arduino/GxEPD2_583_FastClock.cpp" \
  "$build_fixture/output/stale-object.o"
cli_stub="$fixture_dir/arduino-cli-stub"
cli_log="$fixture_dir/arduino-cli.log"
expected_cli_log="$fixture_dir/expected-arduino-cli.log"
printf '%s\n' \
  '#!/bin/sh' \
  ': "${FIRMWARE_CLI_LOG:?}"' \
  'printf '\''%s\n'\'' "$@" > "$FIRMWARE_CLI_LOG"' \
  > "$cli_stub"
chmod +x "$cli_stub"
if ! FIRMWARE_TEST_BUILD_ROOT="$build_fixture" \
    FIRMWARE_ARDUINO_CLI="$cli_stub" FIRMWARE_CLI_LOG="$cli_log" \
    sh "$build_script" >/dev/null; then
  echo "Production build staging fixture failed" >&2
  exit 1
fi
printf '%s\n' \
  compile \
  --fqbn \
  esp8266:esp8266:nodemcuv2 \
  --build-path \
  "$build_fixture/output" \
  --output-dir \
  "$build_fixture/output" \
  "$build_fixture/epd5in83-hanshow-arduino" \
  > "$expected_cli_log"
if ! cmp -s "$expected_cli_log" "$cli_log"; then
  echo "Production build invoked Arduino CLI with unexpected arguments" >&2
  diff -u "$expected_cli_log" "$cli_log" >&2 || true
  exit 1
fi
for staged_source in \
  epd5in83-hanshow-arduino.ino \
  dashboard_model.h \
  GxEPD2_583_DeepBlack.h \
  GxEPD2_583_DeepBlack.cpp \
  secrets.h; do
  staged_name=$(basename "$staged_source")
  if ! cmp -s "$project_root/$staged_source" \
      "$build_fixture/epd5in83-hanshow-arduino/$staged_name"; then
    echo "Production build did not stage current source: $staged_source" >&2
    exit 1
  fi
done
if [ -e "$build_fixture/epd5in83-hanshow-arduino/GxEPD2_583_FastClock.cpp" ] || \
    [ -e "$build_fixture/output/stale-object.o" ]; then
  echo "Production build must clear stale staged sources and output" >&2
  exit 1
fi

for call in fetchPVE drawPVE drawPVEBottomBar fetchNAS drawNAS drawNASBottomBar; do
  if ! rg -q "^[[:space:]]*${call}[[:space:]]*\\([^;]*\\)[[:space:]]*;" \
      "$code_source"; then
    echo "Required active call ${call}() is missing from $sketch" >&2
    exit 1
  fi
done

for required_text in \
  'u8g2_font_wqy16_t_gb2312' \
  'u8g2_font_helvB14_tf' \
  'chineseWeekdayLabel' \
  'formatChineseCalendarHeader' \
  'formatChineseWeatherHeader' \
  'chineseWeatherCondition' \
  'centerTextInRect'; do
  if ! rg -qF "$required_text" "$code_source"; then
    echo "Chinese calendar/weather renderer is missing: $required_text" >&2
    exit 1
  fi
done

if ! rg -q 'MAX_VISIBLE_PVE_VMS[[:space:]]*=[[:space:]]*5' \
    "$project_root/dashboard_model.h"; then
  echo "PVE dashboard must reserve exactly five visible VM rows" >&2
  exit 1
fi

for dashboard_layout in \
  'drawBoldUTF8\(106,[[:space:]]*431,[[:space:]]*"内存使用"\)' \
  'display\.drawRect\(105,[[:space:]]*435,[[:space:]]*190,[[:space:]]*10' \
  'drawBoldUTF8\([^,]+,[[:space:]]*y \+ 43,[[:space:]]*"虚拟机"\)' \
  'drawBoldUTF8\([^,]+,[[:space:]]*y \+ 43,[[:space:]]*"核心数"\)' \
  'drawBoldUTF8\([^,]+,[[:space:]]*y \+ 43,[[:space:]]*"内存"\)' \
  '"运行时间%lu天"'; do
  if ! rg -q "$dashboard_layout" "$active_source"; then
    echo "Requested dashboard layout is missing: $dashboard_layout" >&2
    exit 1
  fi
done

for obsolete_text in \
  'drawHeader(x, y, w, "Calendar")' \
  'drawHeader(x, y, w, "Weather")' \
  '今天天气 %d月%d日' \
  'daily_count' \
  'const char* days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"}' \
  'u8g2Fonts.setCursor(cx + 4, cy)'; do
  if rg -qF "$obsolete_text" "$active_source"; then
    echo "Obsolete calendar/weather renderer remains: $obsolete_text" >&2
    exit 1
  fi
done

for endpoint in \
  '/api2/json/nodes' \
  '/api2/json/cluster/resources?type=vm' \
  '/agent/network-get-interfaces'; do
  if ! rg -qF "$endpoint" "$active_source"; then
    echo "Required PVE endpoint is missing: $endpoint" >&2
    exit 1
  fi
done

for implementation in \
  '#include "dashboard_model.h"' \
  '#include "secrets.h"' \
  'seekPVEDataArray(stream)' \
  'insertPreferredVM(vms, retainedCount, MAX_PVE_VMS, candidate);' \
  'vm_count = static_cast<int>(committedVMCount(ok, retainedCount));' \
  'if (strcmp(sourceNode, pve_node.name) != 0) continue;' \
  'client.setFingerprint(PVE_CERT_FINGERPRINT);' \
  'memoryPercent(pve_node.mem_bytes, pve_node.maxmem_bytes)'; do
  if ! rg -qF "$implementation" "$active_source"; then
    echo "Required PVE implementation is missing: $implementation" >&2
    exit 1
  fi
done

if rg -qF 'GxEPD2_583_FastPartial' "$active_source"; then
  echo "Production dashboard must not include the FastPartial driver" >&2
  exit 1
fi

for source_pattern in \
  'GxEPD2_BW[[:space:]]*<[[:space:]]*GxEPD2_583_DeepBlack[[:space:]]*,[[:space:]]*32[[:space:]]*>' \
  '(^|[^[:alnum:]_])FULL_REFRESH_INTERVAL_MS[[:space:]]*=[[:space:]]*3600000' \
  '(^|[^[:alnum:]_])WIFI_RETRY_INTERVAL_MS[[:space:]]*=[[:space:]]*30000' \
  '(^|[^[:alnum:]_])FULL_RECOVERY_BACKOFF_MS[[:space:]]*=[[:space:]]*60000'; do
  rg -q "$source_pattern" "$code_source" || {
    echo "Required hourly full-refresh implementation is missing: $source_pattern" >&2
    exit 1
  }
done

for deep_driver_pattern in \
  'class GxEPD2_583_DeepBlack : public GxEPD2_583' \
  '_writeData(0x3C)' \
  '_writeData(0x1E)' \
  '_Update_DeepBlack'; do
  if ! rg -qF "$deep_driver_pattern" \
      "$project_root/GxEPD2_583_DeepBlack.h" \
      "$project_root/GxEPD2_583_DeepBlack.cpp"; then
    echo "Deep-black full-refresh driver is missing: $deep_driver_pattern" >&2
    exit 1
  fi
done

for conditioning_pattern in \
  'renderSolidScreen[[:space:]]*\([[:space:]]*GxEPD_WHITE' \
  'renderSolidScreen[[:space:]]*\([[:space:]]*GxEPD_BLACK' \
  'renderSolidScreen[[:space:]]*\([[:space:]]*GxEPD_WHITE,[[:space:]]*"FINAL WHITE"' \
  'conditionPanelBeforeDashboard[[:space:]]*\(' \
  'strcmp[[:space:]]*\([[:space:]]*reason,[[:space:]]*"startup"[[:space:]]*\)'; do
  if ! rg -q "$conditioning_pattern" "$active_source"; then
    echo "Startup panel-conditioning sequence is missing: $conditioning_pattern" >&2
    exit 1
  fi
done

for scheduler_pattern in \
  'intervalElapsed[[:space:]]*\([[:space:]]*now,[[:space:]]*lastFullRefreshMs' \
  'fullRefreshGuardOpen[[:space:]]*\(' \
  'recordFullAttemptCompletion[[:space:]]*\(' \
  'recoverCachedDashboard[[:space:]]*\(' \
  'recoveryPending[[:space:]]*=[[:space:]]*!ok' \
  'WiFiEventHandler[[:space:]]+wifiDisconnectHandler' \
  'WiFi[[:space:]]*\.[[:space:]]*onStationModeDisconnected[[:space:]]*\(' \
  'wifiRetryAnchorAfterDisconnectEvent[[:space:]]*\(' \
  'markWiFiConnectedObserved[[:space:]]*\(' \
  'consumeWiFiDisconnectEvent[[:space:]]*\(' \
  'bool[[:space:]]+syncTime[[:space:]]*\('; do
  rg -q "$scheduler_pattern" "$code_source" || {
    echo "Required persistent scheduler behavior is missing: $scheduler_pattern" >&2
    exit 1
  }
done

disconnect_callback=$(awk '
  /^void onWiFiStationDisconnected\(/ { in_function = 1 }
  in_function { print }
  in_function && /^}/ { exit }
' "$code_source")
for callback_pattern in \
  'classifyWiFiDisconnectEvent[[:space:]]*\([[:space:]]*wifiConnectedObserved[[:space:]]*\)' \
  'wifiConnectedObserved[[:space:]]*=[[:space:]]*transition[[:space:]]*\.[[:space:]]*connected_observed' \
  'wifiDisconnectEventRaised[[:space:]]*=[[:space:]]*true' \
  'wifiOfflineDisconnectEventRaised[[:space:]]*=[[:space:]]*true'; do
  printf '%s\n' "$disconnect_callback" | rg -q "$callback_pattern" || {
    echo "WiFi disconnect callback state transition is missing: $callback_pattern" >&2
    exit 1
  }
done

if printf '%s\n' "$disconnect_callback" | rg -q 'lastWifiRetryMs'; then
  echo "WiFi event callback must not re-anchor offline connection attempts" >&2
  exit 1
fi

cached_recovery=$(awk '
  /^bool recoverCachedDashboard\(/ { in_function = 1 }
  in_function { print }
  in_function && /^}/ { exit }
' "$code_source")
for cached_pattern in \
  'renderAll[[:space:]]*\(' \
  'recordFullAttemptCompletion[[:space:]]*\('; do
  printf '%s\n' "$cached_recovery" | rg -q "$cached_pattern" || {
    echo "Cached full recovery is missing: $cached_pattern" >&2
    exit 1
  }
done

if printf '%s\n' "$cached_recovery" | \
    rg -q 'syncTime[[:space:]]*\(|fetchWeather[[:space:]]*\(|fetchPVE[[:space:]]*\(|fetchNAS[[:space:]]*\('; then
  echo "Cached full recovery must not fetch remote metadata" >&2
  exit 1
fi

if rg -q 'lastRecoveryAttemptMs' "$code_source"; then
  echo "Legacy recovery-only guard must not bypass the unified full guard" >&2
  exit 1
fi

handler_registration_line=$(rg -n \
  'WiFi[[:space:]]*\.[[:space:]]*onStationModeDisconnected[[:space:]]*\(' \
  "$code_source" | head -n 1 | cut -d: -f1)
first_connect_line=$(rg -n \
  '^[[:space:]]+connectWifi[[:space:]]*\([[:space:]]*\)[[:space:]]*;' \
  "$code_source" | head -n 1 | cut -d: -f1)
if [ "$handler_registration_line" -ge "$first_connect_line" ]; then
  echo "WiFi disconnect handler must be retained before the initial connection" >&2
  exit 1
fi

for forbidden_pattern in \
  'refreshWindow[[:space:]]*\(' \
  'beginFastMode[[:space:]]*\(' \
  'sampleNASNetwork[[:space:]]*\(' \
  'formatCompactNetworkRates[[:space:]]*\(' \
  'NAS_SPEED_REFRESH_INTERVAL_MS'; do
  if rg -q "$forbidden_pattern" "$code_source"; then
    echo "Production dashboard still contains NAS speed/partial behavior: $forbidden_pattern" >&2
    exit 1
  fi
done

if ! rg -q 'forecast_hours=8' "$active_source" || \
    ! rg -q 'hourly_count' "$code_source"; then
  echo "Eight-hour weather forecast data and rendering must remain active" >&2
  exit 1
fi

for forbidden_sleep_pattern in \
  'display[[:space:]]*\.[[:space:]]*hibernate[[:space:]]*\(' \
  'ESP[[:space:]]*\.[[:space:]]*deepSleep[[:space:]]*\('; do
  if rg -q "$forbidden_sleep_pattern" "$code_source"; then
    echo "Persistent hourly dashboard must not sleep: $forbidden_sleep_pattern" >&2
    exit 1
  fi
done

if rg -qF 'PVEAPIToken=' "$active_source"; then
  echo "PVE API token must not be embedded in tracked sketch source" >&2
  exit 1
fi

for secret_name in WIFI_SSID WIFI_PASS SNMP_COMMUNITY PVE_TOKEN; do
  if rg -q "^[[:space:]]*const[[:space:]]+char[[:space:]]*\\*[[:space:]]*${secret_name}[[:space:]]*=" \
      "$code_source"; then
    echo "${secret_name} must not be embedded in tracked sketch source" >&2
    exit 1
  fi
  if [ -f secrets.h ] && ! rg -q "^const char\\* ${secret_name}[[:space:]]*=" secrets.h; then
    echo "${secret_name} is missing from local secrets.h" >&2
    exit 1
  fi
  if ! rg -q "^const char\\* ${secret_name}[[:space:]]*=" secrets.example.h; then
    echo "${secret_name} is missing from secrets.example.h" >&2
    exit 1
  fi
done

for placeholder_declaration in \
  'const char* WIFI_SSID = "your-wifi-ssid";' \
  'const char* WIFI_PASS = "your-wifi-password";' \
  'const char* SNMP_COMMUNITY = "your-snmp-community";' \
  'const char* PVE_TOKEN = "PVEAPIToken=user@realm!token-id=token-secret";'; do
  if ! rg -qF "$placeholder_declaration" secrets.example.h; then
    echo "secrets.example.h must contain placeholders only" >&2
    exit 1
  fi
done

if ! git check-ignore -q secrets.h; then
  echo "secrets.h must be ignored by git" >&2
  exit 1
fi

if rg -qF 'vm.node' "$code_source" || rg -qF 'candidate.node' "$code_source"; then
  echo "VM entries must reuse the selected canonical PVE node name" >&2
  exit 1
fi

if ! rg -qF 'PVE_HOST, PVE_PORT, pve_node.name, vm.vmid);' "$code_source"; then
  echo "Guest Agent URL must use the selected canonical PVE node name" >&2
  exit 1
fi

for declaration_kind in wifi_udp snmp_manager; do
  if [ "$declaration_kind" = wifi_udp ]; then
    declaration='static WiFiUDP nasUdp;'
  else
    declaration='static SNMPManager nasSnmp(SNMP_COMMUNITY);'
  fi
  if ! awk -v declaration_kind="$declaration_kind" '
      BEGIN { brace_depth = 0; found = 0 }
      {
        if (brace_depth == 0 &&
            ((declaration_kind == "wifi_udp" &&
              $0 ~ /^[[:space:]]*static[[:space:]]+WiFiUDP[[:space:]]+nasUdp[[:space:]]*;[[:space:]]*$/) ||
             (declaration_kind == "snmp_manager" &&
              $0 ~ /^[[:space:]]*static[[:space:]]+SNMPManager[[:space:]]+nasSnmp[[:space:]]*\([[:space:]]*SNMP_COMMUNITY[[:space:]]*\)[[:space:]]*;[[:space:]]*$/))) {
          found = 1
        }
        for (i = 1; i <= length($0); ++i) {
          character = substr($0, i, 1)
          if (character == "{") ++brace_depth
          else if (character == "}") --brace_depth
        }
      }
      END { exit found ? 0 : 1 }
    ' "$code_source"; then
    echo "Required top-level persistent NAS declaration is missing: $declaration" >&2
    exit 1
  fi
done

echo "Hourly PVE dashboard and NAS monitor calls are active"
