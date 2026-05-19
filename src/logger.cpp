// ============================================================
// logger.cpp — Centralni Logger
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// IMPLEMENTACIJA — KLJUČNE ODLOČITVE:
//
// RAM BUFFER — krožni (circular) v PSRAM:
//   Alociran enkrat v logger_init() z ps_malloc() (PSRAM).
//   Velikost: LOG_RAM_BUF_SIZE (~50KB iz config.h).
//   Struktura: flat char array z write pointer in wrap flag.
//   Ko se zapolni: najstarejše vrstice se prepišejo (kroži).
//   logger_get_recent() rekonstruira zadnjih N vrstic z
//   iskanjem nazaj od write pointerja.
//
// SD FLUSH BUFFER — ločen linearni buffer:
//   Manjši (~8KB), akumulira vrstice za SD write.
//   Flush se sproži ko je 80% poln (LOG_FLUSH_THRESHOLD)
//   ali eksplicitno (logger_flush()) ali ob LOG_ERROR.
//   Ob SD napaki: buffer se ne zavrže — čaka na naslednji flush.
//   Ob ponovni SD napaki: buffer se poreže da ne OOM.
//
// NITNA VARNOST — DVE FAZI:
//   Faza 1 (pred logger_sd_attach()): ni mutexa.
//     Kliče se samo iz bsp_init() — en sam thread, varno.
//     Direkten vpis v RAM buffer in Serial.
//   Faza 2 (po logger_sd_attach()): mutex aktiven.
//     Vsi klici iz FreeRTOS taskov so thread-safe.
//     Mutex timeout: 50ms — ob timeoutu log gre samo na Serial.
//
// TIMESTAMP LOGIKA:
//   time(nullptr) > 1577836800 (2020-01-01) = NTP sinhroniziran.
//   Pred NTP: "M<millis()>" — decimalne ms od zagona.
//   Po NTP: "YYYY-MM-DD HH:MM:SS" — lokalni čas (Slovenija CET/CEST).
//   NTP flag se nastavi eksplicitno prek logger_set_ntp_synced().
//
// ============================================================

#include "logger.h"
#include "config.h"
#include "sd_mgr.h"
#include "light_logic.h"
#include "vehicle_recog.h"
#include "hal_radar.h"
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <time.h>
#include <esp_heap_caps.h>

// ============================================================
// KONSTANTE
// ============================================================

// SD flush buffer — manjši od RAM bufferja, za batch SD write
#define SD_FLUSH_BUF_SIZE       (8 * 1024)      // [bytes]
#define SD_FLUSH_TRIGGER        ((SD_FLUSH_BUF_SIZE * LOG_FLUSH_THRESHOLD) / 100)
#define MAX_LINE_LEN            320              // max ena log vrstica
#define LEVEL_STR_LEN           6               // "ERROR\0"
#define TAG_MAX_LEN             8               // max tag dolžina

// ============================================================
// INTERNO STANJE
// ============================================================

static char*             s_ram_buf        = nullptr;   // PSRAM ring buffer
static uint32_t          s_ram_size       = 0;
static uint32_t          s_ram_write_pos  = 0;         // write pointer
static bool              s_ram_wrapped    = false;     // has buffer wrapped
static bool              s_ram_in_psram   = false;     // alloc source (for log)

static char*             s_sd_buf         = nullptr;   // PSRAM — alocira logger_init()
static uint32_t          s_sd_buf_pos     = 0;

static SemaphoreHandle_t s_mutex          = nullptr;
static bool              s_initialized    = false;
static bool              s_sd_attached    = false;
static bool              s_ntp_synced     = false;

// Timestamp cache — updated every second by FreeRTOS timer task (adequate stack).
// build_timestamp() reads from here — avoids localtime_r on async_tcp 4096B stack.
// Intentional benign race: worst case is one log line with a garbled timestamp.
static char              s_ts_cache[16]   = "[M000000000]";
static TimerHandle_t     s_ts_timer       = nullptr;

static uint32_t          s_total_lines    = 0;
static uint32_t          s_dropped_lines  = 0;
static uint32_t          s_flush_count    = 0;
static uint32_t          s_flush_errors   = 0;

