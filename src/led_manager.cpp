// ============================================================
// led_manager.cpp — LED Manager: FastLED animacije + MUX preklop
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// IMPLEMENTACIJSKE ODLOČITVE:
//
//   FastLED bufferji v PSRAM:
//     s_leds_main[90] in s_leds_signal[144] sta statični arrayi.
//     FastLED na ESP32-S3 z Arduino frameworkom podpira CRGB arrayje
//     v PSRAM — alociramo z ps_malloc() in registriramo prek
//     FastLED.addLeds<>().setMaxRefreshRate(). Če PSRAM ni dostopen,
//     fallback na navadni malloc (bo v SRAM — opozorilo v logu).
//
//   Animacijska state machine:
//     ledTask() teče v ~20ms zanki (50 Hz). V vsaki iteraciji:
//     1. Preberi ukaz iz s_cmd_queue (non-blocking, timeout=0)
//     2. Posodobi animacijsko stanje (s_anim_state)
//     3. Izračunaj novo vrednost za ta frame
//     4. Pokliči FastLED.show() — samo če ni party mode
//     Animacije niso timer-based threads — so iterativne state machine-e.
//
//   Fill/Unfill implementacija:
//     Fill: vsak frame prižge naslednji blok LED.
//     Blok = ceil(90 / (speed_ms / LEDTASK_TICK_MS)) LED na frame.
//     Primer: speed_ms=6000, tick=20ms → 300 frameov → 90/300 = 0.3 LED/frame.
//     Ker je 0.3 < 1, gremo na "LED na N frameov": 1 LED vsakih 3.3 frameov
//     → implementirano z akumulatorjem (floating point progress tracker).
//
//   FastLED.show() timing:
//     WS2815 zahteva min 280µs reset pulse. FastLED.show() blokira ~2.8ms
//     za 90 LED (90 × 24 bitu × 1.2µs/bit + reset). To je sprejemljivo
//     v 20ms zanki (14% obremenitev).
//     NIKOLI ne kliči FastLED.show() ko je party_mode=true (IO45 HIGH)!
//
//   Topla bela barva za WS2815:
//     WS2815 ima drugačne fosfore kot WS2812B — "čista bela" (255,255,255)
//     izgleda modrikasto. Topla bela: CRGB(255, 200, 80) pri polni svetlosti,
//     kar pri brightness=200 da realno toplo belo (≈2700K CCT simulacija).
//
//   Parking assist barvna lestvica:
//     3 cone glede na config prage:
//       dist > GREEN threshold  → zelena  CRGB(0, 200, 0)
//       dist > ORANGE threshold → rumena  CRGB(255, 120, 0)
//       dist <= RED threshold   → rdeča   CRGB(200, 0, 0)  + utripanje
//     Vse 3 cone signalne LED se obarvajo enako (ni gradacija po conah).
//     Pri dist > GREEN se signalna LED UGASNE (samo zelena indikacija je OFF).
//
//   MUX preklop varnost:
//     led_mgr_set_party_mode(true):
//       1. Ugasni vse LED v FastLED bufferjih (fill z CRGB::Black)
//       2. FastLED.show() — buffer spraznjen
//       3. Pavza MUX_SWITCH_DELAY_MS (200ms)
//       4. IO45 HIGH — WLED prevzame signal
//     led_mgr_set_party_mode(false):
//       1. IO45 LOW — FastLED prevzame signal
//       2. Pavza MUX_SWITCH_DELAY_MS
//       3. Naprej normalna animacija
//     S tem preprečimo signal overlap ki bi povzročil vizualni artefakt.
//
// ============================================================

#include "led_manager.h"
#include "signal_led.h"
#include "config_mgr.h"
#include "config.h"
#include "logger.h"
#include <FastLED.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ============================================================
// LOGGING MAKROJI
// ============================================================

#define LEDI(fmt, ...) LOG_INFO ("LED", fmt, ##__VA_ARGS__)
#define LEDW(fmt, ...) LOG_WARN ("LED", fmt, ##__VA_ARGS__)
#define LEDE(fmt, ...) LOG_ERROR("LED", fmt, ##__VA_ARGS__)
#define LEDD(fmt, ...) LOG_DEBUG("LED", fmt, ##__VA_ARGS__)

