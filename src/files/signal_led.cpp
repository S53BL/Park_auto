// ============================================================
// signal_led.cpp — Signalna LED veriga: prioritetni dispečer + animacije
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// IMPLEMENTACIJSKE ODLOČITVE:
//
// [1] Buffer ownership:
//     s_leds_signal[] je v lasti led_manager.cpp (alociran v PSRAM).
//     signal_led.cpp prejme kazalec ob init in piše vanj direktno.
//     FastLED.show() kliče led_manager (ne mi). To pomeni da piše
//     led_manager in signal_led v IST I fizični buffer — to je OK ker
//     oba tečeta ZAPOREDNO v ledTask zanki (ne hkrati).
//
// [2] Thread safety:
//     signal_led_tick() teče v ledTask (Core1).
//     signal_led_set_*() kliče light_logic (Core1, appTask) in
//     sensor_mgr (Core1, sensorTask) — oba na isti jedrni gruči.
//     Ker je ESP32-S3 dual-core in sta appTask+sensorTask na Core1,
//     FreeRTOS scheduler zagotavlja da ne tečeta hkrati.
//     ledTask je prav tako na Core1 → scheduler skrbi za serializacijo.
//     Zaključek: ni potrebe po mutexa za volatile bool/uint32_t spremenljivke.
//     ⚠ Če bi appTask preselili na Core0: dodati mutex za s_active_* flagje.
//
// [3] Comet implementacija (rampa animacija):
//     Comet = glava + rep.
//     Glava: ena LED na polni svetlosti (warning orange).
//     Rep: zadnjih COMET_TAIL_LEN LED za glavo, linearno pojema od
//          polne svetlosti do 0. Rep ima gradient orange→yellow.
//     Implementacija: za vsak frame izračunamo pozicijo glave.
//     Hitrost: COMET_SPEED_LEDS_PER_SEC LED/s.
//     Smer: navzgor (idx++) ali navzdol (idx--) glede na RampDir.
//
// [4] "Pokvarjen pixel" efekt (vsak 2. prehod cometa):
//     Ko comet doseže konec trase, se pojavi naključen "defect" pixel.
//     Pixel stoji na fiksni poziciji (nasprotni konec od smeri gibanja).
//     Ko ga comet doseže: pixel se postopoma integrira v comet barvo.
//     Ko je barva popolnoma prevzeta: pixel "izgine" in comet se podaljša.
//     Implementacija: s_defect_pixel_pos, s_defect_pixel_color,
//     s_defect_integration_progress (0.0→1.0).
//
// [5] Eksplozija (konec rampa animacije):
//     Ko rampaluc izgine: comet se upočasni in ustavi.
//     Nato "razpade": posamezni piksli se razletijo.
//     Implementacija: s_explode_particles[] — vsak delec ima pozicijo,
//     hitrost in brightness. 5s fadeout z organskim poskokom
//     (vsak delec naključno zamakne ±1 LED preden ugasne).
//
// [6] Parking assist (P2):
//     Dokumentacija opisuje "premikajoče se skupino 8–12 LED".
//     Implementacija: skupina = 10 LED.
//     Pozicija se izračuna iz razdalje: dist → pozicija na traku.
//     Mapping: dist=1500mm → pos=0 (vrh), dist=0mm → pos=134 (dno).
//     Cona (barva) se določi iz config pragov (SIG_PARK_THRESH_*).
//     Utripanje pri <500mm: group_visible toggle vsakih PA_BLINK_MS.
//
// [7] Analogna ura (P4):
//     144 LED → 6 segmentov × 22 LED + 7 ločilnih pik × 1 LED = 139 LED.
//     Preostalih 5 LED (139–143) = padding, vedno dimmed 5%.
//     6 segmentov = 6 ur (vsak segment = 1 ura v 6-urni coni).
//     Ure so rumeno-bele (CRGB(255, 220, 80) pri target_brightness).
//     Minute: zeleni 2-LED marker ki se premika znotraj aktivnega segmenta.
//     Timer: 10s (SIG_CLOCK_DURATION_MS), reset ob vsakem novem klicu.
//
// ============================================================

#include "signal_led.h"
#include "config.h"
#include "config_mgr.h"
#include "logger.h"
#include <FastLED.h>
#include <time.h>
#include <math.h>

// ============================================================
// LOGGING MAKROJI
// ============================================================

#define SIGI(fmt, ...) LOG_INFO ("SIGLED", fmt, ##__VA_ARGS__)
#define SIGW(fmt, ...) LOG_WARN ("SIGLED", fmt, ##__VA_ARGS__)
#define SIGE(fmt, ...) LOG_ERROR("SIGLED", fmt, ##__VA_ARGS__)
#define SIGD(fmt, ...) LOG_DEBUG("SIGLED", fmt, ##__VA_ARGS__)

// ============================================================
// KONSTANTE — fizična topologija
// ============================================================

// Cone na 144-LED traku (vrednosti iz config.h)
#define SIG_BOT_START   LED_SIG_ZONE_BOT_START   //   0
#define SIG_BOT_END     LED_SIG_ZONE_BOT_END      //  47
#define SIG_MID_START   LED_SIG_ZONE_MID_START    //  48
#define SIG_MID_END     LED_SIG_ZONE_MID_END      //  95
#define SIG_TOP_START   LED_SIG_ZONE_TOP_START    //  96
#define SIG_TOP_END     LED_SIG_ZONE_TOP_END      // 143

#define SIG_TOTAL       LED_SIGNAL_COUNT          // 144

// ============================================================
// KONSTANTE — barve (WS2815 kalibrirane)
// ============================================================

// Rampa animacija
static const CRGB CLR_RAMP_HEAD   = CRGB(255,  80,   0);  // warning orange
static const CRGB CLR_RAMP_MID    = CRGB(255, 160,  20);  // rep sredina
static const CRGB CLR_RAMP_TAIL   = CRGB(200, 220,   0);  // rep konec (rumenkast)
static const CRGB CLR_RAMP_PULSE  = CRGB(255, 140,   0);  // uvodni pulsi

// Parking assist
static const CRGB CLR_PA_GREEN    = CRGB(  0, 200,   0);
static const CRGB CLR_PA_ORANGE   = CRGB(255, 120,   0);
static const CRGB CLR_PA_RED      = CRGB(210,   0,   0);
static const CRGB CLR_PA_CONFIRM  = CRGB(  0, 200,   0);  // zelena wave ob koncu

// Fotocelice
static const CRGB CLR_CELL1_DAY   = CRGB(200,  20,  20);  // rdeče/oranžno — zunanja
static const CRGB CLR_CELL2_DAY   = CRGB( 40,  60, 220);  // modro/rumeno — notranja
static const CRGB CLR_CELL1_NIGHT = CRGB(160,   5,   5);  // nočna (bolj ugasnjena)
static const CRGB CLR_CELL2_NIGHT = CRGB( 20,  30, 160);  // nočna