// SD flush cooldown — po napaki počakamo 30s preden spet poskušamo.
// Namen: prepreči rekurzivno zanko SD_E → force_flush → SD_E → crash.
// Ko flush faila → nastavimo s_sd_flush_disabled_until_ms = now + 30000.
// Med cooldownom se logi akumulirajo samo v s_sd_buf (PSRAM), ne gredo na SD.
static uint32_t s_sd_flush_disabled_until_ms = 0;

// SD dump position — tracks write_pos at time of last logger_dump_to_sd() call.
// Only content added after this position is written on the next dump call.
static uint32_t s_dump_pos = 0;

// ============================================================
// POMOŽNE — INTERNE
// ============================================================

static bool take_mutex() {
    if (!s_mutex) return true;     // Faza 1: ni mutexa, vedno OK
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void give_mutex() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// FreeRTOS timer callback — runs in timer task (large stack), safe for localtime_r.
// Fires every 1 second after NTP sync; primes s_ts_cache immediately on start.
static void ts_timer_cb(TimerHandle_t) {
    time_t now = time(nullptr);
    if (s_ntp_synced && now > 1577836800UL) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(s_ts_cache, sizeof(s_ts_cache), "[%02d:%02d:%02d]",
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(s_ts_cache, sizeof(s_ts_cache), "[M%09lu]", (unsigned long)millis());
    }
}

// Build timestamp — reads from cache (safe on any stack, including async_tcp 4096B).
// Brez NTP: millis() directly (no localtime_r needed).
static void build_timestamp(char* out, size_t out_len) {
    if (s_ntp_synced) {
        strncpy(out, s_ts_cache, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        snprintf(out, out_len, "[M%09lu]", (unsigned long)millis());
    }
}

// Zapiši v RAM krožni buffer — brez mutexa (caller drži)
// Vrstice so null-terminated in zaporedno v flat arrayu.
// Ko ni prostora: najstarejša vrstica se prepis (kroži).
static void ram_write(const char* line, size_t len) {
    if (!s_ram_buf || len == 0 || len >= s_ram_size) return;

    // Preveri ali je dovolj prostora do konca bufferja
    if (s_ram_write_pos + len + 1 > s_ram_size) {
        // Wrap — pojdi na začetek
        // Zapiši sentinel (0xFF) da get_recent ve kje je wrap point
        if (s_ram_write_pos < s_ram_size) {
            s_ram_buf[s_ram_write_pos] = '\x01';    // wrap marker
        }
        s_ram_write_pos = 0;
        s_ram_wrapped   = true;
    }

    memcpy(s_ram_buf + s_ram_write_pos, line, len);
    s_ram_write_pos += len;
    // Zagotovi null terminator za varnost
    if (s_ram_write_pos < s_ram_size) {
        s_ram_buf[s_ram_write_pos] = '\0';
    }
}

// Zapiši v SD flush buffer — brez mutexa (caller drži)
// Flush sproži sam sebe ko je buffer dovolj poln.
static void sd_buf_write(const char* line, size_t len, bool force_flush) {
    if (!s_sd_buf) return;   // alokacija ni uspela ob init
    if (!s_sd_attached || !sd_mgr_ready()) return;
    if (len == 0) return;

    // Preveri ali se bo prilegalo
    if (s_sd_buf_pos + len >= SD_FLUSH_BUF_SIZE) {
        // Buffer poln — flush takoj (ne čakamo na threshold)
        size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
        if (written == 0) {
            s_flush_errors++;
            // Ob napaki: obreži buffer da ne OOM
            // Ohranimo zadnjo polovico (novejši logi)
            uint32_t keep = s_sd_buf_pos / 2;
            memmove(s_sd_buf, s_sd_buf + (s_sd_buf_pos - keep), keep);
            s_sd_buf_pos = keep;
        } else {
            s_flush_count++;
            s_sd_buf_pos = 0;
        }
    }

    // Kopiraj v buffer (če se zdaj prilega)
    if (s_sd_buf_pos + len < SD_FLUSH_BUF_SIZE) {
        memcpy(s_sd_buf + s_sd_buf_pos, line, len);
        s_sd_buf_pos += len;
    } else {
        // Vrstica je sama po sebi prevelika za buffer — piši direktno
        sd_mgr_log_flush(line, len);
        s_flush_count++;
        return;
    }

    // Sproži flush ob force (ERROR) ali threshold
    bool threshold_hit = (s_sd_buf_pos >= SD_FLUSH_TRIGGER);
    if (force_flush || threshold_hit) {
        // Preveri cooldown — po SD napaki počakamo 30s preden spet poskušamo.
        // Brez cooldowna: SD_E → force_flush → SD_E → loop → heap korupcija.
        uint32_t now_cd = (uint32_t)millis();
        if (s_sd_flush_disabled_until_ms > 0 &&
            now_cd < s_sd_flush_disabled_until_ms) {
            // Cooldown aktiven — preskočimo flush, logi ostanejo v s_sd_buf
            return;
        }

        size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
        if (written == 0) {
            s_flush_errors++;
            // Aktiviraj 30s cooldown — prepreči rekurzivno zanko
            s_sd_flush_disabled_until_ms = (uint32_t)millis() + 30000UL;
        } else {
            s_flush_count++;
            s_sd_buf_pos = 0;
            // Uspešen flush — počisti cooldown
            s_sd_flush_disabled_until_ms = 0;
        }
    }
}

// ============================================================
// INICIALIZACIJA
// ============================================================

bool logger_init() {
    if (s_initialized) return true;

    // Alloc RAM ring buffer — prefer PSRAM
    s_ram_size = LOG_RAM_BUF_SIZE;
    s_ram_buf  = (char*)ps_malloc(s_ram_size);
    if (s_ram_buf) {
        s_ram_in_psram = true;
    } else {
        // PSRAM unavailable — fallback to heap (smaller buffer)
        s_ram_size = 4096;
        s_ram_buf  = (char*)malloc(s_ram_size);
        s_ram_in_psram = false;
        Serial.printf("[LOGGER][W] PSRAM unavailable — RAM buf fallback %d B HEAP\n", s_ram_size);
    }

    if (!s_ram_buf) {
        Serial.printf("[LOGGER][E] RAM buf alloc failed — logger without buffer!\n");
        s_ram_size = 0;
    } else {
        memset(s_ram_buf, 0, s_ram_size);
    }

    // SD flush buffer — PSRAM (saves 8192 B SRAM)
    s_sd_buf = (char*)ps_malloc(SD_FLUSH_BUF_SIZE);
    if (!s_sd_buf) {
        s_sd_buf = (char*)malloc(SD_FLUSH_BUF_SIZE);
        Serial.printf("[LOGGER][W] s_sd_buf: PSRAM unavailable — SRAM fallback (%d B)\n",
                      SD_FLUSH_BUF_SIZE);
    }
    if (!s_sd_buf) {
        Serial.printf("[LOGGER][E] s_sd_buf alloc failed — SD flush disabled!\n");
    } else {
        memset(s_sd_buf, 0, SD_FLUSH_BUF_SIZE);
    }

    s_ram_write_pos = 0;
    s_ram_wrapped   = false;
    s_sd_buf_pos    = 0;
    s_total_lines   = 0;
    s_dropped_lines = 0;
    s_flush_count   = 0;
    s_flush_errors  = 0;
    s_initialized   = true;

    Serial.printf("[LOGGER][I] init OK phase1 | RAM=%uB %s NTP=no\n",
                  (unsigned)s_ram_size, s_ram_in_psram ? "PSRAM" : "HEAP");
    return true;
}

void logger_sd_attach() {
    if (!s_initialized) {
        Serial.printf("[LOGGER][E] logger_sd_attach: not initialized!\n");
        return;
    }

    // Create mutex — scheduler not yet running, safe from bsp_init()
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            Serial.printf("[LOGGER][E] mutex create failed!\n");
            return;
        }
    }

    s_sd_attached = sd_mgr_ready();

    if (s_sd_attached) {
        Serial.printf("[LOGGER][I] attach OK phase2 | Serial+RAM+SD\n");
        // Safety flush of SD staging buffer (s_sd_buf).
        // Normally s_sd_buf_pos == 0 here. This guards against any
        // direct s_sd_buf writes that might have occurred before attach.
        // Phase-1 logs remain in s_ram_buf, accessible via logger_get_recent().
        if (s_sd_buf_pos > 0) {
            size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
            if (written > 0) {
                s_flush_count++;
                s_sd_buf_pos = 0;
                Serial.printf("[LOGGER][I] SD staging buf flushed: %d B\n", (int)written);
            }
        }
    } else {
        Serial.printf("[LOGGER][W] SD not ready — SD output disabled\n");
    }
}

