#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Arduino_SNMP_Manager.h>
#include <SNMPGet.h>
#include "GxEPD2_583_FastPartial.h"
#include "dashboard_model.h"
#include "secrets.h"

// ===== 配置 =====
const char* PVE_HOST = "192.168.31.34";
const int PVE_PORT = 8006;
const char* PVE_CERT_FINGERPRINT = "32:2A:C0:E1:C4:73:01:56:33:7D:CD:72:5C:19:72:DD:37:08:EA:C9";

IPAddress nas_ip(192, 168, 31, 105);

constexpr uint32_t NAS_SPEED_REFRESH_INTERVAL_MS = 5000;
constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 600000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t FULL_RECOVERY_BACKOFF_MS = 60000;
constexpr uint32_t NAS_NETWORK_SAMPLE_MAX_AGE_MS = 60000;

// ===== 数据结构 =====
struct WeatherNow { float temp; int code; int humidity; float wind; };
struct HourlyWeather { int hour; float temp; int code; };
struct DailyWeather { String day; float tMax; float tMin; int code; };
struct PoolInfo { char name[20]; int status; float used_tb; float total_tb; int pct; };

// ===== 全局数据 =====
WeatherNow now_weather;
HourlyWeather hourly[8];
int hourly_count = 0;
DailyWeather daily[7];
int daily_count = 0;
PveNodeInfo pve_node;
VMInfo vms[MAX_PVE_VMS];
int vm_count = 0;
PveNodeInfo previousPveNode;
VMInfo previousPveVMs[MAX_PVE_VMS];
int previousPveVMCount = 0;
bool pveDataValid = false;
PoolInfo pools[4];
int pool_count = 0;
PoolInfo stagedPools[4];
uint32_t g_sysUptime = 0;
struct tm timeinfo;
bool timeValid = false;
uint32_t min_free_heap = UINT32_MAX;

// ===== GxEPD2 显示器 =====
// 将缓冲从全屏(HEIGHT, 33.6KB)改为32行(2.4KB)，解决 OOM 崩溃问题
GxEPD2_BW<GxEPD2_583_FastPartial, 32> display(
  GxEPD2_583_FastPartial(15, 0, 2, 4));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
GFXcanvas1 nasSpeedCanvas(NAS_SPEED_WIDTH, NAS_SPEED_HEIGHT);
U8G2_FOR_ADAFRUIT_GFX nasSpeedFont;
uint8_t previousNasSpeed[NAS_SPEED_BUFFER_SIZE];
bool nasSpeedPartialReady = false;
uint32_t lastNetworkRefreshMs = 0;
uint32_t lastFullRefreshMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastFullAttemptCompletedMs = 0;
bool displayReady = false;
bool offlineRatesDisplayed = false;
bool wifiWasConnected = false;
bool recoveryPending = false;
bool dataRefreshPending = false;
bool fullAttemptRecorded = false;
bool wifiDisconnectNeedsReseed = false;
bool wifiReconnectRefreshPending = false;
volatile bool wifiConnectedObserved = false;
volatile bool wifiDisconnectEventRaised = false;
volatile bool wifiOfflineDisconnectEventRaised = false;
WiFiEventHandler wifiDisconnectHandler;

void onWiFiStationDisconnected(
    const WiFiEventStationModeDisconnected& event) {
  (void)event;
  noInterrupts();
  const WiFiDisconnectEvent transition =
    classifyWiFiDisconnectEvent(wifiConnectedObserved);
  wifiConnectedObserved = transition.connected_observed;
  if (transition.genuine_disconnect) {
    wifiDisconnectEventRaised = true;
  } else {
    wifiOfflineDisconnectEventRaised = true;
  }
  interrupts();
}

void markWiFiConnectedObserved() {
  noInterrupts();
  wifiConnectedObserved = true;
  interrupts();
}

void clearWiFiConnectedObserved() {
  noInterrupts();
  wifiConnectedObserved = false;
  interrupts();
}

bool latchWiFiDisconnectEvent() {
  noInterrupts();
  const bool raised = wifiDisconnectEventRaised;
  const bool offlineEventRaised = wifiOfflineDisconnectEventRaised;
  wifiDisconnectEventRaised = false;
  wifiOfflineDisconnectEventRaised = false;
  interrupts();
  if (offlineEventRaised) {
    Serial.println("WiFi disconnect event ignored while offline/connecting");
  }
  if (!raised) return false;
  wifiDisconnectNeedsReseed = true;
  return true;
}

// ===== WiFi/NTP =====
void connectWifi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) markWiFiConnectedObserved();
  Serial.println(connected ? " OK" : " FAILED");
}

bool syncTime() {
  Serial.print("NTP sync");
  // 使用阿里云NTP，国内稳定
  configTime(28800, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1000000000UL && retries < 20) {  // 等到时间戳合法
    delay(500);
    now = time(nullptr);
    retries++;  // 修复: 原来这里漏掉了递增，导致死循环
  }
  if (now < 1000000000UL) {
    Serial.println(timeValid ? " FAILED; retaining previous time" :
                               " FAILED; time unavailable");
    return false;
  }
  struct tm synchronizedTime;
  if (localtime_r(&now, &synchronizedTime) == nullptr) {
    Serial.println(timeValid ? " FAILED; retaining previous time" :
                               " FAILED; time unavailable");
    return false;
  }
  timeinfo = synchronizedTime;
  timeValid = true;
  Serial.printf(" OK: %04d-%02d-%02d %02d:%02d\n",
    timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
    timeinfo.tm_hour, timeinfo.tm_min);
  return true;
}

// ===== 数据获取 =====
void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  strncpy(destination, source ? source : "", capacity - 1);
  destination[capacity - 1] = '\0';
}

void logHeap(const char* stage) {
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < min_free_heap) min_free_heap = freeHeap;
  Serial.printf("Heap %-14s free=%u max=%u frag=%u%% min=%u\n", stage,
    freeHeap, ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(), min_free_heap);
}

void fetchWeather() {
  Serial.printf("Heap before weather: %d\n", ESP.getFreeHeap());
  Serial.print("Weather...");
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  String url = "http://api.open-meteo.com/v1/forecast"
    "?latitude=39.9042&longitude=116.4074"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    "&hourly=temperature_2m,weather_code"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&timezone=Asia%2FShanghai&forecast_hours=8&forecast_days=7";
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    Serial.printf(" HTTP %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      
      JsonDocument filter;
      filter["current"]["temperature_2m"] = true;
      filter["current"]["weather_code"] = true;
      filter["current"]["relative_humidity_2m"] = true;
      filter["current"]["wind_speed_10m"] = true;
      filter["hourly"]["time"] = true;
      filter["hourly"]["temperature_2m"] = true;
      filter["hourly"]["weather_code"] = true;
      filter["daily"]["time"] = true;
      filter["daily"]["weather_code"] = true;
      filter["daily"]["temperature_2m_max"] = true;
      filter["daily"]["temperature_2m_min"] = true;
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
      
      if (error) {
        Serial.printf("JSON parse failed: %s\n", error.c_str());
      } else {
        now_weather.temp     = doc["current"]["temperature_2m"] | 0.0f;
        now_weather.code     = doc["current"]["weather_code"] | 0;
        now_weather.humidity = doc["current"]["relative_humidity_2m"] | 0;
        now_weather.wind     = doc["current"]["wind_speed_10m"] | 0.0f;
        
        JsonArray time_arr = doc["hourly"]["time"];
        JsonArray temp_arr = doc["hourly"]["temperature_2m"];
        JsonArray code_arr = doc["hourly"]["weather_code"];
        
        hourly_count = 0;
        for (size_t i = 0; i < time_arr.size() && hourly_count < 8; i++) {
          const char* t_str = time_arr[i];
          if (t_str && strlen(t_str) >= 16) {
            char hrStr[3] = { t_str[11], t_str[12], '\0' };
            hourly[hourly_count].hour = atoi(hrStr);
            hourly[hourly_count].temp = temp_arr[i] | 0.0f;
            hourly[hourly_count].code = code_arr[i] | 0;
            hourly_count++;
          }
        }
        
        JsonArray d_time = doc["daily"]["time"];
        JsonArray d_code = doc["daily"]["weather_code"];
        JsonArray d_max = doc["daily"]["temperature_2m_max"];
        JsonArray d_min = doc["daily"]["temperature_2m_min"];
        
        daily_count = 0;
        for (size_t i = 0; i < d_time.size() && daily_count < 7; i++) {
          const char* t_str = d_time[i];
          if (t_str && strlen(t_str) >= 10) {
            String dateStr = String(t_str + 5);
            dateStr.replace("-", "/");
            daily[daily_count].day = dateStr;
            daily[daily_count].code = d_code[i] | 0;
            daily[daily_count].tMax = d_max[i] | 0.0f;
            daily[daily_count].tMin = d_min[i] | 0.0f;
            daily_count++;
          }
        }
      }
      Serial.printf(" %.1fC code=%d %d slots\n", now_weather.temp, now_weather.code, hourly_count);
    }
    http.end();
      client.stop();
  } else {
    Serial.println(" begin FAILED");
  }
  Serial.printf("Heap after weather: %d\n", ESP.getFreeHeap());
}

void configurePVEClient(WiFiClientSecure& client, HTTPClient& http) {
  client.setFingerprint(PVE_CERT_FINGERPRINT);
  client.setBufferSizes(1024, 512);
  client.setTimeout(8000);
  http.useHTTP10(true);
  http.setReuse(false);
  http.setTimeout(8000);
}

bool fetchPVENode() {
  WiFiClientSecure client;
  HTTPClient http;
  configurePVEClient(client, http);

  char url[96];
  snprintf(url, sizeof(url), "https://%s:%d/api2/json/nodes", PVE_HOST, PVE_PORT);
  if (!http.begin(client, url)) {
    Serial.println("PVE node begin FAILED");
    return false;
  }
  http.addHeader("Authorization", PVE_TOKEN);
  const int httpCode = http.GET();
  bool ok = false;
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    filter["data"][0]["node"] = true;
    filter["data"][0]["status"] = true;
    filter["data"][0]["mem"] = true;
    filter["data"][0]["maxmem"] = true;
    JsonDocument doc;
    DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      JsonObject selected;
      for (JsonObject node : doc["data"].as<JsonArray>()) {
        if (selected.isNull()) selected = node;
        const char* status = node["status"] | "unknown";
        if (strcmp(status, "online") == 0) {
          selected = node;
          break;
        }
      }
      if (!selected.isNull()) {
        copyText(pve_node.name, sizeof(pve_node.name), selected["node"] | "PVE");
        pve_node.online = strcmp(selected["status"] | "unknown", "online") == 0;
        pve_node.mem_bytes = selected["mem"].as<uint64_t>();
        pve_node.maxmem_bytes = selected["maxmem"].as<uint64_t>();
        ok = true;
      }
    } else {
      Serial.printf("PVE node JSON %s\n", error.c_str());
    }
  } else {
    Serial.printf("PVE node HTTP %d\n", httpCode);
  }
  http.end();
  client.stop();
  delay(0);
  return ok;
}

