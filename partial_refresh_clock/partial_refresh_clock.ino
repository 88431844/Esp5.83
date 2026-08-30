#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

#include "GxEPD2_583_FastPartial.h"
#include "partial_clock_model.h"
#include "secrets.h"

namespace {

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t NTP_RECONFIGURE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t FULL_CLEAN_INTERVAL = 600;
constexpr uint32_t FULL_CLEAN_RETRY_INTERVAL_MS = 60000;

GxEPD2_583_FastPartial display(15, 0, 2, 4);
GFXcanvas1 clockCanvas(CLOCK_WIDTH, CLOCK_HEIGHT);
U8G2_FOR_ADAFRUIT_GFX clockFont;
uint8_t previousClock[CLOCK_BUFFER_SIZE];

time_t lastDisplayedSecond = 0;
uint32_t partialRefreshCount = 0;
uint32_t lastFullCleanCount = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastNtpConfigMs = 0;
uint32_t lastFullCleanAttemptMs = 0;
bool displayReady = false;
bool fullCleanAttemptRecorded = false;
bool fullCleanRecoveryPending = false;

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

bool drawClockWithFullRefresh() {
  Serial.println("Full clock refresh");
  lastFullCleanAttemptMs = millis();
  fullCleanAttemptRecorded = true;
  display.clearToWhite();
  if (!display.lastFullRefreshSucceeded()) {
    Serial.println("Full clock clear FAILED");
    return false;
  }
  memset(previousClock, 0xFF, sizeof(previousClock));
  if (!display.beginFastMode()) {
    Serial.println("Fast partial mode FAILED");
    return false;
  }
  if (!display.refreshWindow(
          clockCanvas.getBuffer(), previousClock, sizeof(previousClock),
          CLOCK_X, CLOCK_Y, CLOCK_WIDTH, CLOCK_HEIGHT)) {
    Serial.println("Full clock refresh FAILED");
    return false;
  }
  return true;
}

void updateClock(time_t now) {
  struct tm localTime;
  localtime_r(&now, &localTime);

  char value[9];
  renderClock(localTime, value, sizeof(value));

  if (shouldCleanClock(
          partialRefreshCount, lastFullCleanCount, FULL_CLEAN_INTERVAL)) {
    if (!fullRefreshRetryReady(
            millis(), lastFullCleanAttemptMs, fullCleanAttemptRecorded,
            FULL_CLEAN_RETRY_INTERVAL_MS)) {
      return;
    }
    if (!drawClockWithFullRefresh()) {
      displayReady = false;
      fullCleanRecoveryPending = true;
      return;
    }
    fullCleanRecoveryPending = false;
    lastFullCleanCount = partialRefreshCount;
    Serial.printf("Clock %s full_clean_at=%lu heap=%u\n",
        value, static_cast<unsigned long>(partialRefreshCount),
        ESP.getFreeHeap());
    return;
  }

  const uint32_t started = millis();
  const bool refreshed = display.refreshWindow(
      clockCanvas.getBuffer(), previousClock, sizeof(previousClock),
      CLOCK_X, CLOCK_Y, CLOCK_WIDTH, CLOCK_HEIGHT);
  const uint32_t refreshMs = millis() - started;

  if (refreshed) {
    ++partialRefreshCount;
    Serial.printf("Clock %s partial=%lu refresh_ms=%lu heap=%u\n",
        value, static_cast<unsigned long>(partialRefreshCount),
        static_cast<unsigned long>(refreshMs), ESP.getFreeHeap());
  } else {
    const ClockFullRecoveryState recoveryState =
        clockFullRecoveryAfterPartialFailure(millis());
    displayReady = recoveryState.display_ready;
    fullCleanRecoveryPending = recoveryState.recovery_pending;
    lastFullCleanAttemptMs = recoveryState.retry_anchor;
    fullCleanAttemptRecorded = recoveryState.retry_anchor_recorded;
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

  const time_t now = time(nullptr);
  struct tm localTime = {};
  if (now >= 1000000000) localtime_r(&now, &localTime);
  char initialValue[9];
  renderClock(localTime, initialValue, sizeof(initialValue));
  if (!drawClockWithFullRefresh()) {
    fullCleanRecoveryPending = true;
    return;
  }

  lastDisplayedSecond = now >= 1000000000 ? now : 0;
  Serial.printf("Initial clock %s heap=%u\n", initialValue, ESP.getFreeHeap());
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
  if (!displayReady && fullCleanRecoveryPending && now >= 1000000000 &&
      fullRefreshRetryReady(
        nowMs, lastFullCleanAttemptMs, fullCleanAttemptRecorded,
        FULL_CLEAN_RETRY_INTERVAL_MS)) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    char recoveredValue[9];
    renderClock(localTime, recoveredValue, sizeof(recoveredValue));
    if (drawClockWithFullRefresh()) {
      fullCleanRecoveryPending = false;
      lastFullCleanCount = partialRefreshCount;
      displayReady = true;
      lastDisplayedSecond = now;
      Serial.printf("Clock %s full recovery complete heap=%u\n",
        recoveredValue, ESP.getFreeHeap());
    }
  }

  if (displayReady && now >= 1000000000 && now != lastDisplayedSecond) {
    lastDisplayedSecond = now;
    updateClock(now);
  }

  delay(10);
}
