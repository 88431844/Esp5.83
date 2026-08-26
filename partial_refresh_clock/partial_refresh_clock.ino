#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

#include "GxEPD2_583_FastClock.h"
#include "partial_clock_model.h"
#include "secrets.h"

namespace {

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t NTP_RECONFIGURE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t FULL_CLEAN_INTERVAL = 300;

GxEPD2_583_FastClock display(15, 0, 2, 4);
GFXcanvas1 clockCanvas(CLOCK_WIDTH, CLOCK_HEIGHT);
U8G2_FOR_ADAFRUIT_GFX clockFont;
uint8_t previousClock[CLOCK_BUFFER_SIZE];

time_t lastDisplayedSecond = 0;
uint32_t partialRefreshCount = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastNtpConfigMs = 0;
bool displayReady = false;

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  lastWifiAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" FAILED");
    return false;
  }

  Serial.printf(" OK ip=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool configureAndWaitForTime() {
  if (WiFi.status() != WL_CONNECTED) return false;

  lastNtpConfigMs = millis();
  configTime(28800, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
  Serial.print("NTP sync");

  const uint32_t started = millis();
  time_t now = time(nullptr);
  while (now < 1000000000 && millis() - started < 15000) {
    delay(250);
    Serial.print('.');
    now = time(nullptr);
  }

  if (now < 1000000000) {
    Serial.println(" FAILED");
    return false;
  }

  struct tm localTime;
  localtime_r(&now, &localTime);
  Serial.printf(" OK %04d-%02d-%02d %02d:%02d:%02d\n",
      localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
      localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
  return true;
}

void renderClock(const struct tm& localTime, char* value, size_t capacity) {
  formatClockTime(value, capacity,
      localTime.tm_hour, localTime.tm_min, localTime.tm_sec);

  clockCanvas.fillScreen(1);
  clockFont.setFontMode(0);
  clockFont.setFontDirection(0);
  clockFont.setForegroundColor(0);
  clockFont.setBackgroundColor(1);
  clockFont.setFont(u8g2_font_logisoso78_tn);

  const int16_t textWidth = clockFont.getUTF8Width(value);
  const int16_t baseline =
      (CLOCK_HEIGHT + clockFont.getFontAscent() + clockFont.getFontDescent()) / 2;
  clockFont.setCursor((CLOCK_WIDTH - textWidth) / 2, baseline);
  clockFont.print(value);
}

void cleanPanelAndResumeFastMode() {
  Serial.println("Full clean refresh");
  display.clearToWhite();
  memset(previousClock, 0xFF, sizeof(previousClock));
  display.beginFastMode();
}

void updateClock(time_t now) {
  struct tm localTime;
  localtime_r(&now, &localTime);

  char value[9];
  renderClock(localTime, value, sizeof(value));

  if (partialRefreshCount > 0 &&
      partialRefreshCount % FULL_CLEAN_INTERVAL == 0) {
    cleanPanelAndResumeFastMode();
  }

  const uint32_t started = millis();
  const bool refreshed = display.refreshClockWindow(
      clockCanvas.getBuffer(), previousClock, sizeof(previousClock),
      CLOCK_X, CLOCK_Y, CLOCK_WIDTH, CLOCK_HEIGHT);
  const uint32_t refreshMs = millis() - started;

  if (refreshed) {
    ++partialRefreshCount;
    Serial.printf("Clock %s partial=%lu refresh_ms=%lu heap=%u\n",
        value, static_cast<unsigned long>(partialRefreshCount),
        static_cast<unsigned long>(refreshMs), ESP.getFreeHeap());
  } else {
    Serial.printf("Clock %s refresh FAILED heap=%u\n", value, ESP.getFreeHeap());
  }
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("5.83 fast partial-refresh clock test");

  if (clockCanvas.getBuffer() == nullptr) {
    Serial.println("Clock canvas allocation FAILED");
    return;
  }

  connectWifi();
  configureAndWaitForTime();

  display.init(SERIAL_BAUD, true, 2, false);
  clockFont.begin(clockCanvas);
  cleanPanelAndResumeFastMode();
  displayReady = true;
}

void loop() {
  const uint32_t nowMs = millis();
  if (WiFi.status() != WL_CONNECTED &&
      nowMs - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    connectWifi();
  }

  if (WiFi.status() == WL_CONNECTED &&
      nowMs - lastNtpConfigMs >= NTP_RECONFIGURE_INTERVAL_MS) {
    configureAndWaitForTime();
  }

  const time_t now = time(nullptr);
  if (displayReady && now >= 1000000000 && now != lastDisplayedSecond) {
    lastDisplayedSecond = now;
    updateClock(now);
  }

  delay(10);
}
