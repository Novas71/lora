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
#include "secure_transport.h"

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

struct PendingAttrCommand {
  bool inUse;
  uint32_t nodeId;
  AttrCommandPacketV1 packet;
};

struct NodeAddressEntry {
  bool inUse;
  uint32_t nodeId;
  uint16_t shortAddr;
};

struct GroupEntry {
  bool inUse;
  char name[24];
  uint32_t nodes[24];
  uint8_t nodeCount;
};

struct BindRuleEntry {
  bool inUse;
  uint32_t srcNode;
  uint16_t srcCluster;
  uint16_t srcAttr;
  uint32_t dstNode;
  uint16_t dstCluster;
  uint16_t dstAttr;
  int32_t scaleX1000;
  int32_t offset;
};

static PendingDownlink pendingDownlinks[32]{};
static PendingAttrCommand pendingAttrCommands[32]{};
static uint32_t discoveredNodes[64]{};
static size_t discoveredNodeCount = 0;
static NodeAddressEntry nodeAddressTable[64]{};
static GroupEntry groupTable[16]{};
static BindRuleEntry bindRuleTable[64]{};

static void queueAttrCommand(uint32_t nodeId, uint8_t commandType, uint16_t clusterId, uint16_t attributeId, int32_t value);

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

static void loadF3State() {
  prefs.begin("gateway", true);
  const size_t groupBytes = prefs.getBytesLength("f3_groups");
  if (groupBytes == sizeof(groupTable)) {
    prefs.getBytes("f3_groups", groupTable, sizeof(groupTable));
  }
  const size_t bindBytes = prefs.getBytesLength("f3_binds");
  if (bindBytes == sizeof(bindRuleTable)) {
    prefs.getBytes("f3_binds", bindRuleTable, sizeof(bindRuleTable));
  }
  prefs.end();
}

static void saveF3State() {
  prefs.begin("gateway", false);
  prefs.putBytes("f3_groups", groupTable, sizeof(groupTable));
  prefs.putBytes("f3_binds", bindRuleTable, sizeof(bindRuleTable));
  prefs.end();
}

static bool normalizeGroupName(const char *input, char *output, size_t outputLen) {
  if (input == nullptr || output == nullptr || outputLen < 2) {
    return false;
  }
  size_t w = 0;
  for (size_t i = 0; input[i] != '\0' && w + 1 < outputLen; i++) {
    char c = input[i];
    if ((c >= 'A' && c <= 'Z')) {
      c = static_cast<char>(c + ('a' - 'A'));
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      continue;
    }
    output[w++] = c;
  }
  output[w] = '\0';
  return w > 0;
}

static GroupEntry *findGroupEntry(const char *groupName, bool createIfMissing) {
  GroupEntry *freeSlot = nullptr;
  for (size_t i = 0; i < (sizeof(groupTable) / sizeof(groupTable[0])); i++) {
    if (groupTable[i].inUse && strncmp(groupTable[i].name, groupName, sizeof(groupTable[i].name)) == 0) {
      return &groupTable[i];
    }
    if (!groupTable[i].inUse && freeSlot == nullptr) {
      freeSlot = &groupTable[i];
    }
  }
  if (createIfMissing && freeSlot != nullptr) {
    freeSlot->inUse = true;
    safeCopy(freeSlot->name, sizeof(freeSlot->name), groupName);
    freeSlot->nodeCount = 0;
    memset(freeSlot->nodes, 0, sizeof(freeSlot->nodes));
    return freeSlot;
  }
  return nullptr;
}

static bool groupContainsNode(const GroupEntry &group, uint32_t nodeId) {
  for (uint8_t i = 0; i < group.nodeCount; i++) {
    if (group.nodes[i] == nodeId) {
      return true;
    }
  }
  return false;
}

static bool addNodeToGroup(const char *groupName, uint32_t nodeId) {
  GroupEntry *group = findGroupEntry(groupName, true);
  if (group == nullptr) {
    return false;
  }
  if (groupContainsNode(*group, nodeId)) {
    return true;
  }
  if (group->nodeCount >= (sizeof(group->nodes) / sizeof(group->nodes[0]))) {
    return false;
  }
  group->nodes[group->nodeCount++] = nodeId;
  saveF3State();
  return true;
}

static bool removeNodeFromGroup(const char *groupName, uint32_t nodeId) {
  GroupEntry *group = findGroupEntry(groupName, false);
  if (group == nullptr) {
    return false;
  }
  for (uint8_t i = 0; i < group->nodeCount; i++) {
    if (group->nodes[i] == nodeId) {
      for (uint8_t j = i + 1; j < group->nodeCount; j++) {
        group->nodes[j - 1] = group->nodes[j];
      }
      group->nodeCount--;
      group->nodes[group->nodeCount] = 0;
      if (group->nodeCount == 0) {
        group->inUse = false;
        group->name[0] = '\0';
      }
      saveF3State();
      return true;
    }
  }
  return false;
}

static void clearNodeFromAllGroups(uint32_t nodeId) {
  bool changed = false;
  for (size_t i = 0; i < (sizeof(groupTable) / sizeof(groupTable[0])); i++) {
    GroupEntry &group = groupTable[i];
    if (!group.inUse) {
      continue;
    }
    for (uint8_t idx = 0; idx < group.nodeCount; idx++) {
      if (group.nodes[idx] == nodeId) {
        for (uint8_t j = idx + 1; j < group.nodeCount; j++) {
          group.nodes[j - 1] = group.nodes[j];
        }
        group.nodeCount--;
        group.nodes[group.nodeCount] = 0;
        changed = true;
        idx--;
      }
    }
    if (group.nodeCount == 0) {
      group.inUse = false;
      group.name[0] = '\0';
    }
  }
  if (changed) {
    saveF3State();
  }
}

static void publishNodeGroupsMqtt(uint32_t nodeId) {
  char topic[128];
  topicForNode(topic, sizeof(topic), nodeId, "groups");

  JsonDocument doc;
  JsonArray arr = doc["groups"].to<JsonArray>();
  for (size_t i = 0; i < (sizeof(groupTable) / sizeof(groupTable[0])); i++) {
    const GroupEntry &group = groupTable[i];
    if (!group.inUse || !groupContainsNode(group, nodeId)) {
      continue;
    }
    arr.add(group.name);
  }

  String payload;
  serializeJson(doc, payload);
  publishRetained(topic, payload.c_str());
}

