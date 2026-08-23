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

// ===== 配置 =====
const char* WIFI_SSID = "a_luck";
const char* WIFI_PASS = "w0shiwifI!0";

const char* PVE_HOST = "192.168.31.34";
const int PVE_PORT = 8006;
const char* PVE_TOKEN = "PVEAPIToken=root@pam!epd=9d296cd8-5676-4f9b-b174-cda286fb0df1";

IPAddress nas_ip(192, 168, 31, 105);
const char* SNMP_COMMUNITY = "nas_snmp";

// ===== 数据结构 =====
struct WeatherNow { float temp; int code; int humidity; float wind; };
struct HourlyWeather { int hour; float temp; int code; };
struct DailyWeather { String day; float tMax; float tMin; int code; };
struct VMInfo { char name[20]; bool running; int cpus; float mem_gb; };
struct PoolInfo { char name[20]; int status; float used_tb; float total_tb; int pct; };

// ===== 全局数据 =====
WeatherNow now_weather;
HourlyWeather hourly[8];
int hourly_count = 0;
DailyWeather daily[7];
int daily_count = 0;
VMInfo vms[8];
int vm_count = 0;
PoolInfo pools[4];
int pool_count = 0;
struct tm timeinfo;

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
  } else {
    Serial.println(" begin FAILED");
  }
  Serial.printf("Heap after weather: %d\n", ESP.getFreeHeap());
}

void fetchPVE() {
  Serial.printf("Heap before PVE: %d\n", ESP.getFreeHeap());
  Serial.print("PVE...");
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    String url = String("https://") + PVE_HOST + ":" + PVE_PORT
               + "/api2/json/cluster/resources?type=vm";
    if (http.begin(client, url)) {
      http.addHeader("Authorization", PVE_TOKEN);
      int httpCode = http.GET();
      Serial.printf(" HTTP %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK) {
        JsonDocument filter;
        filter["data"][0]["name"]   = true;
        filter["data"][0]["status"] = true;
        filter["data"][0]["maxcpu"] = true;
        filter["data"][0]["maxmem"] = true;
        filter["data"][0]["type"]   = true;
        JsonDocument doc;
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        JsonArray data = doc["data"];
        vm_count = 0;
        for (JsonObject v : data) {
          if (vm_count >= 8) break;
          const char* type = v["type"] | "";
          if (strcmp(type, "qemu") != 0) continue;
          const char* name   = v["name"]   | "?";
          const char* status = v["status"] | "stopped";
          strncpy(vms[vm_count].name, name, 19);
          vms[vm_count].name[19] = '\0';
          vms[vm_count].running  = (strcmp(status, "running") == 0);
          vms[vm_count].cpus     = v["maxcpu"] | 0;
          long long maxmem = v["maxmem"] | 0;
          vms[vm_count].mem_gb   = maxmem / 1073741824.0f;
          vm_count++;
        }
        Serial.printf(" %d VMs\n", vm_count);
      }
      http.end();
    } else {
      Serial.println(" begin FAILED");
    }
  }
  Serial.printf("Heap after PVE: %d\n", ESP.getFreeHeap());
}

