#include <ESP8266WiFi.h>
#include <GxEPD2_BW.h>
#include "secrets.h"

// 5.83-inch V1 panel: CS=15, DC=0, RST=2, BUSY=4
GxEPD2_BW<GxEPD2_583, 32> display(GxEPD2_583(15, 0, 2, 4));

void drawHalfScreenPattern(bool leftBlack) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    if (leftBlack) {
      display.fillRect(0, 0, display.width() / 2, display.height(),
                       GxEPD_BLACK);
    } else {
      display.fillRect(display.width() / 2, 0,
                       display.width() - display.width() / 2,
                       display.height(), GxEPD_BLACK);
    }
  } while (display.nextPage());
}

void setup() {
  Serial.begin(115200);
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int attempt = 0;
       WiFi.status() != WL_CONNECTED && attempt < 40; ++attempt) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf(" %s RSSI=%d\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "FAILED",
                WiFi.RSSI());

  display.init(115200, true, 2, false);

  Serial.println("WiFi-on panel test: left BLACK, right WHITE");
  drawHalfScreenPattern(true);
  display.hibernate();
  Serial.println("Panel test complete; pattern will remain on screen");
}

void loop() {
  delay(1000);
}
