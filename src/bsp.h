// ============================================================
// bsp.h — Board Support Package
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — Ekran + touch (Wire1 izklopljen)
// ============================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/semphr.h>
#include "config.h"

// FreeRTOS task handle-i
extern TaskHandle_t hTaskEventBus;
extern TaskHandle_t hTaskSensor;
extern TaskHandle_t hTaskLed;
extern TaskHandle_t hTaskLvgl;
extern TaskHandle_t hTaskApp;
extern TaskHandle_t hTaskWifi;

// Inicializacija
void bsp_init();

// Status
bool bsp_wire1_ok();    // Wire1 (senzorski bus) — Faza 0: vedno false
bool bsp_mcp_ok();      // MCP23017 — Faza 0: vedno false

// Wire1 mutex — kreiran v bsp_init, uporablja hal_gpio + hal_light
// Faza 0: mutex obstaja ampak Wire1 klici so zakomentirani
SemaphoreHandle_t bsp_get_wire1_mutex();

// TCA9548A hardware reset (IO46) — za kasnejše faze
void bsp_tca_reset();

// Boot čas v ms
uint32_t bsp_boot_time_ms();
