// ============================================================
// screen_party.h — Party zaslon (LCD UI)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// VSEBINA:
//   Party zaslon za upravljanje WLED efektov na ESP-WLED modulu.
//   Dostopen s swipe desno z glavnega zaslona.
//
// ARHITEKTURA — WLED integracija:
//   screen_party.cpp publisha EventBus evente za vse akcije.
//   web_ui.cpp (wledTask) se subscribea na te evente in izvede HTTP klice na WLED ESP.
//
//   EventBus dogodki ki jih ta zaslon oddaja:
//     BUTTON_PARTY_TOGGLE      payload=1 (on) ali 0 (off)
//     BUTTON_PARTY_EFFECT      payload=fx_id (WLED fx ID)
//     BUTTON_PARTY_COLOR       payload=packed RGB: (R<<16)|(G<<8)|B
//     BUTTON_PARTY_BRIGHTNESS  payload=0-255
//     BUTTON_PARTY_SPEED       payload=0-255
//     BUTTON_PARTY_SLOT        payload=slot_idx (0-8)
//     BUTTON_PARTY_PRIORITY    payload=1 (on) ali 0 (off)
//
//   MUX IO45 preklop:
//     Party ON  → digitalWrite(PIN_MUX_SELECT, HIGH) → Party ESP aktiven
//     Party OFF → digitalWrite(PIN_MUX_SELECT, LOW)  → Primary ESP aktiven
//     Preklop izvede wledTask ob obdelavi TOGGLE/RESUME ukaza.
//     200ms zamik med MUX in WLED klicem (prepreči signal collision).
//
// WLED API referenca (za web_ui.cpp implementacijo):
//   POST http://<party_esp_ip>/json/state
//   Body primeri:
//     Vklop:      {"on": true}
//     Izklop:     {"on": false}
//     Efekt:      {"seg":[{"fx": <fx_id>}]}
//     Barva:      {"seg":[{"col":[[R,G,B]]}]}
//     Svetlost:   {"bri": <0-255>}
//     Hitrost:    {"seg":[{"sx": <0-255>}]}
//   Party ESP IP: konfigurabilen prek web_ui.cpp / config_mgr.cpp
//
// NITNA VARNOST:
//   screen_party_create() in screen_party_apply_updates() —
//   kliče samo lvglTask (Core1). Nikoli direktno iz drugega taska.
//
// ============================================================

#pragma once

#include <lvgl.h>
#include "hal_display.h"

// Stanje party zaslona — za sinhronizacijo z web_ui.cpp
struct PartyState {
    bool    party_on;
    uint8_t active_slot;  // 0-8 = aktiven slot; 0xFF = custom (ni slot)
    uint8_t fx_id;
    uint8_t brightness;
    uint8_t speed;
    uint32_t color_rgb;   // packed: (R<<16)|(G<<8)|B
};

// Glavne funkcije — klicati samo iz lvglTask
void screen_party_create(lv_obj_t* parent);
void screen_party_apply_updates();

// Posodobitev stanja iz zunaj (web_ui.cpp ko dobi WLED status)
// Thread-safe — prek interni mutex; nastavi dirty flag za lvglTask
void screen_party_set_state(const PartyState& state);

// Vrne trenutno stanje zaslona (za web_ui.cpp sinhronizacijo)
PartyState screen_party_get_state();

// Posodobi labele slot gumbov iz config_mgr (kliče samo lvglTask prek apply_updates)
void screen_party_reload_slots();

// Nastavi zahtevo za reload slotov — thread-safe, kliči iz kateregakoli taska.
// lvglTask jo pobere v naslednjem screen_party_apply_updates() klicu.
void screen_party_request_slot_reload();
