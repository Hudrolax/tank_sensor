#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          // Wi-Fi captive portal
#include <ESP8266WebServer.h>     // Web UI
#include <PubSubClient.h>         // MQTT
#include <ArduinoJson.h>          // JSON (config + discovery)
#include <LittleFS.h>             // /config.json
#include <ESP8266mDNS.h>

// =================== Аппаратные настройки ===================
const int FACTORY_PIN = D7;                 // GPIO13 — замкнуть на GND и держать ~5 c при старте для сброса
const unsigned long FACTORY_HOLD_MS = 5000;
const int LED_PIN = LED_BUILTIN;            // встроенный LED (ESP8266 активен по LOW)

// =================== Конфиг ===================
struct Config {
  char     device_name[32] = "tank-sensor";
  char     base_topic[64]  = "home/tank";
  char     mqtt_host[64]   = "";        // пусто => MQTT не коннектимся
  uint16_t mqtt_port       = 1883;
  char     mqtt_user[32]   = "";
  char     mqtt_pass[32]   = "";

  // Цифровой датчик
  uint8_t  sensor_pin      = 14;        // GPIO14 = D5
  bool     active_low      = true;      // оптопара тянет к GND => активный LOW
  uint32_t sample_ms       = 50;        // период опроса (мс)
  uint8_t  confirm_samples = 3;         // подряд одинаковых для фиксации состояния
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

  cfg.sensor_pin      = d["sensor_pin"]      | cfg.sensor_pin;
  cfg.active_low      = d["active_low"]      | cfg.active_low;
  cfg.sample_ms       = d["sample_ms"]       | cfg.sample_ms;
  cfg.confirm_samples = d["confirm_samples"] | cfg.confirm_samples;
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
  d["sensor_pin"]      = cfg.sensor_pin;
  d["active_low"]      = cfg.active_low;
  d["sample_ms"]       = cfg.sample_ms;
  d["confirm_samples"] = cfg.confirm_samples;
  File f = LittleFS.open(CFG_PATH, "w"); if (!f) return false;
  serializeJsonPretty(d, f); f.close(); return true;
}

// =================== MQTT (неблокирующее) ===================
WiFiClient   espClient;
PubSubClient mqtt(espClient);
bool         mqtt_online = false;

String topicState() { return String(cfg.base_topic) + "/state"; }
String topicAttr()  { return String(cfg.base_topic) + "/attributes"; }
String topicAvail() { return String(cfg.base_topic) + "/status"; }
String topicIp()    { return String(cfg.base_topic) + "/ip"; }

String discTopicBinary() { return "homeassistant/binary_sensor/" + String(cfg.device_name) + "/tank/config"; }
String discTopicIP()     { return "homeassistant/sensor/"        + String(cfg.device_name) + "/ip/config"; }

void addDeviceObject(JsonObject dev) {
  dev["ids"]  = String(cfg.device_name);
  dev["name"] = String(cfg.device_name);
  dev["mdl"]  = "NodeMCU-ESP8266";
  dev["mf"]   = "DIY";
  dev["sw"]   = "2.1.0";
  JsonArray conns = dev.createNestedArray("cns");  // HA: "connections"
  JsonArray mac   = conns.createNestedArray();
  mac.add("mac"); mac.add(macStr());
}

