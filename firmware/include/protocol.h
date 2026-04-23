#pragma once

#include <Arduino.h>

namespace lora_app {

enum MessageType : uint8_t {
  MSG_TELEMETRY = 0x01,
  MSG_STATUS = 0x02,
  MSG_ALARM = 0x03,
  MSG_DISTANCE = 0x04,
  MSG_DOWNLINK_CMD = 0x10,
  MSG_ACK = 0x11,
  MSG_JOIN_REQUEST = 0x20,
  MSG_JOIN_RESPONSE = 0x21,
  MSG_INTERVIEW_REPORT = 0x22,
  MSG_ATTR_COMMAND = 0x30,
  MSG_ATTR_REPORT = 0x31,
};

enum DeviceType : uint8_t {
  DEVICE_TYPE_SENSOR = 0x01,
  DEVICE_TYPE_DISTANCE = 0x02,
};

enum JoinCapability : uint32_t {
  CAP_TELEMETRY = 0x00000001,
  CAP_DOWNLINK = 0x00000002,
  CAP_OTA = 0x00000004,
  CAP_BATTERY_ADC = 0x00000008,
};

enum JoinStatus : uint8_t {
  JOIN_OK = 0x00,
  JOIN_DENIED = 0x01,
};

enum AttrCommandType : uint8_t {
  ATTR_CMD_READ = 0x01,
  ATTR_CMD_WRITE = 0x02,
};

enum ClusterId : uint16_t {
  CLUSTER_POWER = 0x0001,
  CLUSTER_ENVIRONMENT = 0x0400,
  CLUSTER_WATER_TANK = 0x0500,
  CLUSTER_SYSTEM = 0xF000,
};

enum AttributeId : uint16_t {
  ATTR_BATTERY_MV = 0x0001,
  ATTR_TEMPERATURE_C_X100 = 0x0002,
  ATTR_HUMIDITY_RH_X100 = 0x0003,
  ATTR_PRESSURE_HPA_X10 = 0x0004,

  ATTR_DISTANCE_MM = 0x0010,
  ATTR_LEVEL_MM = 0x0011,
  ATTR_WATER_L_X10 = 0x0012,

  ATTR_TX_INTERVAL_SEC = 0x1001,
  ATTR_TANK_AREA_M2_X1000 = 0x1101,
  ATTR_TANK_DISTANCE_MIN_MM = 0x1102,
  ATTR_TANK_DISTANCE_MAX_MM = 0x1103,
};

enum MetricId : uint8_t {
  METRIC_BATTERY_MV = 0x01,
  METRIC_TEMPERATURE_C_X100 = 0x02,
  METRIC_HUMIDITY_RH_X100 = 0x03,
  METRIC_PRESSURE_HPA_X10 = 0x04,
  METRIC_DISTANCE_MM = 0x10,
  METRIC_LEVEL_MM = 0x11,
  METRIC_WATER_L_X10 = 0x12,
};

enum DownlinkOperation : uint8_t {
  DL_OP_PING = 0x01,
  DL_OP_SET_PARAM = 0x02,
  DL_OP_REBOOT = 0x03,
  DL_OP_ENTER_OTA = 0x04,
};

enum ParameterId : uint16_t {
  PARAM_NONE = 0x0000,
  PARAM_TX_INTERVAL_SEC = 0x0001,
  PARAM_TANK_AREA_M2_X1000 = 0x0101,
  PARAM_TANK_DISTANCE_MIN_MM = 0x0102,
  PARAM_TANK_DISTANCE_MAX_MM = 0x0103,
};

enum AckStatus : uint8_t {
  ACK_OK = 0x00,
  ACK_UNSUPPORTED_CMD = 0x01,
  ACK_INVALID_VALUE = 0x02,
};

enum PacketFlags : uint8_t {
  FLAG_LOW_BATTERY = 0x01,
  FLAG_MEASUREMENT_FAIL = 0x02,
};

#pragma pack(push, 1)
struct MetricsPacketHeaderV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t frame_counter;
  uint32_t unix_time;
  uint8_t metric_count;
  uint8_t flags;
};

struct MetricRecordV1 {
  uint8_t metric_id;
  int32_t value;
};

struct DownlinkPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t target_frame_counter;
  uint8_t operation;
  uint16_t parameter_id;
  int32_t value_i32;
  uint16_t crc16;
};

struct AckPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t acked_frame_counter;
  uint8_t acked_operation;
  uint16_t acked_parameter_id;
  uint8_t status;
  uint32_t current_interval_sec;
  uint16_t crc16;
};

struct JoinRequestV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint8_t device_type;
  uint32_t capabilities;
  uint16_t fw_version;
  uint16_t crc16;
};

struct JoinResponseV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint16_t short_addr;
  uint16_t network_id;
  uint8_t status;
  uint16_t crc16;
};

struct InterviewReportV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint16_t short_addr;
  uint8_t device_type;
  uint32_t metric_bitmap;
  uint32_t current_interval_sec;
  uint16_t crc16;
};

struct AttrCommandPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint32_t target_frame_counter;
  uint8_t command_type;
  uint16_t cluster_id;
  uint16_t attribute_id;
  int32_t value_i32;
  uint16_t crc16;
};

