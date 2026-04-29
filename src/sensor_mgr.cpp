// ============================================================
// sensor_mgr.cpp — Sensor Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — vsi HAL sensory zakomentirani (Wire1 neaktiven)
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
// #include "hal_radar.h"
// #include "hal_light.h"
// #include "hal_gpio.h"

#include "logger.h"
#define SMGI(fmt, ...) LOG_INFO ("SENSOR", fmt, ##__VA_ARGS__)
#define SMGW(fmt, ...) LOG_WARN ("SENSOR", fmt, ##__VA_ARGS__)

static bool s_init_ok = false;

bool sensor_mgr_init() {
    SMGI("=== sensor_mgr_init — FAZA0 (vsi senzorji zakomentirani) ===");

    // FAZA0: Wire1 senzorji zakomentirani
    //   hal_tof_init()   — TCA9548A + 6× VL53L0X  (Wire1)
    //   hal_radar_init() — SC16IS752 + 4× LD2410C  (Wire1)
    //   hal_light_init() — BH1750                  (Wire1)
    //   hal_gpio_init()  — MCP23017                (Wire1) — v eventBusTask
    //
    // Odkomentiraj po eno v Fazi 1 ko Wire1 postane aktiven:
    //   1. hal_light_init() — samo en čip, najlažji test
    //   2. hal_gpio_init()
    //   3. hal_tof_init()
    //   4. hal_radar_init()

    SMGW("FAZA0: hal_tof / hal_radar / hal_light preskočeni");
    s_init_ok = true;
    SMGI("sensor_mgr_init OK (prazno)");
    return true;
}

bool sensor_mgr_ok() { return s_init_ok; }

void sensorTask(void* pvParams) {
    SMGI("sensorTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    sensor_mgr_init();

    SMGI("sensorTask v zanki (FAZA0 — nič ne dela)");
    while (true) {
        esp_task_wdt_reset();

        // FAZA0: vsi HAL ticki zakomentirani
        // hal_tof_tick();
        // hal_radar_tick();
        // hal_light_tick();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