// ============================================================
// KONSTANTE
// ============================================================

// Perioda ledTask zanke (ms) — 50 Hz za gladke animacije
#define LEDTASK_TICK_MS         20

// Topla bela barva za glavno matriko (WS2815 kalibrirano)
// Pri brightness=200 daje ≈2700K videz
static const CRGB WARM_WHITE = CRGB(255, 200, 80);

// Barve parking assist
static const CRGB PA_GREEN  = CRGB(0,   200, 0);
static const CRGB PA_ORANGE = CRGB(255, 120, 0);
static const CRGB PA_RED    = CRGB(200, 0,   0);

// Cona signalne LED za clock prikaz (srednja cona = MID)
#define CLOCK_ZONE_START    LED_SIG_ZONE_MID_START
#define CLOCK_ZONE_END      LED_SIG_ZONE_MID_END

// Utripanje rdeče zone parking assist (ms)
#define PA_RED_BLINK_MS     300

// Celica timer utripanje (ms)
#define CELICA_BLINK_MS     500

// ============================================================
// UKAZI ZA QUEUE
// ============================================================

enum class LedCmd : uint8_t {
    FILL,               // fill animacija glavne matrike
    UNFILL,             // unfill animacija glavne matrike
    FADE_OUT,           // fade-out animacija
    ON_IMMEDIATE,       // takojšen vklop
    OFF_IMMEDIATE,      // takojšen izklop
    SET_BRIGHTNESS,     // nastavi svetlost (payload = brightness)
    PARTY_ON,           // preklop v party mode
    PARTY_OFF,          // izhod iz party mode
    PARKING_ASSIST,     // OPUŠČENO — signal_led_parking_update() direktno
    PARKING_ASSIST_OFF, // OPUŠČENO — signal_led_parking_stop() direktno
    SHOW_CLOCK,         // OPUŠČENO — signal_led_clock_show() direktno
    CELICA_TIMER_ON,    // OPUŠČENO — signal_led_photocell_update() direktno
    CELICA_TIMER_OFF,   // OPUŠČENO — signal_led_photocell_stop() direktno
    SIGNAL_OFF,         // OPUŠČENO — signal_led_off() direktno
};

struct LedCmdMsg {
    LedCmd   cmd;
    uint32_t payload;   // pomen odvisno od cmd
};

// ============================================================
// ANIMACIJSKO STANJE
// ============================================================

enum class AnimState : uint8_t {
    IDLE,
    FILLING,
    UNFILLING,
    FADING,
    PARKING_ASSIST,  // OPUŠČENO (2026-05) — signal_led_parking_update()
    CLOCK_SHOW,      // OPUŠČENO (2026-05) — signal_led_clock_show()
    CELICA_BLINK,    // OPUŠČENO (2026-05) — signal_led_photocell_update()
};

// ============================================================
// INTERNO STANJE
// ============================================================

// FastLED bufferji — alocirani v PSRAM (ps_malloc)
static CRGB* s_leds_main   = nullptr;   // [LED_MAIN_LOGICAL]  = 90 LED
static CRGB* s_leds_signal = nullptr;   // [LED_SIGNAL_COUNT]  = 144 LED
static CLEDController* s_ctrl_signal = nullptr;   // za ločen show med party mode

// FreeRTOS queue za ukaze (caller → ledTask)
static QueueHandle_t s_cmd_queue = nullptr;
#define CMD_QUEUE_SIZE  8

// Stanje
static bool       s_initialized    = false;
static bool       s_party_mode     = false;
static uint8_t    s_brightness_main    = LED_TARGET_BRIGHTNESS;
static uint8_t    s_brightness_signal  = LED_TARGET_BRIGHTNESS;

// Animacijska state machine — glavna matrika
static AnimState  s_anim_state     = AnimState::IDLE;
static float      s_fill_progress  = 0.0f;     // 0.0 = nič, 90.0 = polno
static float      s_fill_delta     = 0.0f;     // LED na frame
static uint32_t   s_fade_start_ms  = 0;
static uint32_t   s_fade_duration  = 0;
static uint8_t    s_fade_start_br  = 0;

