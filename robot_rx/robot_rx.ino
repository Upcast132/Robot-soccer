#include <Arduino.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../soccer_protocol.h"

#if __has_include("team_config.h")
#include "team_config.h"
#else
#include "../team_config.h.example"
#warning "Compilando con configuracion de ejemplo; el robot no se habilitara hasta crear robot_rx/team_config.h."
#endif

// Compile each robot with its matching value: 1, 2, or 3.
#ifndef PAIR_ID
#define PAIR_ID 1
#endif

static_assert(PAIR_ID >= 1 && PAIR_ID <= TEAM_PAIR_COUNT,
              "PAIR_ID debe estar entre 1 y TEAM_PAIR_COUNT");

constexpr uint8_t PIN_LEFT_IN1 = 25;
constexpr uint8_t PIN_LEFT_IN2 = 26;
constexpr uint8_t PIN_LEFT_ENABLE = 32;
constexpr uint8_t PIN_RIGHT_IN1 = 27;
constexpr uint8_t PIN_RIGHT_IN2 = 14;
constexpr uint8_t PIN_RIGHT_ENABLE = 33;

// Status RGB LED (common cathode). Replaces the old single-color status LED.
// Chosen to avoid every pin already used by the L298N driver above.
constexpr uint8_t PIN_LED_R = 17;
constexpr uint8_t PIN_LED_G = 16;
constexpr uint8_t PIN_LED_B = 4;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t FAILSAFE_TIMEOUT_MS = 300;
constexpr uint32_t PWM_FREQUENCY_HZ = 5000;
constexpr uint8_t PWM_RESOLUTION_BITS = 8;
constexpr uint8_t PWM_FULL_SCALE = (1U << PWM_RESOLUTION_BITS) - 1;

// Start conservatively with a 2S battery and 3-6 V motors. Measure motor
// voltage/current under load before increasing this value.
constexpr uint8_t MOTOR_MAX_DUTY = 200;
constexpr uint8_t MOTOR_MIN_DUTY = 60;
constexpr int16_t COMMAND_DEADZONE = 50;
constexpr bool LEFT_MOTOR_REVERSED = false;
constexpr bool RIGHT_MOTOR_REVERSED = false;

const TeamPairConfig &config = teamPair(PAIR_ID);
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
RemoteMsg latestMessage = {};
uint32_t latestPacketAtMs = 0;
uint32_t lastAcceptedSequence = 0;
bool hasAcceptedPacket = false;
bool failsafeActive = true;
uint32_t lastAppliedSequence = 0;

