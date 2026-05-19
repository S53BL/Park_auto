// ============================================================
// screen_main.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.2.0  |  Datum: 2026-05
//
// SPREMEMBE v2.2.0 (ARC4x spec):
//   - RadarArcData: dodana polja dist_cm (razdalja per tip),
//     is_permanent_error (trajna vs začasna napaka)
//   - RadarArcState: IDLE preimenovan v INACTIVE (senzor brez detekcije),
//     dodan komentar za vsako stanje
//   - screen_main_set_radar_arc() — nova implementacija v .cpp
// ============================================================
#pragma once
#include <lvgl.h>
#include "hal_display.h"

void screen_main_create(lv_obj_t* parent);
void screen_main_apply_updates();

// Vrne referenco na screen_main LVGL objekt (za screen_alarm_hide).
lv_obj_t* screen_main_get_screen();

// Direktni setterji — kliče hal_display.cpp iz ui_refresh_cb (LVGL timer kontekst)
void screen_main_set_ssr(uint8_t idx, const SsrDisplayData& data);
void screen_main_set_parking(uint8_t idx, const ParkingDisplayData& data);
void screen_main_set_radar(uint8_t idx, const RadarDisplayData& data);

// E3.3 — noč/dan topbar indikator
void screen_main_set_daynight(bool is_night, float lux);

// Posodobi SSR label po spremembi config (web UI)
void screen_main_set_ssr_label(uint8_t idx, const char* label);

// ============================================================
// Radar arc vizualizacija — ARC4x spec
// ============================================================

enum class RadarArcState : uint8_t {
    INACTIVE     = 0,   // Senzor aktiven, detection=0 — nič ne zaznava (siva, fill 10%)
    MOVING       = 1,   // detection=1 ali 3 — gibanje zaznano (zelena, fill=moving_energy)
    STATIONARY   = 2,   // detection=2 — statična prisotnost (modra, fill=static_energy)
    CONFIG_ERROR = 3,   // Napaka konfiguracije ali senzor ni aktiven (rdeča)
};

struct RadarArcData {
    RadarArcState state;
    uint8_t       energy;           // fill vrednost arc-a (0–100)
    uint16_t      dist_cm;          // razdalja za prikaz v sredini arc-a
    bool          is_permanent_error; // true = senzor ni inicializiran (arc 100%),
                                      // false = začasna napaka (arc 50%)
};

void screen_main_set_radar_arc(uint8_t idx, const RadarArcData& data);

// Vizualna pozicija praga na arcu (% od max).
// Ko energy doseže to vrednost, prag je presežen → peak sproži.
// Ista vrednost se uporablja v hal_display.cpp za skaliranje energije.
#define RAD_THRESHOLD_PCT  75