void logger_set_ntp_synced(bool synced) {
    s_ntp_synced = synced;
    if (synced && !s_ts_timer) {
        s_ts_timer = xTimerCreate("ts_cache", pdMS_TO_TICKS(1000), pdTRUE, nullptr, ts_timer_cb);
        if (s_ts_timer) {
            ts_timer_cb(s_ts_timer);    // prime cache immediately before first NTP log
            xTimerStart(s_ts_timer, 0);
        }
    }
    // Ne kličemo LOG_INFO tukaj — avoid recursive call
    // wifi_manager.cpp bo logiral NTP sync sam
}

bool logger_ntp_synced() { return s_ntp_synced; }

// ============================================================
// GLAVNI ENTRY POINT
// ============================================================

void logger_log(LogLevel level, const char* tag,
                const char* format, ...) {
    if (!s_initialized) {
        // Fallback — logger še ni inicializiran, piši direktno
        va_list args;
        va_start(args, format);
        Serial.printf("[PRE-INIT][%s] ", tag ? tag : "?");
        char tmp[256];
        vsnprintf(tmp, sizeof(tmp), format, args);
        Serial.println(tmp);
        va_end(args);
        return;
    }

    // Level filter
#if LOG_LEVEL < 3
    if (level == LOG_LEVEL_DEBUG) return;
#endif
#if LOG_LEVEL < 2
    if (level == LOG_LEVEL_INFO)  return;
#endif
#if LOG_LEVEL < 1
    if (level == LOG_LEVEL_WARN)  return;
#endif

    // Level char — ena črka namesto besede (D/I/W/E)
    char level_char;
    switch (level) {
        case LOG_LEVEL_ERROR: level_char = 'E'; break;
        case LOG_LEVEL_WARN:  level_char = 'W'; break;
        case LOG_LEVEL_INFO:  level_char = 'I'; break;
        case LOG_LEVEL_DEBUG: level_char = 'D'; break;
        default:              level_char = '?'; break;
    }

    // Format sporočila
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Timestamp
    char timestamp[24];
    build_timestamp(timestamp, sizeof(timestamp));

    // Tag — obreži na TAG_MAX_LEN
    char safe_tag[TAG_MAX_LEN + 1];
    if (tag) {
        strncpy(safe_tag, tag, TAG_MAX_LEN);
        safe_tag[TAG_MAX_LEN] = '\0';
    } else {
        strcpy(safe_tag, "?");
    }

    // Assemble log line
    // Format: "[HH:MM:SS][TAG:L] message\n"  (with NTP)
    //         "[M000123456][TAG:L] message\n" (without NTP)
    char line[MAX_LINE_LEN];
    int len = snprintf(line, sizeof(line),
                       "%s[%s:%c] %s\n",
                       timestamp, safe_tag, level_char, message);

    if (len <= 0) return;
    if (len >= (int)sizeof(line)) {
        // Truncated — dodaj marker
        line[sizeof(line) - 5] = '.';
        line[sizeof(line) - 4] = '.';
        line[sizeof(line) - 3] = '.';
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
        len = sizeof(line) - 1;
    }

    // 1. Vedno na Serial — en sam write() da ne pride do prepletanja med taski
#if LOG_ANSI_COLORS
    {
        const char* ansi_prefix;
        switch (level) {
            case LOG_LEVEL_ERROR: ansi_prefix = "\033[31m"; break;  // rdeča
            case LOG_LEVEL_WARN:  ansi_prefix = "\033[33m"; break;  // rumena
            case LOG_LEVEL_DEBUG: ansi_prefix = "\033[90m"; break;  // temno siva
            default:              ansi_prefix = "\033[0m";  break;  // bela/default (INFO)
        }
        // Sestavi: prefix + line + reset — v en buffer, en atomarni write
        char serial_buf[MAX_LINE_LEN + 16];
        int serial_len = snprintf(serial_buf, sizeof(serial_buf),
                                  "%s%s\033[0m", ansi_prefix, line);
        if (serial_len > 0) {
            Serial.write((const uint8_t*)serial_buf, (size_t)serial_len);
        }
    }
#else
    Serial.write((const uint8_t*)line, (size_t)len);
#endif

    // 2. V RAM buffer + SD buffer (thread-safe)
    bool got_mutex = take_mutex();

    if (got_mutex) {
        // RAM buffer
        if (s_ram_buf) {
            if (!s_ram_wrapped || len < (int)s_ram_size) {
                ram_write(line, (size_t)len);
            } else {
                s_dropped_lines++;
            }
        }

        // SD pisanje med normalnim delovanjem ODSTRANJENO (2026-05, Ideja 1).
        // Logi gredo SAMO v PSRAM RAM buffer (s_ram_buf) in Serial.
        // SD flush enkrat na dan ob 00:01 prek sd_midnight_flush_task.
        // logger_flush() ostane za eksplicitne klice (restart, OTA, /api/logs/flush).

        s_total_lines++;
        give_mutex();
    } else {
        // Mutex timeout — vsaj Serial je že šel, štej dropped
        s_dropped_lines++;
    }
}