static BindRuleEntry *findBindRule(const BindRuleEntry &needle, bool createIfMissing) {
  BindRuleEntry *freeSlot = nullptr;
  for (size_t i = 0; i < (sizeof(bindRuleTable) / sizeof(bindRuleTable[0])); i++) {
    BindRuleEntry &slot = bindRuleTable[i];
    if (slot.inUse && slot.srcNode == needle.srcNode && slot.srcCluster == needle.srcCluster &&
        slot.srcAttr == needle.srcAttr && slot.dstNode == needle.dstNode && slot.dstCluster == needle.dstCluster &&
        slot.dstAttr == needle.dstAttr) {
      return &slot;
    }
    if (!slot.inUse && freeSlot == nullptr) {
      freeSlot = &slot;
    }
  }
  if (createIfMissing && freeSlot != nullptr) {
    *freeSlot = needle;
    freeSlot->inUse = true;
    return freeSlot;
  }
  return nullptr;
}

static bool addOrUpdateBindRule(const BindRuleEntry &rule) {
  BindRuleEntry *slot = findBindRule(rule, true);
  if (slot == nullptr) {
    return false;
  }
  *slot = rule;
  slot->inUse = true;
  saveF3State();
  return true;
}

static bool removeBindRule(const BindRuleEntry &rule) {
  BindRuleEntry *slot = findBindRule(rule, false);
  if (slot == nullptr) {
    return false;
  }
  memset(slot, 0, sizeof(*slot));
  saveF3State();
  return true;
}

static void publishNodeBindingsMqtt(uint32_t nodeId) {
  char topic[128];
  topicForNode(topic, sizeof(topic), nodeId, "bindings");

  JsonDocument doc;
  JsonArray arr = doc["bindings"].to<JsonArray>();
  for (size_t i = 0; i < (sizeof(bindRuleTable) / sizeof(bindRuleTable[0])); i++) {
    const BindRuleEntry &rule = bindRuleTable[i];
    if (!rule.inUse || rule.srcNode != nodeId) {
      continue;
    }
    JsonObject obj = arr.add<JsonObject>();
    obj["src_cluster"] = rule.srcCluster;
    obj["src_attr"] = rule.srcAttr;
    obj["dst_node"] = rule.dstNode;
    obj["dst_cluster"] = rule.dstCluster;
    obj["dst_attr"] = rule.dstAttr;
    obj["scale"] = rule.scaleX1000 / 1000.0f;
    obj["offset"] = rule.offset;
  }

  String payload;
  serializeJson(doc, payload);
  publishRetained(topic, payload.c_str());
}

static int32_t applyBindingTransform(int32_t value, int32_t scaleX1000, int32_t offset) {
  const int64_t scaled = (static_cast<int64_t>(value) * scaleX1000) / 1000;
  const int64_t out = scaled + offset;
  if (out > INT32_MAX) {
    return INT32_MAX;
  }
  if (out < INT32_MIN) {
    return INT32_MIN;
  }
  return static_cast<int32_t>(out);
}

static void processBindingTriggers(uint32_t srcNode, uint16_t srcCluster, uint16_t srcAttr, int32_t srcValue) {
  for (size_t i = 0; i < (sizeof(bindRuleTable) / sizeof(bindRuleTable[0])); i++) {
    const BindRuleEntry &rule = bindRuleTable[i];
    if (!rule.inUse) {
      continue;
    }
    if (rule.srcNode != srcNode || rule.srcCluster != srcCluster || rule.srcAttr != srcAttr) {
      continue;
    }
    const int32_t value = applyBindingTransform(srcValue, rule.scaleX1000, rule.offset);
    queueAttrCommand(rule.dstNode, ATTR_CMD_WRITE, rule.dstCluster, rule.dstAttr, value);
  }
}

static bool metricToAttribute(uint8_t metricId, uint16_t &clusterId, uint16_t &attributeId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
      clusterId = CLUSTER_POWER;
      attributeId = ATTR_BATTERY_MV;
      return true;
    case METRIC_TEMPERATURE_C_X100:
      clusterId = CLUSTER_ENVIRONMENT;
      attributeId = ATTR_TEMPERATURE_C_X100;
      return true;
    case METRIC_HUMIDITY_RH_X100:
      clusterId = CLUSTER_ENVIRONMENT;
      attributeId = ATTR_HUMIDITY_RH_X100;
      return true;
    case METRIC_PRESSURE_HPA_X10:
      clusterId = CLUSTER_ENVIRONMENT;
      attributeId = ATTR_PRESSURE_HPA_X10;
      return true;
    case METRIC_DISTANCE_MM:
      clusterId = CLUSTER_WATER_TANK;
      attributeId = ATTR_DISTANCE_MM;
      return true;
    case METRIC_LEVEL_MM:
      clusterId = CLUSTER_WATER_TANK;
      attributeId = ATTR_LEVEL_MM;
      return true;
    case METRIC_WATER_L_X10:
      clusterId = CLUSTER_WATER_TANK;
      attributeId = ATTR_WATER_L_X10;
      return true;
    default:
      return false;
  }
}

static void shortAddrKeyForNode(uint32_t nodeId, char *buffer, size_t len) {
  snprintf(buffer, len, "sa_%08lX", static_cast<unsigned long>(nodeId));
}

static NodeAddressEntry *findNodeAddressEntry(uint32_t nodeId, bool createIfMissing) {
  NodeAddressEntry *freeSlot = nullptr;
  for (size_t i = 0; i < (sizeof(nodeAddressTable) / sizeof(nodeAddressTable[0])); i++) {
    if (nodeAddressTable[i].inUse && nodeAddressTable[i].nodeId == nodeId) {
      return &nodeAddressTable[i];
    }
    if (!nodeAddressTable[i].inUse && freeSlot == nullptr) {
      freeSlot = &nodeAddressTable[i];
    }
  }

  if (createIfMissing && freeSlot != nullptr) {
    freeSlot->inUse = true;
    freeSlot->nodeId = nodeId;
    freeSlot->shortAddr = 0;
    return freeSlot;
  }

  return nullptr;
}

