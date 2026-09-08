#pragma once

#include <stddef.h>
#include <stdint.h>

// "SOCC" in ASCII. It lets the receiver discard unrelated ESP-NOW traffic.
constexpr uint32_t SOCCER_PROTOCOL_MAGIC = 0x534F4343UL;
constexpr uint8_t SOCCER_PROTOCOL_VERSION = 1;
constexpr uint16_t SOCCER_FLAG_JOYSTICK_BUTTON = 1U << 0;

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