bool seekPVEDataArray(Stream& stream) {
  if (!stream.find("\"data\"")) return false;
  return stream.find("[");
}

int peekPVEJsonToken(Stream& stream) {
  const unsigned long started = millis();
  while (millis() - started < 8000) {
    const int value = stream.peek();
    if (value < 0) {
      delay(0);
      continue;
    }
    if (value == ',' || value == ' ' || value == '\r' || value == '\n' || value == '\t') {
      stream.read();
      continue;
    }
    return value;
  }
  return -1;
}

bool fetchPVEVMs() {
  WiFiClientSecure client;
  HTTPClient http;
  configurePVEClient(client, http);

  char url[128];
  snprintf(url, sizeof(url),
    "https://%s:%d/api2/json/cluster/resources?type=vm", PVE_HOST, PVE_PORT);
  if (!http.begin(client, url)) {
    Serial.println("PVE VM begin FAILED");
    return false;
  }
  http.addHeader("Authorization", PVE_TOKEN);
  const int httpCode = http.GET();
  bool ok = false;
  if (httpCode == HTTP_CODE_OK) {
    Stream& stream = http.getStream();
    JsonDocument filter;
    filter["vmid"] = true;
    filter["node"] = true;
    filter["name"] = true;
    filter["status"] = true;
    filter["maxcpu"] = true;
    filter["mem"] = true;
    filter["maxmem"] = true;
    filter["type"] = true;
    JsonDocument item;
    size_t retainedCount = 0;

    if (seekPVEDataArray(stream)) {
      while (true) {
        const int token = peekPVEJsonToken(stream);
        if (token == ']') {
          stream.read();
          ok = true;
          break;
        }
        if (token != '{') {
          Serial.println("PVE VM JSON framing error");
          break;
        }

        item.clear();
        DeserializationError error = deserializeJson(
          item, stream, DeserializationOption::Filter(filter));
        if (error) {
          Serial.printf("PVE VM JSON %s\n", error.c_str());
          break;
        }

        JsonObject source = item.as<JsonObject>();
        if (strcmp(source["type"] | "", "qemu") != 0) continue;
        const char* sourceNode = source["node"] | "";
        if (strcmp(sourceNode, pve_node.name) != 0) continue;

        VMInfo candidate = {};
        candidate.vmid = source["vmid"] | 0;
        copyText(candidate.name, sizeof(candidate.name), source["name"] | "?");
        copyText(candidate.ip, sizeof(candidate.ip), "-");
        candidate.running = strcmp(source["status"] | "stopped", "running") == 0;
        candidate.cpus = source["maxcpu"] | 1;
        candidate.mem_bytes = source["mem"].as<uint64_t>();
        candidate.maxmem_bytes = source["maxmem"].as<uint64_t>();
        insertPreferredVM(vms, retainedCount, MAX_PVE_VMS, candidate);
      }
    }
    vm_count = static_cast<int>(committedVMCount(ok, retainedCount));
  } else {
    Serial.printf("PVE VM HTTP %d\n", httpCode);
  }
  http.end();
  client.stop();
  delay(0);
  return ok;
}

bool fetchPVEGuestIP(VMInfo& vm) {
  if (!vm.running || !pve_node.name[0]) return true;
  WiFiClientSecure client;
  HTTPClient http;
  configurePVEClient(client, http);

  char url[192];
  snprintf(url, sizeof(url),
    "https://%s:%d/api2/json/nodes/%s/qemu/%d/agent/network-get-interfaces",
    PVE_HOST, PVE_PORT, pve_node.name, vm.vmid);
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", PVE_TOKEN);
  const int httpCode = http.GET();
  bool ok = false;
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    filter["data"]["result"][0]["ip-addresses"][0]["ip-address"] = true;
    filter["data"]["result"][0]["ip-addresses"][0]["ip-address-type"] = true;
    JsonDocument doc;
    DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      char fetchedIP[sizeof(vm.ip)];
      copyText(fetchedIP, sizeof(fetchedIP), "-");
      for (JsonObject interface : doc["data"]["result"].as<JsonArray>()) {
        for (JsonObject address : interface["ip-addresses"].as<JsonArray>()) {
          const char* value = address["ip-address"] | "";
          const char* type = address["ip-address-type"] | "";
          if ((type[0] == '\0' || strcmp(type, "ipv4") == 0) && isUsableGuestIPv4(value)) {
            copyText(fetchedIP, sizeof(fetchedIP), value);
            break;
          }
        }
        if (strcmp(fetchedIP, "-") != 0) break;
      }
      copyText(vm.ip, sizeof(vm.ip), fetchedIP);
      ok = true;
    } else {
      Serial.printf("PVE VM %d agent JSON %s\n", vm.vmid, error.c_str());
    }
  } else {
    Serial.printf("PVE VM %d agent HTTP %d\n", vm.vmid, httpCode);
  }
  http.end();
  client.stop();
  delay(0);
  return ok;
}

void fetchPVE() {
  previousPveNode = pve_node;
  memcpy(previousPveVMs, vms, sizeof(vms));
  previousPveVMCount = vm_count;
  const bool hadValidData = pveDataValid;

  memset(&pve_node, 0, sizeof(pve_node));
  copyText(pve_node.name, sizeof(pve_node.name), "PVE");
  copyText(pve_node.ip, sizeof(pve_node.ip), PVE_HOST);
  vm_count = 0;

  Serial.println("PVE...");
  logHeap("before PVE");
  const bool nodeOk = fetchPVENode();
  logHeap("after node");
  const bool vmOk = nodeOk && fetchPVEVMs();
  logHeap("after VM list");

  if (!nodeOk || !vmOk) {
    if (hadValidData) {
      pve_node = previousPveNode;
      memcpy(vms, previousPveVMs, sizeof(vms));
      vm_count = previousPveVMCount;
      Serial.println("PVE refresh incomplete; retaining previous snapshot");
    } else {
      Serial.println("PVE refresh incomplete; no previous snapshot");
    }
    logHeap("after PVE");
    return;
  }

  const int visible = min(vm_count, static_cast<int>(MAX_VISIBLE_PVE_VMS));
  for (int i = 0; i < visible; ++i) {
    if (!vms[i].running) break;
    for (int previousIndex = 0; previousIndex < previousPveVMCount;
         ++previousIndex) {
      if (previousPveVMs[previousIndex].vmid == vms[i].vmid) {
        copyText(vms[i].ip, sizeof(vms[i].ip),
          previousPveVMs[previousIndex].ip);
        break;
      }
    }
    if (!fetchPVEGuestIP(vms[i])) {
      Serial.printf("PVE VM %d IP stale=%s\n", vms[i].vmid, vms[i].ip);
    }
    logHeap("after guest IP");
  }
  pveDataValid = true;
  Serial.printf("PVE node=%s %s, VMs=%d\n", pve_node.name,
    nodeOk ? "OK" : "FAILED", vmOk ? vm_count : 0);
  logHeap("after PVE");
}

// 全局缓冲，防止底层库持有栈内指针导致崩溃
char g_volNameBufs[4][SNMP_OCTETSTRING_MAX_LENGTH];
char* g_volNames[4] = {g_volNameBufs[0], g_volNameBufs[1], g_volNameBufs[2], g_volNameBufs[3]};
int g_volAlloc[4] = {0, 0, 0, 0};
int g_volTotal[4] = {0, 0, 0, 0};
int g_volUsed[4] = {0, 0, 0, 0};
char g_oidName[4][64];
char g_oidAlloc[4][64];
char g_oidTotal[4][64];
char g_oidUsed[4][64];

static WiFiUDP nasUdp;
static SNMPManager nasSnmp(SNMP_COMMUNITY);
bool nasCallbacksReady = false;
bool nasCountersReady = false;
int nasInterfaceIndex = -1;
uint64_t nasRxOctets = UINT64_MAX;
uint64_t nasTxOctets = UINT64_MAX;
NetworkCounterSample previousNetworkSample = {};
NetworkRates currentNetworkRates = {};
ValueCallback* cbUptime = nullptr;
ValueCallback* cbInterfaceIndex = nullptr;
ValueCallback* cbRxOctets = nullptr;
ValueCallback* cbTxOctets = nullptr;
ValueCallback* cbName[4] = {};
ValueCallback* cbAlloc[4] = {};
ValueCallback* cbTotal[4] = {};
ValueCallback* cbUsed[4] = {};
char oidInterfaceIndex[64];
char oidRxOctets[64];
char oidTxOctets[64];
int nasCounterInterfaceIndex = -1;
uint8_t nasCounterFailureCount = 0;
uint32_t nasLastInterfaceDiscoveryAt = 0;
uint32_t nasRequestGeneration = 0;

static const size_t MAX_NAS_REQUEST_CALLBACKS = 4;
static const uint8_t NAS_FAILURES_BEFORE_REDISCOVERY = 3;
static const uint32_t NAS_INTERFACE_REDISCOVERY_INTERVAL_MS = 300000;
static const uint16_t NAS_REQUEST_PORT_BASE = 49152;
static const uint16_t NAS_REQUEST_PORT_COUNT = 16384;
static constexpr char ipAdEntIfIndex[] = ".1.3.6.1.2.1.4.20.1.2";
static constexpr char ifHCInOctets[] = ".1.3.6.1.2.1.31.1.1.1.6";
static constexpr char ifHCOutOctets[] = ".1.3.6.1.2.1.31.1.1.1.10";