static uint16_t getOrAssignShortAddr(uint32_t nodeId) {
  NodeAddressEntry *entry = findNodeAddressEntry(nodeId, true);
  if (entry == nullptr) {
    return 0;
  }
  if (entry->shortAddr != 0) {
    return entry->shortAddr;
  }

  char key[16];
  shortAddrKeyForNode(nodeId, key, sizeof(key));

  prefs.begin("gateway", false);
  uint16_t shortAddr = prefs.getUShort(key, 0);
  if (shortAddr == 0 || shortAddr == 0xFFFF) {
    uint16_t nextShort = prefs.getUShort("next_sa", 1);
    if (nextShort == 0 || nextShort == 0xFFFF) {
      nextShort = 1;
    }
    shortAddr = nextShort;
    uint16_t after = static_cast<uint16_t>(nextShort + 1);
    if (after == 0 || after == 0xFFFF) {
      after = 1;
    }
    prefs.putUShort(key, shortAddr);
    prefs.putUShort("next_sa", after);
  }
  prefs.end();

  entry->shortAddr = shortAddr;
  return shortAddr;
}

static void publishJoinMqtt(uint32_t nodeId, uint16_t shortAddr, uint8_t deviceType, uint32_t capabilities) {
  char joinTopic[128];
  topicForNode(joinTopic, sizeof(joinTopic), nodeId, "join");

  char payload[256];
  snprintf(payload,
           sizeof(payload),
           "{\"node_id\":%lu,\"short_addr\":%u,\"device_type\":%u,\"capabilities\":%lu,\"network_id\":%u}",
           static_cast<unsigned long>(nodeId),
           shortAddr,
           static_cast<unsigned>(deviceType),
           static_cast<unsigned long>(capabilities),
           static_cast<unsigned>(LORA_NETWORK_ID));
  publishRetained(joinTopic, payload);
}

static void publishInterviewMqtt(const InterviewReportV1 &report, float rssi, float snr) {
  char topic[128];
  topicForNode(topic, sizeof(topic), report.node_id, "interview");

  char payload[320];
  snprintf(payload,
           sizeof(payload),
           "{\"node_id\":%lu,\"short_addr\":%u,\"device_type\":%u,\"metric_bitmap\":%lu,\"interval_sec\":%lu,\"rssi\":%.1f,\"snr\":%.1f}",
           static_cast<unsigned long>(report.node_id),
           report.short_addr,
           static_cast<unsigned>(report.device_type),
           static_cast<unsigned long>(report.metric_bitmap),
           static_cast<unsigned long>(report.current_interval_sec),
           rssi,
           snr);
  publishRetained(topic, payload);
}

static void sendJoinResponse(uint32_t nodeId, uint16_t shortAddr) {
  JoinResponseV1 response{};
  response.proto_ver = 1;
  response.msg_type = MSG_JOIN_RESPONSE;
  response.node_id = nodeId;
  response.short_addr = shortAddr;
  response.network_id = static_cast<uint16_t>(LORA_NETWORK_ID);
  response.status = JOIN_OK;
  finalize_crc_typed(response);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&response), sizeof(response), encrypted, sizeof(encrypted), encryptedLen)) {
    Serial.println("Join response encrypt failed");
    return;
  }

  rxInterruptEnabled = false;
  radio.standby();
  const int state = radio.transmit(encrypted, encryptedLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Join response sent node=%lu short=%u\n", static_cast<unsigned long>(nodeId), shortAddr);
  } else {
    Serial.printf("Join response TX failed state=%d\n", state);
  }
  radio.startReceive();
  rxInterruptEnabled = true;
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

