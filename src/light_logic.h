// ============================================================
// light_logic.h — Osvetlitvena logika (Layer 4)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ODGOVORNOST:
//   Centralni modul ki sprejema EventBus evente in na osnovi
//   stanja (noč/dan, disable, timerji) krmili SSR releje ter
//   LED animacije. Je edini "odločevalec" za osvetlitev.
//
// ARHITEKTURA — LAYER 4:
//   EventBus (eventi) → light_logic → led_manager (animacije)
//                                   → hal_gpio_set_ssr() (releji)
//   light_logic NIKOLI ne kliče I2C direktno.
//   light_logic NIKOLI ne kliče FastLED direktno.
//
// SSR MAPIRANJE (potrjeno iz hardware_arhitektura_v2.1):
//   SSR1 = GPA5 = 12V trafo za LED matriko (WS2815)
//          → avtomatski (radar/rampa/raluc) + ročni toggle
//   SSR2 = GPA6 = 3× V-TAC 18W LED panel 4000K
//          → del avtomatike skupaj s SSR1 (vklop po waveFill+500ms)
//          → ročni toggle neodvisen od avtomatike
//   SSR3 = GPA7 = LED reflektor pred lopo
//          → SAMO ročni, anti-forget timer
//   SSR4 = GPB0 = LED reflektor pred garažo
//          → SAMO ročni, anti-forget timer
//
// ODLOČITEV — DISPLAY POSODOBITEV (Opcija B, dogovorjeno 2026-05):
//   light_logic NE kliče screen_main_set_ssr() direktno.
//   Namesto tega izpostavi light_logic_get_state() getter.
//   LVGL timer v hal_display.cpp periodično (500ms) prebere
//   stanje in posodobi zaslon. Razlog: LVGL ni thread-safe,
//   klici iz appTask konteksta bi povzročili crash. Enoten
//   polling sistem pokriva vse zaslone in vse vire podatkov.
//
// FILL HITROST — dogovorjeno 2026-05:
//   raluc/rampagor trigger → počasna (LIGHT_FADE_SLOW_MS ≈ 7s)
//   radar gibanje trigger  → hitra  (LIGHT_FADE_FAST_MS  ≈ 2.5s)
//   Razlog: raluc/rampagor = vozilo prihaja iz daleč, počasen
//   pristop. Radar = gibanje že v garaži, hiter odziv.
//
// FILL SMER — zaenkrat fiksen (0→89):
//   TODO: različna smer glede na trigger (levo-desno, desno-levo)
//   Implementirati ko bo fizična postavitev LED matrike potrjena.
//   Dokumentacija Glavna_LED_Razsvetljava.docx sekcija 5.
//
// DISABLE LOGIKA — dogovorjeno 2026-05:
//   Dolgi pritisk na SSR gumb (BUTTON_SSR_DISABLE) → toggle
//   disabled stanja. Disabled SSR ne more biti prižgan niti
//   avtomatsko niti ročno. Re-enable = ponovni dolgi pritisk.
//   Enaka funkcionalnost dostopna prek web API /api/ssr.
//   Opomba za web: /api/ssr endpoint (ne /api/config!) ker
//   to je operativno stanje, ne konfiguracija.
//
// ANTI-FORGET LOGIKA:
//   SSR2/3/4: če ni gibanja (noben od 4 radar kanalov) v zadnjih
//   antiforgot_*_min minutah → avtomatski izklop. Velja
//   ne glede na dan/noč. SSR1 nima anti-forget — ima timeout.
//
// PODNEVI:
//   BH1750 > lux_day → is_night = false.
//   Avtomatski triggerji (radar, rampa, raluc) NE prožijo SSR1.
//   Ročni gumbi VEDNO delujejo (dan in noč) za vse SSR.
//   Anti-forget za SSR2/3/4 teče ne glede na dan/noč.
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "alarm.h"

// ============================================================
// SSR STANJE — za prikaz na zaslonu in web API
// ============================================================

enum class SsrLogicState : uint8_t {
    OFF         = 0,   // izklopljeno, timer ne teče
    ON_AUTO     = 1,   // vklopljeno avtomatsko, countdown teče
    ON_MANUAL   = 2,   // vklopljeno ročno, countdown teče
    SSR_DISABLED= 3,   // onemogočeno (dolgi pritisk) — ne reagira na nič
                       // (ime SSR_DISABLED ker esp32-hal-gpio.h rezervira DISABLED kot makro)
};

