// ============================================================
// sensor_mgr.h
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Faza 0 — stub
// ============================================================
#pragma once
#include <Arduino.h>
#include "hal_tof.h"

bool sensor_mgr_init();
bool sensor_mgr_ok();
void sensorTask(void* pvParams);

// Sinhrono preberi H, P1 in P2 za parkirno mesto (id='A' ali 'B').
// Kliče hal_tof_readAll() z Wire1 mutexom.
// Vrne true ob uspehu; vrednosti niso veljavne če vrne false.
bool sensor_mgr_read_place_now(char id, uint16_t* h, uint16_t* p1, uint16_t* p2);
