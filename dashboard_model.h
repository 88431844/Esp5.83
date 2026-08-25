#ifndef ESP583_DASHBOARD_MODEL_H
#define ESP583_DASHBOARD_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const size_t MAX_PVE_VMS = 12;
static const size_t MAX_VISIBLE_PVE_VMS = 7;

struct PveNodeInfo {
  char name[64];
  char ip[16];
  bool online;
  uint64_t mem_bytes;
  uint64_t maxmem_bytes;
};

struct VMInfo {
  int vmid;
  char name[20];
  char ip[16];
  bool running;
  uint16_t cpus;
  uint64_t mem_bytes;
  uint64_t maxmem_bytes;
};

inline bool vmComesBefore(const VMInfo& left, const VMInfo& right) {
  return left.running != right.running
    ? left.running
    : left.vmid < right.vmid;
}

inline void sortVMs(VMInfo* values, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    VMInfo current = values[i];
    size_t j = i;
    while (j > 0) {
      const VMInfo& previous = values[j - 1];
      const bool shouldMove = vmComesBefore(current, previous);
      if (!shouldMove) break;
      values[j] = previous;
      --j;
    }
    values[j] = current;
  }
}

inline void insertPreferredVM(VMInfo* values, size_t& count, size_t capacity,
                              const VMInfo& candidate) {
  if (capacity == 0) return;
  if (count < capacity) {
    values[count++] = candidate;
    sortVMs(values, count);
    return;
  }
  if (vmComesBefore(candidate, values[count - 1])) {
    values[count - 1] = candidate;
    sortVMs(values, count);
  }
}

inline size_t committedVMCount(bool complete, size_t retained) {
  return complete ? retained : 0;
}

inline bool isUsableGuestIPv4(const char* address) {
  if (!address || !address[0] || !strchr(address, '.')) return false;
  if (strncmp(address, "127.", 4) == 0) return false;
  if (strncmp(address, "169.254.", 8) == 0) return false;
  return true;
}

inline uint8_t memoryPercent(uint64_t used, uint64_t total) {
  if (total == 0) return 0;
  if (used >= total) return 100;
  return static_cast<uint8_t>((used * 100ULL) / total);
}

inline float bytesToGiB(uint64_t bytes) {
  return static_cast<float>(bytes) / 1073741824.0f;
}

#endif
