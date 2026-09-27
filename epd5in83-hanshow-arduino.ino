#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Arduino_SNMP_Manager.h>
#include "dashboard_model.h"
#include "GxEPD2_583_DeepBlack.h"
#include "secrets.h"

// ===== 配置 =====
constexpr char DEFAULT_PVE_HOST[] = "192.168.31.34";
constexpr int DEFAULT_PVE_PORT = 8006;
constexpr char DEFAULT_PVE_CERT_FINGERPRINT[] =
  "32:2A:C0:E1:C4:73:01:56:33:7D:CD:72:5C:19:72:DD:37:08:EA:C9";

char PVE_HOST[64] = {};
int PVE_PORT = DEFAULT_PVE_PORT;
char PVE_CERT_FINGERPRINT[80] = {};

IPAddress nas_ip(192, 168, 31, 105);

constexpr int MAX_NAS_VOLUMES = 4;
char configuredWifiSsid[64] = {};
char configuredWifiPass[64] = {};
char configuredSnmpCommunity[64] = {};
char configuredPveToken[192] = {};
char configuredNasIp[16] = {};
char configuredWeatherLatitude[16] = "22.5431";
char configuredWeatherLongitude[16] = "114.0579";
char configuredTimezone[32] = "Asia%2FShanghai";
long configuredTimezoneOffset = 28800;
bool configStorageReady = false;
bool configServerReady = false;
ESP8266WebServer configServer(80);

constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 600000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t FULL_RECOVERY_BACKOFF_MS = 60000;

// ===== 数据结构 =====
struct WeatherNow {
  float temp;
  float min_temp;
  float max_temp;
  int code;
  char updated_at[6];
  bool range_valid;
};
struct HourlyWeather { int hour; float temp; int code; };
struct PoolInfo {
  char name[20];
  int status;
  float used_tb;
  float total_tb;
  int pct;
  int free_pct;
};

struct DataSnapshot {
  bool valid;
  bool cached;
  bool error;
  struct tm updated_at;
  bool updated_at_valid;
};

// ===== 全局数据 =====
WeatherNow now_weather;
HourlyWeather hourly[8];
int hourly_count = 0;
PveNodeInfo pve_node;
VMInfo vms[MAX_PVE_VMS];
int vm_count = 0;
PveNodeInfo previousPveNode;
VMInfo previousPveVMs[MAX_PVE_VMS];
int previousPveVMCount = 0;
bool pveDataValid = false;
PoolInfo pools[MAX_NAS_VOLUMES];
int pool_count = 0;
PoolInfo stagedPools[MAX_NAS_VOLUMES];
uint32_t g_sysUptime = 0;
struct tm timeinfo;
bool timeValid = false;
struct tm currentRefreshDataTime;
bool currentRefreshDataTimeValid = false;
DataSnapshot pveSnapshot = {};
DataSnapshot nasSnapshot = {};
struct tm nextDashboardRefreshTime;
bool nextDashboardRefreshTimeValid = false;
uint32_t min_free_heap = UINT32_MAX;

// ===== GxEPD2 显示器 =====
// 将缓冲从全屏(HEIGHT, 33.6KB)改为32行(2.4KB)，解决 OOM 崩溃问题
GxEPD2_BW<GxEPD2_583_DeepBlack, 32> display(
  GxEPD2_583_DeepBlack(15, 0, 2, 4));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
uint32_t lastFullRefreshMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastFullAttemptCompletedMs = 0;
bool displayReady = false;
bool wifiWasConnected = false;
bool recoveryPending = false;
bool dataRefreshPending = false;
bool fullAttemptRecorded = false;
bool wifiReconnectRefreshPending = false;
volatile bool wifiConnectedObserved = false;
volatile bool wifiDisconnectEventRaised = false;
volatile bool wifiOfflineDisconnectEventRaised = false;
WiFiEventHandler wifiDisconnectHandler;
IPAddress lastDeviceIP;
bool deviceIPValid = false;

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
  return true;
}

// ===== WiFi/NTP =====
void rememberDeviceIPAddress() {
  const IPAddress currentIP = WiFi.localIP();
  const bool nonZero = currentIP[0] != 0 || currentIP[1] != 0 ||
                       currentIP[2] != 0 || currentIP[3] != 0;
  if (!nonZero) return;
  lastDeviceIP = currentIP;
  deviceIPValid = true;
}

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
  if (connected) {
    markWiFiConnectedObserved();
    rememberDeviceIPAddress();
  }
  Serial.println(connected ? " OK" : " FAILED");
  if (connected) {
    Serial.print("Device IP: ");
    Serial.println(lastDeviceIP);
  }
}

bool syncTime() {
  Serial.print("NTP sync");
  configTime(configuredTimezoneOffset, 0, "ntp.aliyun.com",
             "cn.pool.ntp.org", "pool.ntp.org");
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

bool captureDashboardTimes() {
  const time_t now = time(nullptr);
  struct tm refreshedTime;
  if (now < 1000000000UL ||
      localtime_r(&now, &refreshedTime) == nullptr) {
    Serial.println("Dashboard refresh time unavailable");
    nextDashboardRefreshTimeValid = false;
    return false;
  }
  timeinfo = refreshedTime;
  timeValid = true;
  const uint32_t remainingMs = remainingIntervalMs(
    millis(), lastFullRefreshMs, FULL_REFRESH_INTERVAL_MS);
  const time_t nextRefreshEpoch = now + static_cast<time_t>(
    (remainingMs + 999UL) / 1000UL);
  if (localtime_r(
        &nextRefreshEpoch, &nextDashboardRefreshTime) == nullptr) {
    nextDashboardRefreshTimeValid = false;
    Serial.println("Next dashboard refresh time unavailable");
    return false;
  }
  nextDashboardRefreshTimeValid = true;
  Serial.printf(
    "Dashboard refresh time %02d:%02d next=%02d:%02d\n",
    refreshedTime.tm_hour, refreshedTime.tm_min,
    nextDashboardRefreshTime.tm_hour,
    nextDashboardRefreshTime.tm_min);
  return true;
}

// ===== 数据获取 =====
void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  strncpy(destination, source ? source : "", capacity - 1);
  destination[capacity - 1] = '\0';
}

void setConfigDefaults() {
  copyText(PVE_HOST, sizeof(PVE_HOST), DEFAULT_PVE_HOST);
  PVE_PORT = DEFAULT_PVE_PORT;
  copyText(PVE_CERT_FINGERPRINT, sizeof(PVE_CERT_FINGERPRINT),
           DEFAULT_PVE_CERT_FINGERPRINT);
  copyText(configuredWifiSsid, sizeof(configuredWifiSsid), WIFI_SSID);
  copyText(configuredWifiPass, sizeof(configuredWifiPass), WIFI_PASS);
  copyText(configuredSnmpCommunity, sizeof(configuredSnmpCommunity),
           SNMP_COMMUNITY);
  copyText(configuredPveToken, sizeof(configuredPveToken), PVE_TOKEN);
  copyText(configuredNasIp, sizeof(configuredNasIp), "192.168.31.105");
  configuredTimezoneOffset = 28800;
}

void applyRuntimeConfig() {
  WIFI_SSID = configuredWifiSsid;
  WIFI_PASS = configuredWifiPass;
  SNMP_COMMUNITY = configuredSnmpCommunity;
  PVE_TOKEN = configuredPveToken;
  IPAddress parsedNasIp;
  if (parsedNasIp.fromString(configuredNasIp)) nas_ip = parsedNasIp;
}

void assignConfigField(const String& key, const String& value) {
  if (key == "wifi_ssid") {
    copyText(configuredWifiSsid, sizeof(configuredWifiSsid), value.c_str());
  } else if (key == "wifi_pass") {
    copyText(configuredWifiPass, sizeof(configuredWifiPass), value.c_str());
  } else if (key == "snmp_community") {
    copyText(configuredSnmpCommunity, sizeof(configuredSnmpCommunity), value.c_str());
  } else if (key == "pve_token") {
    copyText(configuredPveToken, sizeof(configuredPveToken), value.c_str());
  } else if (key == "pve_host") {
    copyText(PVE_HOST, sizeof(PVE_HOST), value.c_str());
  } else if (key == "pve_port") {
    const int parsed = value.toInt();
    if (parsed > 0 && parsed < 65536) PVE_PORT = parsed;
  } else if (key == "pve_fingerprint") {
    copyText(PVE_CERT_FINGERPRINT, sizeof(PVE_CERT_FINGERPRINT), value.c_str());
  } else if (key == "nas_ip") {
    copyText(configuredNasIp, sizeof(configuredNasIp), value.c_str());
  } else if (key == "weather_latitude") {
    copyText(configuredWeatherLatitude, sizeof(configuredWeatherLatitude), value.c_str());
  } else if (key == "weather_longitude") {
    copyText(configuredWeatherLongitude, sizeof(configuredWeatherLongitude), value.c_str());
  } else if (key == "timezone") {
    copyText(configuredTimezone, sizeof(configuredTimezone), value.c_str());
  } else if (key == "timezone_offset") {
    configuredTimezoneOffset = value.toInt();
  }
}

void loadRuntimeConfig() {
  setConfigDefaults();
  configStorageReady = LittleFS.begin();
  if (!configStorageReady) {
    Serial.println("LittleFS mount failed; formatting configuration storage");
    configStorageReady = LittleFS.format() && LittleFS.begin();
  }
  if (configStorageReady) {
    File configFile = LittleFS.open("/dashboard.cfg", "r");
    if (configFile) {
      while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        line.trim();
        const int separator = line.indexOf('=');
        if (separator <= 0) continue;
        assignConfigField(line.substring(0, separator),
                          line.substring(separator + 1));
      }
      configFile.close();
    }
  }
  applyRuntimeConfig();
}

