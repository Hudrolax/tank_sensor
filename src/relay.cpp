#include "relay.h"
#include "hardware.h"

static bool g_on = false;

void relay_init(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW); // по умолчанию выкл
  g_on = false;
}

void relay_set(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  g_on = on;
}

bool relay_get() {
  return g_on;
}
