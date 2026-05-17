// ============================================================
// sd_mgr.cpp — SD Kartica Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// SD kartica: vgrajena na Waveshare ESP32-S3-Touch-LCD-3.5B
//   MISO = IO9  (SD_MMC DATA0)
//   MOSI = IO10 (SD_MMC CMD)
//   SCLK = IO11 (SD_MMC CLK)
//   CS   = ni (SD_MMC 1-bit mode ne potrebuje CS)
//
// SD_MMC vs SD knjižnica:
//   Waveshare plošča ima SD slot na MMC prehodu (ne SPI).
//   Zato SD_MMC.h, ne SD.h. Oba imata kompatibilen File API.
//
// 1-bit vs 4-bit SD_MMC:
//   Waveshare ekspoze samo DATA0 (IO9) — 1-bit mode.
//   4-bit mode bi zahteval IO9/IO10/IO11/IO12, a IO12 je LCD CS.
//   1-bit mode je dovolj za logiranje (~500 KB/s).
//
// INICIALIZACIJA V BSP:
//   bsp.cpp kliče sd_mgr_init() v bsp_sd_init() PRED task kreacijo.
//   Razlog: logger.cpp mora vedeti ali je SD ready preden logira.
//   Vrstni red: Serial → I2C → GPIO → MCP → SD → TWDT → Tasks.
//
// ============================================================

#include "sd_mgr.h"
#include "config.h"
#include <SD_MMC.h>
#include <freertos/semphr.h>
#include <time.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>

// ============================================================
// LOGGING — direkten Serial (logger.cpp še ni inicializiran ob sd_mgr_init())
// ============================================================

#define SD_I(fmt, ...) Serial.printf("[SD_MGR][I] " fmt "\n", ##__VA_ARGS__)
#define SD_W(fmt, ...) Serial.printf("[SD_MGR][W] " fmt "\n", ##__VA_ARGS__)
#define SD_E(fmt, ...) Serial.printf("[SD_MGR][E] " fmt "\n", ##__VA_ARGS__)
#define SD_D(fmt, ...) Serial.printf("[SD_MGR][D] " fmt "\n", ##__VA_ARGS__)

// ============================================================
// INTERNO STANJE
// ============================================================

static SemaphoreHandle_t s_mutex     = nullptr;
static bool              s_ready     = false;
static char              s_status[48] = "not initialized";

// ============================================================
// POMOŽNE FUNKCIJE — INTERNE
// ============================================================

