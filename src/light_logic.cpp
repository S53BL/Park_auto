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
// [7] Podnevi = SSR1 avtomatika izklopljena, ročno vedno deluje:
//     is_night=false → RAMP_UP, RAMP_MOVING ne prožijo SSR1.
//     RADAR_MOTION: anti-forget timeri, parking assist in vehicle_recog
//     delujejo 24/7. Samo SSR1 prižig je filtriran z dan/noč.
//     Ročni gumbi (BUTTON_SSR) vedno delujejo za vse SSR.
//     Dan/noč filter = SAMO v on_radar_motion(), on_ramp_up(),
//     on_ramp_moving() za TRIGGER_ON_AUTO enqueue odločitev.
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
#include "signal_led.h"
#include "hal_light.h"
#include "event_bus.h"
#include "hal_gpio.h"
#include "led_manager.h"
#include "config_mgr.h"
#include "config.h"
#include "logger.h"
#include "vehicle_recog.h"
#include "parking_log.h"
#include <freertos/semphr.h>
#include <freertos/queue.h>
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

// Periodični log flush — prevzeto iz wifiTask (2026-05)
// Interval: 60s. Teče v appTask ki ima SRAM stack → SD operacije so varne.
static uint32_t s_last_log_flush_ms = 0;

// ============================================================
// SSR COMMAND QUEUE
// ============================================================
// Handlerji pišejo sem, light_logic_tick() bere in izvaja.
// Queue je non-blocking write (xQueueSend z timeout=0):
//   - če je polna (8 ukazov), nov ukaz se zavrže z WARN logom
//   - v praksi se to ne more zgoditi (radar eventi prihajajo vsakih
//     ~100ms, appTask tick je 100ms → 1:1 razmerje)
static QueueHandle_t s_ssr_queue = nullptr;

