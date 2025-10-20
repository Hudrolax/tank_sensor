#pragma once
#include <Arduino.h>

// Инициализация с учётом инверсии/подтяжки
void sensors_init(uint8_t pin50, bool true_high50, bool pullup50,
                  uint8_t pin100, bool true_high100, bool pullup100,
                  uint8_t led_pin, uint32_t sample_ms, uint8_t confirm_samples);

void sensors_tick();

bool sensors_s50();
bool sensors_s100();
int  sensors_level();   // 0/50/100
bool sensors_error();   // 100% без 50% — ошибка

void sensors_led_tick(uint32_t now_ms);
