// =============================================================================
// vehicle_recog.h — DTW identifikacija vozil
// =============================================================================
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.1.0  |  Datum: 2026-05
//
// ODGOVORNOST:
//   Layer 4 (App). Prejme zaključen TOF profil iz sensor_mgr
//   (ki ga pripravi hal_tof), izvede DTW identifikacijo, upravlja
//   modele vozil in vzdržuje state machine parkirnih mest A in B.
//
// ARHITEKTURA (prilagojena glede na obstoječ hal_tof.cpp):
//   hal_tof.cpp gradi profil interno (Δ-filter, stabilnost timer,
//   finalize_profile) in ga dostavi prek TofProfileCallback.
//   sensor_mgr.cpp sprejme TofProfileResult in pokliče
//   vehicle_recog_feed_profile() — NI feed_sample, NI dvojne
//   state machine. vehicle_recog dobi profil v enem kosu in
//   takoj normalizira + DTW v vehicle_recog_tick().
//
// SINHRONIZACIJA hal_tof ↔ vehicle_recog:
//   1. hal_tof: IDLE → DETECT: hal_tof_startDetect() ob DOOR_OPENED
//   2. hal_tof: DETECT → SCANNING: samodejno ob H < VEH_ENTRY_THRESH_MM
//   3. hal_tof: SCANNING → DTW_WAIT: finalize_profile() → callback
//   4. sensor_mgr: callback → vehicle_recog_feed_profile()
//   5. vehicle_recog: DTW_COMPUTE → rezultat → EventBus publish
//   6. vehicle_recog_tick(): po DTW → hal_tof_stopScan() → hal_tof IDLE
//
// PERSISTENCA:
//   Modeli:   LittleFS /vehicles/A/<id>.json, /vehicles/B/<id>.json
//   Baseline: LittleFS /vehicles/baseline_A.json, baseline_B.json
//   Raw:      SD /raw/parkingA/<id>/YYYYMMDD_HHMMSS_<id>.json (FIFO)
//   Config:   NVS prek config_mgr (vr_dtw_thresh, vr_sc_radius, ...)
//
// ODVISNOSTI:
//   event_bus.h, config_mgr.h, logger.h, sd_mgr.h, hal_tof.h
//   LittleFS, ArduinoJson, PSRAM (ps_malloc/ps_calloc/ps_realloc)
//
// NE ODGOVARJA ZA:
//   - Polling senzorjev (hal_tof.cpp)
//   - Detekcijo odhoda (sensor_mgr.cpp → EVT_VEHICLE_DEPARTED)
//   - CSV log (parking_log.cpp)
//   - LCD prikaz (screen_main.cpp prek EVT_VEHICLE_RECOGNIZED)
//
// Specifikacija: Identifikacija_Vozil_v1.1.docx + dodatek_vehicle_recog.md
// =============================================================================

#pragma once
#include <Arduino.h>
#include "hal_tof.h"   // za TofProfileResult

// =============================================================================
// KONSTANTE ALGORITMA
// =============================================================================

// Dimenzije profila
#define VR_PROFILE_DIMS         3      // H, P1, P2
#define VR_PROFILE_NORM_POINTS  80     // max normalizirana dolzina (hard max)
#define VR_PROFILE_DIM_H        0
#define VR_PROFILE_DIM_P1       1
#define VR_PROFILE_DIM_P2       2

// Privzete vrednosti — vse nastavljivo prek config_mgr (NVS kljuci vr_*)
#define VR_DEFAULT_DTW_THRESHOLD           18.0f
#define VR_DEFAULT_SC_RADIUS               15
#define VR_DEFAULT_MIN_POINTS              25     // pod tem zavrzemo profil
#define VR_DEFAULT_NORM_POINTS             80     // normaliziramo na N tock
#define VR_DEFAULT_RAW_PROFILES_PER_MODEL  30     // FIFO rotacija na SD
#define VR_DEFAULT_PRESENCE_CHECK_MIN      10     // periodicno preverjanje
#define VR_DEFAULT_EMPTY_TOLERANCE_MM      200    // +-mm od baseline

// Fallback za detekcijo praznega brez baseline (dodatek pogl. 2)
#define VR_FALLBACK_EMPTY_THRESHOLD_MM     1500

// Cas dolgega pritiska za kalibracijo (ms) — hardkodirano, dodatek pogl. 11
// 2s = rename (screen_main), 10s = calibrate_empty
#define VR_CALIB_HOLD_MS                   10000

