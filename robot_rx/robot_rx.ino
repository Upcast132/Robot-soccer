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
constexpr uint32_t ARM_NEUTRAL_MS = 500;
constexpr uint32_t STATUS_INTERVAL_MS = 100;  // 10 Hz, plus state changes
constexpr uint32_t MOTOR_UPDATE_MS = 10;
constexpr int16_t MOTOR_RAMP_STEP = 40;
constexpr uint32_t MOTOR_REVERSE_PAUSE_MS = 20;
constexpr bool LEFT_MOTOR_REVERSED = false;
constexpr bool RIGHT_MOTOR_REVERSED = false;

const TeamPairConfig &config = teamPair(PAIR_ID);
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
RemoteMsg latestMessage = {};
uint32_t latestPacketAtMs = 0;
uint32_t lastAcceptedSequence = 0;
bool hasAcceptedPacket = false;
// Gesture and safety transitions are evaluated for EVERY accepted packet under
// commandMux, so a short stop press cannot be overwritten by a later command.
RobotState robotState = RobotState::WAITING;
enum class ArmPhase : uint8_t { NEED_RELEASE, NEUTRAL, READY, PRESSED };
ArmPhase armPhase = ArmPhase::NEED_RELEASE;
uint32_t neutralSinceMs = 0;
bool brakePending = true;
uint32_t stateGeneration = 0;

// Motor output and status sending belong exclusively to loop().
struct MotorRamp {
  int16_t output = 0;
  int8_t lastDirection = 0;
  uint32_t zeroSinceMs = 0;
  bool holdingZero = false;
};
MotorRamp leftRamp, rightRamp;
int16_t targetLeft = 0, targetRight = 0;
uint32_t lastMotorUpdateMs = 0;
bool leftPwmReady = false, rightPwmReady = false;
uint32_t lastStatusSentMs = 0;
uint32_t sentStateGeneration = 0;
bool statusSent = false;

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

  const bool pwmReady = enablePin == PIN_LEFT_ENABLE ? leftPwmReady : rightPwmReady;
  if (!pwmReady) {
    // Also leave a failed/unattached PWM channel in the braking pin state.
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    digitalWrite(enablePin, HIGH);
    return;
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
  leftRamp = {};
  rightRamp = {};
  leftRamp.zeroSinceMs = rightRamp.zeroSinceMs = millis();
  leftRamp.holdingZero = rightRamp.holdingZero = true;
  targetLeft = targetRight = 0;
  lastMotorUpdateMs = millis();
  drive(0, 0);
}

void updateTargets(const RemoteMsg &message) {
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
  targetLeft = static_cast<int16_t>(left);
  targetRight = static_cast<int16_t>(right);
}

// Explicit prototype avoids Arduino sketch auto-prototypes preceding the type.
int16_t updateMotorRamp(MotorRamp &motor, int16_t target, uint32_t now);
int16_t updateMotorRamp(MotorRamp &motor, int16_t target, uint32_t now) {
  if (motor.output == 0 && motor.holdingZero) {
    if (static_cast<uint32_t>(now - motor.zeroSinceMs) < MOTOR_REVERSE_PAUSE_MS) {
      return 0;
    }
    motor.holdingZero = false;
  }
  const int8_t targetDirection = target > 0 ? 1 : target < 0 ? -1 : 0;
  // Decelerate to zero first. Remember the previous direction even at zero.
  const bool reversing = targetDirection != 0 && motor.lastDirection != 0 &&
                         targetDirection != motor.lastDirection;
  const int16_t effectiveTarget = reversing && motor.output != 0 ? 0 : target;
  const int16_t previous = motor.output;
  if (motor.output < effectiveTarget) {
    motor.output = min(static_cast<int16_t>(motor.output + MOTOR_RAMP_STEP), effectiveTarget);
  } else if (motor.output > effectiveTarget) {
    motor.output = max(static_cast<int16_t>(motor.output - MOTOR_RAMP_STEP), effectiveTarget);
  }
  if (motor.output != 0) {
    motor.lastDirection = motor.output > 0 ? 1 : -1;
  } else if (previous != 0) {
    motor.zeroSinceMs = now;
    motor.holdingZero = true;
  }
  return motor.output;
}

// The following state helpers require commandMux to be held by the caller.
void setRobotState(RobotState state) {
  if (robotState != state) {
    robotState = state;
    ++stateGeneration;
  }
}

void cancelArmGesture() {
  armPhase = ArmPhase::NEED_RELEASE;
  neutralSinceMs = 0;
}

void loseLink() {
  setRobotState(RobotState::WAITING);
  cancelArmGesture();
  brakePending = true;
}

void processArmGesture(const RemoteMsg &message, uint32_t now) {
  const bool pressed = (message.flags & SOCCER_FLAG_JOYSTICK_BUTTON) != 0;
  const bool neutral = abs(message.throttle) < COMMAND_DEADZONE &&
                       abs(message.steering) < COMMAND_DEADZONE;
  if (robotState == RobotState::WAITING) {
    setRobotState(RobotState::DISARMED);
  }
  if (robotState == RobotState::ARMED) {
    if (pressed) {
      setRobotState(RobotState::DISARMED);
      cancelArmGesture();
      brakePending = true;
    }
    return;
  }
  if (!neutral) {
    cancelArmGesture();
    return;
  }
  switch (armPhase) {
    case ArmPhase::NEED_RELEASE:
      if (!pressed) {
        neutralSinceMs = now;
        armPhase = ArmPhase::NEUTRAL;
      }
      break;
    case ArmPhase::NEUTRAL:
      if (pressed) {
        // A new press may be the packet that completes the 500 ms interval.
        if (static_cast<uint32_t>(now - neutralSinceMs) >= ARM_NEUTRAL_MS) {
          armPhase = ArmPhase::PRESSED;
        } else {
          cancelArmGesture();
        }
      } else if (static_cast<uint32_t>(now - neutralSinceMs) >= ARM_NEUTRAL_MS) {
        armPhase = ArmPhase::READY;
      }
      break;
    case ArmPhase::READY:
      if (pressed) {
        armPhase = ArmPhase::PRESSED;
      }
      break;
    case ArmPhase::PRESSED:
      if (!pressed) {
        setRobotState(RobotState::ARMED);
        cancelArmGesture();
      }
      break;
  }
}

