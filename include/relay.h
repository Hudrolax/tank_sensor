#pragma once
#include <Arduino.h>

void relay_init(uint8_t pin);
void relay_set(bool on);
bool relay_get();