// Analogna ura
static const CRGB CLR_HOUR_SEG    = CRGB(255, 220,  80);  // segmenti (ure)
static const CRGB CLR_MIN_MARKER  = CRGB(  0, 200,  80);  // minute marker (zelena)
static const CRGB CLR_SEP_DOT     = CRGB( 30,  30,  30);  // ločilna pika (dimmed)
static const CRGB CLR_PADDING     = CRGB(  8,   8,   8);  // padding (5%)

// ============================================================
// KONSTANTE — animacijski parametri
// ============================================================

// Rampa — comet
#define COMET_TAIL_LEN          20      // dolžina repa v LED
#define COMET_SPEED_NORMAL      60      // LED/s pri normalnem gibanju
#define COMET_SPEED_SLOW        15      // LED/s pri upočasnitvi (konec)
#define COMET_PULSE_DURATION_MS 700     // trajanje enega uvodnega pulsa
#define COMET_PULSE_HOLD_MS     300     // zadržanje na vrhu pulsa
#define COMET_PAUSE_MS          250     // pavza med pulsoma

// Rampa — pokvarjen pixel (vsak 2. prehod)
#define DEFECT_INTEGRATION_SPEED 0.05f  // stopnja integracije na frame (0=takojšen, 1=počasen)

// Rampa — eksplozija
#define EXPLODE_PARTICLE_COUNT  18      // št. delcev eksplozije
#define EXPLODE_FADE_TOTAL_MS   5000    // skupni čas fadeout po eksploziji
#define EXPLODE_SETTLE_MS       300     // čas "razletavanja" pred fadeout

// Parking assist
#define PA_GROUP_SIZE           10      // LED v premikajoči skupini
#define PA_BG_DIM               15      // svetlost ozadja (0–255)
#define PA_BLINK_MS             350     // utripanje pri kritični razdalji
#define PA_WAVE_DURATION_MS     800     // zelena wave ob koncu
#define PA_FADEOUT_MS           500     // fadeout po wave

// Fotocelice
#define CELL_BLINK_PERIOD_MS    1000    // period 1 Hz
#define CELL_BLINK_DUTY         500     // ON trajanje pri 1Hz (50% duty)
#define CELL_NIGHT_LED_COUNT    2       // št. LED pri nočnem prikazu (1–3)
#define CELL_NIGHT_BRIGHTNESS   30      // svetlost pri nočnem prikazu (0–255)
#define CELL_TIMER_RESET_MS     SIG_CELL_TIMER_MS  // 5 min (config.h)

// Analogna ura — segmentna topologija (144 LED)
// Razporeditev:
//   LED   0      = ločilna pika 1 (spodnja)
//   LED   1–22   = segment 1  (ura 1)
//   LED  23      = ločilna pika 2
//   LED  24–45   = segment 2  (ura 2)
//   LED  46      = ločilna pika 3
//   LED  47–68   = segment 3  (ura 3)
//   LED  69      = ločilna pika 4
//   LED  70–91   = segment 4  (ura 4)
//   LED  92      = ločilna pika 5
//   LED  93–114  = segment 5  (ura 5)
//   LED 115      = ločilna pika 6
//   LED 116–137  = segment 6  (ura 6 — rezerviran za minute marker)
//   LED 138      = ločilna pika 7 (zgornja)
//   LED 139–143  = padding (5% dimmed)
//
// Skupaj: 7 pik + 6×22 = 7 + 132 = 139 + 5 padding = 144 ✓

#define CLOCK_SEG_COUNT         6       // 6 segmentov
#define CLOCK_SEG_LEN           22      // LED na segment
#define CLOCK_SEP_COUNT         7       // 7 ločilnih pik
static const uint8_t CLOCK_SEP_POS[7] = { 0, 23, 46, 69, 92, 115, 138 };
static const uint8_t CLOCK_SEG_START[6] = { 1, 24, 47, 70, 93, 116 };
#define CLOCK_PADDING_START     139
#define CLOCK_MIN_MARKER_LEN    2       // dolžina minute markerja v LED

// ============================================================
// NOTRANJE STANJE
// ============================================================

// Kazalec na FastLED buffer (prejet ob init iz led_manager.cpp)
static CRGB*    s_buf       = nullptr;
static uint16_t s_buf_count = 0;
static bool     s_initialized = false;

// Trenutno aktiven način (dispečer)
static SignalMode s_current_mode = SignalMode::SIG_IDLE;

// -----------------------------------------------
// Aktivnostni flagji za vsak način (atomic-safe)
// -----------------------------------------------
// Postavljeni iz set_*() funkcij, brani v tick()
// Vse so volatile ker jih tick() bere med ko jih drug task piše.
static volatile bool     s_ramp_active       = false;
static volatile RampDir  s_ramp_dir          = RampDir::UP;
static volatile bool     s_ramp_stopping     = false;   // rampaluc je šel HIGH, sproži konec

static volatile bool     s_parking_active    = false;
static volatile uint32_t s_parking_dist_mm   = 9999;
static volatile bool     s_parking_stopping  = false;

static volatile bool     s_cell_active       = false;
static volatile bool     s_cell1_broken      = false;
static volatile bool     s_cell2_broken      = false;
static volatile bool     s_cell_is_night     = false;
static volatile uint32_t s_cell_start_ms     = 0;       // čas zadnje aktivacije (za 5min timer)

static volatile bool     s_clock_active      = false;
static volatile uint32_t s_clock_end_ms      = 0;

// -----------------------------------------------
// Stanje rampa animacije (samo tick() piše/bere)
// -----------------------------------------------

enum class RampPhase : uint8_t {
    RAMP_PULSE1,        // uvodni pulz 1
    RAMP_PAUSE1,        // pavza med pulsoma
    RAMP_PULSE2,        // uvodni pulz 2 (krajši)
    RAMP_COMET,         // glavni comet loop
    RAMP_SLOW,          // upočasnitev (rampaluc izgine)
    RAMP_EXPLODE,       // eksplozija
    RAMP_FADEOUT,       // 5s fadeout delcev
    RAMP_DONE,          // animacija zaključena
};

struct ExplodeParticle {
    float    pos;       // pozicija (float za sub-pixel gibanje)
    float    vel;       // hitrost (LED/s, pozitivna = gor, negativna = dol)
    uint8_t  bri;       // trenutna svetlost
    CRGB     color;     // barva delca
    bool     settled;   // ali je delec že naredil "poskok" pred ugasnitvijo
    bool     active;    // ali je delec aktiven
};

static RampPhase s_ramp_phase          = RampPhase::RAMP_PULSE1;
static uint32_t  s_ramp_phase_start_ms = 0;    // čas začetka trenutne faze
static float     s_comet_pos           = 0.0f; // pozicija glave (0.0–143.0)
static float     s_comet_speed         = (float)COMET_SPEED_NORMAL; // LED/s
static uint8_t   s_comet_pass_count    = 0;    // koliko prehodov je comet opravil
static bool      s_defect_active       = false;
static int       s_defect_pos          = -1;
static CRGB      s_defect_color        = CRGB::Black;
static float     s_defect_integration  = 0.0f; // 0.0 = originaln barva, 1.0 = comet barva
static ExplodeParticle s_particles[EXPLODE_PARTICLE_COUNT] = {};