// =============================================================================
// STANJA IN FAZE
// =============================================================================

// Stanja parkirnega mesta (dodatek_vehicle_recog.md pogl. 1)
typedef enum {
    VR_STATE_EMPTY_CALIBRATED   = 0,  // baseline obstaja, H+P1+P2 v toleranci
    VR_STATE_EMPTY_UNCALIBRATED = 1,  // ni baseline, vse > 150 cm
    VR_STATE_OCCUPIED_UNKNOWN   = 2,  // kratke razdalje, brez DTW konteksta
    VR_STATE_OCCUPIED_KNOWN     = 3   // DTW uspel, ime znano
} vr_place_state_t;

// Faze skeniranja — zrcalijo hal_tof faze za namene UI diagnostike
// Realno skeniranje vodi hal_tof, vehicle_recog sledi
typedef enum {
    VR_PHASE_IDLE        = 0,  // mirovanje (hal_tof: IDLE)
    VR_PHASE_DETECT      = 1,  // caka na vstop (hal_tof: DETECT)
    VR_PHASE_SCANNING    = 2,  // gradi profil (hal_tof: SCANNING)
    VR_PHASE_DTW_COMPUTE = 3,  // DTW burst (hal_tof: DTW_WAIT)
    VR_PHASE_PARKED      = 4   // vozilo miruje, nadzor H za odhod
} vr_phase_t;

// =============================================================================
// EVENTBUS PAYLOAD STRUKTURE
// (prek EventBus::publish z (uint32_t) castom na pointer — payload je pointer)
// =============================================================================

// EVT_VEHICLE_RECOGNIZED in EVT_VEHICLE_NEW_MODEL
// (dodatek_vehicle_recog.md pogl. 6.1)
typedef struct {
    char      parkingId;            // 'A' ali 'B'
    char      modelId[16];
    char      name[32];
    float     dtwDistance;          // NAN za novo vozilo
    float     secondBestDtw;        // NAN ce samo en model
    char      secondBestModelId[16];
    uint16_t  profileLength;        // tocke pred normalizacijo
    uint16_t  scanDurationMs;
    uint32_t  repetitions;
    bool      isNewModel;
} VehicleRecognizedEvent_t;

// EVT_VEHICLE_DEPARTED — poslje sensor_mgr, vehicle_recog posluša
typedef struct {
    char      parkingId;
    char      modelId[16];          // prazno ce neznano
    char      name[32];             // prazno ce neznano
    uint32_t  park_duration_min;    // 0 ce neznano
} VehicleDepartedEvent_t;

// EVT_PARKING_SCAN_ABORTED — samo Logger (dodatek pogl. 6.2)
typedef struct {
    char      parkingId;
    uint16_t  profileLength;        // koliko tock je bilo zajetih
    uint16_t  scanDurationMs;
    char      reason[32];           // "too_few_points", "dtw_failed" ...
    bool      is_aborted;           // vedno true, za parking_log CSV
} ParkingScanAbortedEvent_t;

// EVT_PARKING_PLACE_CALIBRATED — hal_display, Logger
typedef struct {
    char      parkingId;
    uint16_t  h_mm;
    uint16_t  p1_mm;
    uint16_t  p2_mm;
} ParkingPlaceCalibratedEvent_t;

// =============================================================================
// PERSISTENCNE STRUKTURE (public API)
// =============================================================================

// Baseline praznega mesta (LittleFS)
typedef struct {
    bool      valid;
    uint16_t  h_mm;
    uint16_t  p1_mm;
    uint16_t  p2_mm;
    uint32_t  calibrated_at;  // unix timestamp
} vr_baseline_t;

// Povzetek modela — za web /api/vehicles in LCD (brez Welford state)
typedef struct {
    char      id[16];
    char      name[32];
    uint32_t  repetitions;
    uint32_t  lastSeen;           // unix timestamp
    float     lastDtwDistance;    // NAN dokler ni vsaj 2 parkiranj
} vr_model_summary_t;

// Diagnostika za servisni LCD in web /api/status
typedef struct {
    uint16_t  models_A;
    uint16_t  models_B;
    bool      baseline_A_valid;
    bool      baseline_B_valid;
    vr_place_state_t  state_A;
    vr_place_state_t  state_B;
    vr_phase_t        phase_A;
    vr_phase_t        phase_B;
    uint32_t  total_recognitions;
    uint32_t  total_aborted;
    uint32_t  total_new_models;
} vr_diagnostics_t;

