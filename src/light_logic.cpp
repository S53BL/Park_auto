// ============================================================
// light_logic.cpp — Osvetlitvena logika (Layer 4)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// IMPLEMENTACIJSKE ODLOČITVE — dokumentirane ker dokumentacija
// ni bila popolna in so bile sprejete med razvojem:
//
// [1] SSR2 je del avtomatike (ne samo ročni):
//     Dokumentacija Osnovna_Osvetlitev.md sekcija 5 pravi
//     "samo ročni" za SSR2, ampak Glavna_LED_Razsvetljava.docx
//     sekcija 6.2 jasno definira avtomatski vklop po waveFill.
//     Odločitev (2026-05): SSR2 je del SSR1 avtomatike.
//     SSR2 se vklopi po waveFill+SSR2_DELAY_MS, ugasne pred
//     waveUnfill. Ročni toggle deluje neodvisno.
//
// [2] SSR1 timer logika:
//     Vsak avtomatski trigger (radar, rampa, raluc) resetira
//     timer na polno vrednost (timeout_ssr1_s iz config).
//     Minimalni čas prižganosti = timeout_ssr1_s (ker timer
//     se ne more iztečh prej kot po timeout_ssr1_s od
//     zadnjega triggerja). Ročni toggle takoj ugasne (ne čaka).
//
// [3] Ročni gumb SSR1 = stikalo (toggle):
//     Kratki pritisk ko je OFF → ON + timer na manual_extend_min.
//     Kratki pritisk ko je ON  → takoj OFF (ne glede na countdown).
//     Dolgi pritisk → disable/re-enable toggle.
//
// [4] Radar trigger = OR vseh 4 kanalov:
//     Dokumentacija na nekaterih mestih omenja samo ch3 (Garaža).
//     Odločitev (2026-05): vsi 4 radar kanali so relevantni,
//     OR logika. Zanima nas samo sumarno: gibanje/ni gibanja.
//     Radarji pokrivajo celotno območje parkirišča.
//
// [5] RADAR_MOTION payload kodiranje:
//     bit 0-7:  channel index (0–3)
//     bit 8:    motion detected
//     bit 9:    stationary detected
//     Za light_logic: OR motion|stationary = nekdo je tam.
//
// [6] Anti-forget deluje ne glede na dan/noč:
//     SSR2/3/4 se ugasnejo po antiforgot timer izteku
//     ne glede na BH1750 stanje.
//
// [7] Podnevi = avtomatika izklopljena, ročno vedno deluje:
//     is_night=false → RAMP_UP, RAMP_MOVING, RADAR_MOTION
//     eventi so ignorirani za SSR1 prižig.
//     Ročni gumbi (BUTTON_SSR) vedno delujejo.
//
// [8] Fill smer — zaenkrat fiksen (0→89):
//     TODO: implementirati različne smeri glede na trigger vir.
//     (raluc→levo-desno, radar ch0→levo, radar ch3→desno, itd.)
//     Odloženo ker fizična postavitev LED matrike ni bila
//     potrjena med razvojem tega modula.
//
// [9] SSR2_auto_night parameter:
//     Config ima ssr2_auto_night bool. Ker je SSR2 del avtomatike
//     skupaj s SSR1, ta parameter določa ali SSR2 sledi SSR1
//     ob avtomatskem prižigu (true) ali samo ročno (false).
//     Privzeto: true (SSR2 vedno sledi SSR1 avtomatiki).
//
// [10] BUTTON_SSR payload indeksiranje:
//     screen_main.cpp kreira gumbe z idx 0–3 (row*2+col).
//     BUTTON_SSR payload = 0,1,2,3. light_logic doda +1
//     → SSR indeks 1–4. Dokumentirano tukaj ker screen_main
//     tega ne dokumentira eksplicitno.
//
// ============================================================

#include "light_logic.h"
#include "hal_light.h"
#include "event_bus.h"
#include "hal_gpio.h"
#include "led_manager.h"
#include "config_mgr.h"
#include "config.h"
#include "logger.h"
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

// ============================================================
// LOGGING
// ============================================================

