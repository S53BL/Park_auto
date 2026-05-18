// ============================================================
// config.h — Centralne konstante projekta
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
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
// LVGL_HANDLER_MS: povečano 5→10ms (2026-05, CPU razbremenitev)
// Razlog: 5ms = 200Hz lvglTask tick — preveč za ta projekt.
//   10ms = 100Hz → polovica CPU obremenitve lvglTask.
//   Vizualni efekt: animacije ostanejo gladke (100fps > 60fps zaslona).
//   Touch odzivnost: 10ms zamuda je neopazna (prag zaznavanja ~50ms).
#define LVGL_HANDLER_MS     10    // SPREMENJEN iz 5ms (2026-05)
// UI_REFRESH_TIMER_MS: interval LVGL timer za Opcija B display polling.
// 1000ms namesto 500ms — countdown bo manj gladek (±1s natančnost)
//   ampak LVGL timer obremenitev se prepolovi.
//   Za SSR countdown prikaz je 1s natančnost povsem sprejemljiva.
// Lokacija: hal_display.cpp ui_refresh_cb timer kreacija.
#define UI_REFRESH_TIMER_MS 1000  // NOVO (2026-05) — prej hardcoded 500ms

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

// TOF_POLL_IDLE_MS: povečano 200→500ms (2026-05, Wire1 razbremenitev)
// Razlog: TOF v IDLE fazi samo čaka na vstop vozila — 500ms latenca je
//   neopazna za uporabnika. Wire1 zasedenost: 5ms/500ms = 1% (prej 2.5%).
// OPOMBA: TOF WATCHDOG (600s) se ne spreminja — ostaja nespremenjen.
#define TOF_POLL_IDLE_MS    500   // SPREMENJEN iz 200ms (2026-05)
#define TOF_POLL_DETECT_MS  50
// F1: DETECT faza timeout — po 5 min brez detekcije → nazaj v IDLE
#define TOF_DETECT_TIMEOUT_MS   (5UL * 60UL * 1000UL)
// TOF IDLE watchdog interval — health-check meritev H_A + H_B vsakih 10 minut
#define TOF_WATCHDOG_INTERVAL_MS    600000
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
// RADAR_TRIGGER_THROTTLE_MS: minimalni interval med TRIGGER_ON_AUTO
// ukazi v SSR command queue (2026-05).
// Zakaj 500ms: radar pošilja ~40 eventov/s (4 kanali × 10fps).
//   Brez throttla queue se zapolni v 800ms → "queue polna" WARN.
//   500ms = max 2 ukaza/s → queue nikoli ni polna.
//   SSR1 timer reset (ko je SSR1 že ON) se zgodi znotraj throttla —
//   timer se resetira najkasneje 500ms po zadnjem gibanju.
#define RADAR_TRIGGER_THROTTLE_MS   500
// RADAR_PUBLISH_INTERVAL_MS: minimalni interval med callback klici
// za isti kanal (2026-05).
//
// LD2410C pošilja frame vsakih ~100ms (10 Hz fiksno).
// Brez tega intervala: callback → EventBus → light_logic se kliče
// 40×/s (4 kanali × 10 Hz) → poplava logov, Wire1 contention.
//
// Z vrednostjo 100ms: callback se kliče max 10×/s skupaj (1×/kanal/100ms).
// EventBus obremenitev se zmanjša 4× (od 40/s na 10/s).
//
// Latenca zaznave gibanja: max 100ms (publish interval) +
//   100ms (appTask tick) = 200ms worst-case.
//   Za prižig luči je 200ms zamuda neopazna.
//
// Vrednost je nastavljiva — ne hardkodiraj v hal_radar.cpp!
// Razpon: min 50ms (agresivno), max 1000ms (počasno).
#define RADAR_PUBLISH_INTERVAL_MS   100
// RADAR_DRAIN_MAX_LOOPS: max iteracij drain zanke v process_chip_irq()
// (2026-05, OE! odprava).
//
// Zakaj 4 in ne 32:
//   LD2410C frame = 23 bajtov. SC16IS752 FIFO trigger = 8 bajtov.
//   Normalno: 2-3 drain iteracije na frame (8 + 8 + 7 bajtov).
//   max_loops=4 je varen margin brez nepotrebnega Wire1 blokiranje.
//
//   Stara vrednost max_loops=32:
//     Drain čas = 32 × 3 I2C transakcije × ~1ms = do 96ms.
//     Med tem chip2 FIFO (64B) se zapolni v 5.5ms → OE! neizogibni.
//
//   Nova vrednost max_loops=4:
//     Drain čas = 4 × 3ms = 12ms.
//     Z round-robin (radarTask zanka): chip1 draina 1 iter. (~3ms),
//     nato takoj chip2 → chip2 čaka max 3ms << 5.5ms FIFO zapolnitev.
//
//   Vrednost je nastavljiva — ne hardkodiraj v hal_radar.cpp!
#define RADAR_DRAIN_MAX_LOOPS   4
// RADAR_OE_LOG_INTERVAL_MS: minimalni interval med OE! log izpisi per kanal.
// OE! ob zagonu (prvih 10s): normalni — TOF init zasede Wire1 ~300ms.
// Po zagonu: OE! redki z round-robin drainanjem.
// 30s interval: dovolj redko da ne zamaši loga, dovolj pogosto za diagnostiko.
#define RADAR_OE_LOG_INTERVAL_MS    30000
// RADAR_DEBUG_PROTOCOL: če je definirano, logira TX/RX bajte pri
// konfiguracijskem protokolu. Izklopi v produkciji — preveč verbose.
// Odkomentiraj za debug konfiguracije, zakomentiraj po uspešnem testu.
// #define RADAR_DEBUG_PROTOCOL