bool saveRuntimeConfig() {
  if (!configStorageReady) return false;
  File configFile = LittleFS.open("/dashboard.cfg", "w");
  if (!configFile) return false;
  configFile.printf("wifi_ssid=%s\n", configuredWifiSsid);
  configFile.printf("wifi_pass=%s\n", configuredWifiPass);
  configFile.printf("snmp_community=%s\n", configuredSnmpCommunity);
  configFile.printf("pve_token=%s\n", configuredPveToken);
  configFile.printf("pve_host=%s\n", PVE_HOST);
  configFile.printf("pve_port=%d\n", PVE_PORT);
  configFile.printf("pve_fingerprint=%s\n", PVE_CERT_FINGERPRINT);
  configFile.printf("nas_ip=%s\n", configuredNasIp);
  configFile.printf("weather_latitude=%s\n", configuredWeatherLatitude);
  configFile.printf("weather_longitude=%s\n", configuredWeatherLongitude);
  configFile.printf("timezone=%s\n", configuredTimezone);
  configFile.printf("timezone_offset=%ld\n", configuredTimezoneOffset);
  configFile.close();
  return true;
}

void markSnapshotSuccess(DataSnapshot& snapshot) {
  snapshot.valid = true;
  snapshot.cached = false;
  snapshot.error = false;
  if (currentRefreshDataTimeValid) {
    snapshot.updated_at = currentRefreshDataTime;
    snapshot.updated_at_valid = true;
  }
}

void markSnapshotFailure(DataSnapshot& snapshot) {
  snapshot.cached = snapshot.valid;
  snapshot.error = !snapshot.valid;
}

void formatSnapshotStatus(const DataSnapshot& snapshot,
                          char* buffer, size_t bufferSize) {
  const char* prefix = snapshot.cached ? "缓存" :
    (snapshot.error ? "异常" : "更新");
  if (!snapshot.updated_at_valid) {
    snprintf(buffer, bufferSize, "%s --:--", prefix);
    return;
  }
  snprintf(buffer, bufferSize, "%s %02d:%02d", prefix,
           snapshot.updated_at.tm_hour, snapshot.updated_at.tm_min);
}

String htmlEscape(const char* value) {
  String escaped;
  escaped.reserve(strlen(value ? value : "") + 8);
  for (const char* cursor = value ? value : ""; *cursor; ++cursor) {
    switch (*cursor) {
      case '&': escaped += F("&amp;"); break;
      case '<': escaped += F("&lt;"); break;
      case '>': escaped += F("&gt;"); break;
      case '"': escaped += F("&quot;"); break;
      case '\'': escaped += F("&#39;"); break;
      default: escaped += *cursor; break;
    }
  }
  return escaped;
}

void sendConfigPage() {
  configServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  configServer.send(200, "text/html; charset=utf-8", "");
  configServer.sendContent(F("<!doctype html><html lang='zh-CN'><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP 5.83 看板配置</title><style>"
    "body{margin:0;background:#f2f4f7;color:#17202a;font:16px system-ui,sans-serif}"
    ".wrap{max-width:720px;margin:auto;padding:24px}.card{background:#fff;"
    "border:1px solid #d8dee6;border-radius:14px;padding:20px;margin:12px 0;"
    "box-shadow:0 4px 16px #17202a12}h1{margin:0 0 6px;font-size:24px}"
    "h2{font-size:17px;margin:0 0 14px}.muted{color:#5f6b76;font-size:13px}"
    "label{display:block;font-weight:600;margin:12px 0 5px}input{box-sizing:border-box;"
    "width:100%;padding:10px 12px;border:1px solid #b8c2cc;border-radius:8px;"
    "font:inherit}button{margin-top:18px;background:#17202a;color:#fff;border:0;"
    "border-radius:8px;padding:11px 16px;font:inherit;cursor:pointer}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}@media(max-width:560px){"
    ".grid{grid-template-columns:1fr}}</style></head><body><main class='wrap'>"
    "<div class='card'><h1>ESP 5.83 看板</h1><div class='muted'>仅限局域网访问。保存后设备会自动重启。</div></div>"
    "<form method='post' action='/save'><div class='card'><h2>网络</h2>"));
  configServer.sendContent(F("<label>Wi‑Fi 名称</label><input name='wifi_ssid' value='"));
  configServer.sendContent(htmlEscape(configuredWifiSsid));
  configServer.sendContent(F("'><label>Wi‑Fi 密码</label><input type='password' name='wifi_pass' placeholder='留空保持不变'>"
    "</div><div class='card'><h2>PVE</h2><div class='grid'><div><label>地址</label><input name='pve_host' value='"));
  configServer.sendContent(htmlEscape(PVE_HOST));
  configServer.sendContent(F("'></div><div><label>端口</label><input name='pve_port' type='number' value='"));
  configServer.sendContent(String(PVE_PORT));
  configServer.sendContent(F("'></div></div><label>API Token</label><input type='password' name='pve_token' placeholder='留空保持不变'>"
    "<label>证书指纹</label><input name='pve_fingerprint' value='"));
  configServer.sendContent(htmlEscape(PVE_CERT_FINGERPRINT));
  configServer.sendContent(F("'></div><div class='card'><h2>群晖 NAS</h2><div class='grid'><div><label>地址</label><input name='nas_ip' value='"));
  configServer.sendContent(htmlEscape(configuredNasIp));
  configServer.sendContent(F("'></div><div><label>SNMP Community</label><input type='password' name='snmp_community' placeholder='留空保持不变'></div></div></div>"
    "<div class='card'><h2>天气和时间</h2><div class='grid'><div><label>纬度</label><input name='weather_latitude' value='"));
  configServer.sendContent(htmlEscape(configuredWeatherLatitude));
  configServer.sendContent(F("'></div><div><label>经度</label><input name='weather_longitude' value='"));
  configServer.sendContent(htmlEscape(configuredWeatherLongitude));
  configServer.sendContent(F("'></div></div><label>时区偏移（秒）</label><input name='timezone_offset' type='number' value='"));
  configServer.sendContent(String(configuredTimezoneOffset));
  configServer.sendContent(F("'></div><button type='submit'>保存并重启</button></form>"
    "<div class='card'><a href='/refresh'>立即刷新看板</a></div></main></body></html>"));
}

void handleConfigSave() {
  if (configServer.hasArg("wifi_ssid")) {
    copyText(configuredWifiSsid, sizeof(configuredWifiSsid),
             configServer.arg("wifi_ssid").c_str());
  }
  if (configServer.hasArg("wifi_pass") && configServer.arg("wifi_pass").length() > 0) {
    copyText(configuredWifiPass, sizeof(configuredWifiPass),
             configServer.arg("wifi_pass").c_str());
  }
  if (configServer.hasArg("snmp_community") && configServer.arg("snmp_community").length() > 0) {
    copyText(configuredSnmpCommunity, sizeof(configuredSnmpCommunity),
             configServer.arg("snmp_community").c_str());
  }
  if (configServer.hasArg("pve_token") && configServer.arg("pve_token").length() > 0) {
    copyText(configuredPveToken, sizeof(configuredPveToken),
             configServer.arg("pve_token").c_str());
  }
  const char* fields[] = {
    "pve_host", "pve_port", "pve_fingerprint", "nas_ip",
    "weather_latitude", "weather_longitude", "timezone_offset"
  };
  for (const char* field : fields) {
    if (configServer.hasArg(field)) assignConfigField(field, configServer.arg(field));
  }
  applyRuntimeConfig();
  const bool saved = saveRuntimeConfig();
  configServer.send(200, "text/html; charset=utf-8",
    saved ? "<p>配置已保存，设备即将重启。</p>"
          : "<p>配置保存失败，请检查文件系统。</p>");
  if (saved) {
    delay(500);
    ESP.restart();
  }
}

void handleConfigRefresh() {
  dataRefreshPending = true;
  configServer.send(200, "text/plain; charset=utf-8",
                    "已安排下一次完整刷新，请返回看板查看结果。\n");
}

void startWebServices() {
  if (configServerReady) return;
  configServer.on("/", HTTP_GET, sendConfigPage);
  configServer.on("/save", HTTP_POST, handleConfigSave);
  configServer.on("/refresh", HTTP_GET, handleConfigRefresh);
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("esp583")) {
      Serial.println("Config page: http://esp583.local/");
    }
  } else {
    WiFi.mode(WIFI_AP_STA);
    char apName[32] = {};
    snprintf(apName, sizeof(apName), "ESP583-Setup-%06X",
             static_cast<unsigned int>(ESP.getChipId() & 0xFFFFFF));
    WiFi.softAP(apName);
    Serial.printf("Config AP: %s http://%s/\n", apName,
                  WiFi.softAPIP().toString().c_str());
  }
  configServer.begin();
  configServerReady = true;
}