#define LLI(fmt, ...) LOG_INFO ("LLOGIC", fmt, ##__VA_ARGS__)
#define LLW(fmt, ...) LOG_WARN ("LLOGIC", fmt, ##__VA_ARGS__)
#define LLE(fmt, ...) LOG_ERROR("LLOGIC", fmt, ##__VA_ARGS__)
#define LLD(fmt, ...) LOG_DEBUG("LLOGIC", fmt, ##__VA_ARGS__)

// ============================================================
// INTERNO STANJE
// ============================================================

// SSR runtime stanje — indeksi 1–4 (0 neuporabljen)
// Mutex varuje dostop iz appTask in LVGL timer (Opcija B polling).
static SemaphoreHandle_t s_mutex = nullptr;

// Posamezno SSR stanje
struct SsrRuntime {
    bool     on;                // fizično stanje SSR releja
    bool     disabled;          // onemogočeno (dolgi pritisk)
    bool     is_auto;           // prižgal avtomatika (ne ročno)
    uint32_t deadline_ms;       // millis() ko naj se ugasne (0=ni aktiven)
    uint32_t last_motion_ms;    // zadnje zaznano gibanje za anti-forget
};

static SsrRuntime s_ssr[5] = {};    // indeksi 1–4

// Noč/dan stanje — posodobljeno ob NIGHT_THRESHOLD_CHANGED eventu
static bool    s_is_night       = false;
static float   s_lux            = 0.0f;

// Radar sumarni status — OR vseh 4 kanalov
// Posodobljeno ob vsakem RADAR_MOTION eventu
static bool     s_any_motion        = false;
static uint32_t s_last_motion_ms    = 0;

// SSR2 state machine za avtomatski vklop po waveFill
// SSR2 se ne vklopi takoj ko SSR1 — čaka na konec waveFill + SSR2_DELAY_MS.
// Stanja:
//   IDLE    = SSR2 ne čaka na nič
//   WAITING = SSR1 vklopljen, čakamo na konec fill animacije + delay
//   READY   = delay potekel, SSR2 naj se vklopi
enum class Ssr2AutoState : uint8_t { IDLE, WAITING, READY };
static Ssr2AutoState s_ssr2_auto    = Ssr2AutoState::IDLE;
static uint32_t      s_ssr2_arm_ms  = 0;   // ko je bil SSR1 vklopljen

// Inicializacijski flag
static bool s_initialized = false;

// ============================================================
// POMOŽNE FUNKCIJE
// ============================================================

// Fizični vklop SSR releja — kliče hal_gpio_set_ssr() in logira.
// Ne menja s_ssr[].deadline — to naredi klicatelj.
static void ssr_physical_on(uint8_t idx) {
    if (!hal_gpio_set_ssr(idx, true)) {
        LLW("SSR%d: hal_gpio_set_ssr ON napaka", idx);
        return;
    }
    s_ssr[idx].on = true;
    LLI("SSR%d: VKLOPLJEN (%s)", idx, s_ssr[idx].is_auto ? "auto" : "ročno");
}

// Fizični izklop SSR releja.
static void ssr_physical_off(uint8_t idx) {
    if (!hal_gpio_set_ssr(idx, false)) {
        LLW("SSR%d: hal_gpio_set_ssr OFF napaka", idx);
        return;
    }
    s_ssr[idx].on          = false;
    s_ssr[idx].deadline_ms = 0;
    s_ssr[idx].is_auto     = false;
    LLI("SSR%d: izklopljen", idx);
}

// Vrne preostali čas v sekundah za SSR idx.
// Vrne 0 če timer ne teče.
static uint32_t ssr_remaining_s(uint8_t idx) {
    if (!s_ssr[idx].on || s_ssr[idx].deadline_ms == 0) return 0;
    uint32_t now = millis();
    if (now >= s_ssr[idx].deadline_ms) return 0;
    return (s_ssr[idx].deadline_ms - now) / 1000UL;
}

// ============================================================
// VKLOP SSR1 — centralna funkcija
// ============================================================
// Kliče se ob vsakem avtomatskem triggerju (radar, rampa, raluc).
// fill_speed_ms: hitrost fill animacije (počasna/hitra glede na trigger).
//
// Sekvenca (dokumentirano v Glavna_LED_Razsvetljava.docx sekcija 4.1):
//   1. SSR1 ON → 12V trafo pod napetostjo
//   2. Pavza SSR1_STABILIZE_MS (~10ms) za stabilizacijo trafo
//   3. waveFill animacija (led_mgr_fill)
//   4. Po koncu fill + SSR2_DELAY_MS → SSR2 ON (če ni disabled)
//   5. Timer na timeout_ssr1_s sekund od tega triggerja
//
// Če je SSR1 že ON: samo resetiraj timer na polno vrednost.
// Ne ponavljaj fill animacije pri vsakem radar pulzu!

