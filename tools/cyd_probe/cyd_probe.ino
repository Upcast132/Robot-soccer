// Hardware probe for the ESP32-2432S024 (CYD). NOT part of the robot firmware.
// Shows on Serial (115200) and on the screen:
//  - the board MAC (WiFi STA),
//  - an I2C scan on the CST820 pins (SDA 33, SCL 32, RST 25, INT 21),
//  - raw CST820 touch registers at 0x15 if it answers,
//  - raw XPT2046 readings on the resistive pins (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36).
// Touch the screen while it runs and watch which source changes.
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

constexpr uint8_t SDA_PIN = 33, SCL_PIN = 32, TP_RST = 25, TP_INT = 21;
constexpr uint8_t XPT_CLK = 25, XPT_MOSI = 32, XPT_MISO = 39, XPT_CS = 33, XPT_IRQ = 36;
constexpr uint8_t PIN_LED[3] = {4, 16, 17};  // active low

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX() {
    {
      auto c = bus_.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
      bus_.config(c);
      panel_.setBus(&bus_);
    }
    {
      auto c = panel_.config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.invert = false;
      c.rgb_order = false;
      c.readable = false;
      panel_.config(c);
    }
    {
      auto c = light_.config();
      c.pin_bl = 27;
      c.freq = 44100;
      c.pwm_channel = 7;
      light_.config(c);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};

LGFX lcd;
uint8_t foundAddresses[8];
uint8_t foundCount = 0;

void scanI2c() {
  foundCount = 0;
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(60);
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && foundCount < sizeof(foundAddresses)) {
      foundAddresses[foundCount++] = a;
    }
  }
}

// CST820/CST816: regs 0x01 gesture, 0x02 finger count, 0x03..0x06 X/Y.
bool readCst(uint8_t &fingers, uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(0x15);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(0x15), static_cast<uint8_t>(6)) != 6) return false;
  Wire.read();
  fingers = Wire.read() & 0x0F;
  const uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}

// Software SPI (mode 0) with explicit pins so several wirings can be tried.
// Pin sets as {clk, mosi, miso, cs}.
constexpr uint8_t SET_A[4] = {25, 32, 39, 33};  // separate bus (2432S028-style)
constexpr uint8_t SET_B[4] = {14, 13, 12, 33};  // shared with the TFT bus

uint16_t xptRead(const uint8_t *p, uint8_t command) {
  digitalWrite(p[3], LOW);
  for (int8_t i = 7; i >= 0; --i) {
    digitalWrite(p[1], (command >> i) & 1);
    digitalWrite(p[0], HIGH);
    digitalWrite(p[0], LOW);
  }
  uint16_t value = 0;
  for (uint8_t i = 0; i < 16; ++i) {
    digitalWrite(p[0], HIGH);
    digitalWrite(p[0], LOW);
    value = (value << 1) | digitalRead(p[2]);
  }
  digitalWrite(p[3], HIGH);
  return (value >> 3) & 0x0FFF;
}

void prepare(const uint8_t *p) {
  pinMode(p[3], OUTPUT);
  digitalWrite(p[3], HIGH);
  pinMode(p[0], OUTPUT);
  digitalWrite(p[0], LOW);
  pinMode(p[1], OUTPUT);
  pinMode(p[2], INPUT);
}

void setup() {
  for (uint8_t p : PIN_LED) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  Serial.begin(115200);
  delay(500);
  WiFi.mode(WIFI_STA);
  Serial.printf("\nMAC STA: %s\n", WiFi.macAddress().c_str());

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);
  lcd.fillScreen(TFT_BLACK);
  // Color bars: rojo, verde, azul. Si se ven cambiados, rgb_order debe invertirse.
  lcd.fillRect(0, 0, 107, 20, TFT_RED);
  lcd.fillRect(107, 0, 106, 20, TFT_GREEN);
  lcd.fillRect(213, 0, 107, 20, TFT_BLUE);
  digitalWrite(PIN_LED[0], LOW);  // LED rojo encendido: confirma activo en bajo
  delay(300);
  digitalWrite(PIN_LED[0], HIGH);

  scanI2c();
  Serial.printf("I2C (SDA %u, SCL %u): %u dispositivo(s):", SDA_PIN, SCL_PIN, foundCount);
  for (uint8_t i = 0; i < foundCount; ++i) Serial.printf(" 0x%02X", foundAddresses[i]);
  Serial.println(foundCount ? "" : " ninguno");
  pinMode(TP_INT, INPUT_PULLUP);
}

void loop() {
  // Serial only: after the first pass the TFT pins are taken over by bit-banging.
  static uint32_t last = 0, lastBeat = 0;
  if (millis() - last < 30) return;
  last = millis();
  pinMode(XPT_IRQ, INPUT);
  const int irq = digitalRead(XPT_IRQ);
  prepare(SET_A);
  const uint16_t az = xptRead(SET_A, 0xB0), ax = xptRead(SET_A, 0x90), ay = xptRead(SET_A, 0xD0);
  prepare(SET_B);
  const uint16_t bz = xptRead(SET_B, 0xB0), bx = xptRead(SET_B, 0x90), by = xptRead(SET_B, 0xD0);
  xptRead(SET_B, 0x80);
  if (irq == 0 || millis() - lastBeat > 1000) {
    lastBeat = millis();
    Serial.printf("IRQ36=%d | A(25/32/39/33) z=%u x=%u y=%u | B(14/13/12/33) z=%u x=%u y=%u\n",
                  irq, az, ax, ay, bz, bx, by);
  }
}
