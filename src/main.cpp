#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>

#include "hardware.h"
#include "config.h"
#include "sensors.h"
#include "relay.h"
#include "mqtt.h"
#include "web.h"

// --- заводской сброс ---
static void factoryReset() {
  for (int i=0;i<6;i++){ digitalWrite(LED_PIN, LOW); delay(150); digitalWrite(LED_PIN, HIGH); delay(150); }
  LittleFS.begin(); LittleFS.remove(CFG_PATH);
  WiFi.persistent(true); WiFi.disconnect(true); delay(200); WiFi.persistent(false);
  WiFiManager wm; wm.resetSettings();
  delay(300); ESP.restart();
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200); delay(50);

  LittleFS.begin();
  loadConfig(); // грузим ранним этапом (для кастомного factory pin)

  // Настраиваем пин сброса по конфигу
  pinMode(cfg.pin_factory, cfg.factory_pullup ? INPUT_PULLUP : INPUT);
  auto factoryActive = [&](){
    bool isHigh = (digitalRead(cfg.pin_factory) == HIGH);
    return cfg.factory_true_high ? isHigh : !isHigh;
  };

  // Окно удержания для заводского сброса
  if (factoryActive()) {
    unsigned long t0 = millis(); bool stillActive = true;
    while (millis() - t0 < FACTORY_HOLD_MS) {
      if (!factoryActive()) { stillActive = false; break; }
      digitalWrite(LED_PIN, (millis()/200)%2 ? LOW : HIGH);
      delay(10);
    }
    digitalWrite(LED_PIN, HIGH);
    if (stillActive) factoryReset();
  }

  // Датчики
  sensors_init(
    cfg.pin_sensor50,  cfg.s50_true_high,  cfg.s50_pullup,
    cfg.pin_sensor100, cfg.s100_true_high, cfg.s100_pullup,
    LED_PIN, cfg.sample_ms, cfg.confirm_samples
  );

  // Реле выкл по умолчанию
  relay_init(PIN_RELAY);
  relay_set(false);

  // Wi-Fi: если не подключилось — бесконечный AP-портал до конфигурации
  WiFiManager wm;
  char apName[32]; snprintf(apName, sizeof(apName), "Tank-%06X", ESP.getChipId() & 0xFFFFFF);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(0);
  wm.autoConnect(apName);

  // Веб + OTA
  web_init();

  // MQTT
  mqtt_init();
}

void loop() {
  // сервисы
  web_loop();
  mqtt_loop();

  // датчики и LED
  sensors_tick();
  sensors_led_tick(millis());

  // авто-управление насосом
  if (cfg.mode == MODE_AUTO) {
    bool want_on = !sensors_s100(); // нет 100% — насос включен
    if (want_on != relay_get()) {
      relay_set(want_on);
    }
  }

  // Дифф-публикация статусов (раз в ~1 c достаточно)
  static uint32_t t_pub = 0;
  uint32_t now = millis();
  if ((int32_t)(now - t_pub) >= 1000) {
    t_pub = now;
    if (mqtt_online()) mqtt_publish_diff();
  }

  // Отладочный лог — раз в секунду
  static uint32_t t_log = 0;
  if ((int32_t)(now - t_log) >= 1000) {
    t_log = now;
    Serial.printf("s50=%d s100=%d level=%d error=%d relay=%d mode=%s mqtt=%d\n",
      (int)sensors_s50(), (int)sensors_s100(), sensors_level(), (int)sensors_error(),
      (int)relay_get(), (cfg.mode==MODE_EXTERNAL) ? "EXTERNAL" : "AUTO", (int)mqtt_online());
  }
}