struct AttrReportPacketV1 {
  uint8_t proto_ver;
  uint8_t msg_type;
  uint32_t node_id;
  uint16_t short_addr;
  uint16_t cluster_id;
  uint16_t attribute_id;
  int32_t value_i32;
  uint8_t flags;
  uint16_t crc16;
};
#pragma pack(pop)

struct ParameterSchemaV1 {
  ParameterId id;
  const char *key;
  int32_t min_value;
  int32_t max_value;
  float scale;
};

constexpr ParameterSchemaV1 PARAMETER_SCHEMAS_V1[] = {
    {PARAM_TX_INTERVAL_SEC, "tx_interval_sec", 30, 86400, 1.0f},
    {PARAM_TANK_AREA_M2_X1000, "tank_area_m2", 1, 200000, 1000.0f},
    {PARAM_TANK_DISTANCE_MIN_MM, "tank_distance_min_mm", 1, 65535, 1.0f},
    {PARAM_TANK_DISTANCE_MAX_MM, "tank_distance_max_mm", 1, 65535, 1.0f},
};

constexpr size_t PARAMETER_SCHEMA_COUNT_V1 = sizeof(PARAMETER_SCHEMAS_V1) / sizeof(PARAMETER_SCHEMAS_V1[0]);

constexpr uint8_t METRICS_PACKET_MAX_RECORDS = 8;
constexpr size_t METRICS_PACKET_MIN_SIZE = sizeof(MetricsPacketHeaderV1) + sizeof(uint16_t);

constexpr size_t metrics_packet_total_size(uint8_t metricCount) {
  return sizeof(MetricsPacketHeaderV1) + (static_cast<size_t>(metricCount) * sizeof(MetricRecordV1)) + sizeof(uint16_t);
}

inline MetricRecordV1 *metrics_packet_records(MetricsPacketHeaderV1 *header) {
  return reinterpret_cast<MetricRecordV1 *>(reinterpret_cast<uint8_t *>(header) + sizeof(MetricsPacketHeaderV1));
}

inline const MetricRecordV1 *metrics_packet_records(const MetricsPacketHeaderV1 *header) {
  return reinterpret_cast<const MetricRecordV1 *>(reinterpret_cast<const uint8_t *>(header) + sizeof(MetricsPacketHeaderV1));
}

inline const ParameterSchemaV1 *find_parameter_schema_by_id(ParameterId id) {
  for (size_t i = 0; i < PARAMETER_SCHEMA_COUNT_V1; i++) {
    if (PARAMETER_SCHEMAS_V1[i].id == id) {
      return &PARAMETER_SCHEMAS_V1[i];
    }
  }
  return nullptr;
}

inline const ParameterSchemaV1 *find_parameter_schema_by_key(const char *key) {
  if (key == nullptr) {
    return nullptr;
  }

  for (size_t i = 0; i < PARAMETER_SCHEMA_COUNT_V1; i++) {
    if (strcmp(PARAMETER_SCHEMAS_V1[i].key, key) == 0) {
      return &PARAMETER_SCHEMAS_V1[i];
    }
  }
  return nullptr;
}

inline bool parameter_value_is_valid(ParameterId id, int32_t value) {
  const ParameterSchemaV1 *schema = find_parameter_schema_by_id(id);
  if (schema == nullptr) {
    return false;
  }
  return value >= schema->min_value && value <= schema->max_value;
}

inline bool parameter_value_from_float(ParameterId id, float userValue, int32_t &encodedValue) {
  const ParameterSchemaV1 *schema = find_parameter_schema_by_id(id);
  if (schema == nullptr) {
    return false;
  }

  const float scaled = userValue * schema->scale;
  if (scaled > static_cast<float>(schema->max_value) || scaled < static_cast<float>(schema->min_value)) {
    return false;
  }

  encodedValue = static_cast<int32_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
  return parameter_value_is_valid(id, encodedValue);
}

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

inline bool validate_metrics_packet(const uint8_t *raw, size_t len) {
  if (raw == nullptr || len < METRICS_PACKET_MIN_SIZE) {
    return false;
  }

  const auto *header = reinterpret_cast<const MetricsPacketHeaderV1 *>(raw);
  if (header->metric_count > METRICS_PACKET_MAX_RECORDS) {
    return false;
  }

  if (len != metrics_packet_total_size(header->metric_count)) {
    return false;
  }

  const auto *crcField = reinterpret_cast<const uint16_t *>(raw + len - sizeof(uint16_t));
  const uint16_t computed = crc16_ccitt(raw, len - sizeof(uint16_t));
  return computed == *crcField;
}

inline void finalize_metrics_packet(uint8_t *raw, size_t len) {
  if (raw == nullptr || len < METRICS_PACKET_MIN_SIZE) {
    return;
  }

  auto *crcField = reinterpret_cast<uint16_t *>(raw + len - sizeof(uint16_t));
  *crcField = crc16_ccitt(raw, len - sizeof(uint16_t));
}

inline bool validate_packet_crc(const DownlinkPacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline bool validate_packet_crc(const AckPacketV1 &packet) {
  return validate_crc_typed(packet);
}

inline void finalize_packet_crc(DownlinkPacketV1 &packet) {
  finalize_crc_typed(packet);
}

inline void finalize_packet_crc(AckPacketV1 &packet) {
  finalize_crc_typed(packet);
}

}  // namespace lora_app