static void trigger_ssr1_auto(uint32_t fill_speed_ms) {
    const Config cfg = config_get();
    uint32_t timeout_ms = (uint32_t)cfg.timeout_ssr1_s * 1000UL;
    uint32_t now = millis();

    if (s_ssr[1].on) {
        // SSR1 je že ON — samo resetiraj timer
        s_ssr[1].deadline_ms = now + timeout_ms;
        // Posodobi tudi anti-forget za SSR2
        s_ssr[2].last_motion_ms = now;
        LLD("SSR1: timer resetiran → %lu s", (unsigned long)cfg.timeout_ssr1_s);
        return;
    }

    if (s_ssr[1].disabled) {
        LLD("SSR1: trigger ignoriran — disabled");
        return;
    }

    // SSR1 je bil OFF — prižgi
    s_ssr[1].is_auto     = true;
    s_ssr[1].deadline_ms = now + timeout_ms;
    ssr_physical_on(1);

    // Kratka pavza za stabilizacijo 12V trafota pred LED animacijo.
    // SSR1_STABILIZE_MS = 10ms (config.h).
    // Brez tega: WS2815 dobi signal preden je napajanje stabilno
    // → možni glitchi pri prvi LED v verigi.
    vTaskDelay(pdMS_TO_TICKS(SSR1_STABILIZE_MS));

    // Začni fill animacijo — hitrost odvisna od triggerja
    led_mgr_fill(fill_speed_ms);
    LLI("SSR1: fill start (speed=%lu ms)", (unsigned long)fill_speed_ms);

    // Armiraj SSR2 auto sekvencer — vklopi se po fill + SSR2_DELAY_MS.
    // SSR2_DELAY_MS = 500ms (config.h). Sekvencer teče v light_logic_tick().
    // Razlog za zamik: SSR2 (LED paneli) se vklopi POTEM ko je animacija
    // zaključena — vizualni efekt "najprej animacija, nato polna svetloba".
    if (!s_ssr[2].disabled && cfg.ssr2_auto_night) {
        s_ssr2_auto   = Ssr2AutoState::WAITING;
        s_ssr2_arm_ms = now;
        LLD("SSR2 auto: armirano (čakam fill + %dms)", SSR2_DELAY_MS);
    }

    // Posodobi anti-forget referenčni čas za SSR2/3/4
    s_ssr[2].last_motion_ms = now;
    s_ssr[3].last_motion_ms = now;
    s_ssr[4].last_motion_ms = now;
}

// ============================================================
// IZKLOP SSR1 — centralna funkcija
// ============================================================
// Sekvenca izklopa (Glavna_LED_Razsvetljava.docx sekcija 4.2):
//   1. SSR2 OFF takoj (LED paneli ugasnejo)
//   2. Pavza 500ms
//   3. waveUnfill animacija
//   4. Po koncu unfill → SSR1 OFF
//
// Za korak 3+4: unfill animacija teče v ledTask.
// SSR1 fizično ugasnemo TAKOJ po unfill animaciji.
// Ker ne moremo čakati na konec animacije (appTask bi blokiral),
// uporabimo s_ssr1_unfill_pending flag — light_logic_tick()
// preveri kdaj animacija konča in potem ugasne SSR1.

static bool s_ssr1_unfill_pending = false;

