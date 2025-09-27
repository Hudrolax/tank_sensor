#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          // каптив-портал для Wi-Fi
#include <ESP8266WebServer.h>     // веб-интерфейс настроек
#include <PubSubClient.h>         // MQTT
#include <ArduinoJson.h>          // конфиг + discovery
#include <LittleFS.h>             // /config.json
#include <ESP8266mDNS.h>

// =================== Аппаратные настройки ===================
const int FACTORY_PIN = D7;                 // GPIO13 — замкнуть на GND для сброса (удерживать при старте)
const unsigned long FACTORY_HOLD_MS = 5000; // ~5 c
const int LED_PIN = LED_BUILTIN;            // индикатор (ESP8266: активен по LOW)

// =================== Детектор (логика измерений) ===================
static const uint8_t  BURST_READS  = 5;     // медиана из 5
static const uint32_t BURST_GAP_US = 500;   // мкс между чтениями
enum State { FALSE_STATE = 0, TRUE_STATE = 1 };

static inline uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { uint16_t t=a; a=b; b=t; }
  if (b > c) { uint16_t t=b; b=c; c=t; }
  if (a > b) { uint16_t t=a; a=b; b=t; }
  return b;
}
static inline uint16_t median5_fast(uint16_t v0, uint16_t v1, uint16_t v2, uint16_t v3, uint16_t v4) {
  uint16_t m1 = median3(v0, v1, v2);
  uint16_t m2 = median3(v2, v3, v4);
  return (m1 < m2) ? m2 : m1;
}
static uint16_t readBurstMedian() {
  uint16_t v[BURST_READS];
  for (uint8_t i=0;i<BURST_READS;i++){ v[i]=analogRead(A0); delayMicroseconds(BURST_GAP_US); }
  return median5_fast(v[0],v[1],v[2],v[3],v[4]);
}
static uint16_t median_small(uint16_t *arr, uint8_t n) {
  for (uint8_t i=0;i<n;i++) for (uint8_t j=i+1;j<n;j++) if (arr[j]<arr[i]) { uint16_t t=arr[i]; arr[i]=arr[j]; arr[j]=t; }
  return arr[n/2];
}

// =================== Конфиг ===================
struct Config {
  char     device_name[32]    = "tank-sensor";
  char     base_topic[64]     = "home/tank";
  char     mqtt_host[64]      = "";        // пусто по умолчанию — MQTT не дёргаем, пока не настроено
  uint16_t mqtt_port          = 1883;
  char     mqtt_user[32]      = "";
  char     mqtt_pass[32]      = "";
  float    threshold_pct      = 2.0;       // % от базы для детекции
  uint16_t min_counts         = 15;        // нижняя планка порога (отсчёты)
  uint8_t  confirm_samples    = 3;         // N подряд для подтверждения (1..5)
  uint32_t settle_ms          = 5000;      // «досадка» базы после переключения (мс)
  uint32_t sample_ms          = 1000;      // период опроса (мс)
  uint16_t drift_counts       = 1;         // насколько подтягивать базу за 1 цикл к curr (отсчёты)
} cfg;

const char* CFG_PATH = "/config.json";

