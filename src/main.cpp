#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          // Wi-Fi captive portal
#include <ESP8266WebServer.h>     // Веб-страница статуса
#include <PubSubClient.h>         // MQTT
#include <ArduinoJson.h>          // JSON (config + discovery)
#include <LittleFS.h>             // /config.json
#include <ESP8266mDNS.h>

// =================== Аппаратные пины (жёстко) ===================
// NodeMCU v2: D1=GPIO5 (100%), D2=GPIO4 (реле), D5=GPIO14 (50%)
const uint8_t PIN_SENSOR100 = D1;     // датчик 100% (HIGH = бак ПОЛНЫЙ)
const uint8_t PIN_SENSOR50  = D5;     // датчик  50% (HIGH = бак >= ~50%)
const uint8_t PIN_RELAY     = D2;     // выход на реле насоса (HIGH = включить насос)
const uint8_t LED_PIN       = LED_BUILTIN; // встроенный LED (активен по LOW)

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

// Топики
String topicBase()          { return String(cfg.base_topic); }
String topicAvail()         { return topicBase() + "/status"; }          // online/offline
String topicLevelState()    { return topicBase() + "/level/state"; }     // 0 / 50 / 100
String topicErrorState()    { return topicBase() + "/error/state"; }     // ON / OFF (расхождение)
String topicRelayState()    { return topicBase() + "/relay/state"; }     // ON / OFF
String topicRelaySet()      { return topicBase() + "/relay/set"; }       // ON / OFF
String topicModeState()     { return topicBase() + "/mode/state"; }      // "auto" / "external"
String topicModeSet()       { return topicBase() + "/mode/set"; }        // "auto" / "external"
String topicAttr()          { return topicBase() + "/attributes"; }      // JSON телеметрия
String topicIp()            { return topicBase() + "/ip"; }

// Discovery
String discTopicLevel()     { return "homeassistant/sensor/"         + String(cfg.device_name) + "/level/config"; }
String discTopicError()     { return "homeassistant/binary_sensor/"  + String(cfg.device_name) + "/error/config"; }
String discTopicRelay()     { return "homeassistant/switch/"         + String(cfg.device_name) + "/pump/config"; }
String discTopicMode()      { return "homeassistant/select/"         + String(cfg.device_name) + "/mode/config"; }
String discTopicIP()        { return "homeassistant/sensor/"         + String(cfg.device_name) + "/ip/config"; }

void addDeviceObject(JsonObject dev) {
  dev["ids"]  = String(cfg.device_name);
  dev["name"] = String(cfg.device_name);
  dev["mdl"]  = "NodeMCU-ESP8266";
  dev["mf"]   = "DIY";
  dev["sw"]   = "3.2.0";
  JsonArray conns = dev.createNestedArray("cns");
  JsonArray mac   = conns.createNestedArray();
  mac.add("mac"); mac.add(macStr());
}

void mqttSendDiscovery() {
  // sensor — уровень в процентах (0, 50, 100)
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Level";
    d["uniq_id"]     = String(cfg.device_name) + "-level";
    d["stat_t"]      = topicLevelState();
    d["avty_t"]      = topicAvail();
    d["unit_of_meas"]= "%";
    d["icon"]        = "mdi:water-percent";
    d["state_class"] = "measurement";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicLevel().c_str(), payload.c_str(), true);
  }
  // binary_sensor — ошибка (невозможное состояние датчиков)
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Error";
    d["uniq_id"]     = String(cfg.device_name) + "-error";
    d["stat_t"]      = topicErrorState();
    d["avty_t"]      = topicAvail();
    d["pl_on"]       = "ON";
    d["pl_off"]      = "OFF";
    d["dev_cla"]     = "problem";
    d["icon"]        = "mdi:alert-circle";
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicError().c_str(), payload.c_str(), true);
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
    JsonArray opts = d.createNestedArray("ops"); // "options"
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

