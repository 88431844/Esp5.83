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
  partial_refresh/GxEPD2_583_FastPartial.h \
  partial_refresh/GxEPD2_583_FastPartial.cpp \
  partial_refresh/partial_refresh_model.h \
  dashboard_model.h \
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
  'formatChineseWeatherSummary' \
  'centerTextInRect'; do
  if ! rg -qF "$required_text" "$code_source"; then
    echo "Chinese calendar/weather renderer is missing: $required_text" >&2
    exit 1
  fi
done

if ! rg -U -q \
    'drawUTF8\(x \+ 5, y \+ 49, buf\);\n[[:space:]]*u8g2Fonts\.setFont\(u8g2_font_helvR08_tf\);' \
    "$code_source"; then
  echo "Weather charts must restore the compact font after the Chinese summary" >&2
  exit 1
fi

for obsolete_text in \
  'drawHeader(x, y, w, "Calendar")' \
  'drawHeader(x, y, w, "Weather")' \
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

if ! rg -q \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*"GxEPD2_583_FastPartial\.h"' \
    "$active_source"; then
  echo "Required active FastPartial driver include is missing" >&2
  exit 1
fi

for source_pattern in \
  'GxEPD2_BW[[:space:]]*<[[:space:]]*GxEPD2_583_FastPartial[[:space:]]*,[[:space:]]*32[[:space:]]*>' \
  '(^|[^[:alnum:]_])NAS_SPEED_REFRESH_INTERVAL_MS[[:space:]]*=[[:space:]]*5000' \
  '(^|[^[:alnum:]_])FULL_REFRESH_INTERVAL_MS[[:space:]]*=[[:space:]]*600000' \
  '(^|[^[:alnum:]_])WIFI_RETRY_INTERVAL_MS[[:space:]]*=[[:space:]]*30000' \
  '(^|[^[:alnum:]_])FULL_RECOVERY_BACKOFF_MS[[:space:]]*=[[:space:]]*60000' \
  '(^|[^[:alnum:]_])NAS_NETWORK_SAMPLE_MAX_AGE_MS[[:space:]]*=[[:space:]]*60000' \
  '(^|[^[:alnum:]_])ifHCInOctets([^[:alnum:]_]|$)' \
  '(^|[^[:alnum:]_])ifHCOutOctets([^[:alnum:]_]|$)' \
  '(^|[^[:alnum:]_])ipAdEntIfIndex([^[:alnum:]_]|$)'; do
  rg -q "$source_pattern" "$code_source" || {
    echo "Required NAS speed implementation is missing: $source_pattern" >&2
    exit 1
  }
done

for scheduler_pattern in \
  'chooseConnectedDashboardAction[[:space:]]*\(' \
  'shouldInvalidateNetworkSample[[:space:]]*\(' \
  'prepareFullRefresh[[:space:]]*\(' \
  'lastFullRefreshSucceeded[[:space:]]*\(' \
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
  'currentNetworkRates[[:space:]]*=[[:space:]]*\{\}' \
  'renderAll[[:space:]]*\(' \
  'resumePartialModeAfterFullRefresh[[:space:]]*\(' \
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

for call_pattern in \
  '^[[:space:]]*[[:alpha:]_][[:alnum:]_]*[[:space:]]*=[[:space:]]*nasSnmp[[:space:]]*\.[[:space:]]*addCounter64Handler[[:space:]]*\(' \
  '^[[:space:]]*(const[[:space:]]+bool[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]*=[[:space:]]*)?display[[:space:]]*\.[[:space:]]*epd2[[:space:]]*\.[[:space:]]*refreshWindow[[:space:]]*\(' \
  '^[[:space:]]*(const[[:space:]]+bool[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]*=[[:space:]]*)?display[[:space:]]*\.[[:space:]]*epd2[[:space:]]*\.[[:space:]]*beginFastMode[[:space:]]*\('; do
  rg -q "$call_pattern" "$code_source" || {
    echo "Required active NAS speed call is missing: $call_pattern" >&2
    exit 1
  }
done

for rate_buffer in upload download; do
  rate_buffer_count=$(rg -c \
    "char[[:space:]]+${rate_buffer}\\[32\\]" "$code_source" || true)
  if [ "${rate_buffer_count:-0}" -ne 2 ]; then
    echo "NAS speed rendering must use two ${rate_buffer}[32] line buffers" >&2
    exit 1
  fi
done

if rg -q '(^|[^[:alnum:]_])ESP[[:space:]]*\.[[:space:]]*deepSleep[[:space:]]*\(' \
    "$code_source"; then
  echo "Persistent partial-refresh dashboard must not enter deep sleep" >&2
  exit 1
fi

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

echo "PVE dashboard and NAS monitor calls are active"