static void trigger_ssr1_off() {
    if (!s_ssr[1].on) return;

    // Korak 1: SSR2 takoj OFF (pred unfill animacijo)
    // Vizualni efekt: najprej izgine funkcionalna bela svetloba,
    // nato se animirano umakne LED matrika.
    if (s_ssr[2].on && s_ssr[2].is_auto) {
        ssr_physical_off(2);
        s_ssr2_auto = Ssr2AutoState::IDLE;
        LLI("SSR2: izklopljen (pred unfill)");
    }

    // Korak 2: 500ms pavza med SSR2 off in unfill start
    vTaskDelay(pdMS_TO_TICKS(500));

    // Korak 3: začni unfill animacijo
    const Config cfg = config_get();
    led_mgr_unfill(cfg.unfill_speed_ms);
    LLI("SSR1: unfill start (speed=%lu ms)", (unsigned long)cfg.unfill_speed_ms);

    // Korak 4: SSR1 se fizično ugasne ko unfill konča.
    // light_logic_tick() preveri led_mgr_is_animating() in ugasne SSR1.
    s_ssr1_unfill_pending = true;
    s_ssr[1].deadline_ms  = 0;  // timer ni več aktiven
    LLD("SSR1: čakam konec unfill animacije");
}

// ============================================================
// EVENTBUS HANDLERJI
// ============================================================

// RAMP_UP (rampagor) — rampa je dvignjena = vozilo prihaja
// Trigger za SSR1 s počasnim fill (vozilo pride iz daleč).
static void on_ramp_up(const Event& e) {
    if (!e.payload) return;  // rampa spuščena = ni trigger
    if (!s_is_night) {
        LLD("RAMP_UP ignoriran — podnevi");
        return;
    }
    LLI("RAMP_UP → SSR1 trigger (počasen fill)");
    // Počasen fill: LIGHT_FADE_SLOW_MS ≈ 7000ms (config.h)
    // Razlog: raluc/rampagor = vozilo prihaja, animacija naj bo
    // vizualno prijetna — počasen naval svetlobe.
    trigger_ssr1_auto(LIGHT_FADE_SLOW_MS);
}

// RAMP_MOVING (raluc) — opozorilna luč utripa = rampa se premika
// Trigger za SSR1 s počasnim fill — identičen kot RAMP_UP.
static void on_ramp_moving(const Event& e) {
    if (!e.payload) return;  // rampa se je ustavila
    if (!s_is_night) {
        LLD("RAMP_MOVING ignoriran — podnevi");
        return;
    }
    LLI("RAMP_MOVING → SSR1 trigger (počasen fill)");
    trigger_ssr1_auto(LIGHT_FADE_SLOW_MS);
}

// RADAR_MOTION — gibanje zaznano na kateremkoli od 4 radar kanalov
// Payload kodiranje (iz sensor_mgr.cpp):
//   bit 0-7:  channel (0–3)
//   bit 8:    motion flag
//   bit 9:    stationary flag
//
// Trigger za SSR1 s hitrim fill. Anti-forget reset za SSR2/3/4.
static void on_radar_motion(const Event& e) {
    // Preveri ali je event relevanten (motion ali stationary)
    bool motion      = (e.payload >> 8) & 0x01;
    bool stationary  = (e.payload >> 9) & 0x01;
    uint8_t channel  = e.payload & 0xFF;

    if (!motion && !stationary) return;  // clear event — ni gibanja

    // Posodobi sumarni motion status
    s_any_motion     = true;
    s_last_motion_ms = millis();

    // Anti-forget reset za SSR2/3/4 — vsako gibanje podaljša njihov timer
    for (uint8_t i = 2; i <= 4; i++) {
        s_ssr[i].last_motion_ms = s_last_motion_ms;
    }

    if (!s_is_night) {
        LLD("RADAR ch%d ignoriran — podnevi", channel);
        return;
    }

    LLI("RADAR ch%d → SSR1 trigger (hiter fill)", channel);
    // Hiter fill: LIGHT_FADE_FAST_MS ≈ 2500ms (config.h)
    // Razlog: gibanje je že v garaži — hiter, energičen odziv.
    // TODO: implementirati različno smer fill glede na kanal:
    //   ch0 (Vhod)    → fill od LED 89 navznoter (desno-levo)
    //   ch1 (Cesta L) → fill od LED 0 navzven (levo-desno)
    //   ch2 (Cesta D) → fill od LED 89 navznoter (desno-levo)
    //   ch3 (Garaža)  → fill od LED 0 navzven (levo-desno)
    //   Zahteva razširitev led_manager API z direction parametrom.
    trigger_ssr1_auto(LIGHT_FADE_FAST_MS);
}

