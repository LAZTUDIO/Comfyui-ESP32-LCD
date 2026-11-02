// Bring-up test for Waveshare ESP32-S3-Touch-LCD-3.5
// Verifies: TCA9554 expander (LCD reset), ST7796S SPI display, backlight, FT6336U touch.
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TCA9554.h>
#include <TouchDrvFT6X36.hpp>

#define LCD_DC   3
#define LCD_CS   -1
#define LCD_SCK  5
#define LCD_MOSI 1
#define LCD_MISO 2
#define LCD_BL   6
#define I2C_SDA  8
#define I2C_SCL  7
#define EXP_RST_CH 1  // TCA9554 EXIO1 = LCD reset

static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0x0000};

TCA9554 expander(0x20);
TouchDrvFT6X36 touch;
Arduino_GFX *gfx = nullptr;

void lcdResetViaExpander() {
  expander.pinMode1(EXP_RST_CH, OUTPUT);
  expander.write1(EXP_RST_CH, 1);
  delay(10);
  expander.write1(EXP_RST_CH, 0);
  delay(10);
  expander.write1(EXP_RST_CH, 1);
  delay(200);
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB-CDC time
  Serial.println("\n=== bringup: ESP32-S3-Touch-LCD-3.5 ===");
  Serial.printf("heap=%u psram=%u\n", ESP.getHeapSize(), ESP.getPsramSize());

  Wire.begin(I2C_SDA, I2C_SCL, 400000);

  byte found[8]; int n = 0;
  for (byte a = 0x10; a < 0x70; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && n < 8) found[n++] = a;
  }
  Serial.printf("I2C scan: %d devices:", n);
  for (int i = 0; i < n; i++) Serial.printf(" 0x%02X", found[i]);
  Serial.println();

  if (!expander.begin()) Serial.println("TCA9554 @0x20 FAILED");
  else Serial.println("TCA9554 ok");
  lcdResetViaExpander();

  Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
  gfx = new Arduino_ST7796(bus, -1 /*RST done via expander*/, 0 /*rotation*/, false /*8bit*/, 320, 480);
  if (!gfx->begin()) Serial.println("ST7796 begin FAILED");
  else Serial.printf("ST7796 ok %dx%d id=0x%04X\n", gfx->width(), gfx->height(), 0);

  pinMode(LCD_BL, OUTPUT);
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, 200);

  gfx->fillScreen(0x001F);
  delay(500);

  // touch init
  if (touch.begin(Wire, FT6X36_SLAVE_ADDRESS)) {
    Serial.println("FT6336 ok");
    touch.setMaxCoordinates(320, 480);
    touch.setSwapXY(false);
    touch.setMirrorXY(false, false);
  } else {
    Serial.println("FT6336 FAILED");
  }
}

void loop() {
  static unsigned long t0 = millis();
  static int ci = 0;
  if (millis() - t0 > 700) {
    t0 = millis();
    gfx->fillScreen(colors[ci % 5]);
    gfx->setTextColor(0xFFFF, colors[ci % 5]);
    gfx->setCursor(8, 8);
    gfx->printf("c%d %dx%d", ci, gfx->width(), gfx->height());
    ci++;
  }

  int16_t x[3], y[3];
  uint8_t p = touch.getPoint(x, y, 1);
  static uint8_t lastP = 0;
  if (p > 0 && p != lastP) {
    Serial.printf("touch raw: %d %d\n", x[0], y[0]);
  }
  if (p == 0 && lastP > 0) Serial.println("touch release");
  lastP = p;
  delay(20);
}
