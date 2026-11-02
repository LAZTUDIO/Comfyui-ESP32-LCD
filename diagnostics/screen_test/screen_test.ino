// Display diagnostics for Waveshare ESP32-S3-Touch-LCD-3.5
// Cycles modes with the BOOT button (GPIO0): SPI speed / IPS(invert) variants.
// Each mode draws a high-contrast test card; watch the panel and serial log.
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TCA9554.h>
#include <TouchDrvFT6X36.hpp>

#define LCD_SCK 5
#define LCD_MOSI 1
#define LCD_MISO 2
#define LCD_DC 3
#define LCD_CS -1
#define LCD_BL 6
#define I2C_SDA 8
#define I2C_SCL 7

RTC_DATA_ATTR int mode = 0;

struct Cfg { int speed; bool ips; const char *label; };
static const Cfg cfgs[] = {
    {40000000, true, "40M ips"},
    {20000000, true, "20M ips"},
    {10000000, true, "10M ips"},
    {40000000, false, "40M noips"},
    {20000000, false, "20M noips"},
};
static const int nCfg = sizeof(cfgs) / sizeof(cfgs[0]);

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
Arduino_DataBus *bus;
Arduino_GFX *gfx;
int16_t dotX = -1, dotY = -1;

void lcd_reset() {
  TCA.write1(1, 1); delay(10);
  TCA.write1(1, 0); delay(10);
  TCA.write1(1, 1); delay(200);
}

void drawTestCard(const Cfg &c) {
  gfx->fillScreen(RGB565_RED);
  delay(1200);
  gfx->fillScreen(RGB565_GREEN);
  delay(1200);
  gfx->fillScreen(RGB565_BLUE);
  delay(1200);
  gfx->fillScreen(RGB565_WHITE);
  delay(1200);

  // quadrant pattern
  int w = gfx->width(), h = gfx->height();
  gfx->fillRect(0, 0, w / 2, h / 2, RGB565_RED);
  gfx->fillRect(w / 2, 0, w / 2, h / 2, RGB565_GREEN);
  gfx->fillRect(0, h / 2, w / 2, h / 2, RGB565_BLUE);
  gfx->fillRect(w / 2, h / 2, w / 2, h / 2, RGB565_BLACK);

  gfx->drawRect(2, 2, w - 4, h - 4, RGB565_YELLOW);
  gfx->drawRect(3, 3, w - 6, h - 6, RGB565_YELLOW);

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(10, 12);
  gfx->printf("MODE %d  %s", mode, c.label);
  gfx->setTextSize(3);
  gfx->setCursor(10, 44);
  gfx->printf("ComfyUI panel");
  gfx->setTextSize(1);
  gfx->setCursor(10, 80);
  gfx->printf("press BOOT to cycle modes");
  gfx->setCursor(10, 94);
  gfx->printf("%dx%d", w, h);

  // 1px checkerboard strip to expose bit alignment issues
  for (int x = 0; x < w; x += 2) {
    gfx->drawPixel(x, h - 30, RGB565_WHITE);
    gfx->drawPixel(x + 1, h - 30, RGB565_BLACK);
  }
  for (int x = 0; x < w; x++) gfx->drawFastHLine(x, h - 20, 20, (x / 16) % 2 ? RGB565_RED : RGB565_CYAN);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  if (mode >= nCfg) mode = 0;
  const Cfg &c = cfgs[mode];
  Serial.printf("\n=== display test mode %d (%s) ===\n", mode, c.label);
  pinMode(0, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();

  bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
  gfx = new Arduino_ST7796(bus, -1, 0, c.ips, 320, 480);
  gfx->begin(c.speed);
  Serial.printf("panel %dx%d\n", gfx->width(), gfx->height());
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  drawTestCard(c);
  Serial.println("test card drawn; press BOOT for next mode");
}

void loop() {
  static uint32_t last = 0;
  static bool pressed = false;
  int v = digitalRead(0);
  if (v == LOW && !pressed && millis() - last > 300) {
    pressed = true;
    last = millis();
  } else if (v == HIGH && pressed) {
    pressed = false;
    mode = (mode + 1) % nCfg;
    Serial.printf("switching to mode %d\n", mode);
    delay(100);
    ESP.restart();
  }

  // touch test: portrait raw (0..319, 0..479) rotated for landscape panel (480x320)
  int16_t x[1], y[1];
  uint8_t t = touch.getPoint(x, y, 1);
  if (dotX >= 0) {  // erase previous crosshair
    gfx->writeFillRect(dotX - 6, dotY - 6, 13, 13, RGB565_BLACK);
    gfx->writeFastHLine(dotX - 12, dotY, 25, RGB565_WHITE);
    gfx->writeFastVLine(dotX, dotY - 12, 25, RGB565_WHITE);
    dotX = -1;
  }
  if (t) {
    int px = x[0], py = y[0];
    int lx, ly;
    // rotation 90 candidate (swap + mirror): landscape x = raw y, y = 319 - raw x
    lx = py; ly = 319 - px;
    if (lx >= 0 && lx < 480 && ly >= 0 && ly < 320) {
      dotX = lx; dotY = ly;
      gfx->writeFillRect(lx - 6, ly - 6, 13, 13, RGB565_RED);
      gfx->writeFastHLine(lx - 12, ly, 25, RGB565_WHITE);
      gfx->writeFastVLine(lx, ly - 12, 25, RGB565_WHITE);
      static uint32_t lastPrint = 0;
      if (millis() - lastPrint > 150) { lastPrint = millis(); Serial.printf("touch raw %d,%d -> %d,%d\n", px, py, lx, ly); }
    }
  }
  delay(15);
}
