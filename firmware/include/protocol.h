#pragma once

#include <Arduino.h>

namespace lora_app {

enum MessageType : uint8_t {
  MSG_TELEMETRY = 0x01,
  MSG_DISTANCE = 0x04,
  MSG_STATUS = 0x02,
  MSG_ALARM = 0x03,
  MSG_DOWNLINK_CMD = 0x10,
  MSG_ACK = 0x11,
};

enum DownlinkCommand : uint8_t {
  CMD_PING = 0x01,
  CMD_SET_INTERVAL_SEC = 0x02,
  CMD_REBOOT = 0x03,
  CMD_ENTER_OTA = 0x04,
};

enum AckStatus : uint8_t {
  ACK_OK = 0x00,
  ACK_UNSUPPORTED_CMD = 0x01,
  ACK_INVALID_VALUE = 0x02,
};

#pragma pack(push, 1)
struct TelemetryPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t frame_counter;
  uint32_t unix_time;
  uint16_t battery_mV;
  int16_t temp_c_x100;
  uint16_t rh_x100;
  uint16_t pressure_pa_div10;
  uint8_t flags;
  uint16_t crc16;
};

struct DistancePacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t frame_counter;
  uint32_t unix_time;
  uint16_t battery_mV;
  uint16_t distance_mm;
  uint8_t flags;
  uint16_t crc16;
};

struct DownlinkPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t target_frame_counter;
  uint8_t cmd;
  uint8_t reserved;
  uint32_t value_u32;
  uint16_t crc16;
};

struct AckPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t acked_frame_counter;
  uint8_t acked_cmd;
  uint8_t status;
  uint32_t current_interval_sec;
  uint16_t crc16;
};
#pragma pack(pop)

inline uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

template <typename T>
inline bool validate_crc_typed(const T &packet) {
  const auto *raw = reinterpret_cast<const uint8_t *>(&packet);
  const uint16_t *crcField = reinterpret_cast<const uint16_t *>(raw + sizeof(T) - sizeof(uint16_t));
  const uint16_t computed = crc16_ccitt(raw, sizeof(T) - sizeof(uint16_t));
  return computed == *crcField;
}

template <typename T>
inline void finalize_crc_typed(T &packet) {
  auto *raw = reinterpret_cast<uint8_t *>(&packet);
  auto *crcField = reinterpret_cast<uint16_t *>(raw + sizeof(T) - sizeof(uint16_t));
  *crcField = crc16_ccitt(raw, sizeof(T) - sizeof(uint16_t));
}

inline bool validate_packet_crc(const TelemetryPacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline bool validate_packet_crc(const DistancePacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline bool validate_packet_crc(const DownlinkPacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline bool validate_packet_crc(const AckPacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline void finalize_packet_crc(TelemetryPacketV1 &packet) {
  finalize_crc_typed(packet);
}

inline void finalize_packet_crc(DistancePacketV1 &packet) {
  finalize_crc_typed(packet);
}

inline void finalize_packet_crc(DownlinkPacketV1 &packet) {
  finalize_crc_typed(packet);
}

inline void finalize_packet_crc(AckPacketV1 &packet) {
  finalize_crc_typed(packet);
}

}  // namespace lora_app