// -----------------------------------------------
// Stanje parking assist (samo tick() piše/bere)
// -----------------------------------------------

enum class ParkPhase : uint8_t {
    PARK_ACTIVE,        // normalni prikaz razdalje
    PARK_WAVE,          // zelena wave ob koncu
    PARK_FADEOUT,       // fadeout pred IDLE
    PARK_DONE,
};

static ParkPhase  s_park_phase       = ParkPhase::PARK_ACTIVE;
static uint32_t   s_park_phase_ms    = 0;
static bool       s_park_blink_on    = true;
static uint32_t   s_park_blink_ms    = 0;
static float      s_park_wave_pos    = 0.0f;  // pozicija zelene wave (0→143)

// -----------------------------------------------
// Stanje fotocelice (samo tick() piše/bere)
// -----------------------------------------------

static uint32_t  s_cell_blink_ms     = 0;     // čas zadnjega blink toggle
static bool      s_cell1_blink_on    = false;
static bool      s_cell2_blink_on    = false;

// -----------------------------------------------
// Diagnostika
// -----------------------------------------------

static SignalLedStats s_stats = {};

// ============================================================
// POMOŽNE FUNKCIJE — buffer operacije
// ============================================================

// Počisti cel buffer (vse LED na Black)
static inline void buf_clear() {
    if (!s_buf) return;
    for (uint16_t i = 0; i < s_buf_count; i++) s_buf[i] = CRGB::Black;
}

// Nastavi cono [start, end] na barvo
static inline void buf_fill_zone(int start, int end, CRGB color) {
    if (!s_buf) return;
    for (int i = start; i <= end && i < s_buf_count; i++) s_buf[i] = color;
}

// Nastavi eno LED (z mejno preverjanjem)
static inline void buf_set(int idx, CRGB color) {
    if (!s_buf || idx < 0 || idx >= s_buf_count) return;
    s_buf[idx] = color;
}

// Mešaj barvo z obstoječo (za comet rep na obstoječem ozadju)
// ratio: 0.0 = ohrani obstoječe, 1.0 = polna nova barva
static inline void buf_blend(int idx, CRGB color, float ratio) {
    if (!s_buf || idx < 0 || idx >= s_buf_count) return;
    float r = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    s_buf[idx].r = (uint8_t)(s_buf[idx].r * (1.0f - r) + color.r * r);
    s_buf[idx].g = (uint8_t)(s_buf[idx].g * (1.0f - r) + color.g * r);
    s_buf[idx].b = (uint8_t)(s_buf[idx].b * (1.0f - r) + color.b * r);
}

// Skaliraj svetlost LED z faktorjem (0.0–1.0)
static inline void buf_scale(int idx, float factor) {
    if (!s_buf || idx < 0 || idx >= s_buf_count) return;
    float f = factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
    s_buf[idx].r = (uint8_t)(s_buf[idx].r * f);
    s_buf[idx].g = (uint8_t)(s_buf[idx].g * f);
    s_buf[idx].b = (uint8_t)(s_buf[idx].b * f);
}

// Dimmed verzija barve (procent 0–255)
static inline CRGB color_dim(CRGB c, uint8_t dim) {
    return CRGB((c.r * dim) >> 8, (c.g * dim) >> 8, (c.b * dim) >> 8);
}

// Linearna interpolacija med dvema barvama (t=0.0→1.0)
static inline CRGB color_lerp(CRGB a, CRGB b, float t) {
    float tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return CRGB(
        (uint8_t)(a.r + (b.r - a.r) * tc),
        (uint8_t)(a.g + (b.g - a.g) * tc),
        (uint8_t)(a.b + (b.b - a.b) * tc)
    );
}

// ============================================================
// POMOŽNE FUNKCIJE — logika smeri
// ============================================================

// Začetna pozicija cometa glede na smer
static inline float comet_start_pos() {
    return (s_ramp_dir == RampDir::UP) ? 0.0f : (float)(SIG_TOTAL - 1);
}

// Konec trase glede na smer (kam gre comet)
static inline float comet_end_pos() {
    return (s_ramp_dir == RampDir::UP) ? (float)(SIG_TOTAL - 1) : 0.0f;
}

// Ali je comet dosegel konec trase?
static inline bool comet_at_end() {
    if (s_ramp_dir == RampDir::UP)   return s_comet_pos >= (float)(SIG_TOTAL - 1);
    else                              return s_comet_pos <= 0.0f;
}

// Premakni pozicijo za delta (upošteva smer)
static inline float comet_advance(float pos, float delta_leds) {
    return (s_ramp_dir == RampDir::UP) ? pos + delta_leds : pos - delta_leds;
}

// ============================================================
// DISPEČER — posodobi s_current_mode
// ============================================================
// Kliče se na začetku vsake tick() iteracije.
// Vzame način z najvišjo prioriteto med aktivnimi.

static void dispatch_update() {
    SignalMode new_mode;

    // SIG_RAMP je aktiven če:
    //   a) s_ramp_active=true (rampaluc LOW — comet/pulzi tečejo), ALI
    //   b) s_ramp_stopping=true in faza ni zaključena (eksplozija/fadeout še teče)
    // Pogoj (b) zagotavlja da dispečer ostane na SIG_RAMP med celotnim fadeoutom
    // tudi potem ko s_ramp_active postane false (konec fadeout ga ponastavi).
    bool ramp_running = s_ramp_active ||
                        (s_ramp_stopping &&
                         s_ramp_phase != RampPhase::RAMP_DONE &&
                         s_ramp_phase != RampPhase::RAMP_PULSE1);
    if (ramp_running) {
        new_mode = SignalMode::SIG_RAMP;
    } else if (s_parking_active || s_park_phase == ParkPhase::PARK_WAVE ||
               s_park_phase == ParkPhase::PARK_FADEOUT) {
        new_mode = SignalMode::SIG_PARKING;
    } else if (s_cell_active) {
        new_mode = SignalMode::SIG_PHOTOCELL;
    } else if (s_clock_active) {
        new_mode = SignalMode::SIG_CLOCK;
    } else {
        new_mode = SignalMode::SIG_IDLE;
    }

    if (new_mode != s_current_mode) {
        SIGI("DISPEČER: %s → %s",
             signal_led_mode_name(s_current_mode),
             signal_led_mode_name(new_mode));

        // Beležimo prekinitve (višja prioriteta prekine nižjo)
        if ((uint8_t)new_mode > (uint8_t)s_current_mode &&
            s_current_mode != SignalMode::SIG_IDLE) {
            s_stats.preemptions++;
            SIGW("PREKINITEV: %s prekinjen s strani %s",
                 signal_led_mode_name(s_current_mode),
                 signal_led_mode_name(new_mode));
        }

        s_current_mode = new_mode;
        s_stats.mode_changes++;

        // Ob prehodu na IDLE: zagotovi čist buffer
        if (new_mode == SignalMode::SIG_IDLE) {
            buf_clear();
            SIGD("IDLE: buffer počiščen");
        }
    }
}