void mqttSendDiscovery() {
  // binary_sensor (состояние датчика)
  {
    DynamicJsonDocument d(1024);
    d["name"]        = String(cfg.device_name) + " Tank";
    d["uniq_id"]     = String(cfg.device_name) + "-tank";
    d["stat_t"]      = topicState();
    d["json_attr_t"] = topicAttr();
    d["avty_t"]      = topicAvail();
    d["pl_on"]       = "ON";
    d["pl_off"]      = "OFF";
    d["icon"]        = "mdi:water";
    // d["dev_cla"]   = "moisture"; // при желании можно включить
    addDeviceObject(d.createNestedObject("dev"));
    String payload; serializeJson(d, payload);
    mqtt.publish(discTopicBinary().c_str(), payload.c_str(), true);
  }
  // sensor (текущий IP как отдельный сенсор)
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

void mqttOnConnectedOnce() {
  mqtt.publish(topicAvail().c_str(), "online", true);
  mqttSendDiscovery();
  // опубликуем IP отдельно (retained)
  String ip = WiFi.localIP().toString();
  mqtt.publish(topicIp().c_str(), ip.c_str(), true);
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
  String clientId = String(cfg.device_name) + "-" + String(ESP.getChipId(), HEX);
  bool ok = mqtt.connect(clientId.c_str(),
                         cfg.mqtt_user[0] ? cfg.mqtt_user : nullptr,
                         cfg.mqtt_user[0] ? cfg.mqtt_pass : nullptr,
                         topicAvail().c_str(), 0, true, "offline");
  if (ok) { mqtt_online = true; mqttOnConnectedOnce(); }
}

// =================== Веб-интерфейс ===================
ESP8266WebServer www(80);

// Телеметрия для /
bool g_state = false; // OFF/ON
int  g_raw   = 1;     // 0/1

String htmlHeader(const char* title) {
  String s = F("<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  s += "<title>"; s += title; s += "</title>";
  s += F("<style>body{font-family:system-ui,Arial;margin:2rem}input,button,select{font-size:1rem;padding:.4rem}.row{margin:.5rem 0}</style>");
  return s;
}

String row(const char* label, const String& name, const String& val, const char* extra="") {
  return String("<div class=row><label>") + label + ":<br><input name='" + name + "' value='" + val + "' " + extra + "></label></div>";
}
String rowSelectPin(const char* label, const String& name, uint8_t val) {
  uint8_t opts[] = {5,4,14,12}; // безопасные GPIO: D1,D2,D5,D6
  String s = String("<div class=row><label>") + label + ":<br><select name='" + name + "'>";
  for (uint8_t i=0;i<sizeof(opts)/sizeof(opts[0]);i++) {
    uint8_t p = opts[i];
    s += "<option value='" + String(p) + "'";
    if (p == val) s += " selected";
    s += ">GPIO" + String(p) + "</option>";
  }
  s += "</select></label></div>";
  return s;
}
String rowCheckbox(const char* label, const String& name, bool checked) {
  String s = String("<div class=row><label><input type='checkbox' name='") + name + "' ";
  if (checked) s += "checked ";
  s += "> " + String(label) + "</label></div>";
  return s;
}

void handleRoot() {
  String s = htmlHeader("Tank Sensor (Digital)");
  s += F("<h2>Tank Sensor (Digital)</h2>"
         "<p><a href='/config'>Config</a> | <a href='/reannounce'>Re-announce MQTT Discovery</a> | <a href='/reboot'>Reboot</a></p>");
  s += "<p>pin=<b>GPIO" + String(cfg.sensor_pin) + "</b>  raw=" + String(g_raw) +
       "  active_low=" + String(cfg.active_low ? "true" : "false") +
       "  state=<b>" + String(g_state ? "ON" : "OFF") + "</b></p>";
  s += "<p>Wi-Fi IP: <b>" + WiFi.localIP().toString() + "</b>, RSSI " + String(WiFi.RSSI()) + " dBm</p>";
  s += "<p>MQTT: " + String(mqtt_online ? "connected" : "disconnected") + "</p>";
  www.send(200, "text/html; charset=utf-8", s);
}

void handleConfigGet() {
  String s = htmlHeader("Config");
  s += F("<h2>Config</h2><form method='POST' action='/config'>");
  s += row("Device name","device_name", cfg.device_name);
  s += row("Base topic","base_topic",   cfg.base_topic);
  s += row("MQTT host","mqtt_host",     cfg.mqtt_host);
  s += row("MQTT port","mqtt_port",     String(cfg.mqtt_port), "type=number min=1 max=65535");
  s += row("MQTT user","mqtt_user",     cfg.mqtt_user);
  s += row("MQTT pass","mqtt_pass",     cfg.mqtt_pass);

  s += rowSelectPin("Sensor pin", "sensor_pin", cfg.sensor_pin);
  s += rowCheckbox("Active low (pulled to GND => ON)", "active_low", cfg.active_low);
  s += row("Sample ms","sample_ms",     String(cfg.sample_ms), "type=number min=5 max=2000 step=5");
  s += row("Confirm samples","confirm_samples", String(cfg.confirm_samples), "type=number min=1 max=10");

  s += F("<button type=submit>Save & Reboot</button></form><p><a href='/'>Back</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}

void handleConfigPost() {
  auto gv=[&](const String& k){ return www.hasArg(k)?www.arg(k):String(""); };
  strlcpy(cfg.device_name, gv("device_name").c_str(), sizeof(cfg.device_name));
  strlcpy(cfg.base_topic,  gv("base_topic").c_str(),  sizeof(cfg.base_topic));
  strlcpy(cfg.mqtt_host,   gv("mqtt_host").c_str(),   sizeof(cfg.mqtt_host));
  cfg.mqtt_port = gv("mqtt_port").toInt();
  strlcpy(cfg.mqtt_user, gv("mqtt_user").c_str(), sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass, gv("mqtt_pass").c_str(), sizeof(cfg.mqtt_pass));

  long sp = gv("sensor_pin").toInt();
  if (sp==4 || sp==5 || sp==12 || sp==14) cfg.sensor_pin = (uint8_t)sp; // только «безопасные» GPIO
  cfg.active_low = www.hasArg("active_low");

  long sm = gv("sample_ms").toInt(); if (sm < 5) sm = 5; if (sm > 2000) sm = 2000; cfg.sample_ms = (uint32_t)sm;
  long cs = gv("confirm_samples").toInt(); if (cs < 1) cs = 1; if (cs > 10) cs = 10; cfg.confirm_samples = (uint8_t)cs;

  saveConfig();
  www.send(200, "text/html; charset=utf-8", F("<p>Saved. Rebooting…</p>"));
  delay(500);
  ESP.restart();
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

  // Пин датчика
  pinMode(cfg.sensor_pin, cfg.active_low ? INPUT_PULLUP : INPUT);

  // Wi-Fi
  WiFiManager wm;
  char apName[32]; sprintf(apName, "TankSensor-%06X", ESP.getChipId() & 0xFFFFFF);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  wm.autoConnect(apName);

  // mDNS
  if (MDNS.begin(cfg.device_name)) MDNS.addService("http", "tcp", 80);

  // Web
  www.on("/", handleRoot);
  www.on("/config", HTTP_GET,  handleConfigGet);
  www.on("/config", HTTP_POST, handleConfigPost);
  www.on("/reannounce", HTTP_GET, handleReannounce);
  www.on("/reboot", HTTP_GET, handleReboot);
  www.begin();
}

// =================== LOOP (цифровой детектор) ===================
void loop() {
  static uint32_t t_next = 0;

  // сервисы
  www.handleClient();
  mqttPump();

  // дискретизация по сетке
  uint32_t now = millis();
  if (t_next == 0) t_next = now;
  if (now < t_next) { delay(1); return; }
  t_next += cfg.sample_ms;

  // Читаем пин
  int raw = digitalRead(cfg.sensor_pin);                 // 0/1
  bool active = cfg.active_low ? (raw == LOW) : (raw == HIGH);  // переводим в «логический активный»

  // Антидребезг: N подряд одинаковых выборок
  static bool     first_sample = true;
  static bool     state        = false;  // OFF
  static uint8_t  cand_count   = 0;
  static bool     cand_val     = false;

  bool toggled = false;

  if (first_sample) {
    state = active;
    first_sample = false;
    if (mqtt_online) { mqtt.publish(topicAvail().c_str(), "online", true); mqttSendDiscovery(); }
  } else {
    if (active != state) {
      if (cand_count == 0) cand_val = active;
      if (active == cand_val) {
        cand_count++;
        if (cand_count >= cfg.confirm_samples) {
          state = active;
          toggled = true;
          cand_count = 0;
        }
      } else {
        cand_val = active;
        cand_count = 1;
      }
    } else {
      cand_count = 0;
    }
  }

  // Встроенный LED: ON = горит (для ESP8266 LOW включает светодиод)
  digitalWrite(LED_PIN, state ? LOW : HIGH);

  // MQTT публикации
  if (mqtt_online) {
    const char* st = state ? "ON" : "OFF";

    // бинарный датчик
    StaticJsonDocument<512> attr;
    attr["raw"]            = raw;
    attr["active_low"]     = cfg.active_low;
    attr["pin"]            = cfg.sensor_pin;
    attr["sample_ms"]      = cfg.sample_ms;
    attr["confirm_needed"] = cfg.confirm_samples;
    int confirm_left = (active != state) ? (int)cfg.confirm_samples - (int)cand_count : 0;
    if (confirm_left < 0) confirm_left = 0;
    attr["confirm_left"]   = confirm_left;
    attr["uptime_s"]       = (uint32_t)(millis()/1000);
    attr["ip"]             = WiFi.localIP().toString();
    attr["rssi"]           = WiFi.RSSI();
    attr["ssid"]           = WiFi.SSID();

    String payload; serializeJson(attr, payload);
    mqtt.publish(topicState().c_str(), st, true);            // retain
    mqtt.publish(topicAttr().c_str(),  payload.c_str(), false);

    // сенсор IP (отдельная сущность)
    static String last_ip;
    String cur_ip = WiFi.localIP().toString();
    if (last_ip != cur_ip) {
      last_ip = cur_ip;
      mqtt.publish(topicIp().c_str(), cur_ip.c_str(), true); // retain
    }
  }

  // Телеметрия для веб-страницы
  g_state = state; g_raw = raw;

  // Лог в Serial
  Serial.printf("raw=%d active=%d cand_count=%u toggled=%s state=%s\n",
                raw, active, cand_count, toggled ? "YES" : "NO", state ? "ON" : "OFF");
}