// Helper: pošlji ukaz v queue — thread-safe, non-blocking
static bool ssr_cmd_enqueue(SsrCmdType type, uint32_t payload = 0) {
    if (!s_ssr_queue) return false;
    SsrCmd cmd = { type, payload };
    if (xQueueSend(s_ssr_queue, &cmd, 0) != pdTRUE) {
        LLW("ssr_cmd_enqueue: queue polna — ukaz %d zavrnjen", (int)type);
        return false;
    }
    return true;
}

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
    // ============================================================
    // VKLOP SSR1 — avtomatski trigger (radar, rampa, raluc)
    // ============================================================
    // ARHITEKTURA (2026-05):
    //   Kliče se iz CMD executor v light_logic_tick() (appTask kontekst).
    //   Nikoli direktno iz EventBus handlerjev (eventBusTask kontekst).
    //   Razlog: vTaskDelay() in Wire1 operacije zahtevajo appTask kontekst.
    //
    // SEKVENCA ob prvem prižigu:
    //   1. Nastavi meta-podatke (is_auto, deadline) — PRED fizičnim vklopom
    //   2. ssr_physical_on() → hal_gpio_set_ssr(1, true)
    //   3. Če vklop uspe (s_ssr[1].on == true po klicu):
    //      a. SSR1_STABILIZE_MS pavza za 12V trafo
    //      b. led_mgr_fill() — animacija
    //      c. Armiraj SSR2 sekvencer
    //   4. Če vklop NE uspe: počisti meta-podatke (rollback!)
    //      → naslednji TRIGGER_ON_AUTO bo spet poskusil
    //
    // BUG FIX (2026-05): v prejšnji verziji se je s_ssr[1].on nastavil
    //   PRED ssr_physical_on() klicem. Če je Wire1 timeout →
    //   s_ssr[1].on = false ampak deadline_ms je nastavljen →
    //   naslednji trigger gre v "reset timer" vejo namesto "prižgi" →
    //   SSR1 nikoli ne gori. Popravek: nastavi deadline ŠELE po uspešnem
    //   fizičnem vklopu. Ob napaki: rollback is_auto in deadline.
    // ============================================================

    const Config cfg = config_get();
    uint32_t timeout_ms = (uint32_t)cfg.timeout_ssr1_s * 1000UL;
    uint32_t now = millis();

    if (s_ssr[1].on) {
        // SSR1 je že fizično ON — samo resetiraj timer.
        // Ne ponavljaj fill animacije pri vsakem radar pulzu!
        s_ssr[1].deadline_ms    = now + timeout_ms;
        s_ssr[2].last_motion_ms = now;
        // Timer reset log samo 1× per 30s — ne za vsak radar event.
        static uint32_t s_last_reset_log_ms = 0;
        if ((now - s_last_reset_log_ms) >= 30000) {
            s_last_reset_log_ms = now;
            LLD("SSR1: timer aktiven (zadnji reset pred <30s), ostalo=%lu s",
                (unsigned long)ssr_remaining_s(1));
        }
        return;
    }

    if (s_ssr[1].disabled) {
        LLD("SSR1: trigger ignoriran — disabled");
        return;
    }

    // SSR1 je bil OFF — poskusi prižgati.
    // Nastavi meta-podatke pred klicem (ssr_physical_on bere is_auto za log).
    s_ssr[1].is_auto     = true;
    s_ssr[1].deadline_ms = now + timeout_ms;

    ssr_physical_on(1);  // nastavi s_ssr[1].on = true SAMO ob uspehu

    if (!s_ssr[1].on) {
        // ssr_physical_on() ni uspel (Wire1 timeout ali I2C napaka).
        // ROLLBACK: počisti meta-podatke da naslednji trigger spet poskusi.
        // Brez rollbacka: deadline_ms teče, is_auto=true, SSR1 fizično OFF →
        //   naslednji trigger misli da je SSR1 ON in samo resetira timer.
        s_ssr[1].is_auto     = false;
        s_ssr[1].deadline_ms = 0;
        LLW("SSR1: vklop ni uspel (Wire1 napaka) — bo poskusil ob naslednjem triggerju");
        return;
    }

    // SSR1 je uspešno vklopljen — nadaljuj s sekvenco.
    LLI("SSR1: vklopljen, začenjam fill sekvenco (speed=%lu ms)",
        (unsigned long)fill_speed_ms);

    // Kratka pavza za stabilizacijo 12V trafota pred LED animacijo.
    // SSR1_STABILIZE_MS = 10ms. Brez tega: WS2815 dobi signal preden
    // je napajanje stabilno → možni glitchi pri prvi LED.
    vTaskDelay(pdMS_TO_TICKS(SSR1_STABILIZE_MS));

    // Začni fill animacijo — hitrost odvisna od triggerja.
    led_mgr_fill(fill_speed_ms);

    // Armiraj SSR2 auto sekvencer.
    // SSR2 (LED paneli) se vklopi POTEM ko fill konča + SSR2_DELAY_MS.
    if (!s_ssr[2].disabled && cfg.ssr2_auto_night) {
        s_ssr2_auto   = Ssr2AutoState::WAITING;
        s_ssr2_arm_ms = now;
        LLD("SSR2 auto: armirano (čakam fill %lums + delay %dms)",
            (unsigned long)fill_speed_ms, SSR2_DELAY_MS);
    }

    // Posodobi anti-forget referenčni čas za SSR2/3/4.
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

// RAMP_UP (rampagor) — payload=1: rampa dvignjena; payload=0: rampa spuščena
static void on_ramp_up(const Event& e) {
    if (e.payload) {
        signal_led_ramp_start(RampDir::UP);
        if (!s_is_night) { LLD("RAMP_UP: animacija (podnevi)"); return; }
        LLD("RAMP_UP → enqueue TRIGGER_ON_AUTO (počasen fill)");
        ssr_cmd_enqueue(SsrCmdType::TRIGGER_ON_AUTO, LIGHT_FADE_SLOW_MS);
    } else {
        signal_led_ramp_stop();
    }
}

