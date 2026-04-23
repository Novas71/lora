#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <time.h>
#include <WiFi.h>

#include "lora_config.h"
#include "battery_adc.h"
#include "protocol.h"
#include "secure_transport.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);
Preferences prefs;

static uint32_t frameCounter = 0;
RTC_DATA_ATTR static uint32_t txIntervalSec = APP_TX_INTERVAL_SEC;
RTC_DATA_ATTR static uint16_t shortAddr = 0;
volatile bool rxWindowFlag = false;

struct PendingControlPacket {
  bool isAttrCommand;
  DownlinkPacketV1 downlink;
  AttrCommandPacketV1 attr;
};

static void loadPersistentState() {
  prefs.begin("sensor", true);
  frameCounter = prefs.getULong("fcnt", 0);
  shortAddr = prefs.getUShort("saddr", 0);
  prefs.end();
}

static void saveFrameCounter() {
  prefs.begin("sensor", false);
  prefs.putULong("fcnt", frameCounter);
  prefs.end();
}

static void saveShortAddress() {
  prefs.begin("sensor", false);
  prefs.putUShort("saddr", shortAddr);
  prefs.end();
}

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
void setRxWindowFlag(void) {
  rxWindowFlag = true;
}

static int16_t readTemperatureCx100() {
  return static_cast<int16_t>(2200 + static_cast<int16_t>(esp_random() % 1400));
}

static uint16_t readHumidityX100() {
  return static_cast<uint16_t>(3500 + (esp_random() % 3500));
}

static uint16_t readPressureDiv10() {
  return static_cast<uint16_t>(9800 + (esp_random() % 700));
}

static uint16_t readBatteryMilliVolts() {
  return read_battery_millivolts(SENSOR_BATTERY_ADC_PIN, SENSOR_BATTERY_DIVIDER_RATIO, BATTERY_ADC_SAMPLES);
}

static uint32_t unixTimeNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

static uint32_t sensorMetricBitmap() {
  return (1UL << METRIC_BATTERY_MV) | (1UL << METRIC_TEMPERATURE_C_X100) | (1UL << METRIC_HUMIDITY_RH_X100) |
         (1UL << METRIC_PRESSURE_HPA_X10);
}

