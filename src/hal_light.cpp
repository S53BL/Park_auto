// ============================================================
// hal_light.cpp — HAL Driver za BH1750 senzor svetlobe
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// Sloj    : Layer 2 — Hardware Abstraction Layer
// ============================================================
//
// Implementacijske odločitve:
//
//  1. POLLING INTERVAL: 30 s (LIGHT_POLL_SENSOR_MS)
//     BH1750 nima IRQ. 30 s je kompromis med odzivnostjo in
//     obremenitijo Wire1 busa. Branje traja < 5 ms.
//
//  2. DRSEČE POVPREČJE: 5 vzorcev (LIGHT_AVG_SAMPLES)
//     Krožni buffer. Okno = 5 × 30 s = 2,5 minute.
//     Dovolj za gladko zaznavanje mraka/zore, robustno
//     proti prehodnim motnjam (oblak, krat senca).
//
//  3. ASIMETRIČNA HISTEREZA:
//     NOČ < LIGHT_LUX_NIGHT (privzeto 40 lux)
//     DAN  > LIGHT_LUX_DAY  (privzeto 70 lux)
//     Razlika 30 lux preprečuje "tresanje" pri mejnih pogojih.
//     Nakaj ur ob sončnem zahodu je tipično 20–200 lux —
//     histereza zagotovi, da sistem enkrat preklopi, ne trese.
//
//  4. INICIALIZACIJSKO STANJE: is_night = false
//     Ob napaki senzorja sistem privzame DAN → luči se
//     avtomatično NE prižigajo. Varnejše kot privzeti NOČ.
//
//  5. AUTO RESET: po 3 zaporednih napakah
//     hal_light_reset() pošlje Power Off → On → Mode.
//     Ščiti pred začasnimi I2C lockup-i.
//
//  6. WIRE1 MUTEX: ni potreben
//     hal_light_tick() se kliče izključno iz sensorTask
//     (Core1). TCA9548A channel switching (hal_tof) je prav
//     tako v sensorTask — klica se ne překrivata.
//     ⚠ Ključno: hal_light_tick() se NE sme klicati med
//     TOF_PHASE_SCANNING — sensor_mgr to zagotovi.
//
//  7. BH1750 CONTINUOUS HIGH RES MODE (0x10)
//     Resolucija: 1 lx, čas meritve: ~120 ms.
//     Senzor meri neprekinjeno — ob branju vrne zadnjo vrednost.
//     Ni potrebno čakanje po ukazu za vsako branje (za razliko
//     od One Time Mode).
// ============================================================

#include "hal_light.h"
#include "config.h"
#include "event_bus.h"
#include "logger.h"
#include <Wire.h>

// Forward declaration — internal helper
static float _bh1750_force_read_internal();

#define LGI(fmt, ...) LOG_INFO ("LIGHT", fmt, ##__VA_ARGS__)
#define LGW(fmt, ...) LOG_WARN ("LIGHT", fmt, ##__VA_ARGS__)
#define LGE(fmt, ...) LOG_ERROR("LIGHT", fmt, ##__VA_ARGS__)

// ============================================================
// BH1750 I2C ukazi
// ============================================================
#define BH1750_ADDR             I2C_ADDR_BH1750  // 0x23 (ADDR=GND)
#define BH1750_CMD_POWER_OFF    0x00
#define BH1750_CMD_POWER_ON     0x01
#define BH1750_CMD_RESET        0x07
#define BH1750_CMD_CONT_H_RES   0x10  // Continuous High Resolution Mode, 1 lx

// ============================================================
// Interno stanje (file-scope — ne eksponirati navzven)
// ============================================================

// Krožni buffer za drseče povprečje
static float   s_buf[LIGHT_AVG_SAMPLES];
static uint8_t s_buf_head    = 0;        // naslednji slot za pisanje
static uint8_t s_buf_fill    = 0;        // koliko vzorcev je veljavnih (0..LIGHT_AVG_SAMPLES)

// Drseče povprečje — preračunano ob vsakem novem vzorcu
static float   s_lux_avg     = 0.0f;

// Zadnje surovo branje
static float   s_lux_raw     = 0.0f;

// Trenutno stanje dan/noč
static volatile bool s_is_night = false;

// Statistika
static uint32_t s_read_count  = 0;
static uint32_t s_error_count = 0;
static uint32_t s_last_read_ms  = 0;
static uint32_t s_last_event_ms = 0;

// Stanje init-a
static bool s_init_ok = false;

