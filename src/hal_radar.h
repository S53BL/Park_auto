// ============================================================
// hal_radar.h — SC16IS752 IRQ-driven LD2410C Radar HAL
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-04
// Vir     : NXP SC16IS752/SC16IS762 Datasheet Rev. 9.1 (5 Feb 2025)
//           https://www.nxp.com/docs/en/data-sheet/SC16IS752_SC16IS762.pdf
// ============================================================
//
// ARHITEKTURA — IRQ driven (ne polling):
//
//   LD2410C pošlje frame (~100ms interval)
//     → SC16IS752 FIFO se napolni nad trigger level
//     → SC16IS752 spusti IRQ pin LOW (open-drain, active LOW)
//     → ESP32 GPIO ISR sproži (IRAM_ATTR, <1μs)
//         → xQueueSendFromISR(s_irq_queue, &chip_id, ...)
//         → ISR se vrne — I2C se NE dotakne v ISR!
//     → radarTask se zbudi iz xQueueReceive()
//         → Wire1 mutex pridobi
//         → bere IIR za vsak kanal (kateri kanal je sprožil)
//         → bere RXLVL (koliko bajtov je v FIFO)
//         → bere RHR (podatki iz FIFO)
//         → Wire1 mutex sprosti
//         → parse LD2410C frame
//         → EventBus publish RADAR_DATA event
//
// ZAKAJ IRQ IN NE POLLING:
//   - Polling vsake 20ms = 8 I2C transakcij/20ms = 400 transakcij/s NEPRESTANO
//   - IRQ = I2C transakcije SAMO ko pridejo podatki (~10x/s na kanal)
//   - Med fazo parkiranja (PHASE_2): VehicleRecog ignorira RADAR_DATA event
//     v svojem callbacku — I2C bus ostane dostopen za TOF senzorje
//   - Med mirovanjem: radarTask spi, CPU obremenitev praktično 0
//
// FIZIČNE POVEZAVE:
//   SC16IS752 #1 @ 0x48  (A0=VCC, A1=GND)  — datasheet Table 32
//     UART-A → LD2410C #1 [Vhod]
//     UART-B → LD2410C #2 [Cesta_L]
//     IRQ    → IO41 (INPUT_PULLUP, FALLING edge ISR)
//
//   SC16IS752 #2 @ 0x4C  (A0=VCC, A1=VCC)  — datasheet Table 32
//     UART-A → LD2410C #3 [Cesta_D]
//     UART-B → LD2410C #4 [Garaza]
//     IRQ    → IO42 (INPUT_PULLUP, FALLING edge ISR)
//
//   Wire1: SDA=IO17, SCL=IO18, 100kHz
//   LD2410C: 115200 baud, 8N1
//   SC16IS752 XTAL: 1.8432 MHz
//     → Divisor = 1.8432MHz / (16 × 115200) = 1 — točno, 0% napaka
//     → Potrjeno v datasheet Table 7 (115200 baud ni v tabeli ker presega
//        56000 max v tabeli, ampak formula velja: div=1 pri XTAL=1.8432MHz)
//     → Formula: divisor = XTAL_FREQ / (prescaler × 16 × baud_rate)
//        = 1843200 / (1 × 16 × 115200) = 1.000
//
// LD2410C FRAME FORMAT (basic reporting mode, potrjeno iz hardware testa):
//   Header : F4 F3 F2 F1  (4 bajti)
//   Length : 2 bajta LE uint16 (tipično 0x0D = 13)
//   Data[0]: 0x02 = basic reporting type
//   Data[1]: 0xAA = head marker
//   Data[2]: detection — 0=nič, 1=premik, 2=statično, 3=oboje
//   Data[3-4]: razdalja premikajočega cilja [cm, LE uint16]
//   Data[5]:   energija premikajočega cilja [0-100]
//   Data[6-7]: razdalja statičnega cilja [cm, LE uint16]
//   Data[8]:   energija statičnega cilja [0-100]
//   Data[9-10]:razdalja zaznave [cm, LE uint16]
//   Data[11]:  0x55 = tail marker
//   Data[12]:  0x00 = check byte
//   Footer : F8 F7 F6 F5  (4 bajti)
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// PODATKOVNE STRUKTURE — javne
// ============================================================

