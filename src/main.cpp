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

  // Проверка режима точки доступа: если не подключены к WiFi (AP режим), перезагружаемся через 5 минут
  static uint32_t ap_mode_start = 0;
  const uint32_t AP_REBOOT_INTERVAL_MS = 5 * 60 * 1000; // 5 минут
  
  // Проверяем, находимся ли мы в режиме AP (не подключены к WiFi как станция)
  // Если нет локального IP (0.0.0.0), значит мы не подключены как станция
  bool is_ap_mode = (WiFi.localIP() == IPAddress(0, 0, 0, 0));
  
  if (is_ap_mode) {
    // Если только что вошли в AP режим, запоминаем время
    if (ap_mode_start == 0) {
      ap_mode_start = now;
      Serial.println("AP mode detected, will reboot in 5 minutes if still in AP mode");
    }
    
    // Проверяем, прошло ли 5 минут
    if ((int32_t)(now - ap_mode_start) >= (int32_t)AP_REBOOT_INTERVAL_MS) {
      Serial.println("AP mode timeout (5 min), rebooting...");
      delay(500);
      ESP.restart();
    }
  } else {
    // Если подключились к WiFi, сбрасываем таймер
    if (ap_mode_start != 0) {
      ap_mode_start = 0;
      Serial.println("Connected to WiFi, AP mode timer reset");
    }
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
