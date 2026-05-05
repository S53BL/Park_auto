// ============================================================
// hal_gpio.h — MCP23017 GPIO Expander HAL
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ODGOVORNOST:
//   HAL za MCP23017 (0x20) na Wire1 (IO17/IO18).
//   Upravljanje 5 digitalnih vhodov (rampa, vrata, fotocelici)
//   in 4 digitalnih izhodov (SSR1–SSR4).
//
// HARDWARE:
//   I2C naslov : 0x20  (A0=A1=A2=GND)
//   SDA        : IO17  (Wire1)
//   SCL        : IO18  (Wire1)
//   INTA       : IO47  (ESP32, INPUT_PULLUP)
//   RESET pin  : 3.3V  (ne sme lebdeti!)
//
// PORT A — vhodi (GPA0–4) in izhodi (GPA5–7):
//   GPA0  rampagor   IN   INPUT_PULLUP · LOW = rampa dvignjena
//   GPA1  rampaluc   IN   INPUT_PULLUP · LOW = opozorilna luč (utripa ~1 Hz)
//   GPA2  vrataod    IN   INPUT_PULLUP · LOW = drsna vrata odprta
//   GPA3  celica1    IN   INPUT_PULLUP · LOW = zunanja fotocelica prekinjena
//   GPA4  celica2    IN   INPUT_PULLUP · LOW = notranja fotocelica prekinjena
//   GPA5  SSR1       OUT  HIGH = vklop 12V trafota (LED matrika)
//   GPA6  SSR2       OUT  HIGH = vklop 3× LED panel
//   GPA7  SSR3       OUT  HIGH = vklop reflektorja pred garažo
//
// PORT B — izhodi (GPB0) in rezerva (GPB1–7):
//   GPB0  SSR4       OUT  HIGH = vklop reflektorja pred lopo
//   GPB1–7          rezerva
//
// INTERRUPT ARHITEKTURA:
//   MCP23017 INTA → IO47 (FALLING edge ISR).
//   ISR SAMO pošlje uint8_t v s_gpio_queue (xQueueSendFromISR) — I2C prepovedano v ISR!
//   hal_gpio_process_queue() bere queue iz EventBus taska (varni I2C kontekst).
//
//   VRSTNI RED BRANJA (Microchip DS20001952C §3.7) — KRITIČEN:
//   1. Beri INTFA (0x0E) NAJPREJ — ugotovi kateri pin je sprožil.
//      INTFA je veljavno samo DOKLER INTCAP ni prebran!
//   2. Beri INTCAPA (0x10) — snapshot vrednosti ob interrupu. POČISTI INT signal in INTFA.
//   3. Beri INTCAPB (0x11) — za completeness.
//   ⚠ NAPAKA: INTCAP pred INTFA → INTFA vedno 0x00 (INT se počisti pred branjem INTFA).
//
// RAMPALUC — UTRIPAJOČI SIGNAL:
//   Fizični signal utripa ~1 Hz (opozorilna luč na rampi).
//   Logični signal "rampaluc_active" je retriggerable timeout 2000ms:
//     · vsak LOW interrupt od GPA1 resetira timer na 2000ms
//     · dokler timer teče → rampaluc_active = true
//     · ob izteku → EventBus RAMP_MOVING(false) + rampaluc_active = false
//   EventBus dobi samo 2 eventi: RAMP_MOVING(true) ob začetku in RAMP_MOVING(false) ob koncu.
//
// DEBOUNCE:
//   rampagor  : 50 ms   — mehansko stikalo, minimalen šum
//   rampaluc  : retriggerable 2000 ms timeout (glej zgoraj)
//   vrataod   : 2000 ms — drsna vrata, dolg prehod
//   celica1/2 : 4000 ms — fotocelici, daljši debounce za lažne sprožitve
//
// HEALTH-CHECK:
//   Vsakih GPIO_WATCHDOG_INTERVAL_MS (10 min) se prebere GPIOA/B in logira
//   dejansko stanje vseh pinov. Isto kot TOF watchdog — za diagnostiko in
//   zaznavo okvare brez čakanja na naslednji interrupt.
//
// SSR KRMILJENJE:
//   hal_gpio_set_ssr(ssr_idx, on) piše v OLATA/OLATB pod Wire1 mutex.
//   Notranje stanje ssr_state je shadow register — ne beremo MCP nazaj.
//   Indeksi SSR: 1=SSR1(GPA5), 2=SSR2(GPA6), 3=SSR3(GPA7), 4=SSR4(GPB0)
//
// INTEGRACIJA Z EVENT_BUS.CPP:
//   eventBusTask mora:
//     1. Klicati hal_gpio_init() po EventBus::init()
//     2. Klicati EventBus::processGpioQueue() → hal_gpio_process_queue() v zanki
//   event_bus.h mora imeti EventType: RAMP_UP, RAMP_MOVING, DOOR_OPENED,
//   CELL_BROKEN — ti že obstajajo v event_bus.h v2.0.0-dev.
//
// LOGGING:
//   INFO : fazni prehodi vhodov, SSR spremembe, init, health-check
//   WARN : I2C napake, debounce overflow, wire1 mutex timeout
//   ERROR: init napaka (MCP ne odgovori), kritična I2C okvara
//   DEBUG: raw interrupt info, per-pin stanje ob health-checku
//
// ============================================================
#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

// ============================================================
// KONSTANTE — debounce in watchdog
// ============================================================

// Debounce časi [ms]
#define GPIO_DEBOUNCE_RAMPAGOR_MS   50
#define GPIO_DEBOUNCE_VRATAOD_MS    2000
#define GPIO_DEBOUNCE_CELICA_MS     4000

