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
#include <freertos/semphr.h>
#include <time.h>

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

static char*             s_ram_buf        = nullptr;   // PSRAM krožni buffer
static uint32_t          s_ram_size       = 0;
static uint32_t          s_ram_write_pos  = 0;         // write pointer
static bool              s_ram_wrapped    = false;     // ali je buffer že kroži

static char              s_sd_buf[SD_FLUSH_BUF_SIZE];
static uint32_t          s_sd_buf_pos     = 0;

static SemaphoreHandle_t s_mutex          = nullptr;
static bool              s_initialized    = false;
static bool              s_sd_attached    = false;
static bool              s_ntp_synced     = false;

static uint32_t          s_total_lines    = 0;
static uint32_t          s_dropped_lines  = 0;
static uint32_t          s_flush_count    = 0;
static uint32_t          s_flush_errors   = 0;

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

// Sestavi timestamp string
// Z NTP:    "2026-04-15 14:32:01"
// Brez NTP: "M000123456"
static void build_timestamp(char* out, size_t out_len) {
    time_t now = time(nullptr);
    if (s_ntp_synced && now > 1577836800UL) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(out, out_len, "M%010lu", (unsigned long)millis());
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
        size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
        if (written == 0) {
            s_flush_errors++;
        } else {
            s_flush_count++;
            s_sd_buf_pos = 0;
        }
    }
}

// ============================================================
// INICIALIZACIJA
// ============================================================

bool logger_init() {
    if (s_initialized) return true;

    // Alociraj RAM buffer v PSRAM
    s_ram_size = LOG_RAM_BUF_SIZE;
    s_ram_buf  = (char*)ps_malloc(s_ram_size);

    if (!s_ram_buf) {
        // PSRAM ni dostopen — fallback na navadni heap (manjši)
        s_ram_size = 4096;
        s_ram_buf  = (char*)malloc(s_ram_size);
        Serial.printf("[LOGGER][W] PSRAM ni dostopen — RAM buffer %d bytes\n", s_ram_size);
    }

    if (!s_ram_buf) {
        Serial.printf("[LOGGER][E] RAM buffer alokacija NAPAKA — logger brez bufferja!\n");
        s_ram_size = 0;
        // Nadaljujemo brez bufferja — vsaj Serial bo delal
    } else {
        memset(s_ram_buf, 0, s_ram_size);
        Serial.printf("[LOGGER][I] RAM buffer OK — %d bytes %s\n",
                      s_ram_size,
                      (s_ram_buf == (char*)ps_malloc(0) ? "(HEAP)" : "(PSRAM)"));
    }

    s_ram_write_pos = 0;
    s_ram_wrapped   = false;
    s_sd_buf_pos    = 0;
    s_total_lines   = 0;
    s_dropped_lines = 0;
    s_flush_count   = 0;
    s_flush_errors  = 0;
    s_initialized   = true;

    // Logiramo direktno na Serial (logger sam ne more klicati sebe tukaj)
    Serial.printf("[LOGGER][I] logger_init OK — Faza 1 (Serial + RAM)\n");
    return true;
}

void logger_sd_attach() {
    if (!s_initialized) {
        Serial.printf("[LOGGER][E] logger_sd_attach: logger ni inicializiran!\n");
        return;
    }

    // Mutex kreacija — scheduler še ne teče, varno iz bsp_init()
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            Serial.printf("[LOGGER][E] Mutex kreacija NAPAKA!\n");
            return;
        }
    }

    s_sd_attached = sd_mgr_ready();

    if (s_sd_attached) {
        Serial.printf("[LOGGER][I] logger_sd_attach OK — Faza 2 (Serial + RAM + SD)\n");
        // Varnostni flush SD staging bufferja (s_sd_buf).
        // V normalnem zagonu je s_sd_buf_pos == 0 ker sd_buf_write()
        // preskoči vpis dokler !s_sd_attached. Blok je zaščita za
        // morebitne direktne vpise v s_sd_buf pred tem klicem.
        // Opomba: logi iz Faze 1 so v RAM bufferju (s_ram_buf) in
        // dostopni prek logger_get_recent() — na SD jih ne pišemo.
        if (s_sd_buf_pos > 0) {
            size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
            if (written > 0) {
                s_flush_count++;
                s_sd_buf_pos = 0;
                Serial.printf("[LOGGER][I] SD staging buffer flushed: %d bytes\n", written);
            }
        }
    } else {
        Serial.printf("[LOGGER][W] logger_sd_attach: SD ni ready — SD izhod onemogočen\n");
    }
}

void logger_set_ntp_synced(bool synced) {
    s_ntp_synced = synced;
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

    // Level string
    const char* level_str;
    switch (level) {
        case LOG_LEVEL_ERROR: level_str = "ERROR"; break;
        case LOG_LEVEL_WARN:  level_str = "WARN";  break;
        case LOG_LEVEL_INFO:  level_str = "INFO";  break;
        case LOG_LEVEL_DEBUG: level_str = "DEBUG"; break;
        default:              level_str = "?????"; break;
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

    // Sestavi log vrstico
    // Format: "timestamp|PARK|[TAG:LEVEL] message\n"
    char line[MAX_LINE_LEN];
    int len = snprintf(line, sizeof(line),
                       "%s|PARK|[%s:%s] %s\n",
                       timestamp, safe_tag, level_str, message);

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

    // 1. Vedno na Serial
    Serial.print(line);

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

        // SD buffer (flush takoj ob ERROR)
        bool force_flush = (level == LOG_LEVEL_ERROR);
        sd_buf_write(line, (size_t)len, force_flush);

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
        size_t written = sd_mgr_log_flush(s_sd_buf, s_sd_buf_pos);
        if (written > 0) {
            s_flush_count++;
            s_sd_buf_pos = 0;
        } else {
            s_flush_errors++;
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
        strncpy(out, "(RAM buffer ni dostopen)\n", out_len - 1);
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
    static const uint16_t MAX_PTRS = 300;
    const char* line_starts[MAX_PTRS];
    uint16_t    line_lens[MAX_PTRS];
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
    size_t out_pos = 0;
    for (int i = (int)found - 1; i >= 0 && out_pos < out_len - 1; i--) {
        const char* src = line_starts[i];
        uint16_t    ln  = line_lens[i];

        // Preveri ali vrstica ni wrapped čez konec bufferja
        // (za enostavnost: wrappane vrstice preskočimo)
        if (src + ln <= s_ram_buf + s_ram_size) {
            size_t copy = (out_pos + ln < out_len - 1) ? ln : (out_len - 1 - out_pos);
            memcpy(out + out_pos, src, copy);
            out_pos += copy;
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