// ============================================================
// ANIMACIJA — RAMPA (P1)
// ============================================================

// --- Uvodni pulzi ---
// 2× blink v warning oranžni pred comet-om.
// Pulz 1: 0.2s fade-in, 0.3s hold, 0.2s fade-out na 5%.
// Pavza: 0.2–0.3s (implementirano kot COMET_PAUSE_MS).
// Pulz 2: podoben, malo krajši, konec = prehod v comet setup.

static void ramp_draw_pulse(float brightness_fraction) {
    // Napolni cel trak z warning oranžno (dimmirano)
    for (int i = 0; i < SIG_TOTAL; i++) {
        uint8_t bri = (uint8_t)(brightness_fraction * 255.0f);
        s_buf[i] = color_dim(CLR_RAMP_PULSE, bri);
    }
}

static void ramp_tick_pulse(RampPhase pulse_phase, RampPhase next_phase) {
    uint32_t now     = millis();
    uint32_t elapsed = now - s_ramp_phase_start_ms;

    float brightness = 0.0f;
    uint32_t duration = (pulse_phase == RampPhase::RAMP_PULSE1)
                        ? COMET_PULSE_DURATION_MS
                        : (uint32_t)(COMET_PULSE_DURATION_MS * 0.8f);  // pulz 2 je krajši

    if (elapsed < 200) {
        // Fade-in (0–200ms)
        brightness = (float)elapsed / 200.0f;
    } else if (elapsed < 200 + COMET_PULSE_HOLD_MS) {
        // Hold
        brightness = 1.0f;
    } else if (elapsed < duration) {
        // Fade-out do 5%
        uint32_t fade_start = 200 + COMET_PULSE_HOLD_MS;
        float t = (float)(elapsed - fade_start) / (float)(duration - fade_start);
        brightness = 1.0f - (t * 0.95f);  // gre do 5% (ne do 0%)
    } else {
        // Prehod na naslednjo fazo
        s_ramp_phase          = next_phase;
        s_ramp_phase_start_ms = now;
        SIGD("RAMP: faza %d → %d", (int)pulse_phase, (int)next_phase);
        if (next_phase == RampPhase::RAMP_COMET) {
            // Inicializiraj comet ob prehodu iz pulzov na comet
            s_comet_pos        = comet_start_pos();
            s_comet_speed      = (float)COMET_SPEED_NORMAL;
            s_comet_pass_count = 0;
            s_defect_active    = false;
            SIGI("RAMP: comet start (dir=%s, pos=%.1f)",
                 s_ramp_dir == RampDir::UP ? "GOR" : "DOL", (double)s_comet_pos);
        }
        brightness = 0.05f;
    }

    ramp_draw_pulse(brightness);
}

// --- Comet risanje ---
// Nariše comet na trenutni poziciji s_comet_pos.
// Glava: polna barva (CLR_RAMP_HEAD).
// Rep: COMET_TAIL_LEN LED, linearni gradient HEAD→MID→TAIL.
// Smer repa: nasproti smeri gibanja.

static void ramp_draw_comet() {
    buf_clear();

    // Pokvarjen pixel — nariši pred comet-om da comet gre "čezenj"
    if (s_defect_active && s_defect_pos >= 0) {
        CRGB d_color = color_lerp(s_defect_color, CLR_RAMP_HEAD, s_defect_integration);
        buf_set(s_defect_pos, d_color);
    }

    int head = (int)s_comet_pos;
    buf_set(head, CLR_RAMP_HEAD);

    // Rep — teče NASPROTI smeri gibanja
    for (int t = 1; t <= COMET_TAIL_LEN; t++) {
        int tail_idx = (s_ramp_dir == RampDir::UP) ? head - t : head + t;
        if (tail_idx < 0 || tail_idx >= SIG_TOTAL) continue;

        // Gradient: 0 = pri glavi (svetlo), 1 = konec repa (temno)
        float grad = (float)t / (float)COMET_TAIL_LEN;
        CRGB tail_color;
        if (grad < 0.5f) {
            tail_color = color_lerp(CLR_RAMP_HEAD, CLR_RAMP_MID, grad * 2.0f);
        } else {
            tail_color = color_lerp(CLR_RAMP_MID,  CLR_RAMP_TAIL, (grad - 0.5f) * 2.0f);
        }
        // Rep pobledeva z razdaljo
        uint8_t bri = (uint8_t)((1.0f - grad) * 255.0f);
        buf_set(tail_idx, color_dim(tail_color, bri));
    }

    // Integracija pokvarjenega piksla: posodobi progress
    if (s_defect_active && s_defect_pos >= 0) {
        // Preverimo ali je glava blizu defect pozicije
        int dist = abs(head - s_defect_pos);
        if (dist <= COMET_TAIL_LEN) {
            s_defect_integration += DEFECT_INTEGRATION_SPEED;
            if (s_defect_integration >= 1.0f) {
                // Piksel je popolnoma integriran — comet se podaljša
                s_defect_active       = false;
                s_defect_pos          = -1;
                s_defect_integration  = 0.0f;
                SIGD("RAMP: defect pixel integriran → comet daljši");
            }
        }
    }
}

// --- Glavni comet loop ---

static void ramp_tick_comet() {
    // Posodobi pozicijo cometa (delta glede na čas od zadnjega tika)
    // S konstantnim LEDTASK_TICK_MS = 20ms → delta = speed * 0.02 LED
    float delta = s_comet_speed * (20.0f / 1000.0f);  // LED na frame pri 50Hz
    s_comet_pos = comet_advance(s_comet_pos, delta);

    // Nariši trenutni frame
    ramp_draw_comet();

    // Preveri ali smo dosegli konec trase
    if (comet_at_end()) {
        s_comet_pass_count++;
        SIGD("RAMP: comet prehod #%d", s_comet_pass_count);

        // Vsak 2. prehod: sproži pokvarjen pixel efekt
        if (!s_defect_active && (s_comet_pass_count % 2 == 0)) {
            // Defect pixel na NASPROTNEM koncu od smeri
            s_defect_pos = (s_ramp_dir == RampDir::UP) ? 0 : (SIG_TOTAL - 1);
            // Naključna barva (zelena, modra, rumena, vijolična...)
            static const CRGB defect_colors[] = {
                CRGB(0, 200, 0), CRGB(0, 100, 255), CRGB(200, 200, 0),
                CRGB(150, 0, 200), CRGB(0, 200, 150)
            };
            uint8_t rnd = (uint8_t)(millis() & 0xFF) % 5;
            s_defect_color       = defect_colors[rnd];
            s_defect_integration = 0.0f;
            s_defect_active      = true;
            SIGD("RAMP: defect pixel @ LED %d, barva #%02X%02X%02X",
                 s_defect_pos, s_defect_color.r, s_defect_color.g, s_defect_color.b);
        }

        // Obrni smer (comet se vrača)
        s_ramp_dir = (s_ramp_dir == RampDir::UP) ? RampDir::DOWN : RampDir::UP;
        s_comet_pos = comet_start_pos();  // začni od nasprotnega konca

        // Preveri ali je bil rampaluc medtem ugasnjen (s_ramp_stopping)
        if (s_ramp_stopping) {
            SIGI("RAMP: rampaluc ugasnjen med comet → prehod v upočasnitev");
            s_ramp_phase          = RampPhase::RAMP_SLOW;
            s_ramp_phase_start_ms = millis();
        }
    }
}

