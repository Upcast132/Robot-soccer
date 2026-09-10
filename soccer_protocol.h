#pragma once

#include <stddef.h>
#include <stdint.h>

// "SOCC" in ASCII. It lets the receiver discard unrelated ESP-NOW traffic.
// Explicit message identifiers in the existing magic field preserve commands.
constexpr uint32_t SOCCER_COMMAND_MAGIC = 0x534F4343UL;  // SOCC
constexpr uint32_t SOCCER_STATUS_MAGIC = 0x534F4353UL;   // SOCS
constexpr uint8_t SOCCER_PROTOCOL_VERSION = 2;
constexpr uint16_t SOCCER_FLAG_JOYSTICK_BUTTON = 1U << 0;
constexpr uint16_t SOCCER_KNOWN_FLAGS = SOCCER_FLAG_JOYSTICK_BUTTON;

enum class RobotState : uint8_t { WAITING = 0, DISARMED = 1, ARMED = 2 };

// Keep the payload packed and use fixed-width types so both ESP32 devices
// interpret exactly the same bytes.
struct __attribute__((packed)) RemoteMsg {
  uint32_t magic;
  uint8_t version;
  uint8_t pairId;
  uint16_t flags;
  uint32_t seq;
  int16_t throttle;  // -1000 (reverse) to +1000 (forward)
  int16_t steering;  // -1000 (left) to +1000 (right)
  uint16_t crc16;
};

static_assert(sizeof(RemoteMsg) == 18, "RemoteMsg wire format changed");

struct __attribute__((packed)) StatusMsg {
  uint32_t magic;
  uint8_t version;
  uint8_t pairId;
  RobotState state;
  uint8_t reserved;  // Must be zero in v2.
  uint32_t acceptedSeq;
  uint16_t crc16;
};

static_assert(sizeof(StatusMsg) == 14, "StatusMsg wire format changed");

inline bool soccerIsNewerSequence(uint32_t candidate, uint32_t previous) {
  const uint32_t distance = candidate - previous;
  return distance != 0 && distance < 0x80000000UL;
}

inline uint16_t soccerCrc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                            : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

inline uint16_t soccerMessageCrc(const RemoteMsg &message) {
  return soccerCrc16(reinterpret_cast<const uint8_t *>(&message),
                     offsetof(RemoteMsg, crc16));
}

inline uint16_t soccerStatusCrc(const StatusMsg &message) {
  return soccerCrc16(reinterpret_cast<const uint8_t *>(&message),
                     offsetof(StatusMsg, crc16));
}