// RAMP_MOVING (raluc) — payload=1: rampa se premika; payload=0: rampa se je ustavila
static void on_ramp_moving(const Event& e) {
    if (e.payload) {
        signal_led_ramp_start(RampDir::DOWN);
        if (!s_is_night) { LLD("RAMP_MOVING: animacija (podnevi)"); return; }
        LLD("RAMP_MOVING → enqueue TRIGGER_ON_AUTO (počasen fill)");
        ssr_cmd_enqueue(SsrCmdType::TRIGGER_ON_AUTO, LIGHT_FADE_SLOW_MS);
    } else {
        LLD("RAMP_MOVING(0) → rampa ustavljena, zaključujem animacijo");
        signal_led_ramp_stop();
    }
}

// RADAR_MOTION — gibanje zaznano na kateremkoli od 4 radar kanalov
// ============================================================
// ARHITEKTURNA ODLOČITEV (2026-05):
//
// SENZORJI DELUJEJO 24/7 — dan/noč filter velja SAMO za SSR1 prižig!
//   Napačna prejšnja implementacija: ob !is_night smo takoj vrnili
//   → anti-forget timeri niso bili posodobljeni podnevi.
//
//   Pravilna logika:
//     VEDNO (dan + noč): posodobi s_any_motion, anti-forget timerje
//     SAMO ponoči:       enqueue TRIGGER_ON_AUTO za SSR1 prižig
//
// THROTTLE — zakaj je potreben:
//   Radar pošilja ~10 frames/s × 4 kanali = 40 EventBus eventov/s.
//   Brez throttla: 40 TRIGGER_ON_AUTO ukazov/s → queue polna v 800ms.
//   Rešitev: enqueue samo 1× per RADAR_TRIGGER_THROTTLE_MS (500ms).
// ============================================================
static void on_radar_motion(const Event& e) {
    bool motion     = (e.payload >> 8) & 0x01;
    bool stationary = (e.payload >> 9) & 0x01;
    uint8_t channel = e.payload & 0xFF;

    if (!motion && !stationary) return;  // clear event — ni gibanja

    uint32_t now = millis();

    // -------------------------------------------------------
    // VEDNO — ne glede na dan/noč
    // -------------------------------------------------------
    s_any_motion     = true;
    s_last_motion_ms = now;

    // Anti-forget reset — vsako gibanje podaljša SSR2/3/4 timerje.
    // Deluje 24/7 ker so SSR2/3/4 neodvisni od noč/dan (dogovorjeno 2026-05).
    for (uint8_t i = 2; i <= 4; i++) {
        s_ssr[i].last_motion_ms = now;
    }

    // 2026-05-13: Ura na signalni LED verigi naj deluje 24/7 (tudi ponoči)
    // Odstranjena omejitev "samo podnevi" 
    // if (!s_is_night) {
        signal_led_clock_show();     // vedno pokličemo, ne glede na svetlobo / BH1750
    // }

    // TODO: parking_assist feed — deluje 24/7
    // TODO: vehicle_recog feed — deluje 24/7

    // -------------------------------------------------------
    // DAN/NOČ FILTER — samo za SSR1 prižig
    // -------------------------------------------------------
    if (!s_is_night) {
        // Podnevi: anti-forget, parking assist in vehicle_recog so posodobljeni
        // (koda zgoraj). SSR1 se ne proži — za debug zadostuje radar status log.
        return;
    }

    // -------------------------------------------------------
    // THROTTLE — enqueue TRIGGER_ON_AUTO max 1× per 500ms
    // -------------------------------------------------------
    // Namen: prepreči flooding SSR command queue pri intenzivnem
    // radarskem zaznavanju (40 eventov/s → max 2 ukaza/s).
    static uint32_t s_last_trigger_ms = 0;
    if ((now - s_last_trigger_ms) < RADAR_TRIGGER_THROTTLE_MS) {
        return;
    }
    s_last_trigger_ms = now;

    // Log samo ob prvem prižigu (SSR1 je OFF) — ne pri vsakem timer resetu.
    ssr_cmd_enqueue(SsrCmdType::TRIGGER_ON_AUTO, LIGHT_FADE_FAST_MS);
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
    uint8_t ssr_idx = (uint8_t)(e.payload + 1);
    if (ssr_idx < 1 || ssr_idx > 4) {
        LLW("BUTTON_SSR: neveljaven payload=%lu", (unsigned long)e.payload);
        return;
    }
    if (s_ssr[ssr_idx].disabled) {
        LLD("BUTTON_SSR%d ignoriran — disabled", ssr_idx);
        return;
    }
    // Debounce: touch panel pošlje dvojne evente pri hitrih dotiki.
    static uint32_t s_last_toggle_ms[5] = {0};
    uint32_t now_btn = millis();
    if ((now_btn - s_last_toggle_ms[ssr_idx]) < BUTTON_DEBOUNCE_MS) {
        LLD("BUTTON_SSR%d ignoriran — debounce (%lu ms)",
            ssr_idx, (unsigned long)(now_btn - s_last_toggle_ms[ssr_idx]));
        return;
    }
    s_last_toggle_ms[ssr_idx] = now_btn;
    LLI("BUTTON_SSR%d → enqueue BUTTON_TOGGLE", ssr_idx);
    ssr_cmd_enqueue(SsrCmdType::BUTTON_TOGGLE, ssr_idx);
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
    // Debounce za disable — daljši (1000ms) ker je hold event
    static uint32_t s_last_disable_ms[5] = {0};
    uint32_t now_dis = millis();
    if ((now_dis - s_last_disable_ms[ssr_idx]) < 1000) {
        LLD("BUTTON_SSR_DISABLE%d ignoriran — debounce", ssr_idx);
        return;
    }
    s_last_disable_ms[ssr_idx] = now_dis;
    LLI("BUTTON_SSR_DISABLE SSR%d → enqueue BUTTON_DISABLE", ssr_idx);
    ssr_cmd_enqueue(SsrCmdType::BUTTON_DISABLE, ssr_idx);
}