// --- Upočasnitev in eksplozija ---

static void ramp_tick_slow() {
    // Postopno zmanjšuj hitrost
    float target_speed = (float)COMET_SPEED_SLOW;
    s_comet_speed -= (s_comet_speed - target_speed) * 0.08f;  // eksponentno pojemanje

    float delta = s_comet_speed * (20.0f / 1000.0f);
    s_comet_pos = comet_advance(s_comet_pos, delta);
    ramp_draw_comet();

    // Ko je hitrost dovolj nizka (< 5 LED/s) → ustavi in sproži eksplozijo
    if (s_comet_speed < 5.0f) {
        SIGI("RAMP: comet ustavljen na pos=%.1f → EKSPLOZIJA", (double)s_comet_pos);

        // Inicializiraj delce eksplozije iz trenutne pozicije cometa
        for (int p = 0; p < EXPLODE_PARTICLE_COUNT; p++) {
            s_particles[p].pos     = s_comet_pos;
            // Naključna hitrost ±5–25 LED/s
            float speed = 5.0f + (float)((millis() + p * 37) & 0x1F);  // 5–36 LED/s
            s_particles[p].vel     = ((p % 2) == 0) ? speed : -speed;
            s_particles[p].bri     = 200 + (uint8_t)(p * 3);
            // Barva delca: mešanica HEAD in TAIL
            s_particles[p].color   = color_lerp(CLR_RAMP_HEAD, CLR_RAMP_TAIL,
                                                 (float)p / (float)EXPLODE_PARTICLE_COUNT);
            s_particles[p].settled = false;
            s_particles[p].active  = true;
        }

        s_ramp_phase          = RampPhase::RAMP_EXPLODE;
        s_ramp_phase_start_ms = millis();
        SIGD("RAMP: %d delcev inicializiranih", EXPLODE_PARTICLE_COUNT);
    }
}

static void ramp_tick_explode() {
    buf_clear();
    uint32_t elapsed = millis() - s_ramp_phase_start_ms;

    int active_count = 0;
    for (int p = 0; p < EXPLODE_PARTICLE_COUNT; p++) {
        if (!s_particles[p].active) continue;
        active_count++;

        // Premakni delec
        float delta = s_particles[p].vel * (20.0f / 1000.0f);
        s_particles[p].pos += delta;

        // Odbij od sten (zamenja smer pri robu)
        if (s_particles[p].pos < 0.0f) {
            s_particles[p].pos = 0.0f;
            s_particles[p].vel = fabsf(s_particles[p].vel);
        }
        if (s_particles[p].pos >= (float)(SIG_TOTAL - 1)) {
            s_particles[p].pos = (float)(SIG_TOTAL - 1);
            s_particles[p].vel = -fabsf(s_particles[p].vel);
        }

        // Nariši delec
        buf_set((int)s_particles[p].pos,
                color_dim(s_particles[p].color, s_particles[p].bri));
    }

    // Po EXPLODE_SETTLE_MS: prehod v fadeout
    if (elapsed > EXPLODE_SETTLE_MS) {
        SIGI("RAMP: eksplozija zaključena → FADEOUT (%dms)", EXPLODE_FADE_TOTAL_MS);
        s_ramp_phase          = RampPhase::RAMP_FADEOUT;
        s_ramp_phase_start_ms = millis();
    }
}

static void ramp_tick_fadeout() {
    uint32_t elapsed = millis() - s_ramp_phase_start_ms;
    float progress   = (float)elapsed / (float)EXPLODE_FADE_TOTAL_MS;

    if (progress >= 1.0f) {
        // Fadeout zaključen
        buf_clear();
        s_ramp_phase    = RampPhase::RAMP_DONE;
        s_ramp_active   = false;
        s_ramp_stopping = false;
        SIGI("RAMP: animacija popolnoma zaključena");
        return;
    }

    // Fadeout vseh aktivnih delcev — različne hitrosti
    for (int p = 0; p < EXPLODE_PARTICLE_COUNT; p++) {
        if (!s_particles[p].active) continue;

        // Vsak delec ima svojo hitrost ugašanja (naključna razporeditev)
        float p_offset = (float)p / (float)EXPLODE_PARTICLE_COUNT;
        float p_start  = p_offset * 0.3f;  // začne ugašati z zamikom 0–30% skupnega časa
        float p_prog   = (progress - p_start) / (1.0f - p_start);

        if (p_prog < 0.0f) {
            // Delec še ni začel ugašati — nariši ga polnega
            buf_set((int)s_particles[p].pos,
                    color_dim(s_particles[p].color, s_particles[p].bri));
        } else {
            // Poskok: majhen premik tik pred ugasnitvijo
            if (!s_particles[p].settled && p_prog > 0.8f) {
                s_particles[p].settled = true;
                // ±1 LED premik (naključno)
                int jump = ((p & 1) ? 1 : -1);
                s_particles[p].pos += jump;
                SIGD("RAMP: delec %d poskok (%+d) pred ugasnitvijo", p, jump);
            }

            uint8_t bri = (uint8_t)(s_particles[p].bri * (1.0f - p_prog));
            if (bri < 5) {
                s_particles[p].active = false;
                continue;
            }
            buf_set((int)s_particles[p].pos,
                    color_dim(s_particles[p].color, bri));
        }
    }
}

