// ============================================================
// parking_log.h — CSV log parkirnih dogodkov na SD
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// Subscribira na EventBus evente (VEHICLE_RECOGNIZED, VEHICLE_NEW_MODEL,
// VEHICLE_DEPARTED, PARKING_SCAN_ABORTED) in zapisuje CSV vrstice v
// /parking/parking_log.csv na SD kartici.
//
// CSV header:
//   timestamp_iso,event,parking_id,model_id,vehicle_name,dtw_distance,
//   second_best_dtw,second_best_model_id,profile_points_raw,scan_duration_ms,
//   is_new_model,is_aborted
//
// ============================================================

#pragma once
#include <Arduino.h>

// Inicializacija — kliči po vehicle_recog_init() v appTask/bsp.
// Kreira /parking/ mapo in CSV datoteko (doda header če ne obstaja).
// Subscribira na EventBus. Vrne true ob uspehu.
bool parking_log_init(void);

// Periodicni flush — kliči vsakih ~10 s iz appTask zanke.
void parking_log_tick(void);

// Takojšen flush RAM bufferja na SD (kliči ob napakah ali pred rebootom).
void parking_log_flush(void);