// Animacijsko stanje — signalna LED
// OPUŠČENO (2026-05): signal_led.cpp ima lasten state machine.
// Spremenljivka ostaja ker jo referencirajo opuščeni case-i v LedCmd switch.
// Ob naslednjem refactoringu: odstrani skupaj z AnimState vrednostmi spodaj.
static AnimState  s_sig_state      = AnimState::IDLE;  // unused
static uint32_t   s_pa_dist_mm     = 9999;
static uint32_t   s_pa_blink_ms    = 0;
static bool       s_pa_blink_on    = false;
static uint32_t   s_clock_end_ms   = 0;
static uint32_t   s_celica_blink_ms = 0;
static bool       s_celica_blink_on = false;

// Diagnostični števci
static uint32_t   s_fill_count     = 0;
static uint32_t   s_unfill_count   = 0;
static uint32_t   s_fadeout_count  = 0;
static uint32_t   s_pa_updates     = 0;

// Dirty flag — safe_show() samo ob dejanski spremembi bufferja.
// Eliminira RMT DMA alloc/free ko LED miruje (SRAM fragmentacija).
static bool     s_led_dirty = false;
// Adaptive tick — 20ms med animacijo, 100ms v IDLE.
static uint32_t s_tick_ms   = 20;

// Startup ready flag — blokira LED procesiranje med stabilizacijo sistema.
// false = LED task spi in ne procesira ukazov (2 minuti po zagonu).
// true  = normalno delovanje.
// Eksplicitna kontrola prek led_mgr_set_ready() za web/LCD uporabo.
static volatile bool s_startup_ready    = false;
static uint32_t      s_startup_time_ms  = 0;  // čas ko je ledTask začel
#define LED_STARTUP_DELAY_MS  120000           // 2 minuti

// ============================================================
// POMOŽNE FUNKCIJE
// ============================================================

// Pošlji ukaz v queue — non-blocking (timeout=0).
// Če je queue polna, ukaz se zavrže z WARN logom.
static bool send_cmd(LedCmd cmd, uint32_t payload = 0) {
    if (!s_cmd_queue) return false;
    LedCmdMsg msg = { cmd, payload };
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        LEDW("send_cmd(%d): queue polna — ukaz zavrnjen", (int)cmd);
        return false;
    }
    return true;
}

// Nastavi vso glavno matriko na barvo (brez show)
static void fill_main_solid(CRGB color, int count = LED_MAIN_LOGICAL) {
    if (!s_leds_main) return;
    int n = (count < 0 || count > LED_MAIN_LOGICAL) ? LED_MAIN_LOGICAL : count;
    for (int i = 0; i < n; i++) s_leds_main[i] = color;
    if (n < LED_MAIN_LOGICAL) {
        for (int i = n; i < LED_MAIN_LOGICAL; i++) s_leds_main[i] = CRGB::Black;
    }
}

// Nastavi vso signalno LED na barvo (brez show)
static void fill_signal_solid(CRGB color) {
    if (!s_leds_signal) return;
    for (int i = 0; i < LED_SIGNAL_COUNT; i++) s_leds_signal[i] = color;
}

// Nastavi cono signalne LED na barvo (brez show)
static void fill_signal_zone(int start, int end, CRGB color) {
    if (!s_leds_signal) return;
    for (int i = start; i <= end && i < LED_SIGNAL_COUNT; i++) {
        s_leds_signal[i] = color;
    }
}

// Pokliči FastLED.show() — med party mode samo IO40 (signal), ne IO39 (main).
static void safe_show() {
    if (s_party_mode) {
        // IO39 (glavna matrika) je pod WLED med party mode — NE piši!
        // FastLED.show() bi pisal na oba registrirana pina (IO39 + IO40)
        // kar bi povzročilo signal conflict na IO39.
        //
        // IO40 (signalna LED) ni pod WLED — je varna in mora delovati
        // tudi med party mode (rampa animacija ima P1 prioriteto,
        // fotocelice in parking assist morajo biti vidni ne glede na modo).
        //
        // Rešitev: kliči show() samo na signal controllerju (IO40).
        if (s_ctrl_signal) {
            s_ctrl_signal->showLeds(s_brightness_signal);
        }
        return;
    }
    FastLED.show();  // prikaže oba pina skupaj (IO39 + IO40)
}

