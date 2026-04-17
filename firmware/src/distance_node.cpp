#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <time.h>

#include "lora_config.h"
#include "protocol.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);
Preferences prefs;

RTC_DATA_ATTR static uint32_t frameCounter = 0;
RTC_DATA_ATTR static uint32_t txIntervalSec = DISTANCE_TX_INTERVAL_SEC;
RTC_DATA_ATTR static uint32_t tankAreaM2x1000 = static_cast<uint32_t>(TANK_AREA_M2 * 1000.0f + 0.5f);
RTC_DATA_ATTR static uint16_t tankDistanceMinMm = static_cast<uint16_t>(TANK_DISTANCE_MIN_MM);
RTC_DATA_ATTR static uint16_t tankDistanceMaxMm = static_cast<uint16_t>(TANK_DISTANCE_MAX_MM);
volatile bool rxWindowFlag = false;

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

  const uint32_t loadedInterval = prefs.getUInt("tx_i", txIntervalSec);
  const uint32_t loadedAreaX1000 = prefs.getUInt("area_x1k", tankAreaM2x1000);
  const uint16_t loadedMinMm = static_cast<uint16_t>(prefs.getUInt("dmin", tankDistanceMinMm));
  const uint16_t loadedMaxMm = static_cast<uint16_t>(prefs.getUInt("dmax", tankDistanceMaxMm));

  prefs.end();

  if (loadedInterval >= APP_TX_INTERVAL_MIN_SEC && loadedInterval <= APP_TX_INTERVAL_MAX_SEC) {
    txIntervalSec = loadedInterval;
  }

  if (configLooksValid(loadedAreaX1000, loadedMinMm, loadedMaxMm)) {
    tankAreaM2x1000 = loadedAreaX1000;
    tankDistanceMinMm = loadedMinMm;
    tankDistanceMaxMm = loadedMaxMm;
  }
}

static void saveRuntimeConfig() {
  prefs.begin("distance", false);
  prefs.putUInt("tx_i", txIntervalSec);
  prefs.putUInt("area_x1k", tankAreaM2x1000);
  prefs.putUInt("dmin", tankDistanceMinMm);
  prefs.putUInt("dmax", tankDistanceMaxMm);
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
  return 0;
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

static bool sendDistancePacket(uint16_t distanceMm, uint16_t levelMm, uint32_t waterLitersX10, bool measureOk) {
  DistancePacketV1 pkt{};
  pkt.proto_ver = 1;
  pkt.msg_type = MSG_DISTANCE;
  pkt.node_id = DISTANCE_NODE_ID;
  pkt.frame_counter = frameCounter++;
  pkt.unix_time = unixTimeNow();
  pkt.battery_mV = readBatteryMilliVolts();
  pkt.distance_mm = distanceMm;
  pkt.level_mm = levelMm;
  pkt.water_liters_x10 = waterLitersX10;
  pkt.flags = measureOk ? 0x00 : 0x01;
  finalize_packet_crc(pkt);

  int state = radio.transmit(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Distance TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf("Distance TX ok node=%lu fcnt=%lu distance=%u mm level=%u mm water=%.1f L flags=%u\n",
                static_cast<unsigned long>(pkt.node_id),
                static_cast<unsigned long>(pkt.frame_counter),
                pkt.distance_mm,
                pkt.level_mm,
                pkt.water_liters_x10 / 10.0f,
                pkt.flags);
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
    if (downlink.proto_ver != 1 || downlink.msg_type != MSG_DOWNLINK_CMD || downlink.node_id != DISTANCE_NODE_ID) {
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
      saveRuntimeConfig();
      return ACK_OK;
    case CMD_SET_TANK_AREA_M2_X1000:
      if (downlink.value_u32 == 0) {
        return ACK_INVALID_VALUE;
      }
      if (!configLooksValid(downlink.value_u32, tankDistanceMinMm, tankDistanceMaxMm)) {
        return ACK_INVALID_VALUE;
      }
      tankAreaM2x1000 = downlink.value_u32;
      saveRuntimeConfig();
      return ACK_OK;
    case CMD_SET_TANK_DISTANCE_MIN_MM: {
      const uint16_t newMin = static_cast<uint16_t>(downlink.value_u32 > 65535 ? 65535 : downlink.value_u32);
      if (!configLooksValid(tankAreaM2x1000, newMin, tankDistanceMaxMm)) {
        return ACK_INVALID_VALUE;
      }
      tankDistanceMinMm = newMin;
      saveRuntimeConfig();
      return ACK_OK;
    }
    case CMD_SET_TANK_DISTANCE_MAX_MM: {
      const uint16_t newMax = static_cast<uint16_t>(downlink.value_u32 > 65535 ? 65535 : downlink.value_u32);
      if (!configLooksValid(tankAreaM2x1000, tankDistanceMinMm, newMax)) {
        return ACK_INVALID_VALUE;
      }
      tankDistanceMaxMm = newMax;
      saveRuntimeConfig();
      return ACK_OK;
    }
    case CMD_REBOOT:
      return ACK_OK;
    default:
      return ACK_UNSUPPORTED_CMD;
  }
}

static void sendAck(const DownlinkPacketV1 &downlink, uint32_t ackedFrameCounter, uint8_t status) {
  AckPacketV1 ack{};
  ack.proto_ver = 1;
  ack.msg_type = MSG_ACK;
  ack.node_id = DISTANCE_NODE_ID;
  ack.acked_frame_counter = ackedFrameCounter;
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

  const uint32_t ackedFrameCounter = frameCounter;
  const bool txOk = sendDistancePacket(clampedDistanceMm, levelMm, waterLitersX10, measureOk);

  if (txOk) {
    DownlinkPacketV1 downlink{};
    if (waitForDownlink(downlink)) {
      const uint8_t status = applyDownlink(downlink);
      sendAck(downlink, ackedFrameCounter, status);
    }
  }

  Serial.printf("Deep sleep for %lu sec\n", static_cast<unsigned long>(txIntervalSec));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(txIntervalSec) * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
