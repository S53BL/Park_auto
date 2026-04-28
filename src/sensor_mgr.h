// ============================================================
// sensor_mgr.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Faza 0 — stub
// ============================================================
#pragma once
#include <Arduino.h>

bool sensor_mgr_init();
bool sensor_mgr_ok();
void sensorTask(void* pvParams);
