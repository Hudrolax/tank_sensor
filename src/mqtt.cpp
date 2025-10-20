#include "mqtt.h"
#include "config.h"
#include "hardware.h"
#include "sensors.h"
#include "relay.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient   s_client;
static PubSubClient s_mqtt(s_client);
static bool         s_online = false;

// ----------------- helpers -----------------
static String macStr() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[18];
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(buf);
}

// topics
static String topicBase()          { return String(cfg.base_topic); }
static String topicAvail()         { return topicBase() + "/status"; }
static String topicLevelState()    { return topicBase() + "/level/state"; }
static String topicErrorState()    { return topicBase() + "/error/state"; }
static String topicRelayState()    { return topicBase() + "/relay/state"; }
static String topicRelaySet()      { return topicBase() + "/relay/set"; }
static String topicModeState()     { return topicBase() + "/mode/state"; }
static String topicModeSet()       { return topicBase() + "/mode/set"; }
static String topicAttr()          { return topicBase() + "/attributes"; }
static String topicIp()            { return topicBase() + "/ip"; }

// discovery
static String discTopicLevel()     { return "homeassistant/sensor/"        + String(cfg.device_name) + "/level/config"; }
static String discTopicError()     { return "homeassistant/binary_sensor/" + String(cfg.device_name) + "/error/config"; }
static String discTopicRelay()     { return "homeassistant/switch/"        + String(cfg.device_name) + "/pump/config"; }
static String discTopicMode()      { return "homeassistant/select/"        + String(cfg.device_name) + "/mode/config"; }
static String discTopicIP()        { return "homeassistant/sensor/"        + String(cfg.device_name) + "/ip/config"; }

static void addDeviceObject(JsonObject dev) {
  dev["ids"]  = String(cfg.device_name);
  dev["name"] = String(cfg.device_name);
  dev["mdl"]  = "NodeMCU-ESP8266";
  dev["mf"]   = "DIY";
  dev["sw"]   = "3.2.0";
  JsonArray conns = dev.createNestedArray("cns");
  JsonArray mac   = conns.createNestedArray();
  mac.add("mac"); mac.add(macStr());
}

static void sendDiscovery() {
  // level
  {
    DynamicJsonDocument d(1024);
    d["name"]         = String(cfg.device_name) + " Level";
    d["uniq_id"]      = String(cfg.device_name) + "-level";
    d["stat_t"]       = topicLevelState();
    d["avty_t"]       = topicAvail();
    d["unit_of_meas"] = "%";
    d["icon"]         = "mdi:water-percent";
    d["state_class"]  = "measurement";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    s_mqtt.publish(discTopicLevel().c_str(), payload.c_str(), true);
  }
  // error
  {
    DynamicJsonDocument d(1024);
    d["name"]    = String(cfg.device_name) + " Error";
    d["uniq_id"] = String(cfg.device_name) + "-error";
    d["stat_t"]  = topicErrorState();
    d["avty_t"]  = topicAvail();
    d["pl_on"]   = "ON";
    d["pl_off"]  = "OFF";
    d["dev_cla"] = "problem";
    d["icon"]    = "mdi:alert-circle";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    s_mqtt.publish(discTopicError().c_str(), payload.c_str(), true);
  }
  // relay
  {
    DynamicJsonDocument d(1024);
    d["name"]     = String(cfg.device_name) + " Pump";
    d["uniq_id"]  = String(cfg.device_name) + "-pump";
    d["stat_t"]   = topicRelayState();
    d["cmd_t"]    = topicRelaySet();
    d["avty_t"]   = topicAvail();
    d["pl_on"]    = "ON";
    d["pl_off"]   = "OFF";
    d["stat_on"]  = "ON";
    d["stat_off"] = "OFF";
    d["icon"]     = "mdi:pump";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    s_mqtt.publish(discTopicRelay().c_str(), payload.c_str(), true);
  }
  // mode
  {
    DynamicJsonDocument d(1024);
    d["name"]    = String(cfg.device_name) + " Mode";
    d["uniq_id"] = String(cfg.device_name) + "-mode";
    d["stat_t"]  = topicModeState();
    d["cmd_t"]   = topicModeSet();
    d["avty_t"]  = topicAvail();
    JsonArray options = d.createNestedArray("options");
    options.add("auto"); options.add("external");
    d["icon"]    = "mdi:automation";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    s_mqtt.publish(discTopicMode().c_str(), payload.c_str(), true);
  }
  // ip
  {
    DynamicJsonDocument d(1024);
    d["name"]    = String(cfg.device_name) + " IP";
    d["uniq_id"] = String(cfg.device_name) + "-ip";
    d["stat_t"]  = topicIp();
    d["avty_t"]  = topicAvail();
    d["icon"]    = "mdi:ip-network";
    d["ent_cat"] = "diagnostic";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    s_mqtt.publish(discTopicIP().c_str(), payload.c_str(), true);
  }
}