bool consumeWiFiDisconnectEvent(const char* checkpoint) {
  if (!latchWiFiDisconnectEvent()) return false;

  wifiWasConnected = false;
  wifiReconnectRefreshPending = true;
  dataRefreshPending = true;
  previousNetworkSample = {};
  currentNetworkRates = {};
  if (nasCounterFailureCount < NAS_FAILURES_BEFORE_REDISCOVERY) {
    nasCounterFailureCount = NAS_FAILURES_BEFORE_REDISCOVERY;
  }
  lastWifiRetryMs = wifiRetryAnchorAfterDisconnectEvent(
    lastWifiRetryMs, millis(), true);
  Serial.printf("WiFi disconnect event at %s; NAS baseline invalidated\n",
    checkpoint ? checkpoint : "unspecified");
  return true;
}

void drainNASPackets() {
  while (nasUdp.parsePacket() > 0) {
    while (nasUdp.available() > 0) nasUdp.read();
  }
}

bool beginNASRequestSocket() {
  for (uint8_t attempt = 0; attempt < 8; ++attempt) {
    ++nasRequestGeneration;
    const uint16_t localPort = static_cast<uint16_t>(
      NAS_REQUEST_PORT_BASE +
      (nasRequestGeneration % NAS_REQUEST_PORT_COUNT));
    nasUdp.stop();
    if (nasUdp.begin(localPort)) {
      drainNASPackets();
      return true;
    }
  }
  nasUdp.stop();
  return false;
}

struct NASBERWriter {
  uint8_t* begin;
  uint8_t* cursor;
  uint8_t* end;
};

struct NASBERTLV {
  uint8_t tag;
  const uint8_t* value;
  size_t length;
};

struct NASDecodedValue {
  const uint8_t* bytes;
  size_t length;
  int32_t signedValue;
  uint64_t unsignedValue;
};

size_t nasBERSize(const NASBERWriter& writer);
bool nasBERPrepend(NASBERWriter& writer, const uint8_t* value,
                   size_t length);
bool nasBERPrependByte(NASBERWriter& writer, uint8_t value);
bool nasBERPrependLength(NASBERWriter& writer, size_t length);
bool nasBERWrap(NASBERWriter& writer, uint8_t tag, size_t contentLength);
bool nasBERPrependTLV(NASBERWriter& writer, uint8_t tag,
                      const uint8_t* value, size_t length);
bool nasBERPrependPositiveInteger(NASBERWriter& writer, uint32_t value);
bool readNASBERTLV(const uint8_t*& cursor, const uint8_t* end,
                   NASBERTLV& tlv);
bool decodeNASSignedInteger(const NASBERTLV& tlv, int32_t& value);
bool decodeNASUnsignedInteger(const NASBERTLV& tlv, size_t width,
                              uint64_t& value);
bool decodeNASValue(ValueCallback* callback, const NASBERTLV& tlv,
                    NASDecodedValue& decoded);
bool nasOIDMatchesCallback(const NASBERTLV& oid, ValueCallback* callback);
bool commitNASDecodedValue(ValueCallback* callback,
                           const NASDecodedValue& decoded);

size_t nasBERSize(const NASBERWriter& writer) {
  return static_cast<size_t>(writer.end - writer.cursor);
}

bool nasBERPrepend(NASBERWriter& writer, const uint8_t* value,
                   size_t length) {
  if (length > static_cast<size_t>(writer.cursor - writer.begin)) {
    return false;
  }
  writer.cursor -= length;
  if (length > 0) memcpy(writer.cursor, value, length);
  return true;
}

bool nasBERPrependByte(NASBERWriter& writer, uint8_t value) {
  return nasBERPrepend(writer, &value, 1);
}

bool nasBERPrependLength(NASBERWriter& writer, size_t length) {
  uint8_t encoded[3];
  size_t encodedLength = 0;
  if (length < 0x80) {
    encoded[encodedLength++] = static_cast<uint8_t>(length);
  } else if (length <= 0xFF) {
    encoded[encodedLength++] = 0x81;
    encoded[encodedLength++] = static_cast<uint8_t>(length);
  } else if (length <= 0xFFFF) {
    encoded[encodedLength++] = 0x82;
    encoded[encodedLength++] = static_cast<uint8_t>(length >> 8);
    encoded[encodedLength++] = static_cast<uint8_t>(length);
  } else {
    return false;
  }
  return nasBERPrepend(writer, encoded, encodedLength);
}

bool nasBERWrap(NASBERWriter& writer, uint8_t tag, size_t contentLength) {
  return nasBERPrependLength(writer, contentLength) &&
         nasBERPrependByte(writer, tag);
}

bool nasBERPrependTLV(NASBERWriter& writer, uint8_t tag,
                      const uint8_t* value, size_t length) {
  return nasBERPrepend(writer, value, length) &&
         nasBERPrependLength(writer, length) &&
         nasBERPrependByte(writer, tag);
}

bool nasBERPrependPositiveInteger(NASBERWriter& writer, uint32_t value) {
  uint8_t encoded[5];
  size_t length = 0;
  do {
    encoded[sizeof(encoded) - 1 - length] = static_cast<uint8_t>(value);
    value >>= 8;
    ++length;
  } while (value != 0);

  size_t offset = sizeof(encoded) - length;
  if ((encoded[offset] & 0x80) != 0) {
    encoded[--offset] = 0;
    ++length;
  }
  return nasBERPrependTLV(writer, INTEGER, encoded + offset, length);
}

bool appendNASOIDSubidentifier(uint32_t value, uint8_t* output,
                               size_t capacity, size_t& length) {
  uint8_t reversed[5];
  size_t count = 0;
  do {
    reversed[count++] = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
  } while (value != 0 && count < sizeof(reversed));
  if (value != 0 || count > capacity - length) return false;

  while (count > 0) {
    --count;
    uint8_t encoded = reversed[count];
    if (count != 0) encoded |= 0x80;
    output[length++] = encoded;
  }
  return true;
}

bool parseNASOIDArc(const char*& cursor, uint32_t& value) {
  if (*cursor < '0' || *cursor > '9') return false;
  value = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
    ++cursor;
  }
  return *cursor == '.' || *cursor == '\0';
}

bool encodeNASOID(const char* oid, uint8_t* output, size_t capacity,
                  size_t& length) {
  if (oid == nullptr || output == nullptr || capacity == 0) return false;
  const char* cursor = oid;
  if (*cursor == '.') ++cursor;

  uint32_t first = 0;
  uint32_t second = 0;
  if (!parseNASOIDArc(cursor, first) || *cursor++ != '.' ||
      !parseNASOIDArc(cursor, second) || first > 2 ||
      (first < 2 && second > 39) ||
      (first == 2 && second > UINT32_MAX - 80U)) {
    return false;
  }

  length = 0;
  const uint32_t combined = first < 2 ? first * 40U + second : 80U + second;
  if (!appendNASOIDSubidentifier(combined, output, capacity, length)) {
    return false;
  }

  while (*cursor != '\0') {
    if (*cursor++ != '.') return false;
    uint32_t arc = 0;
    if (!parseNASOIDArc(cursor, arc) ||
        !appendNASOIDSubidentifier(arc, output, capacity, length)) {
      return false;
    }
  }
  return true;
}

bool encodeNASGetRequest(ValueCallback* const* callbacks, size_t count,
                         uint16_t requestId, uint8_t* buffer,
                         size_t capacity, size_t& offset, size_t& length) {
  NASBERWriter writer = {buffer, buffer + capacity, buffer + capacity};
  uint8_t encodedOID[MAX_OID_LENGTH];

  for (size_t i = count; i > 0; --i) {
    size_t oidLength = 0;
    if (!encodeNASOID(callbacks[i - 1]->OID, encodedOID,
                      sizeof(encodedOID), oidLength)) {
      return false;
    }
    const size_t before = nasBERSize(writer);
    if (!nasBERPrependTLV(writer, NULLTYPE, nullptr, 0) ||
        !nasBERPrependTLV(writer, OID, encodedOID, oidLength) ||
        !nasBERWrap(writer, STRUCTURE, nasBERSize(writer) - before)) {
      return false;
    }
  }
  if (!nasBERWrap(writer, STRUCTURE, nasBERSize(writer)) ||
      !nasBERPrependPositiveInteger(writer, 0) ||
      !nasBERPrependPositiveInteger(writer, 0) ||
      !nasBERPrependPositiveInteger(writer, requestId) ||
      !nasBERWrap(writer, GetRequestPDU, nasBERSize(writer))) {
    return false;
  }

  const size_t communityLength = strlen(SNMP_COMMUNITY);
  if (!nasBERPrependTLV(writer, STRING,
                        reinterpret_cast<const uint8_t*>(SNMP_COMMUNITY),
                        communityLength) ||
      !nasBERPrependPositiveInteger(writer, 1) ||
      !nasBERWrap(writer, STRUCTURE, nasBERSize(writer))) {
    return false;
  }

  offset = static_cast<size_t>(writer.cursor - buffer);
  length = nasBERSize(writer);
  return true;
}

bool readNASBERTLV(const uint8_t*& cursor, const uint8_t* end,
                   NASBERTLV& tlv) {
  if (cursor == nullptr || end == nullptr || cursor >= end) return false;
  tlv.tag = *cursor++;
  if (cursor >= end) return false;

  const uint8_t firstLength = *cursor++;
  size_t length = 0;
  if ((firstLength & 0x80) == 0) {
    length = firstLength;
  } else {
    const uint8_t lengthBytes = firstLength & 0x7F;
    if (lengthBytes == 0 || lengthBytes > 2 ||
        static_cast<size_t>(end - cursor) < lengthBytes ||
        cursor[0] == 0) {
      return false;
    }
    for (uint8_t i = 0; i < lengthBytes; ++i) {
      length = (length << 8) | *cursor++;
    }
    if (length < 0x80) return false;
  }

  if (length > static_cast<size_t>(end - cursor)) return false;
  tlv.value = cursor;
  tlv.length = length;
  cursor += length;
  return true;
}

