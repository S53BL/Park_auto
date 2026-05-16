// ============================================================
// signal_led.h — Signalna LED veriga: prioritetni dispečer + animacije
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// NAMEN IN ODGOVORNOST:
//   Ta modul prevzame celotno upravljanje 144-LED signalne verige (IO40).
//   led_manager.cpp ostane lastnik FastLED bufferja in safe_show() —
//   signal_led.cpp samo PIŠE v buffer prek kazalca, ki ga prejme ob init.
//
//   Ločitev od led_manager.cpp je bila izvedena ker:
//     1. led_manager je infrastrukturni modul (FastLED, MUX, queue, task)
//        in ne sme rasti z aplikacijsko logiko animacij.
//     2. Signalni scenariji so kompleksni state machine-e z lastnimi
//        parametri, timerji in prioritetno logiko — zaslužijo svojo datoteko.
//     3. Berljivost in vzdrževanost: signal_led.cpp je ~400 vrstic,
//        led_manager.cpp ostane ~300 vrstic, skupaj obvladljivo.
//
// FIZIČNA TOPOLOGIJA signalne verige (IO40, 144 LED, 1m vertikalno):
//   LED   0–47  = spodnja cona (BOT) — bliže tlom, fotocelica1 stran
//   LED  48–95  = srednja cona (MID) — sredina traku, ni posebnega pomena
//   LED  96–143 = zgornja cona (TOP) — bliže stropu, fotocelica2 stran
//
//   Vertikalna montaža pomeni: LED 0 = spodaj, LED 143 = zgoraj.
//   Animacija "navzgor" = naraščajoči indeksi (0→143).
//   Animacija "navzdol" = padajoči indeksi (143→0).
//
//   Rampa se dviga = LED 0→143 (comet gre gor)
//   Rampa se spušča = LED 143→0 (comet gre dol)
//
// PRIORITETE (P1 = najvišja, P4 = najnižja):
//   P1 — SIG_RAMP       : animacija pomika rampe (rampaluc aktiven)
//   P2 — SIG_PARKING    : parking assist (Faza 2, H < 1.5m, samo mesto A)
//   P3 — SIG_PHOTOCELL  : prekinjena fotocelica (celica1 ali celica2)
//   P4 — SIG_CLOCK      : analogna ura (samo podnevi, ob gibanju, 10s)
//   --   SIG_IDLE        : trak ugasnjen (vse OFF)
//
//   Ob konfliktu: višja prioriteta VEDNO preglasi nižjo.
//   Ko višja prioriteta konča, se preveri ali čaka katera nižja.
//   Sekvenca sproščanja: P1→P2→P3→P4→IDLE.
//
// ARHITEKTURNA UMESTITEV:
//   Layer 3 — Services (enako kot led_manager)
//   signal_led_init(CRGB* buf) — prejme kazalec na led_manager buffer
//   signal_led_tick()          — kliče se IZ ledTask zanke (Core1)
//   signal_led_set_*()         — kliče se iz light_logic/sensor_mgr (thread-safe prek led_manager queue)
//
//   ⚠ signal_led_tick() piše v buffer — kliči SAMO iz ledTask,
//     ki je edini pisec FastLED bufferjev (arhitekturna invarianta).
//
//   ⚠ signal_led_set_*() funkcije so thread-safe: ne pišejo direktno
//     v buffer temveč posodobijo volatile stanje ki ga tick() bere.
//     Atomarnost je zagotovljena ker so parametri enostavni tipi
//     (bool, uint32_t) — na Xtensa LX7 so branja/pisanja 32-bit
//     vrednosti atomarna brez mutexa.
//
// ODVISNOSTI:
//   FastLED (prek kazalca na buffer — ne inicializira sam)
//   config.h (SIG_PARK_THRESH_*, SIG_CELL_TIMER_MS, SIG_CLOCK_DURATION_MS)
//   config_mgr.h (config_get() za nastavljive parametre)
//   logger.h (LOG_* makroji)
//   time.h (za analogno uro — NTP/RTC čas)
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// SIGNALNI NAČIN (prioritetni enum)
// ============================================================
// Vrednosti so namerno naraščajoče po prioriteti —
// višja vrednost = višja prioriteta.
// Dispečer vedno vzame max(aktivnih načinov).

enum class SignalMode : uint8_t {
    SIG_IDLE       = 0,   // trak ugasnjen
    SIG_CLOCK      = 1,   // P4: analogna ura (podnevi, ob gibanju)
    SIG_PHOTOCELL  = 2,   // P3: prekinjena fotocelica
    SIG_PARKING    = 3,   // P2: parking assist (Faza 2)
    SIG_RAMP       = 4,   // P1: animacija rampe (rampaluc LOW)
};

// ============================================================
// SMER RAMPE
// ============================================================

enum class RampDir : uint8_t {
    UP   = 0,   // dviganje — comet gre gor (LED 0→143)
    DOWN = 1,   // spuščanje — comet gre dol (LED 143→0)
};

// ============================================================
// DIAGNOSTIKA
// ============================================================

struct SignalLedStats {
    SignalMode current_mode;      // trenutno aktiven način
    uint32_t   mode_changes;      // skupno št. prehodov med načini
    uint32_t   ramp_activations;  // kolikokrat je bila rampa animacija aktivirana
    uint32_t   parking_updates;   // kolikokrat je bil parking assist posodobljen
    uint32_t   photocell_blinks;  // kolikokrat je fotocelica sprožila utripanje
    uint32_t   clock_shows;       // kolikokrat je bila ura prikazana
    uint32_t   tick_count;        // skupno št. tick() klicev (za debug FPS)
    uint32_t   preemptions;       // kolikokrat je višja prioriteta prekinila nižjo
};