// Vzame mutex z timeoutom — vrne false če ni dobil v času
static bool take_mutex(uint32_t timeout_ms = 200) {
    if (!s_mutex) return false;
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void give_mutex() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Kreira mapo če ne obstaja (SD_MMC.mkdir vrne false če že obstaja — to je OK)
static void ensure_dir(const char* path) {
    if (!SD_MMC.exists(path)) {
        if (SD_MMC.mkdir(path)) {
            SD_I("Mapa kreirana: %s", path);
        } else {
            SD_W("Mapa kreacija napaka: %s (morda že obstaja)", path);
        }
    }
}

// Sestavi ime dnevne log datoteke glede na NTP čas ali millis()
// Format: /logs/log_YYYYMMDD.txt
// Ob brez NTP: /logs/log_nodate.txt (vse gre v eno datoteko do sinhronizacije)
static void build_log_filename(char* out, size_t out_len) {
    time_t now = time(nullptr);
    if (now > 1577836800UL) {   // 2020-01-01 = NTP je sinhroniziran
        struct tm t;
        localtime_r(&now, &t);
        snprintf(out, out_len, "%slog_%04d%02d%02d.txt",
                 SD_LOG_PATH,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    } else {
        snprintf(out, out_len, "%slog_nodate.txt", SD_LOG_PATH);
    }
}

// Izvleče datum iz imena datoteke formata "log_YYYYMMDD.txt"
// Vrne integer YYYYMMDD ali 0 če format ni pravilen
static uint32_t extract_date_from_filename(const char* name) {
    // Pričakovano: "log_YYYYMMDD.txt" (16 znakov)
    if (strlen(name) != 16) return 0;
    if (strncmp(name, "log_", 4) != 0) return 0;
    if (strcmp(name + 12, ".txt") != 0) return 0;

    char date_str[9];
    strncpy(date_str, name + 4, 8);
    date_str[8] = '\0';

    // Preveri da so samo cifre
    for (int i = 0; i < 8; i++) {
        if (!isdigit(date_str[i])) return 0;
    }
    return (uint32_t)atol(date_str);
}

// Izračuna cutoff datum (YYYYMMDD integer) za N dni nazaj
static uint32_t cutoff_date(int days_ago) {
    time_t now   = time(nullptr);
    time_t cutoff = now - ((time_t)days_ago * 86400UL);
    struct tm t;
    localtime_r(&cutoff, &t);
    return (uint32_t)(t.tm_year + 1900) * 10000
         + (uint32_t)(t.tm_mon  + 1)    * 100
         + (uint32_t) t.tm_mday;
}

// ============================================================
// INICIALIZACIJA
// ============================================================

bool sd_mgr_init() {
    SD_I("SD_MMC init (1-bit mode, IO9/IO10/IO11)...");

    // Mutex kreacija
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        SD_E("Mutex kreacija napaka!");
        snprintf(s_status, sizeof(s_status), "ERR mutex failed");
        return false;
    }

    // arduino-esp32 v3.x zahteva eksplicitni setPins() pred begin().
    // 1-bit mode: CLK=IO11, CMD=IO10, D0=IO9
    // IO12 je LCD CS — 4-bit mode ni možen na tej plošči.
    SD_MMC.setPins(11, 10, 9);  // CLK, CMD, D0

    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) {
        SD_E("SD_MMC.begin() napaka — kartica ni vstavljena ali napaka montiranja");
        snprintf(s_status, sizeof(s_status), "ERR not mounted");
        s_ready = false;
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        SD_E("SD_MMC: nobena kartica ni zaznana");
        snprintf(s_status, sizeof(s_status), "ERR no card");
        s_ready = false;
        return false;
    }

    const char* type_str = "UNKNOWN";
    switch (cardType) {
        case CARD_MMC:  type_str = "MMC";   break;
        case CARD_SD:   type_str = "SD";    break;
        case CARD_SDHC: type_str = "SDHC";  break;
        default: break;
    }

    uint64_t total_mb = SD_MMC.totalBytes() / (1024ULL * 1024ULL);
    uint64_t free_mb  = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL);

    SD_I("SD_MMC OK — tip: %s, skupaj: %llu MB, prosto: %llu MB", type_str, total_mb, free_mb);

    // Kreacija potrebnih map
    ensure_dir(SD_LOG_PATH);
    ensure_dir(SD_RAW_PATH);
    ensure_dir("/models");
    ensure_dir("/raw/parkingA");
    ensure_dir("/raw/parkingB");

    s_ready = true;
    snprintf(s_status, sizeof(s_status), "OK %llu MB free", free_mb);

    SD_I("sd_mgr_init OK — status: %s", s_status);
    return true;
}

bool sd_mgr_ready() { return s_ready; }