void serviceWebServices() {
  if (!configServerReady) return;
  configServer.handleClient();
  if (WiFi.status() == WL_CONNECTED) MDNS.update();
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
  char url[256] = {};
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
    "&current=temperature_2m,weather_code&hourly=temperature_2m,weather_code"
    "&daily=temperature_2m_max,temperature_2m_min&timezone=%s"
    "&forecast_hours=8&forecast_days=1",
    configuredWeatherLatitude, configuredWeatherLongitude, configuredTimezone);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    Serial.printf(" HTTP %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      
      JsonDocument filter;
      filter["current"]["time"] = true;
      filter["current"]["temperature_2m"] = true;
      filter["current"]["weather_code"] = true;
      filter["hourly"]["time"] = true;
      filter["hourly"]["temperature_2m"] = true;
      filter["hourly"]["weather_code"] = true;
      filter["daily"]["temperature_2m_max"] = true;
      filter["daily"]["temperature_2m_min"] = true;
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
      
      if (error) {
        Serial.printf("JSON parse failed: %s\n", error.c_str());
      } else {
        now_weather.temp     = doc["current"]["temperature_2m"] | 0.0f;
        now_weather.code     = doc["current"]["weather_code"] | 0;
        const char* observedAt = doc["current"]["time"] | "";
        if (strlen(observedAt) >= 16) {
          snprintf(now_weather.updated_at, sizeof(now_weather.updated_at),
                   "%c%c:%c%c", observedAt[11], observedAt[12],
                   observedAt[14], observedAt[15]);
        } else if (timeValid) {
          snprintf(now_weather.updated_at, sizeof(now_weather.updated_at),
                   "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        } else {
          copyText(now_weather.updated_at, sizeof(now_weather.updated_at), "--:--");
        }

        JsonArray maximumArray = doc["daily"]["temperature_2m_max"];
        JsonArray minimumArray = doc["daily"]["temperature_2m_min"];
        now_weather.range_valid =
          maximumArray.size() > 0 && minimumArray.size() > 0;
        if (now_weather.range_valid) {
          now_weather.max_temp = maximumArray[0] | now_weather.temp;
          now_weather.min_temp = minimumArray[0] | now_weather.temp;
        } else {
          now_weather.max_temp = now_weather.temp;
          now_weather.min_temp = now_weather.temp;
        }

        JsonArray timeArray = doc["hourly"]["time"];
        JsonArray temperatureArray = doc["hourly"]["temperature_2m"];
        JsonArray codeArray = doc["hourly"]["weather_code"];
        hourly_count = 0;
        for (size_t i = 0; i < timeArray.size() && hourly_count < 8; ++i) {
          const char* forecastAt = timeArray[i];
          if (!forecastAt || strlen(forecastAt) < 16) continue;
          char hourText[3] = {forecastAt[11], forecastAt[12], '\0'};
          hourly[hourly_count].hour = atoi(hourText);
          hourly[hourly_count].temp = temperatureArray[i] | 0.0f;
          hourly[hourly_count].code = codeArray[i] | 0;
          ++hourly_count;
        }
      }
      Serial.printf(" %.1fC min=%.1f max=%.1f code=%d updated=%s hourly=%d\n",
                    now_weather.temp, now_weather.min_temp,
                    now_weather.max_temp, now_weather.code,
                    now_weather.updated_at, hourly_count);
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

bool fetchPVE() {
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
    markSnapshotFailure(pveSnapshot);
    logHeap("after PVE");
    return false;
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
  markSnapshotSuccess(pveSnapshot);
  Serial.printf("PVE node=%s %s, VMs=%d\n", pve_node.name,
    nodeOk ? "OK" : "FAILED", vmOk ? vm_count : 0);
  logHeap("after PVE");
  return true;
}

// 全局缓冲，防止底层库持有栈内指针导致崩溃
int g_volAlloc[MAX_NAS_VOLUMES] = {};
int g_volTotal[MAX_NAS_VOLUMES] = {};
int g_volUsed[MAX_NAS_VOLUMES] = {};
char g_oidAlloc[MAX_NAS_VOLUMES][64];
char g_oidTotal[MAX_NAS_VOLUMES][64];
char g_oidUsed[MAX_NAS_VOLUMES][64];

static WiFiUDP nasUdp;
static SNMPManager nasSnmp(SNMP_COMMUNITY);
bool nasCallbacksReady = false;
ValueCallback* cbUptime = nullptr;
ValueCallback* cbAlloc[MAX_NAS_VOLUMES] = {};
ValueCallback* cbTotal[MAX_NAS_VOLUMES] = {};
ValueCallback* cbUsed[MAX_NAS_VOLUMES] = {};
uint32_t nasRequestGeneration = 0;

static const size_t MAX_NAS_REQUEST_CALLBACKS = 4;
static const uint16_t NAS_REQUEST_PORT_BASE = 49152;
static const uint16_t NAS_REQUEST_PORT_COUNT = 16384;

bool consumeWiFiDisconnectEvent(const char* checkpoint) {
  if (!latchWiFiDisconnectEvent()) return false;

  wifiWasConnected = false;
  wifiReconnectRefreshPending = true;
  dataRefreshPending = true;
  lastWifiRetryMs = wifiRetryAnchorAfterDisconnectEvent(
    lastWifiRetryMs, millis(), true);
  Serial.printf("WiFi disconnect event at %s; full refresh scheduled\n",
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

struct NASDiscoveredVolume {
  uint32_t index;
  char name[20];
};

constexpr char NAS_STORAGE_DESCR_OID[] = "1.3.6.1.2.1.25.2.3.1.3";
constexpr char NAS_STORAGE_ALLOC_OID[] = "1.3.6.1.2.1.25.2.3.1.4";
constexpr char NAS_STORAGE_SIZE_OID[] = "1.3.6.1.2.1.25.2.3.1.5";
constexpr char NAS_STORAGE_USED_OID[] = "1.3.6.1.2.1.25.2.3.1.6";

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
bool decodeNASOIDText(const NASBERTLV& oid, char* output, size_t capacity);
bool encodeNASGetNextRequest(const char* oid, uint16_t requestId,
                             uint8_t* buffer, size_t capacity,
                             size_t& offset, size_t& length);
bool requestNASGetNext(const char* oid, uint16_t requestId,
                       char* nextOid, size_t nextOidCapacity,
                       char* stringValue, size_t stringValueCapacity,
                       uint32_t timeoutMs);
bool discoverNASVolumes(NASDiscoveredVolume* volumes, size_t capacity,
                        size_t& count);

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

bool readNASOIDSubidentifier(const uint8_t* bytes, size_t length,
                             size_t& offset, uint32_t& value) {
  value = 0;
  uint8_t groups = 0;
  while (offset < length && groups < 5) {
    const uint8_t encoded = bytes[offset++];
    if (value > (UINT32_MAX >> 7)) return false;
    value = (value << 7) | (encoded & 0x7F);
    ++groups;
    if ((encoded & 0x80) == 0) return true;
  }
  return false;
}

bool appendNASOIDArc(char* output, size_t capacity, size_t& used,
                     uint32_t arc, bool first) {
  const int written = snprintf(output + used, capacity - used,
                               first ? "%lu" : ".%lu",
                               static_cast<unsigned long>(arc));
  if (written < 0 || static_cast<size_t>(written) >= capacity - used) {
    return false;
  }
  used += static_cast<size_t>(written);
  return true;
}

bool decodeNASOIDText(const NASBERTLV& oid, char* output, size_t capacity) {
  if (output == nullptr || capacity == 0 || oid.tag != OID ||
      oid.length == 0) return false;
  output[0] = '\0';
  size_t offset = 0;
  uint32_t combined = 0;
  if (!readNASOIDSubidentifier(oid.value, oid.length, offset, combined)) {
    return false;
  }
  const uint32_t first = combined < 40 ? 0 : (combined < 80 ? 1 : 2);
  const uint32_t second = first < 2 ? combined - first * 40 : combined - 80;
  size_t used = 0;
  if (!appendNASOIDArc(output, capacity, used, first, true) ||
      !appendNASOIDArc(output, capacity, used, second, false)) {
    return false;
  }
  while (offset < oid.length) {
    uint32_t arc = 0;
    if (!readNASOIDSubidentifier(oid.value, oid.length, offset, arc) ||
        !appendNASOIDArc(output, capacity, used, arc, false)) {
      return false;
    }
  }
  return true;
}

bool encodeNASGetNextRequest(const char* oid, uint16_t requestId,
                             uint8_t* buffer, size_t capacity,
                             size_t& offset, size_t& length) {
  NASBERWriter writer = {buffer, buffer + capacity, buffer + capacity};
  uint8_t encodedOID[MAX_OID_LENGTH];
  size_t oidLength = 0;
  if (!encodeNASOID(oid, encodedOID, sizeof(encodedOID), oidLength) ||
      !nasBERPrependTLV(writer, NULLTYPE, nullptr, 0) ||
      !nasBERPrependTLV(writer, OID, encodedOID, oidLength) ||
      !nasBERWrap(writer, STRUCTURE, nasBERSize(writer)) ||
      !nasBERWrap(writer, STRUCTURE, nasBERSize(writer)) ||
      !nasBERPrependPositiveInteger(writer, 0) ||
      !nasBERPrependPositiveInteger(writer, 0) ||
      !nasBERPrependPositiveInteger(writer, requestId) ||
      !nasBERWrap(writer, GetNextRequestPDU, nasBERSize(writer))) {
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

bool decodeNASGetNextResponse(const uint8_t* packet, size_t packetLength,
                              uint16_t requestId, char* nextOid,
                              size_t nextOidCapacity, char* stringValue,
                              size_t stringValueCapacity) {
  if (packet == nullptr || packetLength == 0 || nextOid == nullptr ||
      stringValue == nullptr || nextOidCapacity == 0 ||
      stringValueCapacity == 0) return false;

  const uint8_t* packetEnd = packet + packetLength;
  const uint8_t* outerCursor = packet;
  NASBERTLV message;
  if (!readNASBERTLV(outerCursor, packetEnd, message) ||
      message.tag != STRUCTURE || outerCursor != packetEnd) return false;

  const uint8_t* messageCursor = message.value;
  const uint8_t* messageEnd = message.value + message.length;
  NASBERTLV version;
  NASBERTLV community;
  NASBERTLV responsePDU;
  int32_t decodedVersion = 0;
  const size_t communityLength = strlen(SNMP_COMMUNITY);
  if (!readNASBERTLV(messageCursor, messageEnd, version) ||
      !decodeNASSignedInteger(version, decodedVersion) ||
      decodedVersion != 1 ||
      !readNASBERTLV(messageCursor, messageEnd, community) ||
      community.tag != STRING || community.length != communityLength ||
      memcmp(community.value, SNMP_COMMUNITY, communityLength) != 0 ||
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

  const uint8_t* listCursor = varBindList.value;
  const uint8_t* listEnd = varBindList.value + varBindList.length;
  NASBERTLV varBind;
  if (!readNASBERTLV(listCursor, listEnd, varBind) ||
      varBind.tag != STRUCTURE) return false;
  const uint8_t* varBindCursor = varBind.value;
  const uint8_t* varBindEnd = varBind.value + varBind.length;
  NASBERTLV oid;
  NASBERTLV value;
  if (!readNASBERTLV(varBindCursor, varBindEnd, oid) ||
      !readNASBERTLV(varBindCursor, varBindEnd, value) ||
      varBindCursor != varBindEnd ||
      !decodeNASOIDText(oid, nextOid, nextOidCapacity)) {
    return false;
  }
  if (value.tag != STRING) {
    stringValue[0] = '\0';
    return true;
  }
  if (value.length >= stringValueCapacity) return false;
  memcpy(stringValue, value.value, value.length);
  stringValue[value.length] = '\0';
  return true;
}

bool requestNASGetNext(const char* oid, uint16_t requestId,
                       char* nextOid, size_t nextOidCapacity,
                       char* stringValue, size_t stringValueCapacity,
                       uint32_t timeoutMs) {
  if (oid == nullptr || requestId == 0 || requestId > 32767) return false;
  if (!beginNASRequestSocket()) return false;
  static uint8_t requestBuffer[SNMP_PACKET_LENGTH];
  size_t offset = 0;
  size_t length = 0;
  if (!encodeNASGetNextRequest(oid, requestId, requestBuffer,
                               sizeof(requestBuffer), offset, length) ||
      nasUdp.beginPacket(nas_ip, 161) != 1 ||
      nasUdp.write(requestBuffer + offset, length) != length ||
      nasUdp.endPacket() != 1) {
    nasUdp.stop();
    return false;
  }

  const uint32_t startedAt = millis();
  static uint8_t responseBuffer[SNMP_PACKET_LENGTH * 3];
  while (!intervalElapsed(millis(), startedAt, timeoutMs)) {
    const int packetLength = nasUdp.parsePacket();
    if (packetLength <= 0) {
      delay(10);
      continue;
    }
    const IPAddress responseIP = nasUdp.remoteIP();
    const uint16_t responsePort = nasUdp.remotePort();
    if (packetLength > static_cast<int>(sizeof(responseBuffer))) {
      while (nasUdp.available() > 0) nasUdp.read();
      continue;
    }
    const int bytesRead = nasUdp.read(responseBuffer, packetLength);
    if (bytesRead == packetLength && responseIP == nas_ip &&
        responsePort == 161 &&
        decodeNASGetNextResponse(responseBuffer,
          static_cast<size_t>(packetLength), requestId, nextOid,
          nextOidCapacity, stringValue, stringValueCapacity)) {
      nasUdp.stop();
      return true;
    }
  }
  nasUdp.stop();
  return false;
}

bool parseNASVolumeIndex(const char* oid, uint32_t& index) {
  const size_t prefixLength = strlen(NAS_STORAGE_DESCR_OID);
  if (strncmp(oid, NAS_STORAGE_DESCR_OID, prefixLength) != 0 ||
      oid[prefixLength] != '.') return false;
  const char* cursor = oid + prefixLength + 1;
  if (*cursor == '\0') return false;
  uint32_t parsed = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
    ++cursor;
  }
  if (*cursor != '\0' || parsed == 0) return false;
  index = parsed;
  return true;
}

bool discoverNASVolumes(NASDiscoveredVolume* volumes, size_t capacity,
                        size_t& count) {
  count = 0;
  if (volumes == nullptr || capacity == 0) return false;
  char cursor[96] = {};
  copyText(cursor, sizeof(cursor), NAS_STORAGE_DESCR_OID);
  const size_t prefixLength = strlen(NAS_STORAGE_DESCR_OID);
  for (uint16_t attempt = 0; attempt < 64; ++attempt) {
    char nextOid[96] = {};
    char description[SNMP_OCTETSTRING_MAX_LENGTH] = {};
    if (!requestNASGetNext(cursor, static_cast<uint16_t>(3000 + attempt),
                           nextOid, sizeof(nextOid), description,
                           sizeof(description), 1000)) {
      return false;
    }
    if (strncmp(nextOid, NAS_STORAGE_DESCR_OID, prefixLength) != 0 ||
        nextOid[prefixLength] != '.') {
      return true;
    }
    if (strncmp(nextOid, cursor, sizeof(nextOid)) == 0) return false;
    uint32_t index = 0;
    if (parseNASVolumeIndex(nextOid, index) &&
        strncmp(description, "/volume", 7) == 0) {
      if (count < capacity) {
        volumes[count].index = index;
        copyText(volumes[count].name, sizeof(volumes[count].name),
                 description);
        ++count;
      }
      if (count == capacity) return true;
    }
    copyText(cursor, sizeof(cursor), nextOid);
    yield();
  }
  return false;
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

  nasSnmp._community = SNMP_COMMUNITY;
  nasSnmp._udp = nullptr;
  nasSnmp.setUDP(&nasUdp);
  cbUptime = nasSnmp.addTimestampHandler(
    nas_ip, ".1.3.6.1.2.1.25.1.1.0", &g_sysUptime);

  for (int i = 0; i < MAX_NAS_VOLUMES; ++i) {
    snprintf(g_oidAlloc[i], sizeof(g_oidAlloc[i]), "%s.0",
             NAS_STORAGE_ALLOC_OID);
    snprintf(g_oidTotal[i], sizeof(g_oidTotal[i]), "%s.0",
             NAS_STORAGE_SIZE_OID);
    snprintf(g_oidUsed[i], sizeof(g_oidUsed[i]), "%s.0",
             NAS_STORAGE_USED_OID);
    cbAlloc[i] = nasSnmp.addIntegerHandler(
      nas_ip, g_oidAlloc[i], &g_volAlloc[i]);
    cbTotal[i] = nasSnmp.addIntegerHandler(
      nas_ip, g_oidTotal[i], &g_volTotal[i]);
    cbUsed[i] = nasSnmp.addIntegerHandler(
      nas_ip, g_oidUsed[i], &g_volUsed[i]);
  }

  nasCallbacksReady = true;
}

bool updateNASCallbackOID(ValueCallback* callback, const char* oid) {
  if (callback == nullptr || oid == nullptr) return false;
  if (callback->OID != nullptr && strcmp(callback->OID, oid) == 0) {
    return true;
  }

  const size_t oidLength = strlen(oid);
  char* replacement = static_cast<char*>(malloc(oidLength + 1));
  if (replacement == nullptr) return false;
  memcpy(replacement, oid, oidLength + 1);
  if (callback->OID != nullptr) free(callback->OID);
  callback->OID = replacement;
  return true;
}

bool setNASVolumeOIDs(int slot, uint32_t index) {
  if (slot < 0 || slot >= MAX_NAS_VOLUMES) return false;
  snprintf(g_oidAlloc[slot], sizeof(g_oidAlloc[slot]), "%s.%lu",
           NAS_STORAGE_ALLOC_OID, static_cast<unsigned long>(index));
  snprintf(g_oidTotal[slot], sizeof(g_oidTotal[slot]), "%s.%lu",
           NAS_STORAGE_SIZE_OID, static_cast<unsigned long>(index));
  snprintf(g_oidUsed[slot], sizeof(g_oidUsed[slot]), "%s.%lu",
           NAS_STORAGE_USED_OID, static_cast<unsigned long>(index));
  return updateNASCallbackOID(cbAlloc[slot], g_oidAlloc[slot]) &&
         updateNASCallbackOID(cbTotal[slot], g_oidTotal[slot]) &&
         updateNASCallbackOID(cbUsed[slot], g_oidUsed[slot]);
}

bool fetchNAS() {
  Serial.printf("Heap before NAS: %d\n", ESP.getFreeHeap());
  Serial.print("NAS...");

  initializeNASCallbacks();
  if (!nasCallbacksReady) {
    markSnapshotFailure(nasSnapshot);
    return false;
  }
  const uint32_t previousUptime = g_sysUptime;
  ValueCallback* uptimeCallbacks[] = {cbUptime};
  if (!requestNAS(uptimeCallbacks, 1, 1000, 1000)) {
    g_sysUptime = previousUptime;
    Serial.println(" NAS uptime stale");
  }

  NASDiscoveredVolume discovered[MAX_NAS_VOLUMES] = {};
  size_t discoveredCount = 0;
  if (!discoverNASVolumes(discovered, MAX_NAS_VOLUMES, discoveredCount) ||
      discoveredCount == 0) {
    Serial.println(" NAS volume discovery failed");
    markSnapshotFailure(nasSnapshot);
    logHeap("after NAS");
    return false;
  }

  int stagedPoolCount = 0;
  bool volumesComplete = true;
  memset(stagedPools, 0, sizeof(stagedPools));
  for (int i = 0; i < static_cast<int>(discoveredCount); ++i) {
    if (!setNASVolumeOIDs(i, discovered[i].index)) {
      Serial.printf(" NAS volume %d OID update failed\n", i + 1);
      volumesComplete = false;
      continue;
    }
    g_volAlloc[i] = 0;
    g_volTotal[i] = 0;
    g_volUsed[i] = 0;
    ValueCallback* volumeCallbacks[] = {
      cbAlloc[i], cbTotal[i], cbUsed[i]
    };
    const bool volumeComplete =
      requestNAS(volumeCallbacks, 3, 1100 + i, 1000);
    if (!volumeComplete) {
      volumesComplete = false;
      continue;
    }

    if (stagedPoolCount < MAX_NAS_VOLUMES) {
      if (strncmp(discovered[i].name, "/volume", 7) == 0) {
        snprintf(stagedPools[stagedPoolCount].name,
          sizeof(stagedPools[stagedPoolCount].name), "Vol %s",
          discovered[i].name + 7);
      } else {
        copyText(stagedPools[stagedPoolCount].name,
          sizeof(stagedPools[stagedPoolCount].name),
          discovered[i].name);
      }
      stagedPools[stagedPoolCount].status = 1;
      
      double allocUnit = g_volAlloc[i];
      double totalUnits = g_volTotal[i];
      double usedUnits = g_volUsed[i];
      
      float total_tb = (totalUnits * allocUnit) / 1099511627776.0;
      float used_tb = (usedUnits * allocUnit) / 1099511627776.0;
      
      stagedPools[stagedPoolCount].total_tb = total_tb;
      stagedPools[stagedPoolCount].used_tb = used_tb;
      
      if (total_tb > 0) {
        stagedPools[stagedPoolCount].pct = min(
          100, max(0, static_cast<int>((used_tb / total_tb) * 100)));
        stagedPools[stagedPoolCount].free_pct = 100 -
          stagedPools[stagedPoolCount].pct;
      } else {
        stagedPools[stagedPoolCount].pct = 0;
        stagedPools[stagedPoolCount].free_pct = 0;
      }
      stagedPoolCount++;
    }
  }

  if (volumesComplete) {
    memcpy(pools, stagedPools, sizeof(pools));
    pool_count = stagedPoolCount;
  } else {
    Serial.println("NAS volume refresh incomplete; retaining previous pools");
    markSnapshotFailure(nasSnapshot);
    logHeap("after NAS");
    return false;
  }

  markSnapshotSuccess(nasSnapshot);
  Serial.printf(" %d pools\n", pool_count);
  Serial.printf("Heap after NAS: %d\n", ESP.getFreeHeap());
  return true;
}

// ===== 渲染 =====
void formatIPAddress(const IPAddress& address, char* buffer,
                     size_t bufferSize) {
  snprintf(buffer, bufferSize, "%u.%u.%u.%u",
           static_cast<unsigned int>(address[0]),
           static_cast<unsigned int>(address[1]),
           static_cast<unsigned int>(address[2]),
           static_cast<unsigned int>(address[3]));
}

void drawSunIcon(int centerX, int centerY, int size,
                 uint16_t color = GxEPD_BLACK) {
  const int radius = max(2, size / 4);
  const int rayStart = radius + 2;
  const int rayEnd = max(rayStart + 1, size / 2);
  display.drawCircle(centerX, centerY, radius, color);
  display.drawLine(centerX, centerY - rayStart,
                   centerX, centerY - rayEnd, color);
  display.drawLine(centerX, centerY + rayStart,
                   centerX, centerY + rayEnd, color);
  display.drawLine(centerX - rayStart, centerY,
                   centerX - rayEnd, centerY, color);
  display.drawLine(centerX + rayStart, centerY,
                   centerX + rayEnd, centerY, color);
  display.drawLine(centerX - rayStart + 1, centerY - rayStart + 1,
                   centerX - rayEnd + 1, centerY - rayEnd + 1, color);
  display.drawLine(centerX + rayStart - 1, centerY - rayStart + 1,
                   centerX + rayEnd - 1, centerY - rayEnd + 1, color);
  display.drawLine(centerX - rayStart + 1, centerY + rayStart - 1,
                   centerX - rayEnd + 1, centerY + rayEnd - 1, color);
  display.drawLine(centerX + rayStart - 1, centerY + rayStart - 1,
                   centerX + rayEnd - 1, centerY + rayEnd - 1, color);
}

void drawCloudIcon(int centerX, int centerY, int size,
                   uint16_t color = GxEPD_BLACK) {
  const int smallRadius = max(2, size / 5);
  const int largeRadius = max(3, size / 4);
  display.fillCircle(centerX - size / 4, centerY,
                     smallRadius, color);
  display.fillCircle(centerX, centerY - size / 6,
                     largeRadius, color);
  display.fillCircle(centerX + size / 4, centerY,
                     smallRadius, color);
  display.fillRect(centerX - size / 2, centerY,
                   size, max(2, size / 4), color);
}

void drawWeatherIcon(int centerX, int centerY, int code, int size,
                     uint16_t color = GxEPD_BLACK) {
  if (code == 0) {
    drawSunIcon(centerX, centerY, size, color);
    return;
  }

  if (code >= 1 && code <= 2) {
    drawSunIcon(centerX - size / 5, centerY - size / 5,
                max(8, size * 3 / 4), color);
    drawCloudIcon(centerX + size / 6, centerY + size / 6,
                  max(8, size * 3 / 4), color);
    return;
  }

  drawCloudIcon(centerX, centerY - size / 6, size, color);
  const int markTop = centerY + size / 4;
  if (code >= 45 && code <= 48) {
    display.drawLine(centerX - size / 2, markTop,
                     centerX + size / 2, markTop, color);
    display.drawLine(centerX - size / 3, markTop + 3,
                     centerX + size / 3, markTop + 3, color);
  } else if ((code >= 71 && code <= 77) ||
             (code >= 85 && code <= 86)) {
    for (int offset = -size / 4; offset <= size / 4;
         offset += max(2, size / 4)) {
      display.drawLine(centerX + offset - 1, markTop,
                       centerX + offset + 1, markTop + 2, color);
      display.drawLine(centerX + offset + 1, markTop,
                       centerX + offset - 1, markTop + 2, color);
    }
  } else if (code >= 95 && code <= 99) {
    display.drawLine(centerX + 1, markTop - 1,
                     centerX - 2, markTop + 4, color);
    display.drawLine(centerX - 2, markTop + 4,
                     centerX + 2, markTop + 4, color);
    display.drawLine(centerX + 2, markTop + 4,
                     centerX - 1, markTop + 8, color);
  } else if ((code >= 51 && code <= 67) ||
             (code >= 80 && code <= 82)) {
    for (int offset = -size / 4; offset <= size / 4;
         offset += max(2, size / 4)) {
      display.drawLine(centerX + offset + 1, markTop,
                       centerX + offset - 1, markTop + 4, color);
    }
  }
}

size_t utf8CharacterLength(unsigned char lead) {
  if ((lead & 0x80) == 0) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

void copyTextToPixelWidth(char* destination, size_t capacity,
                          const char* source, int maxWidth) {
  if (capacity == 0) return;
  destination[0] = '\0';
  if (!source || maxWidth <= 0) return;

  size_t sourceOffset = 0;
  size_t destinationLength = 0;
  const size_t sourceLength = strlen(source);
  while (sourceOffset < sourceLength) {
    size_t characterLength = utf8CharacterLength(
      static_cast<unsigned char>(source[sourceOffset]));
    if (sourceOffset + characterLength > sourceLength ||
        destinationLength + characterLength >= capacity) {
      break;
    }
    memcpy(destination + destinationLength,
           source + sourceOffset, characterLength);
    destinationLength += characterLength;
    destination[destinationLength] = '\0';
    if (u8g2Fonts.getUTF8Width(destination) > maxWidth) {
      destinationLength -= characterLength;
      destination[destinationLength] = '\0';
      break;
    }
    sourceOffset += characterLength;
  }
}

void drawSourceHeader(int x, int y, int w, const char* title,
                      const DataSnapshot& snapshot) {
  u8g2Fonts.setFont(u8g2_font_helvB12_tf);
  u8g2Fonts.setCursor(x + 5, y + 20);
  u8g2Fonts.print(title);
  char status[24] = {};
  formatSnapshotStatus(snapshot, status, sizeof(status));
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  const int statusWidth = u8g2Fonts.getUTF8Width(status);
  const int statusX = x + w - statusWidth - 9;
  if (snapshot.cached || snapshot.error) {
    display.fillRect(statusX - 4, y + 4, statusWidth + 8, 21, GxEPD_BLACK);
    u8g2Fonts.setFontMode(0);
    u8g2Fonts.setForegroundColor(GxEPD_WHITE);
    u8g2Fonts.setBackgroundColor(GxEPD_BLACK);
    u8g2Fonts.drawUTF8(statusX, y + 20, status);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  } else {
    u8g2Fonts.drawUTF8(statusX, y + 20, status);
  }
  display.drawLine(x, y + 25, x + w, y + 25, GxEPD_BLACK);
}

void drawBoldUTF8(int x, int baselineY, const char* text) {
  u8g2Fonts.drawUTF8(x, baselineY, text);
  u8g2Fonts.drawUTF8(x + 1, baselineY, text);
}

int textWidthWithFont(const uint8_t* font, const char* text) {
  u8g2Fonts.setFont(font);
  return u8g2Fonts.getUTF8Width(text);
}

int drawChineseRun(int x, int baselineY, const char* text) {
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  const int width = u8g2Fonts.getUTF8Width(text) + 1;
  drawBoldUTF8(x, baselineY, text);
  return x + width;
}

int drawLatinRun(int x, int baselineY, const char* text,
                 const uint8_t* font) {
  u8g2Fonts.setFont(font);
  u8g2Fonts.setCursor(x, baselineY);
  u8g2Fonts.print(text);
  return x + u8g2Fonts.getUTF8Width(text);
}

void drawGridDivider(int x, int top, int bottom) {
  if (bottom < top) return;
  display.drawLine(x, top, x, bottom, GxEPD_BLACK);
}

void drawDashedHorizontalLine(int left, int right, int y) {
  for (int x = left; x <= right; ++x) {
    if ((x - left) % 4 < 2) display.drawPixel(x, y, GxEPD_BLACK);
  }
}

void drawDashedVerticalLine(int x, int top, int bottom) {
  for (int y = top; y <= bottom; ++y) {
    if ((y - top) % 4 < 2) display.drawPixel(x, y, GxEPD_BLACK);
  }
}

int calendarDateValueWidth(int month, int day) {
  char value[12] = {};
  snprintf(value, sizeof(value), "%d", month);
  int width = textWidthWithFont(u8g2_font_helvB12_tf, value);
  width += textWidthWithFont(u8g2_font_wqy16_t_gb2312, "月") + 1;
  snprintf(value, sizeof(value), "%d", day);
  width += textWidthWithFont(u8g2_font_helvB12_tf, value);
  width += textWidthWithFont(u8g2_font_wqy16_t_gb2312, "日") + 1;
  return width;
}

int drawCalendarDateValue(int x, int baselineY, int month, int day) {
  char value[12] = {};
  snprintf(value, sizeof(value), "%d", month);
  x = drawLatinRun(x, baselineY, value, u8g2_font_helvB12_tf);
  x = drawChineseRun(x, baselineY, "月");
  snprintf(value, sizeof(value), "%d", day);
  x = drawLatinRun(x, baselineY, value, u8g2_font_helvB12_tf);
  return drawChineseRun(x, baselineY, "日");
}

int dashboardNextRefreshValueWidth(const char* nextRefreshTime) {
  return textWidthWithFont(u8g2_font_wqy16_t_gb2312, "下次刷新") + 1 + 3 +
         textWidthWithFont(u8g2_font_helvB10_tf, nextRefreshTime);
}

int drawDashboardNextRefreshValue(int x, int baselineY,
                                  const char* nextRefreshTime) {
  x = drawChineseRun(x, baselineY, "下次刷新") + 3;
  return drawLatinRun(
    x, baselineY - 1, nextRefreshTime, u8g2_font_helvB10_tf);
}

int weatherRangeWidth(const char* maximum, const char* minimum) {
  return textWidthWithFont(u8g2_font_wqy16_t_gb2312, "高") + 1 +
         textWidthWithFont(u8g2_font_helvB10_tf, maximum) + 5 +
         textWidthWithFont(u8g2_font_wqy16_t_gb2312, "低") + 1 +
         textWidthWithFont(u8g2_font_helvB10_tf, minimum);
}

void drawWeatherRange(int x, int baselineY, const char* maximum,
                      const char* minimum) {
  x = drawChineseRun(x, baselineY, "高");
  x = drawLatinRun(x, baselineY - 1, maximum, u8g2_font_helvB10_tf) + 5;
  x = drawChineseRun(x, baselineY, "低");
  drawLatinRun(x, baselineY - 1, minimum, u8g2_font_helvB10_tf);
}

int nasCapacitySummaryWidth(const char* used, const char* freeSpace,
                            const char* total, int freePercent) {
  const int labelWidth =
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, "已用") + 1 +
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, "可用") + 1 +
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, "总共") + 1;
  const int valueWidth =
    textWidthWithFont(u8g2_font_helvB10_tf, used) +
    textWidthWithFont(u8g2_font_helvB10_tf, freeSpace) +
    textWidthWithFont(u8g2_font_helvB10_tf, total);
  // Keep the three capacity blocks compact while leaving the larger gaps
  // between blocks for their vertical grid rules.
  const int labelValueGap = 2;
  const int fieldGap = 6;
  char percent[8] = {};
  snprintf(percent, sizeof(percent), "%d%%", freePercent);
  const int badgeWidth = textWidthWithFont(
    u8g2_font_wqy16_t_gb2312, "余") +
    textWidthWithFont(u8g2_font_helvB10_tf, percent) + 12;
  return labelWidth + valueWidth + labelValueGap * 3 + fieldGap * 3 +
         badgeWidth;
}

void drawNASCapacitySummary(int x, int baselineY, const char* used,
                            const char* freeSpace, const char* total,
                            int freePercent) {
  const int labelValueGap = 2;
  const int fieldGap = 6;
  const int dividerTop = baselineY - 17;
  const int dividerBottom = baselineY + 4;

  x = drawChineseRun(x, baselineY, "已用") + labelValueGap;
  x = drawLatinRun(x, baselineY - 1, used, u8g2_font_helvB10_tf);
  drawGridDivider(x + fieldGap / 2, dividerTop, dividerBottom);
  x += fieldGap;
  x = drawChineseRun(x, baselineY, "可用") + labelValueGap;
  x = drawLatinRun(x, baselineY - 1, freeSpace, u8g2_font_helvB10_tf);
  drawGridDivider(x + fieldGap / 2, dividerTop, dividerBottom);
  x += fieldGap;
  x = drawChineseRun(x, baselineY, "总共") + labelValueGap;
  x = drawLatinRun(x, baselineY - 1, total, u8g2_font_helvB10_tf) + fieldGap;

  char percent[8] = {};
  snprintf(percent, sizeof(percent), "%d%%", freePercent);
  const int badgeWidth = textWidthWithFont(
    u8g2_font_wqy16_t_gb2312, "余") +
    textWidthWithFont(u8g2_font_helvB10_tf, percent) + 12;
  display.fillRect(x, baselineY - 17, badgeWidth, 21, GxEPD_BLACK);
  u8g2Fonts.setFontMode(0);
  u8g2Fonts.setForegroundColor(GxEPD_WHITE);
  u8g2Fonts.setBackgroundColor(GxEPD_BLACK);
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2Fonts.setCursor(x + 4, baselineY);
  u8g2Fonts.print("余");
  const int labelWidth = u8g2Fonts.getUTF8Width("余");
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  u8g2Fonts.setCursor(x + 4 + labelWidth, baselineY - 1);
  u8g2Fonts.print(percent);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
}

void drawCalendar(int x, int y, int w, int h) {
  char deviceIP[16] = {};
  if (deviceIPValid) {
    formatIPAddress(lastDeviceIP, deviceIP, sizeof(deviceIP));
  } else {
    copyText(deviceIP, sizeof(deviceIP), "--");
  }

  char nextRefreshTime[6] = {};
  formatDashboardRefreshTime(
    nextDashboardRefreshTimeValid, nextDashboardRefreshTime.tm_hour,
    nextDashboardRefreshTime.tm_min,
    nextRefreshTime, sizeof(nextRefreshTime));

  const int dateWidth = timeValid
    ? calendarDateValueWidth(timeinfo.tm_mon + 1, timeinfo.tm_mday)
    : textWidthWithFont(u8g2_font_wqy16_t_gb2312, "时间不可用") + 1;
  const char* weekday = timeValid
    ? chineseWeekdayName(timeinfo.tm_wday) : nullptr;
  const int weekdayWidth = weekday
    ? textWidthWithFont(u8g2_font_wqy16_t_gb2312, weekday) + 1 : 0;
  const int ipWidth = textWidthWithFont(u8g2_font_helvB10_tf, deviceIP);
  const int refreshWidth = dashboardNextRefreshValueWidth(nextRefreshTime);
  const int segmentCount = timeValid ? 4 : 3;
  const int totalTextWidth =
    dateWidth + weekdayWidth + ipWidth + refreshWidth;
  const int availableWidth = w - 8;
  const int headerGap = max(
    2, (availableWidth - totalTextWidth) / (segmentCount - 1));
  const int contentWidth =
    totalTextWidth + headerGap * (segmentCount - 1);
  int headerX = x + max(2, (w - contentWidth) / 2);
  const int headerDividerTop = y + 2;
  const int headerDividerBottom = y + 27;
  if (timeValid) {
    int headerDividerX = headerX + dateWidth + weekdayWidth +
      headerGap + headerGap / 2;
    drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
    headerDividerX += ipWidth + headerGap;
    drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
  } else {
    int headerDividerX = headerX + dateWidth + headerGap / 2;
    drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
    headerDividerX += ipWidth + headerGap;
    drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
  }

  if (timeValid) {
    headerX = drawCalendarDateValue(
      headerX, y + 20, timeinfo.tm_mon + 1, timeinfo.tm_mday) + headerGap;
    u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
    drawBoldUTF8(headerX, y + 20, weekday);
    headerX += weekdayWidth + headerGap;
  } else {
    u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
    drawBoldUTF8(headerX, y + 20, "时间不可用");
    headerX += dateWidth + headerGap;
  }
  headerX = drawLatinRun(
    headerX, y + 19, deviceIP, u8g2_font_helvB10_tf) + headerGap;
  drawDashboardNextRefreshValue(headerX, y + 20, nextRefreshTime);
  display.drawLine(x, y + 28, x + w, y + 28, GxEPD_BLACK);
  if (!timeValid) return;

  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  struct tm firstDay = timeinfo;
  firstDay.tm_mday = 1;
  mktime(&firstDay);

  const int startX = x + 5;
  const int gridY = y + 54;
  const int gridWidth = w - 10;
  const int gridBottom = y + h - 4;
  const int monthDays = daysInGregorianMonth(year, month);
  const int rowCount = calendarRowCount(firstDay.tm_wday, monthDays);

  const int weekdayHeaderTop = y + 31;
  display.fillRect(
    startX, weekdayHeaderTop, gridWidth, gridY - weekdayHeaderTop,
    GxEPD_BLACK);
  u8g2Fonts.setFontMode(0);
  u8g2Fonts.setForegroundColor(GxEPD_WHITE);
  u8g2Fonts.setBackgroundColor(GxEPD_BLACK);
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  for (int column = 0; column < 7; column++) {
    const char* label = chineseWeekdayLabel(column);
    const int labelWidth = u8g2Fonts.getUTF8Width(label);
    const int cellX = evenlyDividedEdge(
      startX, startX + gridWidth, column, 7);
    const int cellRight = evenlyDividedEdge(
      startX, startX + gridWidth, column + 1, 7);
    u8g2Fonts.drawUTF8(
      cellX + (cellRight - cellX - labelWidth) / 2,
      y + 49, label);
  }
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

  for (int column = 0; column <= 7; ++column) {
    const int edge = evenlyDividedEdge(
      startX, startX + gridWidth, column, 7);
    drawDashedVerticalLine(edge, gridY, gridBottom);
  }
  for (int row = 0; row <= rowCount; ++row) {
    const int edge = evenlyDividedEdge(
      gridY, gridBottom, row, rowCount);
    drawDashedHorizontalLine(startX, startX + gridWidth, edge);
  }

  u8g2Fonts.setFont(u8g2_font_helvB14_tf);
  for (int day = 1; day <= monthDays; day++) {
    const CalendarCell cell = calendarCellForDay(firstDay.tm_wday, day);
    const int cellX = evenlyDividedEdge(
      startX, startX + gridWidth, cell.column, 7);
    const int cellRight = evenlyDividedEdge(
      startX, startX + gridWidth, cell.column + 1, 7);
    const int cellY = evenlyDividedEdge(
      gridY, gridBottom, cell.row, rowCount);
    const int cellBottom = evenlyDividedEdge(
      gridY, gridBottom, cell.row + 1, rowCount);
    const int cellW = cellRight - cellX;
    const int cellH = cellBottom - cellY;
    char dayText[3] = {};
    snprintf(dayText, sizeof(dayText), "%d", day);
    const TextPlacement text = centerTextInRect(
      cellX, cellY, cellW, cellH,
      u8g2Fonts.getUTF8Width(dayText),
      u8g2Fonts.getFontAscent(), u8g2Fonts.getFontDescent());

    if (day == timeinfo.tm_mday) {
      display.fillRect(
        cellX + 2, cellY + 2, cellW - 4, cellH - 4, GxEPD_BLACK);
      u8g2Fonts.setFontMode(0);
      u8g2Fonts.setForegroundColor(GxEPD_WHITE);
      u8g2Fonts.setBackgroundColor(GxEPD_BLACK);
    }
    u8g2Fonts.drawUTF8(text.x, text.baseline_y, dayText);
    if (day == timeinfo.tm_mday) {
      u8g2Fonts.setFontMode(1);
      u8g2Fonts.setForegroundColor(GxEPD_BLACK);
      u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    }
  }
}

void drawWeather(int x, int y, int w, int h) {
  const char* condition = chineseWeatherCondition(now_weather.code);
  char maximum[12] = {};
  char minimum[12] = {};
  if (now_weather.range_valid) {
    snprintf(maximum, sizeof(maximum), "%.0f°", now_weather.max_temp);
    snprintf(minimum, sizeof(minimum), "%.0f°", now_weather.min_temp);
  } else {
    copyText(maximum, sizeof(maximum), "--");
    copyText(minimum, sizeof(minimum), "--");
  }
  char temperature[20] = {};
  snprintf(temperature, sizeof(temperature), "%.1f°C", now_weather.temp);
  const char* updateLabel = "更新";
  const char* updateTime = now_weather.updated_at[0]
    ? now_weather.updated_at : "--:--";
  const int conditionWidth = 16 + 5 +
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, condition) + 1;
  const int temperatureWidth =
    textWidthWithFont(u8g2_font_helvB14_tf, temperature);
  const int rangeWidth = weatherRangeWidth(maximum, minimum);
  const int updateWidth =
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, updateLabel) + 1 + 4 +
    textWidthWithFont(u8g2_font_helvB10_tf, updateTime);
  const int totalTextWidth =
    conditionWidth + temperatureWidth + rangeWidth + updateWidth;
  const int availableWidth = w - 8;
  const int weatherHeaderGap = max(
    2, (availableWidth - totalTextWidth) / 3);
  const int contentWidth = totalTextWidth + weatherHeaderGap * 3;
  int headerX = x + max(2, (w - contentWidth) / 2);
  const int headerDividerTop = y + 2;
  const int headerDividerBottom = y + 27;
  int headerDividerX = headerX + conditionWidth + weatherHeaderGap / 2;
  drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
  headerDividerX += temperatureWidth + weatherHeaderGap;
  drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);
  headerDividerX += rangeWidth + weatherHeaderGap;
  drawGridDivider(headerDividerX, headerDividerTop, headerDividerBottom);

  drawWeatherIcon(headerX + 8, y + 14, now_weather.code, 16);
  headerX += 21;
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  drawBoldUTF8(headerX, y + 20, condition);
  headerX += conditionWidth - 21 + weatherHeaderGap;
  headerX = drawLatinRun(
    headerX, y + 19, temperature, u8g2_font_helvB14_tf) +
    weatherHeaderGap;
  drawWeatherRange(headerX, y + 20, maximum, minimum);
  headerX += rangeWidth + weatherHeaderGap;
  headerX = drawChineseRun(headerX, y + 20, updateLabel) + 4;
  drawLatinRun(headerX, y + 19, updateTime, u8g2_font_helvB10_tf);
  display.drawLine(x, y + 28, x + w, y + 28, GxEPD_BLACK);

  if (hourly_count <= 0) return;

  float minTemperature = hourly[0].temp;
  float maxTemperature = hourly[0].temp;
  for (int i = 1; i < hourly_count; ++i) {
    minTemperature = min(minTemperature, hourly[i].temp);
    maxTemperature = max(maxTemperature, hourly[i].temp);
  }
  if (maxTemperature - minTemperature < 1.0f) {
    maxTemperature += 1.0f;
    minTemperature -= 1.0f;
  }

  const int chartTop = y + 29;
  const int chartHeight = h - 29;
  const int plotTop = chartTop + 35;
  const int plotHeight = chartHeight - 65;
  const int stepX = w / hourly_count;
  const int firstX = x + stepX / 2;
  int currentHourIndex = -1;
  if (timeValid) {
    for (int i = 0; i < hourly_count; ++i) {
      if (hourly[i].hour == timeinfo.tm_hour) {
        currentHourIndex = i;
        break;
      }
    }
  }

  // Light dashed rules keep every forecast hour in its own readable column.
  const int hourlyGridTop = chartTop + 1;
  const int hourlyGridBottom = chartTop + chartHeight - 2;
  for (int boundary = 0; boundary <= hourly_count; ++boundary) {
    const int edge = evenlyDividedEdge(
      x, x + w, boundary, hourly_count);
    drawDashedVerticalLine(edge, hourlyGridTop, hourlyGridBottom);
  }

  for (int i = 0; i < hourly_count - 1; ++i) {
    const int x1 = firstX + i * stepX;
    const int y1 = plotTop + plotHeight - static_cast<int>(
      (hourly[i].temp - minTemperature) /
      (maxTemperature - minTemperature) * plotHeight);
    const int x2 = firstX + (i + 1) * stepX;
    const int y2 = plotTop + plotHeight - static_cast<int>(
      (hourly[i + 1].temp - minTemperature) /
      (maxTemperature - minTemperature) * plotHeight);
    display.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
  }

  if (currentHourIndex >= 0) {
    // The hourly grid already supplies the two vertical edges. Reuse those
    // exact boundaries so the current-hour frame adds no duplicate rules.
    const int markerLeft = evenlyDividedEdge(x, x + w, currentHourIndex, hourly_count);
    const int markerRight = evenlyDividedEdge(x, x + w, currentHourIndex + 1, hourly_count);
    if (markerRight > markerLeft) {
      const int markerTop = chartTop + 1;
      const int markerBottom = chartTop + chartHeight - 2;
      drawDashedHorizontalLine(markerLeft, markerRight, markerTop);
      drawDashedHorizontalLine(markerLeft, markerRight, markerBottom);
    }
  }

  u8g2Fonts.setFont(u8g2_font_helvB08_tf);
  for (int i = 0; i < hourly_count; ++i) {
    const int pointX = firstX + i * stepX;
    const int pointY = plotTop + plotHeight - static_cast<int>(
      (hourly[i].temp - minTemperature) /
      (maxTemperature - minTemperature) * plotHeight);
    const bool currentHour = i == currentHourIndex;
    const uint8_t* labelFont = currentHour
      ? u8g2_font_helvB12_tf : u8g2_font_helvB08_tf;
    const uint16_t inkColor = GxEPD_BLACK;
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    display.fillCircle(pointX, pointY, 2, inkColor);

    char label[12] = {};
    snprintf(label, sizeof(label), "%.0f", hourly[i].temp);
    u8g2Fonts.setFont(labelFont);
    u8g2Fonts.setCursor(
      pointX - u8g2Fonts.getUTF8Width(label) / 2,
      pointY - (currentHour ? 6 : 4));
    u8g2Fonts.print(label);

    drawWeatherIcon(pointX, chartTop + 14, hourly[i].code, 14, inkColor);

    snprintf(label, sizeof(label), "%02d", hourly[i].hour);
    u8g2Fonts.setFont(labelFont);
    u8g2Fonts.setCursor(
      pointX - u8g2Fonts.getUTF8Width(label) / 2,
      chartTop + chartHeight - 5);
    u8g2Fonts.print(label);
  }
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
}

void drawPVE(int x, int y, int w, int h) {
  char title[32];
  snprintf(title, sizeof(title), "PVE %s", pve_node.name);
  drawSourceHeader(x, y, w, title, pveSnapshot);

  const int vmRight = x + 68;
  const int ipRight = x + 170;
  const int cpuRight = x + 222;
  const int headerBaseline = y + 44;
  const int headerBottom = y + 48;

  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  const int vmHeaderWidth = u8g2Fonts.getUTF8Width("虚拟机") + 1;
  const int cpuHeaderWidth = u8g2Fonts.getUTF8Width("核心数") + 1;
  const int memoryHeaderWidth = u8g2Fonts.getUTF8Width("内存") + 1;
  drawBoldUTF8(x + (vmRight - x - vmHeaderWidth) / 2,
               headerBaseline, "虚拟机");
  drawBoldUTF8(ipRight + (cpuRight - ipRight - cpuHeaderWidth) / 2,
               headerBaseline, "核心数");
  drawBoldUTF8(cpuRight + (x + w - cpuRight - memoryHeaderWidth) / 2,
               headerBaseline, "内存");
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  const int ipHeaderWidth = u8g2Fonts.getUTF8Width("IP");
  u8g2Fonts.setCursor(vmRight + (ipRight - vmRight - ipHeaderWidth) / 2,
                      headerBaseline - 1);
  u8g2Fonts.print("IP");
  display.drawLine(x, headerBottom, x + w, headerBottom, GxEPD_BLACK);
  display.drawLine(vmRight, y + 26, vmRight, y + h - 1, GxEPD_BLACK);
  display.drawLine(ipRight, y + 26, ipRight, y + h - 1, GxEPD_BLACK);
  display.drawLine(cpuRight, y + 26, cpuRight, y + h - 1, GxEPD_BLACK);

  const int visible = min(vm_count, static_cast<int>(MAX_VISIBLE_PVE_VMS));
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  for (int i = 0; i < static_cast<int>(MAX_VISIBLE_PVE_VMS); i++) {
    const int rowTop = headerBottom + i * (y + h - headerBottom) /
      MAX_VISIBLE_PVE_VMS;
    const int rowBottom = headerBottom + (i + 1) *
      (y + h - headerBottom) / MAX_VISIBLE_PVE_VMS;
    display.drawLine(x, rowBottom, x + w, rowBottom, GxEPD_BLACK);
    if (i >= visible) continue;

    const TextPlacement rowText = centerTextInRect(
      0, rowTop, 0, rowBottom - rowTop, 0,
      u8g2Fonts.getFontAscent(), u8g2Fonts.getFontDescent());
    const int cy = rowText.baseline_y;
    if (vms[i].running) {
      display.fillCircle(x + 7, cy - 3, 3, GxEPD_BLACK);
    } else {
      display.drawCircle(x + 7, cy - 3, 3, GxEPD_BLACK);
    }

    char name[20] = {};
    copyTextToPixelWidth(name, sizeof(name), vms[i].name, vmHeaderWidth);
    u8g2Fonts.setCursor(x + 14, cy);
    u8g2Fonts.print(name);

    const int ipWidth = u8g2Fonts.getUTF8Width(vms[i].ip);
    u8g2Fonts.setCursor(vmRight + (ipRight - vmRight - ipWidth) / 2, cy);
    u8g2Fonts.print(vms[i].ip);

    char cpuText[8] = {};
    snprintf(cpuText, sizeof(cpuText), "%u", vms[i].cpus);
    const int cpuWidth = u8g2Fonts.getUTF8Width(cpuText);
    u8g2Fonts.setCursor(ipRight + (cpuRight - ipRight - cpuWidth) / 2, cy);
    u8g2Fonts.print(vms[i].cpus);

    char buf[20] = {};
    const float usedGb = bytesToGiB(vms[i].mem_bytes);
    const float totalGb = bytesToGiB(vms[i].maxmem_bytes);
    snprintf(buf, sizeof(buf), "%.1f/%.1fG", usedGb, totalGb);
    u8g2Fonts.setCursor(x + w - u8g2Fonts.getUTF8Width(buf) - 4, cy);
    u8g2Fonts.print(buf);
  }
}

void drawNAS(int x, int y, int w, int h) {
  drawSourceHeader(x, y, w, "Synology NAS", nasSnapshot);
  if (pool_count == 0) {
    u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2Fonts.setCursor(x + 16, y + 82);
    u8g2Fonts.print(nasSnapshot.cached ? "使用缓存，暂无新卷数据" :
                    (nasSnapshot.error ? "群晖数据不可用" : "暂无存储池数据"));
    return;
  }
  const int rows = max(pool_count, 1);
  const int contentTop = y + 29;
  const int contentHeight = h - 29;

  for (int i = 0; i < pool_count; i++) {
    const int rowTop = contentTop + i * contentHeight / rows;
    const int rowBottom = contentTop + (i + 1) * contentHeight / rows;
    const int baseline = rowTop + min(18, rowBottom - rowTop - 16);

    char usedStr[16], totalStr[16], freeStr[16];
    
    if (pools[i].used_tb < 1.0) sprintf(usedStr, "%.0fG", pools[i].used_tb * 1024);
    else sprintf(usedStr, "%.1fT", pools[i].used_tb);
    
    if (pools[i].total_tb < 1.0) sprintf(totalStr, "%.0fG", pools[i].total_tb * 1024);
    else sprintf(totalStr, "%.1fT", pools[i].total_tb);
    
    float free_tb = pools[i].total_tb - pools[i].used_tb;
    if (free_tb < 1.0) sprintf(freeStr, "%.0fG", free_tb * 1024);
    else sprintf(freeStr, "%.1fT", free_tb);
    
    const int summaryWidth = nasCapacitySummaryWidth(
      usedStr, freeStr, totalStr, pools[i].free_pct);
    const int summaryX = x + w - summaryWidth - 8;
    u8g2Fonts.setFont(u8g2_font_helvB10_tf);
    char poolName[20] = {};
    copyTextToPixelWidth(poolName, sizeof(poolName), pools[i].name,
                         max(0, summaryX - (x + 8) - 8));
    u8g2Fonts.setCursor(x + 8, baseline);
    u8g2Fonts.print(poolName);
    drawNASCapacitySummary(
      summaryX, baseline, usedStr, freeStr, totalStr,
      pools[i].free_pct);

    const int barY = min(rowTop + 24, rowBottom - 11);
    display.drawRect(x + 8, barY, w - 16, 10, GxEPD_BLACK);
    int barW = (w - 24) * pools[i].pct / 100;
    display.fillRect(x + 10, barY + 2, barW, 6, GxEPD_BLACK);
  }
}



void drawPVEBottomBar() {
  display.fillRect(0, 416, 300, 32, GxEPD_WHITE);
  display.drawLine(100, 416, 100, 447, GxEPD_BLACK);

  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  u8g2Fonts.setCursor((100 - u8g2Fonts.getUTF8Width(pve_node.ip)) / 2, 438);
  u8g2Fonts.print(pve_node.ip);

  const float usedGb = bytesToGiB(pve_node.mem_bytes);
  const float totalGb = bytesToGiB(pve_node.maxmem_bytes);
  const uint8_t pct = memoryPercent(pve_node.mem_bytes, pve_node.maxmem_bytes);
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  drawBoldUTF8(106, 431, "内存使用");
  char memoryText[32] = {};
  snprintf(memoryText, sizeof(memoryText), "%.1f/%.1fG %u%%",
           usedGb, totalGb, pct);
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  u8g2Fonts.setCursor(294 - u8g2Fonts.getUTF8Width(memoryText), 431);
  u8g2Fonts.print(memoryText);
  display.drawRect(105, 435, 190, 10, GxEPD_BLACK);
  const int usedWidth = 186 * pct / 100;
  display.fillRect(107, 437, usedWidth, 6, GxEPD_BLACK);
}

void drawNASBottomBar() {
  display.fillRect(301, 416, 299, 32, GxEPD_WHITE);
  display.drawLine(300, 416, 300, 447, GxEPD_BLACK);
  display.drawLine(450, 416, 450, 447, GxEPD_BLACK);

  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  char nasIP[16] = {};
  formatIPAddress(nas_ip, nasIP, sizeof(nasIP));
  u8g2Fonts.setFont(u8g2_font_helvB10_tf);
  u8g2Fonts.setCursor(300 + (150 - u8g2Fonts.getUTF8Width(nasIP)) / 2, 438);
  u8g2Fonts.print(nasIP);

  const uint32_t days = g_sysUptime / (100UL * 60 * 60 * 24);
  char dayText[12] = {};
  snprintf(dayText, sizeof(dayText), "%lu",
           static_cast<unsigned long>(days));
  const char* runtimeLabel = "运行时间";
  const int runtimeGap = 4;
  const int runtimeWidth =
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, runtimeLabel) + 1 +
    runtimeGap + textWidthWithFont(u8g2_font_helvB10_tf, dayText) +
    textWidthWithFont(u8g2_font_wqy16_t_gb2312, "天") + 1;
  int runtimeX = 450 + (150 - runtimeWidth) / 2;
  runtimeX = drawChineseRun(runtimeX, 440, runtimeLabel) + runtimeGap;
  runtimeX = drawLatinRun(runtimeX, 439, dayText, u8g2_font_helvB10_tf);
  drawChineseRun(runtimeX, 440, "天");
}

