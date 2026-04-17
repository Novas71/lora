#include <Arduino.h>
#include <ArduinoOTA.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <time.h>
#include <WiFi.h>

#include "lora_config.h"
#include "protocol.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);

static uint32_t frameCounter = 0;
RTC_DATA_ATTR static uint32_t txIntervalSec = APP_TX_INTERVAL_SEC;
volatile bool rxWindowFlag = false;

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
  return static_cast<uint16_t>(3600 + (esp_random() % 500));
}

static uint32_t unixTimeNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

static bool sendPacket(TelemetryPacketV1 &pkt) {
  pkt = TelemetryPacketV1{};
  pkt.proto_ver = 1;
  pkt.msg_type = MSG_TELEMETRY;
  pkt.node_id = APP_NODE_ID;
  pkt.frame_counter = frameCounter++;
  pkt.unix_time = unixTimeNow();
  pkt.battery_mV = readBatteryMilliVolts();
  pkt.temp_c_x100 = readTemperatureCx100();
  pkt.rh_x100 = readHumidityX100();
  pkt.pressure_pa_div10 = readPressureDiv10();
  pkt.flags = (pkt.battery_mV < 3500) ? 0x01 : 0x00;
  finalize_packet_crc(pkt);

  int state = radio.transmit(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf(
      "TX ok node=%lu fcnt=%lu bat=%.3fV temp=%.2fC hum=%.2f%% press=%.1fhPa\n",
      static_cast<unsigned long>(pkt.node_id),
      static_cast<unsigned long>(pkt.frame_counter),
      pkt.battery_mV / 1000.0,
      pkt.temp_c_x100 / 100.0,
      pkt.rh_x100 / 100.0,
      pkt.pressure_pa_div10 / 10.0);
  return true;
}

static bool waitForDownlink(DownlinkPacketV1 &downlink) {
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
    uint8_t bytes[sizeof(DownlinkPacketV1)]{};
    state = radio.readData(bytes, sizeof(bytes));
    if (state != RADIOLIB_ERR_NONE) {
      radio.startReceive();
      continue;
    }

    memcpy(&downlink, bytes, sizeof(downlink));
    if (downlink.proto_ver != 1 || downlink.msg_type != MSG_DOWNLINK_CMD || downlink.node_id != APP_NODE_ID) {
      radio.startReceive();
      continue;
    }

    if (!validate_packet_crc(downlink)) {
      Serial.println("Downlink CRC invalid");
      radio.startReceive();
      continue;
    }

    Serial.printf("Downlink cmd=%u value=%lu\n", downlink.cmd, static_cast<unsigned long>(downlink.value_u32));
    return true;
  }

  return false;
}

static uint8_t applyDownlink(const DownlinkPacketV1 &downlink) {
  switch (downlink.cmd) {
    case CMD_PING:
      return ACK_OK;
    case CMD_SET_INTERVAL_SEC:
      if (downlink.value_u32 < APP_TX_INTERVAL_MIN_SEC || downlink.value_u32 > APP_TX_INTERVAL_MAX_SEC) {
        return ACK_INVALID_VALUE;
      }
      txIntervalSec = downlink.value_u32;
      return ACK_OK;
    case CMD_REBOOT:
      return ACK_OK;
    case CMD_ENTER_OTA:
      return ACK_OK;
    default:
      return ACK_UNSUPPORTED_CMD;
  }
}

static void sendAck(const DownlinkPacketV1 &downlink, const TelemetryPacketV1 &uplink, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = APP_NODE_ID;
  ack.acked_frame_counter = uplink.frame_counter;
  ack.acked_cmd = downlink.cmd;
  ack.status = status;
  ack.current_interval_sec = txIntervalSec;
  finalize_packet_crc(ack);

  radio.standby();
  int state = radio.transmit(reinterpret_cast<uint8_t *>(&ack), sizeof(ack));
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("ACK sent cmd=%u status=%u interval=%lu\n",
                  ack.acked_cmd,
                  ack.status,
                  static_cast<unsigned long>(ack.current_interval_sec));
  } else {
    Serial.printf("ACK TX failed, state=%d\n", state);
  }

  if (downlink.cmd == CMD_REBOOT && status == ACK_OK) {
    delay(200);
    ESP.restart();
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

  TelemetryPacketV1 uplink{};
  const bool txOk = sendPacket(uplink);

  if (txOk) {
    DownlinkPacketV1 downlink{};
    if (waitForDownlink(downlink)) {
      const uint8_t status = applyDownlink(downlink);
      sendAck(downlink, uplink, status);
      if (downlink.cmd == CMD_ENTER_OTA && status == ACK_OK) {
        runOtaWindow(downlink.value_u32);
      }
    }
  }

  Serial.printf("Deep sleep for %lu sec\n", static_cast<unsigned long>(txIntervalSec));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(txIntervalSec) * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