uint64_t sd_mgr_free_bytes() {
    if (!s_ready) return 0;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

uint64_t sd_mgr_total_bytes() {
    if (!s_ready) return 0;
    return SD_MMC.totalBytes();
}

const char* sd_mgr_status_str() { return s_status; }

// ============================================================
// PISANJE LOG DATOTEK
// ============================================================

bool sd_mgr_log_write(const char* line) {
    if (!s_ready || !line || strlen(line) == 0) return false;
    if (!take_mutex()) {
        SD_W("log_write: mutex timeout — log izgubljen");
        return false;
    }

    char filename[64];
    build_log_filename(filename, sizeof(filename));

    File f = SD_MMC.open(filename, FILE_APPEND);
    if (!f) {
        SD_W("log_write: ne morem odpreti %s", filename);
        give_mutex();
        return false;
    }

    size_t written = f.print(line);
    f.close();
    give_mutex();

    if (written != strlen(line)) {
        SD_W("log_write: nepopoln zapis (%d/%d bajtov)", written, strlen(line));
        return false;
    }
    return true;
}

size_t sd_mgr_log_flush(const char* buf, size_t len) {
    if (!s_ready || !buf || len == 0) return 0;
    if (!take_mutex(500)) {
        // Daljši timeout za flush — je pomembna operacija
        SD_W("log_flush: mutex timeout (%d bajtov izgubljenih)", len);
        return 0;
    }

    char filename[64];
    build_log_filename(filename, sizeof(filename));

    File f = SD_MMC.open(filename, FILE_APPEND);
    if (!f) {
        SD_E("log_flush: ne morem odpreti %s", filename);

        // Posodobi status
        uint64_t free_mb = sd_mgr_free_bytes() / (1024ULL * 1024ULL);
        if (free_mb < 10) {
            snprintf(s_status, sizeof(s_status), "WARN <10 MB free");
            SD_W("SD kartica skoraj polna! Prosto: %llu MB", free_mb);
        }

        give_mutex();
        return 0;
    }

    // Pisanje v enem kosu — vrnjeno na original.
    // Razlog: vTaskDelay(1) med sektorji je sproščal scheduler med držanjem
    //   sd_mgr mutexa kar je povzročalo nepredvidljivo vedenje.
    //   Pravi fix za SD DMA spike je bil odpraviti logger_flush() iz wifiTask.
    size_t written = f.write((const uint8_t*)buf, len);
    f.close();

    SD_D("log_flush: %d/%d bajtov v %s", written, len, filename);

    // Periodično osveži status z aktualno prosto kapaciteto
    uint64_t free_mb = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL);
    snprintf(s_status, sizeof(s_status), "OK %llu MB free", free_mb);

    give_mutex();
    return written;
}

// ============================================================
// CLEANUP STARIH LOG DATOTEK
// ============================================================

int sd_mgr_cleanup_old_logs() {
    // Cleanup zahteva NTP sinhronizacijo za zanesljive datume
    time_t now = time(nullptr);
    if (now <= 1577836800UL) {
        SD_W("cleanup_old_logs: NTP ni sinhroniziran — preskočeno");
        return 0;
    }

    if (!take_mutex(1000)) {
        SD_W("cleanup_old_logs: mutex timeout");
        return 0;
    }

    uint32_t cutoff = cutoff_date(SD_MAX_LOG_AGE_DAYS);
    SD_I("cleanup_old_logs: brišem datoteke pred %u (%d dni)", cutoff, SD_MAX_LOG_AGE_DAYS);

    File root = SD_MMC.open(SD_LOG_PATH);
    if (!root || !root.isDirectory()) {
        SD_E("cleanup_old_logs: ne morem odpreti %s", SD_LOG_PATH);
        give_mutex();
        return 0;
    }

    int deleted = 0;
    int checked = 0;

    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            checked++;

            uint32_t file_date = extract_date_from_filename(name);
            if (file_date > 0 && file_date < cutoff) {
                // Sestavi polno pot
                char full_path[128];
                snprintf(full_path, sizeof(full_path), "%s%s", SD_LOG_PATH, name);
                entry.close();

                if (SD_MMC.remove(full_path)) {
                    SD_I("cleanup: pobrisano %s (datum %u < cutoff %u)",
                         full_path, file_date, cutoff);
                    deleted++;
                } else {
                    SD_W("cleanup: napaka brisanja %s", full_path);
                }
            } else {
                entry.close();
            }
        } else {
            entry.close();
        }
        entry = root.openNextFile();
    }
    root.close();
    give_mutex();

    SD_I("cleanup_old_logs: preveril %d, pobrisal %d datotek", checked, deleted);
    return deleted;
}

