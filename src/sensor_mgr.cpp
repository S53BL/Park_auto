// ============================================================
// sensor_mgr.cpp — Sensor Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 2 — hal_radar aktiven (IRQ-driven, Wire1 aktiven)
// ============================================================
//
// FAZA0: hal_tof, hal_radar, hal_light so zakomentirani.
// Odkomentiraj postopno ko priklapljaš hardware na Wire1.
//
// ============================================================

#include "sensor_mgr.h"
#include "event_bus.h"
#include <esp_task_wdt.h>

// FAZA0: Wire1 HAL includev zakomentirani
// #include "hal_tof.h"
// #include "hal_light.h"
// #include "hal_gpio.h"

// hal_radar_test.h — odstranjeno, test zaključen 2026-04

// FAZA 2: hal_radar aktiven
#include "hal_radar.h"

#include "logger.h"
#define SMGI(fmt, ...) LOG_INFO ("SENSOR", fmt, ##__VA_ARGS__)
#define SMGW(fmt, ...) LOG_WARN ("SENSOR", fmt, ##__VA_ARGS__)

static bool s_init_ok = false;

bool sensor_mgr_init() {
    SMGI("=== sensor_mgr_init — FAZA2 (hal_radar aktiven) ===");

    // FAZA0: še zakomentirani
    //   hal_tof_init()   — TCA9548A + 6× VL53L0X  (Wire1)
    //   hal_light_init() — BH1750                  (Wire1)
    //   hal_gpio_init()  — MCP23017                (Wire1) — v eventBusTask

    SMGW("FAZA0: hal_tof / hal_light preskočeni");

    SMGI("hal_radar_init...");
    bool radar_ok = hal_radar_init([](const RadarFrame& f) {
        // TODO: EventBus publish RADAR_DATA
        // event_bus_publish(EVT_RADAR_DATA, &f, sizeof(f));
        (void)f; // frame sprejet — EventBus publish pride v naslednji fazi
    });
    if (!radar_ok) {
        SMGW("hal_radar_init NAPAKA — sistem dela naprej brez radarja");
    } else {
        SMGI("hal_radar_init OK");
    }

    s_init_ok = true;
    SMGI("sensor_mgr_init OK");
    return true;
}

bool sensor_mgr_ok() { return s_init_ok; }

void sensorTask(void* pvParams) {
    SMGI("sensorTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    sensor_mgr_init();

    SMGI("sensorTask v zanki (FAZA2 — hal_radar IRQ-driven)");
    while (true) {
        esp_task_wdt_reset();

        // FAZA0: še zakomentirani
        // hal_tof_tick();
        // hal_light_tick();

        // hal_radar nima tick — IRQ-driven, radarTask bere sam

        // Periodična radar statistika (vsake 5 minut)
        static uint32_t last_radar_log = 0;
        if (millis() - last_radar_log > 300000) {
            last_radar_log = millis();
            hal_radar_log_stats();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
