// ============================================================
// hal_radar.h — SC16IS752 Periodical-Polling LD2410C Radar HAL
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0  |  Datum: 2026-05
// ============================================================
//
// SPREMEMBA v2.0: IRQ-pin polling → čisti periodični polling
//
//   V v1.x je radarTask izvajal:
//     while(digitalRead(IO41)==LOW) { process_chip_irq(0); }
//     while(digitalRead(IO42)==LOW) { process_chip_irq(1); }
//
//   V v2.0 radarTask izvaja:
//     vsakih radar_poll_interval_ms:
//       Wire1_mutex → poll_one_channel(×4) → Wire1_mutex sprosti
//
//   Prednosti:
//   - Ni drain zanke, ni max_loops, ni round-robin kompleksnosti
//   - Wire1 mutex enkrat za vse 4 kanale (~8ms max blokada vs ~96ms)
//   - TOF ima prednost: mutex timeout = poll_interval/2
//   - Overflow je normalen pojav — flush + continue, ne panika
//   - Minutni log s % uspešnosti per senzor za diagnostiko
//
// FIZIČNE POVEZAVE (nespremenjena):
//   SC16IS752 #1 @ 0x48: UART-A→LD2410C[Vhod], UART-B→LD2410C[Cesta_L]
//   SC16IS752 #2 @ 0x4C: UART-A→LD2410C[Cesta_D], UART-B→LD2410C[Garaza]
//   Wire1: SDA=IO17, SCL=IO18, 100kHz
//   LD2410C: 115200 baud 8N1, XTAL 1.8432MHz, divisor=1
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// PODATKOVNE STRUKTURE — javne (enake v1.x)
// ============================================================

typedef enum : uint8_t {
    RADAR_SENSOR_VHOD    = 0,
    RADAR_SENSOR_CESTA_L = 1,
    RADAR_SENSOR_CESTA_D = 2,
    RADAR_SENSOR_GARAZA  = 3,
    RADAR_SENSOR_COUNT   = 4
} RadarSensorId;

typedef struct {
    RadarSensorId sensor_id;
    uint8_t  detection;         // 0=nič, 1=premik, 2=statično, 3=oboje
    uint16_t moving_dist_cm;
    uint8_t  moving_energy;
    uint16_t static_dist_cm;
    uint8_t  static_energy;
    uint16_t detect_dist_cm;
    uint32_t timestamp_ms;
} RadarFrame;

typedef struct {
    bool     active;
    uint32_t frames_ok;
    uint32_t frames_err;
    uint32_t parse_errors;
    uint32_t boot_parse_errors;
    uint32_t i2c_errors;
    uint32_t irq_count;         // ohranjen za API kompatibilnost (v polling = 0)
    uint32_t last_frame_ms;
    RadarFrame last_frame;
    uint32_t last_publish_ms;
    uint32_t oe_count;
    uint32_t last_oe_log_ms;
    bool     config_ok;
    bool     config_verified;
    uint32_t config_ms;
    uint8_t  configured_max_dist;
    uint8_t  configured_move_sens;
    uint8_t  configured_static_sens;
    uint16_t configured_unmanned_s;
} RadarSensorStatus;

typedef void (*RadarFrameCallback)(const RadarFrame& frame);

// ============================================================
// JAVNE FUNKCIJE — enake signature kot v1.x (drop-in zamenjava)
// ============================================================

bool hal_radar_init(RadarFrameCallback cb);
void hal_radar_deinit();
const RadarSensorStatus& hal_radar_get_status(RadarSensorId id);
bool hal_radar_channel_ok(RadarSensorId id);
void hal_radar_reset_chip(uint8_t chip_addr);
void hal_radar_log_stats();
void hal_radar_recovery_check();
bool hal_radar_reconfigure(RadarSensorId id,
                            uint8_t max_dist,
                            uint8_t move_sens,
                            uint8_t static_sens,
                            uint16_t unmanned_s);