// ID senzorja — ustreza LD2410C senzorju
typedef enum : uint8_t {
    RADAR_SENSOR_VHOD    = 0,   // SC16IS752 #1, UART-A
    RADAR_SENSOR_CESTA_L = 1,   // SC16IS752 #1, UART-B
    RADAR_SENSOR_CESTA_D = 2,   // SC16IS752 #2, UART-A
    RADAR_SENSOR_GARAZA  = 3,   // SC16IS752 #2, UART-B
    RADAR_SENSOR_COUNT   = 4
} RadarSensorId;

// Parsiran LD2410C frame — basic reporting mode
typedef struct {
    RadarSensorId sensor_id;
    uint8_t  detection;         // 0=nič, 1=premik, 2=statično, 3=oboje
    uint16_t moving_dist_cm;    // razdalja premikajočega cilja [cm]
    uint8_t  moving_energy;     // energija premikajočega cilja [0-100]
    uint16_t static_dist_cm;    // razdalja statičnega cilja [cm]
    uint8_t  static_energy;     // energija statičnega cilja [0-100]
    uint16_t detect_dist_cm;    // razdalja zaznave [cm]
    uint32_t timestamp_ms;      // millis() ob prejemu frame-a
} RadarFrame;

// Status enega senzorja
typedef struct {
    bool     active;            // ali je kanal inicializiran in aktiven
    uint32_t frames_ok;         // veljavni frame-i od zagona
    uint32_t frames_err;        // neveljavi frame-i
    uint32_t parse_errors;      // napake parserja
    uint32_t boot_parse_errors; // parse_err nastale med init/boot (odštejemo pri prikazu)
    uint32_t i2c_errors;        // napake I2C transakcij
    uint32_t irq_count;         // skupaj IRQ sprožitev za ta čip
    uint32_t last_frame_ms;     // millis() zadnjega veljavnega frame-a
    RadarFrame last_frame;      // zadnji veljaven frame
} RadarSensorStatus;

// ============================================================
// CALLBACK TIP
// ============================================================

// Callback ki ga pokliče radarTask ko pride veljaven frame.
// Kliče se v kontekstu radarTask (Core1, ne ISR).
// Implementirano v sensor_mgr.cpp → EventBus publish.
typedef void (*RadarFrameCallback)(const RadarFrame& frame);

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

// Inicializacija — kliči iz sensor_mgr_init().
// Postavi Wire1 mutex, ISR za IO41 in IO42, FreeRTOS queue in task.
// Vrne true če sta oba SC16IS752 odgovorila na I2C ping.
// Vrne false če nobeden ne odgovori (Wire1 problem).
// Delna inicializacija (samo eden od dveh) → vrne true + logira WARNING.
bool hal_radar_init(RadarFrameCallback cb);

// Zaustavi radarTask in ISR — za graceful shutdown (prihodnja razširitev).
void hal_radar_deinit();

// Status posameznega senzorja — kliči kadarkoli po init.
const RadarSensorStatus& hal_radar_get_status(RadarSensorId id);

// Preveri ali je kanal aktiven (čip prisoten + UART inicializiran).
bool hal_radar_channel_ok(RadarSensorId id);

// Zahteva software reset enega SC16IS752 čipa.
// Datasheet Section 7.4: IOControl[2] = 1 → software reset, enakovreden HW reset.
// Registri DLL, DLH, SPR, XON/XOFF se NE resetirajo (ohranijo vrednosti).
void hal_radar_reset_chip(uint8_t chip_addr);

// Dump statistike na Serial/Logger — za diagnostiko.
void hal_radar_log_stats();

// Periodični recovery check — kliči iz sensorTask vsakih 10 minut,
// takoj po hal_tof_tick() watchdog meritvi.
// Preveri IRQ pine in počisti FIFO če je pin ostal LOW.
// NE kliči iz radarTask — povzroča mutex konflikte.
void hal_radar_recovery_check();
