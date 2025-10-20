#include "web.h"
#include "config.h"
#include "mqtt.h"
#include "hardware.h"
#include "sensors.h"
#include "relay.h"

#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WiFi.h>

static ESP8266WebServer www(80);
static ESP8266HTTPUpdateServer httpUpdater;
static bool g_pending_reboot = false;

// ---------- helpers ----------
static String htmlHeader(const char* title) {
  String s = F("<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  s += "<title>"; s += title; s += "</title>";
  s += F("<style>body{font-family:system-ui,Arial;margin:2rem;max-width:860px}"
        "a{color:#06f;text-decoration:none} a:hover{text-decoration:underline}"
        "code{background:#eee;padding:.1rem .3rem;border-radius:.3rem}"
        "input,select{padding:.4rem .5rem;border:1px solid #ccc;border-radius:.5rem;width:100%;max-width:360px} "
        "label{display:block;margin:.5rem 0 .2rem;font-weight:600}"
        "button{padding:.45rem .8rem;border-radius:.6rem;border:0;background:#222;color:#fff;cursor:pointer}"
        "button.secondary{background:#666}"
        ".row{margin:.6rem 0}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}"
        ".hr{height:1px;background:#eee;margin:1rem 0}"
        ".warn{color:#b45309}"
        "</style>");
  return s;
}

static String esc(const String& in){
  String out; out.reserve(in.length()+8);
  for(char c: in){
    if(c=='&') out += F("&amp;");
    else if(c=='<') out += F("&lt;");
    else if(c=='>') out += F("&gt;");
    else if(c=='\"') out += F("&quot;");
    else out += c;
  }
  return out;
}

static void rebootSoon(const String& back = "/"){
  www.send(200, "text/html; charset=utf-8",
    "<meta charset='utf-8'><meta http-equiv='refresh' content='5;url="+back+"'><body>"
    "<p>Сохранено. Устройство перезагрузится через ~2 секунды…</p>"
    "<p><a href='"+back+"'>Вернуться</a></p></body>");
  delay(500);
  g_pending_reboot = true;
}

static String optionSel(int value, int selected) {
  String s = "<option value='" + String(value) + "'";
  if (value == selected) s += " selected";
  s += ">";
  return s;
}

// ---------- handlers ----------
static void handleRoot() {
  String s = htmlHeader("Tank Controller");
  s += F("<h2>Tank Controller</h2>");
  s += "<p>Level: <b>" + String(sensors_level()) + "%</b></p>";
  s += "<p>Error: <b>" + String(sensors_error() ? "TRUE" : "FALSE") + "</b></p>";
  s += "<p>Sensors: S50=" + String(sensors_s50() ? "ON" : "OFF")
     +  ", S100=" + String(sensors_s100() ? "ON" : "OFF") + "</p>";
  s += "<p>Relay: <b>" + String(relay_get() ? "ON" : "OFF") + "</b></p>";
  s += "<p>Mode: <b>" + String((cfg.mode==MODE_EXTERNAL) ? "external" : "auto") + "</b></p>";
  s += "<p>Wi-Fi SSID: <b>" + esc(WiFi.SSID()) + "</b>, IP <b>" + WiFi.localIP().toString()
     + "</b>, RSSI " + String(WiFi.RSSI()) + " dBm</p>";
  s += "<p>MQTT: " + String(mqtt_online() ? "connected" : "disconnected") + "</p>";
  s += F("<div class='hr'></div>"
         "<p><a href='/wifi'>Wi-Fi</a> | <a href='/settings'>Settings</a> | "
         "<a href='/reannounce'>Re-announce</a> | <a href='/update'>Update firmware</a> | "
         "<a href='/reboot'>Reboot</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}

static void handleReannounce() {
  if (mqtt_online()) { mqtt_reannounce(); www.send(200, "text/plain", "Discovery + states re-announced"); }
  else               { www.send(503, "text/plain", "MQTT not connected"); }
}

static void handleReboot() {
  www.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

// --- Wi-Fi page (смена точки доступа в STA режиме) ---
static void handleWifiPage() {
  int n = WiFi.scanNetworks(false, true);
  String s = htmlHeader("Wi-Fi");
  s += F("<h2>Wi-Fi</h2>");
  s += "<p>Текущая сеть: <b>" + esc(WiFi.SSID()) + "</b></p>";

  s += F("<form method='post' action='/wifi/save'>"
         "<div class='row'><label>SSID</label>"
         "<input name='ssid' placeholder='Имя сети' required></div>"
         "<div class='row'><label>Password</label>"
         "<input name='pass' placeholder='Пароль' type='password'></div>"
         "<div class='row'><button type='submit'>Сохранить и перезагрузить</button></div>"
         "</form>");

  s += F("<div class='hr'></div><h3>Доступные сети</h3><ul>");
  for (int i = 0; i < n; i++) {
    s += "<li>" + esc(WiFi.SSID(i)) + " (RSSI " + String(WiFi.RSSI(i)) + " dBm"
       + (WiFi.encryptionType(i) == ENC_TYPE_NONE ? ", open" : "") + ")</li>";
  }
  s += F("</ul>");

  s += F("<div class='hr'></div>"
         "<form method='post' action='/wifi/forget'>"
         "<p>Забыть сохранённые сети и перейти в режим точки доступа (WiFiManager-портал)?</p>"
         "<button class='secondary' type='submit'>Забыть сети и перезагрузить</button>"
         "</form>"
         "<p class='warn'>Примечание: при отсутствии доступной сети устройство само поднимет AP-портал.</p>");

  s += F("<p><a href='/'>Назад</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}

static void handleWifiSave() {
  String ssid = www.arg("ssid");
  String pass = www.arg("pass");
  ssid.trim(); pass.trim();
  if (ssid.length() == 0) { www.send(400, "text/plain", "SSID is required"); return; }

  WiFi.persistent(true);
  if (pass.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());
  else                   WiFi.begin(ssid.c_str());
  WiFi.persistent(false);

  rebootSoon("/wifi");
}

static void handleWifiForget() {
  WiFi.persistent(true);
  WiFi.disconnect(true); // erase creds
  WiFi.persistent(false);
  rebootSoon("/wifi");
}

// --- Settings (MQTT + Pins) ---
static void handleSettingsPage() {
  auto pinSel = [](uint8_t current)->String{
    struct Item{int val; const char* label;};
    const Item items[] = {
      {16,"D0 (GPIO16) ⚠ no PWM"}, {5,"D1 (GPIO5)"}, {4,"D2 (GPIO4)"},
      {0,"D3 (GPIO0) ⚠ boot"}, {2,"D4 (GPIO2) ⚠ boot"}, {14,"D5 (GPIO14)"},
      {12,"D6 (GPIO12)"}, {13,"D7 (GPIO13)"}, {15,"D8 (GPIO15) ⚠ boot"}
    };
    String s = "<select name='pin'>";
    for (auto &it: items) {
      s += optionSel(it.val, current) + it.label + "</option>";
    }
    s += "</select>";
    return s;
  };

  auto boolSel = [](const char* name, bool val_true, const char* true_label, const char* false_label){
    String s = "<select name='"; s += name; s += "'>";
    s += String("<option value='1'") + (val_true?" selected":"") + ">" + true_label + "</option>";
    s += String("<option value='0'") + (!val_true?" selected":"") + ">" + false_label + "</option>";
    s += "</select>";
    return s;
  };

  String s = htmlHeader("Settings");
  s += F("<h2>Settings</h2>");

  // MQTT блок
  s += F("<h3>MQTT</h3><form method='post' action='/settings/save'>");
  s += "<div class='grid'>";

  s += "<div><label>Device name</label><input name='device_name' value='" + esc(cfg.device_name) + "'></div>";
  s += "<div><label>Base topic</label><input name='base_topic' value='" + esc(cfg.base_topic) + "'></div>";
  s += "<div><label>MQTT host</label><input name='mqtt_host' value='" + esc(cfg.mqtt_host) + "'></div>";
  s += "<div><label>MQTT port</label><input name='mqtt_port' value='" + String(cfg.mqtt_port) + "'></div>";
  s += "<div><label>MQTT user</label><input name='mqtt_user' value='" + esc(cfg.mqtt_user) + "'></div>";
  s += "<div><label>MQTT password (оставь пустым — без изменений)</label><input type='password' name='mqtt_pass' value=''></div>";

  s += "<div><label>Mode</label><select name='mode'>"
       "<option value='auto' "     + String(cfg.mode==MODE_AUTO ? "selected" : "")     + ">auto</option>"
       "<option value='external' " + String(cfg.mode==MODE_EXTERNAL ? "selected" : "") + ">external</option>"
       "</select></div>";

  s += "<div><label>sample_ms</label><input name='sample_ms' value='" + String(cfg.sample_ms) + "'></div>";
  s += "<div><label>confirm_samples</label><input name='confirm_samples' value='" + String(cfg.confirm_samples) + "'></div>";

  s += "<div><label>Web auth user (/update)</label><input name='web_user' value='" + esc(cfg.web_user) + "'></div>";
  s += "<div><label>Web auth pass (оставь пустым — без изменений)</label><input type='password' name='web_pass' value=''></div>";

  s += "</div>";

  // Пины/логика
  s += F("<div class='hr'></div><h3>Pins & Logic</h3><div class='grid'>");

  // Sensor 50%
  s += "<div><label>Sensor 50% pin</label>";
  s += pinSel(cfg.pin_sensor50);
  s += "<input type='hidden' name='pin_sensor50_marker' value='1'></div>";

  s += "<div><label>Sensor 50% TRUE when</label>";
  s += boolSel("s50_true_high", cfg.s50_true_high, "HIGH", "LOW");
  s += "</div>";

  s += "<div><label>Sensor 50% pull</label>";
  s += boolSel("s50_pullup", cfg.s50_pullup, "PULLUP", "NONE");
  s += "<div class='warn' style='margin-top:.3rem'>ESP8266 не поддерживает INPUT_PULLDOWN</div></div>";

  // Sensor 100%
  s += "<div><label>Sensor 100% pin</label>";
  s += pinSel(cfg.pin_sensor100);
  s += "<input type='hidden' name='pin_sensor100_marker' value='1'></div>";

  s += "<div><label>Sensor 100% TRUE when</label>";
  s += boolSel("s100_true_high", cfg.s100_true_high, "HIGH", "LOW");
  s += "</div>";

  s += "<div><label>Sensor 100% pull</label>";
  s += boolSel("s100_pullup", cfg.s100_pullup, "PULLUP", "NONE");
  s += "<div class='warn' style='margin-top:.3rem'>Избегай D3/D4/D8 если не уверен (boot-пины)</div></div>";

  // Factory (reset)
  s += "<div><label>Factory/Reset pin</label>";
  s += pinSel(cfg.pin_factory);
  s += "<input type='hidden' name='pin_factory_marker' value='1'></div>";

  s += "<div><label>Factory active when</label>";
  s += boolSel("factory_true_high", cfg.factory_true_high, "HIGH", "LOW");
  s += "</div>";

  s += "<div><label>Factory pull</label>";
  s += boolSel("factory_pullup", cfg.factory_pullup, "PULLUP", "NONE");
  s += "<div class='warn' style='margin-top:.3rem'>Для активного LOW обычно выбирают PULLUP</div></div>";

  s += "</div>";

  s += F("<div class='row'><button type='submit'>Сохранить и перезагрузить</button></div></form>");
  s += F("<p><a href='/'>Назад</a></p>");
  www.send(200, "text/html; charset=utf-8", s);
}

static void handleSettingsSave() {
  // MQTT / базовые
  auto argb = [&](const char* k){ String v = www.arg(k); v.trim(); return v; };

  String device_name = argb("device_name");
  String base_topic  = argb("base_topic");
  String mqtt_host   = argb("mqtt_host");
  String mqtt_port_s = argb("mqtt_port");
  String mqtt_user   = argb("mqtt_user");
  String mqtt_pass   = argb("mqtt_pass");
  String mode_s      = argb("mode");
  String sample_ms_s = argb("sample_ms");
  String confirm_s   = argb("confirm_samples");
  String web_user    = argb("web_user");
  String web_pass    = argb("web_pass");

  if (device_name.length()) strlcpy(cfg.device_name, device_name.c_str(), sizeof(cfg.device_name));
  if (base_topic.length())  strlcpy(cfg.base_topic,  base_topic.c_str(),  sizeof(cfg.base_topic));
  if (mqtt_host.length())   strlcpy(cfg.mqtt_host,   mqtt_host.c_str(),   sizeof(cfg.mqtt_host));
  if (mqtt_user.length())   strlcpy(cfg.mqtt_user,   mqtt_user.c_str(),   sizeof(cfg.mqtt_user));
  if (web_user.length())    strlcpy(cfg.web_user,    web_user.c_str(),    sizeof(cfg.web_user));

  if (mqtt_port_s.length()) {
    uint16_t p = (uint16_t) mqtt_port_s.toInt(); if (!p) p = 1883; cfg.mqtt_port = p;
  }
  if (mqtt_pass.length()) { strlcpy(cfg.mqtt_pass, mqtt_pass.c_str(), sizeof(cfg.mqtt_pass)); }
  if (web_pass.length())  { strlcpy(cfg.web_pass,  web_pass.c_str(),  sizeof(cfg.web_pass)); }

  cfg.mode = (mode_s == "external") ? MODE_EXTERNAL : MODE_AUTO;

  if (sample_ms_s.length())      { uint32_t v = (uint32_t) sample_ms_s.toInt(); if (!v) v = 50; cfg.sample_ms = v; }
  if (confirm_s.length())        { uint8_t v = (uint8_t)  confirm_s.toInt();   if (!v) v = 3;  cfg.confirm_samples = v; }

  // Пины/инверсии/подтяжки
  if (www.hasArg("pin_sensor50_marker"))  { cfg.pin_sensor50   = (uint8_t) www.arg("pin").toInt(); } // см. порядок форм — каждый блок формируется отдельно
  if (www.hasArg("s50_true_high"))        { cfg.s50_true_high  = www.arg("s50_true_high") == "1"; }
  if (www.hasArg("s50_pullup"))           { cfg.s50_pullup     = www.arg("s50_pullup") == "1"; }

  // Из-за одинакового name='pin' у трёх селектов читаем по порядку:
  // второй селект pin = sensor100
  int pin_fields_seen = 0;
  for (uint8_t i = 0; i < www.args(); ++i) {
    if (www.argName(i) == "pin") {
      if (pin_fields_seen == 0 && www.hasArg("pin_sensor50_marker")) { pin_fields_seen++; continue; }
      if (pin_fields_seen == 0) { cfg.pin_sensor50 = (uint8_t) www.arg(i).toInt(); pin_fields_seen++; continue; }
      if (pin_fields_seen == 1) { cfg.pin_sensor100 = (uint8_t) www.arg(i).toInt(); pin_fields_seen++; continue; }
      if (pin_fields_seen == 2) { cfg.pin_factory   = (uint8_t) www.arg(i).toInt(); pin_fields_seen++; continue; }
    }
  }

  if (www.hasArg("s100_true_high"))       { cfg.s100_true_high = www.arg("s100_true_high") == "1"; }
  if (www.hasArg("s100_pullup"))          { cfg.s100_pullup    = www.arg("s100_pullup") == "1"; }

  if (www.hasArg("factory_true_high"))    { cfg.factory_true_high = www.arg("factory_true_high") == "1"; }
  if (www.hasArg("factory_pullup"))       { cfg.factory_pullup    = www.arg("factory_pullup") == "1"; }

  saveConfig();
  rebootSoon("/settings");
}

void web_init() {
  if (MDNS.begin(cfg.device_name)) MDNS.addService("http", "tcp", 80);

  www.on("/", handleRoot);
  www.on("/reannounce", HTTP_GET, handleReannounce);
  www.on("/reboot",     HTTP_GET, handleReboot);

  // Wi-Fi
  www.on("/wifi",        HTTP_GET, handleWifiPage);
  www.on("/wifi/save",   HTTP_POST, handleWifiSave);
  www.on("/wifi/forget", HTTP_POST, handleWifiForget);

  // Settings (MQTT + Pins)
  www.on("/settings",      HTTP_GET,  handleSettingsPage);
  www.on("/settings/save", HTTP_POST, handleSettingsSave);

  // /update — OTA .bin
  if (cfg.web_user[0] && cfg.web_pass[0]) {
    httpUpdater.setup(&www, "/update", cfg.web_user, cfg.web_pass);
  } else {
    httpUpdater.setup(&www, "/update");
  }

  www.begin();
}

void web_loop() {
  www.handleClient();
  if (g_pending_reboot) {
    delay(1500);
    ESP.restart();
  }
}
