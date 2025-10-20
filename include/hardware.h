#pragma once
#include <Arduino.h>

// NodeMCU v2: D1=GPIO5 (100%), D2=GPIO4 (реле), D5=GPIO14 (50%)
static const uint8_t PIN_SENSOR100 = D1;  // HIGH = бак 100%
static const uint8_t PIN_SENSOR50  = D5;  // HIGH = бак >=50%
static const uint8_t PIN_RELAY     = D2;  // HIGH = включить насос
static const uint8_t LED_PIN       = LED_BUILTIN; // активен по LOW

static const uint8_t FACTORY_PIN   = D7;  // удерживать LOW ~5 c при старте
static const unsigned long FACTORY_HOLD_MS = 5000;
