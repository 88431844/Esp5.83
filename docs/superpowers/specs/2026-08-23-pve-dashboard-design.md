# PVE Dashboard Design

## Goal

Enable the lower-left PVE dashboard on the 600x448 ESP8266 e-paper display.
Show live node memory in a footer and a compact, deterministic virtual-machine
list without reintroducing the heap failures seen in earlier firmware.

## Display

- Keep calendar and weather in the upper half and Synology NAS in the lower
  right.
- Use the lower-left area from y=224 through y=415 for one PVE node and up to
  seven QEMU virtual machines.
- Title the panel with the PVE node name. Each VM row contains status, name,
  IPv4 address, configured CPU count, and current/configured memory.
- Sort running VMs first. Within both running and stopped groups, sort by
  numeric VM ID ascending.
- Use the lower-left 32-pixel footer for the configured PVE host IP and node
  memory usage. Keep the existing NAS footer on the right.
- Display `-` when a running guest has no available QEMU Guest Agent IPv4.

## Data Flow

1. Request `/api2/json/nodes` and select the configured/first online node.
2. Request `/api2/json/cluster/resources?type=vm`, parse one array element at a
   time, and retain only the best 12 selected-node QEMU entries in a fixed array.
3. Sort the fixed array by running state and numeric VM ID, then display the
   first seven entries.
4. For each retained running VM, request
   `/nodes/{node}/qemu/{vmid}/agent/network-get-interfaces` and select the first
   non-loopback, non-link-local IPv4 address.
5. Release every HTTPS and JSON object before starting the next request.

## Memory And Failure Handling

- Use ArduinoJson filters and stream parsing instead of storing full response
  bodies.
- Authenticate the PVE server with its configured SHA-1 certificate fingerprint
  before transmitting the API token.
- Load the API token from a local `secrets.h` that is ignored by Git; keep only
  a placeholder `secrets.example.h` in tracked source.
- Keep all dashboard state in fixed-size structs; do not store JSON strings or
  response payloads globally.
- Run HTTPS requests serially and log free heap, largest free block, and heap
  fragmentation after each phase.
- Preserve the previous display data defaults when an endpoint fails, print a
  concise serial error, and continue rendering the remaining panels.

## Refresh Capability

The Waveshare 5.83-inch V1 is the 600x448 model. Its official Arduino V1
driver exposes only full-frame `DisplayFrame`; it has no partial-window or
partial-refresh API. GxEPD2 can constrain writes for this controller, but its
declared partial refresh uses the same roughly 15-second update waveform and
is not a fast partial refresh. Therefore minute-level calendar time and NAS
network-rate updates are intentionally not added for V1.

## Verification

- Host tests cover sorting, usable IPv4 selection, and bounded memory percent.
- A source-level test requires all three PVE endpoints and active fetch/render
  calls while preserving NAS calls.
- Arduino CLI compilation must succeed for `esp8266:esp8266:nodemcuv2`.
- After upload, serial output must show successful boot, PVE node/VM results,
  NAS results, display completion, and no exception, watchdog reset, or heap
  exhaustion.
