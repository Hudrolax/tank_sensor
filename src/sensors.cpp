#include "sensors.h"
#include <Arduino.h>
#include <math.h>  // fmodf, cosf

static uint8_t PIN50, PIN100, LEDP;
static bool TRUE_HIGH50, TRUE_HIGH100;
static uint32_t SAMPLE_MS;
static uint8_t CONFIRM_N;

static bool s50_on=false, s100_on=false;
static uint8_t c50=0, c100=0;
static bool cand50=false, cand100=false;
static bool first_sample=false;

static inline bool readLogic(uint8_t pin, bool true_high) {
  bool isHigh = (digitalRead(pin) == HIGH);
  return true_high ? isHigh : !isHigh;
}

void sensors_init(uint8_t pin50, bool true_high50, bool pullup50,
                  uint8_t pin100, bool true_high100, bool pullup100,
                  uint8_t led_pin, uint32_t sample_ms, uint8_t confirm_samples) {
  PIN50 = pin50; PIN100 = pin100; LEDP = led_pin;
  TRUE_HIGH50 = true_high50; TRUE_HIGH100 = true_high100;
  SAMPLE_MS = sample_ms; CONFIRM_N = confirm_samples;

  pinMode(PIN50,  pullup50  ? INPUT_PULLUP : INPUT);
  pinMode(PIN100, pullup100 ? INPUT_PULLUP : INPUT);
  pinMode(LEDP, OUTPUT);
  digitalWrite(LEDP, HIGH); // LED выкл (активен по LOW)

  s50_on  = readLogic(PIN50,  TRUE_HIGH50);
  s100_on = readLogic(PIN100, TRUE_HIGH100);
  first_sample = true;
}

void sensors_tick() {
  static uint32_t t_next = 0;
  uint32_t now = millis();
  if (t_next == 0) t_next = now;
  if ((int32_t)(now - t_next) < 0) return;
  t_next += SAMPLE_MS;

  bool raw50  = readLogic(PIN50,  TRUE_HIGH50);
  bool raw100 = readLogic(PIN100, TRUE_HIGH100);

  if (first_sample) {
    s50_on = raw50; s100_on = raw100;
    c50=0; c100=0; cand50=raw50; cand100=raw100;
    first_sample = false;
    return;
  }

  if (raw50 != s50_on) {
    if (c50 == 0) cand50 = raw50;
    if (raw50 == cand50) {
      if (++c50 >= CONFIRM_N) { s50_on = raw50; c50 = 0; }
    } else { cand50 = raw50; c50 = 1; }
  } else c50 = 0;

  if (raw100 != s100_on) {
    if (c100 == 0) cand100 = raw100;
    if (raw100 == cand100) {
      if (++c100 >= CONFIRM_N) { s100_on = raw100; c100 = 0; }
    } else { cand100 = raw100; c100 = 1; }
  } else c100 = 0;
}

bool sensors_s50()  { return s50_on; }
bool sensors_s100() { return s100_on; }
int  sensors_level(){ return s100_on ? 100 : (s50_on ? 50 : 0); }
bool sensors_error(){ return (!s50_on && s100_on); }

// LED-паттерны
void sensors_led_tick(uint32_t now_ms) {
  int level = sensors_level();
  bool error = sensors_error();

  if (!error) {
    if (level == 0) {
      digitalWrite(LEDP, HIGH);
    } else if (level == 50) {
      bool onPhase = ((now_ms / 1000) % 2) == 0; // 1 Гц
      digitalWrite(LEDP, onPhase ? LOW : HIGH);
    } else {
      digitalWrite(LEDP, LOW);
    }
    return;
  }

#if defined(ESP8266)
  if (level == 100 && LEDP != 16) {
    const float period_s = 0.2f; // 5 Гц
    float t = fmodf(now_ms / 1000.0f, period_s) / period_s; // 0..1
    float y = 0.5f * (1.0f - cosf(2.0f * 3.1415926f * t));
    int duty = (int)(y * 1023.0f);
    analogWrite(LEDP, 1023 - duty); // активен по LOW
    return;
  }
#endif
  if (level == 100) {
    bool onPhase = ((now_ms / 100) % 2) == 0; // 5 Гц
    digitalWrite(LEDP, onPhase ? LOW : HIGH);
  } else {
    const uint32_t period_ms = 3333; // ~0.3 Гц
    bool onPhase = (now_ms % period_ms) < (period_ms/2);
    digitalWrite(LEDP, onPhase ? LOW : HIGH);
  }
}