// rampaluc retriggerable timeout [ms]
// Signal utripa ~1 Hz → timeout mora biti >> perioda utripanja
#define GPIO_RAMPALUC_TIMEOUT_MS    2000

// Health-check interval [ms] — 10 minut, enako kot TOF watchdog
#define GPIO_WATCHDOG_INTERVAL_MS   600000

// Wire1 mutex timeout [ms]
#define GPIO_WIRE1_MUTEX_TIMEOUT_MS 200

// FreeRTOS queue velikost (interrupt signali)
#define GPIO_ISR_QUEUE_SIZE         8

// ============================================================
// SSR INDEKSI — za hal_gpio_set_ssr()
// ============================================================
// Numerični indeksi za lažjo uporabo v light_logic.cpp
#define SSR_IDX_1   1   // GPA5 — 12V trafo (LED matrika)
#define SSR_IDX_2   2   // GPA6 — 3× LED panel
#define SSR_IDX_3   3   // GPA7 — reflektor pred garažo
#define SSR_IDX_4   4   // GPB0 — reflektor pred lopo

// ============================================================
// STRUKTURA — trenutno stanje GPIO
// ============================================================
// Snapshot vseh vhodov in izhodov. Dostopen prek hal_gpio_get_state().
// Stanje vhodov je LOGIČNO (true = aktiven signal, ne glede na polarnost):
//   rampagor  true = rampa dvignjena (fizično LOW na GPA0)
//   rampaluc  true = rampa se premika — KOLAPIRANI signal (ne surovo utripanje)
//   vrataod   true = vrata odprta (fizično LOW na GPA2)
//   celica1   true = zunanja fotocelica prekinjena (fizično LOW na GPA3)
//   celica2   true = notranja fotocelica prekinjena (fizično LOW na GPA4)
//
// Stanje izhodov (true = SSR vklopljen):
//   ssr[1..4] indeksiran z SSR_IDX_1..4

struct GpioState {
    // Vhodi — logično stanje (po debounceu)
    bool rampagor;
    bool rampaluc;      // kolapsirani signal (true = rampa se premika)
    bool vrataod;
    bool celica1;       // zunanja fotocelica
    bool celica2;       // notranja fotocelica

    // Izhodi — shadow register (true = SSR vklopljen)
    bool ssr[5];        // indeks 1–4, indeks 0 neuporabljen

    // Timestamp zadnje posodobitve
    uint32_t last_update_ms;
};

// ============================================================
// DIAGNOSTIČNA STRUKTURA
// ============================================================
struct GpioDiagnostics {
    bool     mcp_ok;                // MCP23017 je dosegljiv
    bool     initialized;           // hal_gpio_init() uspešen
    uint32_t interrupt_count;       // skupno število prejetih interruptov
    uint32_t debounce_reject_count; // koliko interruptov je debounce zavrgel
    uint32_t i2c_error_count;       // skupno število I2C napak
    uint32_t last_watchdog_ms;      // čas zadnjega health-check
    uint8_t  raw_gpioa;             // zadnja raw vrednost GPIOA registra
    uint8_t  raw_gpiob;             // zadnja raw vrednost GPIOB registra
    GpioState state;                // kopija trenutnega logičnega stanja
};

// ============================================================
// JAVNI API
// ============================================================

// Inicializacija — kliče eventBusTask po EventBus::init().
// Predpogoj: Wire1 inicializiran (bsp_wire1_ok() == true).
// Nastavi MCP23017 registre, prebere začetno stanje, priklopi ISR.
// Vrne true ob uspehu, false ob napaki (MCP ne odgovori, Wire1 ni ready).
bool hal_gpio_init();

// Status — ali je inicializacija uspela?
bool hal_gpio_ok();

// Procesiranje ISR queue — kliči iz EventBus taska (ne iz ISR!).
// Prebere interrupt signale iz queue in obdela: I2C branje INTCAP/INTFA,
// debounce logika, rampaluc timeout, EventBus dispatch.
// Non-blocking — vrne takoj če ni čakajočih interruptov.
void hal_gpio_process_queue();

// Periodični klic za rampaluc timeout in health-check timer.
// Kliči iz EventBus taska v zanki (npr. vsakih 50 ms).
// Ni thread-safe z ISR — kliči samo iz task konteksta.
void hal_gpio_tick();

// Nastavi SSR izhod.
// ssr_idx: SSR_IDX_1..SSR_IDX_4
// on: true = SSR vklopljen (HIGH na MCP pinu)
// Thread-safe — vzame Wire1 mutex.
// Logira spremembo na INFO nivoju.
bool hal_gpio_set_ssr(uint8_t ssr_idx, bool on);

// Vrne kopijo trenutnega logičnega stanja vseh vhodov in izhodov.
// Thread-safe — kopija pod kratkim kritičnim odsekon (nolock, atomski struct copy).
GpioState hal_gpio_get_state();

// Vrne diagnostične podatke za web UI in service screen.
GpioDiagnostics hal_gpio_get_diagnostics();

// Wymušen health-check — prebere stanje in ga logira takoj (ne čaka na timer).
// Kliči ročno iz service screena ali ob I2C recovery.
void hal_gpio_force_healthcheck();

// Vrne skupno število Wire1 mutex timeout napak od zagona.
// Kliče se iz light_logic diagnostike za monitoring Wire1 zdravja.
// Thread-safe: uint32_t read je atomaren na ESP32.
uint32_t hal_gpio_get_wire1_errors();
