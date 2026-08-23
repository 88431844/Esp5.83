#!/bin/sh
set -eu

sketch="${1:-epd5in83-hanshow-arduino.ino}"
active_pve_calls='^[[:space:]]+(fetchPVE|drawPVM)\([^;]*\);'

if rg -n "$active_pve_calls" "$sketch"; then
  echo "PVE call is still active in $sketch" >&2
  exit 1
fi

for call in fetchNAS drawNAS drawBottomBar; do
  if ! rg -q "^[[:space:]]+${call}\\([^;]*\\);" "$sketch"; then
    echo "Required NAS call ${call}() is inactive in $sketch" >&2
    exit 1
  fi
done

for declaration in 'static WiFiUDP udp;' 'static SNMPManager snmp(SNMP_COMMUNITY);'; do
  if ! rg -qF "  $declaration" "$sketch"; then
    echo "Required static declaration is missing: $declaration" >&2
    exit 1
  fi
done

for diagnostic in 'Serial.print("NAS...");' 'Serial.printf(" %d pools\n", pool_count);'; do
  if ! rg -qF "$diagnostic" "$sketch"; then
    echo "Required NAS diagnostic is missing: $diagnostic" >&2
    exit 1
  fi
done

echo "PVE calls are inactive and NAS calls are active"