// NIGHT_THRESHOLD_CHANGED — BH1750 je zaznal prehod noč/dan
// Payload: 1 = prešlo v NOČ, 0 = prešlo v DAN
static void on_night_changed(const Event& e) {
    bool was_night = s_is_night;
    s_is_night = (e.payload == 1);

    if (s_is_night == was_night) return;  // ni spremembe

    LLI("NIGHT_THRESHOLD_CHANGED: %s → %s",
        was_night ? "NOČ" : "DAN",
        s_is_night ? "NOČ" : "DAN");

    // Prehod DAN→NOČ: ne prižgemo avtomatsko — čakamo na trigger.
    // Prehod NOČ→DAN: ugasnemo SSR1 če je bil prižgan AVTOMATSKO.
    // Ročno vklopljeni SSR ostanejo (uporabnik je bil ekspliciten).
    if (!s_is_night && s_ssr[1].on && s_ssr[1].is_auto) {
        LLI("NOČ→DAN: SSR1 (auto) ugašamo");
        trigger_ssr1_off();
    }
}

// BUTTON_SSR — kratki pritisk na SSR gumb (toggle)
// Payload: 0–3 (screen_main idx) → light_logic ga prevede v 1–4.
//
// Odločitev o indeksiranju (2026-05):
//   screen_main.cpp kreira gumbe z row*2+col → idx 0,1,2,3.
//   light_logic vedno dela z indeksi 1–4 → ssr_idx = payload + 1.
static void on_button_ssr(const Event& e) {
    // Pretvori 0-based indeks zaslona v 1-based SSR indeks
    uint8_t ssr_idx = (uint8_t)(e.payload + 1);
    if (ssr_idx < 1 || ssr_idx > 4) {
        LLW("BUTTON_SSR: neveljaven payload=%lu", (unsigned long)e.payload);
        return;
    }

    if (s_ssr[ssr_idx].disabled) {
        LLD("BUTTON_SSR%d ignoriran — disabled", ssr_idx);
        return;
    }

    const Config cfg = config_get();
    uint32_t now = millis();

    if (s_ssr[ssr_idx].on) {
        // SSR je ON → takoj ugasni (toggle)
        LLI("BUTTON_SSR%d: ročni IZKLOP (toggle)", ssr_idx);
        if (ssr_idx == 1) {
            trigger_ssr1_off();
        } else {
            ssr_physical_off(ssr_idx);
        }
    } else {
        // SSR je OFF → prižgi ročno
        LLI("BUTTON_SSR%d: ročni VKLOP", ssr_idx);

        uint32_t timeout_ms = 0;
        switch (ssr_idx) {
            case 1:
                // SSR1 ročni: timer na manual_extend_min (privzeto 30 min)
                timeout_ms = (uint32_t)cfg.manual_extend_min * 60UL * 1000UL;
                s_ssr[1].is_auto     = false;
                s_ssr[1].deadline_ms = now + timeout_ms;
                ssr_physical_on(1);
                // Ročni SSR1 vklop: počasen fill (estetska odločitev —
                // ročni vklop = ni nujnosti, počasna animacija je lepša)
                vTaskDelay(pdMS_TO_TICKS(SSR1_STABILIZE_MS));
                led_mgr_fill(LIGHT_FADE_SLOW_MS);
                // Armiraj SSR2 auto sekvencer enako kot pri auto triggeru
                if (!s_ssr[2].disabled && cfg.ssr2_auto_night) {
                    s_ssr2_auto   = Ssr2AutoState::WAITING;
                    s_ssr2_arm_ms = now;
                }
                break;

            case 2:
                // SSR2 ročni: timer = antiforgot_ssr2_min
                timeout_ms = (uint32_t)cfg.antiforgot_ssr2_min * 60UL * 1000UL;
                s_ssr[2].is_auto     = false;
                s_ssr[2].deadline_ms = now + timeout_ms;
                s_ssr[2].last_motion_ms = now;
                ssr_physical_on(2);
                break;

            case 3:
                // SSR3 ročni: timer = antiforgot_ssr3_min
                timeout_ms = (uint32_t)cfg.antiforgot_ssr3_min * 60UL * 1000UL;
                s_ssr[3].is_auto     = false;
                s_ssr[3].deadline_ms = now + timeout_ms;
                s_ssr[3].last_motion_ms = now;
                ssr_physical_on(3);
                break;

            case 4:
                // SSR4 ročni: timer = antiforgot_ssr3_min (enaka nastavitev)
                // TODO: dodati ločen antiforgot_ssr4_min v config če bo potrebno
                timeout_ms = (uint32_t)cfg.antiforgot_ssr3_min * 60UL * 1000UL;
                s_ssr[4].is_auto     = false;
                s_ssr[4].deadline_ms = now + timeout_ms;
                s_ssr[4].last_motion_ms = now;
                ssr_physical_on(4);
                break;
        }
        LLI("SSR%d: ročno ON, timer=%lu min",
            ssr_idx, (unsigned long)(timeout_ms / 60000UL));
    }
}

