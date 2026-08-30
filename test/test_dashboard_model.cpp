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
  assert(mondayFirstColumn(1) == 0);
  assert(mondayFirstColumn(2) == 1);
  assert(mondayFirstColumn(0) == 6);

  assert(daysInGregorianMonth(2026, 1) == 31);
  assert(daysInGregorianMonth(2026, 2) == 28);
  assert(daysInGregorianMonth(2024, 2) == 29);
  assert(daysInGregorianMonth(2100, 2) == 28);
  assert(daysInGregorianMonth(2000, 2) == 29);

  CalendarCell mondayStart = calendarCellForDay(1, 1);
  assert(mondayStart.row == 0 && mondayStart.column == 0);
  CalendarCell sundayStart = calendarCellForDay(0, 1);
  assert(sundayStart.row == 0 && sundayStart.column == 6);
  CalendarCell nextMonday = calendarCellForDay(0, 2);
  assert(nextMonday.row == 1 && nextMonday.column == 0);

  TextPlacement oneDigit = centerTextInRect(5, 54, 41, 28, 10, 15, -3);
  assert(oneDigit.x == 20);
  assert(oneDigit.baseline_y == 74);
  TextPlacement twoDigits = centerTextInRect(5, 54, 41, 28, 20, 15, -3);
  assert(twoDigits.x == 15);
  assert(twoDigits.baseline_y == 74);

  assert(strcmp(chineseWeekdayLabel(0), "周一") == 0);
  assert(strcmp(chineseWeekdayLabel(6), "周日") == 0);

  char calendarHeader[64] = {};
  formatChineseCalendarHeader(2026, 8, 30, 0,
                              calendarHeader, sizeof(calendarHeader));
  assert(strcmp(calendarHeader, "2026年8月30日 星期日") == 0);

  char weatherHeader[48] = {};
  formatChineseWeatherHeader(8, 30, weatherHeader, sizeof(weatherHeader));
  assert(strcmp(weatherHeader, "今天天气 8月30日") == 0);

  char weatherSummary[80] = {};
  formatChineseWeatherSummary(23.4f, 67, 3.2f,
                              weatherSummary, sizeof(weatherSummary));
  assert(strcmp(weatherSummary,
                "23.4°C 湿度67% 风速3.2km/h") == 0);

  assert(NAS_SPEED_X == 456);
  assert(NAS_SPEED_Y == 416);
  assert(NAS_SPEED_WIDTH == 144);
  assert(NAS_SPEED_HEIGHT == 32);
  assert(NAS_SPEED_BUFFER_SIZE == 576);
  assert((NAS_SPEED_X % 8) == 0);
  assert((NAS_SPEED_WIDTH % 8) == 0);

  NetworkCounterSample first = {1000, 4000, 1000, true};
  NetworkCounterSample second = {6120, 14240, 6000, true};
  NetworkRates rates = {};
  assert(calculateNetworkRates(first, second, rates));
  assert(rates.rx_bytes_per_second == 1024);
  assert(rates.tx_bytes_per_second == 2048);
  assert(rates.valid);

  NetworkCounterSample reset = {900, 3000, 11000, true};
  assert(!calculateNetworkRates(second, reset, rates));
  assert(!rates.valid);

  NetworkCounterSample rxReset = {5000, 14240, 11000, true};
  assert(!calculateNetworkRates(second, rxReset, rates));
  assert(!rates.valid);

  NetworkCounterSample txReset = {6120, 12000, 11000, true};
  assert(!calculateNetworkRates(second, txReset, rates));
  assert(!rates.valid);

  NetworkCounterSample missing = {0, 0, 11000, false};
  assert(!calculateNetworkRates(second, missing, rates));
  assert(!rates.valid);

  NetworkCounterSample invalidPrevious = {1000, 4000, 1000, false};
  assert(!calculateNetworkRates(invalidPrevious, second, rates));
  assert(!rates.valid);

  NetworkCounterSample sameTime = {6120, 14240, 6000, true};
  assert(!calculateNetworkRates(second, sameTime, rates));
  assert(!rates.valid);

  NetworkCounterSample extreme = {0, 0, 1000, true};
  NetworkCounterSample extremeCurrent = {UINT64_MAX, 1, 1001, true};
  assert(!calculateNetworkRates(extreme, extremeCurrent, rates));
  assert(!rates.valid);

  char formatted[64] = {};
  rates = {};
  assert(strcmp(formatNetworkRates(rates, formatted, sizeof(formatted)), "RX:-- TX:--") == 0);
  rates = {1024, 2097152, true};
  assert(strcmp(formatNetworkRates(rates, formatted, sizeof(formatted)),
                "RX:1.0 KB/s TX:2.0 MB/s") == 0);
  char upload[32] = {};
  char download[32] = {};
  formatNetworkRateLines(rates, upload, sizeof(upload),
                         download, sizeof(download));
  assert(strcmp(upload, "TX:2.0 MB/s") == 0);
  assert(strcmp(download, "RX:1.0 KB/s") == 0);
  rates = {UINT64_MAX, UINT64_MAX, true};
  formatNetworkRates(rates, formatted, sizeof(formatted));
  assert(strcmp(formatted,
    "RX:17592186044416.0 MB/s TX:17592186044416.0 MB/s") == 0);

  assert(!intervalElapsed(4999, 0, 5000));
  assert(intervalElapsed(5000, 0, 5000));
  assert(intervalElapsed(0x20, 0xFFFFFFF0, 0x30));

  assert(chooseConnectedDashboardAction(
    600000, 0, 595000, 0, false, true, false, false,
    600000, 5000, 60000) == DASHBOARD_PERIODIC_FULL);
  assert(chooseConnectedDashboardAction(
    65000, 10000, 60000, 5000, true, false, true, false,
    600000, 5000, 60000) == DASHBOARD_RECOVERY_FULL);
  assert(chooseConnectedDashboardAction(
    65000, 10000, 60000, 10000, true, false, true, false,
    600000, 5000, 60000) == DASHBOARD_IDLE);
  assert(chooseConnectedDashboardAction(
    65000, 10000, 60000, 0, false, true, false, false,
    600000, 5000, 60000) == DASHBOARD_NETWORK_SAMPLE);
  assert(chooseConnectedDashboardAction(
    65000, 10000, 60000, 0, false, false, false, false,
    600000, 5000, 60000) == DASHBOARD_IDLE);
  assert(chooseConnectedDashboardAction(
    0x20, 0xFFFFFFF0, 0xFFFFFFF0, 0xFFFFFFF0,
    true, true, false, false,
    0x30, 0x30, 0x30) == DASHBOARD_PERIODIC_FULL);

  // Cached recovery crossed the periodic deadline; guard prevents a second flash.
  assert(chooseConnectedDashboardAction(
    610000, 0, 609000, 609000, true, true, false, false,
    600000, 5000, 60000) == DASHBOARD_IDLE);
  assert(chooseConnectedDashboardAction(
    669000, 0, 609000, 609000, true, true, false, false,
    600000, 5000, 60000) == DASHBOARD_PERIODIC_FULL);

  // Physical recovery wins once the guard opens; periodic remains pending.
  assert(chooseConnectedDashboardAction(
    700000, 0, 699000, 600000, true, false, true, false,
    600000, 5000, 60000) == DASHBOARD_RECOVERY_FULL);

  // A due periodic full is guarded, but rate sampling can continue meanwhile.
  assert(chooseConnectedDashboardAction(
    610000, 0, 600000, 609000, true, true, false, false,
    600000, 5000, 60000) == DASHBOARD_NETWORK_SAMPLE);

  const WiFiDisconnectEvent offlineAttemptEvent =
    classifyWiFiDisconnectEvent(false);
  assert(!offlineAttemptEvent.genuine_disconnect);
  assert(!offlineAttemptEvent.connected_observed);
  assert(wifiRetryAnchorAfterDisconnectEvent(
    1000, 1500, offlineAttemptEvent.genuine_disconnect) == 1000);

  const WiFiDisconnectEvent connectedEvent =
    classifyWiFiDisconnectEvent(true);
  assert(connectedEvent.genuine_disconnect);
  assert(!connectedEvent.connected_observed);
  assert(wifiRetryAnchorAfterDisconnectEvent(
    1000, 1500, connectedEvent.genuine_disconnect) == 1500);

  NetworkCounterSample recent = {1, 2, 1000, true};
  assert(!shouldInvalidateNetworkSample(recent, 59999, 60000, 2, 3));
  assert(shouldInvalidateNetworkSample(recent, 61000, 60000, 2, 3));
  assert(shouldInvalidateNetworkSample(recent, 2000, 60000, 3, 3));
  NetworkCounterSample noBaseline = {0, 0, 0, false};
  assert(!shouldInvalidateNetworkSample(noBaseline, UINT32_MAX,
                                        60000, UINT8_MAX, 3));

  uint64_t scaledRate = 123;
  assert(!scaleByteDelta(1000, 0, scaledRate));
  assert(scaleByteDelta(UINT64_MAX, 1000, scaledRate));
  assert(scaledRate == UINT64_MAX);

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
