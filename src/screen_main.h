// ============================================================
// screen_main.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.1.0-dev  |  Datum: 2026-05
//
// SPREMEMBE v2.1.0:
//   E3.3 — dodana screen_main_set_daynight() deklaracija
// ============================================================
#pragma once
#include <lvgl.h>
#include "hal_display.h"

void screen_main_create(lv_obj_t* parent);
void screen_main_apply_updates();

// Direktni setterji — kliče hal_display.cpp iz ui_refresh_cb (LVGL timer kontekst)
void screen_main_set_ssr(uint8_t idx, const SsrDisplayData& data);
void screen_main_set_parking(uint8_t idx, const ParkingDisplayData& data);
void screen_main_set_radar(uint8_t idx, const RadarDisplayData& data);

// E3.3 — noč/dan topbar indikator
void screen_main_set_daynight(bool is_night, float lux);

// ============================================================
// Radar arc vizualizacija
// ============================================================

enum class RadarArcState : uint8_t {
    INACTIVE     = 0,
    IDLE         = 1,
    MOVING       = 2,
    STATIONARY   = 3,
    CONFIG_ERROR = 4,
};

struct RadarArcData {
    RadarArcState state;
    uint8_t       energy;
    uint16_t      dist_cm;
    bool          config_ok;
    bool          verified;
};

void screen_main_set_radar_arc(uint8_t idx, const RadarArcData& data);
