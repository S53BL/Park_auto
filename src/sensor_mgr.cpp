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
#include "bsp.h"
#include "config.h"
#include "event_bus.h"
#include <esp_task_wdt.h>

#include "hal_tof.h"
#include "hal_light.h"
// #include "hal_gpio.h"
#include "config_mgr.h"
#include "vehicle_recog.h"   // vehicle_recog_feed_profile(), reconcile_state()

// hal_radar_test.h — odstranjeno, test zaključen 2026-04

// FAZA 2: hal_radar aktiven
#include "hal_radar.h"

#include "logger.h"
#define SMGI(fmt, ...) LOG_INFO ("SENSOR", fmt, ##__VA_ARGS__)
#define SMGW(fmt, ...) LOG_WARN ("SENSOR", fmt, ##__VA_ARGS__)

static bool s_init_ok = false;

bool sensor_mgr_init() {
    SMGI("=== sensor_mgr_init — FAZA3 (hal_radar + hal_tof) ===");

    //   hal_gpio_init()  — MCP23017                (Wire1) — v eventBusTask

    // hal_light init — BH1750 senzor svetlobe na Wire1
    SMGI("hal_light_init...");
    bool light_ok = hal_light_init();
    if (!light_ok) {
        SMGW("hal_light_init NAPAKA — sistem dela naprej brez senzorja svetlobe (is_night=false)");
    } else {
        SMGI("hal_light_init OK");
    }

    SMGI("hal_radar_init...");
    // Persistence filter stanje — per kanal
    static uint8_t  s_persist_count[4]    = {0};
    static uint8_t  s_persist_det[4]      = {0};
    static uint32_t s_persist_clear_ms[4] = {0};

    bool radar_ok = hal_radar_init([](const RadarFrame& f) {
        uint8_t ch = (uint8_t)f.sensor_id;
        if (ch >= 4) return;

        const Config cfg = config_get();
        uint8_t persist_n = cfg.radar_persistence_n;

        if (f.detection == 0) {
            if (s_persist_count[ch] > 0) {
                s_persist_count[ch] = 0;
                s_persist_clear_ms[ch] = millis();
                uint32_t payload = (uint32_t)ch;
                EventBus::publish(EventType::RADAR_MOTION, payload);
            }
            return;
        }

        s_persist_count[ch]++;
        s_persist_det[ch] = f.detection;

        if (persist_n == 0 || s_persist_count[ch] >= persist_n) {
            bool motion     = (f.detection == 1 || f.detection == 3);
            bool stationary = (f.detection == 2 || f.detection == 3);
            if (stationary && !motion && cfg.radar_static_sens[ch] == 0) return;
            uint32_t payload = ((uint32_t)ch & 0xFF)
                             | (motion     ? (1u << 8) : 0u)
                             | (stationary ? (1u << 9) : 0u);
            EventBus::publish(EventType::RADAR_MOTION, payload);
        }
    });
    if (!radar_ok) {
        SMGW("hal_radar_init NAPAKA — sistem dela naprej brez radarja");
    } else {
        SMGI("hal_radar_init OK");
    }

    // hal_tof init — TCA9548A + 6× VL53L1X na Wire1
    SMGI("hal_tof_init...");
    hal_tof_setProfileCallback([](const TofProfileResult& profile) {
        char pid = (profile.place == TOF_PLACE_A) ? 'A' : 'B';
        SMGI("TOF_PROFILE_READY — mesto:%c tocke:%d trajanje:%lu ms",
             pid, (int)profile.count,
             (unsigned long)profile.scan_duration_ms);

        // KORAK 1: Posreduj cel profil vehicle_recog PRED EventBus publishom.
        // vehicle_recog_feed_profile() kopira tocke v PSRAM interni buffer —
        // po vrnitvi je profil (na hal_tof stacku) varno osvoboditi.
        // Klic je sinhronen in hiter (~1-2 ms za 120 tock, samo memcpy).
        vehicle_recog_feed_profile(profile);

        // KORAK 2: EventBus publish TOF_PROFILE_READY (payload = 0=A, 1=B).
        // vehicle_recog::on_event(TOF_PROFILE_READY) posodobi UI fazo na
        // VR_PHASE_DTW_COMPUTE. DTW se izvede v vehicle_recog_tick() (appTask).
        uint32_t payload = (profile.place == TOF_PLACE_A) ? 0u : 1u;
        EventBus::publish(EventType::TOF_PROFILE_READY, payload);
    });
    SMGI("TOF profil callback registriran");

    // DOOR_OPENED: rampagor LOW → sproži hal_tof fazni avtomat IDLE/DTW_WAIT → DETECT.
    // hal_tof bo začel meriti H_A in H_B vsake ~40 ms in prehod v SCANNING
    // ko eden od H < VEH_ENTRY_THRESH_MM (350 cm).
    // Klic je idempotenten — ce je hal_tof ze v DETECT, ignorira (z WARN logom).
    EventBus::subscribe(EventType::DOOR_OPENED, [](const Event&) {
        hal_tof_startDetect();
        SMGI("DOOR_OPENED → hal_tof_startDetect()");
    });
    SMGI("DOOR_OPENED subscriber registriran");

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

bool sensor_mgr_read_place_now(char id, uint16_t* h, uint16_t* p1, uint16_t* p2) {
    if (!h || !p1 || !p2) return false;
    if (!hal_tof_ok()) return false;
    TofPlace place = (id == 'A') ? TOF_PLACE_A : TOF_PLACE_B;
    TofProfilePoint pt = hal_tof_readAll(place);
    if (pt.H_mm == TOF_ERR || pt.P1_mm == TOF_ERR || pt.P2_mm == TOF_ERR) {
        SMGW("read_place_now(%c): TOF_ERR na vsaj enem senzorju", id);
        return false;
    }
    *h  = pt.H_mm;
    *p1 = pt.P1_mm;
    *p2 = pt.P2_mm;
    return true;
}

void sensorTask(void* pvParams) {
    SMGI("sensorTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    sensor_mgr_init();

    SMGI("sensorTask v zanki (FAZA3 — hal_radar + hal_tof)");
    static uint32_t last_tof_tick_ms = 0;
    static uint32_t last_light_tick_ms = 0;

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
                // Periodicno preverjanje prisotnosti (vsake TOF_WATCHDOG_INTERVAL_MS):
                // Popravi vehicle_recog state machine glede na aktualne meritve
                // (npr. zagon z vozilom na mestu, ali odhod ki ga VEHICLE_DEPARTED ni ujel).
                uint16_t h, p1, p2;
                const char places[2] = {'A', 'B'};
                for (int i = 0; i < 2; i++) {
                    if (sensor_mgr_read_place_now(places[i], &h, &p1, &p2)) {
                        vehicle_recog_reconcile_state(places[i], h, p1, p2);
                    }
                }
            }
        }

        // --- hal_light tick (BH1750 svetloba) ---
        // Ne med TOF_PHASE_SCANNING — Wire1 je takrat zaseden s TCA9548A.
        TofPhase cur_phase = hal_tof_getPhase();
        if (cur_phase != TOF_PHASE_SCANNING && cur_phase != TOF_PHASE_DTW_WAIT) {
            if ((millis() - last_light_tick_ms) >= LIGHT_POLL_SENSOR_MS) {
                last_light_tick_ms = millis();
                hal_light_tick();
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