// Glavni dispatcher za rampa tick
static void tick_ramp() {
    switch (s_ramp_phase) {
        case RampPhase::RAMP_PULSE1:
            ramp_tick_pulse(RampPhase::RAMP_PULSE1, RampPhase::RAMP_PAUSE1);
            break;
        case RampPhase::RAMP_PAUSE1: {
            uint32_t elapsed = millis() - s_ramp_phase_start_ms;
            if (elapsed >= COMET_PAUSE_MS) {
                s_ramp_phase          = RampPhase::RAMP_PULSE2;
                s_ramp_phase_start_ms = millis();
                SIGD("RAMP: pavza → pulz 2");
            }
            // Med pavzo: trak ostane na 5% (zadnje stanje iz pulse1)
            break;
        }
        case RampPhase::RAMP_PULSE2:
            ramp_tick_pulse(RampPhase::RAMP_PULSE2, RampPhase::RAMP_COMET);
            break;
        case RampPhase::RAMP_COMET:
            // Preveri ali je bila zahteva za ustavitev MED comet fazo
            if (s_ramp_stopping) {
                // Počakamo da comet doseže konec trase (kjer se obrne)
                // → RAMP_SLOW se sproži v ramp_tick_comet() ko comet doseže konec
            }
            ramp_tick_comet();
            break;
        case RampPhase::RAMP_SLOW:
            ramp_tick_slow();
            break;
        case RampPhase::RAMP_EXPLODE:
            ramp_tick_explode();
            break;
        case RampPhase::RAMP_FADEOUT:
            ramp_tick_fadeout();
            break;
        case RampPhase::RAMP_DONE:
            // Normalno ne bi smeli biti tu — dispečer bo preklopil v IDLE
            buf_clear();
            break;
    }
}

// ============================================================
// ANIMACIJA — PARKING ASSIST (P2)
// ============================================================

static void tick_parking() {
    uint32_t now = millis();
    const Config cfg = config_get();

    switch (s_park_phase) {

        case ParkPhase::PARK_ACTIVE: {
            buf_clear();

            // Preveri ali se je razdalja stabilizirala (zahteva stop)
            if (s_parking_stopping) {
                SIGI("PARK: razdalja stabilna → zelena wave");
                s_park_phase    = ParkPhase::PARK_WAVE;
                s_park_phase_ms = now;
                s_park_wave_pos = 0.0f;
                break;
            }

            uint32_t dist = s_parking_dist_mm;

            // Določi cono (barvo) glede na razdaljo
            CRGB zone_color;
            bool blink_mode = false;

            if (dist > (uint32_t)cfg.pa_thresh_green_mm) {
                // Prosto — ne prikazuj (samo pri parking assist aktivnem metu)
                // Ker smo v PARK_ACTIVE, samo ugasnemo trak
                break;
            } else if (dist > (uint32_t)cfg.pa_thresh_orange_mm) {
                zone_color = CLR_PA_GREEN;
            } else if (dist > (uint32_t)cfg.pa_thresh_red_mm) {
                zone_color = CLR_PA_ORANGE;
            } else if (dist > 50) {
                zone_color = CLR_PA_RED;
            } else {
                // Kritično blizu — cel trak utripa rdeče
                blink_mode = true;
                zone_color = CLR_PA_RED;
            }

            // Mapping razdalje na pozicijo na traku
            // dist=1500mm → pos=0 (vrh), dist=50mm → pos=134 (dno)
            // Linearen mapping: pos = (1.0 - (dist-50)/(1500-50)) * 134
            float rel   = 1.0f - ((float)(dist > 50 ? dist - 50 : 0)) / 1450.0f;
            float clamped = rel < 0.0f ? 0.0f : (rel > 1.0f ? 1.0f : rel);
            int group_center = (int)(clamped * (float)(SIG_TOTAL - PA_GROUP_SIZE));
            int group_start  = group_center;
            int group_end    = group_center + PA_GROUP_SIZE - 1;

            // Spodnja stalna referenca — vedno prižgana (5 LED)
            // Voznik vidi "do kje gre" (zadnja stena)
            buf_fill_zone(SIG_TOTAL - 5, SIG_TOTAL - 1, CLR_PA_RED);

            if (blink_mode) {
                // Utripanje celega traku
                if ((now - s_park_blink_ms) >= PA_BLINK_MS) {
                    s_park_blink_ms = now;
                    s_park_blink_on = !s_park_blink_on;
                }
                if (s_park_blink_on) {
                    buf_fill_zone(0, SIG_TOTAL - 6, CLR_PA_RED);
                }
            } else {
                // Ozadje — dimmed verzija cone barve
                CRGB bg = color_dim(zone_color, PA_BG_DIM);
                buf_fill_zone(0, SIG_TOTAL - 6, bg);

                // Premikajoča skupina (brez blink)
                buf_fill_zone(group_start, group_end, zone_color);
            }

            s_stats.parking_updates++;
            break;
        }

        case ParkPhase::PARK_WAVE: {
            // Zelena wave sweepа od vrha navzdol
            float wave_speed = (float)SIG_TOTAL / ((float)PA_WAVE_DURATION_MS / 20.0f);
            s_park_wave_pos += wave_speed;

            buf_clear();
            for (int i = 0; i < SIG_TOTAL; i++) {
                float dist_from_wave = s_park_wave_pos - (float)i;
                if (dist_from_wave > 0.0f && dist_from_wave < 15.0f) {
                    float bri = 1.0f - (dist_from_wave / 15.0f);
                    buf_set(i, color_dim(CLR_PA_CONFIRM, (uint8_t)(bri * 200.0f)));
                }
            }

            if (s_park_wave_pos >= (float)(SIG_TOTAL + 15)) {
                SIGD("PARK: zelena wave zaključena → fadeout");
                s_park_phase    = ParkPhase::PARK_FADEOUT;
                s_park_phase_ms = now;
            }
            break;
        }

        case ParkPhase::PARK_FADEOUT: {
            uint32_t elapsed = now - s_park_phase_ms;
            float progress   = (float)elapsed / (float)PA_FADEOUT_MS;

            if (progress >= 1.0f) {
                buf_clear();
                s_park_phase    = ParkPhase::PARK_DONE;
                s_parking_active   = false;
                s_parking_stopping = false;
                SIGI("PARK: animacija zaključena → IDLE");
            } else {
                float bri = 1.0f - progress;
                for (int i = 0; i < SIG_TOTAL; i++) {
                    buf_scale(i, bri);
                }
            }
            break;
        }

        case ParkPhase::PARK_DONE:
            buf_clear();
            break;
    }
}

// ============================================================
// ANIMACIJA — FOTOCELICE (P3)
// ============================================================