// ============================================================
// FILE LISTING — za web_ui.cpp /api/files endpoint
// ============================================================

int sd_mgr_list_files(const char* path, SdFileInfo* out, int max_cnt) {
    if (!s_ready || !out || max_cnt <= 0) return 0;
    if (!take_mutex()) return 0;

    File root = SD_MMC.open(path);
    if (!root || !root.isDirectory()) {
        SD_W("list_files: ne morem odpreti mape %s", path);
        give_mutex();
        return 0;
    }

    int cnt = 0;
    File entry = root.openNextFile();
    while (entry && cnt < max_cnt) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();

            strncpy(out[cnt].name, name, sizeof(out[cnt].name) - 1);
            out[cnt].name[sizeof(out[cnt].name) - 1] = '\0';

            snprintf(out[cnt].path, sizeof(out[cnt].path), "%s%s", path, name);

            out[cnt].size_bytes = (uint32_t)entry.size();

            // Datum iz imena (log_YYYYMMDD.txt) ali "unknown"
            uint32_t d = extract_date_from_filename(name);
            if (d > 0) {
                snprintf(out[cnt].date, sizeof(out[cnt].date), "%04u-%02u-%02u",
                         d / 10000, (d / 100) % 100, d % 100);
            } else {
                strncpy(out[cnt].date, "unknown", sizeof(out[cnt].date));
            }

            cnt++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    give_mutex();

    SD_D("list_files(%s): %d datotek", path, cnt);
    return cnt;
}

int32_t sd_mgr_file_size(const char* path) {
    if (!s_ready || !path) return -1;
    if (!take_mutex()) return -1;

    File f = SD_MMC.open(path, FILE_READ);
    int32_t sz = -1;
    if (f) {
        sz = (int32_t)f.size();
        f.close();
    }
    give_mutex();
    return sz;
}

File sd_mgr_open_file(const char* path) {
    // ⚠ Caller mora klicati file.close() po uporabi!
    // ⚠ Mutex NI vzet tukaj — streaming web response bi držal mutex predolgo.
    //    web_ui.cpp mora zagotoviti da ne gre vzporedno sd_mgr_log_flush().
    //    Priporočilo: streaming samo za /files endpoint, ne med aktivnim loggingom.
    if (!s_ready || !path) return File();
    return SD_MMC.open(path, FILE_READ);
}

bool sd_mgr_delete_file(const char* path) {
    if (!s_ready || !path) return false;
    if (!take_mutex()) return false;

    bool ok = SD_MMC.remove(path);
    SD_I("delete_file(%s): %s", path, ok ? "OK" : "NAPAKA");
    give_mutex();
    return ok;
}

// ============================================================
// RAW TOF PROFILI
// ============================================================

bool sd_mgr_save_raw_profile(const char* path, const char* data) {
    if (!s_ready || !path || !data) return false;
    if (!take_mutex(500)) return false;

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        SD_E("save_raw_profile: ne morem odpreti %s", path);
        give_mutex();
        return false;
    }

    size_t len     = strlen(data);
    size_t written = f.print(data);
    f.close();
    give_mutex();

    if (written != len) {
        SD_W("save_raw_profile: nepopoln zapis %s (%d/%d)", path, written, len);
        return false;
    }
    SD_D("save_raw_profile: %s OK (%d bajtov)", path, written);
    return true;
}

int sd_mgr_count_files(const char* path) {
    if (!s_ready || !path) return 0;
    if (!take_mutex()) return 0;

    File root = SD_MMC.open(path);
    if (!root || !root.isDirectory()) {
        give_mutex();
        return 0;
    }

    int cnt = 0;
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) cnt++;
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    give_mutex();
    return cnt;
}

