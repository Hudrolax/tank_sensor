#pragma once
#include <Arduino.h>
#include "hardware.h"  // для значений по умолчанию D-пинов

enum ControlMode : uint8_t { MODE_AUTO = 0, MODE_EXTERNAL = 1 };

struct Config {
  // Идентификация / MQTT
  char     device_name[32] = "tank-sensor";
  char     base_topic[64]  = "home/tank";
  char     mqtt_host[64]   = "";
  uint16_t mqtt_port       = 1883;
  char     mqtt_user[32]   = "";
  char     mqtt_pass[32]   = "";

  // Web auth для /update (пусто = без авторизации)
  char     web_user[32]    = "";
  char     web_pass[32]    = "";

  // Сенсоры / дискретизация
  uint32_t sample_ms       = 50;
  uint8_t  confirm_samples = 3;

  ControlMode mode         = MODE_AUTO;

  // ---- Новые: настраиваемые пины и логика ----
  // Пины (используем Arduino-номера, например D1=5, D5=14, D7=13)
  uint8_t  pin_sensor50    = PIN_SENSOR50;   // по умолчанию D5
  uint8_t  pin_sensor100   = PIN_SENSOR100;  // по умолчанию D1
  uint8_t  pin_factory     = FACTORY_PIN;    // по умолчанию D7

  // TRUE когда HIGH? (иначе TRUE когда LOW)
  bool     s50_true_high   = true;
  bool     s100_true_high  = true;
  bool     factory_true_high = false;        // исторически активен по LOW

  // Подтяжка (ESP8266: есть только PULLUP)
  bool     s50_pullup      = true;
  bool     s100_pullup     = true;
  bool     factory_pullup  = true;
};

extern Config cfg;

bool loadConfig();
bool saveConfig();

extern const char* CFG_PATH;