// ============================================================
// led_mgr_init
// ============================================================

bool led_mgr_init() {
    LEDI("=== led_mgr_init ===");

    // Alokacija LED bufferjev v PSRAM
    s_leds_main = (CRGB*)ps_malloc(sizeof(CRGB) * LED_MAIN_LOGICAL);
    if (!s_leds_main) {
        LEDW("PSRAM za main LED ni dostopen — fallback SRAM");
        s_leds_main = (CRGB*)malloc(sizeof(CRGB) * LED_MAIN_LOGICAL);
    }
    if (!s_leds_main) {
        LEDE("malloc za main LED (%d B) napaka!", (int)(sizeof(CRGB) * LED_MAIN_LOGICAL));
        return false;
    }

    s_leds_signal = (CRGB*)ps_malloc(sizeof(CRGB) * LED_SIGNAL_COUNT);
    if (!s_leds_signal) {
        LEDW("PSRAM za signal LED ni dostopen — fallback SRAM");
        s_leds_signal = (CRGB*)malloc(sizeof(CRGB) * LED_SIGNAL_COUNT);
    }
    if (!s_leds_signal) {
        LEDE("malloc za signal LED (%d B) napaka!", (int)(sizeof(CRGB) * LED_SIGNAL_COUNT));
        free(s_leds_main);
        s_leds_main = nullptr;
        return false;
    }

    // Počisti bufferje
    memset(s_leds_main,   0, sizeof(CRGB) * LED_MAIN_LOGICAL);
    memset(s_leds_signal, 0, sizeof(CRGB) * LED_SIGNAL_COUNT);

    // Registriraj FastLED controllere
    // ⚠ addLeds<> zahteva statični array — zato posredujemo pointer direktno.
    // FastLED.addLeds ne kopira podatkov — shranjuje pointer na s_leds_main.
    // Zato mora s_leds_main živeti ves čas programa (heap, ne stack).
    FastLED.addLeds<WS2815, LED_MAIN_PIN,   GRB>(s_leds_main,   LED_MAIN_LOGICAL);
    s_ctrl_signal = &FastLED.addLeds<WS2815, LED_SIGNAL_PIN, GRB>(
                        s_leds_signal, LED_SIGNAL_COUNT);

    // Nastavi začetno svetlost iz config
    const Config cfg = config_get();
    s_brightness_main   = cfg.target_brightness;
    s_brightness_signal = cfg.target_brightness;
    FastLED.setBrightness(s_brightness_main);

    // MUX pin — začnemo v FastLED načinu (IO45 LOW)
    pinMode(PIN_MUX_SELECT, OUTPUT);
    digitalWrite(PIN_MUX_SELECT, LOW);
    s_party_mode = false;
    LEDD("MUX pin IO%d = LOW (FastLED aktiven)", PIN_MUX_SELECT);

    // Vsi LED OFF ob zagonu
    fill_main_solid(CRGB::Black);
    fill_signal_solid(CRGB::Black);
    FastLED.show();

    if (!signal_led_init(s_leds_signal, LED_SIGNAL_COUNT)) {
        LEDE("signal_led_init napaka — signalne animacije ne bodo delovale!");
    }

    // Ustvari ukaz queue
    s_cmd_queue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(LedCmdMsg));
    if (!s_cmd_queue) {
        LEDE("xQueueCreate napaka!");
        return false;
    }

    s_initialized = true;
    LEDI("led_mgr_init OK — main:%d LED (×10 fizično) signal:%d LED",
         LED_MAIN_LOGICAL, LED_SIGNAL_COUNT);
    return true;
}

// ============================================================
// JAVNI API — implementacije (thread-safe prek queue)
// ============================================================

void led_mgr_set_party_mode(bool party) {
    send_cmd(party ? LedCmd::PARTY_ON : LedCmd::PARTY_OFF, 0);
}

bool led_mgr_is_party_mode() {
    return s_party_mode;
}

void led_mgr_fill(uint32_t speed_ms) {
    if (speed_ms == 0) speed_ms = config_get().fill_speed_ms;
    send_cmd(LedCmd::FILL, speed_ms);
}

