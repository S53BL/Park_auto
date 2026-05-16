// ============================================================
// led_manager.h — LED Manager: FastLED animacije + MUX preklop
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ODGOVORNOST:
//   Upravljanje dveh LED verig:
//
//   1. GLAVNA MATRIKA (IO39 → 74HC257 MUX → 10 vzporednih podverig)
//      - Firmware vidi 90 logičnih LED (WS2815)
//      - Fizično je vsaka pomnožena ×10 prek MUX-a = 900 LED skupaj
//      - Animacije: fill (0→89), unfill (89→0), fade-out, statična barva
//      - MUX selektor IO45: LOW = FastLED aktiven, HIGH = WLED (party mode)
//      ⚠ FastLED.show() se NIKOLI ne kliče ko je IO45 HIGH!
//
//   2. SIGNALNA LED (IO40 → direktno → 144 LED WS2815)
//      - Razdeljena v 3 cone: BOT (0–47), MID (48–95), TOP (96–143)
//      - Parking assist: barvna lestvica glede na razdaljo H senzorja
//      - Prikaz ure, celica timer, animacije
//      - Vedno pod FastLED kontrolo (ni MUX, ni WLED)
//
// ARHITEKTURA:
//   - led_mgr_init()   : inicializacija FastLED, MUX pin, queue
//   - ledTask()        : FreeRTOS task na Core1 — procesira animacije
//   - Vse animacije so neblokirajoče (state machine v ledTask zanki)
//   - Caller (light_logic, sensor_mgr) kliče API funkcije ki pošljejo
//     ukaze v interno FreeRTOS queue. Animacija se izvede v ledTask.
//
// MUX PREKLOP:
//   led_mgr_set_party_mode(true)  → IO45 HIGH → WLED prevzame matriko
//   led_mgr_set_party_mode(false) → IO45 LOW  → FastLED prevzame matriko
//   MUX_SWITCH_DELAY_MS pavza zagotovi da WLED/FastLED ne hkrati poganjata signal.
//
// THREAD SAFETY:
//   Vse javne funkcije so thread-safe — pišejo v FreeRTOS queue.
//   ledTask je edini pisec v FastLED bufferje.
//   config_get() se kliče v ledTask kontekstu (thread-safe).
//
// ODVISNOSTI:
//   FastLED @ ^3.7.0 (lib_deps)
//   config_mgr.h, logger.h, config.h
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>

// ============================================================
// PARKING PLACE
// ============================================================

enum class ParkPlace : uint8_t {
    A = 0,
    B = 1
};

// ============================================================
// JAVNI API — Glavna matrika
// ============================================================

// Inicializacija — kliči iz ledTask ob zagonu (ne iz bsp_init direktno).
// Vrne true ob uspehu.
bool led_mgr_init();

// Preklop med FastLED (normal) in WLED (party) načinom.
// party=true  → IO45 HIGH → matrika pod WLED kontrolo (FastLED ne piše!)
// party=false → IO45 LOW  → matrika pod FastLED kontrolo
// Vedno kliči to funkcijo za preklop — ne postavljaj IO45 direktno!
void led_mgr_set_party_mode(bool party);

// Vrne true če je party mode aktiven (IO45 HIGH).
bool led_mgr_is_party_mode();

// Fill animacija: postopno polnjenje matrike od LED 0 do 89.
// speed_ms = skupni čas za fill celotne matrike (iz config.fill_speed_ms).
// Kliče se ob SSR1 vklopu (svetloba ON).
// Neblokirajoče — animacija teče v ledTask.
void led_mgr_fill(uint32_t speed_ms = 0);   // 0 = vzame iz config

// Unfill animacija: postopno gašenje matrike od LED 89 do 0.
// speed_ms = skupni čas (iz config.unfill_speed_ms).
// Kliče se ob SSR1 izklopu (svetloba OFF) — pred dejanskim SSR izklopom.
void led_mgr_unfill(uint32_t speed_ms = 0);

// Fade-out: postepno zniževanje svetlosti celotne matrike na 0.
// duration_ms = čas fade-out (iz config.fade_duration_ms).
// Po fade-out: matrika ostane OFF (brightness=0).
void led_mgr_fade_out(uint32_t duration_ms = 0);

// Takojšen vklop matrike na polno svetlost (brez animacije).
// Barva: topla bela (WS2815 optimizirana vrednost).
// Svetlost: config.target_brightness (ali brightness_night če je noč).
void led_mgr_on_immediate();

// Takojšen izklop matrike (brez animacije).
void led_mgr_off_immediate();

// Nastavi svetlost matrike (0–255).
// Kliče se ob noč/dan prehodu (light_logic).
// Neblokirajoče — uveljavi se pri naslednjem FastLED.show().
void led_mgr_set_brightness(uint8_t brightness);

// Vrne trenutno nastavljeno svetlost matrike.
uint8_t led_mgr_get_brightness();

// Vrne true če fill/unfill/fade animacija trenutno teče.
bool led_mgr_is_animating();

// ============================================================
// JAVNI API — Signalna LED
// ============================================================

// Parking assist: prikaže barvno lestvico na signalni LED
// glede na razdaljo H senzorja (mm).
//   dist_mm > pa_thresh_green_mm  → zelena (prosto)
//   dist_mm > pa_thresh_orange_mm → rumena (bliže)
//   dist_mm <= pa_thresh_red_mm   → rdeča (stop)
// place = A ali B (za kasnejšo razširitev na dve mesti — za zdaj enako obnašanje)
void led_mgr_parking_assist(ParkPlace place, uint32_t dist_mm);

// Ugasni parking assist (vse signalne LED OFF).
void led_mgr_parking_assist_off();

// Prikaži uro na signalni LED (placeholder animacija — N barvnih LED = ura).
// duration_ms = čas prikaza (iz config.clock_duration_s * 1000).
void led_mgr_show_clock(uint32_t duration_ms = 0);

// Prikaži celica timer na signalni LED (utripajoča cona).
// active=true → utripajoča BOT cona (fotocelica prekinjena)
// active=false → ugasni
void led_mgr_celica_timer(bool active);

// Takojšen izklop vseh signalnih LED.
void led_mgr_signal_off();

// ============================================================
// FreeRTOS TASK
// ============================================================

// Implementirana v led_manager.cpp — registrira se v bsp.cpp.
// Stack: TASK_LED_STACK (4096), Core1, prioriteta TASK_LED_PRIO (3).
void ledTask(void* pvParams);

// ============================================================
// DIAGNOSTIKA
// ============================================================

struct LedMgrStats {
    bool     initialized;
    bool     party_mode;
    uint8_t  brightness_main;
    uint8_t  brightness_signal;
    bool     animating;
    uint32_t fill_count;        // koliko fill animacij je bilo
    uint32_t unfill_count;
    uint32_t fadeout_count;
    uint32_t parking_assist_updates;
};

LedMgrStats led_mgr_get_stats();
bool        led_mgr_ok();

// Startup ready flag — LED procesiranje blokirano dokler ni true.
// Privzeto false ob zagonu, postane true po 120s ali ob eksplicitnem klicu.
// Namenjeno: web UI, LCD, prihodnje izklapljanje LED taskov.
void led_mgr_set_ready(bool ready);
bool led_mgr_is_ready();
