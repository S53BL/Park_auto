// ============================================================
// sd_midnight_flush.h — Nočni flush logov na SD kartico
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
#pragma once

// Zažene background task ki vsak dan ob 00:01 flusira loge na SD.
// Kliče se iz bsp.cpp po task kreaciji in po sd_mgr_init().
// ⚠ Task stack je v SRAM (ne PSRAM) — kliče SD_MMC prek logger_flush().
void sd_midnight_flush_start();

// Obvesti task da je NTP sinhroniziran — brez tega flush ni možen.
// Kliče wifi_manager.cpp ob uspešni NTP sinhronizaciji.
void sd_midnight_flush_notify_ntp();
