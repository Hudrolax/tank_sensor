#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          // Wi-Fi captive portal
#include <ESP8266WebServer.h>     // Веб-страница статуса
#include <PubSubClient.h>         // MQTT
#include <ArduinoJson.h>          // JSON (config + discovery)
#include <LittleFS.h>             // /config.json
#include <ESP8266mDNS.h>

// =================== Аппаратные пины (жёстко) ===================
// NodeMCU v2: D1=GPIO5, D2=GPIO4
const uint8_t PIN_SENSOR = D1;     // вход от датчика уровня (LOW = бак ПОЛНЫЙ)
const uint8_t PIN_RELAY  = D2;     // выход на реле насоса (HIGH = включить насос)
const uint8_t LED_PIN    = LED_BUILTIN; // встроенный LED (на ESP8266 активен по LOW)

const int FACTORY_PIN = D7;                 // GPIO13 — держать LOW ~5 c при старте для сброса
const unsigned long FACTORY_HOLD_MS = 5000;

// =================== Конфиг ===================
enum ControlMode : uint8_t { MODE_AUTO = 0, MODE_EXTERNAL = 1 };

struct Config {
  char     device_name[32] = "tank-sensor";
  char     base_topic[64]  = "home/tank";
  char     mqtt_host[64]   = "";        // пусто => MQTT не используем
  uint16_t mqtt_port       = 1883;
  char     mqtt_user[32]   = "";
  char     mqtt_pass[32]   = "";

  // Параметры дискретизации/подтверждения
  uint32_t sample_ms       = 50;        // период опроса (мс)
  uint8_t  confirm_samples = 3;         // подряд одинаковых для фиксации состояния

  // Режим работы
  ControlMode mode         = MODE_AUTO; // AUTO — автономная логика; EXTERNAL — только внешнее управление
} cfg;

const char* CFG_PATH = "/config.json";

// =================== Утилиты ===================
String macStr() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[18];
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(buf);
}

bool loadConfig() {
  LittleFS.begin();
  if (!LittleFS.exists(CFG_PATH)) return false;
  File f = LittleFS.open(CFG_PATH, "r"); if (!f) return false;
  DynamicJsonDocument d(2048);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();

  strlcpy(cfg.device_name, d["device_name"] | cfg.device_name, sizeof(cfg.device_name));
  strlcpy(cfg.base_topic,  d["base_topic"]  | cfg.base_topic,  sizeof(cfg.base_topic));
  strlcpy(cfg.mqtt_host,   d["mqtt_host"]   | cfg.mqtt_host,   sizeof(cfg.mqtt_host));
  cfg.mqtt_port = d["mqtt_port"] | cfg.mqtt_port;
  strlcpy(cfg.mqtt_user, d["mqtt_user"] | cfg.mqtt_user, sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass, d["mqtt_pass"] | cfg.mqtt_pass, sizeof(cfg.mqtt_pass));

  cfg.sample_ms       = d["sample_ms"]       | cfg.sample_ms;
  cfg.confirm_samples = d["confirm_samples"] | cfg.confirm_samples;

  const char* mode_s  = d["mode"] | "auto";
  cfg.mode = (strcmp(mode_s, "external") == 0) ? MODE_EXTERNAL : MODE_AUTO;
  return true;
}

bool saveConfig() {
  DynamicJsonDocument d(2048);
  d["device_name"]     = cfg.device_name;
  d["base_topic"]      = cfg.base_topic;
  d["mqtt_host"]       = cfg.mqtt_host;
  d["mqtt_port"]       = cfg.mqtt_port;
  d["mqtt_user"]       = cfg.mqtt_user;
  d["mqtt_pass"]       = cfg.mqtt_pass;
  d["sample_ms"]       = cfg.sample_ms;
  d["confirm_samples"] = cfg.confirm_samples;
  d["mode"]            = (cfg.mode == MODE_EXTERNAL) ? "external" : "auto";
  File f = LittleFS.open(CFG_PATH, "w"); if (!f) return false;
  serializeJsonPretty(d, f); f.close(); return true;
}

// =================== MQTT ===================
WiFiClient   espClient;
PubSubClient mqtt(espClient);
bool         mqtt_online = false;

String topicBase()          { return String(cfg.base_topic); }
String topicAvail()         { return topicBase() + "/status"; }        // online/offline
String topicSensorState()   { return topicBase() + "/sensor/state"; }  // ON (full) / OFF (not full)
String topicRelayState()    { return topicBase() + "/relay/state"; }   // ON / OFF
String topicRelaySet()      { return topicBase() + "/relay/set"; }     // ON / OFF
String topicModeState()     { return topicBase() + "/mode/state"; }    // "auto" / "external"
String topicModeSet()       { return topicBase() + "/mode/set"; }      // "auto" / "external"
String topicAttr()          { return topicBase() + "/attributes"; }    // JSON телеметрия
String topicIp()            { return topicBase() + "/ip"; }