bool decodeNASSignedInteger(const NASBERTLV& tlv, int32_t& value) {
  if (tlv.tag != INTEGER || tlv.length == 0 || tlv.length > 4) return false;
  if (tlv.length > 1 &&
      ((tlv.value[0] == 0 && (tlv.value[1] & 0x80) == 0) ||
       (tlv.value[0] == 0xFF && (tlv.value[1] & 0x80) != 0))) {
    return false;
  }

  uint32_t encoded = 0;
  for (size_t i = 0; i < tlv.length; ++i) {
    encoded = (encoded << 8) | tlv.value[i];
  }
  int64_t decoded = encoded;
  if ((tlv.value[0] & 0x80) != 0) {
    decoded -= static_cast<int64_t>(1ULL << (tlv.length * 8));
  }
  if (decoded < INT32_MIN || decoded > INT32_MAX) return false;
  value = static_cast<int32_t>(decoded);
  return true;
}

bool decodeNASUnsignedInteger(const NASBERTLV& tlv, size_t width,
                              uint64_t& value) {
  if (tlv.length == 0 || tlv.length > width + 1 ||
      (tlv.value[0] & 0x80) != 0) {
    return false;
  }

  size_t offset = 0;
  if (tlv.length > 1 && tlv.value[0] == 0) {
    if ((tlv.value[1] & 0x80) == 0) return false;
    offset = 1;
  }
  if (tlv.length - offset > width) return false;

  value = 0;
  for (size_t i = offset; i < tlv.length; ++i) {
    if (value > (UINT64_MAX - tlv.value[i]) / 256ULL) return false;
    value = value * 256ULL + tlv.value[i];
  }
  return true;
}

bool decodeNASValue(ValueCallback* callback, const NASBERTLV& tlv,
                    NASDecodedValue& decoded) {
  if (callback == nullptr || callback->type != tlv.tag) return false;
  decoded = {};

  switch (callback->type) {
    case STRING:
      if (tlv.length >= SNMP_OCTETSTRING_MAX_LENGTH) return false;
      decoded.bytes = tlv.value;
      decoded.length = tlv.length;
      return true;
    case INTEGER:
      return decodeNASSignedInteger(tlv, decoded.signedValue);
    case TIMESTAMP:
      return decodeNASUnsignedInteger(tlv, 4, decoded.unsignedValue) &&
             decoded.unsignedValue <= UINT32_MAX;
    case COUNTER64:
      return decodeNASUnsignedInteger(tlv, 8, decoded.unsignedValue);
    default:
      return false;
  }
}

bool nasOIDMatchesCallback(const NASBERTLV& oid, ValueCallback* callback) {
  if (oid.tag != OID || callback == nullptr) return false;
  uint8_t expected[MAX_OID_LENGTH];
  size_t expectedLength = 0;
  return encodeNASOID(callback->OID, expected, sizeof(expected),
                      expectedLength) &&
         oid.length == expectedLength &&
         memcmp(oid.value, expected, expectedLength) == 0;
}

bool commitNASDecodedValue(ValueCallback* callback,
                           const NASDecodedValue& decoded) {
  switch (callback->type) {
    case STRING: {
      char* destination = *static_cast<StringCallback*>(callback)->value;
      if (destination == nullptr) return false;
      if (decoded.length > 0) {
        memcpy(destination, decoded.bytes, decoded.length);
      }
      destination[decoded.length] = '\0';
      return true;
    }
    case INTEGER:
      *static_cast<IntegerCallback*>(callback)->value =
        static_cast<int>(decoded.signedValue);
      return true;
    case TIMESTAMP:
      *static_cast<TimestampCallback*>(callback)->value =
        static_cast<uint32_t>(decoded.unsignedValue);
      return true;
    case COUNTER64:
      *static_cast<Counter64Callback*>(callback)->value =
        decoded.unsignedValue;
      return true;
    default:
      return false;
  }
}

bool decodeNASResponsePacket(const uint8_t* packet, size_t packetLength,
                             uint16_t requestId,
                             ValueCallback* const* callbacks,
                             size_t count) {
  if (packet == nullptr || packetLength == 0 || count == 0 ||
      count > MAX_NAS_REQUEST_CALLBACKS) {
    return false;
  }

  const uint8_t* packetEnd = packet + packetLength;
  const uint8_t* outerCursor = packet;
  NASBERTLV message;
  if (!readNASBERTLV(outerCursor, packetEnd, message) ||
      message.tag != STRUCTURE || outerCursor != packetEnd) {
    return false;
  }

  const uint8_t* messageCursor = message.value;
  const uint8_t* messageEnd = message.value + message.length;
  NASBERTLV version;
  NASBERTLV community;
  NASBERTLV responsePDU;
  int32_t decodedVersion = 0;
  const size_t expectedCommunityLength = strlen(SNMP_COMMUNITY);
  if (!readNASBERTLV(messageCursor, messageEnd, version) ||
      !decodeNASSignedInteger(version, decodedVersion) ||
      decodedVersion != 1 ||
      !readNASBERTLV(messageCursor, messageEnd, community) ||
      community.tag != STRING ||
      community.length != expectedCommunityLength ||
      memcmp(community.value, SNMP_COMMUNITY, expectedCommunityLength) != 0 ||
      !readNASBERTLV(messageCursor, messageEnd, responsePDU) ||
      responsePDU.tag != GetResponsePDU || messageCursor != messageEnd) {
    return false;
  }

  const uint8_t* pduCursor = responsePDU.value;
  const uint8_t* pduEnd = responsePDU.value + responsePDU.length;
  NASBERTLV responseRequestId;
  NASBERTLV errorStatus;
  NASBERTLV errorIndex;
  NASBERTLV varBindList;
  int32_t decodedRequestId = 0;
  int32_t decodedErrorStatus = 0;
  int32_t decodedErrorIndex = 0;
  if (!readNASBERTLV(pduCursor, pduEnd, responseRequestId) ||
      !decodeNASSignedInteger(responseRequestId, decodedRequestId) ||
      decodedRequestId != requestId ||
      !readNASBERTLV(pduCursor, pduEnd, errorStatus) ||
      !decodeNASSignedInteger(errorStatus, decodedErrorStatus) ||
      decodedErrorStatus != 0 ||
      !readNASBERTLV(pduCursor, pduEnd, errorIndex) ||
      !decodeNASSignedInteger(errorIndex, decodedErrorIndex) ||
      decodedErrorIndex != 0 ||
      !readNASBERTLV(pduCursor, pduEnd, varBindList) ||
      varBindList.tag != STRUCTURE || pduCursor != pduEnd) {
    return false;
  }

  bool matched[MAX_NAS_REQUEST_CALLBACKS] = {};
  NASDecodedValue decoded[MAX_NAS_REQUEST_CALLBACKS] = {};
  size_t varBindCount = 0;
  const uint8_t* listCursor = varBindList.value;
  const uint8_t* listEnd = varBindList.value + varBindList.length;
  while (listCursor < listEnd) {
    if (varBindCount >= count) return false;
    NASBERTLV varBind;
    if (!readNASBERTLV(listCursor, listEnd, varBind) ||
        varBind.tag != STRUCTURE) {
      return false;
    }

    const uint8_t* varBindCursor = varBind.value;
    const uint8_t* varBindEnd = varBind.value + varBind.length;
    NASBERTLV oid;
    NASBERTLV value;
    if (!readNASBERTLV(varBindCursor, varBindEnd, oid) ||
        !readNASBERTLV(varBindCursor, varBindEnd, value) ||
        varBindCursor != varBindEnd) {
      return false;
    }

    size_t callbackIndex = count;
    for (size_t i = 0; i < count; ++i) {
      if (nasOIDMatchesCallback(oid, callbacks[i])) {
        callbackIndex = i;
        break;
      }
    }
    if (callbackIndex == count || matched[callbackIndex] ||
        !decodeNASValue(callbacks[callbackIndex], value,
                        decoded[callbackIndex])) {
      return false;
    }
    matched[callbackIndex] = true;
    ++varBindCount;
  }

  if (listCursor != listEnd || varBindCount != count) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!matched[i]) return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!commitNASDecodedValue(callbacks[i], decoded[i])) return false;
  }
  return true;
}

bool receiveNASResponse(uint16_t requestId,
                        ValueCallback* const* callbacks, size_t count) {
  const int packetLength = nasUdp.parsePacket();
  if (packetLength <= 0) return false;

  const IPAddress responseIP = nasUdp.remoteIP();
  const uint16_t responsePort = nasUdp.remotePort();
  static uint8_t packetBuffer[SNMP_PACKET_LENGTH * 3];
  if (packetLength > static_cast<int>(sizeof(packetBuffer))) {
    while (nasUdp.available() > 0) nasUdp.read();
    return false;
  }

  const int bytesRead = nasUdp.read(packetBuffer, packetLength);
  return bytesRead == packetLength && responseIP == nas_ip &&
         responsePort == 161 &&
         decodeNASResponsePacket(packetBuffer,
           static_cast<size_t>(packetLength), requestId, callbacks, count);
}

bool sendNASGetRequest(ValueCallback* const* callbacks, size_t count,
                       uint16_t requestId) {
  static uint8_t requestBuffer[SNMP_PACKET_LENGTH];
  size_t offset = 0;
  size_t length = 0;
  if (!encodeNASGetRequest(callbacks, count, requestId, requestBuffer,
                           sizeof(requestBuffer), offset, length) ||
      nasUdp.beginPacket(nas_ip, 161) != 1) {
    return false;
  }
  if (nasUdp.write(requestBuffer + offset, length) != length) return false;
  return nasUdp.endPacket() == 1;
}