// ============================================================
// INICIALIZACIJA
// ============================================================

// Inicializacija signal_led modula.
// Kliči IZ led_mgr_init(), potem ko je FastLED inicializiran in je
// buffer alociran v PSRAM.
//
// Parametri:
//   buf   : kazalec na s_leds_signal[] v led_manager.cpp
//   count : LED_SIGNAL_COUNT (144)
//
// Vrne true ob uspehu. Ob napaki (nullptr buf) logira ERROR in vrne false.
// Brez inicializacije signal_led_tick() ne naredi ničesar.
bool signal_led_init(CRGB* buf, uint16_t count);

// ============================================================
// TICK — kliči IZ ledTask zanke (vsakih ~20ms)
// ============================================================

// Procesira trenutni animacijski frame za signalno verigo.
// Piše direktno v buf[] ki je bil predan ob signal_led_init().
// led_manager.cpp pokliče FastLED.show() POTEM ko se ta funkcija vrne.
//
// Vrne true če je bil LED buffer dejansko spremenjen (animacija aktivna).
// Vrne false v SIG_IDLE stanju — led_manager s tem implementira dirty flag.
//
// ⚠ Kliči SAMO iz ledTask (Core1). Nikoli iz ISR, EventBus handlerjev
//   ali kateregakoli drugega taska — edina izjema bi bila če bi bil
//   signal_led tick zaščiten z muxtexom, kar pa ni in ne sme biti
//   (FastLED.show() blokira ~3ms, mutex bi povzročil priority inversion).
bool signal_led_tick();  // vrne true če je LED buffer spremenjen

// ============================================================
// JAVNI API — nastavljanje načinov
// ============================================================
// Vse te funkcije so klicane iz light_logic.cpp ali sensor_mgr.cpp
// (različni taski). Ker pišejo samo v enostavne atomarne volatile
// spremenljivke, ni mutexa.

// --- P1: Animacija rampe ---

// Aktiviraj rampa animacijo.
// dir      : UP (dviganje) ali DOWN (spuščanje)
// Kliči ko rampaluc postane aktiven (LOW na MCP GPA1).
// Animacija teče DOKLER ne kličeš signal_led_ramp_stop().
// Ob klicu med aktivno parking/photocell/clock animacijo:
//   te se takoj prekinejo (P1 preglasi vse).
void signal_led_ramp_start(RampDir dir);

// Zaključi rampa animacijo.
// Sproži "dramatičen konec": upočasnitev → eksplozija → 5s fadeout.
// Kliči ko rampaluc izgine (HIGH na MCP GPA1).
// Po zaključku animacije se način samodejno vrne na P2/P3/P4/IDLE
// glede na to kaj je čakalo.
void signal_led_ramp_stop();

// --- P2: Parking assist ---

// Posodobi parking assist z novo razdaljo.
// dist_mm : razdalja od H senzorja v milimetrih
// Kliči iz sensor_mgr ob vsaki novi TOF H meritvi MED Fazo 2.
// Aktivacija: signal_led samodejno preklopi na P2 ob prvem klicu.
// ⚠ Kliči SAMO ko je activeParking == A in faza == FASE_2.
void signal_led_parking_update(uint32_t dist_mm);

// Zaključi parking assist.
// Kliči ko Faza 2 konča ali ko vozilo stabilizira (H stabilen >3-5s).
// Sproži: kratka zelena wave (0.8s) → fadeout (0.5s) → vrni na P3/P4/IDLE.
void signal_led_parking_stop();

// --- P3: Fotocelice ---

// Aktiviraj/posodobi utripanje fotocelice.
// celica1 : true = celica1 (zunanja) prekinjena → BOT cona (LED 0–47)
// celica2 : true = celica2 (notranja) prekinjena → TOP cona (LED 96–143)
// is_night : true = nočni diskretni mode (1–3 LED, nizka svetlost)
// Vsak klic ponastavi 5-minutni timer.
// Ob klicu z obema false: enako kot signal_led_photocell_stop().
void signal_led_photocell_update(bool celica1, bool celica2, bool is_night);

// Zaustavi fotocelično utripanje.
void signal_led_photocell_stop();

// --- P4: Analogna ura ---

// Prikaži analogno uro (10s prikaz).
// Kliči ob zaznavi gibanja PODNEVI (is_night == false).
// Ob klicu ponoči: ignorira se (notranja zaščita v funkciji).
// Ob klicu med aktivno P1/P2/P3 animacijo: ura se zakaže in
//   se prikaže šele ko se te sprostijo.
void signal_led_clock_show();

// Zaustavi prikaz ure (predčasno, če je potrebno).
void signal_led_clock_stop();

// --- Splošno ---

// Takojšen izklop vseh signalnih LED in reset na IDLE.
// Kliči ob reset ali emergency stop situaciji.
void signal_led_off();

// ============================================================
// DIAGNOSTIKA
// ============================================================

SignalLedStats signal_led_get_stats();

// Vrne ime trenutnega načina kot C-string (za logiranje).
const char* signal_led_mode_name(SignalMode mode);
