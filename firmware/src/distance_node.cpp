#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <time.h>

#include "lora_config.h"
#include "battery_adc.h"
#include "protocol.h"
#include "secure_transport.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);
Preferences prefs;

RTC_DATA_ATTR static uint32_t frameCounter = 0;
RTC_DATA_ATTR static uint32_t txIntervalSec = DISTANCE_TX_INTERVAL_SEC;
RTC_DATA_ATTR static uint32_t tankAreaM2x1000 = static_cast<uint32_t>(TANK_AREA_M2 * 1000.0f + 0.5f);
RTC_DATA_ATTR static uint16_t tankDistanceMinMm = static_cast<uint16_t>(TANK_DISTANCE_MIN_MM);
RTC_DATA_ATTR static uint16_t tankDistanceMaxMm = static_cast<uint16_t>(TANK_DISTANCE_MAX_MM);
RTC_DATA_ATTR static uint16_t shortAddr = 0;
volatile bool rxWindowFlag = false;

struct PendingControlPacket {
  bool isAttrCommand;
  DownlinkPacketV1 downlink;
  AttrCommandPacketV1 attr;
};

struct DistanceSnapshot {
  uint16_t distanceMm;
  uint16_t levelMm;
  uint32_t waterLitersX10;
  bool measureOk;
};

static DistanceSnapshot lastSnapshot{};

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
void setRxWindowFlag(void) {
  rxWindowFlag = true;
}

static uint16_t clampU16(uint16_t value, uint16_t minValue, uint16_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

static bool configLooksValid(uint32_t areaX1000, uint16_t minMm, uint16_t maxMm) {
  if (areaX1000 == 0) {
    return false;
  }
  if (minMm >= maxMm) {
    return false;
  }
  return true;
}

static void loadRuntimeConfig() {
  prefs.begin("distance", true);

  const uint32_t loadedFrameCounter = prefs.getULong("fcnt", frameCounter);
  const uint32_t loadedInterval = prefs.getUInt("tx_i", txIntervalSec);
  const uint32_t loadedAreaX1000 = prefs.getUInt("area_x1k", tankAreaM2x1000);
  const uint16_t loadedMinMm = static_cast<uint16_t>(prefs.getUInt("dmin", tankDistanceMinMm));
  const uint16_t loadedMaxMm = static_cast<uint16_t>(prefs.getUInt("dmax", tankDistanceMaxMm));
  const uint16_t loadedShortAddr = prefs.getUShort("saddr", shortAddr);

  prefs.end();

  frameCounter = loadedFrameCounter;

  if (loadedInterval >= APP_TX_INTERVAL_MIN_SEC && loadedInterval <= APP_TX_INTERVAL_MAX_SEC) {
    txIntervalSec = loadedInterval;
  }

  if (configLooksValid(loadedAreaX1000, loadedMinMm, loadedMaxMm)) {
    tankAreaM2x1000 = loadedAreaX1000;
    tankDistanceMinMm = loadedMinMm;
    tankDistanceMaxMm = loadedMaxMm;
  }

  shortAddr = loadedShortAddr;
}

static void saveFrameCounter() {
  prefs.begin("distance", false);
  prefs.putULong("fcnt", frameCounter);
  prefs.end();
}

static void saveRuntimeConfig() {
  prefs.begin("distance", false);
  prefs.putUInt("tx_i", txIntervalSec);
  prefs.putUInt("area_x1k", tankAreaM2x1000);
  prefs.putUInt("dmin", tankDistanceMinMm);
  prefs.putUInt("dmax", tankDistanceMaxMm);
  prefs.end();
}

static void saveShortAddress() {
  prefs.begin("distance", false);
  prefs.putUShort("saddr", shortAddr);
  prefs.end();
}

static uint32_t unixTimeNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

static uint16_t readBatteryMilliVolts() {
  return read_battery_millivolts(DISTANCE_BATTERY_ADC_PIN, DISTANCE_BATTERY_DIVIDER_RATIO, BATTERY_ADC_SAMPLES);
}

static uint32_t distanceMetricBitmap() {
  return (1UL << METRIC_BATTERY_MV) | (1UL << METRIC_DISTANCE_MM) | (1UL << METRIC_LEVEL_MM) | (1UL << METRIC_WATER_L_X10);
}

static bool sendJoinRequest() {
  JoinRequestV1 request{};
  request.proto_ver = 1;
  request.msg_type = MSG_JOIN_REQUEST;
  request.node_id = DISTANCE_NODE_ID;
  request.device_type = DEVICE_TYPE_DISTANCE;
  request.capabilities = CAP_TELEMETRY | CAP_DOWNLINK | CAP_BATTERY_ADC;
  request.fw_version = 1;
  finalize_crc_typed(request);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&request), sizeof(request), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Join request encrypt failed");
    return false;
  }

  const int state = radio.transmit(encrypted, encryptedLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Join request TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf("Join request sent node=%lu\n", static_cast<unsigned long>(DISTANCE_NODE_ID));
  return true;
}