// retained publications
static void publishAvailability() { s_mqtt.publish(topicAvail().c_str(), "online", true); }
static void publishMode()         { s_mqtt.publish(topicModeState().c_str(), (cfg.mode==MODE_EXTERNAL) ? "external" : "auto", true); }
static void publishLevel(int v)   { String s = String(v); s_mqtt.publish(topicLevelState().c_str(), s.c_str(), true); }
static void publishError(bool e)  { s_mqtt.publish(topicErrorState().c_str(), e ? "ON" : "OFF", true); }
static void publishRelay(bool on) { s_mqtt.publish(topicRelayState().c_str(), on ? "ON" : "OFF", true); }
static void publishIp()           { String ip = WiFi.localIP().toString(); s_mqtt.publish(topicIp().c_str(), ip.c_str(), true); }
static void publishAttr_payload(const String& payload) {
  s_mqtt.publish(topicAttr().c_str(), payload.c_str(), true);
}

// атрибуты: формируем payload БЕЗ uptime, чтобы дифф не триггерился каждую секунду
static String buildAttrPayload() {
  StaticJsonDocument<256> attr;
  attr["sample_ms"]      = cfg.sample_ms;
  attr["confirm_needed"] = cfg.confirm_samples;
  attr["mode"]           = (cfg.mode==MODE_EXTERNAL) ? "external" : "auto";
  attr["rssi"]           = WiFi.RSSI();
  attr["s50"]            = sensors_s50();
  attr["s100"]           = sensors_s100();
  attr["error"]          = sensors_error();
  String payload; serializeJson(attr, payload);
  return payload;
}

// cache для дифф-публикации
static bool   cache_init = false;
static int    last_level = -1;
static bool   last_error = false;
static bool   last_relay = false;
static String last_mode;
static String last_ip;
static String last_attr;
static uint32_t last_attr_pub_ms = 0;

static void ensure_cache_init() {
  if (cache_init) return;
  last_level = sensors_level();
  last_error = sensors_error();
  last_relay = relay_get();
  last_mode  = (cfg.mode==MODE_EXTERNAL) ? "external" : "auto";
  last_ip    = WiFi.localIP().toString();
  last_attr  = buildAttrPayload();
  last_attr_pub_ms = millis();
  cache_init = true;
}

// ----------------- API -----------------
void mqtt_publish_all() {
  if (!s_online) return;
  publishAvailability();
  sendDiscovery();
  publishMode();
  publishIp();
  publishLevel(sensors_level());
  publishError(sensors_error());
  publishRelay(relay_get());
  // атрибуты единоразово
  String p = buildAttrPayload();
  publishAttr_payload(p);
  last_attr = p;
  last_attr_pub_ms = millis();
}