static PendingAttrCommand *findPendingAttr(uint32_t nodeId, bool createIfMissing) {
  PendingAttrCommand *freeSlot = nullptr;
  for (size_t i = 0; i < (sizeof(pendingAttrCommands) / sizeof(pendingAttrCommands[0])); i++) {
    if (pendingAttrCommands[i].inUse && pendingAttrCommands[i].nodeId == nodeId) {
      return &pendingAttrCommands[i];
    }
    if (!pendingAttrCommands[i].inUse && freeSlot == nullptr) {
      freeSlot = &pendingAttrCommands[i];
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

static void queueDownlinkCommand(uint32_t nodeId, uint8_t operation, uint16_t parameterId, int32_t value) {
  PendingDownlink *slot = findPending(nodeId, true);
  if (slot == nullptr) {
    Serial.println("Downlink queue full");
    return;
  }

  slot->packet.proto_ver = 1;
  slot->packet.msg_type = MSG_DOWNLINK_CMD;
  slot->packet.node_id = nodeId;
  slot->packet.target_frame_counter = 0;
  slot->packet.operation = operation;
  slot->packet.parameter_id = parameterId;
  slot->packet.value_i32 = value;
  finalize_packet_crc(slot->packet);

  Serial.printf("Queued downlink node=%lu op=%u param=%u value=%ld\n",
                static_cast<unsigned long>(nodeId),
                operation,
                parameterId,
                static_cast<long>(value));
}

static void queueAttrCommand(uint32_t nodeId, uint8_t commandType, uint16_t clusterId, uint16_t attributeId, int32_t value) {
  PendingAttrCommand *slot = findPendingAttr(nodeId, true);
  if (slot == nullptr) {
    Serial.println("Attr command queue full");
    return;
  }

  slot->packet.proto_ver = 1;
  slot->packet.msg_type = MSG_ATTR_COMMAND;
  slot->packet.node_id = nodeId;
  slot->packet.target_frame_counter = 0;
  slot->packet.command_type = commandType;
  slot->packet.cluster_id = clusterId;
  slot->packet.attribute_id = attributeId;
  slot->packet.value_i32 = value;
  finalize_crc_typed(slot->packet);

  Serial.printf("Queued attr command node=%lu cmd=%u cluster=0x%04X attr=0x%04X value=%ld\n",
                static_cast<unsigned long>(nodeId),
                commandType,
                clusterId,
                attributeId,
                static_cast<long>(value));
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

static bool parseGroupSetTopic(const char *topic, char *groupName, size_t groupNameLen) {
  char prefix[80];
  snprintf(prefix, sizeof(prefix), "%s/group/", config.mqttBaseTopic);
  if (strncmp(topic, prefix, strlen(prefix)) != 0) {
    return false;
  }

  const char *groupStart = topic + strlen(prefix);
  const char *slash = strchr(groupStart, '/');
  if (slash == nullptr) {
    return false;
  }

  if (String(slash) != "/set") {
    return false;
  }

  const String raw = String(groupStart).substring(0, slash - groupStart);
  return normalizeGroupName(raw.c_str(), groupName, groupNameLen);
}

static size_t getGroupTargets(const char *groupName, uint32_t *targets, size_t maxTargets) {
  if (targets == nullptr || maxTargets == 0) {
    return 0;
  }
  GroupEntry *group = findGroupEntry(groupName, false);
  if (group == nullptr || !group->inUse) {
    return 0;
  }
  const size_t count = group->nodeCount < maxTargets ? group->nodeCount : maxTargets;
  for (size_t i = 0; i < count; i++) {
    targets[i] = group->nodes[i];
  }
  return count;
}

static void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  uint32_t nodeId = 0;
  char groupName[24]{};
  const bool isNodeTopic = parseSetTopicNodeId(topic, nodeId);
  const bool isGroupTopic = !isNodeTopic && parseGroupSetTopic(topic, groupName, sizeof(groupName));
  if (!isNodeTopic && !isGroupTopic) {
    return;
  }

  uint32_t targets[24]{};
  size_t targetCount = 0;
  if (isNodeTopic) {
    targets[0] = nodeId;
    targetCount = 1;
  } else {
    targetCount = getGroupTargets(groupName, targets, sizeof(targets) / sizeof(targets[0]));
    if (targetCount == 0) {
      Serial.printf("Group '%s' has no targets\n", groupName);
      return;
    }
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload, length);
  const char *cmd = nullptr;
  int32_t encodedValue = 0;

  if (!err) {
    cmd = doc["cmd"] | "";
  } else {
    String raw;
    for (unsigned int i = 0; i < length; i++) {
      raw += static_cast<char>(payload[i]);
    }
    raw.trim();
    cmd = raw.c_str();
  }

  uint8_t downlinkOp = 0;
  uint16_t downlinkParam = PARAM_NONE;

  if (isNodeTopic && !err && (strcmp(cmd, "group_add") == 0 || strcmp(cmd, "group_remove") == 0 || strcmp(cmd, "group_clear") == 0 ||
                              strcmp(cmd, "group_list") == 0)) {
    if (strcmp(cmd, "group_list") == 0) {
      publishNodeGroupsMqtt(nodeId);
      return;
    }
    if (strcmp(cmd, "group_clear") == 0) {
      clearNodeFromAllGroups(nodeId);
      publishNodeGroupsMqtt(nodeId);
      return;
    }

    char normalized[24]{};
    const char *groupRaw = doc["group"] | "";
    if (!normalizeGroupName(groupRaw, normalized, sizeof(normalized))) {
      Serial.println("Rejected group cmd: invalid group name");
      return;
    }

    bool ok = false;
    if (strcmp(cmd, "group_add") == 0) {
      ok = addNodeToGroup(normalized, nodeId);
    } else {
      ok = removeNodeFromGroup(normalized, nodeId);
    }
    publishNodeGroupsMqtt(nodeId);
    Serial.printf("Group cmd %s node=%lu group=%s result=%s\n",
                  cmd,
                  static_cast<unsigned long>(nodeId),
                  normalized,
                  ok ? "ok" : "fail");
    return;
  }

  if (isNodeTopic && !err && (strcmp(cmd, "bind_attr") == 0 || strcmp(cmd, "unbind_attr") == 0 || strcmp(cmd, "bind_list") == 0)) {
    if (strcmp(cmd, "bind_list") == 0) {
      publishNodeBindingsMqtt(nodeId);
      return;
    }

    BindRuleEntry rule{};
    rule.inUse = true;
    rule.srcNode = nodeId;
    rule.srcCluster = static_cast<uint16_t>(doc["src_cluster"] | doc["cluster"] | 0);
    rule.srcAttr = static_cast<uint16_t>(doc["src_attr"] | doc["attr"] | 0);
    rule.dstNode = static_cast<uint32_t>(doc["dst_node"] | 0);
    rule.dstCluster = static_cast<uint16_t>(doc["dst_cluster"] | 0);
    rule.dstAttr = static_cast<uint16_t>(doc["dst_attr"] | 0);
    const float scale = doc["scale"].isNull() ? 1.0f : doc["scale"].as<float>();
    rule.scaleX1000 = static_cast<int32_t>(scale * 1000.0f + (scale >= 0 ? 0.5f : -0.5f));
    rule.offset = doc["offset"].isNull() ? 0 : doc["offset"].as<int32_t>();

    if (rule.srcCluster == 0 || rule.srcAttr == 0 || rule.dstNode == 0 || rule.dstCluster == 0 || rule.dstAttr == 0) {
      Serial.println("Rejected bind cmd: missing src/dst fields");
      return;
    }

    bool ok = false;
    if (strcmp(cmd, "bind_attr") == 0) {
      ok = addOrUpdateBindRule(rule);
    } else {
      ok = removeBindRule(rule);
    }

    publishNodeBindingsMqtt(nodeId);
    Serial.printf("Bind cmd %s src=%lu src=%u/%u dst=%lu dst=%u/%u result=%s\n",
                  cmd,
                  static_cast<unsigned long>(rule.srcNode),
                  rule.srcCluster,
                  rule.srcAttr,
                  static_cast<unsigned long>(rule.dstNode),
                  rule.dstCluster,
                  rule.dstAttr,
                  ok ? "ok" : "fail");
    return;
  }

  if ((strcmp(cmd, "read_attr") == 0 || strcmp(cmd, "write_attr") == 0) && !err) {
    const uint16_t clusterId = static_cast<uint16_t>(doc["cluster"] | 0);
    const uint16_t attributeId = static_cast<uint16_t>(doc["attr"] | doc["attribute"] | 0);
    if (clusterId == 0 || attributeId == 0) {
      Serial.println("Rejected attr cmd: missing cluster/attr");
      return;
    }

    if (strcmp(cmd, "read_attr") == 0) {
      for (size_t i = 0; i < targetCount; i++) {
        queueAttrCommand(targets[i], ATTR_CMD_READ, clusterId, attributeId, 0);
      }
      return;
    }

    if (doc["value"].isNull()) {
      Serial.println("Rejected write_attr: missing value");
      return;
    }
    const int32_t value = doc["value"].as<int32_t>();
    for (size_t i = 0; i < targetCount; i++) {
      queueAttrCommand(targets[i], ATTR_CMD_WRITE, clusterId, attributeId, value);
    }
    return;
  }

  if (strcmp(cmd, "ping") == 0) {
    downlinkOp = DL_OP_PING;
  } else if (strcmp(cmd, "reboot") == 0) {
    downlinkOp = DL_OP_REBOOT;
  } else if (strcmp(cmd, "enter_ota") == 0) {
    downlinkOp = DL_OP_ENTER_OTA;
    float otaSec = 0.0f;
    if (!err) {
      if (!doc["sec"].isNull()) {
        otaSec = doc["sec"].as<float>();
      } else if (!doc["value"].isNull()) {
        otaSec = doc["value"].as<float>();
      }
    }
    if (otaSec <= 0.0f) {
      otaSec = OTA_DEFAULT_WINDOW_SEC;
    }
    encodedValue = static_cast<int32_t>(otaSec + 0.5f);
  } else {
    const ParameterSchemaV1 *schema = nullptr;
    float userValue = 0.0f;
    bool hasUserValue = false;

    if (strcmp(cmd, "set_param") == 0 || strcmp(cmd, "set_parameter") == 0) {
      if (err) {
        Serial.println("Rejected set_param: invalid JSON");
        return;
      }
      const char *paramKey = doc["param"] | doc["parameter"] | "";
      schema = find_parameter_schema_by_key(paramKey);
      if (schema == nullptr) {
        Serial.printf("Rejected set_param unknown param: %s\n", paramKey);
        return;
      }
      if (!doc["value"].isNull()) {
        userValue = doc["value"].as<float>();
        hasUserValue = true;
      }
    } else if (strcmp(cmd, "set_interval") == 0 || strcmp(cmd, "set_interval_sec") == 0) {
      schema = find_parameter_schema_by_id(PARAM_TX_INTERVAL_SEC);
      if (!err) {
        if (!doc["sec"].isNull()) {
          userValue = doc["sec"].as<float>();
          hasUserValue = true;
        } else if (!doc["value"].isNull()) {
          userValue = doc["value"].as<float>();
          hasUserValue = true;
        }
      }
    } else if (strcmp(cmd, "set_tank_area") == 0 || strcmp(cmd, "set_tank_area_m2") == 0) {
      schema = find_parameter_schema_by_id(PARAM_TANK_AREA_M2_X1000);
      if (!err) {
        if (!doc["area_m2"].isNull()) {
          userValue = doc["area_m2"].as<float>();
          hasUserValue = true;
        } else if (!doc["area"].isNull()) {
          userValue = doc["area"].as<float>();
          hasUserValue = true;
        } else if (!doc["value"].isNull()) {
          userValue = doc["value"].as<float>();
          hasUserValue = true;
        }
      }
    } else if (strcmp(cmd, "set_tank_min") == 0 || strcmp(cmd, "set_tank_min_mm") == 0) {
      schema = find_parameter_schema_by_id(PARAM_TANK_DISTANCE_MIN_MM);
      if (!err) {
        if (!doc["min_mm"].isNull()) {
          userValue = doc["min_mm"].as<float>();
          hasUserValue = true;
        } else if (!doc["value"].isNull()) {
          userValue = doc["value"].as<float>();
          hasUserValue = true;
        }
      }
    } else if (strcmp(cmd, "set_tank_max") == 0 || strcmp(cmd, "set_tank_max_mm") == 0) {
      schema = find_parameter_schema_by_id(PARAM_TANK_DISTANCE_MAX_MM);
      if (!err) {
        if (!doc["max_mm"].isNull()) {
          userValue = doc["max_mm"].as<float>();
          hasUserValue = true;
        } else if (!doc["value"].isNull()) {
          userValue = doc["value"].as<float>();
          hasUserValue = true;
        }
      }
    } else {
      Serial.printf("Unsupported cmd: %s\n", cmd ? cmd : "<null>");
      return;
    }

    if (schema == nullptr || !hasUserValue) {
      Serial.printf("Rejected cmd %s: missing/invalid value\n", cmd ? cmd : "<null>");
      return;
    }

    if (!parameter_value_from_float(schema->id, userValue, encodedValue)) {
      Serial.printf("Rejected cmd %s: out of range value=%.3f\n", cmd ? cmd : "<null>", userValue);
      return;
    }

    downlinkOp = DL_OP_SET_PARAM;
    downlinkParam = schema->id;
  }

  for (size_t i = 0; i < targetCount; i++) {
    queueDownlinkCommand(targets[i], downlinkOp, downlinkParam, encodedValue);
  }
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
    char groupSetWildcard[96];
    snprintf(setWildcard, sizeof(setWildcard), "%s/node/+/set", config.mqttBaseTopic);
    snprintf(groupSetWildcard, sizeof(groupSetWildcard), "%s/group/+/set", config.mqttBaseTopic);
    mqttClient.subscribe(setWildcard);
    mqttClient.subscribe(groupSetWildcard);
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

static void publishDiscoveryBinarySensor(uint32_t nodeId, const char *key, const char *name, const char *deviceClass) {
  char topic[128];
  char payload[768];
  char optionalFields[96];
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/lora_%lu_%s/config", static_cast<unsigned long>(nodeId), key);

  optionalFields[0] = '\0';
  if (strlen(deviceClass) > 0) {
    strncat(optionalFields, "\"device_class\":\"", sizeof(optionalFields) - strlen(optionalFields) - 1);
    strncat(optionalFields, deviceClass, sizeof(optionalFields) - strlen(optionalFields) - 1);
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
      "\"payload_on\":\"true\",\"payload_off\":\"false\","
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

static const char *metricKey(uint8_t metricId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
      return "battery";
    case METRIC_TEMPERATURE_C_X100:
      return "temperature";
    case METRIC_HUMIDITY_RH_X100:
      return "humidity";
    case METRIC_PRESSURE_HPA_X10:
      return "pressure";
    case METRIC_DISTANCE_MM:
      return "distance_cm";
    case METRIC_LEVEL_MM:
      return "level_cm";
    case METRIC_WATER_L_X10:
      return "water_l";
    default:
      return nullptr;
  }
}

static const char *metricName(uint8_t metricId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
      return "Battery";
    case METRIC_TEMPERATURE_C_X100:
      return "Temperature";
    case METRIC_HUMIDITY_RH_X100:
      return "Humidity";
    case METRIC_PRESSURE_HPA_X10:
      return "Pressure";
    case METRIC_DISTANCE_MM:
      return "Distance";
    case METRIC_LEVEL_MM:
      return "Water level";
    case METRIC_WATER_L_X10:
      return "Water volume";
    default:
      return "Unknown";
  }
}

static const char *metricUnit(uint8_t metricId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
      return "V";
    case METRIC_TEMPERATURE_C_X100:
      return "°C";
    case METRIC_HUMIDITY_RH_X100:
      return "%";
    case METRIC_PRESSURE_HPA_X10:
      return "hPa";
    case METRIC_DISTANCE_MM:
    case METRIC_LEVEL_MM:
      return "cm";
    case METRIC_WATER_L_X10:
      return "L";
    default:
      return "";
  }
}

static const char *metricDeviceClass(uint8_t metricId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
      return "voltage";
    case METRIC_TEMPERATURE_C_X100:
      return "temperature";
    case METRIC_HUMIDITY_RH_X100:
      return "humidity";
    case METRIC_PRESSURE_HPA_X10:
      return "pressure";
    case METRIC_DISTANCE_MM:
    case METRIC_LEVEL_MM:
      return "distance";
    case METRIC_WATER_L_X10:
      return "volume_storage";
    default:
      return "";
  }
}

static const char *metricStateClass(uint8_t metricId) {
  switch (metricId) {
    case METRIC_BATTERY_MV:
    case METRIC_TEMPERATURE_C_X100:
    case METRIC_HUMIDITY_RH_X100:
    case METRIC_PRESSURE_HPA_X10:
    case METRIC_DISTANCE_MM:
    case METRIC_LEVEL_MM:
    case METRIC_WATER_L_X10:
      return "measurement";
    default:
      return "";
  }
}

static void publishDiscoveryMetric(uint32_t nodeId, uint8_t metricId) {
  const char *key = metricKey(metricId);
  if (key == nullptr) {
    return;
  }
  publishDiscoverySensor(nodeId, key, metricName(metricId), metricUnit(metricId), metricDeviceClass(metricId),
                         metricStateClass(metricId));
}

static void addMetricToJson(JsonDocument &doc, uint8_t metricId, int32_t value) {
  const char *key = metricKey(metricId);
  if (key == nullptr) {
    return;
  }

  switch (metricId) {
    case METRIC_BATTERY_MV:
      doc[key] = value / 1000.0f;
      break;
    case METRIC_TEMPERATURE_C_X100:
    case METRIC_HUMIDITY_RH_X100:
      doc[key] = value / 100.0f;
      break;
    case METRIC_PRESSURE_HPA_X10:
    case METRIC_DISTANCE_MM:
    case METRIC_LEVEL_MM:
    case METRIC_WATER_L_X10:
      doc[key] = value / 10.0f;
      break;
    default:
      doc[key] = value;
      break;
  }
}

static const char *attributeKey(uint16_t clusterId, uint16_t attributeId) {
  if (clusterId == CLUSTER_POWER && attributeId == ATTR_BATTERY_MV) {
    return "battery";
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_TEMPERATURE_C_X100) {
    return "temperature";
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_HUMIDITY_RH_X100) {
    return "humidity";
  }
  if (clusterId == CLUSTER_ENVIRONMENT && attributeId == ATTR_PRESSURE_HPA_X10) {
    return "pressure";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_DISTANCE_MM) {
    return "distance_cm";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_LEVEL_MM) {
    return "level_cm";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_WATER_L_X10) {
    return "water_l";
  }
  if (clusterId == CLUSTER_SYSTEM && attributeId == ATTR_TX_INTERVAL_SEC) {
    return "tx_interval_sec";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_AREA_M2_X1000) {
    return "tank_area_m2";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_DISTANCE_MIN_MM) {
    return "tank_distance_min_mm";
  }
  if (clusterId == CLUSTER_WATER_TANK && attributeId == ATTR_TANK_DISTANCE_MAX_MM) {
    return "tank_distance_max_mm";
  }
  return nullptr;
}

static void addAttributeToJson(JsonDocument &doc, uint16_t clusterId, uint16_t attributeId, int32_t value) {
  const char *key = attributeKey(clusterId, attributeId);
  if (key == nullptr) {
    return;
  }

  if (attributeId == ATTR_BATTERY_MV) {
    doc[key] = value / 1000.0f;
    return;
  }
  if (attributeId == ATTR_TEMPERATURE_C_X100 || attributeId == ATTR_HUMIDITY_RH_X100) {
    doc[key] = value / 100.0f;
    return;
  }
  if (attributeId == ATTR_PRESSURE_HPA_X10 || attributeId == ATTR_DISTANCE_MM || attributeId == ATTR_LEVEL_MM ||
      attributeId == ATTR_WATER_L_X10) {
    doc[key] = value / 10.0f;
    return;
  }

  if (attributeId == ATTR_TANK_AREA_M2_X1000) {
    doc[key] = value / 1000.0f;
    return;
  }

  doc[key] = value;
}

static void publishAttrReportMqtt(const AttrReportPacketV1 &report, float rssi, float snr) {
  char stateTopic[128];
  char statusTopic[128];
  char availabilityTopic[128];

  topicForNode(stateTopic, sizeof(stateTopic), report.node_id, "state");
  topicForNode(statusTopic, sizeof(statusTopic), report.node_id, "status");
  topicForNode(availabilityTopic, sizeof(availabilityTopic), report.node_id, "availability");

  JsonDocument stateDoc;
  JsonDocument statusDoc;
  stateDoc["short_addr"] = report.short_addr;
  stateDoc["flags"] = report.flags;
  addAttributeToJson(stateDoc, report.cluster_id, report.attribute_id, report.value_i32);

  statusDoc["node_id"] = report.node_id;
  statusDoc["short_addr"] = report.short_addr;
  statusDoc["cluster"] = report.cluster_id;
  statusDoc["attribute"] = report.attribute_id;
  statusDoc["flags"] = report.flags;
  statusDoc["rssi"] = rssi;
  statusDoc["snr"] = snr;
  addAttributeToJson(statusDoc, report.cluster_id, report.attribute_id, report.value_i32);

  String statePayload;
  String statusPayload;
  serializeJson(stateDoc, statePayload);
  serializeJson(statusDoc, statusPayload);

  mqttClient.publish(stateTopic, statePayload.c_str(), false);
  mqttClient.publish(statusTopic, statusPayload.c_str(), false);
  publishRetained(availabilityTopic, "online");

  processBindingTriggers(report.node_id, report.cluster_id, report.attribute_id, report.value_i32);
}

static void publishTelemetryMqtt(const MetricsPacketHeaderV1 &header, const MetricRecordV1 *records, float rssi,
                                 float snr) {
  char stateTopic[128];
  char statusTopic[128];
  char availabilityTopic[128];

  topicForNode(stateTopic, sizeof(stateTopic), header.node_id, "state");
  topicForNode(statusTopic, sizeof(statusTopic), header.node_id, "status");
  topicForNode(availabilityTopic, sizeof(availabilityTopic), header.node_id, "availability");

  publishDiscoverySensor(header.node_id, "rssi", "RSSI", "dBm", "signal_strength", "measurement");
  publishDiscoverySensor(header.node_id, "snr", "SNR", "dB", "", "measurement");
  publishDiscoveryBinarySensor(header.node_id, "low_battery", "Low battery", "battery");

  JsonDocument stateDoc;
  JsonDocument statusDoc;

  const uint16_t shortAddr = getOrAssignShortAddr(header.node_id);
  const bool lowBattery = (header.flags & FLAG_LOW_BATTERY) != 0;
  const bool measurementFail = (header.flags & FLAG_MEASUREMENT_FAIL) != 0;

  stateDoc["fcnt"] = header.frame_counter;
  stateDoc["short_addr"] = shortAddr;
  stateDoc["flags"] = header.flags;
  stateDoc["low_battery"] = lowBattery;
  stateDoc["measurement_ok"] = !measurementFail;
  stateDoc["rssi"] = rssi;
  stateDoc["snr"] = snr;

  statusDoc["proto"] = header.proto_ver;
  statusDoc["node_id"] = header.node_id;
  statusDoc["short_addr"] = shortAddr;
  statusDoc["fcnt"] = header.frame_counter;
  statusDoc["flags"] = header.flags;
  statusDoc["low_battery"] = lowBattery;
  statusDoc["measurement_ok"] = !measurementFail;
  statusDoc["metric_count"] = header.metric_count;
  statusDoc["rssi"] = rssi;
  statusDoc["snr"] = snr;

  for (uint8_t i = 0; i < header.metric_count; i++) {
    publishDiscoveryMetric(header.node_id, records[i].metric_id);
    addMetricToJson(stateDoc, records[i].metric_id, records[i].value);
    addMetricToJson(statusDoc, records[i].metric_id, records[i].value);

    uint16_t clusterId = 0;
    uint16_t attributeId = 0;
    if (metricToAttribute(records[i].metric_id, clusterId, attributeId)) {
      processBindingTriggers(header.node_id, clusterId, attributeId, records[i].value);
    }
  }

  String statePayload;
  String statusPayload;
  serializeJson(stateDoc, statePayload);
  serializeJson(statusDoc, statusPayload);

  mqttClient.publish(stateTopic, statePayload.c_str(), false);
  mqttClient.publish(statusTopic, statusPayload.c_str(), false);
  publishRetained(availabilityTopic, "online");
}

static void publishAckMqtt(const AckPacketV1 &ack, float rssi, float snr) {
  char ackTopic[128];
  char ackPayload[256];
  topicForNode(ackTopic, sizeof(ackTopic), ack.node_id, "ack");
  snprintf(
      ackPayload,
      sizeof(ackPayload),
  "{\"node_id\":%lu,\"acked_fcnt\":%lu,\"op\":%u,\"param\":%u,\"status\":%u,\"interval_sec\":%lu,\"rssi\":%.1f,\"snr\":%.1f}",
      static_cast<unsigned long>(ack.node_id),
      static_cast<unsigned long>(ack.acked_frame_counter),
  ack.acked_operation,
  ack.acked_parameter_id,
      ack.status,
      static_cast<unsigned long>(ack.current_interval_sec),
      rssi,
      snr);
  mqttClient.publish(ackTopic, ackPayload, false);
}

static void maybeSendPendingDownlink(uint32_t nodeId, uint32_t currentFcnt) {
  PendingDownlink *slot = findPending(nodeId, false);
  if (slot == nullptr || !slot->inUse) {
    return;
  }

  slot->packet.target_frame_counter = currentFcnt;
  finalize_packet_crc(slot->packet);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&slot->packet), sizeof(slot->packet), encrypted,
                            sizeof(encrypted), encryptedLen)) {
    Serial.println("Downlink encrypt failed");
    return;
  }

  rxInterruptEnabled = false;
  radio.standby();
  int state = radio.transmit(encrypted, encryptedLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Downlink sent node=%lu op=%u param=%u value=%ld\n",
                  static_cast<unsigned long>(nodeId),
                  slot->packet.operation,
                  slot->packet.parameter_id,
                  static_cast<long>(slot->packet.value_i32));
    slot->inUse = false;
  } else {
    Serial.printf("Downlink TX failed state=%d\n", state);
  }

  radio.startReceive();
  rxInterruptEnabled = true;
}