static bool waitForJoinResponse() {
  rxWindowFlag = false;
  radio.setDio1Action(setRxWindowFlag);
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Join RX window start failed, state=%d\n", state);
    return false;
  }

  const uint32_t deadline = millis() + DOWNLINK_RX_WINDOW_MS;
  while (millis() < deadline) {
    if (!rxWindowFlag) {
      delay(10);
      continue;
    }

    rxWindowFlag = false;
    const size_t packetLen = radio.getPacketLength();
    if (packetLen == 0 || packetLen > SECURE_MAX_FRAME_SIZE) {
      radio.startReceive();
      continue;
    }

    uint8_t bytes[SECURE_MAX_FRAME_SIZE]{};
    state = radio.readData(bytes, packetLen);
    if (state != RADIOLIB_ERR_NONE) {
      radio.startReceive();
      continue;
    }

    uint8_t plain[sizeof(JoinResponseV1)]{};
    size_t plainLen = 0;
    if (!secure_decrypt_frame(bytes, packetLen, plain, sizeof(plain), plainLen) || plainLen != sizeof(JoinResponseV1)) {
      radio.startReceive();
      continue;
    }

    JoinResponseV1 response{};
    memcpy(&response, plain, sizeof(response));
    if (response.proto_ver != 1 || response.msg_type != MSG_JOIN_RESPONSE || response.node_id != DISTANCE_NODE_ID) {
      radio.startReceive();
      continue;
    }

    if (!validate_crc_typed(response)) {
      Serial.println("Join response CRC invalid");
      radio.startReceive();
      continue;
    }

    if (response.status != JOIN_OK || response.short_addr == 0 || response.short_addr == 0xFFFF) {
      Serial.printf("Join rejected status=%u\n", response.status);
      return false;
    }

    shortAddr = response.short_addr;
    saveShortAddress();
    Serial.printf("Join accepted short_addr=%u network=%u\n", shortAddr, response.network_id);
    return true;
  }

  Serial.println("Join response timeout");
  return false;
}

static bool sendInterviewReport() {
  InterviewReportV1 report{};
  report.proto_ver = 1;
  report.msg_type = MSG_INTERVIEW_REPORT;
  report.node_id = DISTANCE_NODE_ID;
  report.short_addr = shortAddr;
  report.device_type = DEVICE_TYPE_DISTANCE;
  report.metric_bitmap = distanceMetricBitmap();
  report.current_interval_sec = txIntervalSec;
  finalize_crc_typed(report);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&report), sizeof(report), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Interview encrypt failed");
    return false;
  }

  const int state = radio.transmit(encrypted, encryptedLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Interview TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf("Interview sent short_addr=%u\n", shortAddr);
  return true;
}

