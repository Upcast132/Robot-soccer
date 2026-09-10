#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../soccer_protocol.h"

#if __has_include("team_config.h")
#include "team_config.h"
#else
#include "../team_config.h.example"
#warning "Compilando con configuracion de ejemplo; el control no se habilitara hasta crear control_tx/team_config.h."
#endif

// Compile each controller with its matching value: 1, 2, or 3.
#ifndef PAIR_ID
#define PAIR_ID 1
#endif

static_assert(PAIR_ID >= 1 && PAIR_ID <= TEAM_PAIR_COUNT,
              "PAIR_ID debe estar entre 1 y TEAM_PAIR_COUNT");

constexpr uint8_t PIN_JOYSTICK_X = 35;  // ADC1
constexpr uint8_t PIN_JOYSTICK_Y = 34;  // ADC1
constexpr uint8_t PIN_JOYSTICK_BUTTON = 27;

// Status RGB LED (common cathode). Replaces the old single-color status LED.
constexpr uint8_t PIN_LED_R = 25;
constexpr uint8_t PIN_LED_G = 26;
constexpr uint8_t PIN_LED_B = 33;

// 16x2 LCD with I2C backpack (PCF8574). Backpack VCC must be wired to the
// ESP32's 3.3V pin, NOT 5V/VIN: most backpacks pull SDA/SCL up to their own
// VCC, and 5V on those lines exceeds the ESP32 GPIO absolute maximum. Running
// the backpack at 3.3V is safe; only the backlight is a bit dimmer.
constexpr uint8_t LCD_ADDRESS_PRIMARY = 0x27;
constexpr uint8_t LCD_ADDRESS_SECONDARY = 0x3F;
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;
constexpr uint32_t LCD_UPDATE_INTERVAL_MS = 300;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SEND_INTERVAL_US = 20000;  // 50 Hz
constexpr uint16_t SEQUENCE_BLOCK_SIZE = 50;
constexpr uint8_t ADC_SAMPLES = 8;
constexpr int16_t JOYSTICK_DEADZONE = 50;  // On the normalized -1000..1000 scale
constexpr bool INVERT_X = false;
constexpr bool INVERT_Y = true;

// Consecutive send failures before showing "sin senal". A single dropped
// packet should not trip the alarm; several in a row should.
constexpr uint8_t SEND_FAILURE_THRESHOLD = 10;  // ~200ms at 50Hz

Preferences preferences;
const TeamPairConfig &config = teamPair(PAIR_ID);
LiquidCrystal_I2C lcd(LCD_ADDRESS_PRIMARY, LCD_COLUMNS, LCD_ROWS);

uint16_t centerX = 2048;
uint16_t centerY = 2048;
uint32_t nextSequence = 1;
uint32_t reservedUntil = 1;
uint32_t nextSendAtUs = 0;
bool radioReady = false;
bool lcdAvailable = false;
bool lastDisplayedLinked = true;
uint32_t lastLcdUpdateMs = 0;

// Written from the ESP-NOW send callback (WiFi task), read from loop() (app
// task). A single byte is naturally atomic on ESP32, so volatile alone is
// enough here; no critical section needed for a lone byte-sized flag.
volatile uint8_t consecutiveSendFailures = 0;

void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setStatusColor(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void lcdShowStatusLine(const char *text) {
  if (!lcdAvailable) {
    return;
  }
  lcd.setCursor(0, 1);
  lcd.print("                ");  // clear row 1 (16 spaces)
  lcd.setCursor(0, 1);
  lcd.print(text);
}

[[noreturn]] void fatalError(const char *message, esp_err_t error = ESP_OK) {
  Serial.print("ERROR: ");
  Serial.print(message);
  if (error != ESP_OK) {
    Serial.print(" (");
    Serial.print(esp_err_to_name(error));
    Serial.print(")");
  }
  Serial.println();

  lcdShowStatusLine("ERROR FATAL");

  while (true) {
    setStatusColor(true, false, false);
    delay(150);
    setStatusColor(false, false, false);
    delay(150);
  }
}

uint16_t readAveraged(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; ++i) {
    sum += analogRead(pin);
  }
  return static_cast<uint16_t>(sum / ADC_SAMPLES);
}

