// ============================================================
// logger.h — Centralni Logger
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// ODGOVORNOST:
//   Enoten vstopni točka za vse logiranje v projektu.
//   Nadomesti vse direktne Serial.printf() klice v modulih.
//
// IZHODI (vzporedno, vsak log gre na vse aktivne):
//   1. Serial (USB)      — vedno, od logger_init() naprej
//   2. RAM krožni buffer — vedno, od logger_init() naprej
//                          vir za /api/logs web endpoint
//   3. SD kartica        — šele po logger_sd_attach()
//                          dnevna rotacija, flush pri 80% bufferja
//
// INICIALIZACIJSKI VRSTNI RED (kritično):
//   logger_init()       — takoj po Serial.begin() v bsp.cpp
//                         pred vsem ostalim (Wire, GPIO, SD)
//                         Faza 1: Serial + RAM buffer
//   logger_sd_attach()  — po sd_mgr_init() v bsp.cpp
//                         Faza 2: doda SD flush
//   logger_set_ntp_synced() — pokliče wifi_manager.cpp ob NTP sinhronizaciji
//                         Od tu naprej: berljivi timestampsi
//
// NITNA VARNOST:
//   Pred scheduler startom (v bsp_init()): mutex ne obstaja,
//   vpisi so direktni — varno ker teče en sam thread (setup()).
//   Po logger_sd_attach(): mutex kreiran, od tu naprej thread-safe.
//   Mutex se kreira v logger_sd_attach() ker taski še niso startani.
//
// FORMAT LOG VRSTICE:
//   Z NTP:    "[HH:MM:SS][TAG:L] message\n"    (10-char timestamp)
//   Brez NTP: "[M000123456][TAG:L] message\n"  (M + 9-digit millis)
//             L = D/I/W/E
//
// MAKROJI (primarna API — vsi moduli kličejo samo te):
//   LOG_ERROR("TAG", "fmt", ...)  — flush takoj na SD (kritično)
//   LOG_WARN ("TAG", "fmt", ...)
//   LOG_INFO ("TAG", "fmt", ...)
//   LOG_DEBUG("TAG", "fmt", ...)  — samo če LOG_LEVEL >= 3
//
//   LOG_LEVEL build flag v platformio.ini:
//     0 = ERROR only | 1 = +WARN | 2 = +INFO | 3 = +DEBUG
//     Privzeto v razvoju: 3 | Produkcija: 2
//
// ODVISNOSTI:
//   config.h   — LOG_RAM_BUF_SIZE, LOG_FLUSH_THRESHOLD, LOG_WEB_LINES
//   sd_mgr.h   — sd_mgr_log_flush(), sd_mgr_ready()
//   time.h     — NTP timestamp
//
// ============================================================

// LOG FORMAT STANDARD:
//   [HH:MM:SS][TAG:L] message        (with NTP)
//   [M000123456][TAG:L] message      (without NTP, M + 9-digit millis)
//   L = D/I/W/E | target line length: 60–90 chars
//
// VALID TAGs: BSP LOGGER LLOGIC WIFI SENSOR RADAR TOF VR
//             ALARM PLOG WEBUI SDMGR LED GPIO LIGHT SCREEN SDFLUSH
//
// LANGUAGE: English. Exception: UI labels visible to user on screen.
//
// CONTENT: always include concrete context. Avoid: "OK", "Initialized.", "Ready."
//
// EXAMPLE:
//   LOG_INFO("BSP", "Wire1 init OK | SDA=IO17 SCL=IO18 freq=100kHz");
//   LOG_WARN("RADAR", "SC16[0x48] chA: OE! overflow=%lu", cnt);

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdarg.h>

// ============================================================
// LOG NIVOJI
// ============================================================

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} LogLevel;

// ============================================================
// MAKROJI — primarna API za vse module
// ============================================================
// Uporaba: LOG_INFO("BSP", "Wire1 init OK, freq=%d", freq);
// TAG: kratka oznaka modula, max 8 znakov (BSP, WIFI, SENSOR, ...)

#ifndef LOG_LEVEL
#define LOG_LEVEL 3     // privzeto DEBUG med razvojem
#endif

