// ============================================================
// sensor_mgr.cpp — Sensor Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 3 — hal_radar + hal_tof aktivna (Wire1)
// ============================================================
//
// FAZA0: hal_tof, hal_radar, hal_light so zakomentirani.
// Odkomentiraj postopno ko priklapljaš hardware na Wire1.
//
// ============================================================

#include "sensor_mgr.h"
#include "config.h"
#include "event_bus.h"
#include <esp_task_wdt.h>

#include "hal_tof.h"
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
    SMGI("=== sensor_mgr_init — FAZA3 (hal_radar + hal_tof) ===");

    // FAZA0: še zakomentirani
    //   hal_light_init() — BH1750                  (Wire1)
    //   hal_gpio_init()  — MCP23017                (Wire1) — v eventBusTask

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

    // hal_tof init — TCA9548A + 6× VL53L1X na Wire1
    SMGI("hal_tof_init...");
    hal_tof_setProfileCallback([](const TofProfileResult& profile) {
        // TODO: event_bus_publish(EVT_TOF_PROFILE_READY, &profile, sizeof(profile));
        SMGI("TOF_PROFILE_READY — mesto:%c točke:%d trajanje:%lu ms",
             (profile.place == TOF_PLACE_A) ? 'A' : 'B',
             profile.count,
             (unsigned long)profile.scan_duration_ms);
        (void)profile;
    });
    bool tof_ok = hal_tof_init();
    if (!tof_ok) {
        SMGW("hal_tof_init NAPAKA — sistem teče brez TOF (identifikacija vozil onemogočena)");
    } else {
        SMGI("hal_tof_init OK");
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

    SMGI("sensorTask v zanki (FAZA3 — hal_radar + hal_tof)");
    static uint32_t last_tof_tick_ms = 0;

    while (true) {
        esp_task_wdt_reset();

        // --- hal_tof tick s fazno-odvisnim intervalom ---
        // IDLE:     tick preverja watchdog timer — dejanska meritev samo vsakih 10 min
        // DETECT:   50 ms — H_A + H_B za detekcijo mesta
        // SCANNING: 0 ms — čim prej, naravni I2C cikel (~120ms) je throttle
        // DTW_WAIT: 500 ms — samo WDT reset, brez Wire1
        uint32_t now = millis();
        uint32_t tof_interval;
        switch (hal_tof_getPhase()) {
            case TOF_PHASE_DETECT:   tof_interval = TOF_POLL_DETECT_MS;       break;
            case TOF_PHASE_SCANNING: tof_interval = 0;                        break;
            case TOF_PHASE_DTW_WAIT: tof_interval = 500;                      break;
            default:                 tof_interval = TOF_WATCHDOG_INTERVAL_MS; break;
        }
        if ((now - last_tof_tick_ms) >= tof_interval) {
            last_tof_tick_ms = now;
            hal_tof_tick();

            // Radar recovery check — samo v IDLE fazi, sinhrono s TOF watchdog-om.
            // V IDLE fazi hal_tof_tick() izvede watchdog meritev (vsakih 10 min)
            // ali takoj vrne brez Wire1 dostopa (med čakanjem na watchdog).
            // Recovery check se izvede v obeh primerih — overhead je zanemarljiv
            // ker preverimo samo digitalRead() in samo po potrebi vzamemo mutex.
            //
            // TODO: ko bo EventBus integriran, preveriti frames_ok counter preden
            //   kličemo recovery — če radar normalno dobiva frames, pin LOW ni
            //   napaka in recovery preskoči (glej komentar v hal_radar_recovery_check).
            if (hal_tof_getPhase() == TOF_PHASE_IDLE) {
                hal_radar_recovery_check();
            }
        }

        // --- Periodična radar statistika (vsake 5 minut) ---
        static uint32_t last_radar_log = 0;
        if (millis() - last_radar_log > 300000) {
            last_radar_log = millis();
            hal_radar_log_stats();
            hal_tof_logStats();
        }

        if (hal_tof_getPhase() != TOF_PHASE_SCANNING) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            taskYIELD();
        }
    }
}