String discTopicBinary()    { return "homeassistant/binary_sensor/" + String(cfg.device_name) + "/tank/config"; }
String discTopicRelay()     { return "homeassistant/switch/"        + String(cfg.device_name) + "/pump/config"; }
String discTopicMode()      { return "homeassistant/select/"        + String(cfg.device_name) + "/mode/config"; }
String discTopicIP()        { return "homeassistant/sensor/"        + String(cfg.device_name) + "/ip/config"; }

void addDeviceObject(JsonObject dev) {
  dev["ids"]  = String(cfg.device_name);
  dev["name"] = String(cfg.device_name);
  dev["mdl"]  = "NodeMCU-ESP8266";
  dev["mf"]   = "DIY";
  dev["sw"]   = "3.0.0";
  JsonArray conns = dev.createNestedArray("cns");  // HA: "connections"
  JsonArray mac   = conns.createNestedArray();
  mac.add("mac"); mac.add(macStr());
}

void mqttSendDiscovery() {
  // binary_sensor — «бак полный»
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Tank";
    d["uniq_id"]     = String(cfg.device_name) + "-tank";
    d["stat_t"]      = topicSensorState();
    d["avty_t"]      = topicAvail();
    d["pl_on"]       = "ON";
    d["pl_off"]      = "OFF";
    d["icon"]        = "mdi:water-check";
    // d["dev_cla"]   = "moisture"; // опционально
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicBinary().c_str(), payload.c_str(), true);
  }
  // switch — насос
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Pump";
    d["uniq_id"]     = String(cfg.device_name) + "-pump";
    d["stat_t"]      = topicRelayState();
    d["cmd_t"]       = topicRelaySet();
    d["avty_t"]      = topicAvail();
    d["pl_on"]       = "ON";
    d["pl_off"]      = "OFF";
    d["stat_on"]     = "ON";
    d["stat_off"]    = "OFF";
    d["icon"]        = "mdi:pump";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicRelay().c_str(), payload.c_str(), true);
  }
  // select — режим (AUTO / EXTERNAL)
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Mode";
    d["uniq_id"]     = String(cfg.device_name) + "-mode";
    d["stat_t"]      = topicModeState();
    d["cmd_t"]       = topicModeSet();
    d["avty_t"]      = topicAvail();
    JsonArray opts = d.createNestedArray("ops"); // "options" (короткий ключ)
    opts.add("auto");
    opts.add("external");
    d["icon"]        = "mdi:automation";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicMode().c_str(), payload.c_str(), true);
  }
  // sensor — IP
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " IP";
    d["uniq_id"]     = String(cfg.device_name) + "-ip";
    d["stat_t"]      = topicIp();
    d["avty_t"]      = topicAvail();
    d["icon"]        = "mdi:ip-network";
    d["ent_cat"]     = "diagnostic";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicIP().c_str(), payload.c_str(), true);
  }
}

void mqttPublishAvailability() {
  mqtt.publish(topicAvail().c_str(), "online", true);
}

void mqttPublishMode() {
  const char* m = (cfg.mode == MODE_EXTERNAL) ? "external" : "auto";
  mqtt.publish(topicModeState().c_str(), m, true);
}

void mqttPublishSensor(bool full) {
  mqtt.publish(topicSensorState().c_str(), full ? "ON" : "OFF", true);
}

void mqttPublishRelay(bool on) {
  mqtt.publish(topicRelayState().c_str(), on ? "ON" : "OFF", true);
}

void mqttPublishIpRetained() {
  String ip = WiFi.localIP().toString();
  mqtt.publish(topicIp().c_str(), ip.c_str(), true);
}

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg; msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim(); msg.toLowerCase();

  if (t == topicRelaySet()) {
    bool want_on = (msg == "on" || msg == "1" || msg == "true");
    // В EXTERNAL — просто применяем; в AUTO — применим, но алгоритм сразу может переиграть по датчику
    digitalWrite(PIN_RELAY, want_on ? HIGH : LOW);
    mqttPublishRelay(want_on);
  }
  else if (t == topicModeSet()) {
    if (msg == "external") cfg.mode = MODE_EXTERNAL;
    else                   cfg.mode = MODE_AUTO;
    saveConfig();
    mqttPublishMode();
  }
}

