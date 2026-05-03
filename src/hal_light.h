// ============================================================
// hal_light.h — HAL Driver za BH1750 senzor svetlobe
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// Sloj    : Layer 2 — Hardware Abstraction Layer
// ============================================================
//
// Odgovornost tega modula:
//   - Inicializacija BH1750 prek Wire1 (I2C naslov 0x23, ADDR=GND)
//   - Branje surove vrednosti in pretvorba v lux
//   - Vzdrževanje drsečega povprečja (krožni buffer)
//   - Zaznavanje prehoda dan ↔ noč z asimetrično histerezno logiko
//   - Oddajanje LIGHT_THRESHOLD eventa prek EventBus ob spremembi stanja
//
// Odvisnosti:
//   - Wire1   — I2C bus (IO17/IO18), inicializiran v bsp.cpp
//   - config.h — LIGHT_* konstante (interval, vzorci, pragi)
//   - event_bus.h — EVT_LIGHT_THRESHOLD
//   - logger.h  — LOG_INFO / LOG_WARN / LOG_ERROR
//
// Mehanizem polling-a:
//   Branje se NE izvaja v ločenem tasku. sensor_mgr::sensorTask()
//   kliče hal_light_tick() vsakih LIGHT_POLL_SENSOR_MS (privzeto 30 s).
//   hal_light_tick() ne blokira — I2C branje traja < 5 ms.
//
// Histereza (zaščita pred tresanjem na meji):
//   Preklop v NOČ : avg_lux < LIGHT_LUX_NIGHT   (privzeto 40 lux)
//   Preklop v DAN : avg_lux > LIGHT_LUX_DAY     (privzeto 70 lux)
//   Oba praga sta neodvisno nastavljiva prek ConfigManager.
//
// Thread safety:
//   Vse funkcije se kličejo izključno iz sensorTask (Core1).
//   Nobenih mutexov ni potrebnih — ni souporabe med taski.
//
// BH1750 I2C protokol:
//   Continuous High Resolution Mode (0x10): resolucija 1 lx, čas meritve ~120 ms
//   Preberi 2 bajta (MSB, LSB) → lux = (MSB<<8 | LSB) / 1.2
//   Reset: pošlji 0x07 (Power Off) → 0x01 (Power On) → 0x10 (Mode)
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Tipi in strukture
// ============================================================

// Stanje svetlobe (noč/dan) — oddano prek EventBus ob spremembi
typedef struct {
    bool    is_night;       // true = noč (under prag), false = dan
    float   lux_avg;        // povprečna vrednost lux ob prehodu
    float   lux_raw;        // zadnja surova vrednost lux
    uint8_t samples_valid;  // koliko vzorcev v bufferju je veljavnih
} LightThresholdEvent;

// Interna statistika — za diagnostiko in /api/status
typedef struct {
    float   lux_last_raw;   // zadnja surova meritev
    float   lux_avg;        // trenutno drseče povprečje
    bool    is_night;       // trenutno stanje
    uint8_t buf_fill;       // koliko vzorcev v bufferju (0–LIGHT_AVG_SAMPLES)
    uint32_t read_count;    // skupno število uspešnih branj
    uint32_t error_count;   // skupno število I2C napak
    uint32_t last_read_ms;  // millis() zadnjega uspešnega branja
    uint32_t last_event_ms; // millis() zadnjega oddanega eventa (0 = nikoli)
} LightStats;

// ============================================================
// Public API
// ============================================================

// hal_light_init()
// Inicializira BH1750 na Wire1. Kliče se iz sensor_mgr_init().
// Pošlje Power On + Continuous High Res Mode ukaz.
// Izvede en self-test read — če senzor ne odgovori, vrne false.
// Pri false sistem teče naprej, osvetlitev ostane v privzetem
// stanju (is_night = false), da ne bi po nepotrebnem prižigala
// luči podnevi ob napaki senzorja.
// Vrne: true = init OK, false = senzor ne odgovori ali I2C napaka
bool hal_light_init();

// hal_light_tick()
// Kliče se iz sensorTask() vsakih LIGHT_POLL_SENSOR_MS.
// Prebere surovo vrednost z BH1750, shrani v krožni buffer,
// izračuna povprečje in preveri histerezni prehod.
// Ob prehodu dan↔noč: oddaja EVT_LIGHT_THRESHOLD prek EventBus.
// Ob I2C napaki: inkrementira error_count, brez crasha.
// Samodejno poskusi reset senzorja po 3 zaporednih napakah.
void hal_light_tick();

// hal_light_is_night()
// Vrne trenutno stanje dan/noč brez branja senzorja.
// Varno za klic iz kateregakoli konteksta (bere volatile bool).
bool hal_light_is_night();

// hal_light_get_lux()
// Vrne zadnje drseče povprečje lux.
// Vrne 0.0 če init ni bil uspešen ali buffer še ni poln.
float hal_light_get_lux();

// hal_light_get_stats()
// Zapolni LightStats strukturo — za /api/status in diagnostiko.
// Vrne false če hal_light_init() ni bil uspešen.
bool hal_light_get_stats(LightStats* out);

// hal_light_force_read()
// Enkratno sinhrono branje brez vpisa v buffer in brez event.
// Namen: web /api/status prikaz sveže vrednosti na zahtevo.
// Vrne: lux vrednost, ali -1.0 ob napaki.
float hal_light_force_read();

// hal_light_reset()
// Pošlje Power Off → Power On → Mode ukaz senzorju.
// Kliče se samodejno po 3 napakah ali ročno iz diagnostike.
// Vrne: true = reset uspel
bool hal_light_reset();