static void tick_photocell() {
    uint32_t now = millis();

    // 5-minutni timer — preveri iztekanje
    if (s_cell_start_ms > 0 &&
        (now - s_cell_start_ms) >= CELL_TIMER_RESET_MS) {
        SIGI("CELL: 5-minutni timer iztekel → samodejni izklop");
        s_cell_active  = false;
        s_cell1_broken = false;
        s_cell2_broken = false;
        buf_clear();
        return;
    }

    // Blink toggle (1 Hz, 50% duty)
    if ((now - s_cell_blink_ms) >= CELL_BLINK_DUTY) {
        s_cell_blink_ms  = now;
        s_cell1_blink_on = !s_cell1_blink_on;
        s_cell2_blink_on = !s_cell2_blink_on;
        s_stats.photocell_blinks++;
    }

    buf_clear();

    if (s_cell_is_night) {
        // -------------------------------------------------------
        // Nočni diskretni mode: samo 1–3 LED na coni
        // Zelo nizka svetlost (CELL_NIGHT_BRIGHTNESS)
        // -------------------------------------------------------

        if (s_cell1_broken && s_cell1_blink_on) {
            // celica1 → spodnja cona, CELL_NIGHT_LED_COUNT LED pri dnu
            for (int i = 0; i < CELL_NIGHT_LED_COUNT; i++) {
                buf_set(SIG_BOT_START + i,
                        color_dim(CLR_CELL1_NIGHT, CELL_NIGHT_BRIGHTNESS));
            }
        }
        if (s_cell2_broken && s_cell2_blink_on) {
            // celica2 → zgornja cona, CELL_NIGHT_LED_COUNT LED pri vrhu
            for (int i = 0; i < CELL_NIGHT_LED_COUNT; i++) {
                buf_set(SIG_TOP_END - i,
                        color_dim(CLR_CELL2_NIGHT, CELL_NIGHT_BRIGHTNESS));
            }
        }

    } else {
        // -------------------------------------------------------
        // Dnevni mode: celotni tretjini utripata
        // fade-in/fade-out (emulirano z blink on/off)
        // -------------------------------------------------------

        if (s_cell1_broken) {
            // celica1 (zunanja) → spodnja tretjina (BOT: 0–47) rdeče/oranžna
            CRGB c = s_cell1_blink_on ? CLR_CELL1_DAY : CRGB::Black;
            buf_fill_zone(SIG_BOT_START, SIG_BOT_END, c);
        }

        if (s_cell2_broken) {
            // celica2 (notranja) → zgornja tretjina (TOP: 96–143) modra/rumena
            CRGB c = s_cell2_blink_on ? CLR_CELL2_DAY : CRGB::Black;
            buf_fill_zone(SIG_TOP_START, SIG_TOP_END, c);
        }

        // Srednja tretjina ostane temna (ozadje)
    }
}

// ============================================================
// ANIMACIJA — ANALOGNA URA (P4)
// ============================================================
//
// Topologija (144 LED):
//   7 ločilnih pik na pozicijah CLOCK_SEP_POS[]  → dimmed siva
//   6 segmentov po 22 LED → CLOCK_SEG_START[]    → rumeno-bela (ure)
//   5 padding LED (139–143)                       → 5% dimmed
//
// Ure: ob X:00 sta prižgana prva X segmentov v 6-urni coni.
//   Cona 1 (00–05h): segmenti 1–6 = ure 0–5
//   Cona 2 (06–11h): segmenti 1–6 = ure 6–11
//   Cona 3 (12–17h): segmenti 1–6 = ure 12–17
//   Cona 4 (18–23h): segmenti 1–6 = ure 18–23
//
// Minute: 2-LED zeleni marker ki se premika znotraj 6. segmenta.
//   Segment 6 (LED 116–137) = 22 LED = 60 minut.
//   Marker pos = min_pos × (22/60) znotraj seg6.

static void tick_clock() {
    uint32_t now = millis();

    // Preveri iztekanje prikazovalnega časa
    if (now >= s_clock_end_ms) {
        SIGD("CLOCK: prikaz iztekel");
        s_clock_active = false;
        buf_clear();
        return;
    }

    buf_clear();

    // Preberem sistemski čas
    struct tm ti;
    time_t t = time(nullptr);
    localtime_r(&t, &ti);

    int hour   = ti.tm_hour;    // 0–23
    int minute = ti.tm_min;     // 0–59
    int zone   = hour / 6;      // 0–3 (katera 6-urna cona)
    int hour_in_zone = hour % 6;  // 0–5 (ura znotraj cone)

    // --- Ločilne pike (vedno dimmed siva) ---
    for (int s = 0; s < CLOCK_SEP_COUNT; s++) {
        buf_set(CLOCK_SEP_POS[s], CLR_SEP_DOT);
    }

    // --- Padding (vedno 5% dimmed) ---
    for (int i = CLOCK_PADDING_START; i < SIG_TOTAL; i++) {
        buf_set(i, CLR_PADDING);
    }

    // --- Segmenti za ure (rumeno-bela, en segment = 1 ura) ---
    // Prižgamo segmente 0 do (hour_in_zone - 1).
    // Segment 5 (index 5) je rezerviran za minute marker — nikoli se ne prižge za ure.
    for (int seg = 0; seg < 5 && seg < hour_in_zone; seg++) {
        buf_fill_zone(CLOCK_SEG_START[seg],
                      CLOCK_SEG_START[seg] + CLOCK_SEG_LEN - 1,
                      CLR_HOUR_SEG);
    }

    // --- Minute marker (zeleni 2-LED, v segmentu 5) ---
    // Marker se premika od začetka seg5 (minuta 0) do konca seg5 (minuta 59)
    // seg5 = LED 116–137 (22 LED), marker dolžina 2 LED
    float min_rel = (float)minute / 59.0f;  // 0.0–1.0
    int marker_start = CLOCK_SEG_START[5] +
                       (int)(min_rel * (float)(CLOCK_SEG_LEN - CLOCK_MIN_MARKER_LEN));
    int marker_end = marker_start + CLOCK_MIN_MARKER_LEN - 1;

    buf_fill_zone(marker_start, marker_end, CLR_MIN_MARKER);

    // Debug log (samo enkrat na minuto)
    static int s_last_logged_min = -1;
    if (minute != s_last_logged_min) {
        s_last_logged_min = minute;
        SIGD("CLOCK: cona=%d ura=%02d:%02d seg_lit=%d marker@%d",
             zone, hour, minute, hour_in_zone, marker_start);
    }
}

// ============================================================
// INICIALIZACIJA
// ============================================================

bool signal_led_init(CRGB* buf, uint16_t count) {
    SIGI("=== signal_led_init ===");

    if (!buf) {
        SIGE("signal_led_init: nullptr buf — inicializacija neuspešna!");
        return false;
    }
    if (count != LED_SIGNAL_COUNT) {
        SIGW("signal_led_init: count=%d != LED_SIGNAL_COUNT=%d — nadaljujem z LED_SIGNAL_COUNT",
             count, LED_SIGNAL_COUNT);
    }

    s_buf       = buf;
    s_buf_count = LED_SIGNAL_COUNT;

    // Počisti buffer ob zagonu
    buf_clear();

    // Resetiraj vse stanje
    s_current_mode    = SignalMode::SIG_IDLE;
    s_ramp_active     = false;
    s_ramp_stopping   = false;
    s_ramp_phase      = RampPhase::RAMP_PULSE1;
    s_parking_active  = false;
    s_parking_stopping = false;
    s_park_phase      = ParkPhase::PARK_DONE;
    s_cell_active     = false;
    s_clock_active    = false;
    memset(&s_stats, 0, sizeof(s_stats));

    s_initialized = true;
    SIGI("signal_led_init OK — buf=%p, count=%d", (void*)buf, LED_SIGNAL_COUNT);
    return true;
}