void led_mgr_unfill(uint32_t speed_ms) {
    if (speed_ms == 0) speed_ms = config_get().unfill_speed_ms;
    send_cmd(LedCmd::UNFILL, speed_ms);
}

void led_mgr_fade_out(uint32_t duration_ms) {
    if (duration_ms == 0) duration_ms = config_get().fade_duration_ms;
    send_cmd(LedCmd::FADE_OUT, duration_ms);
}

void led_mgr_on_immediate() {
    send_cmd(LedCmd::ON_IMMEDIATE, 0);
}

void led_mgr_off_immediate() {
    send_cmd(LedCmd::OFF_IMMEDIATE, 0);
}

void led_mgr_set_brightness(uint8_t brightness) {
    send_cmd(LedCmd::SET_BRIGHTNESS, (uint32_t)brightness);
}

uint8_t led_mgr_get_brightness() {
    return s_brightness_main;
}

bool led_mgr_is_animating() {
    return (s_anim_state != AnimState::IDLE);
}

void led_mgr_parking_assist(ParkPlace place, uint32_t dist_mm) {
    if (place == ParkPlace::A) {
        signal_led_parking_update(dist_mm);
    }
    // send_cmd(LedCmd::PARKING_ASSIST, dist_mm);  // opuščeno
}

void led_mgr_parking_assist_off() {
    signal_led_parking_stop();
    // send_cmd(LedCmd::PARKING_ASSIST_OFF, 0);  // opuščeno
}

void led_mgr_show_clock(uint32_t duration_ms) {
    // duration_ms se ignorira — signal_led bere iz SIG_CLOCK_DURATION_MS (config.h)
    (void)duration_ms;
    signal_led_clock_show();
    // send_cmd(LedCmd::SHOW_CLOCK, duration_ms);  // opuščeno
}

void led_mgr_celica_timer(bool active) {
    // Stara API brez ločenih celica1/celica2 in is_night — predpostavi celica1=active
    // Pravi klici gredo prek on_cell_broken() EventBus handlerja v light_logic.cpp
    if (active) {
        signal_led_photocell_update(true, false, false);
    } else {
        signal_led_photocell_stop();
    }
    // send_cmd(active ? LedCmd::CELICA_TIMER_ON : LedCmd::CELICA_TIMER_OFF, 0);  // opuščeno
}

void led_mgr_signal_off() {
    signal_led_off();
    // send_cmd(LedCmd::SIGNAL_OFF, 0);  // opuščeno
}

bool led_mgr_ok() {
    return s_initialized;
}

void led_mgr_set_ready(bool ready) {
    s_startup_ready = ready;
    if (ready) {
        LEDI("led_mgr_set_ready(true) — LED procesiranje omogočeno");
    } else {
        LEDI("led_mgr_set_ready(false) — LED procesiranje blokirano");
    }
}

bool led_mgr_is_ready() {
    return s_startup_ready;
}

LedMgrStats led_mgr_get_stats() {
    LedMgrStats st;
    st.initialized           = s_initialized;
    st.party_mode            = s_party_mode;
    st.brightness_main       = s_brightness_main;
    st.brightness_signal     = s_brightness_signal;
    st.animating             = (s_anim_state != AnimState::IDLE);
    st.fill_count            = s_fill_count;
    st.unfill_count          = s_unfill_count;
    st.fadeout_count         = s_fadeout_count;
    st.parking_assist_updates = s_pa_updates;
    return st;
}

// ============================================================
// ANIMACIJA — FILL (ledTask interno)
// ============================================================
// Vsak frame: izračunaj koliko LED prižgemo.
// s_fill_progress: 0.0 = nič, 90.0 = vse prižgane.
// Vrne true ko je animacija končana.

static bool anim_fill_tick() {
    s_fill_progress += s_fill_delta;
    if (s_fill_progress > (float)LED_MAIN_LOGICAL) {
        s_fill_progress = (float)LED_MAIN_LOGICAL;
    }

    int lit = (int)s_fill_progress;
    for (int i = 0; i < LED_MAIN_LOGICAL; i++) {
        s_leds_main[i] = (i < lit) ? WARM_WHITE : CRGB::Black;
    }

    return (lit >= LED_MAIN_LOGICAL);
}

