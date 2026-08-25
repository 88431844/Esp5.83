#!/bin/sh
set -eu

sketch="${1:-epd5in83-hanshow-arduino.ino}"

for call in fetchPVE drawPVE drawPVEBottomBar fetchNAS drawNAS drawNASBottomBar; do
  if ! rg -q "^[[:space:]]+${call}\\([^;]*\\);" "$sketch"; then
    echo "Required active call ${call}() is missing from $sketch" >&2
    exit 1
  fi
done

for endpoint in \
  '/api2/json/nodes' \
  '/api2/json/cluster/resources?type=vm' \
  '/agent/network-get-interfaces'; do
  if ! rg -qF "$endpoint" "$sketch"; then
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
  if ! rg -qF "$implementation" "$sketch"; then
    echo "Required PVE implementation is missing: $implementation" >&2
    exit 1
  fi
done

if rg -qF 'PVEAPIToken=' "$sketch"; then
  echo "PVE API token must not be embedded in tracked sketch source" >&2
  exit 1
fi

for secret_name in WIFI_SSID WIFI_PASS SNMP_COMMUNITY PVE_TOKEN; do
  if rg -q "^const char\\* ${secret_name}[[:space:]]*=" "$sketch"; then
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

if rg -qF 'vm.node' "$sketch" || rg -qF 'candidate.node' "$sketch"; then
  echo "VM entries must reuse the selected canonical PVE node name" >&2
  exit 1
fi

if ! rg -qF 'PVE_HOST, PVE_PORT, pve_node.name, vm.vmid);' "$sketch"; then
  echo "Guest Agent URL must use the selected canonical PVE node name" >&2
  exit 1
fi

for declaration in 'static WiFiUDP udp;' 'static SNMPManager snmp(SNMP_COMMUNITY);'; do
  if ! rg -qF "  $declaration" "$sketch"; then
    echo "Required stable NAS declaration is missing: $declaration" >&2
    exit 1
  fi
done

echo "PVE dashboard and NAS monitor calls are active"