// Štetje zaporednih napak (za auto reset)
static uint8_t s_consec_errors = 0;
#define LIGHT_ERROR_RESET_THRESH  3

// ============================================================
// Interne funkcije
// ============================================================

// Pošlji en bajt ukaz na BH1750
static bool _bh1750_send_cmd(uint8_t cmd) {
    Wire1.beginTransmission(BH1750_ADDR);
    Wire1.write(cmd);
    uint8_t err = Wire1.endTransmission();
    return (err == 0);
}

// Preberi 2 bajta z BH1750 in pretvori v lux
// BH1750 formula: lux = (MSB<<8 | LSB) / 1.2
// Vrne -1.0 ob napaki
static float _bh1750_read_lux() {
    uint8_t n = Wire1.requestFrom((uint8_t)BH1750_ADDR, (uint8_t)2);
    if (n != 2) {
        return -1.0f;
    }
    uint8_t msb = Wire1.read();
    uint8_t lsb = Wire1.read();
    uint16_t raw = ((uint16_t)msb << 8) | lsb;
    return (float)raw / 1.2f;
}

// Izračunaj drseče povprečje iz krožnega bufferja
static float _calc_average() {
    if (s_buf_fill == 0) return 0.0f;
    float sum = 0.0f;
    for (uint8_t i = 0; i < s_buf_fill; i++) {
        sum += s_buf[i];
    }
    return sum / (float)s_buf_fill;
}

// Dodaj vzorec v krožni buffer in posodobi povprečje
static void _buf_push(float lux) {
    s_buf[s_buf_head] = lux;
    s_buf_head = (s_buf_head + 1) % LIGHT_AVG_SAMPLES;
    if (s_buf_fill < LIGHT_AVG_SAMPLES) {
        s_buf_fill++;
    }
    s_lux_avg = _calc_average();
}

// Preveri histerezni prehod in oddaj event ob spremembi
// Kliče se po vsakem novem vzorcu — samo ob dejanskem prehodu
// se oddaja EVT_LIGHT_THRESHOLD.
static void _check_threshold() {
    // Počakaj da je buffer vsaj delno zapolnjen (min 1 vzorec)
    // Zanesljiva ocena šele po LIGHT_AVG_SAMPLES vzorcih,
    // ampak ne čakamo — prvi vzorec je boljši kot nič.
    if (s_buf_fill == 0) return;

    bool new_is_night = s_is_night;  // privzeto: ohrani stanje

    if (s_is_night) {
        // Trenutno NOČ → preklopi v DAN samo če avg > LIGHT_LUX_DAY
        if (s_lux_avg > (float)LIGHT_LUX_DAY) {
            new_is_night = false;
        }
    } else {
        // Trenutno DAN → preklopi v NOČ samo če avg < LIGHT_LUX_NIGHT
        if (s_lux_avg < (float)LIGHT_LUX_NIGHT) {
            new_is_night = true;
        }
    }

    // Oddaj event samo ob dejanski spremembi stanja
    if (new_is_night != s_is_night) {
        s_is_night = new_is_night;
        s_last_event_ms = millis();

        // EventBus publish — NIGHT_THRESHOLD_CHANGED, payload = is_night (uint32_t)
        // Podrobnosti so dostopne prek hal_light_get_stats() za subscriberje.
        EventBus::publish(EventType::NIGHT_THRESHOLD_CHANGED, (uint32_t)s_is_night);

        LGI("PREHOD: %s | avg=%.1f lux | raw=%.1f lux | vzorci=%d/%d",
            s_is_night ? "DAN→NOČ" : "NOČ→DAN",
            s_lux_avg, s_lux_raw,
            s_buf_fill, LIGHT_AVG_SAMPLES);
    }
}

// ============================================================
// Public API — Implementacija
// ============================================================

