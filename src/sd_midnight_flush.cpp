// ============================================================
// sd_midnight_flush.cpp — Nočni flush logov na SD kartico
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ARHITEKTURA (Ideja 1 — SD kot backup medij):
//
//   SD kartica ni več primarni log izhod med normalnim delovanjem.
//   Logi med delovanjem živijo SAMO v PSRAM ring bufferju (logger.cpp).
//   SD_MMC.open() se med normalnim delovanjem NE kliče — s tem so
//   odpravljeni SRAM DMA spike-i ki so blokiral AsyncTCP konekcije.
//
//   Ta modul vsak dan ob 00:01 (po NTP sinhronizaciji) naredi en
//   flush celotnega PSRAM log bufferja na SD kartico.
//   logger_flush() se pokliče samo enkrat na dan — SD_MMC.open()
//   prav tako samo enkrat na dan.
//
// INTEGRACIJA:
//   bsp.cpp: sd_midnight_flush_start() po task kreaciji
//   wifi_manager.cpp: ob NTP sync pokliče sd_midnight_flush_notify_ntp()
//
// SRAM poraba:
//   Task stack: 3072 B v SRAM (mora biti SRAM — kliče SD_MMC prek logger_flush)
//   TCB overhead: ~500 B
//   Skupaj: ~3.6 KB SRAM — enkratna cena za eliminacijo DMA spikeov
//
// ⚠ SRAM STACK — NE PSRAM:
//   logger_flush() → sd_mgr_log_flush() → SD_MMC.open() → flash/DMA ops.
//   Cache-disable med SD dostopom naredi PSRAM nedostopen.
//   Stack mora biti v SRAM (enako kot appTask, wifiTask).
// ============================================================

#include "sd_midnight_flush.h"
#include "logger.h"
#include "sd_mgr.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

static const char* TAG = "SDFLUSH";

// Flag: NTP je sinhroniziran — brez tega ne moremo zanesljivo določiti 00:01
static volatile bool s_ntp_ready = false;

// Prepreči dvojni flush v isti minuti (00:01 traja 60s)
static uint32_t s_last_flush_day = 0;   // YYYYMMDD integer zadnjega flusha

// ============================================================
// INTERNA POMOŽNA
// ============================================================

static uint32_t get_today_yyyymmdd() {
    time_t now = time(nullptr);
    if (now <= 1577836800UL) return 0;   // NTP ni sinhroniziran
    struct tm t;
    localtime_r(&now, &t);
    return (uint32_t)(t.tm_year + 1900) * 10000
         + (uint32_t)(t.tm_mon  + 1)    * 100
         + (uint32_t) t.tm_mday;
}

// ============================================================
// TASK
// ============================================================

static void sd_midnight_flush_task(void* arg) {
    LOG_INFO(TAG, "sd_midnight_flush_task start — čakam NTP sinhronizacijo");

    // Čakaj NTP — brez točnega časa ne moremo vedeti kdaj je 00:01
    while (!s_ntp_ready) {
        vTaskDelay(pdMS_TO_TICKS(10000));   // preveri vsakih 10s
    }

    LOG_INFO(TAG, "NTP OK — midnight flush aktiven (vsak dan ob 00:01)");

    while (true) {
        // Preveri čas vsakih 30s — dovolj natančno za 00:01 okno
        vTaskDelay(pdMS_TO_TICKS(30000));

        if (!s_ntp_ready) continue;

        time_t now = time(nullptr);
        if (now <= 1577836800UL) continue;   // NTP izgubljen

        struct tm t;
        localtime_r(&now, &t);

        // Okno: ura == 0, minuta == 1 (00:01:xx)
        // 30s interval preverjanja zagotavlja da ne zamudimo okna.
        if (t.tm_hour != 0 || t.tm_min != 1) continue;

        // Prepreči večkratni flush v isti minuti
        uint32_t today = get_today_yyyymmdd();
        if (today == s_last_flush_day) continue;

        // --- Flush ---
        LOG_INFO(TAG, "Midnight flush: %04d-%02d-%02d 00:01 — flush logov na SD",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if (!sd_mgr_ready()) {
            LOG_WARN(TAG, "SD ni ready — flush preskočen za danes");
            s_last_flush_day = today;   // označi da smo poskusili — ne ponavljaj danes
            continue;
        }

        // logger_flush() je thread-safe — vzame interni mutex
        // Kliče sd_mgr_log_flush() ki kliče SD_MMC.open() — ENKRAT na dan
        logger_flush();
        s_last_flush_day = today;

        // Cleanup starih log datotek (po defaultu: briše datoteke starejše od SD_MAX_LOG_AGE_DAYS)
        int deleted = sd_mgr_cleanup_old_logs();
        if (deleted > 0) {
            LOG_INFO(TAG, "Midnight cleanup: pobrisano %d starih log datotek", deleted);
        }

        LOG_INFO(TAG, "Midnight flush OK");
    }
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

void sd_midnight_flush_start() {
    // ⚠ Stack MORA biti v SRAM — ne PSRAM (kliče SD_MMC prek logger_flush)
    // Prioriteta 1 — nižja od appTask (2) in wifiTask (3)
    // Core 1 — skupaj z appTask, ne moti WiFi na Core 0
    BaseType_t r = xTaskCreatePinnedToCore(
        sd_midnight_flush_task,
        "SdFlush",
        3072,           // stack v SRAM
        nullptr,
        1,              // prioriteta 1 (najnižja aktivna)
        nullptr,
        1               // Core 1
    );

    if (r != pdPASS) {
        LOG_WARN(TAG, "sd_midnight_flush_task kreacija NAPAKA (%d) — SD flush onemogočen", (int)r);
        // Sistem dela naprej — logi so v PSRAM, SD flush je samo bonus
    } else {
        LOG_INFO(TAG, "sd_midnight_flush_task OK (stack:3072 SRAM Core1 prio:1)");
    }
}

void sd_midnight_flush_notify_ntp() {
    // Kliče wifi_manager.cpp ob NTP sinhronizaciji
    s_ntp_ready = true;
    LOG_INFO(TAG, "NTP sync sporočen — midnight flush bo aktiven ob 00:01");
}
