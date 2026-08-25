#include <assert.h>
#include <string.h>

#include "../dashboard_model.h"

static VMInfo vm(int id, bool running) {
  VMInfo value = {};
  value.vmid = id;
  value.running = running;
  return value;
}

int main() {
  PveNodeInfo longNode = {};
  assert(sizeof(longNode.name) >= 64);

  VMInfo values[] = {vm(300, false), vm(105, true), vm(101, true), vm(200, false)};
  sortVMs(values, 4);
  assert(values[0].running && values[0].vmid == 101);
  assert(values[1].running && values[1].vmid == 105);
  assert(!values[2].running && values[2].vmid == 200);
  assert(!values[3].running && values[3].vmid == 300);

  VMInfo preferred[3] = {};
  size_t preferredCount = 0;
  const VMInfo candidates[] = {
    vm(300, false), vm(500, true), vm(200, false), vm(100, true), vm(400, true)
  };
  for (const VMInfo& candidate : candidates) {
    insertPreferredVM(preferred, preferredCount, 3, candidate);
  }
  assert(preferredCount == 3);
  assert(preferred[0].vmid == 100);
  assert(preferred[1].vmid == 400);
  assert(preferred[2].vmid == 500);

  VMInfo largeCpu = {};
  largeCpu.cpus = static_cast<uint16_t>(512);
  assert(largeCpu.cpus == 512);

  assert(committedVMCount(true, 3) == 3);
  assert(committedVMCount(false, 3) == 0);

  assert(isUsableGuestIPv4("192.168.31.50"));
  assert(!isUsableGuestIPv4("127.0.0.1"));
  assert(!isUsableGuestIPv4("169.254.10.20"));
  assert(!isUsableGuestIPv4("fe80::1"));
  assert(!isUsableGuestIPv4(""));
  assert(!isUsableGuestIPv4(nullptr));

  assert(memoryPercent(0, 0) == 0);
  assert(memoryPercent(5, 10) == 50);
  assert(memoryPercent(12, 10) == 100);

  assert(bytesToGiB(0) == 0.0f);
  assert(bytesToGiB(1073741824ULL) == 1.0f);
  assert(bytesToGiB(1610612736ULL) == 1.5f);

  return 0;
}