void mqttOnConnectedOnce() {
  mqttPublishAvailability();
  mqttSendDiscovery();
  mqttPublishMode();

  mqtt.subscribe(topicRelaySet().c_str());
  mqtt.subscribe(topicModeSet().c_str());

  mqttPublishIpRetained();
}

void mqttPump() {
  mqtt.loop();
  if (cfg.mqtt_host[0] == '\0') { mqtt_online = false; return; } // MQTT не настроен
  static unsigned long lastTry = 0;
  const unsigned long RETRY_MS = 3000;
  if (mqtt.connected()) { mqtt_online = true; return; }
  mqtt_online = false;
  unsigned long now = millis();
  if (now - lastTry < RETRY_MS) return;
  lastTry = now;

  mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  mqtt.setCallback(handleMqttMessage);

  String clientId = String(cfg.device_name) + "-" + String(ESP.getChipId(), HEX);
  bool ok = mqtt.connect(clientId.c_str(),
                         cfg.mqtt_user[0] ? cfg.mqtt_user : nullptr,
                         cfg.mqtt_user[0] ? cfg.mqtt_pass : nullptr,
                         topicAvail().c_str(), 0, true, "offline");
  if (ok) { mqtt_online = true; mqttOnConnectedOnce(); }
}

// =================== Веб-интерфейс (только статус) ===================
ESP8266WebServer www(80);

bool g_sensor_full = false;  // состояние датчика (true = полный)
bool g_relay_on    = false;  // состояние реле