// BUTTON_SSR_DISABLE — dolgi pritisk na SSR gumb (disable/enable toggle)
// Payload: 0–3 (screen_main idx) → prevedi v 1–4.
//
// Disabled SSR:
//   - ne reagira na avtomatske triggerje
//   - ne reagira na ročne gumbe
//   - če je bil ON ko ga disablamo → takoj ugasnemo
//   - re-enable = ponovni dolgi pritisk
//   - enaka funkcionalnost dostopna prek /api/ssr (web UI)
static void on_button_ssr_disable(const Event& e) {
    uint8_t ssr_idx = (uint8_t)(e.payload + 1);
    if (ssr_idx < 1 || ssr_idx > 4) {
        LLW("BUTTON_SSR_DISABLE: neveljaven payload=%lu", (unsigned long)e.payload);
        return;
    }

    if (s_ssr[ssr_idx].disabled) {
        // RE-ENABLE
        s_ssr[ssr_idx].disabled = false;
        LLI("SSR%d: RE-ENABLED (dolgi pritisk)", ssr_idx);
    } else {
        // DISABLE — ugasni če je bil ON
        if (s_ssr[ssr_idx].on) {
            if (ssr_idx == 1) {
                trigger_ssr1_off();
            } else {
                ssr_physical_off(ssr_idx);
            }
        }
        s_ssr[ssr_idx].disabled = true;
        LLI("SSR%d: DISABLED (dolgi pritisk) — ne reagira na nič", ssr_idx);
    }

    // Objavi SSR_STATE_CHANGED da zaslon takoj posodobi stanje
    // (ne čakamo na 500ms polling timer)
    EventBus::publish(EventType::SSR_STATE_CHANGED, ssr_idx);
}

// ============================================================
// light_logic_init
// ============================================================

bool light_logic_init() {
    LLI("=== light_logic_init ===");

    // Ustvari mutex za LightLogicState branje (Opcija B polling)
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        LLE("xSemaphoreCreateMutex napaka!");
        return false;
    }

    // Inicializiraj SSR runtime stanje — vsi OFF, ni disabled
    for (uint8_t i = 0; i <= 4; i++) {
        s_ssr[i] = { false, false, false, 0, 0 };
    }

    // Preberi začetno noč/dan stanje iz hal_light
    // (BH1750 je že inicializiran v sensor_mgr)
    s_is_night = hal_light_get_is_night();
    s_lux      = hal_light_get_lux();
    LLI("Začetno stanje: %s (%.1f lux)", s_is_night ? "NOČ" : "DAN", (double)s_lux);

    // Registriraj EventBus subscriberje
    // MAX_HANDLERS_PER_TYPE = 8 — ne bo težav
    EventBus::subscribe(EventType::RAMP_UP,                  on_ramp_up);
    EventBus::subscribe(EventType::RAMP_MOVING,              on_ramp_moving);
    EventBus::subscribe(EventType::RADAR_MOTION,             on_radar_motion);
    EventBus::subscribe(EventType::NIGHT_THRESHOLD_CHANGED,  on_night_changed);
    EventBus::subscribe(EventType::BUTTON_SSR,               on_button_ssr);
    EventBus::subscribe(EventType::BUTTON_SSR_DISABLE,       on_button_ssr_disable);

    s_initialized = true;
    LLI("light_logic_init OK — 6 EventBus subscriberjev registriranih");
    return true;
}

// ============================================================
// light_logic_tick — kliči iz appTask vsakih ~100ms
// ============================================================
// Preverja:
//   1. SSR1 countdown → timeout → sproži izklop
//   2. SSR2 auto sekvencer (fill konec + delay)
//   3. SSR2 countdown (avtomatski)
//   4. SSR3/4 anti-forget timer
//   5. Sumarni radar motion timeout (za s_any_motion flag)

