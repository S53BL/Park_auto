// ============================================================
// config.h — Centralne konstante projekta
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — Ekran + touch (Wire1 izklopljen)
// ============================================================
//
// FAZA 0 SPREMEMBE glede na v1.0.1:
//   - Odstranjene Wire2 / I2C Bus 1 definicije (IO21/IO38)
//   - Vse I2C konstante so samo za Wire in Wire1
//   - WDT_TIMEOUT_SEC poenoteno (bil WDT_TIMEOUT_S in WDT_TIMEOUT_SEC)
//
// ============================================================

#pragma once
#include <stdint.h>

// ============================================================
// 1. VERZIJA FIRMWARE-A
// ============================================================
#define FW_VERSION_STRING   "2.0.0-dev"

// ============================================================
// 2. DISPLAY IN TOUCH PINI (BSP ONLY)
// ============================================================
// Display: AXS15231B — QSPI (4 podatkovne linije, ni MISO, ni RST)
// Touch:   integrirano v AXS15231B, I2C interni bus (IO7/IO8)
// Touch reset: prek TCA9554 GPIO expander (interni bus, 0x20)
//
// Waveshare demo referenca: 10_lvgl_arduino_v9.ino

#define PIN_LCD_CS          12
#define PIN_LCD_CLK         5
#define PIN_LCD_D0          1
#define PIN_LCD_D1          2
#define PIN_LCD_D2          3
#define PIN_LCD_D3          4
#define PIN_LCD_BL          6       // HIGH = prižgano

#define LCD_HOR_RES         320
#define LCD_VER_RES         480

// Touch (interni bus IO7/IO8 — BSP ga inicializira, ne mi)
#define PIN_TOUCH_SDA       7
#define PIN_TOUCH_SCL       8
#define TCA9554_ADDR        0x20    // TCA9554 na internem busu (touch reset)
#define TCA9554_TOUCH_PIN   1       // pin 1 = touch reset line

// LVGL
#define LVGL_BUF_LINES      40
#define LVGL_TICK_MS        5
#define LVGL_HANDLER_MS     5

// ============================================================
// 3. I2C BUS KONFIGURACIJA
// ============================================================
// FAZA 0: samo interni bus (Wire, IO7/IO8) je aktiven.
// Wire1 (IO17/IO18) je zakomeniran — nič ni priklopljeno.
//
// Interni bus — Wire (Waveshare BSP):
//   AXS15231B touch + TCA9554 (touch reset @ 0x20)
//   AXP2101 PMU, QMI8658 IMU, PCF85063 RTC, ES8311 Audio
//   ⚠ Ne inicializiramo sami — BSP to naredi z Wire.begin(8, 7)
//
// Senzorski bus — Wire1 (IO17/IO18):
//   TCA9548A (0x70), SC16IS752 #1 (0x48), SC16IS752 #2 (0x4C)
//   MCP23017 (0x20), BH1750 (0x23)
//   ⚠ FAZA 0: ZAKOMENTIRANO

#define I2C_SDA             17      // Wire1 SDA (za kasnejše faze)
#define I2C_SCL             18      // Wire1 SCL (za kasnejše faze)
#define I2C_FREQ_HZ         100000

// ============================================================
// 4. GPIO PINI — ESP32-S3 DIREKTNI
// ============================================================
#define PIN_LED_MAIN        39      // → 74HC257N MUX → 10× WS2815
#define PIN_LED_SIGNAL      40      // → direktno → 144 LED signalna
#define PIN_MUX_SELECT      45      // LOW=Primary, HIGH=Party(WLED)
#define PIN_TCA_RESET       46      // TCA9548A hw reset (aktiven LOW)
#define PIN_MCP_INT         47      // MCP23017 INTA interrupt
#define PIN_SC1_IRQ         41      // SC16IS752 #1 IRQ
#define PIN_SC2_IRQ         42      // SC16IS752 #2 IRQ

// ============================================================
// 5. I2C NASLOVI ČIPOV
// ============================================================
// Wire1 (senzorski — FAZA 0 neaktiven):
#define I2C_ADDR_TCA9548A   0x70
#define I2C_ADDR_TOF        0x29
#define I2C_ADDR_SC16_1     0x48
#define I2C_ADDR_SC16_2     0x4C
#define I2C_ADDR_MCP23017   0x20
#define I2C_ADDR_BH1750     0x23

// ============================================================
// 6. MCP23017 PORT ASSIGNMENT (za kasnejše faze)
// ============================================================
#define MCP_GPA0_RAMPAGOR   0x01
#define MCP_GPA1_RAMPALUC   0x02
#define MCP_GPA2_VRATAOD    0x04
#define MCP_GPA3_CELICA1    0x08
#define MCP_GPA4_CELICA2    0x10
#define MCP_GPA5_SSR1       0x20
#define MCP_GPA6_SSR2       0x40
#define MCP_GPA7_SSR3       0x80
#define MCP_GPB0_SSR4       0x01

#define MCP_PORTA_IODIR     0x1F
#define MCP_PORTB_IODIR     0xFE
#define MCP_PORTA_PULLUP    0x1F
#define MCP_PORTB_PULLUP    0xFE

// ============================================================
// 7. TCA9548A KANALI (za kasnejše faze)
// ============================================================
#define TCA_CH_TOF_H_A      0
#define TCA_CH_TOF_P1_A     1
#define TCA_CH_TOF_P2_A     2
#define TCA_CH_TOF_H_B      3
#define TCA_CH_TOF_P1_B     4
#define TCA_CH_TOF_P2_B     5

