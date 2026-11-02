// Bare-metal probe: TCA9554 reset state + raw 4-wire SPI to the panel.
#include <Wire.h>
#include <SPI.h>

#define LCD_SCK 5
#define LCD_MOSI 1
#define LCD_MISO 2
#define LCD_DC 3
#define LCD_CS 4
#define LCD_BL 6
#define I2C_SDA 8
#define I2C_SCL 7
#define TCA_ADDR 0x20

uint8_t tcaRead(uint8_t reg) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xEE;
  if (Wire.requestFrom((uint8_t)TCA_ADDR, (uint8_t)1) != 1) return 0xEE;
  return Wire.read();
}
void tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void cs(bool c) { digitalWrite(LCD_CS, c ? HIGH : LOW); }  // GPIO4 = panel CS per schematic
void cmd(uint8_t c) { digitalWrite(LCD_DC, LOW); SPI.transfer(c); digitalWrite(LCD_DC, HIGH); }
void cmdData(uint8_t c, const uint8_t *d, size_t n) {
  digitalWrite(LCD_DC, LOW); SPI.transfer(c); digitalWrite(LCD_DC, HIGH);
  for (size_t i = 0; i < n; i++) SPI.transfer(d[i]);
}
uint32_t spiReadBytes(uint8_t c, int n) {
  digitalWrite(LCD_DC, LOW); SPI.transfer(c); digitalWrite(LCD_DC, HIGH);
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | SPI.transfer(0x00);
  return v;
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t c[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  cmdData(0x2A, c, 4);
  uint8_t r[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmdData(0x2B, r, 4);
}

void fill(uint16_t color) {
  setWindow(0, 0, 319, 479);
  digitalWrite(LCD_DC, LOW); SPI.transfer(0x2C); digitalWrite(LCD_DC, HIGH);
  for (uint32_t i = 0; i < 320UL * 480UL; i++) SPI.transfer16(color);
}

void panelInit() {
  cmd(0x01); delay(150);            // SWRESET
  cmd(0x11); delay(150);            // SLPOUT
  uint8_t fmt = 0x55;               // 16-bit RGB565
  cmdData(0x3A, &fmt, 1);
  uint8_t madctl = 0x48;            // MX|BGR
  cmdData(0x36, &madctl, 1);
  cmd(0x21);                        // INVON (IPS)
  cmd(0x13);                        // NORON
  cmd(0x29); delay(120);            // DISPON
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== bare lcd probe ===");

  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_CS, LOW);  // keep panel selected
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  // probe TCA registers
  Serial.printf("TCA cfg=0x%02X out=0x%02X in=0x%02X\n", tcaRead(0x03), tcaRead(0x01), tcaRead(0x00));

  // configure only EXIO1 as output, others input; drive reset pulse
  tcaWrite(0x03, 0xFF);
  tcaWrite(0x03, 0xFD);
  tcaWrite(0x01, 0x02); delay(60);
  Serial.printf("TCA after setup cfg=0x%02X out=0x%02X\n", tcaRead(0x03), tcaRead(0x01));
  tcaWrite(0x01, 0x00); delay(120);
  tcaWrite(0x01, 0x02); delay(250);
  Serial.printf("TCA after reset out=0x%02X in=0x%02X\n", tcaRead(0x01), tcaRead(0x00));

  SPI.begin(LCD_SCK, LCD_MISO, LCD_MOSI, -1);
  const int modes[2] = {SPI_MODE0, SPI_MODE3};
  for (int m = 0; m < 2; m++) {
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, modes[m]));
    digitalWrite(LCD_DC, HIGH);
    uint32_t id = spiReadBytes(0x04, 4);
    uint32_t pm = spiReadBytes(0x0A, 2);
    uint32_t mad = spiReadBytes(0x0B, 2);
    Serial.printf("CS=low mode=%d: id=0x%08X powermode=0x%04X madctl=0x%04X\n", modes[m], id, pm, mad);
    SPI.endTransaction();
  }
  // try CS toggling variant
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, HIGH); delay(5); digitalWrite(LCD_CS, LOW); delay(5);
  uint32_t id2 = spiReadBytes(0x04, 4);
  Serial.printf("CS toggled: id=0x%08X\n", id2);
  SPI.endTransaction();

  panelInit();  fill(0xF800); delay(600);
  fill(0x07E0); delay(600);
  fill(0x001F); delay(600);
  fill(0xFFFF); delay(600);
  fill(0x0000);
  Serial.println("raw flashes done (red green blue white black)");
  SPI.endTransaction();
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 5000) {
    t = millis();
    SPI.beginTransaction(SPISettings(15000000, MSBFIRST, SPI_MODE0));
    Serial.printf("alive id=0x%08X madctl=0x%04X tca_out=0x%02X\n",
                  spiReadBytes(0x04, 4), spiReadBytes(0x0B, 2), tcaRead(0x01));
    fill(0xF800); delay(500);
    fill(0x001F); delay(500);
    fill(0x0000);
    SPI.endTransaction();
  }
  delay(50);
}
