#pragma once
#include <Arduino.h>

void mqtt_init();
void mqtt_loop();
bool mqtt_online();

void mqtt_reannounce();     // discovery + актуальные стейты (ручной вызов)
void mqtt_publish_all();    // полный пакет (используем только при первом коннекте/реанонсе)
void mqtt_publish_diff();   // публикует только изменившиеся значения + редкий heartbeat атрибутов