void onDataReceived(const esp_now_recv_info_t *info,
                    const uint8_t *incomingData, int length) {
  if (info == nullptr || incomingData == nullptr || length != sizeof(RemoteMsg)) {
    return;
  }
  if (memcmp(info->src_addr, config.controlMac, sizeof(config.controlMac)) != 0) {
    return;
  }
  if (memcmp(info->des_addr, config.robotMac, sizeof(config.robotMac)) != 0) {
    return;
  }

  RemoteMsg message = {};
  memcpy(&message, incomingData, sizeof(message));
  if (message.magic != SOCCER_COMMAND_MAGIC ||
      message.version != SOCCER_PROTOCOL_VERSION ||
      message.pairId != PAIR_ID ||
      (message.flags & ~SOCCER_KNOWN_FLAGS) != 0 ||
      message.throttle < -1000 || message.throttle > 1000 ||
      message.steering < -1000 || message.steering > 1000 ||
      message.crc16 != soccerMessageCrc(message)) {
    return;
  }

  portENTER_CRITICAL(&commandMux);
  if (hasAcceptedPacket && !soccerIsNewerSequence(message.seq, lastAcceptedSequence)) {
    portEXIT_CRITICAL(&commandMux);
    return;
  }
  const uint32_t now = millis();
  // Detect a gap here too, even if loop() was delayed while packets resumed.
  if (hasAcceptedPacket && now - latestPacketAtMs > FAILSAFE_TIMEOUT_MS) {
    loseLink();
  }
  processArmGesture(message, now);
  lastAcceptedSequence = message.seq;
  latestMessage = message;
  latestPacketAtMs = now;
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

  pinMode(PIN_LEFT_ENABLE, OUTPUT);
  pinMode(PIN_RIGHT_ENABLE, OUTPUT);
  brakeAll();
  leftPwmReady = ledcAttach(PIN_LEFT_ENABLE, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  rightPwmReady = ledcAttach(PIN_RIGHT_ENABLE, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  if (!leftPwmReady || !rightPwmReady) {
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
  initializeMotors();
  delay(300);
  Serial.printf("Iniciando robot ESP-NOW, par %u\n", PAIR_ID);

  initializeRadio();
}

void sendRobotStatus(const RemoteMsg &message, RobotState state,
                     uint32_t generation, uint32_t now) {
  if (statusSent && generation == sentStateGeneration &&
      now - lastStatusSentMs < STATUS_INTERVAL_MS) {
    return;
  }
  StatusMsg status = {};
  status.magic = SOCCER_STATUS_MAGIC;
  status.version = SOCCER_PROTOCOL_VERSION;
  status.pairId = PAIR_ID;
  status.state = state;
  status.acceptedSeq = message.seq;
  status.crc16 = soccerStatusCrc(status);
  const esp_err_t error = esp_now_send(config.controlMac,
      reinterpret_cast<const uint8_t *>(&status), sizeof(status));
  // Rate-limit failed attempts too. The next periodic status retries naturally.
  lastStatusSentMs = now;
  sentStateGeneration = generation;
  statusSent = true;
  if (error != ESP_OK) {
    Serial.printf("No se pudo encolar estado: %s\n", esp_err_to_name(error));
  }
}

void loop() {
  portENTER_CRITICAL(&commandMux);
  const uint32_t now = millis();
  const bool packetAvailable = hasAcceptedPacket;
  const bool linkIsFresh = packetAvailable &&
                          now - latestPacketAtMs <= FAILSAFE_TIMEOUT_MS;
  if (!linkIsFresh) {
    loseLink();
  }
  const RemoteMsg message = latestMessage;
  const RobotState state = robotState;
  const uint32_t generation = stateGeneration;
  const bool mustBrake = brakePending;
  brakePending = false;
  portEXIT_CRITICAL(&commandMux);

  // Never perform GPIO/PWM, ESP-NOW sends or Serial work in the callback/lock.
  if (mustBrake || state != RobotState::ARMED || !linkIsFresh) {
    brakeAll();
  } else {
    updateTargets(message);
    if (now - lastMotorUpdateMs >= MOTOR_UPDATE_MS) {
      lastMotorUpdateMs = now;  // No catch-up burst after a delayed iteration.
      const int16_t left = updateMotorRamp(leftRamp, targetLeft, now);
      const int16_t right = updateMotorRamp(rightRamp, targetRight, now);
      drive(left, right);
    }
  }

  if (!packetAvailable) {
    setStatusColor(false, false, true);
  } else if (!linkIsFresh) {
    setStatusColor(true, false, false);
  } else {
    setStatusColor(state != RobotState::ARMED, true, false);
  }
  if (packetAvailable) {
    if (!statusSent || generation != sentStateGeneration) {
      Serial.printf("Estado robot: %s\n", state == RobotState::ARMED ? "Armado" :
                    state == RobotState::DISARMED ? "Desarmado" : "Esperando / failsafe");
    }
    sendRobotStatus(message, state, generation, now);
  }
  delay(1);
}