static bool readDistanceAttribute(uint16_t clusterId, uint16_t attributeId, int32_t &valueOut) {
  if (clusterId == CLUSTER_SYSTEM && attributeId == ATTR_TX_INTERVAL_SEC) {
    valueOut = static_cast<int32_t>(txIntervalSec);
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_AREA_M2_X1000) {
    valueOut = static_cast<int32_t>(tankAreaM2x1000);
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_DISTANCE_MIN_MM) {
    valueOut = static_cast<int32_t>(tankDistanceMinMm);
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_DISTANCE_MAX_MM) {
    valueOut = static_cast<int32_t>(tankDistanceMaxMm);
    return true;
  }
  if (clusterId == CLUSTER_POWER && attributeId == ATTR_BATTERY_MV) {
    valueOut = static_cast<int32_t>(readBatteryMilliVolts());
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_DISTANCE_MM) {
    valueOut = static_cast<int32_t>(lastSnapshot.distanceMm);
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_LEVEL_MM) {
    valueOut = static_cast<int32_t>(lastSnapshot.levelMm);
    return true;
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_WATER_L_X10) {
    valueOut = static_cast<int32_t>(lastSnapshot.waterLitersX10);
    return true;
  }
  return false;
}

static bool sendAttributeReport(uint16_t clusterId, uint16_t attributeId, int32_t value, uint8_t flags) {
  AttrReportPacketV1 report{};
  report.proto_ver = 1;
  report.msg_type = MSG_ATTR_REPORT;
  report.node_id = DISTANCE_NODE_ID;
  report.short_addr = shortAddr;
  report.cluster_id = clusterId;
  report.attribute_id = attributeId;
  report.value_i32 = value;
  report.flags = flags;
  finalize_crc_typed(report);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&report), sizeof(report), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Attr report encrypt failed");
    return false;
  }

  radio.standby();
  const int state = radio.transmit(encrypted, encryptedLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Attr report TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf("Attr report sent cluster=0x%04X attr=0x%04X value=%ld\n",
                clusterId,
                attributeId,
                static_cast<long>(value));
  return true;
}

static void sensorPowerOn() {
  if (JSN_POWER_PIN >= 0) {
    pinMode(JSN_POWER_PIN, OUTPUT);
    digitalWrite(JSN_POWER_PIN, HIGH);
    delay(150);
  }
}

static void sensorPowerOff() {
  if (JSN_POWER_PIN >= 0) {
    digitalWrite(JSN_POWER_PIN, LOW);
  }
}

