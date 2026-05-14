// ============================================================
// sd_mgr.h — SD Kartica Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// ODGOVORNOST:
//   Enkapsulira vse operacije z SD kartico na Waveshare plošči.
//   SD je vgrajena na IO9 (MISO) / IO10 (MOSI) / IO11 (SCLK) — SD_MMC.
//
//   Ta modul pokriva:
//     - Inicializacija SD_MMC (kliče bsp.cpp ob zagonu)
//     - Kreacija map (/logs/, /raw/, /models/)
//     - Atomarno pisanje v log datoteke z dnevno rotacijo
//     - Flush PSRAM bufferja na SD (kliče logger.cpp)
//     - Cleanup starih log datotek (> SD_MAX_LOG_AGE_DAYS)
//     - File listing za web endpoint /files
//     - Download posamezne datoteke (streaming za web_ui.cpp)
//     - Zdravstveni status (dostopnost, prosta kapaciteta)
//
// NITNA VARNOST:
//   Vse funkcije so thread-safe prek internega FreeRTOS mutexa.
//   Kličejo jih: logger.cpp (sensorTask/appTask), web_ui.cpp (wifiTask).
//   SD_MMC ni thread-safe sam po sebi — mutex je obvezen.
//
// INICIALIZACIJA:
//   sd_mgr_init() kliče bsp.cpp v bsp_sd_init() PRED task kreacijo.
//   Po inicializaciji vrne sd_mgr_ready() = true.
//   Vse write funkcije ob !ready() tiho vrnejo false — ne crashajo.
//
// ODVISNOSTI:
//   config.h   — SD_LOG_PATH, SD_MAX_LOG_AGE_DAYS, LOG_SD_PATH
//   SD_MMC.h   — ESP32 Arduino framework (vgrajen)
//   time.h     — za NTP timestamp v imenih datotek
//
// ============================================================

#pragma once

#include <Arduino.h>
#include <stdint.h>

// ============================================================
// INICIALIZACIJA IN STATUS
// ============================================================

// Inicializacija SD_MMC — kliče bsp.cpp PRED task kreacijo.
// Vrne true = SD montirana in mape kreirane.
// Vrne false = SD ni prisotna / napaka montiranja (sistem dela naprej brez SD).
bool sd_mgr_init();

// true = SD je montirana in funkcionalna
bool sd_mgr_ready();

// Prosta kapaciteta v bajtih (0 če SD ni ready)
uint64_t sd_mgr_free_bytes();

// Skupna kapaciteta v bajtih (0 če SD ni ready)
uint64_t sd_mgr_total_bytes();

// Vrne string s statusom za /api/status endpoint
// Format: "OK 1234 MB free" ali "ERR not mounted"
const char* sd_mgr_status_str();

// ============================================================
// PISANJE LOG DATOTEK
// ============================================================

// Zapiše en log string v dnevno datoteko (npr. /logs/log_20260415.txt).
// Ime datoteke določi sam glede na NTP čas ali millis() če NTP ni sinhroniziran.
// Thread-safe — vzame interni mutex.
// Vrne true = uspešno zapisano.
bool sd_mgr_log_write(const char* line);

// Zapiše blok podatkov (RAM buffer flush iz logger.cpp).
// buf     = pointer na buffer z log vrsticami
// len     = dolžina v bajtih
// Vrne število zapisanih bajtov (0 = napaka).
size_t sd_mgr_log_flush(const char* buf, size_t len);

// ============================================================
// CLEANUP STARIH LOG DATOTEK
// ============================================================

// Pobriše log datoteke starejše od SD_MAX_LOG_AGE_DAYS dni.
// Kliče logger.cpp ob vsakem dnevu ob polnoči (NTP sinhroniziran).
// Vrne število pobrisanih datotek.
int sd_mgr_cleanup_old_logs();

// ============================================================
// DATOTEČNE OPERACIJE — za web_ui.cpp
// ============================================================

// Struktura za opis datoteke (za /api/files endpoint)
struct SdFileInfo {
    char     name[64];      // ime datoteke (brez poti)
    char     path[128];     // polna pot
    uint32_t size_bytes;    // velikost
    char     date[16];      // datum iz imena datoteke ali "unknown"
};

// Napolni array z vsebino mape.
// path    = npr. "/logs/" ali "/raw/"
// out     = caller-alocirani array
// max_cnt = max elementov v out[]
// Vrne dejansko število elementov (≤ max_cnt).
int sd_mgr_list_files(const char* path, SdFileInfo* out, int max_cnt);

// Vrne velikost datoteke v bajtih (-1 = ne obstaja / napaka)
int32_t sd_mgr_file_size(const char* path);

// Odpre datoteko za streaming (za web_ui.cpp chunked response).
// Vrne File objekt — caller je odgovoren za file.close().
// Vrne neveljavno File (!) če ni ready ali datoteka ne obstaja.
#include <SD_MMC.h>
File sd_mgr_open_file(const char* path);

// Pobriše datoteko. Vrne true = uspešno.
bool sd_mgr_delete_file(const char* path);

// ============================================================
// RAW TOF PROFILI — za vehicle_recog.cpp
// ============================================================

// Shrani raw TOF profil v JSON datoteko.
// path    = npr. "/raw/parkingA/profile_001.json"
// data    = JSON string z raw TOF meritvami
// Vrne true = uspešno zapisano.
bool sd_mgr_save_raw_profile(const char* path, const char* data);

// Prešteje datoteke v mapi (za vehicle_recog.cpp — max raw profilov)
int sd_mgr_count_files(const char* path);

// Doda podatke (append) na konec datoteke. Datoteko kreira če ne obstaja.
// Thread-safe. Vrne true ob uspehu.
bool sd_mgr_append_file(const char* path, const char* data, size_t len);

// Kreira mapo in vse starševske mape (rekurzivno).
// Varno klicati večkrat — vrne true če mapa obstaja ali je bila kreirana.
bool sd_mgr_ensure_dir(const char* path);

// V mapi path obdrži samo zadnjih n datotek (po abecednem vrstnem redu —
// format YYYYMMDD_HHMMSS_ zagotavlja da je abecedni red enak kronološkemu).
// Vse starejše datoteke pobriše. Vrne število pobrisanih datotek.
int sd_mgr_keep_newest_n(const char* path, uint16_t n);
