// ============================================================
// sd_midnight_flush.h — Hourly SD log flush
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0  |  Datum: 2026-05
// ============================================================
#pragma once

// Starts the background task that dumps new log content to SD every full hour.
// One file per day (log_YYYYMMDD.txt, append mode).
// Called from bsp.cpp after task creation.
// ⚠ Task stack is in SRAM (not PSRAM) — calls SD_MMC via logger_dump_to_sd().
void sd_midnight_flush_start();

// Notify the task that NTP is synced — required for hour-boundary detection.
// Called by wifi_manager.cpp on successful NTP sync.
void sd_midnight_flush_notify_ntp();
