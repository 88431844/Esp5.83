#include <Arduino.h>

#include <Wire.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <U8g2_for_Adafruit_GFX.h>

#define ENABLE_GxEPD2_GFX 0

GxEPD2_BW<GxEPD2_583, GxEPD2_583::HEIGHT / 2> display(GxEPD2_583(/*CS=15*/ 15, /*DC=4*/ 4, /*RST=5*/ 2, /*BUSY=16*/ 5));

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

void helloArduino()
{
  Serial.println("helloArduino");
  uint16_t bg = GxEPD_WHITE;
  uint16_t fg = GxEPD_BLACK;
  u8g2Fonts.setFontMode(1);                // use u8g2 transparent mode (this is default)
  u8g2Fonts.setFontDirection(0);           // left to right (this is default)
  u8g2Fonts.setForegroundColor(fg);        // apply Adafruit GFX color
  u8g2Fonts.setBackgroundColor(bg);        // apply Adafruit GFX color
  u8g2Fonts.setFont(u8g2_font_helvR14_tf); // select u8g2 font from here: https://github.com/olikraus/u8g2/wiki/fntlistall
  uint16_t x = (display.width() - 160) / 2;
  uint16_t y = display.height() / 4;
  display.setPartialWindow(0, y - 14, display.width(), 20);
  display.firstPage();
  do
  {
    display.fillScreen(bg);
    u8g2Fonts.setCursor(x, y);
    u8g2Fonts.println("Hello Arduino!");
  } while (display.nextPage());
  Serial.println("helloArduino done");
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);
  display.init();
  u8g2Fonts.begin(display); // connect u8g2 procedures to Adafruit GFX
  helloArduino();
}

void loop()
{
  // put your main code here, to run repeatedly:
}