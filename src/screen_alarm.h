// ============================================================
// screen_alarm.h — LCD Alarm zaslon (LVGL)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// OPIS:
//   Ločen LVGL zaslon ki se prikaže ko je alarm ARMED ali TRIGGERED.
//   Zamenja screen_main (lv_scr_load) — normalni prikaz parkirišča
//   je popolnoma skrit med alarm modom.
//
//   ARMED:     rdeč zaslon, napis "ALARM AKTIVEN", majhen tekst
//              "Dotakni se" (brez razlage zakaj)
//   TRIGGERED: močnejši vizualni efekt (utripajoč okvir),
//              enaka interakcija
//
//   Ob dotiku → numerična tipkovnica za vnos PIN kode.
//   Pravilen PIN → alarm_disarm_pin() → screen_main naložen nazaj.
//   Napačen PIN → kratka "napačen PIN" animacija, znova čaka.
//
// KLICANJE:
//   screen_alarm_show()  — kliči iz LVGL timer callback ko
//                          alarm_get_state().active == true
//   screen_alarm_hide()  — kliči ko je alarm OFF
//   Oba klica MORATA biti iz LVGL konteksta (hal_display LVGL timer).
//
// ============================================================

#pragma once
#include <lvgl.h>
#include "alarm.h"

// Inicializacija — ustvari LVGL objekte (enkrat ob zagonu).
// Kliči iz hal_display_init() ali screen_main_init() po lv_init().
void screen_alarm_init();

// Pokaži alarm zaslon (lv_scr_load).
// state: trenutno stanje alarma (ARMED ali TRIGGERED)
void screen_alarm_show(AlarmStateEnum state);

// Skrij alarm zaslon — naloži screen_main nazaj.
void screen_alarm_hide();

// Posodobi vizualno stanje (ARMED ↔ TRIGGERED) brez ponovnega nalaganja.
// Kliči iz LVGL timer polling-a.
void screen_alarm_update(AlarmStateEnum state);

// Vrne true če je alarm zaslon trenutno aktiven.
bool screen_alarm_is_active();