bool renderAll() {
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
  return true;
}

void renderSolidScreen(uint16_t color, const char* label) {
  Serial.printf("Panel conditioning %s\n", label);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(color);
  } while (display.nextPage());
  delay(1000);
}

void conditionPanelAtStartup() {
  // A black-to-white cycle clears the previous image while leaving a clean
  // white background for the connection status and final dashboard.
  renderSolidScreen(GxEPD_BLACK, "BLACK");
  renderSolidScreen(GxEPD_WHITE, "WHITE");
}

void renderWiFiConnectingScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

    const char* latin = "WiFi";
    const char* chinese = "连接中";
    const int totalWidth =
      textWidthWithFont(u8g2_font_helvB14_tf, latin) + 6 +
      textWidthWithFont(u8g2_font_wqy16_t_gb2312, chinese) + 1;
    int messageX = (display.width() - totalWidth) / 2;
    const int baselineY = display.height() / 2 + 7;
    messageX = drawLatinRun(
      messageX, baselineY - 1, latin, u8g2_font_helvB14_tf) + 6;
    drawChineseRun(messageX, baselineY, chinese);
  } while (display.nextPage());
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
  currentRefreshDataTimeValid = false;
  const bool wifiConnectedAtStart = WiFi.status() == WL_CONNECTED;
  if (wifiConnectedAtStart) markWiFiConnectedObserved();
  bool wifiStayedConnected = wifiConnectedAtStart;

  wifiStayedConnected = updateWiFiContinuity(
    wifiStayedConnected, "full-start");
  syncTime();
  if (timeValid) {
    currentRefreshDataTime = timeinfo;
    currentRefreshDataTimeValid = true;
  }
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
  captureDashboardTimes();

  if (wifiConnectedAtStart && !wifiStayedConnected) {
    Serial.println("WiFi lost during full refresh");
  }

  const bool fullRenderOk = renderAll();
  if (!fullRenderOk) {
    updateWiFiContinuity(wifiStayedConnected, "after-full-render");
    Serial.printf("Full refresh FAILED reason=%s\n",
      reason ? reason : "unspecified");
    logHeap("full failed");
    return false;
  }

  displayReady = true;
  updateWiFiContinuity(wifiStayedConnected, "after-full-render");
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
    displayReady = true;
    ok = true;
  } else {
    Serial.printf("Full refresh FAILED reason=%s source=cached\n",
      reason ? reason : "unspecified");
  }

  recordFullAttemptCompletion();
  recoveryPending = !ok;
  Serial.printf("Cached full recovery complete reason=%s ready=%d\n",
    reason ? reason : "unspecified", displayReady ? 1 : 0);
  logHeap(ok ? "cached recovery" : "cached failed");
  return ok;
}