void light_logic_tick() {
    if (!s_initialized) return;
    uint32_t now = millis();
    const Config cfg = config_get();

    // ----------------------------------------------------------
    // 1. SSR1 countdown
    // ----------------------------------------------------------
    if (s_ssr[1].on && !s_ssr1_unfill_pending &&
        s_ssr[1].deadline_ms > 0 && now >= s_ssr[1].deadline_ms) {
        LLI("SSR1: timeout — začenjam izklop sekvencer");
        trigger_ssr1_off();
    }

    // ----------------------------------------------------------
    // 2. SSR1 unfill pending — čakaj na konec animacije
    // ----------------------------------------------------------
    if (s_ssr1_unfill_pending && !led_mgr_is_animating()) {
        // Unfill je končan — fizično ugasni SSR1
        s_ssr1_unfill_pending = false;
        ssr_physical_off(1);
        s_ssr2_auto = Ssr2AutoState::IDLE;
        LLI("SSR1: unfill končan → fizično OFF");
    }

    // ----------------------------------------------------------
    // 3. SSR2 auto sekvencer
    // ----------------------------------------------------------
    // WAITING → čakamo da fill animacija konča + SSR2_DELAY_MS
    // READY   → vklopi SSR2
    if (s_ssr2_auto == Ssr2AutoState::WAITING) {
        // Preveri ali je fill animacija končana in je preteklo dovolj časa
        bool fill_done = !led_mgr_is_animating();
        bool delay_ok  = (now - s_ssr2_arm_ms) >= 
                         (cfg.fill_speed_ms + (uint32_t)SSR2_DELAY_MS);
        if (fill_done && delay_ok) {
            s_ssr2_auto = Ssr2AutoState::READY;
        }
    }

    if (s_ssr2_auto == Ssr2AutoState::READY) {
        s_ssr2_auto = Ssr2AutoState::IDLE;
        if (!s_ssr[2].disabled && !s_ssr[2].on) {
            // SSR2 ON — timer = enako kot SSR1 (tečeta skupaj)
            uint32_t timeout_ms = (uint32_t)cfg.timeout_ssr1_s * 1000UL;
            s_ssr[2].is_auto     = true;
            s_ssr[2].deadline_ms = now + timeout_ms;
            ssr_physical_on(2);
            LLI("SSR2: auto vklopljen (po fill+delay), timer=%lu s",
                (unsigned long)cfg.timeout_ssr1_s);
        }
    }

    // ----------------------------------------------------------
    // 4. SSR2 countdown (avtomatski način)
    // ----------------------------------------------------------
    // SSR2 se v auto načinu ugasne pred waveUnfill (v trigger_ssr1_off).
    // Tukaj preverimo samo če je SSR2 ON dlje kot SSR1 deadline
    // (robni primer: SSR1 je bil ročno ugasnjen pred SSR2 avto).
    if (s_ssr[2].on && s_ssr[2].is_auto &&
        s_ssr[2].deadline_ms > 0 && now >= s_ssr[2].deadline_ms) {
        LLI("SSR2: auto timeout — izklop");
        ssr_physical_off(2);
    }

    // ----------------------------------------------------------
    // 5. SSR3/4 anti-forget timer
    // ----------------------------------------------------------
    // Deluje ne glede na dan/noč (dogovorjeno 2026-05).
    // Referenčni čas: zadnje zaznano gibanje NA KATEREMKOLI radarskem kanalu.
    // Če ni gibanja v antiforgot_ssr3_min minutah → izklop.

    uint32_t af3_ms = (uint32_t)cfg.antiforgot_ssr3_min * 60UL * 1000UL;
    uint32_t af2_ms = (uint32_t)cfg.antiforgot_ssr2_min * 60UL * 1000UL;

    // SSR2 anti-forget (ročno vklopljeni SSR2)
    if (s_ssr[2].on && !s_ssr[2].is_auto) {
        if (s_ssr[2].last_motion_ms > 0 &&
            (now - s_ssr[2].last_motion_ms) >= af2_ms) {
            LLI("SSR2: anti-forget timeout (ni gibanja %lu min) — izklop",
                (unsigned long)cfg.antiforgot_ssr2_min);
            ssr_physical_off(2);
        }
    }

    // SSR3 anti-forget
    if (s_ssr[3].on) {
        if (s_ssr[3].last_motion_ms > 0 &&
            (now - s_ssr[3].last_motion_ms) >= af3_ms) {
            LLI("SSR3: anti-forget timeout (ni gibanja %lu min) — izklop",
                (unsigned long)cfg.antiforgot_ssr3_min);
            ssr_physical_off(3);
        }
    }

    // SSR4 anti-forget (enaka vrednost kot SSR3)
    if (s_ssr[4].on) {
        if (s_ssr[4].last_motion_ms > 0 &&
            (now - s_ssr[4].last_motion_ms) >= af3_ms) {
            LLI("SSR4: anti-forget timeout — izklop");
            ssr_physical_off(4);
        }
    }

    // ----------------------------------------------------------
    // 6. Sumarni motion flag timeout
    // ----------------------------------------------------------
    // s_any_motion se postavi v on_radar_motion().
    // Ponastavimo ga po 5s brez novega radar eventa.
    // Namen: web UI /api/status in servisni zaslon.
    if (s_any_motion && (now - s_last_motion_ms) >= 5000) {
        s_any_motion = false;
    }

    // ----------------------------------------------------------
    // 7. Posodobi lux iz hal_light
    // ----------------------------------------------------------
    // Ne kličemo direktno hal_light (mutex contention z Wire1).
    // hal_light_get_lux() vrne zadnjo vrednost brez Wire1 klica —
    // to je varno (samo branje volatile float).
    s_lux = hal_light_get_lux();
}