bool validateNASBERHelpers() {
  struct IntegerVector {
    uint16_t value;
    uint8_t high;
    uint8_t low;
  };
  const IntegerVector vectors[] = {
    {1000, 0x03, 0xE8},
    {1100, 0x04, 0x4C},
    {1101, 0x04, 0x4D},
    {1102, 0x04, 0x4E},
    {1103, 0x04, 0x4F},
    {2000, 0x07, 0xD0},
    {2100, 0x08, 0x34}
  };
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    uint8_t buffer[8];
    NASBERWriter writer = {buffer, buffer + sizeof(buffer),
                           buffer + sizeof(buffer)};
    const uint8_t expected[] = {
      INTEGER, 0x02, vectors[i].high, vectors[i].low
    };
    if (!nasBERPrependPositiveInteger(writer, vectors[i].value) ||
        nasBERSize(writer) != sizeof(expected) ||
        memcmp(writer.cursor, expected, sizeof(expected)) != 0) {
      return false;
    }
  }

  uint8_t leadingZeroBuffer[8];
  NASBERWriter leadingZeroWriter = {
    leadingZeroBuffer, leadingZeroBuffer + sizeof(leadingZeroBuffer),
    leadingZeroBuffer + sizeof(leadingZeroBuffer)
  };
  const uint8_t expected128[] = {INTEGER, 0x02, 0x00, 0x80};
  if (!nasBERPrependPositiveInteger(leadingZeroWriter, 128) ||
      nasBERSize(leadingZeroWriter) != sizeof(expected128) ||
      memcmp(leadingZeroWriter.cursor, expected128,
             sizeof(expected128)) != 0) {
    return false;
  }

  const uint8_t truncatedLength[] = {STRUCTURE, 0x82, 0x01};
  const uint8_t indefiniteLength[] = {STRUCTURE, 0x80};
  const uint8_t nonMinimalLength[] = {STRING, 0x81, 0x01, 0x00};
  const uint8_t* cursor = truncatedLength;
  NASBERTLV tlv;
  if (readNASBERTLV(cursor, truncatedLength + sizeof(truncatedLength), tlv)) {
    return false;
  }
  cursor = indefiniteLength;
  if (readNASBERTLV(cursor, indefiniteLength + sizeof(indefiniteLength), tlv)) {
    return false;
  }
  cursor = nonMinimalLength;
  if (readNASBERTLV(cursor, nonMinimalLength + sizeof(nonMinimalLength), tlv)) {
    return false;
  }

  const uint8_t signed1000[] = {0x03, 0xE8};
  NASBERTLV signedTLV = {INTEGER, signed1000, sizeof(signed1000)};
  int32_t signedValue = 0;
  if (!decodeNASSignedInteger(signedTLV, signedValue) ||
      signedValue != 1000) {
    return false;
  }

  const uint8_t maximumCounter[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF
  };
  NASBERTLV counterTLV = {
    COUNTER64, maximumCounter, sizeof(maximumCounter)
  };
  uint64_t counterValue = 0;
  return decodeNASUnsignedInteger(counterTLV, 8, counterValue) &&
         counterValue == UINT64_MAX;
}

bool hasNASCallbackDestination(ValueCallback* callback) {
  if (callback == nullptr || callback->OID == nullptr) return false;
  switch (callback->type) {
    case STRING: {
      StringCallback* stringCallback =
        static_cast<StringCallback*>(callback);
      return stringCallback->value != nullptr &&
             *stringCallback->value != nullptr;
    }
    case INTEGER: {
      IntegerCallback* integerCallback =
        static_cast<IntegerCallback*>(callback);
      return integerCallback->value != nullptr &&
             !integerCallback->isFloat;
    }
    case TIMESTAMP:
      return static_cast<TimestampCallback*>(callback)->value != nullptr;
    case COUNTER64:
      return static_cast<Counter64Callback*>(callback)->value != nullptr;
    default:
      return false;
  }
}

bool requestNAS(ValueCallback* const* callbacks, size_t count,
                int requestId, uint32_t timeoutMs) {
  if (count == 0 || count > MAX_NAS_REQUEST_CALLBACKS ||
      requestId <= 0 || requestId > 32767) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!hasNASCallbackDestination(callbacks[i])) return false;
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(callbacks[i]->OID, callbacks[j]->OID) == 0) return false;
    }
  }

  if (!beginNASRequestSocket()) {
    Serial.printf("NAS request %d socket failed\n", requestId);
    return false;
  }

  if (!sendNASGetRequest(callbacks, count,
                         static_cast<uint16_t>(requestId))) {
    nasUdp.stop();
    return false;
  }

  const uint32_t startedAt = millis();
  while (!intervalElapsed(millis(), startedAt, timeoutMs)) {
    if (receiveNASResponse(static_cast<uint16_t>(requestId),
                           callbacks, count)) {
      nasUdp.stop();
      return true;
    }
    delay(10);
  }
  nasUdp.stop();
  return false;
}

void initializeNASCallbacks() {
  if (nasCallbacksReady) return;
  if (!validateNASBERHelpers()) {
    Serial.println("NAS BER self-test FAILED");
    return;
  }

  nasSnmp._udp = nullptr;
  nasSnmp.setUDP(&nasUdp);
  cbUptime = nasSnmp.addTimestampHandler(
    nas_ip, ".1.3.6.1.2.1.25.1.1.0", &g_sysUptime);

  const int volumeIndices[4] = {59, 57, 56, 58};
  for (int i = 0; i < 4; ++i) {
    snprintf(g_oidName[i], sizeof(g_oidName[i]),
      ".1.3.6.1.2.1.25.2.3.1.3.%d", volumeIndices[i]);
    cbName[i] = nasSnmp.addStringHandler(nas_ip, g_oidName[i], &g_volNames[i]);
    snprintf(g_oidAlloc[i], sizeof(g_oidAlloc[i]),
      ".1.3.6.1.2.1.25.2.3.1.4.%d", volumeIndices[i]);
    cbAlloc[i] = nasSnmp.addIntegerHandler(nas_ip, g_oidAlloc[i], &g_volAlloc[i]);
    snprintf(g_oidTotal[i], sizeof(g_oidTotal[i]),
      ".1.3.6.1.2.1.25.2.3.1.5.%d", volumeIndices[i]);
    cbTotal[i] = nasSnmp.addIntegerHandler(nas_ip, g_oidTotal[i], &g_volTotal[i]);
    snprintf(g_oidUsed[i], sizeof(g_oidUsed[i]),
      ".1.3.6.1.2.1.25.2.3.1.6.%d", volumeIndices[i]);
    cbUsed[i] = nasSnmp.addIntegerHandler(nas_ip, g_oidUsed[i], &g_volUsed[i]);
  }

  snprintf(oidInterfaceIndex, sizeof(oidInterfaceIndex),
    "%s.%u.%u.%u.%u", ipAdEntIfIndex,
    static_cast<unsigned int>(nas_ip[0]),
    static_cast<unsigned int>(nas_ip[1]),
    static_cast<unsigned int>(nas_ip[2]),
    static_cast<unsigned int>(nas_ip[3]));
  cbInterfaceIndex = nasSnmp.addIntegerHandler(
    nas_ip, oidInterfaceIndex, &nasInterfaceIndex);
  nasCallbacksReady = true;
}

char* allocateNASOID(const char* oid) {
  const size_t length = strlen(oid) + 1;
  char* value = static_cast<char*>(malloc(length));
  if (value != nullptr) memcpy(value, oid, length);
  return value;
}

bool configureNASCounterCallbacks(int interfaceIndex) {
  char nextRxOID[sizeof(oidRxOctets)];
  char nextTxOID[sizeof(oidTxOctets)];
  snprintf(nextRxOID, sizeof(nextRxOID),
    "%s.%d", ifHCInOctets, interfaceIndex);
  snprintf(nextTxOID, sizeof(nextTxOID),
    "%s.%d", ifHCOutOctets, interfaceIndex);

  if (!nasCountersReady) {
    copyText(oidRxOctets, sizeof(oidRxOctets), nextRxOID);
    copyText(oidTxOctets, sizeof(oidTxOctets), nextTxOID);
    cbRxOctets = nasSnmp.addCounter64Handler(
      nas_ip, oidRxOctets, &nasRxOctets);
    cbTxOctets = nasSnmp.addCounter64Handler(
      nas_ip, oidTxOctets, &nasTxOctets);
    nasCounterInterfaceIndex = interfaceIndex;
    nasCountersReady = true;
    previousNetworkSample = {};
    currentNetworkRates = {};
    return true;
  }

  if (interfaceIndex == nasCounterInterfaceIndex) return true;
  if (cbRxOctets == nullptr || cbTxOctets == nullptr) return false;

  char* replacementRxOID = allocateNASOID(nextRxOID);
  char* replacementTxOID = allocateNASOID(nextTxOID);
  if (replacementRxOID == nullptr || replacementTxOID == nullptr) {
    free(replacementRxOID);
    free(replacementTxOID);
    return false;
  }

  free(cbRxOctets->OID);
  free(cbTxOctets->OID);
  cbRxOctets->OID = replacementRxOID;
  cbTxOctets->OID = replacementTxOID;
  copyText(oidRxOctets, sizeof(oidRxOctets), nextRxOID);
  copyText(oidTxOctets, sizeof(oidTxOctets), nextTxOID);
  nasCounterInterfaceIndex = interfaceIndex;
  previousNetworkSample = {};
  currentNetworkRates = {};
  return true;
}

bool discoverNASInterface() {
  initializeNASCallbacks();
  const int previousInterfaceIndex = nasCounterInterfaceIndex;
  nasInterfaceIndex = -1;
  ValueCallback* callbacks[] = {cbInterfaceIndex};
  if (!requestNAS(callbacks, 1, 2000, 1000) || nasInterfaceIndex <= 0) {
    nasInterfaceIndex = previousInterfaceIndex;
    Serial.println("NAS interface discovery FAILED");
    return false;
  }

  const int discoveredInterfaceIndex = nasInterfaceIndex;
  if (!configureNASCounterCallbacks(discoveredInterfaceIndex)) {
    nasInterfaceIndex = previousInterfaceIndex;
    Serial.println("NAS counter callback configuration FAILED");
    return false;
  }

  nasLastInterfaceDiscoveryAt = millis();
  nasCounterFailureCount = 0;
  if (previousInterfaceIndex > 0 &&
      discoveredInterfaceIndex != previousInterfaceIndex) {
    Serial.printf("NAS interface changed %d -> %d\n",
      previousInterfaceIndex, discoveredInterfaceIndex);
  } else {
    Serial.printf("NAS interface index=%d\n", discoveredInterfaceIndex);
  }
  return true;
}

