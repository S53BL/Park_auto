// ============================================================
// sd_midnight_flush.cpp — Hourly SD log flush
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0  |  Datum: 2026-05
// ============================================================
//
// ARCHITECTURE (Ideja 1 — SD as backup medium):
//
//   During normal operation, logs live only in the PSRAM ring buffer.
//   SD_MMC.open() is never called during normal operation — this
//   eliminates SRAM DMA spikes that blocked AsyncTCP connections.
//
//   This task calls logger_dump_to_sd() once per full hour.
//   Each call writes only the new content added since the previous
//   dump (incremental — no duplicate lines).
//
//   File naming: log_YYYYMMDD.txt (sd_mgr.cpp, append mode).
//   One file per day — multiple hourly dumps append to the same file.
//
//   SD cleanup (delete files older than SD_MAX_LOG_AGE_DAYS) runs
//   once per day at 00:00.
//
// INTEGRATION:
//   bsp.cpp: sd_midnight_flush_start() after task creation
//   wifi_manager.cpp: sd_midnight_flush_notify_ntp() on NTP sync
//
// SRAM usage:
//   Task stack: 3072 B SRAM (must be SRAM — calls SD_MMC via logger_dump_to_sd)
//   TCB overhead: ~500 B
//   Total: ~3.6 KB SRAM
//
// ⚠ SRAM STACK — NOT PSRAM:
//   logger_dump_to_sd() → sd_mgr_log_flush() → SD_MMC.open() → DMA ops.
//   Cache-disable during SD access makes PSRAM inaccessible.
//   Stack must be in SRAM (same as appTask, wifiTask).
// ============================================================

#include "sd_midnight_flush.h"
#include "logger.h"
#include "sd_mgr.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

static const char* TAG = "SDMGR";

// NTP flag — without valid time we cannot determine full-hour boundaries
static volatile bool s_ntp_ready = false;

// Track the last flushed hour as YYYYMMDDHH integer to prevent double flush
static uint32_t s_last_flush_yyyymmddhh = 0;

// ============================================================
// INTERNAL HELPERS
// ============================================================

static uint32_t get_yyyymmddhh() {
    time_t now = time(nullptr);
    if (now <= 1577836800UL) return 0;   // NTP not synced
    struct tm t;
    localtime_r(&now, &t);
    return (uint32_t)(t.tm_year + 1900) * 1000000UL
         + (uint32_t)(t.tm_mon  + 1)    * 10000UL
         + (uint32_t) t.tm_mday         * 100UL
         + (uint32_t) t.tm_hour;
}

static uint32_t get_today_yyyymmdd() {
    time_t now = time(nullptr);
    if (now <= 1577836800UL) return 0;
    struct tm t;
    localtime_r(&now, &t);
    return (uint32_t)(t.tm_year + 1900) * 10000UL
         + (uint32_t)(t.tm_mon  + 1)    * 100UL
         + (uint32_t) t.tm_mday;
}

// ============================================================
// TASK
// ============================================================

static void sd_midnight_flush_task(void* arg) {
    LOG_INFO(TAG, "sd_flush_task start — waiting for NTP sync");

    while (!s_ntp_ready) {
        vTaskDelay(pdMS_TO_TICKS(10000));   // check every 10s
    }

    LOG_INFO(TAG, "NTP OK — hourly SD flush active (every full hour)");

    while (true) {
        // Check every 30s — enough precision for a full-hour boundary
        vTaskDelay(pdMS_TO_TICKS(30000));

        if (!s_ntp_ready) continue;

        time_t now = time(nullptr);
        if (now <= 1577836800UL) continue;   // NTP lost

        struct tm t;
        localtime_r(&now, &t);

        // Trigger at minute == 0 (any full hour: 01:00, 02:00, ... 23:00, 00:00)
        if (t.tm_min != 0) continue;

        // Prevent double flush within the same hour
        uint32_t now_hh = get_yyyymmddhh();
        if (now_hh == s_last_flush_yyyymmddhh) continue;

        // --- Flush new log content to SD ---
        LOG_INFO(TAG, "Hourly flush: %04d-%02d-%02d %02d:00",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);

        if (!sd_mgr_ready()) {
            LOG_WARN(TAG, "SD not ready — flush skipped for this hour");
            s_last_flush_yyyymmddhh = now_hh;   // don't retry this hour
            continue;
        }

        size_t written = logger_dump_to_sd();
        s_last_flush_yyyymmddhh = now_hh;

        if (written > 0) {
            LOG_INFO(TAG, "Hourly flush OK — %u B written", (unsigned)written);
        } else {
            LOG_INFO(TAG, "Hourly flush — nothing new to write");
        }

        // SD cleanup — only once per day at 00:00
        if (t.tm_hour == 0) {
            int deleted = sd_mgr_cleanup_old_logs();
            if (deleted > 0) {
                LOG_INFO(TAG, "Daily cleanup: removed %d old log files", deleted);
            }
        }
    }
}

// ============================================================
// PUBLIC API
// ============================================================

void sd_midnight_flush_start() {
    // ⚠ Stack MUST be in SRAM — not PSRAM (calls SD_MMC via logger_dump_to_sd)
    // Priority 1 — lower than appTask (2) and wifiTask (3)
    // Core 1 — same as appTask, does not interfere with WiFi on Core 0
    BaseType_t r = xTaskCreatePinnedToCore(
        sd_midnight_flush_task,
        "SdFlush",
        3072,           // SRAM stack
        nullptr,
        1,              // priority 1 (lowest active)
        nullptr,
        1               // Core 1
    );

    if (r != pdPASS) {
        LOG_WARN(TAG, "sd_flush_task create failed (%d) — hourly flush disabled", (int)r);
    } else {
        LOG_INFO(TAG, "sd_flush_task OK (stack:3072 SRAM Core1 prio:1)");
    }
}

void sd_midnight_flush_notify_ntp() {
    s_ntp_ready = true;
    LOG_INFO(TAG, "NTP sync notified — hourly flush will activate at next full hour");
}