bool hal_light_init() {
    LGI("init — BH1750 @ 0x%02X (Wire1, IO%d/IO%d)",
        BH1750_ADDR, I2C_SDA, I2C_SCL);

    // Resetiraj interno stanje
    memset(s_buf, 0, sizeof(s_buf));
    s_buf_head       = 0;
    s_buf_fill       = 0;
    s_lux_avg        = 0.0f;
    s_lux_raw        = 0.0f;
    s_is_night       = false;  // privzeto DAN — varno ob napaki
    s_read_count     = 0;
    s_error_count    = 0;
    s_last_read_ms   = 0;
    s_last_event_ms  = 0;
    s_consec_errors  = 0;
    s_init_ok        = false;

    // Power On
    if (!_bh1750_send_cmd(BH1750_CMD_POWER_ON)) {
        LGE("Power On ukaz neuspešen — senzor ne odgovori na 0x%02X", BH1750_ADDR);
        LGW("Sistem bo delal brez senzorja svetlobe (is_night=false)");
        return false;
    }

    // Postavi Continuous High Resolution Mode
    if (!_bh1750_send_cmd(BH1750_CMD_CONT_H_RES)) {
        LGE("Cont H-Res Mode ukaz neuspešen");
        return false;
    }

    // Počakaj na prvo meritev (BH1750 potrebuje ~120 ms za prvi rezultat)
    delay(150);

    // Self-test: preberi eno vrednost
    float lux = _bh1750_force_read_internal();
    if (lux < 0.0f) {
        LGE("Self-test branje neuspešno — senzor ne vrne podatkov");
        return false;
    }

    s_init_ok = true;
    LGI("init OK | self-test: %.1f lux | interval=%d s | vzorci=%d | prag NOČ=%.0f/DAN=%.0f lux",
        lux,
        LIGHT_POLL_SENSOR_MS / 1000,
        LIGHT_AVG_SAMPLES,
        (float)LIGHT_LUX_NIGHT,
        (float)LIGHT_LUX_DAY);

    return true;
}

// Interna verzija force read — brez init preverjanja (za self-test)
// Implementirana kot static helper, ni v .h
static float _bh1750_force_read_internal() {
    float lux = _bh1750_read_lux();
    return lux;
}

void hal_light_tick() {
    if (!s_init_ok) return;

    // Preberi surovo vrednost
    float lux = _bh1750_read_lux();

    if (lux < 0.0f) {
        // I2C napaka
        s_error_count++;
        s_consec_errors++;
        LGW("I2C branje napaka #%lu (zaporednih: %d)",
            (unsigned long)s_error_count, s_consec_errors);

        // Auto reset po pragu zaporednih napak
        if (s_consec_errors >= LIGHT_ERROR_RESET_THRESH) {
            LGW("Auto reset BH1750 po %d zaporednih napakah", s_consec_errors);
            hal_light_reset();
            s_consec_errors = 0;
        }
        return;  // ne piši v buffer — ohrani stare vzorce
    }

    // Uspešno branje
    s_consec_errors = 0;
    s_read_count++;
    s_last_read_ms = millis();
    s_lux_raw = lux;

    // Vpis v krožni buffer in posodobitev povprečja
    _buf_push(lux);

    // Log vsake 10. meritve (DEBUG level) — ne zapolnimo loga
    if ((s_read_count % 10) == 1) {
        LGI("tick | raw=%.1f lux | avg=%.1f lux | vzorci=%d/%d | stanje=%s",
            s_lux_raw, s_lux_avg, s_buf_fill, LIGHT_AVG_SAMPLES,
            s_is_night ? "NOC" : "DAN");
    }

    // Preveri prehod dan/noč
    _check_threshold();
}

bool hal_light_is_night() {
    return s_is_night;
}

float hal_light_get_lux() {
    if (!s_init_ok || s_buf_fill == 0) return 0.0f;
    return s_lux_avg;
}

bool hal_light_get_stats(LightStats* out) {
    if (!out) return false;
    out->lux_last_raw  = s_lux_raw;
    out->lux_avg       = s_lux_avg;
    out->is_night      = s_is_night;
    out->buf_fill      = s_buf_fill;
    out->read_count    = s_read_count;
    out->error_count   = s_error_count;
    out->last_read_ms  = s_last_read_ms;
    out->last_event_ms = s_last_event_ms;
    return s_init_ok;
}

float hal_light_force_read() {
    if (!s_init_ok) return -1.0f;
    return _bh1750_read_lux();
}

bool hal_light_reset() {
    LGW("reset BH1750...");
    bool ok = true;
    ok &= _bh1750_send_cmd(BH1750_CMD_POWER_OFF);
    delay(10);
    ok &= _bh1750_send_cmd(BH1750_CMD_POWER_ON);
    delay(10);
    ok &= _bh1750_send_cmd(BH1750_CMD_CONT_H_RES);
    delay(150);  // čakaj na prvo meritev po resetu
    if (ok) {
        LGI("reset OK");
    } else {
        LGE("reset NEUSPEŠEN — senzor morda ni priključen");
        s_init_ok = false;
    }
    return ok;
}