#define TOF_POLL_IDLE_MS    100
#define TOF_POLL_DETECT_MS  40
#define TOF_POLL_SCAN_MS    90
#define TOF_I2C_TIMEOUT_MS  50
#define TOF_RECOVERY_RETRIES 3
#define TCA_RECOVERY_WAIT_MS 10

// ============================================================
// 8. RADAR (za kasnejše faze)
// ============================================================
#define RADAR_COUNT         4
// XTAL na CJMCU-SC16IS752 modulu je 1.8432 MHz (potrjeno).
// Divisor = 1.8432MHz / (16 × 115200) = 1.000 → točno, 0% napaka.
#define RADAR_BAUD_RATE     115200

// ============================================================
// 9. LED KONFIGURACIJA
// ============================================================
#define LED_MAIN_PIN        PIN_LED_MAIN
#define LED_MAIN_LOGICAL    90
#define LED_SIGNAL_PIN      PIN_LED_SIGNAL
#define LED_SIGNAL_COUNT    144
#define MUX_SWITCH_DELAY_MS 200

// Signalna LED cone
#define LED_SIG_ZONE_BOT_START  0
#define LED_SIG_ZONE_BOT_END    47
#define LED_SIG_ZONE_MID_START  48
#define LED_SIG_ZONE_MID_END    95
#define LED_SIG_ZONE_TOP_START  96
#define LED_SIG_ZONE_TOP_END    143

// ============================================================
// 10. SSR
// ============================================================
#define SSR_ON              HIGH
#define SSR_OFF             LOW
#define SSR1_STABILIZE_MS   10
#define SSR2_DELAY_MS       500
#define SSR3_ANTIFORGET_MS  (5 * 60 * 1000)
#define SSR4_ANTIFORGET_MS  (5 * 60 * 1000)

// ============================================================
// 11. FREERTOS TASKI
// ============================================================
#define CORE_WIFI           0
#define CORE_APP            1

#define TASK_WIFI_STACK     8192
#define TASK_WIFI_PRIO      1
#define TASK_EVENTBUS_STACK 4096
#define TASK_EVENTBUS_PRIO  5
#define TASK_SENSOR_STACK   6144
#define TASK_SENSOR_PRIO    4
#define TASK_LED_STACK      4096
#define TASK_LED_PRIO       3
#define TASK_LVGL_STACK     8192
#define TASK_LVGL_PRIO      2
#define TASK_APP_STACK      6144
#define TASK_APP_PRIO       3

#define EVENTBUS_QUEUE_SIZE 16

// ============================================================
// 12. OSVETLITEV
// ============================================================
#define LIGHT_LUX_THRESHOLD     50
#define LIGHT_POLL_MS           1000
#define LIGHT_FADE_SLOW_MS      7000
#define LIGHT_FADE_FAST_MS      2500
#define LIGHT_TIMEOUT_AUTO_MS   (3 * 60 * 1000)
#define LIGHT_TIMEOUT_MANUAL_MS (30 * 60 * 1000)
#define LED_FILL_SPEED_MS       6000
#define LED_UNFILL_SPEED_MS     3000
#define LED_FADE_DURATION_MS    800
#define LED_TARGET_BRIGHTNESS   200
#define LED_NIGHT_BRIGHTNESS    120
#define LED_NIGHT_HOUR_START    22
#define LED_NIGHT_HOUR_END      6

// ============================================================
// 13. DTW (za kasnejše faze)
// ============================================================
#define VEH_ENTRY_THRESH_MM     3500
#define VEH_DELTA_FILTER_MM     15
#define VEH_MIN_PROFILE_PTS     25
#define VEH_STABLE_TIME_MS      1500
#define DTW_NORMALIZE_PTS       80
#define DTW_SAKOE_RADIUS        15
#define DTW_MATCH_THRESHOLD     18.0f
#define VEH_MODEL_FILE_A        "/models/models_parkingA.json"
#define VEH_MODEL_FILE_B        "/models/models_parkingB.json"

// ============================================================
// 14. SIGNALNA LED PARAMETRI
// ============================================================
#define SIG_PARK_THRESH_GREEN   1500
#define SIG_PARK_THRESH_ORANGE  1000
#define SIG_PARK_THRESH_RED     500
#define SIG_PARK_STABLE_MS      4000
#define SIG_CELL_TIMER_MS       (5 * 60 * 1000)
#define SIG_CLOCK_DURATION_MS   10000

// ============================================================
// 15. LOGGER
// ============================================================
#define LOG_RAM_BUF_SIZE        (50 * 1024)
#define LOG_FLUSH_THRESHOLD     80
#define LOG_WEB_LINES           200

// ============================================================
// 16. WEB UI
// ============================================================
#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME           "parking-esp32"
#endif
#ifndef WEB_PORT
#define WEB_PORT                80
#endif
#define WEB_POLL_INTERVAL_MS    2000

// ============================================================
// 17. WATCHDOG
// ============================================================
#define WDT_TIMEOUT_SEC         10

// ============================================================
// 18. SD KARTICA
// ============================================================
#define SD_LOG_PATH             "/logs/"
#define SD_RAW_PATH             "/raw/"
#define SD_MAX_LOG_AGE_DAYS     30
