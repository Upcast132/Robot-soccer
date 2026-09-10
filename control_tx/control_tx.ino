#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../soccer_protocol.h"
#include "../esp_now_compat.h"

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
constexpr uint32_t SEQUENCE_BLOCK_SIZE = 10000;
constexpr uint8_t ADC_SAMPLES = 8;
constexpr int16_t JOYSTICK_DEADZONE = 50;  // On the normalized -1000..1000 scale
constexpr bool INVERT_X = false;
constexpr bool INVERT_Y = true;

constexpr uint32_t STATUS_TIMEOUT_MS = 300;
constexpr uint16_t CALIBRATION_CENTER_MIN = 1536;
constexpr uint16_t CALIBRATION_CENTER_MAX = 2560;
constexpr uint16_t CALIBRATION_MAX_SPREAD = 100;
constexpr uint16_t CALIBRATION_SAMPLES = 128;
constexpr uint32_t BUTTON_STABLE_MS = 30;
constexpr uint8_t SENT_HISTORY_SIZE = 32;

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
uint8_t lastDisplayedStatus = 255;
uint32_t lastLcdUpdateMs = 0;

// All callback/loop shared data below is protected by statusMux.
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t consecutiveSendFailures = 0;
bool hasRobotStatus = false;
StatusMsg latestStatus = {};
uint32_t lastStatusAtMs = 0;
struct SentPacket {
  uint32_t seq;
  uint32_t sentAtMs;
  bool valid;
};
SentPacket sentHistory[SENT_HISTORY_SIZE] = {};
uint8_t nextHistorySlot = 0;  // loop only

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

void waitForButtonLevel(uint8_t level) {
  uint32_t stableSince = millis();
  while (static_cast<uint32_t>(millis() - stableSince) < BUTTON_STABLE_MS) {
    if (digitalRead(PIN_JOYSTICK_BUTTON) != level) {
      stableSince = millis();
    }
    delay(1);
  }
}

