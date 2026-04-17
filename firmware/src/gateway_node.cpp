#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <SPI.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>

#include "lora_config.h"
#include "protocol.h"

using namespace lora_app;

SX1262 radio = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences prefs;
WebServer webServer(80);

volatile bool receivedFlag = false;
volatile bool rxInterruptEnabled = true;
static bool webUploadAuthorized = false;

struct GatewayConfig {
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[64];
  char mqttPass[64];
  char mqttBaseTopic[48];
  char mqttClientId[48];
  char staIp[16];
  char staGw[16];
  char staMask[16];
  char staDns[16];
};

GatewayConfig config{};
static void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME_GATEWAY);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    Serial.println("OTA start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA end");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error: %u\n", static_cast<unsigned>(error));
  });

  ArduinoOTA.begin();
  Serial.printf("OTA ready hostname=%s\n", OTA_HOSTNAME_GATEWAY);
}

static bool webAuthOk() {
  if (strlen(OTA_PASSWORD) == 0) {
    return true;
  }
  if (webServer.authenticate("admin", OTA_PASSWORD)) {
    return true;
  }
  webServer.requestAuthentication();
  return false;
}

static void setupWebOtaServer() {
  webServer.on("/status", HTTP_GET, []() {
    if (!webAuthOk()) {
      return;
    }

    JsonDocument doc;
    doc["device"] = "lora-gateway";
    doc["ip"] = WiFi.localIP().toString();
    doc["hostname"] = OTA_HOSTNAME_GATEWAY;
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = mqttClient.connected();
    doc["mqtt_host"] = config.mqttHost;
    doc["mqtt_port"] = config.mqttPort;
    doc["mqtt_base_topic"] = config.mqttBaseTopic;
    doc["uptime_sec"] = millis() / 1000UL;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["sketch_size"] = ESP.getSketchSize();
    doc["free_sketch_space"] = ESP.getFreeSketchSpace();

    String payload;
    serializeJson(doc, payload);
    webServer.send(200, "application/json", payload);
  });

  webServer.on("/", HTTP_GET, []() {
    if (!webAuthOk()) {
      return;
    }

    String page;
    page.reserve(1600);
    page += "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    page += "<title>LoRa Gateway OTA</title></head><body style='font-family:sans-serif;max-width:700px;margin:2rem auto;padding:0 1rem'>";
    page += "<h2>LoRa Gateway OTA</h2>";
    page += "<p><b>IP:</b> ";
    page += WiFi.localIP().toString();
    page += "<br><b>Hostname:</b> ";
    page += OTA_HOSTNAME_GATEWAY;
    page += "</p>";
    page += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    page += "<p><input type='file' name='firmware' accept='.bin' required></p>";
    page += "<p><button type='submit' style='padding:.6rem 1rem'>Upload & Flash</button></p>";
    page += "</form>";
    page += "<p style='color:#666'>Použij .bin soubor pro env xiao_esp32s3_gateway.</p>";
    page += "<p><a href='/status'>Gateway status JSON</a></p>";
    page += "</body></html>";

    webServer.send(200, "text/html", page);
  });

  webServer.on(
      "/update",
      HTTP_POST,
      []() {
        if (!webAuthOk()) {
          return;
        }

        const bool success = webUploadAuthorized && !Update.hasError();
        webServer.send(success ? 200 : 500, "text/plain", success ? "Update OK, rebooting..." : "Update failed");
        delay(400);
        if (success) {
          ESP.restart();
        }
      },
      []() {
        HTTPUpload &upload = webServer.upload();

        if (upload.status == UPLOAD_FILE_START) {
          webUploadAuthorized = (strlen(OTA_PASSWORD) == 0) || webServer.authenticate("admin", OTA_PASSWORD);
          if (!webUploadAuthorized) {
            return;
          }

          Serial.printf("HTTP OTA start: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (!webUploadAuthorized) {
            return;
          }
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (!webUploadAuthorized) {
            return;
          }
          if (Update.end(true)) {
            Serial.printf("HTTP OTA success, size=%u\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          if (webUploadAuthorized) {
            Update.abort();
          }
          Serial.println("HTTP OTA aborted");
        }
      });

  webServer.begin();
  Serial.printf("HTTP OTA server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

bool shouldSavePortalConfig = false;

struct PendingDownlink {
  bool inUse;
  uint32_t nodeId;
  DownlinkPacketV1 packet;
};

static PendingDownlink pendingDownlinks[32]{};
static uint32_t discoveredNodes[64]{};
static size_t discoveredNodeCount = 0;

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  if (!rxInterruptEnabled) {
    return;
  }
  receivedFlag = true;
}

static void safeCopy(char *dst, size_t dstLen, const char *src) {
  if (dstLen == 0) {
    return;
  }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static bool parseIp(const char *value, IPAddress &ip) {
  if (strlen(value) == 0) {
    return false;
  }
  return ip.fromString(value);
}

static bool isNodeDiscovered(uint32_t nodeId) {
  for (size_t i = 0; i < discoveredNodeCount; i++) {
    if (discoveredNodes[i] == nodeId) {
      return true;
    }
  }
  return false;
}

static void markNodeDiscovered(uint32_t nodeId) {
  if (isNodeDiscovered(nodeId) || discoveredNodeCount >= 64) {
    return;
  }
  discoveredNodes[discoveredNodeCount++] = nodeId;
}

static void topicForNode(char *buffer, size_t len, uint32_t nodeId, const char *suffix) {
  snprintf(buffer, len, "%s/node/%lu/%s", config.mqttBaseTopic, static_cast<unsigned long>(nodeId), suffix);
}

static void publishRetained(const char *topic, const char *payload) {
  mqttClient.publish(topic, payload, true);
}

static void loadConfig() {
  safeCopy(config.mqttHost, sizeof(config.mqttHost), MQTT_HOST);
  config.mqttPort = MQTT_PORT;
  safeCopy(config.mqttUser, sizeof(config.mqttUser), MQTT_USERNAME);
  safeCopy(config.mqttPass, sizeof(config.mqttPass), MQTT_PASSWORD);
  safeCopy(config.mqttBaseTopic, sizeof(config.mqttBaseTopic), MQTT_BASE_TOPIC);
  safeCopy(config.mqttClientId, sizeof(config.mqttClientId), MQTT_CLIENT_ID);
  config.staIp[0] = '\0';
  config.staGw[0] = '\0';
  config.staMask[0] = '\0';
  config.staDns[0] = '\0';

  prefs.begin("gateway", true);
  String mqttHost = prefs.getString("mqtt_host", config.mqttHost);
  String mqttUser = prefs.getString("mqtt_user", config.mqttUser);
  String mqttPass = prefs.getString("mqtt_pass", config.mqttPass);
  String mqttBase = prefs.getString("mqtt_base", config.mqttBaseTopic);
  String mqttCid = prefs.getString("mqtt_cid", config.mqttClientId);
  String staIp = prefs.getString("sta_ip", "");
  String staGw = prefs.getString("sta_gw", "");
  String staMask = prefs.getString("sta_mask", "");
  String staDns = prefs.getString("sta_dns", "");
  uint16_t mqttPort = static_cast<uint16_t>(prefs.getUInt("mqtt_port", config.mqttPort));
  prefs.end();

  safeCopy(config.mqttHost, sizeof(config.mqttHost), mqttHost.c_str());
  safeCopy(config.mqttUser, sizeof(config.mqttUser), mqttUser.c_str());
  safeCopy(config.mqttPass, sizeof(config.mqttPass), mqttPass.c_str());
  safeCopy(config.mqttBaseTopic, sizeof(config.mqttBaseTopic), mqttBase.c_str());
  safeCopy(config.mqttClientId, sizeof(config.mqttClientId), mqttCid.c_str());
  safeCopy(config.staIp, sizeof(config.staIp), staIp.c_str());
  safeCopy(config.staGw, sizeof(config.staGw), staGw.c_str());
  safeCopy(config.staMask, sizeof(config.staMask), staMask.c_str());
  safeCopy(config.staDns, sizeof(config.staDns), staDns.c_str());
  config.mqttPort = mqttPort;
}

static void saveConfig() {
  prefs.begin("gateway", false);
  prefs.putString("mqtt_host", config.mqttHost);
  prefs.putUInt("mqtt_port", config.mqttPort);
  prefs.putString("mqtt_user", config.mqttUser);
  prefs.putString("mqtt_pass", config.mqttPass);
  prefs.putString("mqtt_base", config.mqttBaseTopic);
  prefs.putString("mqtt_cid", config.mqttClientId);
  prefs.putString("sta_ip", config.staIp);
  prefs.putString("sta_gw", config.staGw);
  prefs.putString("sta_mask", config.staMask);
  prefs.putString("sta_dns", config.staDns);
  prefs.end();
}

static void onPortalSaveConfig() {
  shouldSavePortalConfig = true;
}

static void setupWifiWithPortal() {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setSaveConfigCallback(onPortalSaveConfig);

  WiFiManagerParameter pMqttHost("mqtt_host", "MQTT host", config.mqttHost, sizeof(config.mqttHost));
  char mqttPortStr[8];
  snprintf(mqttPortStr, sizeof(mqttPortStr), "%u", config.mqttPort);
  WiFiManagerParameter pMqttPort("mqtt_port", "MQTT port", mqttPortStr, sizeof(mqttPortStr));
  WiFiManagerParameter pMqttUser("mqtt_user", "MQTT user", config.mqttUser, sizeof(config.mqttUser));
  WiFiManagerParameter pMqttPass("mqtt_pass", "MQTT pass", config.mqttPass, sizeof(config.mqttPass));
  WiFiManagerParameter pMqttBase("mqtt_base", "MQTT base topic", config.mqttBaseTopic, sizeof(config.mqttBaseTopic));
  WiFiManagerParameter pMqttClientId("mqtt_cid", "MQTT client id", config.mqttClientId, sizeof(config.mqttClientId));
  WiFiManagerParameter pStaIp("sta_ip", "Static IP", config.staIp, sizeof(config.staIp));
  WiFiManagerParameter pStaGw("sta_gw", "Gateway", config.staGw, sizeof(config.staGw));
  WiFiManagerParameter pStaMask("sta_mask", "Subnet mask", config.staMask, sizeof(config.staMask));
  WiFiManagerParameter pStaDns("sta_dns", "DNS", config.staDns, sizeof(config.staDns));

  wm.addParameter(&pMqttHost);
  wm.addParameter(&pMqttPort);
  wm.addParameter(&pMqttUser);
  wm.addParameter(&pMqttPass);
  wm.addParameter(&pMqttBase);
  wm.addParameter(&pMqttClientId);
  wm.addParameter(&pStaIp);
  wm.addParameter(&pStaGw);
  wm.addParameter(&pStaMask);
  wm.addParameter(&pStaDns);

  IPAddress ip, gw, mask, dns;
  if (parseIp(config.staIp, ip) && parseIp(config.staGw, gw) && parseIp(config.staMask, mask)) {
    if (parseIp(config.staDns, dns)) {
      wm.setSTAStaticIPConfig(ip, gw, mask, dns);
    } else {
      wm.setSTAStaticIPConfig(ip, gw, mask);
    }
  }

  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(20);

  Serial.println("Connecting WiFi (auto) / fallback AP captive portal...");
  if (!wm.autoConnect(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD)) {
    Serial.println("WiFi setup failed, rebooting...");
    delay(1000);
    ESP.restart();
  }

  safeCopy(config.mqttHost, sizeof(config.mqttHost), pMqttHost.getValue());
  config.mqttPort = static_cast<uint16_t>(atoi(pMqttPort.getValue()));
  if (config.mqttPort == 0) {
    config.mqttPort = MQTT_PORT;
  }
  safeCopy(config.mqttUser, sizeof(config.mqttUser), pMqttUser.getValue());
  safeCopy(config.mqttPass, sizeof(config.mqttPass), pMqttPass.getValue());
  safeCopy(config.mqttBaseTopic, sizeof(config.mqttBaseTopic), pMqttBase.getValue());
  safeCopy(config.mqttClientId, sizeof(config.mqttClientId), pMqttClientId.getValue());
  safeCopy(config.staIp, sizeof(config.staIp), pStaIp.getValue());
  safeCopy(config.staGw, sizeof(config.staGw), pStaGw.getValue());
  safeCopy(config.staMask, sizeof(config.staMask), pStaMask.getValue());
  safeCopy(config.staDns, sizeof(config.staDns), pStaDns.getValue());

  if (shouldSavePortalConfig) {
    saveConfig();
    shouldSavePortalConfig = false;
  }

  Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

static void publishGatewayStatus() {
  char statusTopic[96];
  snprintf(statusTopic, sizeof(statusTopic), "%s/gateway/status", config.mqttBaseTopic);
  publishRetained(statusTopic, "online");
}

static PendingDownlink *findPending(uint32_t nodeId, bool createIfMissing) {
  PendingDownlink *freeSlot = nullptr;
  for (size_t i = 0; i < (sizeof(pendingDownlinks) / sizeof(pendingDownlinks[0])); i++) {
    if (pendingDownlinks[i].inUse && pendingDownlinks[i].nodeId == nodeId) {
      return &pendingDownlinks[i];
    }
    if (!pendingDownlinks[i].inUse && freeSlot == nullptr) {
      freeSlot = &pendingDownlinks[i];
    }
  }

  if (createIfMissing && freeSlot != nullptr) {
    freeSlot->inUse = true;
    freeSlot->nodeId = nodeId;
    memset(&freeSlot->packet, 0, sizeof(freeSlot->packet));
    return freeSlot;
  }

  return nullptr;
}

static void queueDownlinkCommand(uint32_t nodeId, uint8_t cmd, uint32_t value) {
  PendingDownlink *slot = findPending(nodeId, true);
  if (slot == nullptr) {
    Serial.println("Downlink queue full");
    return;
  }

  slot->packet.proto_ver = 1;
  slot->packet.msg_type = MSG_DOWNLINK_CMD;
  slot->packet.node_id = nodeId;
  slot->packet.target_frame_counter = 0;
  slot->packet.cmd = cmd;
  slot->packet.reserved = 0;
  slot->packet.value_u32 = value;
  finalize_packet_crc(slot->packet);

  Serial.printf("Queued downlink node=%lu cmd=%u value=%lu\n",
                static_cast<unsigned long>(nodeId),
                cmd,
                static_cast<unsigned long>(value));
}

static bool parseSetTopicNodeId(const char *topic, uint32_t &nodeId) {
  char prefix[80];
  snprintf(prefix, sizeof(prefix), "%s/node/", config.mqttBaseTopic);
  if (strncmp(topic, prefix, strlen(prefix)) != 0) {
    return false;
  }

  const char *nodeStart = topic + strlen(prefix);
  const char *slash = strchr(nodeStart, '/');
  if (slash == nullptr) {
    return false;
  }

  String nodeStr = String(nodeStart).substring(0, slash - nodeStart);
  if (String(slash) != "/set") {
    return false;
  }

  nodeId = static_cast<uint32_t>(nodeStr.toInt());
  return nodeId > 0;
}

static void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  uint32_t nodeId = 0;
  if (!parseSetTopicNodeId(topic, nodeId)) {
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload, length);
  const char *cmd = nullptr;
  uint32_t value = 0;
  float areaM2 = 0.0f;

  if (!err) {
    cmd = doc["cmd"] | "";
    value = doc["sec"] | doc["value"] | 0;
    if (!doc["area_m2"].isNull()) {
      areaM2 = doc["area_m2"].as<float>();
    } else if (!doc["area"].isNull()) {
      areaM2 = doc["area"].as<float>();
    }
    if (!doc["min_mm"].isNull()) {
      value = doc["min_mm"].as<uint32_t>();
    }
    if (!doc["max_mm"].isNull()) {
      value = doc["max_mm"].as<uint32_t>();
    }
  } else {
    String raw;
    for (unsigned int i = 0; i < length; i++) {
      raw += static_cast<char>(payload[i]);
    }
    raw.trim();
    cmd = raw.c_str();
    value = 0;
  }

  uint8_t downlinkCmd = 0;
  if (strcmp(cmd, "ping") == 0) {
    downlinkCmd = CMD_PING;
  } else if (strcmp(cmd, "reboot") == 0) {
    downlinkCmd = CMD_REBOOT;
  } else if (strcmp(cmd, "enter_ota") == 0) {
    downlinkCmd = CMD_ENTER_OTA;
    if (value == 0) {
      value = OTA_DEFAULT_WINDOW_SEC;
    }
  } else if (strcmp(cmd, "set_interval") == 0 || strcmp(cmd, "set_interval_sec") == 0) {
    downlinkCmd = CMD_SET_INTERVAL_SEC;
    if (value < APP_TX_INTERVAL_MIN_SEC || value > APP_TX_INTERVAL_MAX_SEC) {
      Serial.printf("Rejected set_interval out of range: %lu\n", static_cast<unsigned long>(value));
      return;
    }
  } else if (strcmp(cmd, "set_tank_area") == 0 || strcmp(cmd, "set_tank_area_m2") == 0) {
    downlinkCmd = CMD_SET_TANK_AREA_M2_X1000;
    if (areaM2 <= 0.0f) {
      Serial.printf("Rejected set_tank_area invalid area: %.4f\n", areaM2);
      return;
    }
    const float areaX1000f = areaM2 * 1000.0f;
    if (areaX1000f < 1.0f || areaX1000f > 4294967295.0f) {
      Serial.printf("Rejected set_tank_area out of range: %.4f\n", areaM2);
      return;
    }
    value = static_cast<uint32_t>(areaX1000f + 0.5f);
  } else if (strcmp(cmd, "set_tank_min") == 0 || strcmp(cmd, "set_tank_min_mm") == 0) {
    downlinkCmd = CMD_SET_TANK_DISTANCE_MIN_MM;
    if (value == 0 || value > 65535) {
      Serial.printf("Rejected set_tank_min_mm out of range: %lu\n", static_cast<unsigned long>(value));
      return;
    }
  } else if (strcmp(cmd, "set_tank_max") == 0 || strcmp(cmd, "set_tank_max_mm") == 0) {
    downlinkCmd = CMD_SET_TANK_DISTANCE_MAX_MM;
    if (value == 0 || value > 65535) {
      Serial.printf("Rejected set_tank_max_mm out of range: %lu\n", static_cast<unsigned long>(value));
      return;
    }
  } else {
    Serial.printf("Unsupported cmd: %s\n", cmd ? cmd : "<null>");
    return;
  }

  queueDownlinkCommand(nodeId, downlinkCmd, value);
}

static void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.reconnect();
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Reconnect failed, reopening captive portal...");
    setupWifiWithPortal();
  }
}

static void ensureMqttConnected() {
  if (mqttClient.connected()) {
    return;
  }

  mqttClient.setServer(config.mqttHost, config.mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);

  char statusTopic[96];
  snprintf(statusTopic, sizeof(statusTopic), "%s/gateway/status", config.mqttBaseTopic);

  while (!mqttClient.connected()) {
    Serial.printf("Connecting MQTT %s:%u ... ", config.mqttHost, config.mqttPort);
    bool connected;
    if (strlen(config.mqttUser) > 0) {
      connected = mqttClient.connect(config.mqttClientId, config.mqttUser, config.mqttPass, statusTopic, 1, true, "offline");
    } else {
      connected = mqttClient.connect(config.mqttClientId, statusTopic, 1, true, "offline");
    }

    if (!connected) {
      Serial.printf("failed rc=%d\n", mqttClient.state());
      delay(3000);
      continue;
    }

    char setWildcard[96];
    snprintf(setWildcard, sizeof(setWildcard), "%s/node/+/set", config.mqttBaseTopic);
    mqttClient.subscribe(setWildcard);
    publishGatewayStatus();
    Serial.println("ok");
  }
}

static void publishDiscoverySensor(uint32_t nodeId, const char *key, const char *name, const char *unit,
                                   const char *deviceClass, const char *stateClass) {
  char topic[128];
  char payload[768];
  char optionalFields[160];
  snprintf(topic, sizeof(topic), "homeassistant/sensor/lora_%lu_%s/config", static_cast<unsigned long>(nodeId), key);

  optionalFields[0] = '\0';
  if (strlen(unit) > 0) {
    strncat(optionalFields, "\"unit_of_measurement\":\"", sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, unit, sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, "\",", sizeof(optionalFields) - strlen(optionalFields) - 1);
  }
  if (strlen(deviceClass) > 0) {
    strncat(optionalFields, "\"device_class\":\"", sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, deviceClass, sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, "\",", sizeof(optionalFields) - strlen(optionalFields) - 1);
  }
  if (strlen(stateClass) > 0) {
    strncat(optionalFields, "\"state_class\":\"", sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, stateClass, sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, "\",", sizeof(optionalFields) - strlen(optionalFields) - 1);
  }
  const size_t optLen = strlen(optionalFields);
  if (optLen > 0 && optionalFields[optLen - 1] == ',') {
    optionalFields[optLen - 1] = '\0';
  }

  int written = snprintf(
      payload,
      sizeof(payload),
      "{\"name\":\"%s\",\"unique_id\":\"lora_%lu_%s\",\"state_topic\":\"%s/node/%lu/state\","
      "\"availability_topic\":\"%s/node/%lu/availability\",\"value_template\":\"{{ value_json.%s }}\","
      "\"device\":{\"identifiers\":[\"lora_%lu\"],\"name\":\"LoRa Node %lu\",\"manufacturer\":\"Seeed Studio\",\"model\":\"XIAO ESP32S3 + Wio-SX1262\"}%s%s}",
      name,
      static_cast<unsigned long>(nodeId),
      key,
      config.mqttBaseTopic,
      static_cast<unsigned long>(nodeId),
      config.mqttBaseTopic,
      static_cast<unsigned long>(nodeId),
      key,
      static_cast<unsigned long>(nodeId),
      static_cast<unsigned long>(nodeId),
      strlen(optionalFields) > 0 ? "," : "",
      optionalFields);

  if (written > 0 && written < static_cast<int>(sizeof(payload))) {
    publishRetained(topic, payload);
  }
}

static void publishDiscoveryNode(uint32_t nodeId) {
  publishDiscoverySensor(nodeId, "temperature", "Temperature", "°C", "temperature", "measurement");
  publishDiscoverySensor(nodeId, "humidity", "Humidity", "%", "humidity", "measurement");
  publishDiscoverySensor(nodeId, "pressure", "Pressure", "hPa", "pressure", "measurement");
  publishDiscoverySensor(nodeId, "battery", "Battery", "V", "voltage", "measurement");
  publishDiscoverySensor(nodeId, "rssi", "RSSI", "dBm", "signal_strength", "measurement");
  publishDiscoverySensor(nodeId, "snr", "SNR", "dB", "", "measurement");
}

static void publishDiscoveryDistanceNode(uint32_t nodeId) {
  publishDiscoverySensor(nodeId, "distance_cm", "Distance", "cm", "distance", "measurement");
  publishDiscoverySensor(nodeId, "level_cm", "Water level", "cm", "distance", "measurement");
  publishDiscoverySensor(nodeId, "water_l", "Water volume", "L", "volume_storage", "measurement");
  publishDiscoverySensor(nodeId, "battery", "Battery", "V", "voltage", "measurement");
  publishDiscoverySensor(nodeId, "rssi", "RSSI", "dBm", "signal_strength", "measurement");
  publishDiscoverySensor(nodeId, "snr", "SNR", "dB", "", "measurement");
}

static void publishTelemetryMqtt(const TelemetryPacketV1 &pkt, float rssi, float snr) {
  char stateTopic[128];
  char statusTopic[128];
  char availabilityTopic[128];
  char statePayload[256];
  char statusPayload[256];

  if (!isNodeDiscovered(pkt.node_id)) {
    publishDiscoveryNode(pkt.node_id);
    markNodeDiscovered(pkt.node_id);
  }

  topicForNode(stateTopic, sizeof(stateTopic), pkt.node_id, "state");
  topicForNode(statusTopic, sizeof(statusTopic), pkt.node_id, "status");
  topicForNode(availabilityTopic, sizeof(availabilityTopic), pkt.node_id, "availability");

  snprintf(
      statePayload,
      sizeof(statePayload),
      "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f,\"battery\":%.3f,\"rssi\":%.1f,\"snr\":%.1f,\"fcnt\":%lu,\"flags\":%u}",
      pkt.temp_c_x100 / 100.0,
      pkt.rh_x100 / 100.0,
      pkt.pressure_pa_div10 / 10.0,
      pkt.battery_mV / 1000.0,
      rssi,
      snr,
      static_cast<unsigned long>(pkt.frame_counter),
      pkt.flags);

  snprintf(
      statusPayload,
      sizeof(statusPayload),
      "{\"proto\":%u,\"node_id\":%lu,\"fcnt\":%lu,\"battery\":%.3f,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f,\"flags\":%u,\"rssi\":%.1f,\"snr\":%.1f}",
      pkt.proto_ver,
      static_cast<unsigned long>(pkt.node_id),
      static_cast<unsigned long>(pkt.frame_counter),
      pkt.battery_mV / 1000.0,
      pkt.temp_c_x100 / 100.0,
      pkt.rh_x100 / 100.0,
      pkt.pressure_pa_div10 / 10.0,
      pkt.flags,
      rssi,
      snr);

  mqttClient.publish(stateTopic, statePayload, false);
  mqttClient.publish(statusTopic, statusPayload, false);
  publishRetained(availabilityTopic, "online");
}

static void publishAckMqtt(const AckPacketV1 &ack, float rssi, float snr) {
  char ackTopic[128];
  char ackPayload[256];
  topicForNode(ackTopic, sizeof(ackTopic), ack.node_id, "ack");
  snprintf(
      ackPayload,
      sizeof(ackPayload),
      "{\"node_id\":%lu,\"acked_fcnt\":%lu,\"cmd\":%u,\"status\":%u,\"interval_sec\":%lu,\"rssi\":%.1f,\"snr\":%.1f}",
      static_cast<unsigned long>(ack.node_id),
      static_cast<unsigned long>(ack.acked_frame_counter),
      ack.acked_cmd,
      ack.status,
      static_cast<unsigned long>(ack.current_interval_sec),
      rssi,
      snr);
  mqttClient.publish(ackTopic, ackPayload, false);
}

static void publishDistanceMqtt(const DistancePacketV1 &pkt, float rssi, float snr) {
  char stateTopic[128];
  char statusTopic[128];
  char availabilityTopic[128];
  char statePayload[256];
  char statusPayload[256];

  if (!isNodeDiscovered(pkt.node_id)) {
    publishDiscoveryDistanceNode(pkt.node_id);
    markNodeDiscovered(pkt.node_id);
  }

  topicForNode(stateTopic, sizeof(stateTopic), pkt.node_id, "state");
  topicForNode(statusTopic, sizeof(statusTopic), pkt.node_id, "status");
  topicForNode(availabilityTopic, sizeof(availabilityTopic), pkt.node_id, "availability");

  const float distanceCm = pkt.distance_mm / 10.0f;
  const float levelCm = pkt.level_mm / 10.0f;
  const float waterL = pkt.water_liters_x10 / 10.0f;
  const float batteryV = (pkt.battery_mV > 0) ? (pkt.battery_mV / 1000.0f) : 0.0f;

  snprintf(
      statePayload,
      sizeof(statePayload),
      "{\"distance_cm\":%.1f,\"level_cm\":%.1f,\"water_l\":%.1f,\"battery\":%.3f,\"rssi\":%.1f,\"snr\":%.1f,\"fcnt\":%lu,\"flags\":%u}",
      distanceCm,
      levelCm,
      waterL,
      batteryV,
      rssi,
      snr,
      static_cast<unsigned long>(pkt.frame_counter),
      pkt.flags);

  snprintf(
      statusPayload,
      sizeof(statusPayload),
      "{\"proto\":%u,\"node_id\":%lu,\"fcnt\":%lu,\"distance_cm\":%.1f,\"level_cm\":%.1f,\"water_l\":%.1f,\"battery\":%.3f,\"flags\":%u,\"rssi\":%.1f,\"snr\":%.1f}",
      pkt.proto_ver,
      static_cast<unsigned long>(pkt.node_id),
      static_cast<unsigned long>(pkt.frame_counter),
      distanceCm,
      levelCm,
      waterL,
      batteryV,
      pkt.flags,
      rssi,
      snr);

  mqttClient.publish(stateTopic, statePayload, false);
  mqttClient.publish(statusTopic, statusPayload, false);
  publishRetained(availabilityTopic, "online");
}

static void maybeSendPendingDownlink(uint32_t nodeId, uint32_t currentFcnt) {
  PendingDownlink *slot = findPending(nodeId, false);
  if (slot == nullptr || !slot->inUse) {
    return;
  }

  slot->packet.target_frame_counter = currentFcnt;
  finalize_packet_crc(slot->packet);

  rxInterruptEnabled = false;
  radio.standby();
  int state = radio.transmit(reinterpret_cast<uint8_t *>(&slot->packet), sizeof(slot->packet));
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Downlink sent node=%lu cmd=%u value=%lu\n",
                  static_cast<unsigned long>(nodeId),
                  slot->packet.cmd,
                  static_cast<unsigned long>(slot->packet.value_u32));
    slot->inUse = false;
  } else {
    Serial.printf("Downlink TX failed state=%d\n", state);
  }

  radio.startReceive();
  rxInterruptEnabled = true;
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  loadConfig();
  setupWifiWithPortal();

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

  ensureMqttConnected();
  setupOta();
  setupWebOtaServer();

  radio.setDio1Action(setFlag);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed, state=%d\n", state);
  } else {
    Serial.println("Gateway ready");
  }
}