static bool readDistanceMillimeters(uint16_t &distanceMm) {
  pinMode(JSN_TRIG_PIN, OUTPUT);
  pinMode(JSN_ECHO_PIN, INPUT);

  digitalWrite(JSN_TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(JSN_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(JSN_TRIG_PIN, LOW);

  const uint32_t pulseUs = pulseIn(JSN_ECHO_PIN, HIGH, JSN_PING_TIMEOUT_US);
  if (pulseUs == 0) {
    return false;
  }

  const uint32_t mm = (pulseUs * 343UL) / 2000UL;
  distanceMm = static_cast<uint16_t>(mm > 65535 ? 65535 : mm);
  return true;
}

static bool sendDistancePacket(uint16_t distanceMm, uint16_t levelMm, uint32_t waterLitersX10, bool measureOk,
                               uint32_t &sentFrameCounter) {
  constexpr uint8_t metricCount = 4;
  uint8_t packet[metrics_packet_total_size(metricCount)]{};
  const uint16_t batteryMv = readBatteryMilliVolts();

  auto *header = reinterpret_cast<MetricsPacketHeaderV1 *>(packet);
  header->proto_ver = 1;
  header->msg_type = MSG_TELEMETRY;
  header->node_id = DISTANCE_NODE_ID;
  header->frame_counter = frameCounter++;
  header->unix_time = unixTimeNow();
  header->metric_count = metricCount;
  header->flags = 0x00;
  if (!measureOk) {
    header->flags |= FLAG_MEASUREMENT_FAIL;
  }
  if (batteryMv > 0 && batteryMv < DISTANCE_LOW_BATTERY_MV) {
    header->flags |= FLAG_LOW_BATTERY;
  }

  auto *records = metrics_packet_records(header);
  records[0] = MetricRecordV1{METRIC_BATTERY_MV, batteryMv};
  records[1] = MetricRecordV1{METRIC_DISTANCE_MM, distanceMm};
  records[2] = MetricRecordV1{METRIC_LEVEL_MM, levelMm};
  records[3] = MetricRecordV1{METRIC_WATER_L_X10, static_cast<int32_t>(waterLitersX10)};

  finalize_metrics_packet(packet, sizeof(packet));

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(packet, sizeof(packet), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Distance TX encrypt failed");
    return false;
  }

  int state = radio.transmit(encrypted, encryptedLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Distance TX failed, state=%d\n", state);
    return false;
  }

  sentFrameCounter = header->frame_counter;
  saveFrameCounter();

  Serial.printf("Distance TX ok node=%lu fcnt=%lu distance=%u mm level=%u mm water=%.1f L flags=%u\n",
                static_cast<unsigned long>(header->node_id),
                static_cast<unsigned long>(header->frame_counter),
                distanceMm,
                levelMm,
                waterLitersX10 / 10.0f,
                header->flags);
  return true;
}

static bool waitForControlPacket(PendingControlPacket &control) {
  control.isAttrCommand = false;
  memset(&control.downlink, 0, sizeof(control.downlink));
  memset(&control.attr, 0, sizeof(control.attr));

  rxWindowFlag = false;
  radio.setDio1Action(setRxWindowFlag);
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("RX window start failed, state=%d\n", state);
    return false;
  }

  const uint32_t deadline = millis() + DOWNLINK_RX_WINDOW_MS;
  while (millis() < deadline) {
    if (!rxWindowFlag) {
      delay(10);
      continue;
    }

    rxWindowFlag = false;
    size_t packetLen = radio.getPacketLength();
    if (packetLen == 0 || packetLen > SECURE_MAX_FRAME_SIZE) {
      radio.startReceive();
      continue;
    }

    uint8_t bytes[SECURE_MAX_FRAME_SIZE]{};
    state = radio.readData(bytes, packetLen);
    if (state != RADIOLIB_ERR_NONE) {
      radio.startReceive();
      continue;
    }

    uint8_t plain[64]{};
    size_t plainLen = 0;
    if (!secure_decrypt_frame(bytes, packetLen, plain, sizeof(plain), plainLen) || plainLen < 2) {
      radio.startReceive();
      continue;
    }

    if (plain[0] == 1 && plain[1] == MSG_DOWNLINK_CMD && plainLen == sizeof(DownlinkPacketV1)) {
      memcpy(&control.downlink, plain, sizeof(control.downlink));
      if (control.downlink.node_id != DISTANCE_NODE_ID || !validate_packet_crc(control.downlink)) {
        radio.startReceive();
        continue;
      }
      control.isAttrCommand = false;
      Serial.printf("Downlink op=%u param=%u value=%ld\n",
                    control.downlink.operation,
                    control.downlink.parameter_id,
                    static_cast<long>(control.downlink.value_i32));
      return true;
    }

    if (plain[0] == 1 && plain[1] == MSG_ATTR_COMMAND && plainLen == sizeof(AttrCommandPacketV1)) {
      memcpy(&control.attr, plain, sizeof(control.attr));
      if (control.attr.node_id != DISTANCE_NODE_ID || !validate_crc_typed(control.attr)) {
        radio.startReceive();
        continue;
      }
      control.isAttrCommand = true;
      Serial.printf("Attr cmd type=%u cluster=0x%04X attr=0x%04X value=%ld\n",
                    control.attr.command_type,
                    control.attr.cluster_id,
                    control.attr.attribute_id,
                    static_cast<long>(control.attr.value_i32));
      return true;
    }

    radio.startReceive();
  }

  return false;
}

static uint8_t applyDownlink(const DownlinkPacketV1 &downlink) {
  switch (downlink.operation) {
    case DL_OP_PING:
      return ACK_OK;
    case DL_OP_SET_PARAM:
      switch (downlink.parameter_id) {
        case PARAM_TX_INTERVAL_SEC:
          if (!parameter_value_is_valid(static_cast<ParameterId>(downlink.parameter_id), downlink.value_i32)) {
            return ACK_INVALID_VALUE;
          }
          txIntervalSec = static_cast<uint32_t>(downlink.value_i32);
          saveRuntimeConfig();
          return ACK_OK;
        case PARAM_TANK_AREA_M2_X1000:
          if (!parameter_value_is_valid(static_cast<ParameterId>(downlink.parameter_id), downlink.value_i32)) {
            return ACK_INVALID_VALUE;
          }
          if (!configLooksValid(static_cast<uint32_t>(downlink.value_i32), tankDistanceMinMm, tankDistanceMaxMm)) {
            return ACK_INVALID_VALUE;
          }
          tankAreaM2x1000 = static_cast<uint32_t>(downlink.value_i32);
          saveRuntimeConfig();
          return ACK_OK;
        case PARAM_TANK_DISTANCE_MIN_MM: {
          if (!parameter_value_is_valid(static_cast<ParameterId>(downlink.parameter_id), downlink.value_i32)) {
            return ACK_INVALID_VALUE;
          }
          const uint16_t newMin = static_cast<uint16_t>(downlink.value_i32 > 65535 ? 65535 : downlink.value_i32);
          if (!configLooksValid(tankAreaM2x1000, newMin, tankDistanceMaxMm)) {
            return ACK_INVALID_VALUE;
          }
          tankDistanceMinMm = newMin;
          saveRuntimeConfig();
          return ACK_OK;
        }
        case PARAM_TANK_DISTANCE_MAX_MM: {
          if (!parameter_value_is_valid(static_cast<ParameterId>(downlink.parameter_id), downlink.value_i32)) {
            return ACK_INVALID_VALUE;
          }
          const uint16_t newMax = static_cast<uint16_t>(downlink.value_i32 > 65535 ? 65535 : downlink.value_i32);
          if (!configLooksValid(tankAreaM2x1000, tankDistanceMinMm, newMax)) {
            return ACK_INVALID_VALUE;
          }
          tankDistanceMaxMm = newMax;
          saveRuntimeConfig();
          return ACK_OK;
        }
        default:
          return ACK_UNSUPPORTED_CMD;
      }
    case DL_OP_REBOOT:
      return ACK_OK;
    case DL_OP_ENTER_OTA:
      return ACK_OK;
    default:
      return ACK_UNSUPPORTED_CMD;
  }
}

static uint8_t applyAttrCommand(const AttrCommandPacketV1 &attr) {
  if (attr.command_type == ATTR_CMD_READ) {
    int32_t value = 0;
    if (!readDistanceAttribute(attr.cluster_id, attr.attribute_id, value)) {
      return ACK_UNSUPPORTED_CMD;
    }
    uint8_t flags = 0;
    if (!lastSnapshot.measureOk) {
      flags |= FLAG_MEASUREMENT_FAIL;
    }
    sendAttributeReport(attr.cluster_id, attr.attribute_id, value, flags);
    return ACK_OK;
  }

  if (attr.command_type == ATTR_CMD_WRITE) {
    if (attr.cluster_id == CLUSTER_SYSTEM && attr.attribute_id == ATTR_TX_INTERVAL_SEC) {
      if (!parameter_value_is_valid(PARAM_TX_INTERVAL_SEC, attr.value_i32)) {
        return ACK_INVALID_VALUE;
      }
      txIntervalSec = static_cast<uint32_t>(attr.value_i32);
      saveRuntimeConfig();
      sendAttributeReport(attr.cluster_id, attr.attribute_id, attr.value_i32, 0);
      return ACK_OK;
    }
    if (attr.cluster_id == CLUSTER_WATER_TANK && attr.attribute_id == ATTR_TANK_AREA_M2_X1000) {
      if (!parameter_value_is_valid(PARAM_TANK_AREA_M2_X1000, attr.value_i32)) {
        return ACK_INVALID_VALUE;
      }
      if (!configLooksValid(static_cast<uint32_t>(attr.value_i32), tankDistanceMinMm, tankDistanceMaxMm)) {
        return ACK_INVALID_VALUE;
      }
      tankAreaM2x1000 = static_cast<uint32_t>(attr.value_i32);
      saveRuntimeConfig();
      sendAttributeReport(attr.cluster_id, attr.attribute_id, attr.value_i32, 0);
      return ACK_OK;
    }
    if (attr.cluster_id == CLUSTER_WATER_TANK && attr.attribute_id == ATTR_TANK_DISTANCE_MIN_MM) {
      if (!parameter_value_is_valid(PARAM_TANK_DISTANCE_MIN_MM, attr.value_i32)) {
        return ACK_INVALID_VALUE;
      }
      const uint16_t newMin = static_cast<uint16_t>(attr.value_i32 > 65535 ? 65535 : attr.value_i32);
      if (!configLooksValid(tankAreaM2x1000, newMin, tankDistanceMaxMm)) {
        return ACK_INVALID_VALUE;
      }
      tankDistanceMinMm = newMin;
      saveRuntimeConfig();
      sendAttributeReport(attr.cluster_id, attr.attribute_id, attr.value_i32, 0);
      return ACK_OK;
    }
    if (attr.cluster_id == CLUSTER_WATER_TANK && attr.attribute_id == ATTR_TANK_DISTANCE_MAX_MM) {
      if (!parameter_value_is_valid(PARAM_TANK_DISTANCE_MAX_MM, attr.value_i32)) {
        return ACK_INVALID_VALUE;
      }
      const uint16_t newMax = static_cast<uint16_t>(attr.value_i32 > 65535 ? 65535 : attr.value_i32);
      if (!configLooksValid(tankAreaM2x1000, tankDistanceMinMm, newMax)) {
        return ACK_INVALID_VALUE;
      }
      tankDistanceMaxMm = newMax;
      saveRuntimeConfig();
      sendAttributeReport(attr.cluster_id, attr.attribute_id, attr.value_i32, 0);
      return ACK_OK;
    }
    return ACK_UNSUPPORTED_CMD;
  }

  return ACK_UNSUPPORTED_CMD;
}

static void sendAck(const DownlinkPacketV1 &downlink, uint32_t ackedFrameCounter, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = DISTANCE_NODE_ID;
  ack.acked_frame_counter = ackedFrameCounter;
  ack.acked_operation = downlink.operation;
  ack.acked_parameter_id = downlink.parameter_id;
  ack.status = status;
  ack.current_interval_sec = txIntervalSec;
  finalize_packet_crc(ack);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&ack), sizeof(ack), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("ACK encrypt failed");
    return;
  }

  radio.standby();
  int state = radio.transmit(encrypted, encryptedLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("ACK sent op=%u param=%u status=%u interval=%lu\n",
                  ack.acked_operation,
                  ack.acked_parameter_id,
                  ack.status,
                  static_cast<unsigned long>(ack.current_interval_sec));
  } else {
    Serial.printf("ACK TX failed, state=%d\n", state);
  }

  if (downlink.operation == DL_OP_REBOOT && status == ACK_OK) {
    delay(200);
    ESP.restart();
  }
}

