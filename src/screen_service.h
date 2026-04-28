// ============================================================
// screen_service.h — Servisni zaslon (LCD UI)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// VSEBINA:
//   Read-only diagnostični zaslon — dostopen s swipe levo z glavnega.
//   Scroll view z naslednjimi sekcijami od zgoraj navzdol:
//     1. Statistični boxci — lux, noč/dan, uptime
//     2. Fotocelice + rampa + vrata
//     3. TOF senzorji (6×, razdalje v mm)
//     4. I2C health (TCA9548A, MCP23017, SC16IS752×2, BH1750)
//     5. Sistemski podatki (SRAM, PSRAM, CPU, IP, FPS)
//
// PODATKI:
//   Vse vrednosti so placeholder (0 / "--") dokler ni zunanjega
//   hardwarea. Ko HAL moduli postanejo aktivni, kličejo:
//     hal_display_updateLux()        → sekcija 1 + 2
//     hal_display_updateTof()        → sekcija 3
//     hal_display_updateSystemStats()→ sekcija 5
//   I2C health (sekcija 4) se posodobi prek screen_service_set_i2c_health().
//
// NITNA VARNOST:
//   screen_service_create() in screen_service_apply_updates() se
//   kličeta SAMO iz lvglTask (Core1). Nikoli direktno iz drugega taska.
//   hal_display_update*() so thread-safe — pišejo v pending buffer.
//
// ============================================================

#pragma once

#include <lvgl.h>
#include "hal_display.h"

// I2C health stanje za prikaz
struct I2cHealthData {
    bool tca9548a_ok;      // TOF MUX
    bool mcp23017_ok;      // GPIO expander (SSR, vhodi)
    bool sc16is752_1_ok;   // UART bridge #1 (radar 1+2)
    bool sc16is752_2_ok;   // UART bridge #2 (radar 3+4)
    bool bh1750_ok;        // svetlobni senzor
};

// Glavne funkcije — klicati samo iz lvglTask
void screen_service_create(lv_obj_t* parent);
void screen_service_apply_updates();

// Posodobitev lokalnega bufferja — thread-safe, kliče hal_display iz apply_pending()
void screen_service_update_lux(float lux, bool night);
void screen_service_update_tof(const uint16_t mm[6]);
void screen_service_update_sys(uint32_t fh, uint32_t fp,
                                uint8_t c0, uint8_t c1, uint32_t up);

// Posodobitev I2C health — thread-safe (prek mutex)
void screen_service_set_i2c_health(const I2cHealthData& data);