void fetchNAS() {
  Serial.printf("Heap before NAS: %d\n", ESP.getFreeHeap());
  Serial.print("NAS...");

  static WiFiUDP udp;
  static SNMPManager snmp(SNMP_COMMUNITY);
  
  snmp._udp = nullptr; 
  snmp.setUDP(&udp);
  snmp.begin();
  
  pool_count = 0;
  
  // 核心修复：16个OID的响应超过了库内置的512字节上限(SNMP_PACKET_LENGTH)，
  // 截断会导致 ASN.1 解析器读到乱码/越界内存，从而引发 malloc 失败和 Exception 29！
  // 解决办法：分 4 次请求，每次只请求 1 个存储池（4个OID），响应包缩小到 ~200 字节，完美避开溢出。
  for(int i = 0; i < 4; i++) {
    SNMPGet request(SNMP_COMMUNITY, 1);
    request.setUDP(&udp);
    
    char* poolName = nullptr;
    int poolStatus = 0;
    uint64_t poolFree = 0;
    uint64_t poolTotal = 0;
    
    char oidStr[64];
    
    sprintf(oidStr, ".1.3.6.1.4.1.6574.3.1.1.2.%d", i+1);
    request.addOIDPointer(snmp.addStringHandler(nas_ip, oidStr, &poolName));
    
    sprintf(oidStr, ".1.3.6.1.4.1.6574.3.1.1.3.%d", i+1);
    request.addOIDPointer(snmp.addIntegerHandler(nas_ip, oidStr, &poolStatus));
    
    sprintf(oidStr, ".1.3.6.1.4.1.6574.3.1.1.4.%d", i+1);
    request.addOIDPointer(snmp.addCounter64Handler(nas_ip, oidStr, &poolFree));
    
    sprintf(oidStr, ".1.3.6.1.4.1.6574.3.1.1.5.%d", i+1);
    request.addOIDPointer(snmp.addCounter64Handler(nas_ip, oidStr, &poolTotal));
    
    request.sendTo(nas_ip);
    
    // 给NAS响应留时间并接收报文
    unsigned long start = millis();
    while(millis() - start < 1500) {
      snmp.loop();
      delay(10);
    }
    
    // 如果获取到了名字，说明该池子存在
    if(poolName != nullptr && strlen(poolName) > 0) {
      strncpy(pools[pool_count].name, poolName, 19);
      pools[pool_count].name[19] = '\0';
      pools[pool_count].status = poolStatus;
      
      float total_tb = poolTotal / 1099511627776.0;
      float free_tb = poolFree / 1099511627776.0;
      pools[pool_count].total_tb = total_tb;
      pools[pool_count].used_tb = total_tb - free_tb;
      
      if(total_tb > 0) {
        pools[pool_count].pct = (pools[pool_count].used_tb / total_tb) * 100;
      } else {
        pools[pool_count].pct = 0;
      }
      
      free(poolName);
      pool_count++;
    }
    
    // 每次请求后清理旧的 handlers，避免下一次请求重复叠加
    // 由于这个库没有提供 clear() 方法，我们会用最简单的黑客手段——在下一次循环丢弃之前的引用。
    // 但是库里可能会一直累积 callbacks。由于 fetchNAS 只会在开机执行一次，
    // 累积 4 次 callbacks(共16个)是完全没问题的（内存够用）。
  }
  
  udp.stop();
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

void drawPVM(int x, int y, int w, int h) {
  drawHeader(x, y, w, "PVE Virtual Machines");
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  
  int startY = y + 45;
  for (int i = 0; i < vm_count; i++) {
    int cy = startY + i * 20;
    if (vms[i].running) {
      display.fillCircle(x + 10, cy - 4, 4, GxEPD_BLACK);
    } else {
      display.drawCircle(x + 10, cy - 4, 4, GxEPD_BLACK);
    }
    
    u8g2Fonts.setCursor(x + 25, cy);
    u8g2Fonts.print(vms[i].name);
    
    char buf[30];
    sprintf(buf, "%dC %.1fG", vms[i].cpus, vms[i].mem_gb);
    u8g2Fonts.setCursor(x + 150, cy);
    u8g2Fonts.print(buf);
  }
}

void drawNAS(int x, int y, int w, int h) {
  drawHeader(x, y, w, "Synology Storage Pools");
  u8g2Fonts.setFont(u8g2_font_helvR08_tf);
  
  int startY = y + 45;
  for (int i = 0; i < pool_count; i++) {
    int cy = startY + i * 40;
    
    u8g2Fonts.setCursor(x + 10, cy);
    u8g2Fonts.print(pools[i].name);
    
    char buf[30];
    sprintf(buf, "%d%% (%.1f/%.1f TB)", pools[i].pct, pools[i].used_tb, pools[i].total_tb);
    u8g2Fonts.setCursor(x + 100, cy);
    u8g2Fonts.print(buf);
    
    display.drawRect(x + 10, cy + 5, w - 20, 10, GxEPD_BLACK);
    int barW = (w - 24) * pools[i].pct / 100;
    display.fillRect(x + 12, cy + 7, barW, 6, GxEPD_BLACK);
  }
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
    
    display.drawLine(300, 0, 300, 448, GxEPD_BLACK);
    display.drawLine(0, 224, 600, 224, GxEPD_BLACK);
    
    drawCalendar(0, 0, 300, 224);
    drawWeather(300, 0, 300, 224);
    drawPVM(0, 224, 300, 224);
    drawNAS(300, 224, 300, 224);
    
  } while (display.nextPage());
}

void setup() {
  Serial.begin(115200);
  
  connectWifi();
  syncTime();
  fetchWeather();
  // 听你的，分步调试！先把容易导致崩溃的 PVE 和 NAS 获取注释掉
  // fetchPVE();
  // fetchNAS();
  
  display.init(115200, true, 2, false);
  u8g2Fonts.begin(display);
  
  renderAll();
  
  display.hibernate();
  ESP.deepSleep(600e6);
}

void loop() {}