String htmlHeader(const char* title) {
  String s = F("<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  s += "<title>"; s += title; s += "</title>";
  s += F("<style>body{font-family:system-ui,Arial;margin:2rem}code{background:#eee;padding:.1rem .3rem;border-radius:.3rem}</style>");
  return s;
}

void handleRoot() {
  String s = htmlHeader("Tank Controller");
  s += F("<h2>Tank Controller</h2>");
  s += "<p>Sensor: <b>" + String(g_sensor_full ? "FULL (ON)" : "NOT FULL (OFF)") + "</b></p>";
  s += "<p>Relay: <b>" + String(g_relay_on ? "ON" : "OFF") + "</b></p>";
  s += "<p>Mode: <b>" + String((cfg.mode==MODE_EXTERNAL) ? "external" : "auto") + "</b></p>";
  s += "<p>Wi-Fi IP: <b>" + WiFi.localIP().toString() + "</b>, RSSI " + String(WiFi.RSSI()) + " dBm</p>";
  s += "<p>MQTT: " + String(mqtt_online ? "connected" : "disconnected") + "</p>";
  s += F("<p><a href='/reannounce'>Re-announce MQTT Discovery</a> | <a href='/reboot'>Reboot</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}

void handleReannounce() {
  mqttPump();
  if (mqtt_online) { mqttSendDiscovery(); www.send(200, "text/plain", "Discovery re-announced"); }
  else             { www.send(503, "text/plain", "MQTT not connected"); }
}

void handleReboot() {
  www.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

// =================== Сброс к заводским ===================
void factoryReset() {
  for (int i=0;i<6;i++){ digitalWrite(LED_PIN, LOW); delay(150); digitalWrite(LED_PIN, HIGH); delay(150); }
  LittleFS.begin(); LittleFS.remove(CFG_PATH);
  WiFi.persistent(true); WiFi.disconnect(true); delay(200); WiFi.persistent(false);
  WiFiManager wm; wm.resetSettings();
  delay(300); ESP.restart();
}

// =================== Низкоуровневые функции управления ===================
void setRelay(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  g_relay_on = on;
  if (mqtt_online) mqttPublishRelay(on);
}

bool readSensorRaw() {
  // Сигнал с датчика: HIGH = бак ПОЛНЫЙ
  return digitalRead(PIN_SENSOR) == LOW;
}

// =================== SETUP ===================
void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH); // HIGH = выкл (активен по LOW)
  pinMode(FACTORY_PIN, INPUT_PULLUP);

  // Окно удержания для заводского сброса
  if (digitalRead(FACTORY_PIN) == LOW) {
    unsigned long t0 = millis(); bool stillLow = true;
    while (millis() - t0 < FACTORY_HOLD_MS) {
      if (digitalRead(FACTORY_PIN) != LOW) { stillLow = false; break; }
      digitalWrite(LED_PIN, (millis()/200)%2 ? LOW : HIGH);
      delay(10);
    }
    digitalWrite(LED_PIN, HIGH);
    if (stillLow) factoryReset();
  }

  Serial.begin(115200); delay(100);
  LittleFS.begin(); loadConfig();

  // Пины датчика и реле
  pinMode(PIN_SENSOR, INPUT_PULLUP); // датчик — вход (внешний источник уровня)
  pinMode(PIN_RELAY,  OUTPUT);     // реле — выход
  setRelay(false);                 // по умолчанию насос выкл

  // Wi-Fi
  WiFiManager wm;
  char apName[32]; sprintf(apName, "Tank-%06X", ESP.getChipId() & 0xFFFFFF);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  wm.autoConnect(apName);

  // mDNS
  if (MDNS.begin(cfg.device_name)) MDNS.addService("http", "tcp", 80);

  // Web
  www.on("/",        handleRoot);
  www.on("/reannounce", HTTP_GET, handleReannounce);
  www.on("/reboot",     HTTP_GET, handleReboot);
  www.begin();

  // MQTT сразу сформируем стартовые публикации (как только подключимся)
}

// =================== LOOP ===================
void loop() {
  static uint32_t t_next = 0;

  // сервисы
  www.handleClient();
  mqttPump();

  // дискретизация по сетке
  uint32_t now = millis();
  if (t_next == 0) t_next = now;
  if ((int32_t)(now - t_next) < 0) { delay(1); return; }
  t_next += cfg.sample_ms;

  // Читаем датчик и антидребезг (N подряд)
  bool raw = readSensorRaw();          // true = FULL
  static bool     first_sample = true;
  static bool     sensor_full  = false;
  static uint8_t  cand_count   = 0;
  static bool     cand_val     = false;

  if (first_sample) {
    sensor_full  = raw;
    first_sample = false;
    // Первый коннект к MQTT — опубликовать всё
    if (mqtt_online) {
      mqttPublishAvailability();
      mqttSendDiscovery();
      mqttPublishMode();
      mqttPublishIpRetained();
      mqttPublishSensor(sensor_full);
      mqttPublishRelay(g_relay_on);
    }
  } else {
    if (raw != sensor_full) {
      if (cand_count == 0) cand_val = raw;
      if (raw == cand_val) {
        cand_count++;
        if (cand_count >= cfg.confirm_samples) {
          sensor_full = raw;
          cand_count = 0;
        }
      } else {
        cand_val = raw;
        cand_count = 1;
      }
    } else {
      cand_count = 0;
    }
  }

  // LED: FULL => горит (LOW включает)
  digitalWrite(LED_PIN, sensor_full ? LOW : HIGH);
  g_sensor_full = sensor_full;

  // Управление насосом по режиму
  if (cfg.mode == MODE_AUTO) {
    // Автологика:
    // - если бак ПОЛНЫЙ => насос ВЫКЛ
    // - если НЕ ПОЛНЫЙ => насос ВКЛ
    bool want_on = !sensor_full;
    if (want_on != g_relay_on) setRelay(want_on);
  } else {
    // EXTERNAL: ничего не делаем, состояние реле задаёт внешний MQTT /relay/set
    // (без автопереигрывания по датчику)
  }

  // Публикации MQTT при изменениях
  if (mqtt_online) {
    static bool last_pub_sensor = !sensor_full;
    if (sensor_full != last_pub_sensor) {
      mqttPublishSensor(sensor_full);
      last_pub_sensor = sensor_full;
    }

    static bool last_pub_relay = !g_relay_on;
    if (g_relay_on != last_pub_relay) {
      mqttPublishRelay(g_relay_on);
      last_pub_relay = g_relay_on;
    }

    // IP при смене
    static String last_ip;
    String cur_ip = WiFi.localIP().toString();
    if (cur_ip != last_ip) { last_ip = cur_ip; mqttPublishIpRetained(); }

    // Доп. телеметрия
    StaticJsonDocument<256> attr;
    attr["sample_ms"]       = cfg.sample_ms;
    attr["confirm_needed"]  = cfg.confirm_samples;
    attr["mode"]            = (cfg.mode == MODE_EXTERNAL) ? "external" : "auto";
    attr["uptime_s"]        = (uint32_t)(millis()/1000);
    attr["rssi"]            = WiFi.RSSI();
    String payload; serializeJson(attr, payload);
    mqtt.publish(topicAttr().c_str(), payload.c_str(), false);
  }

  // Лог
  Serial.printf("sensor_raw=%d full=%d relay=%d mode=%s\n",
                (int)raw, (int)sensor_full, (int)g_relay_on,
                (cfg.mode==MODE_EXTERNAL) ? "EXTERNAL" : "AUTO");
}