void loop() {
  ensureWifiConnected();
  ensureMqttConnected();
  mqttClient.loop();
  ArduinoOTA.handle();
  webServer.handleClient();

  if (!receivedFlag) {
    delay(10);
    return;
  }

  rxInterruptEnabled = false;
  receivedFlag = false;

  size_t packetLen = radio.getPacketLength();
  if (packetLen == 0 || packetLen > 64) {
    radio.startReceive();
    rxInterruptEnabled = true;
    return;
  }

  uint8_t bytes[64]{};
  int state = radio.readData(bytes, packetLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("{\"error\":\"read\",\"state\":%d}\n", state);
    radio.startReceive();
    rxInterruptEnabled = true;
    return;
  }

  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();

  if (packetLen >= 2 && bytes[0] == 1 && bytes[1] == MSG_TELEMETRY && packetLen == sizeof(TelemetryPacketV1)) {
    TelemetryPacketV1 pkt{};
    memcpy(&pkt, bytes, sizeof(pkt));
    if (!validate_packet_crc(pkt)) {
      Serial.println("{\"error\":\"crc\"}");
    } else {
      Serial.printf("Telemetry node=%lu fcnt=%lu rssi=%.1f snr=%.1f\n",
                    static_cast<unsigned long>(pkt.node_id),
                    static_cast<unsigned long>(pkt.frame_counter),
                    rssi,
                    snr);
      publishTelemetryMqtt(pkt, rssi, snr);
      maybeSendPendingDownlink(pkt.node_id, pkt.frame_counter);
    }
  } else if (packetLen >= 2 && bytes[0] == 1 && bytes[1] == MSG_ACK && packetLen == sizeof(AckPacketV1)) {
    AckPacketV1 ack{};
    memcpy(&ack, bytes, sizeof(ack));
    if (!validate_packet_crc(ack)) {
      Serial.println("{\"error\":\"ack_crc\"}");
    } else {
      Serial.printf("ACK node=%lu cmd=%u status=%u interval=%lu\n",
                    static_cast<unsigned long>(ack.node_id),
                    ack.acked_cmd,
                    ack.status,
                    static_cast<unsigned long>(ack.current_interval_sec));
      publishAckMqtt(ack, rssi, snr);
    }
  } else if (packetLen >= 2 && bytes[0] == 1 && bytes[1] == MSG_DISTANCE && packetLen == sizeof(DistancePacketV1)) {
    DistancePacketV1 pkt{};
    memcpy(&pkt, bytes, sizeof(pkt));
    if (!validate_packet_crc(pkt)) {
      Serial.println("{\"error\":\"distance_crc\"}");
    } else {
      Serial.printf("Distance node=%lu fcnt=%lu distance=%.1f cm flags=%u\n",
                    static_cast<unsigned long>(pkt.node_id),
                    static_cast<unsigned long>(pkt.frame_counter),
                    pkt.distance_mm / 10.0f,
                    pkt.flags);
      publishDistanceMqtt(pkt, rssi, snr);
      maybeSendPendingDownlink(pkt.node_id, pkt.frame_counter);
    }
  } else {
    Serial.printf("Unknown packet len=%u msg_type=%u\n", static_cast<unsigned>(packetLen), bytes[1]);
  }

  radio.startReceive();
  rxInterruptEnabled = true;
}