// ============================================================
// GETTER FUNKCIJE — za Opcija B polling (LVGL timer)
// ============================================================

LightLogicState light_logic_get_state() {
    LightLogicState st;

    // Kratka kritična sekcija — samo kopiramo vrednosti
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (uint8_t i = 1; i <= 4; i++) {
            st.ssr[i].state       = s_ssr[i].disabled  ? SsrLogicState::SSR_DISABLED  :
                                    !s_ssr[i].on        ? SsrLogicState::OFF        :
                                    s_ssr[i].is_auto    ? SsrLogicState::ON_AUTO    :
                                                          SsrLogicState::ON_MANUAL;
            st.ssr[i].countdown_s = ssr_remaining_s(i);
            st.ssr[i].disabled    = s_ssr[i].disabled;
            st.ssr[i].is_auto     = s_ssr[i].is_auto;
        }
        st.is_night       = s_is_night;
        st.lux            = s_lux;
        st.any_motion     = s_any_motion;
        st.last_motion_ms = s_last_motion_ms;
        xSemaphoreGive(s_mutex);
    } else {
        // Fallback: brez mutexa (morda init ni bil klican)
        st.is_night = s_is_night;
        st.lux      = s_lux;
    }
    return st;
}

SsrState light_logic_get_ssr(uint8_t idx) {
    if (idx < 1 || idx > 4) return {};
    return light_logic_get_state().ssr[idx];
}

bool light_logic_ok() {
    return s_initialized;
}

// ============================================================
// appTask — zamenja stub v bsp.cpp
// ============================================================
// Stack: TASK_APP_STACK = 6144 (config.h)
// Worst-case stack analiza:
//   appTask frame          ~200 B
//   light_logic_tick()     ~200 B
//   config_get() kopija     ~80 B  (Config struct)
//   hal_gpio_set_ssr()     ~100 B  + Wire1 mutex
//   mcp_write_reg()         ~80 B
//   LOG_INFO buffer        ~512 B  ← dominantno
//   FreeRTOS overhead      ~500 B
//   Skupaj worst-case:    ~1670 B → faktor 3.7× pod 6144 ✓

void appTask(void* pvParams) {
    LLI("appTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    // Kratka pavza da se vsi taski inicializirajo
    // (EventBus, sensor_mgr, hal_gpio morajo biti ready)
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (!light_logic_init()) {
        LLE("light_logic_init NAPAKA — appTask degraded mode (samo WDT)");
        while (true) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    LLI("appTask v zanki (tick: 100ms)");
    while (true) {
        esp_task_wdt_reset();
        light_logic_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
