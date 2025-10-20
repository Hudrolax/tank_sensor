#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

Config cfg;
const char* CFG_PATH = "/config.json";

bool loadConfig() {
  LittleFS.begin();
  if (!LittleFS.exists(CFG_PATH)) return false;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return false;

  DynamicJsonDocument d(4096);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();

  // Базовые
  strlcpy(cfg.device_name, d["device_name"] | cfg.device_name, sizeof(cfg.device_name));
  strlcpy(cfg.base_topic,  d["base_topic"]  | cfg.base_topic,  sizeof(cfg.base_topic));
  strlcpy(cfg.mqtt_host,   d["mqtt_host"]   | cfg.mqtt_host,   sizeof(cfg.mqtt_host));
  cfg.mqtt_port = d["mqtt_port"] | cfg.mqtt_port;
  strlcpy(cfg.mqtt_user, d["mqtt_user"] | cfg.mqtt_user, sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass, d["mqtt_pass"] | cfg.mqtt_pass, sizeof(cfg.mqtt_pass));
  strlcpy(cfg.web_user,  d["web_user"]  | cfg.web_user,  sizeof(cfg.web_user));
  strlcpy(cfg.web_pass,  d["web_pass"]  | cfg.web_pass,  sizeof(cfg.web_pass));
  cfg.sample_ms       = d["sample_ms"]       | cfg.sample_ms;
  cfg.confirm_samples = d["confirm_samples"] | cfg.confirm_samples;

  const char* mode_s  = d["mode"] | "auto";
  cfg.mode = (strcmp(mode_s, "external") == 0) ? MODE_EXTERNAL : MODE_AUTO;

  // Пины/логика (с дефолтами)
  cfg.pin_sensor50   = d["pin_sensor50"]   | cfg.pin_sensor50;
  cfg.pin_sensor100  = d["pin_sensor100"]  | cfg.pin_sensor100;
  cfg.pin_factory    = d["pin_factory"]    | cfg.pin_factory;

  cfg.s50_true_high    = d["s50_true_high"]    | cfg.s50_true_high;
  cfg.s100_true_high   = d["s100_true_high"]   | cfg.s100_true_high;
  cfg.factory_true_high= d["factory_true_high"]| cfg.factory_true_high;

  cfg.s50_pullup      = d["s50_pullup"]      | cfg.s50_pullup;
  cfg.s100_pullup     = d["s100_pullup"]     | cfg.s100_pullup;
  cfg.factory_pullup  = d["factory_pullup"]  | cfg.factory_pullup;

  return true;
}

bool saveConfig() {
  DynamicJsonDocument d(4096);

  d["device_name"]     = cfg.device_name;
  d["base_topic"]      = cfg.base_topic;
  d["mqtt_host"]       = cfg.mqtt_host;
  d["mqtt_port"]       = cfg.mqtt_port;
  d["mqtt_user"]       = cfg.mqtt_user;
  d["mqtt_pass"]       = cfg.mqtt_pass;
  d["web_user"]        = cfg.web_user;
  d["web_pass"]        = cfg.web_pass;
  d["sample_ms"]       = cfg.sample_ms;
  d["confirm_samples"] = cfg.confirm_samples;
  d["mode"]            = (cfg.mode == MODE_EXTERNAL) ? "external" : "auto";

  d["pin_sensor50"]    = cfg.pin_sensor50;
  d["pin_sensor100"]   = cfg.pin_sensor100;
  d["pin_factory"]     = cfg.pin_factory;

  d["s50_true_high"]     = cfg.s50_true_high;
  d["s100_true_high"]    = cfg.s100_true_high;
  d["factory_true_high"] = cfg.factory_true_high;

  d["s50_pullup"]      = cfg.s50_pullup;
  d["s100_pullup"]     = cfg.s100_pullup;
  d["factory_pullup"]  = cfg.factory_pullup;

  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) return false;
  serializeJsonPretty(d, f);
  f.close();
  return true;
}
