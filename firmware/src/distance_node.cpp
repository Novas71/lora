#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <time.h>

#include "lora_config.h"
#include "protocol.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);

RTC_DATA_ATTR static uint32_t frameCounter = 0;

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

static bool sendDistancePacket(uint16_t distanceMm, bool measureOk) {
  DistancePacketV1 pkt{};
  pkt.proto_ver = 1;
  pkt.msg_type = MSG_DISTANCE;
  pkt.node_id = DISTANCE_NODE_ID;
  pkt.frame_counter = frameCounter++;
  pkt.unix_time = unixTimeNow();
  pkt.battery_mV = readBatteryMilliVolts();
  pkt.distance_mm = distanceMm;
  pkt.flags = measureOk ? 0x00 : 0x01;
  finalize_packet_crc(pkt);

  int state = radio.transmit(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Distance TX failed, state=%d\n", state);
    return false;
  }

  Serial.printf("Distance TX ok node=%lu fcnt=%lu distance=%u mm flags=%u\n",
                static_cast<unsigned long>(pkt.node_id),
                static_cast<unsigned long>(pkt.frame_counter),
                pkt.distance_mm,
                pkt.flags);
  return true;
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

  sendDistancePacket(distanceMm, measureOk);

  Serial.printf("Deep sleep for %d sec\n", DISTANCE_TX_INTERVAL_SEC);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(DISTANCE_TX_INTERVAL_SEC) * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