// ============================================================
// SD FLUSH — ekspliciten
// ============================================================

void logger_flush() {
    if (!s_sd_attached || !sd_mgr_ready()) return;
    if (!take_mutex()) return;

    if (s_sd_buf_pos > 0) {
        // Eksplicitni flush (iz appTask) — počisti cooldown in poskusi
        // Razlog: appTask ima SRAM stack, SD DMA ima prostor ob normalnem delovanju.
        // Periodični flush vsakih 60s bo uspel ko SRAM ni obremenjen.
        s_sd_flush_disabled_until_ms = 0;

        size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
        if (written > 0) {
            s_flush_count++;
            s_sd_buf_pos = 0;
        } else {
            s_flush_errors++;
            // Nastavi cooldown — naslednji eksplicitni flush bo spet poskusil
            s_sd_flush_disabled_until_ms = (uint32_t)millis() + 30000UL;
        }
    }
    give_mutex();
}

// ============================================================
// RAM BUFFER — za /api/logs
// ============================================================

size_t logger_get_recent(char* out, size_t out_len, uint16_t max_lines) {
    if (!out || out_len == 0) return 0;
    if (!s_ram_buf || s_ram_size == 0) {
        strncpy(out, "(RAM buffer unavailable)\n", out_len - 1);
        return strlen(out);
    }

    if (!take_mutex()) {
        strncpy(out, "(mutex timeout)\n", out_len - 1);
        return strlen(out);
    }

    // Sestavimo seznam pointer-jev na začetke vrstic v RAM bufferju.
    // Iščemo nazaj od s_ram_write_pos — zadnje vrstice so tik pred write_pos.
    //
    // Algoritem:
    //   Začnemo na s_ram_write_pos - 1 (zadnji zapisani znak).
    //   Iščemo '\n' znake nazaj.
    //   Vsak '\n' je konec ene vrstice.
    //   Zberemo do max_lines začetkov vrstic.
    //   Nato kopiramo vrstice v out[] v pravilnem vrstnem redu.

    // Maksimalno max_lines ali LOG_WEB_LINES
    uint16_t limit = max_lines > 0 ? max_lines : (uint16_t)LOG_WEB_LINES;

    // Array pointer-jev na začetke vrstic (od najnovejše do najstarejše)
    const uint16_t MAX_PTRS = 200;         // ne static — stack (sensorTask = PSRAM)
    const char* line_starts[MAX_PTRS];     // ne static
    uint16_t    line_lens[MAX_PTRS];       // ne static
    uint16_t    found = 0;

    // Določi obseg iskanja
    uint32_t scan_end   = s_ram_write_pos;    // tja smo zadnje pisali
    uint32_t scan_start = s_ram_wrapped ? scan_end : 0;
    uint32_t scan_len   = s_ram_wrapped ? s_ram_size : scan_end;

    if (scan_len == 0) {
        give_mutex();
        strncpy(out, "(ni logov)\n", out_len - 1);
        return strlen(out);
    }

    // Išči '\n' znake nazaj od write_pos
    uint32_t pos  = (scan_end == 0) ? s_ram_size - 1 : scan_end - 1;
    uint32_t end_of_line = pos;
    uint32_t scanned = 0;

    while (scanned < scan_len && found < limit && found < MAX_PTRS) {
        char c = s_ram_buf[pos];

        if (c == '\x01') {
            // Wrap marker — preskoči
        } else if (c == '\n') {
            // Najdena konec vrstice — začetek je tik za prejšnjim '\n'
            // Poiščemo začetek te vrstice
            uint32_t line_end = end_of_line;
            uint32_t line_start_pos = (pos + 1) % s_ram_size;

            uint32_t llen = 0;
            if (line_end >= line_start_pos) {
                llen = line_end - line_start_pos + 1;
            } else {
                llen = (s_ram_size - line_start_pos) + line_end + 1;
            }

            if (llen > 1 && llen < MAX_LINE_LEN) {
                line_starts[found] = s_ram_buf + line_start_pos;
                line_lens[found]   = (uint16_t)llen;
                found++;
            }
            end_of_line = (pos == 0) ? s_ram_size - 1 : pos - 1;
        }

        pos = (pos == 0) ? s_ram_size - 1 : pos - 1;
        scanned++;
    }

    give_mutex();

    // Kopiraj vrstice v out[] v obratnem vrstnem redu (najstarejša najprej)
    // Vsaka vrstica dobi \n na koncu (logger je shranjuje brez).
    size_t out_pos = 0;
    for (int i = (int)found - 1; i >= 0 && out_pos < out_len - 2; i--) {
        const char* src = line_starts[i];
        uint16_t    ln  = line_lens[i];

        // Preveri ali vrstica ni wrapped čez konec bufferja
        // (za enostavnost: wrappane vrstice preskočimo)
        if (src + ln <= s_ram_buf + s_ram_size) {
            size_t copy = (out_pos + ln < out_len - 2) ? ln : (out_len - 2 - out_pos);
            if (copy == 0) break;
            memcpy(out + out_pos, src, copy);
            out_pos += copy;
            out[out_pos++] = '\n';
        }
    }
    out[out_pos] = '\0';
    return out_pos;
}