static bool sendJoinRequest() {
  JoinRequestV1 request{};
  request.proto_ver = 1;
  request.msg_type = MSG_JOIN_REQUEST;
  request.node_id = APP_NODE_ID;
  request.device_type = DEVICE_TYPE_SENSOR;
  request.capabilities = CAP_TELEMETRY | CAP_DOWNLINK | CAP_OTA | CAP_BATTERY_ADC;
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

  Serial.printf("Join request sent node=%lu\n", static_cast<unsigned long>(APP_NODE_ID));
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
    if (response.proto_ver != 1 || response.msg_type != MSG_JOIN_RESPONSE || response.node_id != APP_NODE_ID) {
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
  report.node_id = APP_NODE_ID;
  report.short_addr = shortAddr;
  report.device_type = DEVICE_TYPE_SENSOR;
  report.metric_bitmap = sensorMetricBitmap();
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

static bool readSensorAttribute(uint16_t clusterId, uint16_t attributeId, int32_t &valueOut) {
  if (clusterId == CLUSTER_SYSTEM && attributeId == ATTR_TX_INTERVAL_SEC) {
    valueOut = static_cast<int32_t>(txIntervalSec);
    return true;
  }
  if (clusterId == CLUSTER_POWER && attributeId == ATTR_BATTERY_MV) {
    valueOut = static_cast<int32_t>(readBatteryMilliVolts());
    return true;
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_TEMPERATURE_C_X100) {
    valueOut = static_cast<int32_t>(readTemperatureCx100());
    return true;
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_HUMIDITY_RH_X100) {
    valueOut = static_cast<int32_t>(readHumidityX100());
    return true;
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_PRESSURE_HPA_X10) {
    valueOut = static_cast<int32_t>(readPressureDiv10());
    return true;
  }
  return false;
}

static bool sendAttributeReport(uint16_t clusterId, uint16_t attributeId, int32_t value, uint8_t flags) {
  AttrReportPacketV1 report{};
  report.proto_ver = 1;
  report.msg_type = MSG_ATTR_REPORT;
  report.node_id = APP_NODE_ID;
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

static bool sendPacket(uint32_t &sentFrameCounter) {
  constexpr uint8_t metricCount = 4;
  uint8_t packet[metrics_packet_total_size(metricCount)]{};

  const uint16_t batteryMv = readBatteryMilliVolts();
  const int16_t temperatureCx100 = readTemperatureCx100();
  const uint16_t humidityX100 = readHumidityX100();
  const uint16_t pressureHpaX10 = readPressureDiv10();

  auto *header = reinterpret_cast<MetricsPacketHeaderV1 *>(packet);
  header->proto_ver = 1;
  header->msg_type = MSG_TELEMETRY;
  header->node_id = APP_NODE_ID;
  header->frame_counter = frameCounter++;
  header->unix_time = unixTimeNow();
  header->metric_count = metricCount;
  header->flags = (batteryMv > 0 && batteryMv < SENSOR_LOW_BATTERY_MV) ? FLAG_LOW_BATTERY : 0x00;

  auto *records = metrics_packet_records(header);
  records[0] = MetricRecordV1{METRIC_BATTERY_MV, batteryMv};
  records[1] = MetricRecordV1{METRIC_TEMPERATURE_C_X100, temperatureCx100};
  records[2] = MetricRecordV1{METRIC_HUMIDITY_RH_X100, humidityX100};
  records[3] = MetricRecordV1{METRIC_PRESSURE_HPA_X10, pressureHpaX10};

  finalize_metrics_packet(packet, sizeof(packet));

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(packet, sizeof(packet), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("TX encrypt failed");
    return false;
  }

  int state = radio.transmit(encrypted, encryptedLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("TX failed, state=%d\n", state);
    return false;
  }

  sentFrameCounter = header->frame_counter;
  saveFrameCounter();

  Serial.printf(
      "TX ok node=%lu fcnt=%lu bat=%.3fV temp=%.2fC hum=%.2f%% press=%.1fhPa\n",
      static_cast<unsigned long>(header->node_id),
      static_cast<unsigned long>(header->frame_counter),
      batteryMv / 1000.0,
      temperatureCx100 / 100.0,
      humidityX100 / 100.0,
      pressureHpaX10 / 10.0);
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
      if (control.downlink.node_id != APP_NODE_ID || !validate_packet_crc(control.downlink)) {
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
      if (control.attr.node_id != APP_NODE_ID || !validate_crc_typed(control.attr)) {
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
      if (downlink.parameter_id == PARAM_TX_INTERVAL_SEC) {
        if (!parameter_value_is_valid(static_cast<ParameterId>(downlink.parameter_id), downlink.value_i32)) {
          return ACK_INVALID_VALUE;
        }
        txIntervalSec = static_cast<uint32_t>(downlink.value_i32);
        return ACK_OK;
      }
      return ACK_UNSUPPORTED_CMD;
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
    if (!readSensorAttribute(attr.cluster_id, attr.attribute_id, value)) {
      return ACK_UNSUPPORTED_CMD;
    }
    sendAttributeReport(attr.cluster_id, attr.attribute_id, value, 0);
    return ACK_OK;
  }

  if (attr.command_type == ATTR_CMD_WRITE) {
    if (attr.cluster_id == CLUSTER_SYSTEM && attr.attribute_id == ATTR_TX_INTERVAL_SEC) {
      if (!parameter_value_is_valid(PARAM_TX_INTERVAL_SEC, attr.value_i32)) {
        return ACK_INVALID_VALUE;
      }
      txIntervalSec = static_cast<uint32_t>(attr.value_i32);
      sendAttributeReport(attr.cluster_id, attr.attribute_id, attr.value_i32, 0);
      return ACK_OK;
    }
    return ACK_UNSUPPORTED_CMD;
  }

  return ACK_UNSUPPORTED_CMD;
}

static void sendAck(const DownlinkPacketV1 &downlink, uint32_t uplinkFrameCounter, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = APP_NODE_ID;
  ack.acked_frame_counter = uplinkFrameCounter;
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

static void sendAttrAck(const AttrCommandPacketV1 &attr, uint32_t uplinkFrameCounter, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = APP_NODE_ID;
  ack.acked_frame_counter = uplinkFrameCounter;
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

static void runOtaWindow(uint32_t windowSec) {
  if (windowSec == 0) {
    windowSec = OTA_DEFAULT_WINDOW_SEC;
  }

  Serial.printf("Entering OTA mode for %lu sec\n", static_cast<unsigned long>(windowSec));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t wifiDeadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < wifiDeadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA WiFi connect failed");
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME_SENSOR);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    Serial.println("Sensor OTA start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("Sensor OTA end");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Sensor OTA error: %u\n", static_cast<unsigned>(error));
  });
  ArduinoOTA.begin();

  const uint32_t deadline = millis() + (windowSec * 1000UL);
  while (millis() < deadline) {
    ArduinoOTA.handle();
    delay(10);
  }

  Serial.println("OTA window finished");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  loadPersistentState();

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

  uint32_t uplinkFrameCounter = 0;
  const bool txOk = sendPacket(uplinkFrameCounter);

  if (txOk) {
    PendingControlPacket control{};
    if (waitForControlPacket(control)) {
      if (control.isAttrCommand) {
        const uint8_t status = applyAttrCommand(control.attr);
        sendAttrAck(control.attr, uplinkFrameCounter, status);
      } else {
        const uint8_t status = applyDownlink(control.downlink);
        sendAck(control.downlink, uplinkFrameCounter, status);
        if (control.downlink.operation == DL_OP_ENTER_OTA && status == ACK_OK) {
          const uint32_t otaWindow = control.downlink.value_i32 > 0 ? static_cast<uint32_t>(control.downlink.value_i32) : 0;
          runOtaWindow(otaWindow);
        }
      }
    }
  }

  Serial.printf("Deep sleep for %lu sec\n", static_cast<unsigned long>(txIntervalSec));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(txIntervalSec) * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
