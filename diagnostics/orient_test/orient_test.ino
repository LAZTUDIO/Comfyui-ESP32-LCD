// Orientation + touch calibration for Waveshare ESP32-S3-Touch-LCD-3.5
// BOOT short press cycles: rot1/mapA -> rot1/mapB -> rot3/mapA -> rot3/mapB
// mapA = clockwise 90 (lx=479-rawY, ly=rawX), mapB = counter-clockwise (lx=rawY, ly=319-rawX)
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TCA9554.h>
#include <TouchDrvFT6X36.hpp>

#define LCD_SCK 5
#define LCD_MOSI 1
#define LCD_MISO 2
#define LCD_DC 3
#define LCD_BL 6
#define I2C_SDA 8
#define I2C_SCL 7

RTC_DATA_ATTR int cfgIdx = 0;

struct Cfg { uint8_t rot; bool mapA; const char *label; };
static const Cfg cfgs[] = {
    {1, true, "rot1 mapA CW"},
    {1, false, "rot1 mapB CCW"},
    {3, true, "rot3 mapA CW"},
    {3, false, "rot3 mapB CCW"},
};
static const int nCfg = 4;

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
Arduino_GFX *gfx;
uint32_t touchCount = 0;
int16_t lastRawX = -1, lastRawY = -1;
int16_t lastLvX = -1, lastLvY = -1;
int16_t touchOk = 0;

void drawStatus(const Cfg &c) {
  int w = gfx->width(), h = gfx->height();
  gfx->fillRect(0, 78, w, 34, RGB565_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(10, 82);
  gfx->printf("touches:%u  init:%d", touchCount, touchOk);
  gfx->setCursor(10, 104);
  gfx->printf("raw %d,%d -> %d,%d", lastRawX, lastRawY, lastLvX, lastLvY);
}

void drawCard(const Cfg &c) {
  int w = gfx->width(), h = gfx->height();
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(1, 1, w - 2, h - 2, RGB565_YELLOW);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(3);
  gfx->setCursor(14, 10);
  gfx->printf("%s", c.label);
  gfx->setTextSize(2);
  gfx->setCursor(14, 44);
  gfx->printf("%dx%d  touch the 4 corners", w, h);
  drawStatus(c);

  // corner markers, text must be upright
  gfx->setTextColor(RGB565_RED);
  gfx->setCursor(6, 6 + 26);
  gfx->printf("TL");
  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(w - 40, 6 + 26);
  gfx->printf("TR");
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(6, h - 34);
  gfx->printf("BL");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(w - 40, h - 34);
  gfx->printf("BR");

  // clickable targets
  gfx->fillRect(0, 0, 34, 34, RGB565_RED);
  gfx->fillRect(w - 34, 0, 34, 34, RGB565_GREEN);
  gfx->fillRect(0, h - 34, 34, 34, RGB565_BLUE);
  gfx->fillRect(w - 34, h - 34, 34, 34, RGB565_WHITE);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  if (cfgIdx >= nCfg) cfgIdx = 0;
  const Cfg &c = cfgs[cfgIdx];
  Serial.printf("\n=== orient test: %s ===\n", c.label);

  pinMode(0, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  TCA.write1(1, 1); delay(10);
  TCA.write1(1, 0); delay(120);
  TCA.write1(1, 1); delay(200);

  Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, LCD_SCK, LCD_MOSI, LCD_MISO);
  gfx = new Arduino_ST7796(bus, -1, 0, true, 320, 480);
  gfx->begin();
  gfx->setRotation(c.rot);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS) ? 1 : 0;
  Serial.printf("touch begin=%d\n", touchOk);
  drawCard(c);
  Serial.println("ready - press BOOT to cycle, touch corners to test");
}

void loop() {
  static uint32_t lastBtn = 0, lastRedraw = 0;
  static bool btnDown = false;

  int b = digitalRead(0);
  if (b == LOW && !btnDown && millis() - lastBtn > 300) btnDown = true;
  else if (b == HIGH && btnDown) {
    btnDown = false;
    cfgIdx = (cfgIdx + 1) % nCfg;
    Serial.printf("switch -> %s\n", cfgs[cfgIdx].label);
    delay(80);
    ESP.restart();
  }

  const Cfg &c = cfgs[cfgIdx];
  int16_t x[1], y[1];
  uint8_t t = touch.getPoint(x, y, 1);
  static bool down = false;
  if (t) {
    int px = x[0], py = y[0];
    int lx = c.mapA ? (479 - py) : py;
    int ly = c.mapA ? px : (319 - px);
    if (lx < 0 || lx > 479 || ly < 0 || ly > 319) { lx = -1; }
    touchCount++;
    lastRawX = px; lastRawY = py; lastLvX = lx; lastLvY = ly;
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 100) {
      lastPrint = millis();
      Serial.printf("touch raw=%d,%d -> lv=%d,%d\n", px, py, lx, ly);
      drawStatus(c);
    }
    if (lx >= 0) {
      gfx->fillCircle(lx, ly, 6, RGB565_MAGENTA);
      gfx->drawFastHLine(lx - 12, ly, 25, RGB565_MAGENTA);
      gfx->drawFastVLine(lx, ly - 12, 25, RGB565_MAGENTA);
    }
    down = true;
  } else if (down) {
    down = false;
    if (millis() - lastRedraw > 400) { lastRedraw = millis(); drawCard(c); }
  }
  delay(12);
}
