#ifndef ESP583_DASHBOARD_MODEL_H
#define ESP583_DASHBOARD_MODEL_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const size_t MAX_PVE_VMS = 12;
static const size_t MAX_VISIBLE_PVE_VMS = 7;
static const size_t NAS_SPEED_X = 456;
static const size_t NAS_SPEED_Y = 416;
static const size_t NAS_SPEED_WIDTH = 144;
static const size_t NAS_SPEED_HEIGHT = 32;
static const size_t NAS_SPEED_BUFFER_SIZE = (NAS_SPEED_WIDTH / 8) * NAS_SPEED_HEIGHT;

struct CalendarCell {
  int row;
  int column;
};

struct TextPlacement {
  int x;
  int baseline_y;
};

inline int mondayFirstColumn(int tm_wday) {
  return (tm_wday + 6) % 7;
}

inline int daysInGregorianMonth(int year, int month_one_based) {
  static const int lengths[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (month_one_based == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) ||
                      (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return lengths[month_one_based - 1];
}

inline CalendarCell calendarCellForDay(int first_tm_wday, int day) {
  const int index = mondayFirstColumn(first_tm_wday) + day - 1;
  return {index / 7, index % 7};
}

inline TextPlacement centerTextInRect(
    int x, int y, int width, int height, int text_width,
    int font_ascent, int font_descent) {
  const int text_height = font_ascent - font_descent;
  return {
    x + (width - text_width) / 2,
    y + (height - text_height) / 2 + font_ascent
  };
}

inline const char* chineseWeekdayLabel(int monday_first_column) {
  static const char* const labels[] = {
    "周一", "周二", "周三", "周四", "周五", "周六", "周日"
  };
  return labels[monday_first_column];
}

inline const char* chineseWeekdayName(int tm_wday) {
  static const char* const names[] = {
    "星期日", "星期一", "星期二", "星期三",
    "星期四", "星期五", "星期六"
  };
  return names[tm_wday];
}

inline char* formatChineseCalendarHeader(
    int year, int month, int day, int tm_wday,
    char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size, "%d年%d月%d日 %s",
           year, month, day, chineseWeekdayName(tm_wday));
  return buffer;
}

inline char* formatChineseWeatherHeader(
    int month, int day, char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size, "今天天气 %d月%d日", month, day);
  return buffer;
}

inline char* formatChineseWeatherSummary(
    float temperature, int humidity, float wind,
    char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size,
           "%.1f°C 湿度%d%% 风速%.1fkm/h",
           temperature, humidity, wind);
  return buffer;
}

struct NetworkCounterSample {
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint32_t sampled_at_ms;
  bool valid;
};

struct NetworkRates {
  uint64_t rx_bytes_per_second;
  uint64_t tx_bytes_per_second;
  bool valid;
};

inline bool intervalElapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

enum DashboardAction {
  DASHBOARD_IDLE,
  DASHBOARD_PERIODIC_FULL,
  DASHBOARD_RECOVERY_FULL,
  DASHBOARD_NETWORK_SAMPLE
};

struct WiFiDisconnectEvent {
  bool connected_observed;
  bool genuine_disconnect;
};

inline WiFiDisconnectEvent classifyWiFiDisconnectEvent(
    bool connected_observed) {
  return {false, connected_observed};
}

inline uint32_t wifiRetryAnchorAfterDisconnectEvent(
    uint32_t current_anchor, uint32_t event_time, bool genuine_disconnect) {
  return genuine_disconnect ? event_time : current_anchor;
}

inline DashboardAction chooseConnectedDashboardAction(
    uint32_t now, uint32_t last_full_start, uint32_t last_network_start,
    uint32_t last_full_completion, bool full_attempt_recorded,
    bool display_ready, bool recovery_pending, bool data_refresh_pending,
    uint32_t full_interval, uint32_t network_interval,
    uint32_t full_guard_interval) {
  const bool fullGuardOpen = !full_attempt_recorded ||
    intervalElapsed(now, last_full_completion, full_guard_interval);
  if (fullGuardOpen) {
    if (recovery_pending) return DASHBOARD_RECOVERY_FULL;
    if (data_refresh_pending ||
        intervalElapsed(now, last_full_start, full_interval)) {
      return DASHBOARD_PERIODIC_FULL;
    }
  }
  if (display_ready &&
      intervalElapsed(now, last_network_start, network_interval)) {
    return DASHBOARD_NETWORK_SAMPLE;
  }
  return DASHBOARD_IDLE;
}