bool sampleNASNetwork() {
  const uint32_t requestStartedAt = millis();
  if (shouldInvalidateNetworkSample(
        previousNetworkSample, requestStartedAt,
        NAS_NETWORK_SAMPLE_MAX_AGE_MS, nasCounterFailureCount,
        NAS_FAILURES_BEFORE_REDISCOVERY)) {
    previousNetworkSample = {};
    currentNetworkRates = {};
    Serial.println("NAS network baseline stale; reseeding");
  }

  const bool rediscoveryDue = nasCountersReady &&
    intervalElapsed(millis(), nasLastInterfaceDiscoveryAt,
                    NAS_INTERFACE_REDISCOVERY_INTERVAL_MS);
  if ((!nasCountersReady ||
       nasCounterFailureCount >= NAS_FAILURES_BEFORE_REDISCOVERY ||
       rediscoveryDue) &&
      !discoverNASInterface()) {
    currentNetworkRates = {};
    return false;
  }

  nasRxOctets = UINT64_MAX;
  nasTxOctets = UINT64_MAX;
  ValueCallback* callbacks[] = {cbRxOctets, cbTxOctets};
  if (!requestNAS(callbacks, 2, 2100, 1000)) {
    currentNetworkRates = {};
    if (nasCounterFailureCount < UINT8_MAX) ++nasCounterFailureCount;
    if (shouldInvalidateNetworkSample(
          previousNetworkSample, millis(),
          NAS_NETWORK_SAMPLE_MAX_AGE_MS, nasCounterFailureCount,
          NAS_FAILURES_BEFORE_REDISCOVERY)) {
      previousNetworkSample = {};
      Serial.println("NAS network baseline invalidated after failures");
    }
    Serial.printf("NAS network sample FAILED (%u/%u)\n",
      static_cast<unsigned int>(nasCounterFailureCount),
      static_cast<unsigned int>(NAS_FAILURES_BEFORE_REDISCOVERY));
    return false;
  }

  // Timestamp after the verified response so it reflects this counter sample.
  const NetworkCounterSample sample = {
    nasRxOctets, nasTxOctets, millis(), true
  };
  NetworkRates rates = {};
  calculateNetworkRates(previousNetworkSample, sample, rates);
  previousNetworkSample = sample;
  currentNetworkRates = rates;
  nasCounterFailureCount = 0;
  if (wifiDisconnectNeedsReseed && previousNetworkSample.valid) {
    wifiDisconnectNeedsReseed = false;
    Serial.println("NAS network baseline reseeded after WiFi disconnect");
  }

  Serial.printf(
    "NAS network rx=%llu tx=%llu rx_rate=%llu tx_rate=%llu valid=%d\n",
    static_cast<unsigned long long>(nasRxOctets),
    static_cast<unsigned long long>(nasTxOctets),
    static_cast<unsigned long long>(currentNetworkRates.rx_bytes_per_second),
    static_cast<unsigned long long>(currentNetworkRates.tx_bytes_per_second),
    currentNetworkRates.valid ? 1 : 0);
  return currentNetworkRates.valid;
}

void fetchNAS() {
  Serial.printf("Heap before NAS: %d\n", ESP.getFreeHeap());
  Serial.print("NAS...");

  initializeNASCallbacks();
  const uint32_t previousUptime = g_sysUptime;
  ValueCallback* uptimeCallbacks[] = {cbUptime};
  if (!requestNAS(uptimeCallbacks, 1, 1000, 1000)) {
    g_sysUptime = previousUptime;
    Serial.println(" NAS uptime stale");
  }

  int stagedPoolCount = 0;
  bool volumesComplete = true;
  memset(stagedPools, 0, sizeof(stagedPools));
  for (int i = 0; i < 4; ++i) {
    memset(g_volNameBufs[i], 0, sizeof(g_volNameBufs[i]));
    g_volAlloc[i] = 0;
    g_volTotal[i] = 0;
    g_volUsed[i] = 0;
    ValueCallback* volumeCallbacks[] = {
      cbName[i], cbAlloc[i], cbTotal[i], cbUsed[i]
    };
    const bool volumeComplete =
      requestNAS(volumeCallbacks, 4, 1100 + i, 1000);
    if (!volumeComplete) {
      volumesComplete = false;
      continue;
    }

    if (g_volNames[i][0] != '\0' && stagedPoolCount < 4) {
      if (strncmp(g_volNames[i], "/volume", 7) == 0) {
        snprintf(stagedPools[stagedPoolCount].name,
          sizeof(stagedPools[stagedPoolCount].name),
          "Vol %s", g_volNames[i] + 7);
      } else {
        copyText(stagedPools[stagedPoolCount].name,
          sizeof(stagedPools[stagedPoolCount].name),
          g_volNames[i]);
      }
      stagedPools[stagedPoolCount].status = 1;
      
      double allocUnit = g_volAlloc[i];
      double totalUnits = g_volTotal[i];
      double usedUnits = g_volUsed[i];
      
      float total_tb = (totalUnits * allocUnit) / 1099511627776.0;
      float used_tb = (usedUnits * allocUnit) / 1099511627776.0;
      
      stagedPools[stagedPoolCount].total_tb = total_tb;
      stagedPools[stagedPoolCount].used_tb = used_tb;
      
      if(total_tb > 0) {
        stagedPools[stagedPoolCount].pct = (used_tb / total_tb) * 100;
      } else {
        stagedPools[stagedPoolCount].pct = 0;
      }
      stagedPoolCount++;
    }
  }

  if (volumesComplete) {
    memcpy(pools, stagedPools, sizeof(pools));
    pool_count = stagedPoolCount;
  } else {
    Serial.println("NAS volume refresh incomplete; retaining previous pools");
  }

  Serial.printf(" %d pools\n", pool_count);
  Serial.printf("Heap after NAS: %d\n", ESP.getFreeHeap());
}

// ===== 渲染 =====
char getWeatherChar(int code) {
  if (code == 0) return 'S'; // Sun
  if (code >= 1 && code <= 3) return 'C'; // Cloud
  if (code >= 45 && code <= 48) return 'F'; // Fog
  if (code >= 51 && code <= 67) return 'R'; // Rain
  if (code >= 71 && code <= 77) return 'W'; // Snow
  if (code >= 80 && code <= 82) return 'H'; // Shower
  if (code >= 95) return 'T'; // Storm
  return '?';
}

void drawHeader(int x, int y, int w, const char* title) {
  u8g2Fonts.setFont(u8g2_font_helvB12_tf);
  u8g2Fonts.setCursor(x + 5, y + 20);
  u8g2Fonts.print(title);
  display.drawLine(x, y + 25, x + w, y + 25, GxEPD_BLACK);
}

void drawChineseHeader(int x, int y, int w, const char* title) {
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2Fonts.drawUTF8(x + 5, y + 20, title);
  display.drawLine(x, y + 28, x + w, y + 28, GxEPD_BLACK);
}

void drawCalendar(int x, int y, int w, int h) {
  char header[64] = {};
  if (timeValid) {
    formatChineseCalendarHeader(
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
      timeinfo.tm_mday, timeinfo.tm_wday, header, sizeof(header));
  } else {
    snprintf(header, sizeof(header), "时间不可用");
  }
  drawChineseHeader(x, y, w, header);
  if (!timeValid) return;

  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  struct tm firstDay = timeinfo;
  firstDay.tm_mday = 1;
  mktime(&firstDay);

  const int startX = x + 5;
  const int gridY = y + 54;
  const int cellW = (w - 10) / 7;
  const int cellH = (h - 54) / 6;

  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  for (int column = 0; column < 7; column++) {
    const char* label = chineseWeekdayLabel(column);
    const int labelWidth = u8g2Fonts.getUTF8Width(label);
    u8g2Fonts.drawUTF8(
      startX + column * cellW + (cellW - labelWidth) / 2,
      y + 49, label);
  }

  u8g2Fonts.setFont(u8g2_font_helvB14_tf);
  for (int day = 1; day <= daysInGregorianMonth(year, month); day++) {
    const CalendarCell cell = calendarCellForDay(firstDay.tm_wday, day);
    const int cellX = startX + cell.column * cellW;
    const int cellY = gridY + cell.row * cellH;
    char dayText[3] = {};
    snprintf(dayText, sizeof(dayText), "%d", day);
    const TextPlacement text = centerTextInRect(
      cellX, cellY, cellW, cellH,
      u8g2Fonts.getUTF8Width(dayText),
      u8g2Fonts.getFontAscent(), u8g2Fonts.getFontDescent());

    if (day == timeinfo.tm_mday) {
      display.fillRect(
        cellX + 2, cellY + 2, cellW - 4, cellH - 4, GxEPD_BLACK);
      u8g2Fonts.setForegroundColor(GxEPD_WHITE);
    }
    u8g2Fonts.drawUTF8(text.x, text.baseline_y, dayText);
    if (day == timeinfo.tm_mday) {
      u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    }
  }
}