// Radar polling interval (v2.0 arhitektura)
#define RADAR_POLL_INTERVAL_MS_DEFAULT   50u
#define RADAR_POLL_INTERVAL_MIN_MS       10u
#define RADAR_POLL_INTERVAL_MAX_MS      100u
#define RADAR_MAX_CONSECUTIVE_OVERFLOWS  10u

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
// Minimalni interval med zaporednima BUTTON_SSR eventoma istega gumba.
// Touch panel pošlje dvojne evente — 500ms je neopazno za uporabnika.
#define BUTTON_DEBOUNCE_MS  500

// ============================================================
// 11. FREERTOS TASKI
// ============================================================
#define CORE_WIFI           0
#define CORE_APP            1

// wifiTask stack: 4096 B (2026-05: znižano iz 8192)
// AsyncTCP ne potrebuje handleClient() v wifiTask — handlers tečejo v AsyncTCP tasku.
// wifiTask po zagonu: samo watchdog zanka + NTP check vsakih 6h.
// Stack free v originalni 4096 arhitekturi: 956-964 B free → dovolj rezerve.
// Historični razlog za 8192: sinhroni WebServer iteracije — ZASTARELO z v3.0.
// ⚠ Stack MORA biti v SRAM (ne PSRAM) — WiFi init kliče flash ops.
#define TASK_WIFI_STACK     5120
// 2026-05: dvigneno iz 4096. Izmerjeno: 784 B free / 4096 → premalo rezerve
// ob reconnect + NTP resync + logging kombinaciji.
#define TASK_WIFI_PRIO      1
// TASK_EVENTBUS_STACK: zmanjšano 6144→4096 (2026-05, optimizacija RAM)
// Razlog: eventBusTask handlerji (on_button_ssr, on_radar_motion itd.)
//   so po SsrCmd queue refaktoringu SAMO enqueue operacije (~50B stack).
//   Ni več Wire1 klicev v handlerjih → ni več 512B logger stack nevarnosti.
//   4096B z PSRAM overhead daje faktor varnosti >2×.
#define TASK_EVENTBUS_STACK 4096
#define TASK_EVENTBUS_PRIO  5
#define TASK_SENSOR_STACK   6144
#define TASK_SENSOR_PRIO    4
#define TASK_LED_STACK      6144   // povečano 2026-05: signal_led ExplodeParticles + float aritmetika
#define TASK_LED_PRIO       3
#define TASK_LVGL_STACK     8192
#define TASK_LVGL_PRIO      2
// TASK_APP_STACK: zmanjšano 8192→6144 (2026-05, optimizacija RAM)
// Razlog: dejanski stack ob zagonu = 5748B free / 8192B total = samo
//   2444B porabljenih. Worst-case med SSR sekvenco: ~2000B dodatno =
//   skupaj ~4500B. 6144B daje faktor varnosti 1.35× — zadostuje.
//   Ob dvomih: log "appTask stack ob zagonu" bo pokazal dejanski usage.
#define TASK_APP_STACK      6144
#define TASK_APP_PRIO       3
// RADAR_TASK_STACK: povečano iz 4096 na 8192 (2026-05, stabilizacija)
// Razlog: radarTask teče v PSRAM (MALLOC_CAP_SPIRAM). FreeRTOS PSRAM stack
// ima ~1.5-2× overhead na frame. IDF 5.3 i2c_master_transmit() porabi ~800B,
// LOG_WARN format buffer ~512B, process_chip_irq() drain zanka ~300B.
// Worst-case z PSRAM overhead: ~4140B → preseže 4096 → stack canary crash.
// 8192B daje faktor varnosti 2× brez opaznega vpliva na PSRAM porabo.
#define RADAR_TASK_STACK    8192

#define EVENTBUS_QUEUE_SIZE 16

// ============================================================
// 12. OSVETLITEV
// ============================================================
#define LIGHT_LUX_THRESHOLD     50
// BH1750 polling in povprečenje
#define LIGHT_POLL_SENSOR_MS        30000   // interval med branjem senzorja (ms)
#define LIGHT_AVG_SAMPLES           5       // vzorci v drsečem povprečju
#define LIGHT_LUX_NIGHT             40      // prag pod katerim preklopi v NOČ
#define LIGHT_LUX_DAY               70      // prag nad katerim preklopi v DAN
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
#define SIG_CLOCK_COOLDOWN_MS   (5UL * 60UL * 1000UL)   // 300000ms

// ============================================================
// 15. LOGGER
// ============================================================
// LOG_RAM_BUF_SIZE: povečano 16KB→128KB (2026-05, Ideja 1 — SD kot backup medij)
// Razlog: SD se ne piše med normalnim delovanjem → logi živijo cel dan v PSRAM.
// 128 KB pri ~80 B/vrstica = ~1600 vrstic = dovolj za cel dan logov.
// PSRAM ima ~6.5 MB prostora — 128 KB je zanemarljivo.
#define LOG_RAM_BUF_SIZE        (128 * 1024)
// LOG_FLUSH_THRESHOLD: neuporabljeno (sd_buf_write ni več v logger_log, 2026-05).
// Vrednost ohranjena za logger_flush() ki ostane za eksplicitne klice.
#define LOG_FLUSH_THRESHOLD     15
#define LOG_WEB_LINES           200
// LOG_ANSI_COLORS: ANSI escape kode v Serial izhodu (ne SD/RAM buffer)
// 1 = vklop (E=rdeča, W=rumena, I=bela, D=temno siva), 0 = izklop
#define LOG_ANSI_COLORS         1

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