void setup() {
  Serial.begin(115200);

  loadRuntimeConfig();
  wifiDisconnectHandler =
    WiFi.onStationModeDisconnected(onWiFiStationDisconnected);
  display.init(115200, true, 2, false);
  u8g2Fonts.begin(display);
  conditionPanelAtStartup();
  renderWiFiConnectingScreen();

  lastWifiRetryMs = millis();
  connectWifi();
  startWebServices();

  initializeNASCallbacks();
  wifiWasConnected = WiFi.status() == WL_CONNECTED;

  attemptFullDashboard("startup", true);
  Serial.flush();
}

void loop() {
  serviceWebServices();
  consumeWiFiDisconnectEvent("loop");
  const bool wifiConnectedNow = WiFi.status() == WL_CONNECTED;
  if (wifiConnectedNow) markWiFiConnectedObserved();
  if (!wifiConnectedNow) {
    if (wifiWasConnected) {
      Serial.println("WiFi disconnected; full refresh pending");
      wifiWasConnected = false;
      clearWiFiConnectedObserved();
      wifiReconnectRefreshPending = true;
      dataRefreshPending = true;
      lastWifiRetryMs = millis();
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
    Serial.println("WiFi reconnected; scheduling full refresh");
    wifiWasConnected = true;
    lastFullRefreshMs = millis();
    wifiReconnectRefreshPending = true;
    dataRefreshPending = true;
    delay(50);
    yield();
    return;
  }

  const uint32_t now = millis();
  if (fullRefreshGuardOpen(now)) {
    if (recoveryPending) {
      recoverCachedDashboard("readiness-recovery");
    } else if (dataRefreshPending ||
               intervalElapsed(now, lastFullRefreshMs,
                               FULL_REFRESH_INTERVAL_MS)) {
      attemptFullDashboard(
        wifiReconnectRefreshPending ? "wifi-reconnected" : "scheduled", true);
    }
  }

  delay(25);
  yield();
}