void drawWeather(int x, int y, int w, int h) {
  char header[48] = {};
  if (timeValid) {
    formatChineseWeatherHeader(
      timeinfo.tm_mon + 1, timeinfo.tm_mday, header, sizeof(header));
  } else {
    snprintf(header, sizeof(header), "今天天气");
  }
  drawChineseHeader(x, y, w, header);

  char buf[80] = {};
  formatChineseWeatherSummary(
    now_weather.temp, now_weather.humidity, now_weather.wind,
    buf, sizeof(buf));
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2Fonts.drawUTF8(x + 5, y + 49, buf);
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  
  // -- 8小时预报折线图 (高度区间 y+55 到 y+128) --
  if (hourly_count > 0) {
    float minT = hourly[0].temp, maxT = hourly[0].temp;
    for (int i=1; i<hourly_count; i++) {
      if (hourly[i].temp < minT) minT = hourly[i].temp;
      if (hourly[i].temp > maxT) maxT = hourly[i].temp;
    }
    if (maxT - minT < 1.0f) { maxT += 1.0f; minT -= 1.0f; }
    
    int cY = y + 55;
    int cH = 73;
    int padT = 15;
    int padB = 15;
    int innerH = cH - padT - padB;
    int stepX = w / hourly_count;
    int offX = x + stepX / 2;
    
    for (int i=0; i<hourly_count-1; i++) {
      int x1 = offX + i*stepX;
      int y1 = cY + padT + innerH - (int)((hourly[i].temp - minT)/(maxT - minT)*innerH);
      int x2 = offX + (i+1)*stepX;
      int y2 = cY + padT + innerH - (int)((hourly[i+1].temp - minT)/(maxT - minT)*innerH);
      display.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
    }
    
    for (int i=0; i<hourly_count; i++) {
      int px = offX + i*stepX;
      int py = cY + padT + innerH - (int)((hourly[i].temp - minT)/(maxT - minT)*innerH);
      display.fillCircle(px, py, 2, GxEPD_BLACK);
      
      sprintf(buf, "%.0f", hourly[i].temp);
      u8g2Fonts.setCursor(px - 5, py - 4);
      u8g2Fonts.print(buf);
      
      char icon = getWeatherChar(hourly[i].code);
      u8g2Fonts.setCursor(px - 3, cY + 12);
      u8g2Fonts.print(icon);
      
      sprintf(buf, "%02d", hourly[i].hour);
      u8g2Fonts.setCursor(px - 5, cY + cH - 2);
      u8g2Fonts.print(buf);
    }
  }
  
  // 画一条横线分隔 8 小时预报和 7 天预报
  display.drawLine(x, y + 128, x + w, y + 128, GxEPD_BLACK);
  
  // -- 7天预报折线图 (高度区间 y+135 到 y+224) --
  if (daily_count > 0) {
    float minT = daily[0].tMin, maxT = daily[0].tMax;
    for (int i=1; i<daily_count; i++) {
      if (daily[i].tMin < minT) minT = daily[i].tMin;
      if (daily[i].tMax > maxT) maxT = daily[i].tMax;
    }
    if (maxT - minT < 1.0f) { maxT += 1.0f; minT -= 1.0f; }
    
    int cY = y + 130;
    int cH = 94; 
    int padT = 15;
    int padB = 25; 
    int innerH = cH - padT - padB;
    int stepX = w / daily_count;
    int offX = x + stepX / 2;
    
    for(int i=0; i<daily_count-1; i++) {
      int x1 = offX + i*stepX;
      int y1 = cY + padT + innerH - (int)((daily[i].tMax - minT)/(maxT - minT)*innerH);
      int x2 = offX + (i+1)*stepX;
      int y2 = cY + padT + innerH - (int)((daily[i+1].tMax - minT)/(maxT - minT)*innerH);
      display.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
    }
    
    for(int i=0; i<daily_count-1; i++) {
      int x1 = offX + i*stepX;
      int y1 = cY + padT + innerH - (int)((daily[i].tMin - minT)/(maxT - minT)*innerH);
      int x2 = offX + (i+1)*stepX;
      int y2 = cY + padT + innerH - (int)((daily[i+1].tMin - minT)/(maxT - minT)*innerH);
      display.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
    }
    
    for(int i=0; i<daily_count; i++) {
      int px = offX + i*stepX;
      int pyMax = cY + padT + innerH - (int)((daily[i].tMax - minT)/(maxT - minT)*innerH);
      int pyMin = cY + padT + innerH - (int)((daily[i].tMin - minT)/(maxT - minT)*innerH);
      
      display.fillCircle(px, pyMax, 2, GxEPD_BLACK);
      display.fillCircle(px, pyMin, 2, GxEPD_BLACK);
      
      sprintf(buf, "%.0f", daily[i].tMax);
      u8g2Fonts.setCursor(px - 5, pyMax - 4);
      u8g2Fonts.print(buf);
      
      sprintf(buf, "%.0f", daily[i].tMin);
      u8g2Fonts.setCursor(px - 5, pyMin + 10);
      u8g2Fonts.print(buf);
      
      char icon = getWeatherChar(daily[i].code);
      u8g2Fonts.setCursor(px - 3, cY + 12);
      u8g2Fonts.print(icon);
      
      u8g2Fonts.setCursor(px - 10, cY + cH - 2);
      u8g2Fonts.print(daily[i].day);
    }
  }
}

void drawPVE(int x, int y, int w, int h) {
  char title[32];
  snprintf(title, sizeof(title), "PVE %s", pve_node.name);
  drawHeader(x, y, w, title);
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);

  u8g2Fonts.setCursor(x + 18, y + 41);
  u8g2Fonts.print("VM");
  u8g2Fonts.setCursor(x + 92, y + 41);
  u8g2Fonts.print("IP");
  u8g2Fonts.setCursor(x + 196, y + 41);
  u8g2Fonts.print("C");
  u8g2Fonts.setCursor(x + 220, y + 41);
  u8g2Fonts.print("MEM");
  display.drawLine(x, y + 45, x + w, y + 45, GxEPD_BLACK);

  const int visible = min(vm_count, static_cast<int>(MAX_VISIBLE_PVE_VMS));
  for (int i = 0; i < visible; i++) {
    int cy = y + 62 + i * 20;
    if (vms[i].running) {
      display.fillCircle(x + 8, cy - 4, 3, GxEPD_BLACK);
    } else {
      display.drawCircle(x + 8, cy - 4, 3, GxEPD_BLACK);
    }

    char name[13];
    copyText(name, sizeof(name), vms[i].name);
    u8g2Fonts.setCursor(x + 18, cy);
    u8g2Fonts.print(name);
    u8g2Fonts.setCursor(x + 92, cy);
    u8g2Fonts.print(vms[i].ip);
    u8g2Fonts.setCursor(x + 196, cy);
    u8g2Fonts.print(vms[i].cpus);

    char buf[20];
    const float usedGb = bytesToGiB(vms[i].mem_bytes);
    const float totalGb = bytesToGiB(vms[i].maxmem_bytes);
    snprintf(buf, sizeof(buf), "%.1f/%.1fG", usedGb, totalGb);
    u8g2Fonts.setCursor(x + 220, cy);
    u8g2Fonts.print(buf);
  }
}

void drawNAS(int x, int y, int w, int h) {
  drawHeader(x, y, w, "Synology NAS");
  int startY = y + 45;
  
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  for (int i = 0; i < pool_count; i++) {
    int cy = startY + i * 40;
    
    u8g2Fonts.setCursor(x + 10, cy);
    u8g2Fonts.print(pools[i].name);
    
    char buf[40];
    char usedStr[16], totalStr[16], freeStr[16];
    
    if (pools[i].used_tb < 1.0) sprintf(usedStr, "%.0fG", pools[i].used_tb * 1024);
    else sprintf(usedStr, "%.1fT", pools[i].used_tb);
    
    if (pools[i].total_tb < 1.0) sprintf(totalStr, "%.0fG", pools[i].total_tb * 1024);
    else sprintf(totalStr, "%.1fT", pools[i].total_tb);
    
    float free_tb = pools[i].total_tb - pools[i].used_tb;
    if (free_tb < 1.0) sprintf(freeStr, "%.0fG", free_tb * 1024);
    else sprintf(freeStr, "%.1fT", free_tb);
    
    sprintf(buf, "%s/%s Free:%s", usedStr, totalStr, freeStr);
    
    // 右对齐文本
    int tw = u8g2Fonts.getUTF8Width(buf);
    u8g2Fonts.setCursor(x + w - tw - 10, cy);
    u8g2Fonts.print(buf);
    
    display.drawRect(x + 10, cy + 5, w - 20, 10, GxEPD_BLACK);
    int barW = (w - 24) * pools[i].pct / 100;
    display.fillRect(x + 12, cy + 7, barW, 6, GxEPD_BLACK);
  }
}



void drawPVEBottomBar() {
  display.fillRect(0, 416, 300, 32, GxEPD_WHITE);

  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  u8g2Fonts.setCursor(10, 436);
  u8g2Fonts.printf("IP:%s", pve_node.ip);

  const float usedGb = bytesToGiB(pve_node.mem_bytes);
  const float totalGb = bytesToGiB(pve_node.maxmem_bytes);
  const uint8_t pct = memoryPercent(pve_node.mem_bytes, pve_node.maxmem_bytes);
  u8g2Fonts.setCursor(132, 436);
  u8g2Fonts.printf("Mem:%.1f/%.1fG %u%%", usedGb, totalGb, pct);
}

void drawNASBottomBar() {
  display.fillRect(301, 416, 299, 32, GxEPD_WHITE);
  display.drawLine(300, 416, 300, 447, GxEPD_BLACK);
  display.drawLine(452, 416, 452, 447, GxEPD_BLACK);

  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

  u8g2Fonts.setCursor(308, 428);
  u8g2Fonts.print("IP:192.168.31.105");

  const uint32_t days = g_sysUptime / (100UL * 60 * 60 * 24);
  u8g2Fonts.setCursor(308, 444);
  u8g2Fonts.printf("Up:%lu d", static_cast<unsigned long>(days));

  char upload[32];
  char download[32];
  formatNetworkRateLines(currentNetworkRates, upload, sizeof(upload),
                         download, sizeof(download));
  u8g2Fonts.setCursor(NAS_SPEED_X + 4, 428);
  u8g2Fonts.print(upload);
  u8g2Fonts.setCursor(NAS_SPEED_X + 4, 444);
  u8g2Fonts.print(download);
}

void renderNASSpeedCanvas() {
  nasSpeedCanvas.fillScreen(1);
  nasSpeedFont.setFontMode(0);
  nasSpeedFont.setFontDirection(0);
  nasSpeedFont.setForegroundColor(0);
  nasSpeedFont.setBackgroundColor(1);
  nasSpeedFont.setFont(u8g2_font_helvR08_tf);

  char upload[32];
  char download[32];
  formatNetworkRateLines(currentNetworkRates, upload, sizeof(upload),
                         download, sizeof(download));
  nasSpeedFont.setCursor(4, 13);
  nasSpeedFont.print(upload);
  nasSpeedFont.setCursor(4, 29);
  nasSpeedFont.print(download);
}

bool refreshNASSpeedWindow() {
  if (!nasSpeedPartialReady) {
    Serial.printf("NAS speed partial not ready heap=%u\n", ESP.getFreeHeap());
    return false;
  }
  renderNASSpeedCanvas();
  const uint32_t started = millis();
  const bool ok = display.epd2.refreshWindow(
    nasSpeedCanvas.getBuffer(), previousNasSpeed,
    sizeof(previousNasSpeed), NAS_SPEED_X, NAS_SPEED_Y,
    NAS_SPEED_WIDTH, NAS_SPEED_HEIGHT);
  Serial.printf("NAS speed partial ok=%d ms=%lu heap=%u\n", ok,
    static_cast<unsigned long>(millis() - started), ESP.getFreeHeap());
  if (!ok) nasSpeedPartialReady = false;
  return ok;
}

bool resumePartialModeAfterFullRefresh() {
  nasSpeedPartialReady = false;
  renderNASSpeedCanvas();
  memcpy(previousNasSpeed, nasSpeedCanvas.getBuffer(),
    sizeof(previousNasSpeed));
  const bool ready = display.epd2.beginFastMode();
  nasSpeedPartialReady = ready;
  Serial.printf("NAS speed partial mode ok=%d heap=%u\n",
    nasSpeedPartialReady, ESP.getFreeHeap());
  return nasSpeedPartialReady;
}