// ============================================================
// ANIMACIJA — UNFILL (ledTask interno)
// ============================================================
// Ravno obratno od fill: gašenje od konca nazaj.
// s_fill_progress: 90.0 = vse prižgane, 0.0 = vse ugašene.

static bool anim_unfill_tick() {
    s_fill_progress -= s_fill_delta;
    if (s_fill_progress < 0.0f) s_fill_progress = 0.0f;

    int lit = (int)s_fill_progress;
    for (int i = 0; i < LED_MAIN_LOGICAL; i++) {
        s_leds_main[i] = (i < lit) ? WARM_WHITE : CRGB::Black;
    }

    return (lit <= 0);
}

// ============================================================
// ANIMACIJA — FADE OUT (ledTask interno)
// ============================================================
// Linearno zniževanje brightness od s_fade_start_br do 0.

static bool anim_fade_tick() {
    uint32_t elapsed = millis() - s_fade_start_ms;
    if (elapsed >= s_fade_duration) {
        FastLED.setBrightness(0);
        fill_main_solid(CRGB::Black);
        s_brightness_main = 0;
        return true;
    }
    float progress = (float)elapsed / (float)s_fade_duration;
    uint8_t br = (uint8_t)((1.0f - progress) * (float)s_fade_start_br);
    FastLED.setBrightness(br);
    return false;
}

// ============================================================
// OPUŠČENO — 2026-05: preneseno v signal_led.cpp
// Te funkcije so bile placeholder implementacije signalnih animacij.
// signal_led.cpp implementira polne verzije vseh scenarijev.
// ============================================================

/*
static void anim_parking_assist_tick(const Config& cfg) { ... }
static void anim_clock_tick() { ... }
static void anim_celica_tick() { ... }
*/

// ============================================================
// PARTY MODE PREKLOP (ledTask interno — pod zahteva safe_show pred)
// ============================================================

static void do_party_on() {
    LEDI("PARTY MODE ON — ugašam FastLED, preklapljam MUX na WLED");
    // 1. Ugasni vse LED v bufferjih
    fill_main_solid(CRGB::Black);
    fill_signal_solid(CRGB::Black);
    FastLED.show();
    // 2. Pavza — zagotovi da WS2815 prejme reset pulse
    vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
    // 3. MUX na WLED
    digitalWrite(PIN_MUX_SELECT, HIGH);
    s_party_mode = true;
    // Resetiraj animacijo
    s_anim_state = AnimState::IDLE;
    LEDI("MUX IO%d = HIGH (WLED aktiven)", PIN_MUX_SELECT);
}

static void do_party_off() {
    LEDI("PARTY MODE OFF — preklapljam MUX na FastLED");
    // 1. MUX nazaj na FastLED
    digitalWrite(PIN_MUX_SELECT, LOW);
    s_party_mode = false;
    // 2. Pavza — zagotovi da WLED ne moti
    vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
    // 3. Obnovi svetlost in stanje
    FastLED.setBrightness(s_brightness_main);
    fill_main_solid(CRGB::Black);
    FastLED.show();
    LEDI("MUX IO%d = LOW (FastLED aktiven)", PIN_MUX_SELECT);
}

// ============================================================
// ledTask — FreeRTOS task
// ============================================================

