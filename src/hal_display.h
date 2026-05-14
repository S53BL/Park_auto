// ============================================================
// hal_display.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.1-dev  |  Datum: 2026-04
// Faza    : 0 — ekran + touch
//
// POPRAVEK v2.0.1:
//   - Dodan #include <lvgl.h> — hal_display.h ga potrebuje za lv_obj_t
//     v forward deklaracijah. Brez tega screen_main.h in drugi moduli
//     ki vključijo samo hal_display.h dobijo "lv_obj_t undeclared".
// ============================================================
#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <stdint.h>
#include "config.h"

// ============================================================
// PODATKOVNI TIPI ZA UI POSODOBITVE
// ============================================================

enum class DisplaySsrState : uint8_t {
    OFF          = 0,
    ON           = 1,
    SSR_DISABLED = 2,
};

struct SsrDisplayData {
    DisplaySsrState state;
    uint32_t        countdown_s;
    bool            is_manual;
};

struct ParkingDisplayData {
    bool     occupied;
    char     vehicle_name[32];
    uint32_t parking_count;
    float    dtw_distance;
    uint8_t  tof_phase;      // 0=IDLE 1=DETECT 2=SCANNING 3=DTW_WAIT
    bool     tof_active;     // to parkirno mesto je trenutno aktivno
    uint16_t horiz_mm;       // razdalja horizontalnega TOF (0 = ni meritve)

    // B4 — vehicle_recog stanje
    // vr_place_state_t: 0=EMPTY_CAL, 1=EMPTY_UNCAL, 2=OCC_UNKNOWN, 3=OCC_KNOWN
    uint8_t  vr_state;
    bool     baseline_valid;
    float    last_dtw;       // NAN = ni
};

struct DayNightData {
    bool  is_night;
    float lux;
};

struct RadarDisplayData {
    uint8_t  strength_pct;
    bool     moving;
    bool     still;
};

enum class DisplayScreen : uint8_t {
    MAIN    = 0,
    SERVICE = 1,
    PARTY   = 2,
};

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

bool          hal_display_init();
bool          hal_display_ok();

// Thread-safe UI posodobitve
void hal_display_updateSsr(uint8_t idx, const SsrDisplayData& d);
void hal_display_updateParking(uint8_t idx, const ParkingDisplayData& d);
void hal_display_updateRadar(uint8_t idx, const RadarDisplayData& d);
void hal_display_updateLux(float lux, bool is_night);
void hal_display_updateTof(const uint16_t mm[6]);
void hal_display_updateSystemStats(uint32_t fh, uint32_t fp,
                                   uint8_t c0, uint8_t c1, uint32_t up);

// Navigacija
void          hal_display_showScreen(DisplayScreen s);
DisplayScreen hal_display_getCurrentScreen();

// Backlight
void          hal_display_setBacklight(uint8_t b);
uint8_t       hal_display_getBacklight();

// Diagnostika
uint32_t      hal_display_getFps();
bool          hal_display_isTouched();

// FreeRTOS task
void lvglTask(void* pvParams);