void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setStatusColor(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void setBridgePins(uint8_t in1, uint8_t in2, uint8_t enablePin,
                   int16_t command, bool reversed) {
  command = constrain(command, -1000, 1000);
  if (reversed) {
    command = -command;
  }

  // Remove drive before changing bridge direction to reduce current spikes.
  ledcWrite(enablePin, 0);

  if (command == 0) {
    // Both outputs low with the bridge enabled provides active braking.
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    ledcWrite(enablePin, PWM_FULL_SCALE);
    return;
  }

  const bool forward = command > 0;
  digitalWrite(in1, forward ? HIGH : LOW);
  digitalWrite(in2, forward ? LOW : HIGH);

  const uint16_t magnitude = abs(command);
  const uint8_t duty = static_cast<uint8_t>(
      map(magnitude, 1, 1000, MOTOR_MIN_DUTY, MOTOR_MAX_DUTY));
  ledcWrite(enablePin, duty);
}

void drive(int16_t left, int16_t right) {
  setBridgePins(PIN_LEFT_IN1, PIN_LEFT_IN2, PIN_LEFT_ENABLE,
                left, LEFT_MOTOR_REVERSED);
  setBridgePins(PIN_RIGHT_IN1, PIN_RIGHT_IN2, PIN_RIGHT_ENABLE,
                right, RIGHT_MOTOR_REVERSED);
}

void brakeAll() {
  drive(0, 0);
}

void mixAndDrive(const RemoteMsg &message) {
  // The joystick button acts as a hold-to-stop emergency control.
  if ((message.flags & SOCCER_FLAG_JOYSTICK_BUTTON) != 0) {
    brakeAll();
    return;
  }

  int32_t throttle = message.throttle;
  int32_t steering = message.steering;
  if (abs(throttle) < COMMAND_DEADZONE) {
    throttle = 0;
  }
  if (abs(steering) < COMMAND_DEADZONE) {
    steering = 0;
  }

  int32_t left = throttle + steering;
  int32_t right = throttle - steering;
  const int32_t largest = max(abs(left), abs(right));
  if (largest > 1000) {
    left = (left * 1000) / largest;
    right = (right * 1000) / largest;
  }
  drive(static_cast<int16_t>(left), static_cast<int16_t>(right));
}

bool isNewerSequence(uint32_t candidate, uint32_t previous) {
  // Signed subtraction keeps comparisons valid across the uint32_t wrap.
  return static_cast<int32_t>(candidate - previous) > 0;
}

void onDataReceived(const esp_now_recv_info_t *info,
                    const uint8_t *incomingData, int length) {
  if (info == nullptr || incomingData == nullptr || length != sizeof(RemoteMsg)) {
    return;
  }
  if (memcmp(info->src_addr, config.controlMac, sizeof(config.controlMac)) != 0) {
    return;
  }

  RemoteMsg message = {};
  memcpy(&message, incomingData, sizeof(message));
  if (message.magic != SOCCER_PROTOCOL_MAGIC ||
      message.version != SOCCER_PROTOCOL_VERSION ||
      message.pairId != PAIR_ID ||
      message.crc16 != soccerMessageCrc(message)) {
    return;
  }

  portENTER_CRITICAL(&commandMux);
  if (hasAcceptedPacket && !isNewerSequence(message.seq, lastAcceptedSequence)) {
    portEXIT_CRITICAL(&commandMux);
    return;
  }
  lastAcceptedSequence = message.seq;
  latestMessage = message;
  latestPacketAtMs = millis();
  hasAcceptedPacket = true;
  portEXIT_CRITICAL(&commandMux);
}

[[noreturn]] void fatalError(const char *message, esp_err_t error = ESP_OK) {
  brakeAll();
  Serial.print("ERROR: ");
  Serial.print(message);
  if (error != ESP_OK) {
    Serial.print(" (");
    Serial.print(esp_err_to_name(error));
    Serial.print(")");
  }
  Serial.println();

  while (true) {
    setStatusColor(true, false, false);
    delay(150);
    setStatusColor(false, false, false);
    delay(150);
  }
}

void initializeMotors() {
  pinMode(PIN_LEFT_IN1, OUTPUT);
  pinMode(PIN_LEFT_IN2, OUTPUT);
  pinMode(PIN_RIGHT_IN1, OUTPUT);
  pinMode(PIN_RIGHT_IN2, OUTPUT);

  if (!ledcAttach(PIN_LEFT_ENABLE, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS) ||
      !ledcAttach(PIN_RIGHT_ENABLE, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS)) {
    fatalError("No se pudieron configurar los canales PWM");
  }
  brakeAll();
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
  Serial.print("MAC del robot: ");
  printMac(localMac);
  Serial.println();
  if (memcmp(localMac, config.robotMac, sizeof(localMac)) != 0) {
    Serial.print("MAC esperada para este PAIR_ID: ");
    printMac(config.robotMac);
    Serial.println();
    fatalError("PAIR_ID o team_config.h no corresponde a este robot");
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
  memcpy(peer.peer_addr, config.controlMac, sizeof(peer.peer_addr));
  memcpy(peer.lmk, config.lmk, sizeof(peer.lmk));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  error = esp_now_add_peer(&peer);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el control como peer cifrado", error);
  }

  error = esp_now_register_recv_cb(onDataReceived);
  if (error != ESP_OK) {
    fatalError("No se pudo registrar el receptor ESP-NOW", error);
  }

  Serial.printf("Robot del par %u listo en canal %u. Control: ", PAIR_ID, WIFI_CHANNEL);
  printMac(config.controlMac);
  Serial.println();
}

void setup() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setStatusColor(false, false, true);  // blue: starting up, no packet yet

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("Iniciando robot ESP-NOW, par %u\n", PAIR_ID);

  initializeMotors();
  initializeRadio();
}

void loop() {
  RemoteMsg message = {};
  uint32_t packetAtMs = 0;
  bool packetAvailable = false;

  portENTER_CRITICAL(&commandMux);
  packetAvailable = hasAcceptedPacket;
  if (packetAvailable) {
    message = latestMessage;
    packetAtMs = latestPacketAtMs;
  }
  portEXIT_CRITICAL(&commandMux);

  const uint32_t now = millis();
  const bool linkIsFresh = packetAvailable &&
                           static_cast<uint32_t>(now - packetAtMs) <= FAILSAFE_TIMEOUT_MS;

  if (!linkIsFresh) {
    if (!failsafeActive) {
      brakeAll();
      failsafeActive = true;
      Serial.println("Failsafe activo: motores detenidos");
    }
    if (hasAcceptedPacket) {
      setStatusColor(true, false, false);  // red: link lost, was connected before
    } else {
      setStatusColor(false, false, true);  // blue: still waiting for the first packet
    }
    delay(1);
    return;
  }

  if (failsafeActive) {
    failsafeActive = false;
    Serial.println("Enlace valido: control habilitado");
  }
  setStatusColor(false, true, false);  // green: link active
  if (message.seq != lastAppliedSequence) {
    mixAndDrive(message);
    lastAppliedSequence = message.seq;
  }
  delay(1);
}