void calibrateJoystick() {
  Serial.println("Deja el joystick centrado: calibrando...");
  uint32_t sumX = 0;
  uint32_t sumY = 0;
  constexpr uint16_t CALIBRATION_SAMPLES = 128;

  for (uint16_t i = 0; i < CALIBRATION_SAMPLES; ++i) {
    sumX += readAveraged(PIN_JOYSTICK_X);
    sumY += readAveraged(PIN_JOYSTICK_Y);
    delay(3);
  }

  centerX = static_cast<uint16_t>(sumX / CALIBRATION_SAMPLES);
  centerY = static_cast<uint16_t>(sumY / CALIBRATION_SAMPLES);
  Serial.printf("Centro X=%u, Y=%u\n", centerX, centerY);

  // Values close to a rail usually mean a disconnected or badly wired joystick.
  if (centerX < 400 || centerX > 3695 || centerY < 400 || centerY > 3695) {
    fatalError("Joystick fuera de rango; revisa VCC, GND, VRx y VRy");
  }
}

int16_t normalizeAxis(uint16_t raw, uint16_t center, bool invert) {
  int32_t value;
  if (raw >= center) {
    const int32_t span = 4095 - center;
    value = span > 0 ? (static_cast<int32_t>(raw - center) * 1000) / span : 0;
  } else {
    value = -(static_cast<int32_t>(center - raw) * 1000) / center;
  }

  value = constrain(value, -1000, 1000);
  if (abs(value) < JOYSTICK_DEADZONE) {
    value = 0;
  }
  if (invert) {
    value = -value;
  }
  return static_cast<int16_t>(value);
}

void reserveSequenceBlock() {
  nextSequence = preferences.getULong("next_seq", 1);
  reservedUntil = nextSequence + SEQUENCE_BLOCK_SIZE;
  if (reservedUntil < nextSequence) {
    // A wrap after years of continuous use starts a new sequence epoch. Both
    // devices should be power-cycled together if this ever happens.
    nextSequence = 1;
    reservedUntil = 1 + SEQUENCE_BLOCK_SIZE;
  }
  if (preferences.putULong("next_seq", reservedUntil) == 0) {
    fatalError("No se pudo reservar el contador anti-replay en NVS");
  }
}

void advanceSequenceReservation() {
  ++nextSequence;
  if (nextSequence == reservedUntil) {
    reservedUntil += SEQUENCE_BLOCK_SIZE;
    if (preferences.putULong("next_seq", reservedUntil) == 0) {
      fatalError("No se pudo actualizar el contador anti-replay en NVS");
    }
  }
}

// Real delivery confirmation (peer ACK at the radio layer), not just "queued
// locally". This is what actually makes the RGB LED and LCD mean something:
// a robot out of range will show failures here even though esp_now_send()
// itself returns ESP_OK.
void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  if (status == ESP_NOW_SEND_SUCCESS) {
    consecutiveSendFailures = 0;
  } else if (consecutiveSendFailures < 255) {
    ++consecutiveSendFailures;
  }
}

void initializeLcd() {
  Wire.begin();

  uint8_t address = LCD_ADDRESS_PRIMARY;
  Wire.beginTransmission(LCD_ADDRESS_PRIMARY);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(LCD_ADDRESS_SECONDARY);
    if (Wire.endTransmission() == 0) {
      address = LCD_ADDRESS_SECONDARY;
    } else {
      Serial.println("Aviso: LCD I2C no detectado en 0x27 ni 0x3F. Continuando sin pantalla.");
      lcdAvailable = false;
      return;
    }
  }

  lcd = LiquidCrystal_I2C(address, LCD_COLUMNS, LCD_ROWS);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  char line0[LCD_COLUMNS + 1];
  snprintf(line0, sizeof(line0), "Control PAIR %u", PAIR_ID);
  lcd.setCursor(0, 0);
  lcd.print(line0);

  lcdAvailable = true;
  lcdShowStatusLine("Iniciando...");
}

void initializeRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);

  esp_err_t error = esp_wifi_set_ps(WIFI_PS_NONE);
  if (error != ESP_OK) {
    fatalError("No se pudo desactivar el ahorro de energia Wi-Fi", error);
  }
  error = esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (error != ESP_OK) {
    fatalError("No se pudo fijar el canal Wi-Fi", error);
  }

  uint8_t localMac[6] = {};
  error = esp_wifi_get_mac(WIFI_IF_STA, localMac);
  if (error != ESP_OK) {
    fatalError("No se pudo leer la MAC local", error);
  }
  Serial.print("MAC del control: ");
  printMac(localMac);
  Serial.println();
  if (memcmp(localMac, config.controlMac, sizeof(localMac)) != 0) {
    Serial.print("MAC esperada para este PAIR_ID: ");
    printMac(config.controlMac);
    Serial.println();
    fatalError("PAIR_ID o team_config.h no corresponde a este control");
  }

  error = esp_now_init();
  if (error != ESP_OK) {
    fatalError("No se pudo iniciar ESP-NOW", error);
  }
  error = esp_now_set_pmk(config.pmk);
  if (error != ESP_OK) {
    fatalError("No se pudo configurar la PMK", error);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, config.robotMac, sizeof(peer.peer_addr));
  memcpy(peer.lmk, config.lmk, sizeof(peer.lmk));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  error = esp_now_add_peer(&peer);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el robot como peer cifrado", error);
  }

  error = esp_now_register_send_cb(onDataSent);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el callback de envio", error);
  }

  radioReady = true;
  Serial.printf("Control del par %u listo en canal %u. Robot: ", PAIR_ID, WIFI_CHANNEL);
  printMac(config.robotMac);
  Serial.println();
}

void sendControlPacket() {
  RemoteMsg message = {};
  message.magic = SOCCER_PROTOCOL_MAGIC;
  message.version = SOCCER_PROTOCOL_VERSION;
  message.pairId = PAIR_ID;
  message.flags = digitalRead(PIN_JOYSTICK_BUTTON) == LOW
                      ? SOCCER_FLAG_JOYSTICK_BUTTON
                      : 0;
  message.seq = nextSequence;
  message.steering = normalizeAxis(readAveraged(PIN_JOYSTICK_X), centerX, INVERT_X);
  message.throttle = normalizeAxis(readAveraged(PIN_JOYSTICK_Y), centerY, INVERT_Y);
  message.crc16 = soccerMessageCrc(message);

  const esp_err_t error = esp_now_send(
      config.robotMac, reinterpret_cast<const uint8_t *>(&message), sizeof(message));
  if (error != ESP_OK) {
    Serial.printf("No se pudo encolar paquete %lu: %s\n",
                  static_cast<unsigned long>(message.seq), esp_err_to_name(error));
    if (consecutiveSendFailures < 255) {
      ++consecutiveSendFailures;
    }
  }
  advanceSequenceReservation();
}

void updateStatusIndicators() {
  const bool linked = consecutiveSendFailures < SEND_FAILURE_THRESHOLD;
  setStatusColor(!linked, linked, false);

  const uint32_t now = millis();
  if (linked != lastDisplayedLinked || (now - lastLcdUpdateMs) >= LCD_UPDATE_INTERVAL_MS) {
    lastLcdUpdateMs = now;
    lastDisplayedLinked = linked;
    lcdShowStatusLine(linked ? "Enlazado" : "Sin senal");
  }
}

void setup() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setStatusColor(false, false, true);  // blue: starting up
  pinMode(PIN_JOYSTICK_BUTTON, INPUT_PULLUP);

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("Iniciando control ESP-NOW, par %u\n", PAIR_ID);

  initializeLcd();

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_JOYSTICK_X, ADC_11db);
  analogSetPinAttenuation(PIN_JOYSTICK_Y, ADC_11db);
  calibrateJoystick();

  if (!preferences.begin("soccer_tx", false)) {
    fatalError("No se pudo abrir NVS");
  }
  reserveSequenceBlock();
  initializeRadio();
  nextSendAtUs = micros();

  setStatusColor(false, true, false);  // green: ready
  lcdShowStatusLine("Enlazado");
}

void loop() {
  if (!radioReady) {
    return;
  }

  const uint32_t now = micros();
  if (static_cast<int32_t>(now - nextSendAtUs) >= 0) {
    // Schedule from the current time to avoid a burst after any long pause.
    nextSendAtUs = now + SEND_INTERVAL_US;
    sendControlPacket();
    updateStatusIndicators();
  }
}