void mqttPublishAvailability() { mqtt.publish(topicAvail().c_str(), "online", true); }
void mqttPublishMode()         { mqtt.publish(topicModeState().c_str(), (cfg.mode == MODE_EXTERNAL) ? "external" : "auto", true); }
void mqttPublishLevel(int lvl) { String s = String(lvl); mqtt.publish(topicLevelState().c_str(), s.c_str(), true); }
void mqttPublishError(bool e)  { mqtt.publish(topicErrorState().c_str(), e ? "ON" : "OFF", true); }
void mqttPublishRelay(bool on) { mqtt.publish(topicRelayState().c_str(), on ? "ON" : "OFF", true); }
void mqttPublishIpRetained()   { String ip = WiFi.localIP().toString(); mqtt.publish(topicIp().c_str(), ip.c_str(), true); }

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg; msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim(); msg.toLowerCase();

  if (t == topicRelaySet()) {
    bool want_on = (msg == "on" || msg == "1" || msg == "true");
    // EXTERNAL — применяем напрямую; AUTO — автологика всё равно переиграет по 100%
    digitalWrite(PIN_RELAY, want_on ? HIGH : LOW);
    mqttPublishRelay(want_on);
  } else if (t == topicModeSet()) {
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

// =================== Веб-интерфейс (статус) ===================
ESP8266WebServer www(80);

int  g_level_percent = 0;   // 0 / 50 / 100
bool g_error         = false;
bool g_relay_on      = false;
bool g_s50_on        = false;  // дебаунс-итог датчика 50%
bool g_s100_on       = false;  // дебаунс-итог датчика 100%

String htmlHeader(const char* title) {
  String s = F("<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  s += "<title>"; s += title; s += "</title>";
  s += F("<style>body{font-family:system-ui,Arial;margin:2rem}code{background:#eee;padding:.1rem .3rem;border-radius:.3rem}</style>");
  return s;
}

void handleRoot() {
  String s = htmlHeader("Tank Controller");
  s += F("<h2>Tank Controller</h2>");
  s += "<p>Level: <b>" + String(g_level_percent) + "%</b></p>";
  s += "<p>Error: <b>" + String(g_error ? "TRUE" : "FALSE") + "</b></p>";
  s += "<p>Sensors: S50=" + String(g_s50_on ? "HIGH(ON)" : "LOW(OFF)")
     +  ", S100=" + String(g_s100_on ? "HIGH(ON)" : "LOW(OFF)") + "</p>";
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

// =================== Датчики и реле ===================
inline bool readRaw50()  { return digitalRead(PIN_SENSOR50)  == HIGH; } // >=50%
inline bool readRaw100() { return digitalRead(PIN_SENSOR100) == HIGH; } // 100%

void setRelay(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  g_relay_on = on;
  if (mqtt_online) mqttPublishRelay(on);
}

// =================== LED поведение ===================
// ВАЖНО: LED активен по LOW.
// error=false:
//   0%  -> OFF
//   50% -> blink 1 Hz
//   100%-> ON
// error=true:
//   not 100% -> blink 0.3 Hz
//   100%     -> smooth breathing @ 5 Hz (PWM). Фолбэк: быстрый мигающий, если PWM недоступен.
void driveLED(bool error, int level, uint32_t now_ms) {
  // Проверим, сможем ли использовать PWM (GPIO16 не поддерживает)
  bool pwm_possible = (LED_PIN != 16);

  if (!error) {
    if (level == 0) {
      digitalWrite(LED_PIN, HIGH);                 // выкл
    } else if (level == 50) {
      bool onPhase = ((now_ms / 1000) % 2) == 0;   // 1 Гц
      digitalWrite(LED_PIN, onPhase ? LOW : HIGH);
    } else { // 100
      digitalWrite(LED_PIN, LOW);                  // постоянно горит
    }
    return;
  }

  // error = true
  if (level == 100) {
    if (pwm_possible) {
      // Плавное дыхание 5 Гц: яркость по синусу. Период = 1/5 c = 200 мс.
      // Яркость 0..1023, инвертируем под активный LOW.
      const float period_s = 0.2f; // 5 Гц
      float t = fmodf(now_ms / 1000.0f, period_s) / period_s; // 0..1
      float y = 0.5f * (1.0f - cosf(2.0f * 3.1415926f * t));  // 0..1
      int duty = (int)(y * 1023.0f);                          // 0..1023
      analogWrite(LED_PIN, 1023 - duty); // инвертируем
    } else {
      // Фолбэк — быстрое мигание 5 Гц
      bool onPhase = ((now_ms / 100) % 2) == 0; // 5 Гц ~ 100 мс полупериод
      digitalWrite(LED_PIN, onPhase ? LOW : HIGH);
    }
  } else {
    // not 100%: мигаем 0.3 Гц (полный период ≈ 3.333c)
    // 50% скважность
    const uint32_t period_ms = 3333; // ~0.3 Гц
    uint32_t phase = now_ms % period_ms;
    bool onPhase = (phase < (period_ms / 2));
    digitalWrite(LED_PIN, onPhase ? LOW : HIGH);
  }
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

  // Пины датчиков и реле
  pinMode(PIN_SENSOR50,  INPUT_PULLUP);
  pinMode(PIN_SENSOR100, INPUT_PULLUP);
  pinMode(PIN_RELAY,     OUTPUT);
  setRelay(false); // по умолчанию насос выкл

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

  // (опционально) Частота PWM по умолчанию достаточна
  // analogWriteRange(1023);
  // analogWriteFreq(1000);
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

  // Чтение с антидребезгом
  bool raw50  = (digitalRead(PIN_SENSOR50)  == HIGH);  // сработал?
  bool raw100 = (digitalRead(PIN_SENSOR100) == HIGH);  // сработал?

  static bool first = true;
  static bool s50_on = false, s100_on = false;
  static uint8_t c50 = 0, c100 = 0;
  static bool cand50 = false, cand100 = false;

  if (first) {
    s50_on  = raw50;
    s100_on = raw100;
    first = false;
    if (mqtt_online) {
      mqttPublishAvailability();
      mqttSendDiscovery();
      mqttPublishMode();
      mqttPublishIpRetained();
      int lvl = s100_on ? 100 : (s50_on ? 50 : 0);
      bool err = (!s50_on && s100_on);
      mqttPublishLevel(lvl);
      mqttPublishError(err);
      mqttPublishRelay(g_relay_on);
    }
  } else {
    if (raw50 != s50_on) {
      if (c50 == 0) cand50 = raw50;
      if (raw50 == cand50) {
        if (++c50 >= cfg.confirm_samples) { s50_on = raw50; c50 = 0; }
      } else { cand50 = raw50; c50 = 1; }
    } else c50 = 0;

    if (raw100 != s100_on) {
      if (c100 == 0) cand100 = raw100;
      if (raw100 == cand100) {
        if (++c100 >= cfg.confirm_samples) { s100_on = raw100; c100 = 0; }
      } else { cand100 = raw100; c100 = 1; }
    } else c100 = 0;
  }

  // Строго дискретные уровни
  int  level = s100_on ? 100 : (s50_on ? 50 : 0);
  bool error = (!s50_on && s100_on); // невозможно: 100% без 50%

  // Обновления для веба
  g_s50_on        = s50_on;
  g_s100_on       = s100_on;
  g_level_percent = level;
  g_error         = error;

  // Светодиод согласно ТЗ
  driveLED(error, level, now);

  // Управление насосом (только датчик 100%)
  if (cfg.mode == MODE_AUTO) {
    bool want_on = !s100_on; // 100% не достигнут — насос включён
    if (want_on != g_relay_on) setRelay(want_on);
  }

  // MQTT публикации на изменения
  if (mqtt_online) {
    static int  last_level = -1;
    static bool last_error = !error;
    static bool last_pub_relay = !g_relay_on;

    if (level != last_level) { mqttPublishLevel(level); last_level = level; }
    if (error != last_error) { mqttPublishError(error);  last_error = error; }
    if (g_relay_on != last_pub_relay) { mqttPublishRelay(g_relay_on); last_pub_relay = g_relay_on; }

    // IP при смене
    static String last_ip;
    String cur_ip = WiFi.localIP().toString();
    if (cur_ip != last_ip) { last_ip = cur_ip; mqttPublishIpRetained(); }

    // Атрибуты
    StaticJsonDocument<256> attr;
    attr["sample_ms"]       = cfg.sample_ms;
    attr["confirm_needed"]  = cfg.confirm_samples;
    attr["mode"]            = (cfg.mode == MODE_EXTERNAL) ? "external" : "auto";
    attr["uptime_s"]        = (uint32_t)(millis()/1000);
    attr["rssi"]            = WiFi.RSSI();
    attr["s50"]             = s50_on;
    attr["s100"]            = s100_on;
    attr["error"]           = error;
    String payload; serializeJson(attr, payload);
    mqtt.publish(topicAttr().c_str(), payload.c_str(), false);
  }

  // Лог
  Serial.printf("s50=%d s100=%d level=%d error=%d relay=%d mode=%s\n",
                (int)s50_on, (int)s100_on, level, (int)error, (int)g_relay_on,
                (cfg.mode==MODE_EXTERNAL) ? "EXTERNAL" : "AUTO");
}