bool sd_mgr_append_file(const char* path, const char* data, size_t len) {
    if (!s_ready || !path || !data || len == 0) return false;
    if (!take_mutex(500)) return false;

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) {
        SD_E("append_file: ne morem odpreti %s", path);
        give_mutex();
        return false;
    }
    size_t written = f.write((const uint8_t*)data, len);
    f.close();
    give_mutex();

    if (written != len) {
        SD_W("append_file: nepopoln zapis %s (%d/%d)", path, written, len);
        return false;
    }
    return true;
}

bool sd_mgr_ensure_dir(const char* path) {
    if (!s_ready || !path || path[0] != '/') return false;
    if (!take_mutex(500)) return false;

    // Preberi pot segment po segment in kreiraj vsak
    char buf[128];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    bool ok = true;
    for (int i = 1; buf[i] != '\0'; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (!SD_MMC.exists(buf)) {
                if (!SD_MMC.mkdir(buf)) {
                    SD_W("ensure_dir: mkdir('%s') napaka", buf);
                    ok = false;
                    break;
                }
            }
            buf[i] = '/';
        }
    }
    if (ok && !SD_MMC.exists(path)) {
        ok = SD_MMC.mkdir(path);
        if (!ok) SD_W("ensure_dir: mkdir('%s') napaka", path);
    }
    give_mutex();
    return ok;
}

int sd_mgr_keep_newest_n(const char* path, uint16_t n) {
    if (!s_ready || !path || n == 0) return 0;
    if (!take_mutex(500)) return 0;

    // Poberi imena vseh datotek v mapi (max 200) — buffer v PSRAM
    static const int NAMES_MAX = 200;
    static const int NAME_LEN  = 64;
    char* names_buf = (char*)heap_caps_malloc((size_t)NAMES_MAX * NAME_LEN, MALLOC_CAP_SPIRAM);
    if (!names_buf) names_buf = (char*)malloc((size_t)NAMES_MAX * NAME_LEN);
    if (!names_buf) {
        give_mutex();
        return 0;
    }
    // Makro za dostop do names_buf kot 2D array
    #define NAME(i) (names_buf + (i) * NAME_LEN)

    int total = 0;

    File root = SD_MMC.open(path);
    if (!root || !root.isDirectory()) {
        heap_caps_free(names_buf);
        give_mutex();
        return 0;
    }

    File entry = root.openNextFile();
    while (entry && total < NAMES_MAX) {
        if (!entry.isDirectory()) {
            strncpy(NAME(total), entry.name(), NAME_LEN - 1);
            NAME(total)[NAME_LEN - 1] = '\0';
            total++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    if (total <= (int)n) {
        heap_caps_free(names_buf);
        give_mutex();
        return 0;
    }

    // Sortiraj abecedno (=kronološko ker format YYYYMMDD_HHMMSS_)
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - i - 1; j++) {
            if (strcmp(NAME(j), NAME(j + 1)) > 0) {
                char tmp[NAME_LEN];
                strncpy(tmp,      NAME(j),     NAME_LEN - 1);
                strncpy(NAME(j),  NAME(j + 1), NAME_LEN - 1);
                strncpy(NAME(j + 1), tmp,      NAME_LEN - 1);
            }
        }
    }

    // Pobriši najstarejše (prvih total-n)
    int del_count = total - (int)n;
    int deleted = 0;
    char full[192];
    for (int i = 0; i < del_count; i++) {
        snprintf(full, sizeof(full), "%s/%s", path, NAME(i));
        if (SD_MMC.remove(full)) {
            deleted++;
            SD_D("keep_newest_n: pobrisal %s", full);
        } else {
            SD_W("keep_newest_n: napaka brisanja %s", full);
        }
    }

    #undef NAME
    heap_caps_free(names_buf);
    give_mutex();
    return deleted;
}