inline bool shouldInvalidateNetworkSample(
    const NetworkCounterSample& sample, uint32_t now, uint32_t max_age,
    uint8_t failure_count, uint8_t max_failures) {
  if (!sample.valid) return false;
  return failure_count >= max_failures ||
         intervalElapsed(now, sample.sampled_at_ms, max_age);
}

inline bool scaleByteDelta(uint64_t delta, uint32_t elapsed, uint64_t& rate) {
  if (elapsed == 0) return false;
  const uint64_t quotient = delta / elapsed;
  const uint64_t remainder = delta % elapsed;
  if (quotient > UINT64_MAX / 1000ULL) return false;
  const uint64_t base = quotient * 1000ULL;
  const uint64_t fractional = (remainder * 1000ULL) / elapsed;
  if (base > UINT64_MAX - fractional) return false;
  rate = base + fractional;
  return true;
}

inline bool calculateNetworkRates(const NetworkCounterSample& previous,
                                  const NetworkCounterSample& current,
                                  NetworkRates& rates) {
  rates = {};
  if (!previous.valid || !current.valid) return false;
  const uint32_t elapsed = static_cast<uint32_t>(current.sampled_at_ms - previous.sampled_at_ms);
  if (current.rx_bytes < previous.rx_bytes || current.tx_bytes < previous.tx_bytes || elapsed == 0) {
    return false;
  }
  if (!scaleByteDelta(current.rx_bytes - previous.rx_bytes, elapsed,
                      rates.rx_bytes_per_second) ||
      !scaleByteDelta(current.tx_bytes - previous.tx_bytes, elapsed,
                      rates.tx_bytes_per_second)) {
    rates = {};
    return false;
  }
  rates.valid = true;
  return true;
}

inline char* formatByteRate(uint64_t bytes_per_second, char* buffer, size_t buffer_size) {
  if (bytes_per_second >= 1024ULL * 1024ULL) {
    snprintf(buffer, buffer_size, "%.1f MB/s",
             static_cast<double>(bytes_per_second) / (1024.0 * 1024.0));
  } else if (bytes_per_second >= 1024ULL) {
    snprintf(buffer, buffer_size, "%.1f KB/s",
             static_cast<double>(bytes_per_second) / 1024.0);
  } else {
    snprintf(buffer, buffer_size, "%llu B/s",
             static_cast<unsigned long long>(bytes_per_second));
  }
  return buffer;
}

inline char* formatNetworkRates(const NetworkRates& rates, char* buffer,
                                size_t buffer_size) {
  if (!rates.valid) {
    snprintf(buffer, buffer_size, "RX:-- TX:--");
    return buffer;
  }
  char rx[32] = {};
  char tx[32] = {};
  formatByteRate(rates.rx_bytes_per_second, rx, sizeof(rx));
  formatByteRate(rates.tx_bytes_per_second, tx, sizeof(tx));
  snprintf(buffer, buffer_size, "RX:%s TX:%s", rx, tx);
  return buffer;
}

inline void formatNetworkRateLines(const NetworkRates& rates,
                                   char* upload, size_t upload_size,
                                   char* download, size_t download_size) {
  if (!rates.valid) {
    snprintf(upload, upload_size, "TX:--");
    snprintf(download, download_size, "RX:--");
    return;
  }
  char rate[32] = {};
  formatByteRate(rates.tx_bytes_per_second, rate, sizeof(rate));
  snprintf(upload, upload_size, "TX:%s", rate);
  formatByteRate(rates.rx_bytes_per_second, rate, sizeof(rate));
  snprintf(download, download_size, "RX:%s", rate);
}

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
