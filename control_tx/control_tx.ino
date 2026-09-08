#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
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
constexpr uint8_t PIN_STATUS_LED = 2;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SEND_INTERVAL_US = 20000;  // 50 Hz
constexpr uint16_t SEQUENCE_BLOCK_SIZE = 50;
constexpr uint8_t ADC_SAMPLES = 8;
constexpr int16_t JOYSTICK_DEADZONE = 50;  // On the normalized -1000..1000 scale
constexpr bool INVERT_X = false;
constexpr bool INVERT_Y = true;

Preferences preferences;
const TeamPairConfig &config = teamPair(PAIR_ID);

uint16_t centerX = 2048;
uint16_t centerY = 2048;
uint32_t nextSequence = 1;
uint32_t reservedUntil = 1;
uint32_t nextSendAtUs = 0;
bool radioReady = false;

void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

  while (true) {
    digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
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
  digitalWrite(PIN_STATUS_LED, error == ESP_OK ? HIGH : LOW);
  if (error != ESP_OK) {
    Serial.printf("No se pudo encolar paquete %lu: %s\n",
                  static_cast<unsigned long>(message.seq), esp_err_to_name(error));
  }
  advanceSequenceReservation();
}

void setup() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_JOYSTICK_BUTTON, INPUT_PULLUP);

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("Iniciando control ESP-NOW, par %u\n", PAIR_ID);

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
  }
}