static void maybeSendPendingAttrCommand(uint32_t nodeId, uint32_t currentFcnt) {
  PendingAttrCommand *slot = findPendingAttr(nodeId, false);
  if (slot == nullptr || !slot->inUse) {
    return;
  }

  slot->packet.target_frame_counter = currentFcnt;
  finalize_crc_typed(slot->packet);

  uint8_t encrypted[SECURE_MAX_FRAME_SIZE]{};
  size_t encryptedLen = 0;
  if (!secure_encrypt_frame(reinterpret_cast<const uint8_t *>(&slot->packet), sizeof(slot->packet), encrypted,
                            sizeof(encrypted), encryptedLen)) {
    Serial.println("Attr command encrypt failed");
    return;
  }

  rxInterruptEnabled = false;
  radio.standby();
  int state = radio.transmit(encrypted, encryptedLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Attr command sent node=%lu cmd=%u cluster=0x%04X attr=0x%04X value=%ld\n",
                  static_cast<unsigned long>(nodeId),
                  slot->packet.command_type,
                  slot->packet.cluster_id,
                  slot->packet.attribute_id,
                  static_cast<long>(slot->packet.value_i32));
    slot->inUse = false;
  } else {
    Serial.printf("Attr command TX failed state=%d\n", state);
  }

  radio.startReceive();
  rxInterruptEnabled = true;
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  loadConfig();
  loadF3State();
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
  if (packetLen == 0 || packetLen > SECURE_MAX_FRAME_SIZE) {
    radio.startReceive();
    rxInterruptEnabled = true;
    return;
  }

  uint8_t bytes[SECURE_MAX_FRAME_SIZE]{};
  int state = radio.readData(bytes, packetLen);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("{\"error\":\"read\",\"state\":%d}\n", state);
    radio.startReceive();
    rxInterruptEnabled = true;
    return;
  }

  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();

  uint8_t plain[64]{};
  size_t plainLen = 0;
  if (!secure_decrypt_frame(bytes, packetLen, plain, sizeof(plain), plainLen)) {
    Serial.println("{\"error\":\"decrypt\"}");
    radio.startReceive();
    rxInterruptEnabled = true;
    return;
  }

  if (plainLen >= 2 && plain[0] == 1 && plain[1] == MSG_TELEMETRY) {
    if (!validate_metrics_packet(plain, plainLen)) {
      Serial.println("{\"error\":\"crc\"}");
    } else {
      const auto *header = reinterpret_cast<const MetricsPacketHeaderV1 *>(plain);
      const auto *records = metrics_packet_records(header);
      Serial.printf("Telemetry node=%lu fcnt=%lu metrics=%u rssi=%.1f snr=%.1f\n",
                    static_cast<unsigned long>(header->node_id),
                    static_cast<unsigned long>(header->frame_counter),
                    header->metric_count,
                    rssi,
                    snr);
      publishTelemetryMqtt(*header, records, rssi, snr);
      maybeSendPendingDownlink(header->node_id, header->frame_counter);
      maybeSendPendingAttrCommand(header->node_id, header->frame_counter);
    }
  } else if (plainLen == sizeof(JoinRequestV1) && plain[0] == 1 && plain[1] == MSG_JOIN_REQUEST) {
    JoinRequestV1 joinReq{};
    memcpy(&joinReq, plain, sizeof(joinReq));
    if (!validate_crc_typed(joinReq)) {
      Serial.println("{\"error\":\"join_crc\"}");
    } else {
      const uint16_t shortAddr = getOrAssignShortAddr(joinReq.node_id);
      markNodeDiscovered(joinReq.node_id);
      publishJoinMqtt(joinReq.node_id, shortAddr, joinReq.device_type, joinReq.capabilities);
      sendJoinResponse(joinReq.node_id, shortAddr);
      Serial.printf("JOIN node=%lu short=%u dev=%u caps=%lu\n",
                    static_cast<unsigned long>(joinReq.node_id),
                    shortAddr,
                    static_cast<unsigned>(joinReq.device_type),
                    static_cast<unsigned long>(joinReq.capabilities));
    }
  } else if (plainLen == sizeof(InterviewReportV1) && plain[0] == 1 && plain[1] == MSG_INTERVIEW_REPORT) {
    InterviewReportV1 report{};
    memcpy(&report, plain, sizeof(report));
    if (!validate_crc_typed(report)) {
      Serial.println("{\"error\":\"interview_crc\"}");
    } else {
      publishInterviewMqtt(report, rssi, snr);
      Serial.printf("INTERVIEW node=%lu short=%u metrics=0x%08lX interval=%lu\n",
                    static_cast<unsigned long>(report.node_id),
                    report.short_addr,
                    static_cast<unsigned long>(report.metric_bitmap),
                    static_cast<unsigned long>(report.current_interval_sec));
    }
  } else if (plainLen == sizeof(AttrReportPacketV1) && plain[0] == 1 && plain[1] == MSG_ATTR_REPORT) {
    AttrReportPacketV1 report{};
    memcpy(&report, plain, sizeof(report));
    if (!validate_crc_typed(report)) {
      Serial.println("{\"error\":\"attr_crc\"}");
    } else {
      publishAttrReportMqtt(report, rssi, snr);
      Serial.printf("ATTR_REPORT node=%lu short=%u cluster=0x%04X attr=0x%04X value=%ld\n",
                    static_cast<unsigned long>(report.node_id),
                    report.short_addr,
                    report.cluster_id,
                    report.attribute_id,
                    static_cast<long>(report.value_i32));
    }
  } else if (plainLen >= 2 && plain[0] == 1 && plain[1] == MSG_ACK && plainLen == sizeof(AckPacketV1)) {
    AckPacketV1 ack{};
    memcpy(&ack, plain, sizeof(ack));
    if (!validate_packet_crc(ack)) {
      Serial.println("{\"error\":\"ack_crc\"}");
    } else {
      Serial.printf("ACK node=%lu op=%u param=%u status=%u interval=%lu\n",
                    static_cast<unsigned long>(ack.node_id),
                    ack.acked_operation,
            ack.acked_parameter_id,
                    ack.status,
                    static_cast<unsigned long>(ack.current_interval_sec));
      publishAckMqtt(ack, rssi, snr);
    }
  } else {
    Serial.printf("Unknown packet len=%u msg_type=%u\n", static_cast<unsigned>(plainLen), plain[1]);
  }

  radio.startReceive();
  rxInterruptEnabled = true;
}