static void sendAttrAck(const AttrCommandPacketV1 &attr, uint32_t ackedFrameCounter, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = DISTANCE_NODE_ID;
  ack.acked_frame_counter = ackedFrameCounter;
  ack.acked_operation = attr.command_type;
  ack.acked_parameter_id = attr.attribute_id;
  ack.status = status;
  ack.current_interval_sec = txIntervalSec;
  finalize_packet_crc(ack);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&ack), sizeof(ack), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Attr ACK encrypt failed");
    return;
  }

  radio.standby();
  int state = radio.transmit(encrypted, encryptedLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Attr ACK sent type=%u attr=0x%04X status=%u interval=%lu\n",
                  attr.command_type,
                  attr.attribute_id,
                  ack.status,
                  static_cast<unsigned long>(ack.current_interval_sec));
  } else {
    Serial.printf("Attr ACK TX failed, state=%d\n", state);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  loadRuntimeConfig();

  Serial.printf("Distance cfg: area=%.3f m2 min=%u mm max=%u mm interval=%lu s\n",
                tankAreaM2x1000 / 1000.0f,
                tankDistanceMinMm,
                tankDistanceMaxMm,
                static_cast<unsigned long>(txIntervalSec));

  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);

  int state = radio.begin(
      LORA_FREQUENCY_MHZ,
      LORA_BANDWIDTH_KHZ,
      LORA_SPREADING_FACTOR,
      LORA_CODING_RATE,
      LORA_SYNC_WORD,
      LORA_TX_POWER_DBM,
      LORA_PREAMBLE_LEN,
      0);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio init failed, state=%d\n", state);
    delay(5000);
    ESP.restart();
  }

  radio.setCurrentLimit(80);

  if (shortAddr == 0 || shortAddr == 0xFFFF) {
    if (sendJoinRequest()) {
      if (waitForJoinResponse()) {
        sendInterviewReport();
      }
    }
  }

  sensorPowerOn();
  uint16_t distanceMm = 0;
  bool measureOk = false;

  for (int i = 0; i < 3; i++) {
    uint16_t sample = 0;
    if (readDistanceMillimeters(sample)) {
      if (!measureOk) {
        distanceMm = sample;
      } else {
        distanceMm = static_cast<uint16_t>((static_cast<uint32_t>(distanceMm) + sample) / 2UL);
      }
      measureOk = true;
    }
    delay(60);
  }

  sensorPowerOff();

  if (!measureOk) {
    distanceMm = 0;
  }

  const uint16_t minDistanceMm = tankDistanceMinMm;
  const uint16_t maxDistanceMm = tankDistanceMaxMm;
  const uint16_t clampedDistanceMm = measureOk ? clampU16(distanceMm, minDistanceMm, maxDistanceMm) : 0;

  uint16_t levelMm = 0;
  if (measureOk && maxDistanceMm > minDistanceMm) {
    const uint32_t rawLevel = (clampedDistanceMm > maxDistanceMm) ? 0 : (maxDistanceMm - clampedDistanceMm);
    const uint32_t maxLevel = maxDistanceMm - minDistanceMm;
    levelMm = static_cast<uint16_t>(rawLevel > maxLevel ? maxLevel : rawLevel);
  }

  uint32_t waterLitersX10 = 0;
  if (measureOk) {
    const float areaM2 = tankAreaM2x1000 / 1000.0f;
    const float liters = areaM2 * (levelMm / 1000.0f) * 1000.0f;
    const float litersX10 = liters * 10.0f;
    waterLitersX10 = litersX10 <= 0.0f ? 0 : static_cast<uint32_t>(litersX10 + 0.5f);
  }

  lastSnapshot.distanceMm = clampedDistanceMm;
  lastSnapshot.levelMm = levelMm;
  lastSnapshot.waterLitersX10 = waterLitersX10;
  lastSnapshot.measureOk = measureOk;

  uint32_t ackedFrameCounter = 0;
  const bool txOk = sendDistancePacket(clampedDistanceMm, levelMm, waterLitersX10, measureOk, ackedFrameCounter);

  if (txOk) {
    PendingControlPacket control{};
    if (waitForControlPacket(control)) {
      if (control.isAttrCommand) {
        const uint8_t status = applyAttrCommand(control.attr);
        sendAttrAck(control.attr, ackedFrameCounter, status);
      } else {
        const uint8_t status = applyDownlink(control.downlink);
        sendAck(control.downlink, ackedFrameCounter, status);
      }
    }
  }

  Serial.printf("Deep sleep for %lu sec\n", static_cast<unsigned long>(txIntervalSec));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(txIntervalSec) * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
