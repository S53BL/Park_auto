// ============================================================
// main.cpp — Entry point
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — Ekran + touch (Wire1 izklopljen)
// ============================================================
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "bsp.h"

void setup() {
    // Vidni boot signal — backlight utripa 6× da potrdimo da ESP živi
    pinMode(6, OUTPUT);
    for (int i = 0; i < 6; i++) {
        digitalWrite(6, HIGH); delay(200);
        digitalWrite(6, LOW);  delay(200);
    }
    bsp_init();
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
}

void loop() {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
}
