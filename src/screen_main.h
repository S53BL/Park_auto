// ============================================================
// screen_main.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.1-dev  |  Datum: 2026-04
//
// POPRAVEK v2.0.1:
//   - Odstranjeno #include "hal_display.h" — screen_main.h
//     NE vključuje hal_display.h, ker hal_display.cpp že vključuje
//     screen_main.h prek extern deklaracij. Krožni include bi povzročil
//     "SsrDisplayData redeclared" ali "lv_obj_t undeclared" napake.
//   - hal_display.h zdaj sam vključuje <lvgl.h>, zato screen_main.h
//     dobi lv_obj_t* prek #include <lvgl.h> direktno.
//   - screen_main.h ostaja neodvisen header — vključuje samo kar res rabi.
// ============================================================
#pragma once
#include <lvgl.h>
#include "hal_display.h"

void screen_main_create(lv_obj_t* parent);
void screen_main_apply_updates();

// Direktni setterji — kliče hal_display.cpp iz apply_pending()
void screen_main_set_ssr(uint8_t idx, const SsrDisplayData& data);
void screen_main_set_parking(uint8_t idx, const ParkingDisplayData& data);
void screen_main_set_radar(uint8_t idx, const RadarDisplayData& data);