void calibrateJoystick() {
  // Radio is initialized only after successful calibration and button release.
  while (true) {
    lcdShowStatusLine("Centrar joystick");
    Serial.println("Centrar joystick; soltar y pulsar para calibrar");
    waitForButtonLevel(HIGH);
    waitForButtonLevel(LOW);
    uint32_t sumX = 0, sumY = 0;
    uint16_t minX = 4095, minY = 4095, maxX = 0, maxY = 0;
    for (uint16_t i = 0; i < CALIBRATION_SAMPLES; ++i) {
      const uint16_t x = analogRead(PIN_JOYSTICK_X);
      const uint16_t y = analogRead(PIN_JOYSTICK_Y);
      sumX += x;
      sumY += y;
      minX = min(minX, x);
      maxX = max(maxX, x);
      minY = min(minY, y);
      maxY = max(maxY, y);
      delay(3);
    }
    const uint16_t x = sumX / CALIBRATION_SAMPLES;
    const uint16_t y = sumY / CALIBRATION_SAMPLES;
    const bool valid = x >= CALIBRATION_CENTER_MIN && x <= CALIBRATION_CENTER_MAX &&
                       y >= CALIBRATION_CENTER_MIN && y <= CALIBRATION_CENTER_MAX &&
                       maxX - minX <= CALIBRATION_MAX_SPREAD &&
                       maxY - minY <= CALIBRATION_MAX_SPREAD;
    Serial.printf("Centro X=%u Y=%u; variacion X=%u Y=%u\n",
                  x, y, maxX - minX, maxY - minY);
    if (valid) {
      centerX = x;
      centerY = y;
      lcdShowStatusLine("Soltar boton");
      waitForButtonLevel(HIGH);
      return;
    }
    lcdShowStatusLine("Recalibrar");
    Serial.println("Recalibrar: centro fuera de rango o captura inestable");
    waitForButtonLevel(HIGH);
    delay(1000);
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
  // Unsigned wrap is intentional; the receiver uses modular comparison.
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

// Radio delivery is diagnostic only; application status confirms the robot.
void onDataSent(const SoccerSendInfo *info, esp_now_send_status_t status) {
  (void)info;
  portENTER_CRITICAL(&statusMux);
  if (status == ESP_NOW_SEND_SUCCESS) {
    consecutiveSendFailures = 0;
  } else if (consecutiveSendFailures < 255) {
    ++consecutiveSendFailures;
  }
  portEXIT_CRITICAL(&statusMux);
}

void onRobotStatus(const esp_now_recv_info_t *info,
                   const uint8_t *data, int length) {
  if (info == nullptr || data == nullptr || length != sizeof(StatusMsg) ||
      memcmp(info->src_addr, config.robotMac, sizeof(config.robotMac)) != 0 ||
      memcmp(info->des_addr, config.controlMac, sizeof(config.controlMac)) != 0) {
    return;
  }
  StatusMsg status = {};
  memcpy(&status, data, sizeof(status));
  if (status.magic != SOCCER_STATUS_MAGIC ||
      status.version != SOCCER_PROTOCOL_VERSION || status.pairId != PAIR_ID ||
      status.reserved != 0 ||
      (status.state != RobotState::WAITING && status.state != RobotState::DISARMED &&
       status.state != RobotState::ARMED) || status.crc16 != soccerStatusCrc(status)) {
    return;
  }
  portENTER_CRITICAL(&statusMux);
  const uint32_t now = millis();
  bool recentCommand = false;
  for (uint8_t i = 0; i < SENT_HISTORY_SIZE; ++i) {
    if (sentHistory[i].valid && sentHistory[i].seq == status.acceptedSeq &&
        static_cast<uint32_t>(now - sentHistory[i].sentAtMs) <= STATUS_TIMEOUT_MS) {
      recentCommand = true;
      break;
    }
  }
  if (recentCommand && (!hasRobotStatus ||
      soccerIsNewerSequence(status.acceptedSeq, latestStatus.acceptedSeq))) {
    latestStatus = status;
    lastStatusAtMs = now;
    hasRobotStatus = true;
  }
  portEXIT_CRITICAL(&statusMux);
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

  error = esp_now_register_recv_cb(onRobotStatus);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el receptor de estado", error);
  }

  radioReady = true;
  Serial.printf("Control del par %u listo en canal %u. Robot: ", PAIR_ID, WIFI_CHANNEL);
  printMac(config.robotMac);
  Serial.println();
}

void sendControlPacket() {
  RemoteMsg message = {};
  message.magic = SOCCER_COMMAND_MAGIC;
  message.version = SOCCER_PROTOCOL_VERSION;
  message.pairId = PAIR_ID;
  message.flags = digitalRead(PIN_JOYSTICK_BUTTON) == LOW
                      ? SOCCER_FLAG_JOYSTICK_BUTTON
                      : 0;
  message.seq = nextSequence;
  message.steering = normalizeAxis(readAveraged(PIN_JOYSTICK_X), centerX, INVERT_X);
  message.throttle = normalizeAxis(readAveraged(PIN_JOYSTICK_Y), centerY, INVERT_Y);
  message.crc16 = soccerMessageCrc(message);

  const uint8_t slot = nextHistorySlot;
  nextHistorySlot = (nextHistorySlot + 1) % SENT_HISTORY_SIZE;
  portENTER_CRITICAL(&statusMux);
  sentHistory[slot] = {message.seq, millis(), true};
  portEXIT_CRITICAL(&statusMux);

  const esp_err_t error = esp_now_send(
      config.robotMac, reinterpret_cast<const uint8_t *>(&message), sizeof(message));
  if (error != ESP_OK) {
    Serial.printf("No se pudo encolar paquete %lu: %s\n",
                  static_cast<unsigned long>(message.seq), esp_err_to_name(error));
    portENTER_CRITICAL(&statusMux);
    sentHistory[slot].valid = false;
    if (consecutiveSendFailures < 255) {
      ++consecutiveSendFailures;
    }
    portEXIT_CRITICAL(&statusMux);
  }
  advanceSequenceReservation();
}

void updateStatusIndicators() {
  portENTER_CRITICAL(&statusMux);
  const bool confirmed = hasRobotStatus;
  const RobotState state = latestStatus.state;
  const uint32_t statusAt = lastStatusAtMs;
  const uint8_t failures = consecutiveSendFailures;
  portEXIT_CRITICAL(&statusMux);

  const uint32_t now = millis();
  const bool fresh = confirmed && static_cast<uint32_t>(now - statusAt) <= STATUS_TIMEOUT_MS;
  const uint8_t display = !confirmed ? 0 : !fresh ? 1 :
                          state == RobotState::ARMED ? 3 : 2;
  const char *labels[] = {"Esperando robot", "Sin senal", "Desarmado", "Armado"};
  setStatusColor(display == 1 || display == 2,
                 display == 2 || display == 3, display == 0);
  if (display != lastDisplayedStatus || now - lastLcdUpdateMs >= LCD_UPDATE_INTERVAL_MS) {
    if (display != lastDisplayedStatus) {
      Serial.printf("%s (fallos de radio consecutivos: %u)\n", labels[display], failures);
    }
    lastLcdUpdateMs = now;
    lastDisplayedStatus = display;
    lcdShowStatusLine(labels[display]);
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

  updateStatusIndicators();
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
