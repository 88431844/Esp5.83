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
#include "dashboard_model.h"
#include "secrets.h"

// ===== 配置 =====
const char* PVE_HOST = "192.168.31.34";
const int PVE_PORT = 8006;
const char* PVE_CERT_FINGERPRINT = "32:2A:C0:E1:C4:73:01:56:33:7D:CD:72:5C:19:72:DD:37:08:EA:C9";

IPAddress nas_ip(192, 168, 31, 105);

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
PoolInfo pools[4];
int pool_count = 0;
uint32_t g_sysUptime = 0;
struct tm timeinfo;
uint32_t min_free_heap = UINT32_MAX;

// ===== GxEPD2 显示器 =====
// 将缓冲从全屏(HEIGHT, 33.6KB)改为32行(2.4KB)，解决 OOM 崩溃问题
GxEPD2_BW<GxEPD2_583, 32> display(GxEPD2_583(15, 0, 2, 4));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

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
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " FAILED");
}

void syncTime() {
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
  localtime_r(&now, &timeinfo);
  Serial.printf(" OK: %04d-%02d-%02d %02d:%02d\n",
    timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
    timeinfo.tm_hour, timeinfo.tm_min);
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

void fetchPVEGuestIP(VMInfo& vm) {
  if (!vm.running || !pve_node.name[0]) return;
  WiFiClientSecure client;
  HTTPClient http;
  configurePVEClient(client, http);

  char url[192];
  snprintf(url, sizeof(url),
    "https://%s:%d/api2/json/nodes/%s/qemu/%d/agent/network-get-interfaces",
    PVE_HOST, PVE_PORT, pve_node.name, vm.vmid);
  if (!http.begin(client, url)) return;
  http.addHeader("Authorization", PVE_TOKEN);
  const int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    filter["data"]["result"][0]["ip-addresses"][0]["ip-address"] = true;
    filter["data"]["result"][0]["ip-addresses"][0]["ip-address-type"] = true;
    JsonDocument doc;
    DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      for (JsonObject interface : doc["data"]["result"].as<JsonArray>()) {
        for (JsonObject address : interface["ip-addresses"].as<JsonArray>()) {
          const char* value = address["ip-address"] | "";
          const char* type = address["ip-address-type"] | "";
          if ((type[0] == '\0' || strcmp(type, "ipv4") == 0) && isUsableGuestIPv4(value)) {
            copyText(vm.ip, sizeof(vm.ip), value);
            break;
          }
        }
        if (strcmp(vm.ip, "-") != 0) break;
      }
    }
  } else {
    Serial.printf("PVE VM %d agent HTTP %d\n", vm.vmid, httpCode);
  }
  http.end();
  client.stop();
  delay(0);
}

void fetchPVE() {
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
  const int visible = min(vm_count, static_cast<int>(MAX_VISIBLE_PVE_VMS));
  for (int i = 0; i < visible; ++i) {
    if (!vms[i].running) break;
    fetchPVEGuestIP(vms[i]);
    logHeap("after guest IP");
  }
  Serial.printf("PVE node=%s %s, VMs=%d\n", pve_node.name,
    nodeOk ? "OK" : "FAILED", vmOk ? vm_count : 0);
  logHeap("after PVE");
}

// 全局缓冲，防止底层库持有栈内指针导致崩溃
char g_volNameBufs[4][64];
char* g_volNames[4] = {g_volNameBufs[0], g_volNameBufs[1], g_volNameBufs[2], g_volNameBufs[3]};
int g_volAlloc[4] = {0, 0, 0, 0};
int g_volTotal[4] = {0, 0, 0, 0};
int g_volUsed[4] = {0, 0, 0, 0};
char g_oidName[4][64];
char g_oidAlloc[4][64];
char g_oidTotal[4][64];
char g_oidUsed[4][64];