bool loadConfig() {
  if (!LittleFS.exists(CFG_PATH)) return false;
  File f = LittleFS.open(CFG_PATH, "r"); if (!f) return false;
  DynamicJsonDocument d(2048);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();

  strlcpy(cfg.device_name, d["device_name"] | cfg.device_name, sizeof(cfg.device_name));
  strlcpy(cfg.base_topic,  d["base_topic"]  | cfg.base_topic,  sizeof(cfg.base_topic));
  strlcpy(cfg.mqtt_host,   d["mqtt_host"]   | cfg.mqtt_host,   sizeof(cfg.mqtt_host));
  cfg.mqtt_port       = d["mqtt_port"]       | cfg.mqtt_port;
  strlcpy(cfg.mqtt_user,   d["mqtt_user"]   | cfg.mqtt_user,   sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass,   d["mqtt_pass"]   | cfg.mqtt_pass,   sizeof(cfg.mqtt_pass));
  cfg.threshold_pct   = d["threshold_pct"]   | cfg.threshold_pct;
  cfg.min_counts      = d["min_counts"]      | cfg.min_counts;
  cfg.confirm_samples = d["confirm_samples"] | cfg.confirm_samples;
  cfg.settle_ms       = d["settle_ms"]       | cfg.settle_ms;
  cfg.sample_ms       = d["sample_ms"]       | cfg.sample_ms;
  cfg.drift_counts    = d["drift_counts"]    | cfg.drift_counts;
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
  d["threshold_pct"]   = cfg.threshold_pct;
  d["min_counts"]      = cfg.min_counts;
  d["confirm_samples"] = cfg.confirm_samples;
  d["settle_ms"]       = cfg.settle_ms;
  d["sample_ms"]       = cfg.sample_ms;
  d["drift_counts"]    = cfg.drift_counts;
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
String discTopic()  { return "homeassistant/binary_sensor/" + String(cfg.device_name) + "/tank/config"; }

void mqttSendDiscovery() {
  DynamicJsonDocument d(1024);
  d["name"]        = String(cfg.device_name) + " Tank";
  d["uniq_id"]     = String(cfg.device_name) + "-tank";
  d["stat_t"]      = topicState();
  d["json_attr_t"] = topicAttr();
  d["avty_t"]      = topicAvail();
  d["pl_on"]       = "ON";
  d["pl_off"]      = "OFF";
  d["icon"]        = "mdi:water";
  JsonObject dev = d.createNestedObject("dev");
  dev["ids"]  = String(cfg.device_name);
  dev["name"] = String(cfg.device_name);
  dev["mdl"]  = "NodeMCU-ESP8266";
  dev["mf"]   = "DIY";
  dev["sw"]   = "1.1.0";
  String payload; serializeJson(d, payload);
  mqtt.publish(discTopic().c_str(), payload.c_str(), true);
}
void mqttOnConnectedOnce() {
  mqtt.publish(topicAvail().c_str(), "online", true);
  mqttSendDiscovery();
}
void mqttPump() {
  mqtt.loop();
  if (cfg.mqtt_host[0] == '\0') { mqtt_online = false; return; } // не настроен
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

// Телеметрия для / (на лету)
uint16_t g_curr=0, g_base=0, g_thr=0, g_diff=0;
bool     g_settling=false;
State    g_state=FALSE_STATE;

String htmlHeader(const char* title) {
  String s = F("<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  s += "<title>"; s += title; s += "</title>";
  s += F("<style>body{font-family:system-ui,Arial;margin:2rem}input,button{font-size:1rem;padding:.4rem}.row{margin:.5rem 0}</style>");
  return s;
}
void handleRoot() {
  String s = htmlHeader("Tank Sensor");
  s += F("<h2>Tank Sensor</h2>"
         "<p><a href='/config'>Config</a> | <a href='/reannounce'>Re-announce MQTT Discovery</a> | <a href='/reboot'>Reboot</a></p>");
  s += "<p>curr=<b>" + String(g_curr) + "</b> base=" + String(g_base) +
       " diff=" + String(g_diff) + " thr=" + String(g_thr) +
       " state=<b>" + String(g_state == TRUE_STATE ? "TRUE" : "FALSE") + "</b>" +
       (g_settling ? " (settling)" : "") + "</p>";
  s += "<p>MQTT: " + String(mqtt_online ? "connected" : "disconnected") + "</p>";
  www.send(200, "text/html; charset=utf-8", s);
}
void handleConfigGet() {
  String s = htmlHeader("Config");
  s += F("<h2>Config</h2><form method='POST' action='/config'>");
  auto row=[&](const char* label, const String& name, const String& val, const char* extra=""){
    return String("<div class=row><label>")+label+":<br><input name='"+name+"' value='"+val+"' "+extra+"></label></div>";
  };
  s += row("Device name","device_name", cfg.device_name);
  s += row("Base topic","base_topic",   cfg.base_topic);
  s += row("MQTT host","mqtt_host",     cfg.mqtt_host);
  s += row("MQTT port","mqtt_port",     String(cfg.mqtt_port), "type=number min=1 max=65535");
  s += row("MQTT user","mqtt_user",     cfg.mqtt_user);
  s += row("MQTT pass","mqtt_pass",     cfg.mqtt_pass);
  s += row("Threshold, %","threshold_pct", String(cfg.threshold_pct, 2), "type=number step=0.1");
  s += row("Min counts","min_counts",   String(cfg.min_counts), "type=number");
  s += row("Confirm samples","confirm_samples", String(cfg.confirm_samples), "type=number min=1 max=5");
  s += row("Settle ms","settle_ms",     String(cfg.settle_ms), "type=number");
  s += row("Sample ms","sample_ms",     String(cfg.sample_ms), "type=number");
  s += row("Drift counts/sample","drift_counts", String(cfg.drift_counts), "type=number min=0 max=50");
  s += F("<button type=submit>Save & Reboot</button></form><p><a href='/'>Back</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}
void handleConfigPost() {
  auto gv=[&](const String& k){ return www.hasArg(k)?www.arg(k):String(""); };
  strlcpy(cfg.device_name, gv("device_name").c_str(), sizeof(cfg.device_name));
  strlcpy(cfg.base_topic,  gv("base_topic").c_str(),  sizeof(cfg.base_topic));
  strlcpy(cfg.mqtt_host,   gv("mqtt_host").c_str(),   sizeof(cfg.mqtt_host));
  cfg.mqtt_port        = gv("mqtt_port").toInt();
  strlcpy(cfg.mqtt_user,   gv("mqtt_user").c_str(),   sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass,   gv("mqtt_pass").c_str(),   sizeof(cfg.mqtt_pass));
  cfg.threshold_pct    = gv("threshold_pct").toFloat();
  cfg.min_counts       = gv("min_counts").toInt();
  cfg.confirm_samples  = gv("confirm_samples").toInt();
  cfg.settle_ms        = gv("settle_ms").toInt();
  cfg.sample_ms        = gv("sample_ms").toInt();
  // без std::max — аккуратно зажмём вручную
  {
    long dc = gv("drift_counts").toInt(); // String::toInt() -> long
    if (dc < 0)   dc = 0;
    if (dc > 50)  dc = 50;                // разумная защита сверху
    cfg.drift_counts = (uint16_t)dc;
  }
  saveConfig();
  www.send(200, "text/html; charset=utf-8", F("<p>Saved. Rebooting…</p>"));
  delay(500);
  ESP.restart();
}
void handleReannounce() {
  mqttPump();
  if (mqtt_online) { mqttSendDiscovery(); www.send(200,"text/plain","Discovery re-announced"); }
  else             { www.send(503,"text/plain","MQTT not connected"); }
}
void handleReboot() {
  www.send(200,"text/plain","Rebooting...");
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
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(FACTORY_PIN, INPUT_PULLUP);

  // Окно удержания для «заводского» сброса
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

  // 1) Wi-Fi: если не подключится — поднимет AP с порталом
  WiFiManager wm;
  char apName[32]; sprintf(apName,"TankSensor-%06X", ESP.getChipId() & 0xFFFFFF);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  wm.autoConnect(apName);

  // 2) mDNS
  if (MDNS.begin(cfg.device_name)) MDNS.addService("http","tcp",80);

  // 3) Web
  www.on("/", handleRoot);
  www.on("/config", HTTP_GET,  handleConfigGet);
  www.on("/config", HTTP_POST, handleConfigPost);
  www.on("/reannounce", HTTP_GET, handleReannounce);
  www.on("/reboot", HTTP_GET, handleReboot);
  www.begin();
}

// =================== LOOP ===================
void loop() {
  static uint32_t t_next = 0;

  www.handleClient();
  mqttPump();

  // ---- Детектор ----
  static bool     first_sample   = true;
  static uint16_t last_stable    = 0;
  static State    state          = FALSE_STATE;

  // Кандидат + подтверждение
  static int8_t   cand_dir       = 0;   // +1 -> TRUE, -1 -> FALSE
  static uint8_t  cand_count     = 0;
  static uint16_t cand_vals[5];

  // Пост-стабилизация после переключения
  static bool     settling       = false;
  static uint32_t settle_until   = 0;
  static uint8_t  settle_count   = 0;
  static const uint8_t SETTLE_BUF_MAX = 8;
  static uint16_t settle_vals[SETTLE_BUF_MAX];

  uint32_t now = millis();
  if (t_next == 0) t_next = now;
  if (now < t_next) { delay(1); return; }
  t_next += cfg.sample_ms;

  uint16_t curr = readBurstMedian();

  if (first_sample) {
    last_stable = curr;
    first_sample = false;
    if (mqtt_online) { mqtt.publish(topicAvail().c_str(), "online", true); mqttSendDiscovery(); }
  }

  // Порог от базы (на начало шага)
  uint16_t base_used = last_stable;
  uint16_t denom = (base_used < 16) ? 16 : base_used;
  uint16_t thr_counts = (uint16_t) lround((cfg.threshold_pct / 100.0f) * denom);
  if (thr_counts < cfg.min_counts) thr_counts = cfg.min_counts;

  int32_t  signed_diff = (int32_t)curr - (int32_t)base_used;
  uint16_t diff_counts = (signed_diff >= 0) ? (uint16_t)signed_diff : (uint16_t)(-signed_diff);
  float    diff_pct    = 100.0f * ((float)diff_counts / (float)denom);

  bool toggled = false;

  if (settling) {
    if (settle_count < SETTLE_BUF_MAX) settle_vals[settle_count++] = curr;
    if (now >= settle_until || settle_count >= SETTLE_BUF_MAX) {
      uint16_t new_base = median_small(settle_vals, settle_count);
      last_stable = new_base;
      settling = false;
      settle_count = 0;
    }
  } else {
    bool exceed = (diff_counts >= thr_counts);
    if (exceed) {
      int8_t dir = (signed_diff > 0) ? +1 : -1;
      if (cand_dir != dir) { cand_dir = dir; cand_count = 0; }
      if (cand_count < cfg.confirm_samples && cfg.confirm_samples<=5) cand_vals[cand_count] = curr;
      cand_count++;
      if (cand_count >= cfg.confirm_samples) {
        State new_state = (dir > 0) ? TRUE_STATE : FALSE_STATE;
        if (new_state != state) {
          uint8_t n = (cfg.confirm_samples<=5)? cfg.confirm_samples : 5;
          uint16_t commit_base = median_small(cand_vals, n);
          last_stable = commit_base;
          state = new_state;
          toggled = true;
          // Старт фазы пост-стабилизации
          settling = true;
          settle_until = now + cfg.settle_ms;
          settle_count = 0;
          settle_vals[settle_count++] = curr;
        }
        cand_dir = 0; cand_count = 0;
      }
    } else {
      cand_dir = 0; cand_count = 0;
    }

    // --- Плавный дрейф базы (всегда, когда не settling) ---
    if (cfg.drift_counts > 0) {
      int16_t err = (int16_t)curr - (int16_t)last_stable;
      int16_t step = err;
      if (step > (int16_t)cfg.drift_counts)  step = (int16_t)cfg.drift_counts;
      if (step < -(int16_t)cfg.drift_counts) step = -(int16_t)cfg.drift_counts;
      int32_t nb = (int32_t)last_stable + step;
      if (nb < 0)    nb = 0;
      if (nb > 1023) nb = 1023;
      last_stable = (uint16_t)nb;
    }
  }

  // MQTT публикации
  if (mqtt_online) {
    const char* st = (state == TRUE_STATE) ? "ON" : "OFF";
    StaticJsonDocument<384> attr;
    attr["curr"]         = curr;
    attr["base"]         = base_used;      // база, использованная на этом шаге
    attr["diff"]         = diff_counts;
    attr["thr"]          = thr_counts;
    attr["diff_pct"]     = diff_pct;
    attr["settling"]     = settling;
    int confirm_left = (cand_dir!=0) ? (int)cfg.confirm_samples - (int)cand_count : 0;
    if (confirm_left < 0) confirm_left = 0;
    attr["confirm_left"] = confirm_left;
    attr["drift_step"]   = cfg.drift_counts;
    attr["uptime_s"]     = (uint32_t)(millis()/1000);
    String attrPayload; serializeJson(attr, attrPayload);
    mqtt.publish(topicState().c_str(), st, true);               // retain
    mqtt.publish(topicAttr().c_str(),  attrPayload.c_str(), false);
  }

  // Телеметрия для веб-страницы
  g_curr = curr; g_base = base_used; g_thr = thr_counts; g_diff = diff_counts; g_settling = settling; g_state = state;

  // Лог (опционально)
  Serial.printf("curr=%u base=%u diff=%u (%.3f%%)  thr=%u  toggled=%s  state=%s%s\n",
                curr, base_used, diff_counts, diff_pct, thr_counts,
                toggled ? "YES" : "NO", (state==TRUE_STATE?"ON":"OFF"), settling ? " [settling]" : "");
}