bool renderAll() {
  display.epd2.prepareFullRefresh();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    
    display.drawLine(300, 0, 300, 447, GxEPD_BLACK);
    display.drawLine(0, 224, 600, 224, GxEPD_BLACK);
    display.drawLine(0, 415, 600, 415, GxEPD_BLACK);
    
    drawCalendar(0, 0, 300, 224);
    drawWeather(300, 0, 300, 224);
    drawPVE(0, 224, 300, 191);
    drawNAS(300, 224, 300, 191);

    drawPVEBottomBar();
    drawNASBottomBar();
  } while (display.nextPage());
  return display.epd2.lastFullRefreshSucceeded();
}

bool fullRefreshGuardOpen(uint32_t now) {
  return !fullAttemptRecorded || intervalElapsed(
    now, lastFullAttemptCompletedMs, FULL_RECOVERY_BACKOFF_MS);
}

void recordFullAttemptCompletion() {
  lastFullAttemptCompletedMs = millis();
  fullAttemptRecorded = true;
}

bool updateWiFiContinuity(bool stayedConnected, const char* checkpoint) {
  const bool disconnectObserved = consumeWiFiDisconnectEvent(checkpoint);
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) markWiFiConnectedObserved();
  return stayedConnected && !disconnectObserved && connected;
}

bool refreshFullDashboard(const char* reason) {
  Serial.printf("Full refresh reason=%s\n", reason ? reason : "unspecified");
  displayReady = false;
  nasSpeedPartialReady = false;
  const bool wifiConnectedAtStart = WiFi.status() == WL_CONNECTED;
  if (wifiConnectedAtStart) markWiFiConnectedObserved();
  bool wifiStayedConnected = wifiConnectedAtStart;

  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "full-start");
  syncTime();
  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "after-ntp");
  fetchWeather();
  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "after-weather");
  fetchPVE();
  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "after-pve");
  fetchNAS();
  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "after-nas");

  const bool wifiConnectedAfterFetch = WiFi.status() == WL_CONNECTED;
  if (wifiConnectedAtStart && !wifiStayedConnected) {
    previousNetworkSample = {};
    currentNetworkRates = {};
    if (nasCounterFailureCount < NAS_FAILURES_BEFORE_REDISCOVERY) {
      nasCounterFailureCount = NAS_FAILURES_BEFORE_REDISCOVERY;
    }
    Serial.println("WiFi lost during full refresh; NAS baseline invalidated");
  }

  if (shouldInvalidateNetworkSample(
        previousNetworkSample, millis(), NAS_NETWORK_SAMPLE_MAX_AGE_MS,
        nasCounterFailureCount, NAS_FAILURES_BEFORE_REDISCOVERY)) {
    previousNetworkSample = {};
    currentNetworkRates = {};
    Serial.println("NAS network baseline expired during full refresh");
  }

  if (wifiConnectedAfterFetch && !previousNetworkSample.valid) {
    Serial.println("NAS network baseline seed");
    sampleNASNetwork();
  }

  const bool fullRenderOk = renderAll();
  if (!fullRenderOk) {
    updateWiFiContinuity(wifiStayedConnected, "after-full-render");
    lastNetworkRefreshMs = millis();
    Serial.printf("Full refresh FAILED reason=%s\n",
      reason ? reason : "unspecified");
    logHeap("full failed");
    return false;
  }

  displayReady = resumePartialModeAfterFullRefresh();
  updateWiFiContinuity(wifiStayedConnected, "after-full-render");
  lastNetworkRefreshMs = millis();
  Serial.printf("Full refresh complete reason=%s ready=%d\n",
    reason ? reason : "unspecified", displayReady ? 1 : 0);
  logHeap("full refresh");
  return displayReady;
}

bool attemptFullDashboard(const char* reason, bool resetPeriodicSchedule) {
  const uint32_t startedAt = millis();
  if (resetPeriodicSchedule) {
    lastFullRefreshMs = startedAt;
  }

  if (!fullRefreshGuardOpen(startedAt)) {
    dataRefreshPending = true;
    Serial.printf("Full refresh deferred reason=%s guard_remaining_ms=%lu\n",
      reason ? reason : "unspecified",
      static_cast<unsigned long>(FULL_RECOVERY_BACKOFF_MS -
        static_cast<uint32_t>(startedAt - lastFullAttemptCompletedMs)));
    return false;
  }

  dataRefreshPending = false;
  if (resetPeriodicSchedule) wifiReconnectRefreshPending = false;
  const bool ok = refreshFullDashboard(reason);
  recordFullAttemptCompletion();
  recoveryPending = !ok;
  return ok;
}

bool recoverCachedDashboard(const char* reason) {
  displayReady = false;
  nasSpeedPartialReady = false;
  currentNetworkRates = {};
  recoveryPending = true;
  if (WiFi.status() == WL_CONNECTED) markWiFiConnectedObserved();

  const uint32_t startedAt = millis();
  if (!fullRefreshGuardOpen(startedAt)) {
    Serial.printf("Cached full recovery deferred reason=%s guard_remaining_ms=%lu\n",
      reason ? reason : "unspecified",
      static_cast<unsigned long>(FULL_RECOVERY_BACKOFF_MS -
        static_cast<uint32_t>(startedAt - lastFullAttemptCompletedMs)));
    return false;
  }

  Serial.printf("Full refresh reason=%s source=cached\n",
    reason ? reason : "unspecified");
  consumeWiFiDisconnectEvent("cached-recovery-start");
  const bool fullRenderOk = renderAll();
  consumeWiFiDisconnectEvent("cached-recovery-render");

  bool ok = false;
  if (fullRenderOk) {
    displayReady = resumePartialModeAfterFullRefresh();
    ok = displayReady;
  } else {
    Serial.printf("Full refresh FAILED reason=%s source=cached\n",
      reason ? reason : "unspecified");
  }

  lastNetworkRefreshMs = millis();
  recordFullAttemptCompletion();
  recoveryPending = !ok;
  Serial.printf("Cached full recovery complete reason=%s ready=%d\n",
    reason ? reason : "unspecified", displayReady ? 1 : 0);
  logHeap(ok ? "cached recovery" : "cached failed");
  return ok;
}

void setup() {
  Serial.begin(115200);

  if (nasSpeedCanvas.getBuffer() == nullptr) {
    Serial.println("NAS speed canvas allocation FAILED");
    return;
  }

  wifiDisconnectHandler =
    WiFi.onStationModeDisconnected(onWiFiStationDisconnected);
  lastWifiRetryMs = millis();
  connectWifi();
  display.init(115200, true, 2, false);
  u8g2Fonts.begin(display);
  nasSpeedFont.begin(nasSpeedCanvas);

  initializeNASCallbacks();
  wifiWasConnected = WiFi.status() == WL_CONNECTED;
  if (wifiWasConnected) {
    lastNetworkRefreshMs = millis();
    sampleNASNetwork();
  }

  attemptFullDashboard("startup", true);
  Serial.flush();
}

void loop() {
  consumeWiFiDisconnectEvent("loop");
  const bool wifiConnectedNow = WiFi.status() == WL_CONNECTED;
  if (wifiConnectedNow) markWiFiConnectedObserved();
  if (!wifiConnectedNow) {
    if (wifiWasConnected) {
      Serial.println("WiFi disconnected; invalidating NAS rate baseline");
      wifiWasConnected = false;
      clearWiFiConnectedObserved();
      wifiDisconnectNeedsReseed = true;
      wifiReconnectRefreshPending = true;
      dataRefreshPending = true;
      previousNetworkSample = {};
      currentNetworkRates = {};
      if (nasCounterFailureCount < NAS_FAILURES_BEFORE_REDISCOVERY) {
        nasCounterFailureCount = NAS_FAILURES_BEFORE_REDISCOVERY;
      }
      lastWifiRetryMs = millis();
    }

    if (displayReady && !offlineRatesDisplayed) {
      currentNetworkRates = {};
      offlineRatesDisplayed = true;
      if (!refreshNASSpeedWindow()) {
        displayReady = false;
        Serial.println("Offline NAS speed partial FAILED");
      }
    }

    if (intervalElapsed(millis(), lastWifiRetryMs,
                        WIFI_RETRY_INTERVAL_MS)) {
      Serial.println("WiFi retry");
      lastWifiRetryMs = millis();
      connectWifi();
    }
    delay(50);
    yield();
    return;
  }

  if (!wifiWasConnected) {
    Serial.println("WiFi reconnected; reseeding NAS rate baseline");
    wifiWasConnected = true;
    offlineRatesDisplayed = false;
    previousNetworkSample = {};
    currentNetworkRates = {};
    if (nasCounterFailureCount < NAS_FAILURES_BEFORE_REDISCOVERY) {
      nasCounterFailureCount = NAS_FAILURES_BEFORE_REDISCOVERY;
    }
    lastNetworkRefreshMs = millis();
    sampleNASNetwork();
    lastFullRefreshMs = millis();
    wifiReconnectRefreshPending = true;
    dataRefreshPending = true;
    delay(50);
    yield();
    return;
  }

  const uint32_t now = millis();
  const DashboardAction action = chooseConnectedDashboardAction(
    now, lastFullRefreshMs, lastNetworkRefreshMs,
    lastFullAttemptCompletedMs, fullAttemptRecorded, displayReady,
    recoveryPending, dataRefreshPending, FULL_REFRESH_INTERVAL_MS,
    NAS_SPEED_REFRESH_INTERVAL_MS, FULL_RECOVERY_BACKOFF_MS);
  if (action == DASHBOARD_PERIODIC_FULL) {
    attemptFullDashboard(
      wifiReconnectRefreshPending ? "wifi-reconnected" : "scheduled", true);
  } else if (action == DASHBOARD_RECOVERY_FULL) {
    recoverCachedDashboard("readiness-recovery");
  } else if (action == DASHBOARD_NETWORK_SAMPLE) {
    lastNetworkRefreshMs = now;
    sampleNASNetwork();
    if (!refreshNASSpeedWindow()) {
      Serial.println("NAS speed partial failed; starting full recovery");
      recoverCachedDashboard("partial-recovery");
    }
  }

  delay(25);
  yield();
}