uint32_t logger_total_lines()   { return s_total_lines;   }
uint32_t logger_dropped_lines() { return s_dropped_lines; }

// ============================================================
// STATISTIKA
// ============================================================

LoggerStats logger_get_stats() {
    LoggerStats st;
    st.total_lines     = s_total_lines;
    st.dropped_lines   = s_dropped_lines;
    st.sd_flush_count  = s_flush_count;
    st.sd_flush_errors = s_flush_errors;
    st.buf_used_bytes  = s_ram_write_pos;
    st.buf_total_bytes = s_ram_size;
    st.sd_attached     = s_sd_attached;
    st.ntp_synced      = s_ntp_synced;
    return st;
}

// ============================================================
// SD DUMP — incremental ring buffer → SD (for hourly flush)
// ============================================================
// Writes only new content added since the last dump call.
// Allocates a temp PSRAM buffer sized to the new content — freed immediately.
// On wrap: dumps at most s_ram_size bytes (full buffer content).
// Thread-safe.

size_t logger_dump_to_sd() {
    if (!s_sd_attached || !sd_mgr_ready()) return 0;
    if (!s_ram_buf || s_ram_size == 0) return 0;
    if (!take_mutex()) return 0;

    uint32_t write_pos = s_ram_write_pos;
    uint32_t dump_pos  = s_dump_pos;
    give_mutex();

    // Calculate new bytes since last dump
    uint32_t new_bytes;
    bool wraps;
    if (write_pos >= dump_pos) {
        new_bytes = write_pos - dump_pos;
        wraps = false;
    } else {
        // write_pos wrapped past dump_pos
        new_bytes = (s_ram_size - dump_pos) + write_pos;
        wraps = true;
    }

    if (new_bytes == 0) return 0;
    if (new_bytes > s_ram_size) new_bytes = s_ram_size;  // safety cap

    // Allocate temp buffer in PSRAM — freed after write
    char* tmp = (char*)ps_malloc(new_bytes);
    if (!tmp) tmp = (char*)malloc(new_bytes);
    if (!tmp) return 0;

    // Copy new bytes from ring buffer (handle wrap)
    if (!wraps) {
        memcpy(tmp, s_ram_buf + dump_pos, new_bytes);
    } else {
        uint32_t first  = s_ram_size - dump_pos;
        uint32_t second = write_pos;
        memcpy(tmp,         s_ram_buf + dump_pos, first);
        memcpy(tmp + first, s_ram_buf,             second);
    }

    size_t written = sd_mgr_log_flush(tmp, new_bytes);
    free(tmp);

    if (written > 0) {
        s_dump_pos = write_pos;
    }
    return written;
}