// CELL_BROKEN — fotocelica prekinjena ali obnovljena
// Payload: bit0=celica1, bit1=celica2, 0x00=obe OK
static void on_cell_broken(const Event& e) {
    bool celica1 = (e.payload & 0x01) != 0;
    bool celica2 = (e.payload & 0x02) != 0;
    if (!celica1 && !celica2) {
        signal_led_photocell_stop();
    } else {
        signal_led_photocell_update(celica1, celica2, s_is_night);
    }
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

    // Ustvari SSR command queue
    s_ssr_queue = xQueueCreate(SSR_CMD_QUEUE_SIZE, sizeof(SsrCmd));
    if (!s_ssr_queue) {
        LLE("xQueueCreate SSR queue napaka!");
        return false;
    }
    LLD("SSR command queue OK (size=%d)", SSR_CMD_QUEUE_SIZE);

    // Inicializiraj SSR runtime stanje — vsi OFF, ni disabled
    // last_motion_ms = init_ms (ne 0) da anti-forget ne sproži takoj ob
    // prvem ročnem vklopu brez predhodnih radar eventov.
    uint32_t init_ms = millis();
    for (uint8_t i = 0; i <= 4; i++) {
        s_ssr[i] = { false, false, false, 0, init_ms };
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
    EventBus::subscribe(EventType::CELL_BROKEN,              on_cell_broken);

    s_initialized = true;
    LLI("light_logic_init OK — 7 EventBus subscriberjev registriranih");
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

    // -------------------------------------------------------
    // Periodični log flush (prevzeto iz wifiTask — 2026-05)
    // -------------------------------------------------------
    // appTask ima SRAM stack → SD_MMC.open() v sd_mgr_log_flush() je varen.
    // wifiTask stack je bil premajhen za ta klic (268 B free ob flush-u).
    {
        uint32_t flush_now = millis();
        if ((flush_now - s_last_log_flush_ms) >= 60000UL) {
            s_last_log_flush_ms = flush_now;
            logger_flush();
        }
    }

    const Config cfg = config_get();

    // -------------------------------------------------------
    // 0. Procesiranje SSR command queue
    // -------------------------------------------------------
    // Ukazi so bili enqueued v EventBus handlerjih (ki tečejo v
    // eventBusTask kontekstu). Tukaj jih izvajamo v appTask kontekstu
    // kjer so vTaskDelay() in Wire1 operacije varni.
    // Procesiramo vse čakajoče ukaze pred ostalimi tick operacijami.
    {
        SsrCmd cmd;
        while (xQueueReceive(s_ssr_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {

                case SsrCmdType::TRIGGER_ON_AUTO:
                    // Log samo če SSR1 ni že ON (prižig) — timer reset je tih
                    if (!s_ssr[1].on) {
                        LLD("CMD: TRIGGER_ON_AUTO → prižig SSR1 (speed=%lu ms)",
                            (unsigned long)cmd.payload);
                    }
                    trigger_ssr1_auto(cmd.payload);
                    break;

                case SsrCmdType::TRIGGER_OFF:
                    LLD("CMD: TRIGGER_OFF");
                    trigger_ssr1_off();
                    break;

                case SsrCmdType::BUTTON_TOGGLE: {
                    uint8_t idx = (uint8_t)cmd.payload;
                    LLI("CMD: BUTTON_TOGGLE SSR%d", idx);
                    const Config ccfg = config_get();
                    uint32_t t_now = millis();
                    if (s_ssr[idx].on) {
                        LLI("SSR%d: ročni IZKLOP (toggle)", idx);
                        if (idx == 1) {
                            trigger_ssr1_off();
                        } else {
                            ssr_physical_off(idx);
                        }
                    } else {
                        LLI("SSR%d: ročni VKLOP", idx);
                        uint32_t timeout_ms = 0;
                        switch (idx) {
                            case 1:
                                timeout_ms = (uint32_t)ccfg.manual_extend_min * 60UL * 1000UL;
                                s_ssr[1].is_auto     = false;
                                s_ssr[1].deadline_ms = t_now + timeout_ms;
                                ssr_physical_on(1);
                                vTaskDelay(pdMS_TO_TICKS(SSR1_STABILIZE_MS));
                                led_mgr_fill(LIGHT_FADE_SLOW_MS);
                                if (!s_ssr[2].disabled && ccfg.ssr2_auto_night) {
                                    s_ssr2_auto   = Ssr2AutoState::WAITING;
                                    s_ssr2_arm_ms = t_now;
                                }
                                break;
                            case 2:
                                timeout_ms = (uint32_t)ccfg.antiforgot_ssr2_min * 60UL * 1000UL;
                                s_ssr[2].is_auto     = false;
                                s_ssr[2].deadline_ms = t_now + timeout_ms;
                                s_ssr[2].last_motion_ms = t_now;
                                ssr_physical_on(2);
                                break;
                            case 3:
                                timeout_ms = (uint32_t)ccfg.antiforgot_ssr3_min * 60UL * 1000UL;
                                s_ssr[3].is_auto     = false;
                                s_ssr[3].deadline_ms = t_now + timeout_ms;
                                s_ssr[3].last_motion_ms = t_now;
                                ssr_physical_on(3);
                                break;
                            case 4:
                                timeout_ms = (uint32_t)ccfg.antiforgot_ssr3_min * 60UL * 1000UL;
                                s_ssr[4].is_auto     = false;
                                s_ssr[4].deadline_ms = t_now + timeout_ms;
                                s_ssr[4].last_motion_ms = t_now;
                                ssr_physical_on(4);
                                break;
                        }
                        LLI("SSR%d: ročno ON, timer=%lu min",
                            idx, (unsigned long)(timeout_ms / 60000UL));
                    }
                    break;
                }

                case SsrCmdType::BUTTON_DISABLE: {
                    uint8_t idx = (uint8_t)cmd.payload;
                    if (s_ssr[idx].disabled) {
                        s_ssr[idx].disabled = false;
                        LLI("SSR%d: RE-ENABLED (CMD)", idx);
                    } else {
                        if (s_ssr[idx].on) {
                            if (idx == 1) trigger_ssr1_off();
                            else ssr_physical_off(idx);
                        }
                        s_ssr[idx].disabled = true;
                        LLI("SSR%d: DISABLED (CMD) — ne reagira na nič", idx);
                    }
                    EventBus::publish(EventType::SSR_STATE_CHANGED, idx);
                    break;
                }

                default:
                    LLW("CMD: neznan tip %d", (int)cmd.type);
                    break;
            }
        }
    }

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

    // -------------------------------------------------------
    // 8. Periodični SSR status log (vsake 2 minuti)
    // -------------------------------------------------------
    // Namen: diagnostika v produkcijskem okolju — takoj vidno
    // ali SSR deluje kot pričakovano brez fizičnega pregleda.
    {
        static uint32_t last_ssr_log_ms = 0;
        if ((now - last_ssr_log_ms) >= 120000UL) {
            last_ssr_log_ms = now;
            LLI("SSR status | 1:%s%s 2:%s%s 3:%s%s 4:%s%s | %s %.1f lux",
                s_ssr[1].on ? "ON"  : "off",
                s_ssr[1].disabled ? "(dis)" : "",
                s_ssr[2].on ? "ON"  : "off",
                s_ssr[2].disabled ? "(dis)" : "",
                s_ssr[3].on ? "ON"  : "off",
                s_ssr[3].disabled ? "(dis)" : "",
                s_ssr[4].on ? "ON"  : "off",
                s_ssr[4].disabled ? "(dis)" : "",
                s_is_night ? "NOČ" : "DAN",
                (double)s_lux);
            for (uint8_t i = 1; i <= 4; i++) {
                if (s_ssr[i].on) {
                    LLI("  SSR%d: %s timer=%lu s ostalo",
                        i,
                        s_ssr[i].is_auto ? "auto" : "ročno",
                        (unsigned long)ssr_remaining_s(i));
                }
            }
            // Stack usage diagnostika — kritično za dolgoročno stabilnost
            LLI("  appTask stack: %lu B free",
                (unsigned long)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));

            // Wire1 diagnostika — koliko SSR Wire1 napak je bilo skupno
            // Namen: zaznati sistemsko Wire1 contention brez pregledovanja
            //        celotnega loga. Če wire1_errors raste → preveriti mutex.
            static uint32_t s_last_wire1_errors = 0;
            uint32_t cur_errors = hal_gpio_get_wire1_errors();
            if (cur_errors != s_last_wire1_errors) {
                LLW("  Wire1 napake skupno: %lu (+%lu od zadnjega loga)",
                    (unsigned long)cur_errors,
                    (unsigned long)(cur_errors - s_last_wire1_errors));
                s_last_wire1_errors = cur_errors;
            }
        }
    }
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
    LLI("appTask stack ob zagonu: %lu B free / %d B total",
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t),
        TASK_APP_STACK);
    // Pričakovano po norm_profile→PSRAM optimizaciji: > 3000 B free
    // Če je < 1500 B → TASK_APP_STACK je premajhen, povečaj v config.h

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

    // Identifikacija vozil — zahteva LittleFS (inicializiran v bsp) in config_mgr
    if (!vehicle_recog_init()) {
        LLI("vehicle_recog_init NAPAKA — identifikacija vozil onemogočena");
    }
    parking_log_init();

    LLI("appTask v zanki (tick: 100ms)");
    while (true) {
        esp_task_wdt_reset();
        light_logic_tick();
        vehicle_recog_tick();

        // parking_log_tick vsakih ~10 s (perioda je interha v tick())
        parking_log_tick();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