// =============================================================================
// PUBLIC API
// =============================================================================

// ---- Inicializacija ----

// Klicati iz appTask po hal_tof_init() in config_mgr_init().
// - Subscribe na EventBus (EVT_DOOR_OPENED, EVT_VEHICLE_DEPARTED,
//   EVT_BUTTON_EDIT_VEHICLE_A/B)
// - Alocira modele array in DTW matriko v PSRAM
// - Nalozi baseline za A in B iz LittleFS
bool vehicle_recog_init(void);

// Klicati iz appTask zanke (vsakih ~100 ms).
// - Ce je profil v cakanju (po feed_profile): izvede DTW burst
// - Po uspesnem DTW: pokliče hal_tof_stopScan()
// - Vzdrzuje diagnosticne stevce
void vehicle_recog_tick(void);

// ---- Sprejem profila ----

// Klicati iz sensor_mgr TOF profil callbacka.
// Prejme zaključen TofProfileResult, kopira tocke v interni buffer,
// nastavi fazo na VR_PHASE_DTW_COMPUTE — DTW se izvede v naslednji
// vehicle_recog_tick() (Core1, appTask kontekst).
//
// NE klicati iz ISR ali eventBusTask — samo sensorTask ali appTask.
// Podatki se kopirajo — klicatelj lahko takoj osvobodi profil.
void vehicle_recog_feed_profile(const TofProfileResult& profile);

// ---- Stanje ----

vr_place_state_t vehicle_recog_get_state(char parkingId);
vr_phase_t       vehicle_recog_get_phase(char parkingId);
const char*      vehicle_recog_get_vehicle_name(char parkingId);
const char*      vehicle_recog_get_model_id(char parkingId);
float            vehicle_recog_get_last_dtw(char parkingId);

// ---- Modeli ----

// Stevilo modelov (po lazy load). Sprozí load ce ni se naloženo.
uint16_t vehicle_recog_get_model_count(char parkingId);

// Pridobi summary modela po indeksu (za web /api/vehicles).
bool vehicle_recog_get_model_summary(char parkingId, uint16_t index,
                                     vr_model_summary_t* out);

// Pridobi normaliziran profil modela (za web podrobnosti).
// out_data in out_variance: caller alocira [VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS].
bool vehicle_recog_get_model_profile(char parkingId, const char* modelId,
                                     float (*out_data)[VR_PROFILE_DIMS],
                                     float (*out_variance)[VR_PROFILE_DIMS],
                                     uint16_t* out_length);

// Preimenuj model — kliče screen_main (LVGL KB ok callback) ali web.
// Async write v LittleFS (v ticku).
bool vehicle_recog_rename_model(char parkingId, const char* modelId,
                                const char* newName);

// Izbrisi model iz RAM in LittleFS. Raw profili ostanejo na SD.
bool vehicle_recog_delete_model(char parkingId, const char* modelId);

// ---- Kalibracija ----

// Kliče screen_main po 10s pritisku + DA potrditvi.
// Izmeri H/P1/P2 prek sensor_mgr_read_place_now() in shrani baseline.
// Blokirano ce: stanje ni EMPTY_* ali faza ni IDLE/PARKED.
// Pošlje EVT_PARKING_PLACE_CALIBRATED ob uspehu.
bool vehicle_recog_calibrate_empty(char parkingId);

// Vrne baseline (lahko !valid).
vr_baseline_t vehicle_recog_get_baseline(char parkingId);

// ---- Konfig in diagnostika ----

// Kliče web_ui po uspesnem POST /api/config.
void vehicle_recog_on_config_changed(void);

// Kliče sensor_mgr periodicno (vsake vr_presence_min minut) in ob zagonu.
// Popravi state machine glede na aktualne meritve (npr. zagon z vozilom).
void vehicle_recog_reconcile_state(char parkingId,
                                   uint16_t h, uint16_t p1, uint16_t p2);

// Helper za sensor_mgr — ali meritve pomenijo prazno mesto?
// Uposteva baseline + toleranco ali fallback 150 cm.
bool vehicle_recog_is_empty_reading(char parkingId,
                                    uint16_t h, uint16_t p1, uint16_t p2);

// Diagnostika za screen_service in web /api/status.
vr_diagnostics_t vehicle_recog_get_diagnostics(void);