void ledTask(void* pvParams) {
    LEDI("ledTask start — Core%d", xPortGetCoreID());

    // Inicializacija
    if (!led_mgr_init()) {
        LEDE("led_mgr_init napaka — ledTask se ustavlja!");
        vTaskDelete(nullptr);
        return;
    }

    LEDI("ledTask v zanki (tick: %d ms)", LEDTASK_TICK_MS);

    TickType_t last_wake = xTaskGetTickCount();

    // Zabeleži čas zagona za startup delay
    s_startup_time_ms = millis();
    LEDI("Startup delay: %d ms — čakam stabilizacijo sistema", LED_STARTUP_DELAY_MS);

    while (true) {
        // -------------------------------------------------------
        // 0. Startup ready check
        // -------------------------------------------------------
        if (!s_startup_ready) {
            if ((millis() - s_startup_time_ms) >= LED_STARTUP_DELAY_MS) {
                s_startup_ready = true;
                LEDI("Startup delay iztekel — LED procesiranje omogočeno");
            } else {
                // Še čakamo — spi in ne delaj ničesar
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        // -------------------------------------------------------
        // 1. Preberi ukaz iz queue (non-blocking)
        // -------------------------------------------------------
        LedCmdMsg msg;
        while (xQueueReceive(s_cmd_queue, &msg, 0) == pdTRUE) {

            const Config cfg = config_get();

            switch (msg.cmd) {

                case LedCmd::FILL:
                    if (s_party_mode) {
                        LEDW("FILL ukaz ignoriran — party mode aktiven");
                        break;
                    }
                    s_fill_progress = 0.0f;
                    // delta = LED/frame = LED_MAIN_LOGICAL / (speed_ms / tick_ms)
                    s_fill_delta = (float)LED_MAIN_LOGICAL /
                                   ((float)msg.payload / (float)LEDTASK_TICK_MS);
                    if (s_fill_delta <= 0.0f) s_fill_delta = 0.1f;
                    // Obnovi brightness na normalno vrednost
                    FastLED.setBrightness(s_brightness_main);
                    s_anim_state = AnimState::FILLING;
                    s_fill_count++;
                    LEDD("FILL start: speed=%lu ms, delta=%.3f LED/frame",
                         (unsigned long)msg.payload, s_fill_delta);
                    break;

                case LedCmd::UNFILL:
                    if (s_party_mode) {
                        LEDW("UNFILL ukaz ignoriran — party mode aktiven");
                        break;
                    }
                    // Začnemo od trenutnega stanja (ne nujno 90.0)
                    if (s_anim_state != AnimState::FILLING) {
                        s_fill_progress = (float)LED_MAIN_LOGICAL;
                    }
                    s_fill_delta = (float)LED_MAIN_LOGICAL /
                                   ((float)msg.payload / (float)LEDTASK_TICK_MS);
                    if (s_fill_delta <= 0.0f) s_fill_delta = 0.1f;
                    s_anim_state = AnimState::UNFILLING;
                    s_unfill_count++;
                    LEDD("UNFILL start: speed=%lu ms, delta=%.3f LED/frame",
                         (unsigned long)msg.payload, s_fill_delta);
                    break;

                case LedCmd::FADE_OUT:
                    if (s_party_mode) break;
                    s_fade_start_ms  = millis();
                    s_fade_duration  = msg.payload;
                    s_fade_start_br  = s_brightness_main;
                    if (s_fade_duration == 0) s_fade_duration = 1;
                    s_anim_state = AnimState::FADING;
                    s_fadeout_count++;
                    LEDD("FADE_OUT start: duration=%lu ms", (unsigned long)msg.payload);
                    break;

                case LedCmd::ON_IMMEDIATE:
                    if (s_party_mode) break;
                    s_anim_state = AnimState::IDLE;
                    FastLED.setBrightness(s_brightness_main);
                    fill_main_solid(WARM_WHITE);
                    s_fill_progress = (float)LED_MAIN_LOGICAL;
                    LEDD("ON_IMMEDIATE");
                    break;

                case LedCmd::OFF_IMMEDIATE:
                    if (s_party_mode) break;
                    s_anim_state = AnimState::IDLE;
                    fill_main_solid(CRGB::Black);
                    s_fill_progress = 0.0f;
                    LEDD("OFF_IMMEDIATE");
                    break;

                case LedCmd::SET_BRIGHTNESS:
                    s_brightness_main = (uint8_t)msg.payload;
                    if (!s_party_mode) {
                        FastLED.setBrightness(s_brightness_main);
                    }
                    LEDD("SET_BRIGHTNESS: %u", (unsigned)s_brightness_main);
                    break;

                case LedCmd::PARTY_ON:
                    if (!s_party_mode) do_party_on();
                    break;

                case LedCmd::PARTY_OFF:
                    if (s_party_mode) do_party_off();
                    break;

                // -----------------------------------------------
                // OPUŠČENO (2026-05): wrapper funkcije kličejo
                // signal_led_* direktno, ne prek queue.
                // Case-i ostajajo za kompatibilnost — send_cmd se ne
                // kliče več za te ukaze. Ob naslednjem refactoringu: odstrani.
                // -----------------------------------------------
                case LedCmd::PARKING_ASSIST:
                    s_pa_dist_mm = msg.payload;
                    s_sig_state  = AnimState::PARKING_ASSIST;
                    s_pa_updates++;
                    LEDD("PARKING_ASSIST: dist=%lu mm", (unsigned long)s_pa_dist_mm);
                    break;

                case LedCmd::PARKING_ASSIST_OFF:
                    if (s_sig_state == AnimState::PARKING_ASSIST) {
                        s_sig_state = AnimState::IDLE;
                        fill_signal_solid(CRGB::Black);
                        LEDD("PARKING_ASSIST_OFF");
                    }
                    break;

                case LedCmd::SHOW_CLOCK:
                    s_clock_end_ms = millis() + msg.payload;
                    s_sig_state    = AnimState::CLOCK_SHOW;
                    LEDD("SHOW_CLOCK: duration=%lu ms", (unsigned long)msg.payload);
                    break;

                case LedCmd::CELICA_TIMER_ON:
                    s_sig_state      = AnimState::CELICA_BLINK;
                    s_celica_blink_ms = millis();
                    s_celica_blink_on = true;
                    LEDD("CELICA_TIMER ON");
                    break;

                case LedCmd::CELICA_TIMER_OFF:
                    if (s_sig_state == AnimState::CELICA_BLINK) {
                        s_sig_state = AnimState::IDLE;
                        fill_signal_zone(LED_SIG_ZONE_BOT_START, LED_SIG_ZONE_BOT_END, CRGB::Black);
                        LEDD("CELICA_TIMER OFF");
                    }
                    break;

                case LedCmd::SIGNAL_OFF:
                    s_sig_state = AnimState::IDLE;
                    fill_signal_solid(CRGB::Black);
                    LEDD("SIGNAL_OFF");
                    break;

                default:
                    LEDW("Neznan LED ukaz: %d", (int)msg.cmd);
                    break;
            }
        }

        // -------------------------------------------------------
        // 2. Animacijski frame — glavna matrika
        // -------------------------------------------------------
        if (!s_party_mode) {
            bool done = false;
            switch (s_anim_state) {
                case AnimState::FILLING:
                    done = anim_fill_tick();
                    s_led_dirty = true;
                    if (done) {
                        s_anim_state = AnimState::IDLE;
                        LEDI("FILL animacija končana");
                    }
                    break;

                case AnimState::UNFILLING:
                    done = anim_unfill_tick();
                    s_led_dirty = true;
                    if (done) {
                        s_anim_state = AnimState::IDLE;
                        fill_main_solid(CRGB::Black);
                        LEDI("UNFILL animacija končana");
                    }
                    break;

                case AnimState::FADING:
                    done = anim_fade_tick();
                    s_led_dirty = true;
                    if (done) {
                        s_anim_state = AnimState::IDLE;
                        LEDI("FADE_OUT animacija končana");
                    }
                    break;

                case AnimState::IDLE:
                default:
                    // Nič — ne kliči show() če se ni nič spremenilo
                    // (bo klicano po signal LED posodobitvi)
                    break;
            }
        }

        // -------------------------------------------------------
        // 3. Animacijski frame — signalna LED
        // -------------------------------------------------------
        // signal_led_tick() je centralni dispečer za vse signalne scenarije.
        // Piše direktno v s_leds_signal[] buffer.
        // FastLED.show() (korak 4) bo te vrednosti poslal na IO40.
        if (signal_led_tick()) {
            s_led_dirty = true;
        }

        // -------------------------------------------------------
        // 4. FastLED.show() — samo ob dejanski spremembi bufferja
        // -------------------------------------------------------
        if (s_led_dirty) {
            s_led_dirty = false;
            s_tick_ms = 20;   // animacija aktivna — ostani na 50 Hz
            safe_show();
        } else {
            s_tick_ms = 100;  // IDLE — 10 Hz, brez show(), brez RMT DMA
        }

        // -------------------------------------------------------
        // 5. Čakaj do naslednjega tika (adaptive: 20ms ali 100ms)
        // -------------------------------------------------------
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_tick_ms));
    }
}