void fetchNAS() {
  Serial.printf("Heap before NAS: %d\n", ESP.getFreeHeap());
  Serial.print("NAS...");

  static WiFiUDP udp;
  static SNMPManager snmp(SNMP_COMMUNITY);
  snmp._udp = nullptr;
  snmp.setUDP(&udp);
  snmp.begin();
  
  ValueCallback* cb_uptime = snmp.addTimestampHandler(nas_ip, ".1.3.6.1.2.1.25.1.1.0", &g_sysUptime);
  
  ValueCallback* cb_name[4];
  ValueCallback* cb_alloc[4];
  ValueCallback* cb_total[4];
  ValueCallback* cb_used[4];
  int vol_indices[4] = {59, 57, 56, 58};
  
  for(int i = 0; i < 4; i++) {
    char oidBuf[64];
    sprintf(oidBuf, ".1.3.6.1.2.1.25.2.3.1.3.%d", vol_indices[i]);
    cb_name[i] = snmp.addStringHandler(nas_ip, oidBuf, &g_volNames[i]);
    sprintf(oidBuf, ".1.3.6.1.2.1.25.2.3.1.4.%d", vol_indices[i]);
    cb_alloc[i] = snmp.addIntegerHandler(nas_ip, oidBuf, &g_volAlloc[i]);
    sprintf(oidBuf, ".1.3.6.1.2.1.25.2.3.1.5.%d", vol_indices[i]);
    cb_total[i] = snmp.addIntegerHandler(nas_ip, oidBuf, &g_volTotal[i]);
    sprintf(oidBuf, ".1.3.6.1.2.1.25.2.3.1.6.%d", vol_indices[i]);
    cb_used[i] = snmp.addIntegerHandler(nas_ip, oidBuf, &g_volUsed[i]);
  }
  
  // Fetch uptime
  SNMPGet req_up(SNMP_COMMUNITY, 1);
  req_up.setUDP(&udp);
  req_up.addOIDPointer(cb_uptime);
  req_up.sendTo(nas_ip);
  unsigned long start = millis();
  while(millis() - start < 1000) { snmp.loop(); delay(10); }
  
  pool_count = 0;
  for(int i = 0; i < 4; i++) {
    SNMPGet req(SNMP_COMMUNITY, 1);
    req.setRequestID(1000 + i);
    req.setUDP(&udp);
    req.addOIDPointer(cb_name[i]);
    req.addOIDPointer(cb_alloc[i]);
    req.addOIDPointer(cb_total[i]);
    req.addOIDPointer(cb_used[i]);
    req.sendTo(nas_ip);
    
    start = millis();
    while(millis() - start < 1000) { snmp.loop(); delay(10); }
    
    if(g_volNames[i] != nullptr && strlen(g_volNames[i]) > 0) {
      if (strncmp(g_volNames[i], "/volume", 7) == 0) {
        sprintf(pools[pool_count].name, "Vol %s", g_volNames[i] + 7);
      } else {
        strncpy(pools[pool_count].name, g_volNames[i], 19);
      }
      pools[pool_count].name[19] = '\0';
      pools[pool_count].status = 1;
      
      double allocUnit = g_volAlloc[i];
      double totalUnits = g_volTotal[i];
      double usedUnits = g_volUsed[i];
      
      float total_tb = (totalUnits * allocUnit) / 1099511627776.0;
      float used_tb = (usedUnits * allocUnit) / 1099511627776.0;
      
      pools[pool_count].total_tb = total_tb;
      pools[pool_count].used_tb = used_tb;
      
      if(total_tb > 0) {
        pools[pool_count].pct = (used_tb / total_tb) * 100;
      } else {
        pools[pool_count].pct = 0;
      }
      pool_count++;
    }
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

void drawCalendar(int x, int y, int w, int h) {
  drawHeader(x, y, w, "Calendar");
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  
  int daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int year = timeinfo.tm_year + 1900;
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) daysInMonth[1] = 29;
  
  int currentDay = timeinfo.tm_mday;
  int currentMonth = timeinfo.tm_mon;
  
  struct tm firstDay = timeinfo;
  firstDay.tm_mday = 1;
  mktime(&firstDay);
  int startDayOfWeek = firstDay.tm_wday;
  
  int cellW = (w - 10) / 7;
  int cellH = (h - 40) / 6;
  int startX = x + 5;
  int startY = y + 45;
  
  const char* days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  for (int i = 0; i < 7; i++) {
    u8g2Fonts.setCursor(startX + i * cellW + 2, y + 38);
    u8g2Fonts.print(days[i]);
  }
  
  int day = 1;
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 7; col++) {
      if (row == 0 && col < startDayOfWeek) continue;
      if (day > daysInMonth[currentMonth]) break;
      
      int cx = startX + col * cellW;
      int cy = startY + row * cellH;
      
      if (day == currentDay) {
        display.fillRect(cx, cy - 12, cellW - 2, cellH - 2, GxEPD_BLACK);
        u8g2Fonts.setForegroundColor(GxEPD_WHITE);
        u8g2Fonts.setBackgroundColor(GxEPD_BLACK);
      }
      
      u8g2Fonts.setCursor(cx + 4, cy);
      u8g2Fonts.print(day);
      
      if (day == currentDay) {
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
      }
      
      day++;
    }
  }
}

void drawWeather(int x, int y, int w, int h) {
  drawHeader(x, y, w, "Weather");
  
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  u8g2Fonts.setCursor(x + 5, y + 35);
  char buf[64];
  sprintf(buf, "Now: %.1fC   Hum: %d%%   Wind: %.1fkm/h", now_weather.temp, now_weather.humidity, now_weather.wind);
  u8g2Fonts.print(buf);
  
  // -- 8小时预报折线图 (高度区间 y+45 到 y+125) --
  if (hourly_count > 0) {
    float minT = hourly[0].temp, maxT = hourly[0].temp;
    for (int i=1; i<hourly_count; i++) {
      if (hourly[i].temp < minT) minT = hourly[i].temp;
      if (hourly[i].temp > maxT) maxT = hourly[i].temp;
    }
    if (maxT - minT < 1.0f) { maxT += 1.0f; minT -= 1.0f; }
    
    int cY = y + 45;
    int cH = 80;
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
  
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  
  u8g2Fonts.setCursor(315, 436);
  u8g2Fonts.print("IP: 192.168.31.105");
  
  uint32_t days = g_sysUptime / (100UL * 60 * 60 * 24);
  u8g2Fonts.setCursor(470, 436);
  u8g2Fonts.printf("Up: %d d", days);
}

void renderAll() {
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
}

void setup() {
  Serial.begin(115200);
  
  connectWifi();
  syncTime();
  fetchWeather();
  fetchPVE();
  fetchNAS();

  Serial.flush();
  display.init(115200, true, 2, false);
  u8g2Fonts.begin(display);
  
  renderAll();
  
  display.hibernate();
  // Deep sleep for 10 minutes (600,000,000 microseconds)
  ESP.deepSleep(600e6);
}

void loop() {
}