struct SsrState {
    SsrLogicState state;
    uint32_t      countdown_s;    // preostali čas do ugasitve (0 = ni aktivno)
    bool          disabled;       // alias za state == DISABLED (za enostavno branje)
    bool          is_auto;        // true = avtomatika ga je prižgala
};

// ============================================================
// CELOTNO STANJE — za Opcija B polling
// ============================================================
// hal_display.cpp LVGL timer kliče light_logic_get_state()
// vsakih 500ms in posodobi zaslon. Brez direktnih klicev
// iz light_logic v screen_main (LVGL thread-safety!).

struct LightLogicState {
    SsrState ssr[5];        // indeksi 1–4 (0 neuporabljen)
    bool     is_night;      // trenutni noč/dan status
    float    lux;           // zadnja izmerjena vrednost lux
    bool     any_motion;    // OR vseh 4 radar kanalov
    uint32_t last_motion_ms;// millis() zadnjega gibanja
    // Alarm stanje (za LCD polling in web UI)
    bool           alarm_active;
    AlarmStateEnum alarm_state;
};

// ============================================================
// SSR COMMAND QUEUE — za non-blocking EventBus handler arhitekturo
// ============================================================
// ARHITEKTURNA ODLOČITEV (2026-05):
//   EventBus handlerji (on_button_ssr, on_radar_motion itd.) tečejo
//   v eventBusTask kontekstu. Ne smejo klicati vTaskDelay() ker bi
//   blokirali celoten event bus za čas pavze (500ms).
//   Rešitev: handler samo shrani SsrCmd v s_ssr_queue, appTask
//   ga prebere in izvede dejansko sekvenco z vsemi pavzami.
//   Prednosti:
//     - eventBusTask nikoli ne blokira
//     - Wire1 mutex contention odpravljen
//     - SSR sekvence so deterministične (appTask ima lasten Wire1 mutex kontekst)
//
// IMPLEMENTACIJA:
//   - FreeRTOS queue velikosti SSR_CMD_QUEUE_SIZE (8 ukazov)
//   - light_logic_tick() bere ukaze in jih izvede
//   - Handlerji kličejo ssr_cmd_enqueue() namesto direktnih funkcij

enum class SsrCmdType : uint8_t {
    TRIGGER_ON_AUTO,    // prižgi SSR1 avtomatsko (payload = fill_speed_ms)
    TRIGGER_OFF,        // ugasni SSR1 sekvenco (payload = 0)
    BUTTON_TOGGLE,      // ročni toggle SSR (payload = ssr_idx 1-4)
    BUTTON_DISABLE,     // disable/enable toggle (payload = ssr_idx 1-4)
};

struct SsrCmd {
    SsrCmdType type;
    uint32_t   payload;
};

#define SSR_CMD_QUEUE_SIZE  8

// ============================================================
// JAVNI API
// ============================================================

// Inicializacija — kliči iz appTask ob zagonu.
// Registrira EventBus subscriberje za vse relevantne evente.
// Vrne true ob uspehu.
bool light_logic_init();

// Tick — kliči iz appTask zanke vsakih ~100ms.
// Preverja SSR countdown timerje, anti-forget, in sproži
// ugasitve ko timerji potečejo.
void light_logic_tick();

// Vrne kopijo trenutnega stanja — thread-safe (atomski read).
// Kliči iz LVGL timer konteksta (hal_display.cpp) za posodobitev
// zaslona. Nikoli ne kliči screen_main_* direktno iz light_logic!
LightLogicState light_logic_get_state();

// Vrne stanje enega SSR (indeks 1–4).
SsrState light_logic_get_ssr(uint8_t idx);

// Vrne true če je light_logic_init() uspešno zaključil.
bool light_logic_ok();

// Vrne true če je party začasno prekinjen (gibanje/rampa) in čaka na nadaljevanje.
bool light_logic_is_party_suspended();

// Vrne true če sistem miruje (ni gibanja, ni rampe, noben SSR ni ON).
bool light_logic_is_system_idle();

// ============================================================
// FreeRTOS TASK
// ============================================================
// Implementirana v light_logic.cpp — registrira se kot appTask
// v bsp.cpp (zamenja stub).
// Stack: TASK_APP_STACK (6144), Core1, prio TASK_APP_PRIO (3).
void appTask(void* pvParams);