#define LOG_ERROR(tag, fmt, ...) logger_log(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag,  fmt, ...) logger_log(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)

#if LOG_LEVEL >= 2
#define LOG_INFO(tag,  fmt, ...) logger_log(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(tag,  fmt, ...)  do {} while(0)
#endif

#if LOG_LEVEL >= 3
#define LOG_DEBUG(tag, fmt, ...) logger_log(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(tag, fmt, ...) do {} while(0)
#endif

// ============================================================
// INICIALIZACIJA
// ============================================================

// Faza 1 — takoj po Serial.begin() v bsp.cpp
// Inicializira RAM buffer, Serial izhod.
// Mutex se NE kreira tukaj (scheduler še ne teče).
// Vrne true vedno (ne more ne uspeti).
bool logger_init();

// Faza 2 — po sd_mgr_init() v bsp.cpp
// Kreira FreeRTOS mutex (scheduler še ne teče — varno).
// Registrira SD kot flush izhod.
// Po tem klicu je logger popolnoma thread-safe.
void logger_sd_attach();

// Obvesti logger da je NTP sinhroniziran.
// Pokliče wifi_manager.cpp ko configTime() uspe.
// Od tu naprej: Unix timestamp namesto millis().
void logger_set_ntp_synced(bool synced);

// true = NTP sinhroniziran
bool logger_ntp_synced();

// ============================================================
// GLAVNI ENTRY POINT
// ============================================================

// Kliči samo prek makrojev zgoraj — ne direktno.
// __attribute__((format)) za compile-time format string preverjanje.
void logger_log(LogLevel level, const char* tag,
                const char* format, ...)
                __attribute__((format(printf, 3, 4)));

// ============================================================
// SD FLUSH
// ============================================================

// Ekspliciten flush RAM bufferja na SD.
// Kliče se: ob LOG_ERROR (avtomatsko), ob WiFi izpadu,
//           periodično iz wifiTask vsakih 60s.
// Thread-safe.
void logger_flush();

// ============================================================
// RAM BUFFER — za web endpoint /api/logs
// ============================================================

// Vrne zadnjih N vrstic iz RAM bufferja kot null-terminated string.
// out      = caller-alocirani buffer
// out_len  = velikost out bufferja
// max_lines = max vrstic (0 = vse dostopne do out_len)
// Vrne število znakov v out (brez null terminator).
size_t logger_get_recent(char* out, size_t out_len, uint16_t max_lines);

// Vrne skupno število log vrstic od zagona
uint32_t logger_total_lines();

// Vrne število vrstic ki so bile izgubljene (buffer overflow)
uint32_t logger_dropped_lines();

// ============================================================
// STATISTIKA
// ============================================================

struct LoggerStats {
    uint32_t total_lines;       // skupno število vrstic od zagona
    uint32_t dropped_lines;     // izgubljene (buffer overflow)
    uint32_t sd_flush_count;    // število SD flush operacij
    uint32_t sd_flush_errors;   // število neuspešnih SD flushev
    uint32_t buf_used_bytes;    // trenutno zaseden RAM buffer [bytes]
    uint32_t buf_total_bytes;   // skupna velikost RAM bufferja [bytes]
    bool     sd_attached;       // true = SD je registriran kot izhod
    bool     ntp_synced;        // true = berljivi timestampsi
};

LoggerStats logger_get_stats();

// ============================================================
// SD DUMP — incremental ring buffer → SD
// ============================================================

// Dumps new ring buffer content to SD (only bytes added since the last call).
// Allocates a temp PSRAM buffer — freed immediately after write.
// Thread-safe. Returns bytes written, 0 on error or nothing new.
// Called by sd_midnight_flush_task every full hour.
size_t logger_dump_to_sd();

// ============================================================
// SYSTEM HEALTH SUMMARY
// ============================================================

// Prints a compact multi-line health summary to the log.
// is_boot=true  → called once from appTask after parking_log_init()
// is_boot=false → called every 120s from light_logic_tick()
// Boot call also triggers hal_radar_log_stats(); periodic does not.
void logger_log_system_health(bool is_boot);