// ============================================================
// TICK — kliči iz ledTask
// ============================================================

void signal_led_tick() {
    if (!s_initialized) return;

    s_stats.tick_count++;

    // 1. Posodobi dispečer (določi aktiven način)
    dispatch_update();

    // 2. Izvedi animacijski frame za trenutni način
    switch (s_current_mode) {

        case SignalMode::SIG_RAMP:
            tick_ramp();
            break;

        case SignalMode::SIG_PARKING:
            tick_parking();
            break;

        case SignalMode::SIG_PHOTOCELL:
            tick_photocell();
            break;

        case SignalMode::SIG_CLOCK:
            tick_clock();
            break;

        case SignalMode::SIG_IDLE:
        default:
            // Buffer je že počiščen ob prehodu v IDLE (v dispatch_update)
            break;
    }
}

// ============================================================
// JAVNI API — implementacije
// ============================================================

void signal_led_ramp_start(RampDir dir) {
    if (!s_initialized) return;

    // Prepreči dvojni start (rampaluc bounce)
    if (s_ramp_active && !s_ramp_stopping) {
        SIGW("RAMP: signal_led_ramp_start() ignoriran — že aktiven");
        return;
    }

    s_ramp_dir            = dir;
    s_ramp_stopping       = false;
    s_ramp_phase          = RampPhase::RAMP_PULSE1;
    s_ramp_phase_start_ms = millis();
    s_comet_pos           = comet_start_pos();
    s_comet_speed         = (float)COMET_SPEED_NORMAL;
    s_comet_pass_count    = 0;
    s_defect_active       = false;
    s_ramp_active         = true;

    s_stats.ramp_activations++;
    SIGI("RAMP: START (dir=%s) — uvodni pulzi",
         dir == RampDir::UP ? "GOR↑" : "DOL↓");
}

void signal_led_ramp_stop() {
    if (!s_initialized) return;
    if (!s_ramp_active) {
        SIGW("RAMP: signal_led_ramp_stop() ignoriran — ni aktiven");
        return;
    }

    SIGI("RAMP: STOP zahteva — prehod v upočasnitev/eksplozijo");
    s_ramp_stopping = true;

    // Če smo še v fazi pulzov (comet se še ni začel), takoj sproži konec
    if (s_ramp_phase == RampPhase::RAMP_PULSE1 ||
        s_ramp_phase == RampPhase::RAMP_PAUSE1 ||
        s_ramp_phase == RampPhase::RAMP_PULSE2) {
        SIGD("RAMP: stop med uvodni pulzi → direktno v SLOW");
        s_ramp_phase          = RampPhase::RAMP_SLOW;
        s_ramp_phase_start_ms = millis();
        s_comet_pos           = (float)(SIG_TOTAL / 2);  // začni iz sredine
        s_comet_speed         = (float)COMET_SPEED_SLOW;
    }
    // Sicer: comet se upočasni ob koncu naslednjega prehoda (ramp_tick_comet)
}

void signal_led_parking_update(uint32_t dist_mm) {
    if (!s_initialized) return;

    s_parking_dist_mm  = dist_mm;
    s_parking_stopping = false;

    if (!s_parking_active) {
        SIGI("PARK: START (dist=%lu mm)", (unsigned long)dist_mm);
        s_parking_active = true;
        s_park_phase     = ParkPhase::PARK_ACTIVE;
        s_park_phase_ms  = millis();
        s_park_blink_ms  = millis();
        s_park_blink_on  = true;
        s_park_wave_pos  = 0.0f;
    } else {
        SIGD("PARK: posodobitev dist=%lu mm", (unsigned long)dist_mm);
    }
}

void signal_led_parking_stop() {
    if (!s_initialized || !s_parking_active) return;
    SIGI("PARK: STOP zahteva — zelena wave + fadeout");
    s_parking_stopping = true;
}

void signal_led_photocell_update(bool celica1, bool celica2, bool is_night) {
    if (!s_initialized) return;

    if (!celica1 && !celica2) {
        // Obe celici OK → ustavi
        signal_led_photocell_stop();
        return;
    }

    bool was_active = s_cell_active;
    s_cell1_broken  = celica1;
    s_cell2_broken  = celica2;
    s_cell_is_night = is_night;

    if (!was_active) {
        // Nova aktivacija — ponastavi timer
        s_cell_start_ms = millis();
        s_cell_active   = true;
        SIGI("CELL: aktiviran (cel1=%d cel2=%d noč=%d) — timer 5min",
             celica1, celica2, is_night);
    } else {
        // Obstoječa aktivacija — ponastavi timer
        s_cell_start_ms = millis();
        SIGD("CELL: posodobitev (cel1=%d cel2=%d) — timer ponastavljen",
             celica1, celica2);
    }
}

void signal_led_photocell_stop() {
    if (!s_initialized || !s_cell_active) return;
    SIGI("CELL: izklop (celici normalni)");
    s_cell_active  = false;
    s_cell1_broken = false;
    s_cell2_broken = false;
    buf_clear();
}

void signal_led_clock_show() {
    if (!s_initialized) return;

    uint32_t duration_ms = SIG_CLOCK_DURATION_MS;  // config.h: 10000ms

    s_clock_end_ms = millis() + duration_ms;
    s_clock_active = true;

    s_stats.clock_shows++;
    SIGD("CLOCK: prikaži (trajanje=%lu ms)", (unsigned long)duration_ms);
}

void signal_led_clock_stop() {
    if (!s_initialized || !s_clock_active) return;
    SIGD("CLOCK: predčasni izklop");
    s_clock_active = false;
    buf_clear();
}

void signal_led_off() {
    if (!s_initialized) return;
    SIGI("SIGNAL_OFF: takojšen izklop vsega");
    s_ramp_active     = false;
    s_ramp_stopping   = false;
    s_parking_active  = false;
    s_parking_stopping = false;
    s_cell_active     = false;
    s_clock_active    = false;
    s_current_mode    = SignalMode::SIG_IDLE;
    s_ramp_phase      = RampPhase::RAMP_PULSE1;  // reset za naslednji start
    s_park_phase      = ParkPhase::PARK_DONE;
    buf_clear();
}

// ============================================================
// DIAGNOSTIKA
// ============================================================

SignalLedStats signal_led_get_stats() {
    SignalLedStats st = s_stats;
    st.current_mode = s_current_mode;
    return st;
}

const char* signal_led_mode_name(SignalMode mode) {
    switch (mode) {
        case SignalMode::SIG_IDLE:      return "IDLE";
        case SignalMode::SIG_CLOCK:     return "CLOCK(P4)";
        case SignalMode::SIG_PHOTOCELL: return "CELL(P3)";
        case SignalMode::SIG_PARKING:   return "PARK(P2)";
        case SignalMode::SIG_RAMP:      return "RAMP(P1)";
        default:                        return "???";
    }
}