void mqtt_publish_diff() {
  if (!s_online) return;
  ensure_cache_init();

  bool changed = false;

  // level
  int lvl = sensors_level();
  if (lvl != last_level) {
    publishLevel(lvl);
    last_level = lvl;
    changed = true;
  }

  // error
  bool err = sensors_error();
  if (err != last_error) {
    publishError(err);
    last_error = err;
    changed = true;
  }

  // relay
  bool rel = relay_get();
  if (rel != last_relay) {
    publishRelay(rel);
    last_relay = rel;
    changed = true;
  }

  // mode — может измениться через /settings или MQTT командой
  String mode = (cfg.mode==MODE_EXTERNAL) ? "external" : "auto";
  if (mode != last_mode) {
    publishMode();
    last_mode = mode;
    changed = true;
  }

  // ip — публикуем только при смене
  String ip = WiFi.localIP().toString();
  if (ip != last_ip) {
    publishIp();
    last_ip = ip;
  }

  // attributes — при значимых изменениях сразу, иначе heartbeat раз в 5 минут
  const uint32_t ATTR_HEARTBEAT_MS = 300000; // 5 минут
  uint32_t now = millis();

  if (changed) {
    String p = buildAttrPayload();
    if (p != last_attr) {
      publishAttr_payload(p);
      last_attr = p;
      last_attr_pub_ms = now;
    }
  } else if ((int32_t)(now - last_attr_pub_ms) >= (int32_t)ATTR_HEARTBEAT_MS) {
    String p = buildAttrPayload();
    if (p != last_attr) {
      publishAttr_payload(p);
      last_attr = p;
    } else {
      // даже если не изменилось — дернем по таймеру, чтобы у клиентов был “живой” retained с новым timestamp брокера
      publishAttr_payload(p);
    }
    last_attr_pub_ms = now;
  }
}

static void onMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg; msg.reserve(length+1);
  for (unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim(); msg.toLowerCase();

  if (t == topicRelaySet()) {
    bool want_on = (msg=="on" || msg=="1" || msg=="true");
    relay_set(want_on);
    publishRelay(want_on);
    // обновим атрибуты
    String p = buildAttrPayload();
    publishAttr_payload(p);
    last_attr = p;
    last_attr_pub_ms = millis();
  } else if (t == topicModeSet()) {
    if (msg=="external") cfg.mode = MODE_EXTERNAL;
    else                 cfg.mode = MODE_AUTO;
    saveConfig();
    publishMode();
    // и атрибуты
    String p = buildAttrPayload();
    publishAttr_payload(p);
    last_attr = p;
    last_attr_pub_ms = millis();
  }
}

void mqtt_init() {
  s_mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  s_mqtt.setCallback(onMessage);
}

bool mqtt_online() { return s_online; }

void mqtt_loop() {
  if (cfg.mqtt_host[0] == '\0') { s_online = false; return; }

  if (s_mqtt.connected()) {
    s_online = true;
    s_mqtt.loop();
    return;
  }

  s_online = false;

  static unsigned long lastTry = 0;
  const unsigned long RETRY_MS = 3000;
  unsigned long now = millis();
  if (now - lastTry < RETRY_MS) return;
  lastTry = now;

  String clientId = String(cfg.device_name) + "-" + String(ESP.getChipId(), HEX);
  bool ok = s_mqtt.connect(clientId.c_str(),
                           cfg.mqtt_user[0] ? cfg.mqtt_user : nullptr,
                           cfg.mqtt_user[0] ? cfg.mqtt_pass : nullptr,
                           topicAvail().c_str(), 0, true, "offline");
  if (ok) {
    s_online = true;
    publishAvailability();
    sendDiscovery();
    publishMode();
    s_mqtt.subscribe(topicRelaySet().c_str());
    s_mqtt.subscribe(topicModeSet().c_str());
    publishIp();
    // первичный пакет стейтов
    publishLevel(sensors_level());
    publishError(sensors_error());
    publishRelay(relay_get());
    // и атрибуты единожды
    String p = buildAttrPayload();
    publishAttr_payload(p);
    last_attr = p;
    last_attr_pub_ms = millis();
    // сброс/инициализация кэша (на случай реконнекта)
    cache_init = false;
    ensure_cache_init();
  }
}

void mqtt_reannounce() {
  if (!s_online) return;
  sendDiscovery();
  mqtt_publish_all();
}