// ============================================================
// SYSTEM HEALTH SUMMARY
// ============================================================
// Compact multi-line health report — boot or periodic.
// is_boot=true:  called once from appTask after parking_log_init()
// is_boot=false: called every 120s from light_logic_tick()
//
// Radar on-demand stats (hal_radar_log_stats) only on boot —
// radar has its own 60s log in radarTask, do not duplicate.

void logger_log_system_health(bool is_boot) {
    // --- SSR + ENV from light_logic ---
    LightLogicState ll = light_logic_get_state();

    // --- Logger stats ---
    LoggerStats ls = logger_get_stats();

    // --- SD ---
    bool sd_ok = sd_mgr_ready();

    // --- VR diagnostics ---
    vr_diagnostics_t vr = vehicle_recog_get_diagnostics();

    // --- Radar ---
    const RadarSensorStatus& r0 = hal_radar_get_status(RADAR_SENSOR_VHOD);
    const RadarSensorStatus& r1 = hal_radar_get_status(RADAR_SENSOR_CESTA_L);
    const RadarSensorStatus& r2 = hal_radar_get_status(RADAR_SENSOR_CESTA_D);
    const RadarSensorStatus& r3 = hal_radar_get_status(RADAR_SENSOR_GARAZA);

    uint32_t uptime_s = (uint32_t)(millis() / 1000UL);

    LOG_INFO("LOGGER", "%s", is_boot ? "=== BOOT HEALTH ===" : "=== SYS HEALTH ===");

    auto ssr_on = [](const SsrState& s) -> bool {
        return s.state == SsrLogicState::ON_AUTO || s.state == SsrLogicState::ON_MANUAL;
    };

    LOG_INFO("LOGGER", "SSR:  1=%s%s 2=%s%s 3=%s%s 4=%s%s",
             ssr_on(ll.ssr[1]) ? "on" : "off", ll.ssr[1].disabled ? "(dis)" : "",
             ssr_on(ll.ssr[2]) ? "on" : "off", ll.ssr[2].disabled ? "(dis)" : "",
             ssr_on(ll.ssr[3]) ? "on" : "off", ll.ssr[3].disabled ? "(dis)" : "",
             ssr_on(ll.ssr[4]) ? "on" : "off", ll.ssr[4].disabled ? "(dis)" : "");

    LOG_INFO("LOGGER", "ENV:  %s  lux=%.1f  motion=%s  alarm=%s",
             ll.is_night ? "NIGHT" : "DAY",
             (double)ll.lux,
             ll.any_motion ? "yes" : "no",
             ll.alarm_active ? "ON" : "off");

    LOG_INFO("LOGGER", "RAM:  buf=%uB %s  used=%uB  dropped=%lu",
             (unsigned)ls.buf_total_bytes,
             s_ram_in_psram ? "PSRAM" : "HEAP",
             (unsigned)ls.buf_used_bytes,
             (unsigned long)ls.dropped_lines);

    LOG_INFO("LOGGER", "SD:   %s  NTP=%s  uptime=%lus",
             sd_ok ? "ok" : "err",
             ls.ntp_synced ? "ok" : "no",
             (unsigned long)uptime_s);

    LOG_INFO("LOGGER", "VR:   A=%s(%u) B=%s(%u)  recs=%lu aborted=%lu",
             vr.baseline_A_valid ? "calib" : "uncalib", (unsigned)vr.models_A,
             vr.baseline_B_valid ? "calib" : "uncalib", (unsigned)vr.models_B,
             (unsigned long)vr.total_recognitions,
             (unsigned long)vr.total_aborted);

    LOG_INFO("LOGGER", "RADAR: Vhod=%s CestaL=%s CestaD=%s Garaza=%s",
             r0.active ? "ok" : "err",
             r1.active ? "ok" : "err",
             r2.active ? "ok" : "err",
             r3.active ? "ok" : "err");

    if (is_boot) {
        hal_radar_log_stats();
    }
}
