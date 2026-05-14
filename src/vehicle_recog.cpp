// =============================================================================
// vehicle_recog.cpp — DTW identifikacija vozil
// =============================================================================
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.1.0  |  Datum: 2026-05
//
// KLJUCNE RAZLIKE glede na v1.0 (prilagoditev za Park_auto):
//
//   feed_profile() namesto feed_sample():
//     hal_tof.cpp gradi profil interno (Δ-filter, stabilnost timer).
//     vehicle_recog_feed_profile() sprejme zaključen TofProfileResult,
//     kopira tocke v interni buffer in nastavi VR_PHASE_DTW_COMPUTE.
//     feed_sample() NI KLICAN — hal_tof ne posreduje posameznih vzorcev.
//
//   hal_tof_stopScan() po DTW:
//     Po koncu DTW burst-a (v tick()) pokliče hal_tof_stopScan()
//     → hal_tof preide DTW_WAIT → IDLE.
//     Brez tega klica hal_tof ostane blokiran v DTW_WAIT za vedno.
//
//   EventType:: enum class (ne EVT_* konstante):
//     Projekt uporablja enum class EventType : uint16_t.
//     Vsi publish/subscribe klici: EventType::VEHICLE_RECOGNIZED ipd.
//
//   BUTTON_EDIT locen per mesto (ne skupen + payload):
//     EventType::BUTTON_EDIT_VEHICLE_A in BUTTON_EDIT_VEHICLE_B.
//
//   VEHICLE_DEPARTED payload = 0=A, 1=B (ne char):
//     sensor_mgr poslje uint32_t 0 ali 1, ne char 'A'/'B'.
//
//   config_get() struct (ne string kljuci):
//     vehicle_recog_on_config_changed() bere iz const Config cfg = config_get().
//     Polja: cfg.dtw_threshold, cfg.sakoe_radius, cfg.min_profile_points,
//            cfg.normalize_points, cfg.raw_profiles_per_model,
//            cfg.presence_check_min, cfg.empty_tolerance_mm.
//
//   sd_mgr_save_raw_profile() namesto SD.open():
//     Prek sd_mgr HAL za thread-safe SD dostop.
//     PSRAM buffer za JSON sestavljanje (lazy alociran).
//
// STRUKTURA:
//   1. Interne strukture (model z Welford, raw buffer na mestu)
//   2. Globalno stanje (oba mesta, DTW matrika, config snapshot)
//   3. Init + EventBus subscribe
//   4. EventBus callback
//   5. feed_profile() — sprejem profila iz sensor_mgr
//   6. tick() — DTW burst + hal_tof_stopScan + async write
//   7. DTW algoritem (Sakoe-Chiba band, PSRAM matrika)
//   8. Linearna interpolacija profila na N tock
//   9. Welford running variance
//   10. Persistenca (LittleFS modeli + baseline, SD raw profili)
//   11. Public API implementacije
// =============================================================================

#include "vehicle_recog.h"
#include "event_bus.h"
#include "config_mgr.h"
#include "logger.h"
#include "sd_mgr.h"
#include "sensor_mgr.h"    // sensor_mgr_read_place_now()
#include "hal_tof.h"        // hal_tof_stopScan(), TofProfileResult
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

// =============================================================================
// 1. INTERNE STRUKTURE
// =============================================================================

#define VR_LOG_TAG "VR"

// Interni model — vkljucuje Welford M2 state (ni del public API)
typedef struct {
    char      id[16];
    char      name[32];
    float     average_profile[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS];
    float     welford_m2[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS];
    float     variance[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS];
    uint32_t  repetitions;
    uint32_t  lastSeen;
    float     lastDtwDistance;
    uint16_t  norm_length;
    bool      dirty;   // caka na write v LittleFS
} vr_internal_model_t;

// Kopija profila iz hal_tof (napolni feed_profile, bere tick DTW)
// Per mesto — samo eno je aktivno naenkrat (sociasno parkiranje ni mozno)
typedef struct {
    bool      pending;              // true: DTW se mora izvesti v tick()
    char      parkingId;            // 'A' ali 'B'
    uint16_t  point_count;
    uint16_t  scan_duration_ms;
    uint32_t  scan_start_ms;        // za raw profil filename (timestamp)
    // Loceni arrayi za H, P1, P2 — direktna kopija iz TofProfileResult
    uint16_t  h[120];               // max TofProfileResult.points
    uint16_t  p1[120];
    uint16_t  p2[120];
    uint32_t  ts[120];              // relativni millis od scan_start
} vr_profile_buf_t;

// Stanje per parkirno mesto
typedef struct {
    vr_place_state_t  state;
    vr_phase_t        phase;
    vr_baseline_t     baseline;

    // Trenutno vozilo na mestu
    char              current_model_id[16];
    char              current_name[32];
    float             current_dtw;
    uint32_t          park_start_ms;  // millis() ob ARRIVED

    // Modeli (lazy loaded iz LittleFS)
    vr_internal_model_t* models;      // alociran v PSRAM
    uint16_t          model_count;
    uint16_t          model_capacity;
    bool              models_loaded;
} vr_place_t;

// =============================================================================
// 2. GLOBALNO STANJE
// =============================================================================

static vr_place_t  s_place_A;
static vr_place_t  s_place_B;

// En profil buffer za oba mesta skupaj — ker hkrati le eno skenira
static vr_profile_buf_t  s_profile_buf;

// DTW matrika v PSRAM (lazy alocirana ob prvem DTW)
// Velikost: N×N float, N = max(la, lb) <= VR_PROFILE_NORM_POINTS (80)
static float*    s_dtw_matrix = nullptr;
static uint16_t  s_dtw_N = 0;

// PSRAM buffer za raw profil JSON (lazy alociran)
static char*          s_raw_json_buf = nullptr;
static const size_t   RAW_JSON_BUF_SIZE = 14336;  // 14 KB zadostuje za 120 tock

// Config snapshot — osvezuje vehicle_recog_on_config_changed()
typedef struct {
    float     dtw_threshold;
    uint16_t  sc_radius;
    uint16_t  min_points;
    uint16_t  norm_points;
    uint16_t  raw_profiles_per_model;
    uint16_t  presence_check_min;
    uint16_t  empty_tolerance_mm;
} vr_cfg_t;

static vr_cfg_t s_cfg;

// Globalna statistika (za diagnostiko)
static uint32_t s_total_recognitions = 0;
static uint32_t s_total_aborted = 0;
static uint32_t s_total_new_models = 0;
static uint32_t s_next_model_id = 1;

// =============================================================================
// Forward declarations
// =============================================================================
static vr_place_t* place_ptr(char parkingId);
static void        load_models(vr_place_t* p);
static void        save_model_sync(vr_place_t* p, vr_internal_model_t* m);
static void        load_baseline(vr_place_t* p);
static void        save_baseline(vr_place_t* p);
static void        normalize_from_buf(const vr_profile_buf_t* buf,
                                      float (*out)[VR_PROFILE_DIMS],
                                      uint16_t out_len);
static float       compute_dtw(const float (*A)[VR_PROFILE_DIMS], uint16_t la,
                                const float (*B)[VR_PROFILE_DIMS], uint16_t lb,
                                uint16_t sc_radius);
static void        run_dtw_against_all(vr_place_t* p,
                                       const float (*profile)[VR_PROFILE_DIMS],
                                       uint16_t plen,
                                       int* best_idx, float* best_dtw,
                                       int* second_idx, float* second_dtw);
static vr_internal_model_t* create_new_model(vr_place_t* p,
                                              const float (*profile)[VR_PROFILE_DIMS],
                                              uint16_t plen);
static void update_model_welford(vr_internal_model_t* m,
                                 const float (*new_profile)[VR_PROFILE_DIMS],
                                 uint16_t plen);
static void save_raw_profile_to_sd(const vr_profile_buf_t* buf,
                                   const char* modelId,
                                   float dtw, float second_dtw,
                                   const char* second_id, bool is_new);
static void on_event(const Event& evt);

// =============================================================================
// 3. INIT
// =============================================================================

bool vehicle_recog_init(void) {
    memset(&s_place_A, 0, sizeof(s_place_A));
    memset(&s_place_B, 0, sizeof(s_place_B));
    memset(&s_profile_buf, 0, sizeof(s_profile_buf));

    s_place_A.state = VR_STATE_EMPTY_UNCALIBRATED;
    s_place_B.state = VR_STATE_EMPTY_UNCALIBRATED;
    s_place_A.phase = VR_PHASE_IDLE;
    s_place_B.phase = VR_PHASE_IDLE;
    s_place_A.current_dtw = NAN;
    s_place_B.current_dtw = NAN;

    // Preberi config snapshot
    vehicle_recog_on_config_changed();

    // Alociraj modele array v PSRAM (capacity=32 na start, rasteta dynamicno)
    s_place_A.model_capacity = 32;
    s_place_B.model_capacity = 32;
    s_place_A.models = (vr_internal_model_t*)ps_calloc(
        s_place_A.model_capacity, sizeof(vr_internal_model_t));
    s_place_B.models = (vr_internal_model_t*)ps_calloc(
        s_place_B.model_capacity, sizeof(vr_internal_model_t));
    if (!s_place_A.models || !s_place_B.models) {
        LOG_ERROR(VR_LOG_TAG, "ps_calloc za modele ni uspel — PSRAM?");
        return false;
    }

    // Nalozi baseline (ce ne obstaja, ostane valid=false)
    load_baseline(&s_place_A);
    load_baseline(&s_place_B);

    // Zagotovi da LittleFS direktorij /vehicles obstaja
    if (!LittleFS.exists("/vehicles")) {
        LittleFS.mkdir("/vehicles");
    }

    // EventBus subscribe
    // DOOR_OPENED: vehicle_recog samo sledi fazi za UI (realno faza = hal_tof)
    EventBus::subscribe(EventType::DOOR_OPENED, on_event);
    // TOF_PROFILE_READY: signal da je hal_tof zaključil profil
    // (feed_profile je ze bil klican iz sensorTask callbacka — tukaj samo log)
    EventBus::subscribe(EventType::TOF_PROFILE_READY, on_event);
    // VEHICLE_DEPARTED: sensor_mgr zaznal odhod
    EventBus::subscribe(EventType::VEHICLE_DEPARTED, on_event);
    // Gumbi (loceni per mesto — brez payload)
    EventBus::subscribe(EventType::BUTTON_EDIT_VEHICLE_A, on_event);
    EventBus::subscribe(EventType::BUTTON_EDIT_VEHICLE_B, on_event);
    // Kalibracija — screen_main publishira po 10s + DA
    EventBus::subscribe(EventType::BUTTON_CALIBRATE_EMPTY_A, on_event);
    EventBus::subscribe(EventType::BUTTON_CALIBRATE_EMPTY_B, on_event);

    LOG_INFO(VR_LOG_TAG, "Init OK. Baseline A=%s B=%s",
             s_place_A.baseline.valid ? "valid" : "missing",
             s_place_B.baseline.valid ? "valid" : "missing");
    return true;
}

// =============================================================================
// 4. EVENTBUS CALLBACK
// =============================================================================

static void on_event(const Event& evt) {
    switch (evt.type) {

        // ---------------------------------------------------------------------
        // DOOR_OPENED — rampagor LOW: vehicle_recog sledi fazi za UI
        // Realna detekcija je v hal_tof (startDetect je klican iz sensor_mgr
        // ali light_logic ob DOOR_OPENED). Tukaj samo posodobimo fazo
        // za screen_main badge.
        // ---------------------------------------------------------------------
        case EventType::DOOR_OPENED: {
            if (s_place_A.phase == VR_PHASE_IDLE
                && s_place_A.state != VR_STATE_OCCUPIED_KNOWN
                && s_place_A.state != VR_STATE_OCCUPIED_UNKNOWN) {
                s_place_A.phase = VR_PHASE_DETECT;
                LOG_INFO(VR_LOG_TAG, "A -> DETECT (door)");
            }
            if (s_place_B.phase == VR_PHASE_IDLE
                && s_place_B.state != VR_STATE_OCCUPIED_KNOWN
                && s_place_B.state != VR_STATE_OCCUPIED_UNKNOWN) {
                s_place_B.phase = VR_PHASE_DETECT;
                LOG_INFO(VR_LOG_TAG, "B -> DETECT (door)");
            }
            break;
        }

        // ---------------------------------------------------------------------
        // TOF_PROFILE_READY — hal_tof je zakljucil profil.
        // feed_profile() je bil ZE klican iz sensorTask pred tem eventom.
        // Tukaj samo posodobimo fazo za UI (SCANNING → DTW_COMPUTE).
        // payload: 0=A, 1=B
        // ---------------------------------------------------------------------
        case EventType::TOF_PROFILE_READY: {
            char pid = (evt.payload == 0u) ? 'A' : 'B';
            vr_place_t* p = place_ptr(pid);
            if (!p) break;
            p->phase = VR_PHASE_DTW_COMPUTE;
            LOG_INFO(VR_LOG_TAG, "%c -> DTW_COMPUTE (%u pts, %u ms)",
                     pid, s_profile_buf.point_count, s_profile_buf.scan_duration_ms);
            break;
        }

        // ---------------------------------------------------------------------
        // VEHICLE_DEPARTED — sensor_mgr zaznal odhod (stabilnost > 2s)
        // payload: 0=A, 1=B
        // ---------------------------------------------------------------------
        case EventType::VEHICLE_DEPARTED: {
            char pid = (evt.payload == 0u) ? 'A' : 'B';
            vr_place_t* p = place_ptr(pid);
            if (!p) break;
            uint32_t park_min = 0;
            if (p->park_start_ms > 0) {
                park_min = (millis() - p->park_start_ms) / 60000UL;
            }
            LOG_INFO(VR_LOG_TAG, "%c -> EMPTY (departed %u min)", pid, park_min);
            p->phase = VR_PHASE_IDLE;
            p->current_model_id[0] = '\0';
            p->current_name[0]     = '\0';
            p->current_dtw         = NAN;
            p->park_start_ms       = 0;
            p->state = p->baseline.valid
                       ? VR_STATE_EMPTY_CALIBRATED
                       : VR_STATE_EMPTY_UNCALIBRATED;
            break;
        }

        // ---------------------------------------------------------------------
        // BUTTON_EDIT_VEHICLE_A/B — 2s dolg pritisk na parking kartico.
        // Blokirano ce mesto ni v OCCUPIED_* (dodatek pogl. 11).
        // Blokirano med dinamicnimi fazami.
        // hal_display odpre LVGL keyboard — ko user potrdi, klice
        // vehicle_recog_rename_model() direktno.
        // ---------------------------------------------------------------------
        case EventType::BUTTON_EDIT_VEHICLE_A:
        case EventType::BUTTON_EDIT_VEHICLE_B: {
            char pid = (evt.type == EventType::BUTTON_EDIT_VEHICLE_A) ? 'A' : 'B';
            vr_place_t* p = place_ptr(pid);
            if (!p) break;
            if (p->state != VR_STATE_OCCUPIED_KNOWN
                && p->state != VR_STATE_OCCUPIED_UNKNOWN) {
                LOG_INFO(VR_LOG_TAG, "Edit %c: blokirano — ni vozila", pid);
                break;
            }
            if (p->phase == VR_PHASE_DETECT
                || p->phase == VR_PHASE_SCANNING
                || p->phase == VR_PHASE_DTW_COMPUTE) {
                LOG_INFO(VR_LOG_TAG, "Edit %c: blokirano — dinamicna faza", pid);
                break;
            }
            // hal_display je ze subscriberan na ta event — sam odpre dialog.
            // Tukaj samo logiramo za debug.
            LOG_INFO(VR_LOG_TAG, "Edit %c: '%s' (%s)",
                     pid, p->current_name,
                     p->current_model_id[0] ? p->current_model_id : "---");
            break;
        }

        // ---------------------------------------------------------------------
        // BUTTON_CALIBRATE_EMPTY_A/B — po 10s + DA confirm v screen_main.
        // vehicle_recog_calibrate_empty() je ze klicano direktno iz screen_main
        // po confirm dialogu. Ta event je signal za logging.
        // ---------------------------------------------------------------------
        case EventType::BUTTON_CALIBRATE_EMPTY_A:
        case EventType::BUTTON_CALIBRATE_EMPTY_B: {
            char pid = (evt.type == EventType::BUTTON_CALIBRATE_EMPTY_A) ? 'A' : 'B';
            LOG_INFO(VR_LOG_TAG, "Calibrate event %c — screen_main bo klical calibrate_empty()", pid);
            break;
        }

        default:
            break;
    }
}

// =============================================================================
// 5. SPREJEM PROFILA IZ sensor_mgr
// =============================================================================
// Kliče sensor_mgr iz TofProfileCallback (sensorTask kontekst).
// Kopira tocke iz TofProfileResult v interni buffer.
// Nastavi s_profile_buf.pending = true → tick() bo izvedel DTW.
// NE blokira — kopiranje je ~1-2 ms za 120 tock.

void vehicle_recog_feed_profile(const TofProfileResult& profile) {
    char pid = (profile.place == TOF_PLACE_A) ? 'A' : 'B';
    vr_place_t* p = place_ptr(pid);
    if (!p) return;

    // Ce je DTW ze v teku (pending) — preskok (ne bi smelo zgoditi,
    // hal_tof ostane v DTW_WAIT dokler ni stopScan klican)
    if (s_profile_buf.pending) {
        LOG_WARN(VR_LOG_TAG, "feed_profile %c: ze pending — prezremo", pid);
        return;
    }

    uint16_t cnt = profile.count;
    if (cnt == 0) {
        LOG_WARN(VR_LOG_TAG, "feed_profile %c: count=0 — prezremo", pid);
        return;
    }
    // Clamp na max (zastitna meja — hal_tof ze omejuje na 120)
    if (cnt > 120) cnt = 120;

    // Kopiraj tocke
    s_profile_buf.parkingId       = pid;
    s_profile_buf.point_count     = cnt;
    s_profile_buf.scan_duration_ms = (uint16_t)
        (profile.scan_duration_ms > 65535 ? 65535 : profile.scan_duration_ms);
    s_profile_buf.scan_start_ms   = profile.scan_start_ms;

    for (uint16_t i = 0; i < cnt; i++) {
        s_profile_buf.h[i]  = profile.points[i].H_mm;
        s_profile_buf.p1[i] = profile.points[i].P1_mm;
        s_profile_buf.p2[i] = profile.points[i].P2_mm;
        s_profile_buf.ts[i] = profile.points[i].ts_ms;
    }

    // Posodobi fazni UI (bo SCANNING → DTW_COMPUTE po EVT_TOF_PROFILE_READY)
    p->phase = VR_PHASE_SCANNING;

    // Lazy load modelov ce se niso nalozeni (pred DTW burst-om)
    if (!p->models_loaded) {
        load_models(p);
    }

    s_profile_buf.pending = true;
    LOG_INFO(VR_LOG_TAG, "feed_profile %c: %u pts, %u ms",
             pid, cnt, s_profile_buf.scan_duration_ms);
}

// =============================================================================
// 6. TICK — DTW burst + hal_tof_stopScan + async LittleFS write
// =============================================================================

void vehicle_recog_tick(void) {
    // Ce ni profila v cakanju — ni DTW dela
    if (!s_profile_buf.pending) return;

    char pid = s_profile_buf.parkingId;
    vr_place_t* p = place_ptr(pid);
    if (!p) {
        s_profile_buf.pending = false;
        return;
    }

    uint16_t raw_count = s_profile_buf.point_count;
    uint16_t scan_ms   = s_profile_buf.scan_duration_ms;

    // ---- Preveri min tocke ----
    if (raw_count < s_cfg.min_points) {
        static ParkingScanAbortedEvent_t ev_abort;
        memset(&ev_abort, 0, sizeof(ev_abort));
        ev_abort.parkingId    = pid;
        ev_abort.profileLength = raw_count;
        ev_abort.scanDurationMs = scan_ms;
        strncpy(ev_abort.reason, "too_few_points", sizeof(ev_abort.reason) - 1);
        ev_abort.is_aborted   = true;
        EventBus::publish(EventType::PARKING_SCAN_ABORTED, (uint32_t)&ev_abort);
        s_total_aborted++;
        LOG_WARN(VR_LOG_TAG, "%c: abort — %u pts < %u min", pid, raw_count, s_cfg.min_points);
        // Vrni hal_tof v IDLE
        hal_tof_stopScan();
        p->phase = VR_PHASE_IDLE;
        s_profile_buf.pending = false;
        return;
    }

    // ---- Normalizacija profila na N tock ----
    float norm_profile[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS];
    normalize_from_buf(&s_profile_buf, norm_profile, s_cfg.norm_points);

    // ---- DTW proti vsem modelom ----
    int   best_idx = -1,  second_idx = -1;
    float best_dtw = INFINITY, second_dtw = INFINITY;
    run_dtw_against_all(p, norm_profile, s_cfg.norm_points,
                        &best_idx, &best_dtw,
                        &second_idx, &second_dtw);

    // ---- Odlocitev: nov model ali ujemanje ----
    bool is_new = (best_idx < 0) || (best_dtw >= s_cfg.dtw_threshold);
    vr_internal_model_t* m = nullptr;

    if (is_new) {
        m = create_new_model(p, norm_profile, s_cfg.norm_points);
        if (!m) {
            LOG_ERROR(VR_LOG_TAG, "%c: create_new_model ni uspel", pid);
            hal_tof_stopScan();
            p->phase = VR_PHASE_IDLE;
            s_profile_buf.pending = false;
            return;
        }
        s_total_new_models++;
    } else {
        m = &p->models[best_idx];
        update_model_welford(m, norm_profile, s_cfg.norm_points);
        m->repetitions++;
        m->lastSeen = (uint32_t)time(nullptr);
        m->lastDtwDistance = best_dtw;
        m->dirty = true;
        save_model_sync(p, m);
    }

    // ---- Posodobi stanje mesta ----
    strncpy(p->current_model_id, m->id,   sizeof(p->current_model_id) - 1);
    p->current_model_id[sizeof(p->current_model_id) - 1] = '\0';
    strncpy(p->current_name,     m->name, sizeof(p->current_name) - 1);
    p->current_name[sizeof(p->current_name) - 1] = '\0';
    p->current_dtw   = is_new ? NAN : best_dtw;
    p->park_start_ms = millis();
    p->state         = VR_STATE_OCCUPIED_KNOWN;
    p->phase         = VR_PHASE_PARKED;
    s_total_recognitions++;

    // ---- EventBus publish ----
    // static: naslov veljavne med synchronous EventBus dispatcha
    static VehicleRecognizedEvent_t ev_rec;
    memset(&ev_rec, 0, sizeof(ev_rec));
    ev_rec.parkingId   = pid;
    strncpy(ev_rec.modelId, m->id,   sizeof(ev_rec.modelId) - 1);
    strncpy(ev_rec.name,    m->name, sizeof(ev_rec.name) - 1);
    ev_rec.dtwDistance    = is_new ? NAN : best_dtw;
    ev_rec.secondBestDtw  = (second_idx >= 0) ? second_dtw : NAN;
    if (second_idx >= 0) {
        strncpy(ev_rec.secondBestModelId,
                p->models[second_idx].id,
                sizeof(ev_rec.secondBestModelId) - 1);
    }
    ev_rec.profileLength  = raw_count;
    ev_rec.scanDurationMs = scan_ms;
    ev_rec.repetitions    = m->repetitions;
    ev_rec.isNewModel     = is_new;
    // Locen event za novo vozilo (LCD reagira drugace — dodatek pogl. 6.2)
    EventBus::publish(
        is_new ? EventType::VEHICLE_NEW_MODEL : EventType::VEHICLE_RECOGNIZED,
        (uint32_t)&ev_rec);

    LOG_INFO(VR_LOG_TAG, "%c: %s '%s' DTW=%.2f (2nd=%.2f) %u pts %u ms rep=%u",
             pid, is_new ? "NEW" : "MATCH", m->name,
             best_dtw, second_dtw, raw_count, scan_ms, m->repetitions);

    // ---- SD raw profil (FIFO) ----
    save_raw_profile_to_sd(&s_profile_buf, m->id, best_dtw, second_dtw,
                           second_idx >= 0 ? p->models[second_idx].id : "",
                           is_new);
    // Cleanup: obdrzi samo zadnjih N
    char raw_dir[64];
    snprintf(raw_dir, sizeof(raw_dir), "/raw/parking%c/%s", pid, m->id);
    sd_mgr_keep_newest_n(raw_dir, s_cfg.raw_profiles_per_model);

    // ---- hal_tof_stopScan(): DTW_WAIT → IDLE ----
    // KRITICNO: brez tega klica hal_tof ostane v DTW_WAIT za vedno
    // in sensorTask ne bo vec inicializiral novih skeniranj.
    hal_tof_stopScan();

    // Pocisti buffer
    s_profile_buf.pending = false;
}

// =============================================================================
// 7. DTW ALGORITEM — Sakoe-Chiba band
// =============================================================================
// M[i][j] = cost(A[i],B[j]) + min(M[i-1][j], M[i][j-1], M[i-1][j-1])
// Sakoe-Chiba: |i - j| <= sc_radius
// cost = evklidska razdalja v 3D (H, P1, P2 v mm)
// Rezultat normaliziran z (la + lb) — primerljivo med razlicnimi dolzinami

static float compute_dtw(const float (*A)[VR_PROFILE_DIMS], uint16_t la,
                         const float (*B)[VR_PROFILE_DIMS], uint16_t lb,
                         uint16_t sc_radius) {
    uint16_t N = (la > lb) ? la : lb;

    // Lazy alokacija v PSRAM
    if (!s_dtw_matrix || s_dtw_N < N) {
        if (s_dtw_matrix) free(s_dtw_matrix);
        s_dtw_matrix = (float*)ps_malloc((size_t)N * N * sizeof(float));
        s_dtw_N = N;
        if (!s_dtw_matrix) {
            LOG_ERROR(VR_LOG_TAG, "DTW matrix alloc napaka (N=%u)", N);
            return INFINITY;
        }
    }

    // Init z INFINITY — samo celice znotraj SC bande se izpolnijo
    for (uint16_t i = 0; i < la; i++) {
        for (uint16_t j = 0; j < lb; j++) {
            s_dtw_matrix[i * lb + j] = INFINITY;
        }
    }

    // Prva celica
    {
        float dh  = A[0][0] - B[0][0];
        float dp1 = A[0][1] - B[0][1];
        float dp2 = A[0][2] - B[0][2];
        s_dtw_matrix[0] = sqrtf(dh * dh + dp1 * dp1 + dp2 * dp2);
    }

    // Rekurzija z SC bando
    for (uint16_t i = 0; i < la; i++) {
        uint16_t j_min = (i > sc_radius) ? (i - sc_radius) : 0;
        uint16_t j_max = (i + sc_radius < lb) ? (i + sc_radius) : (lb - 1);
        for (uint16_t j = j_min; j <= j_max; j++) {
            if (i == 0 && j == 0) continue;
            float dh  = A[i][0] - B[j][0];
            float dp1 = A[i][1] - B[j][1];
            float dp2 = A[i][2] - B[j][2];
            float cost = sqrtf(dh * dh + dp1 * dp1 + dp2 * dp2);
            float best = INFINITY;
            if (i > 0)       best = fminf(best, s_dtw_matrix[(i - 1) * lb + j]);
            if (j > 0)       best = fminf(best, s_dtw_matrix[i * lb + (j - 1)]);
            if (i > 0 && j > 0) best = fminf(best, s_dtw_matrix[(i - 1) * lb + (j - 1)]);
            s_dtw_matrix[i * lb + j] = cost + best;
        }
    }

    float raw = s_dtw_matrix[(la - 1) * lb + (lb - 1)];
    return raw / (float)(la + lb);
}

static void run_dtw_against_all(vr_place_t* p,
                                const float (*profile)[VR_PROFILE_DIMS],
                                uint16_t plen,
                                int* best_idx, float* best_dtw,
                                int* second_idx, float* second_dtw) {
    *best_idx = -1;  *second_idx = -1;
    *best_dtw = INFINITY; *second_dtw = INFINITY;
    for (uint16_t i = 0; i < p->model_count; i++) {
        float d = compute_dtw(p->models[i].average_profile,
                              p->models[i].norm_length,
                              profile, plen, s_cfg.sc_radius);
        if (d < *best_dtw) {
            *second_dtw = *best_dtw;  *second_idx = *best_idx;
            *best_dtw = d;            *best_idx = i;
        } else if (d < *second_dtw) {
            *second_dtw = d;          *second_idx = i;
        }
    }
}

// =============================================================================
// 8. LINEARNA INTERPOLACIJA NA N TOCK
// =============================================================================
// Vhod: vr_profile_buf_t (loceni H, P1, P2 arrayi)
// Izhod: float[out_len][3] normaliziran profil

static void normalize_from_buf(const vr_profile_buf_t* buf,
                                float (*out)[VR_PROFILE_DIMS],
                                uint16_t out_len) {
    uint16_t in_len = buf->point_count;
    if (in_len == 0 || out_len == 0) return;

    // Edge case: ena tocka → repliciramo
    if (in_len == 1) {
        float h  = (buf->h[0]  == TOF_ERR) ? 0.0f : (float)buf->h[0];
        float p1 = (buf->p1[0] == TOF_ERR) ? 0.0f : (float)buf->p1[0];
        float p2 = (buf->p2[0] == TOF_ERR) ? 0.0f : (float)buf->p2[0];
        for (uint16_t i = 0; i < out_len; i++) {
            out[i][0] = h; out[i][1] = p1; out[i][2] = p2;
        }
        return;
    }

    for (uint16_t i = 0; i < out_len; i++) {
        float pos = (float)i * (in_len - 1) / (out_len - 1);
        uint16_t lo = (uint16_t)floorf(pos);
        uint16_t hi = (lo + 1 < in_len) ? (lo + 1) : (in_len - 1);
        float frac = pos - lo;

        // TOF_ERR (0xFFFF) pri P1/P2 je mozno — zamenjamo z H vrednostjo
        // (degradiran senzor: profil ostane validen za H dimenzijo)
        float h_lo  = (buf->h[lo]  == TOF_ERR) ? 0.0f : (float)buf->h[lo];
        float p1_lo = (buf->p1[lo] == TOF_ERR) ? h_lo : (float)buf->p1[lo];
        float p2_lo = (buf->p2[lo] == TOF_ERR) ? h_lo : (float)buf->p2[lo];
        float h_hi  = (buf->h[hi]  == TOF_ERR) ? 0.0f : (float)buf->h[hi];
        float p1_hi = (buf->p1[hi] == TOF_ERR) ? h_hi : (float)buf->p1[hi];
        float p2_hi = (buf->p2[hi] == TOF_ERR) ? h_hi : (float)buf->p2[hi];

        out[i][0] = h_lo  * (1.0f - frac) + h_hi  * frac;
        out[i][1] = p1_lo * (1.0f - frac) + p1_hi * frac;
        out[i][2] = p2_lo * (1.0f - frac) + p2_hi * frac;
    }
}

// =============================================================================
// 9. WELFORD RUNNING VARIANCE
// =============================================================================
// new_avg = old_avg + (x - old_avg) / n
// new_M2  = old_M2  + (x - old_avg) * (x - new_avg)
// variance = M2 / (n - 1)

static void update_model_welford(vr_internal_model_t* m,
                                 const float (*new_profile)[VR_PROFILE_DIMS],
                                 uint16_t plen) {
    // n = repetitions + 1 (nova meritev je ze stevec pred inkrementom)
    uint32_t n = m->repetitions + 1;
    for (uint16_t i = 0; i < plen; i++) {
        for (uint16_t d = 0; d < VR_PROFILE_DIMS; d++) {
            float old_avg = m->average_profile[i][d];
            float x       = new_profile[i][d];
            float new_avg = old_avg + (x - old_avg) / (float)n;
            m->welford_m2[i][d]      += (x - old_avg) * (x - new_avg);
            m->average_profile[i][d]  = new_avg;
            if (n > 1) m->variance[i][d] = m->welford_m2[i][d] / (float)(n - 1);
        }
    }
}

static vr_internal_model_t* create_new_model(vr_place_t* p,
                                              const float (*profile)[VR_PROFILE_DIMS],
                                              uint16_t plen) {
    // Rast capacity (PSRAM realloc)
    if (p->model_count >= p->model_capacity) {
        uint16_t new_cap = p->model_capacity * 2;
        vr_internal_model_t* new_arr = (vr_internal_model_t*)ps_realloc(
            p->models, (size_t)new_cap * sizeof(vr_internal_model_t));
        if (!new_arr) {
            LOG_ERROR(VR_LOG_TAG, "ps_realloc za modele ni uspel");
            return nullptr;
        }
        p->models = new_arr;
        p->model_capacity = new_cap;
    }

    vr_internal_model_t* m = &p->models[p->model_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->id,   sizeof(m->id),   "m_%03u", (unsigned)s_next_model_id++);
    snprintf(m->name, sizeof(m->name), "Avto %u", (unsigned)(p->model_count + 1));
    m->norm_length     = plen;
    m->repetitions     = 1;
    m->lastSeen        = (uint32_t)time(nullptr);
    m->lastDtwDistance = NAN;

    for (uint16_t i = 0; i < plen; i++) {
        for (uint16_t d = 0; d < VR_PROFILE_DIMS; d++) {
            m->average_profile[i][d] = profile[i][d];
            m->welford_m2[i][d]      = 0.0f;
            m->variance[i][d]        = 0.0f;
        }
    }
    m->dirty = true;
    p->model_count++;
    save_model_sync(p, m);
    return m;
}

// =============================================================================
// 10. PERSISTENCA — LittleFS modeli + baseline
// =============================================================================

static void load_models(vr_place_t* p) {
    p->models_loaded = true;
    p->model_count   = 0;
    char dir[32];
    snprintf(dir, sizeof(dir), "/vehicles/%c",
             (p == &s_place_A) ? 'A' : 'B');
    if (!LittleFS.exists(dir)) {
        LittleFS.mkdir(dir);
        LOG_INFO(VR_LOG_TAG, "Kreiran %s (prazen)", dir);
        return;
    }
    File root = LittleFS.open(dir);
    if (!root || !root.isDirectory()) {
        LOG_WARN(VR_LOG_TAG, "Ne morem odpreti %s", dir);
        return;
    }
    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory() && strstr(f.name(), ".json")) {
            DynamicJsonDocument doc(16384);
            if (deserializeJson(doc, f) == DeserializationError::Ok
                && p->model_count < p->model_capacity) {
                vr_internal_model_t* m = &p->models[p->model_count];
                memset(m, 0, sizeof(*m));
                strncpy(m->id,   doc["id"]   | "", sizeof(m->id) - 1);
                strncpy(m->name, doc["name"] | "Avto X", sizeof(m->name) - 1);
                m->repetitions     = doc["repetitions"] | 1;
                m->lastSeen        = doc["lastSeen"]    | 0;
                m->lastDtwDistance = doc["lastDtw"]     | NAN;
                m->norm_length     = doc["normLength"]  | VR_DEFAULT_NORM_POINTS;
                if (m->norm_length > VR_PROFILE_NORM_POINTS)
                    m->norm_length = VR_PROFILE_NORM_POINTS;
                JsonArray avg = doc["avg"];
                JsonArray var = doc["var"];
                JsonArray m2  = doc["m2"];
                for (uint16_t i = 0; i < m->norm_length; i++) {
                    for (uint16_t d = 0; d < VR_PROFILE_DIMS; d++) {
                        uint16_t k = i * VR_PROFILE_DIMS + d;
                        m->average_profile[i][d] = avg[k] | 0.0f;
                        m->variance[i][d]        = var[k] | 0.0f;
                        m->welford_m2[i][d]      = m2[k]  | 0.0f;
                    }
                }
                // Posodobi naslednji ID
                unsigned id_num = 0;
                if (sscanf(m->id, "m_%u", &id_num) == 1
                    && id_num >= (unsigned)s_next_model_id) {
                    s_next_model_id = id_num + 1;
                }
                p->model_count++;
            }
        }
        f = root.openNextFile();
    }
    LOG_INFO(VR_LOG_TAG, "Nalozeno %u modelov za %c",
             p->model_count, (p == &s_place_A) ? 'A' : 'B');
}

static void save_model_sync(vr_place_t* p, vr_internal_model_t* m) {
    if (!m || !m->dirty) return;
    char path[64];
    snprintf(path, sizeof(path), "/vehicles/%c/%s.json",
             (p == &s_place_A) ? 'A' : 'B', m->id);
    File f = LittleFS.open(path, "w");
    if (!f) {
        LOG_ERROR(VR_LOG_TAG, "Ne morem pisati: %s", path);
        return;
    }
    DynamicJsonDocument doc(16384);
    doc["id"]          = m->id;
    doc["name"]        = m->name;
    doc["repetitions"] = m->repetitions;
    doc["lastSeen"]    = m->lastSeen;
    doc["lastDtw"]     = m->lastDtwDistance;
    doc["normLength"]  = m->norm_length;
    JsonArray avg = doc.createNestedArray("avg");
    JsonArray var = doc.createNestedArray("var");
    JsonArray m2  = doc.createNestedArray("m2");
    for (uint16_t i = 0; i < m->norm_length; i++) {
        for (uint16_t d = 0; d < VR_PROFILE_DIMS; d++) {
            avg.add(m->average_profile[i][d]);
            var.add(m->variance[i][d]);
            m2.add(m->welford_m2[i][d]);
        }
    }
    serializeJson(doc, f);
    f.close();
    m->dirty = false;
}

static void load_baseline(vr_place_t* p) {
    char path[48];
    snprintf(path, sizeof(path), "/vehicles/baseline_%c.json",
             (p == &s_place_A) ? 'A' : 'B');
    p->baseline.valid = false;
    if (!LittleFS.exists(path)) return;
    File f = LittleFS.open(path, "r");
    if (!f) return;
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        p->baseline.valid         = doc["valid"] | false;
        p->baseline.h_mm          = doc["h"]     | 0;
        p->baseline.p1_mm         = doc["p1"]    | 0;
        p->baseline.p2_mm         = doc["p2"]    | 0;
        p->baseline.calibrated_at = doc["ts"]    | 0;
    }
    f.close();
    if (p->baseline.valid) p->state = VR_STATE_EMPTY_CALIBRATED;
}

static void save_baseline(vr_place_t* p) {
    if (!LittleFS.exists("/vehicles")) LittleFS.mkdir("/vehicles");
    char path[48];
    snprintf(path, sizeof(path), "/vehicles/baseline_%c.json",
             (p == &s_place_A) ? 'A' : 'B');
    File f = LittleFS.open(path, "w");
    if (!f) {
        LOG_ERROR(VR_LOG_TAG, "Ne morem pisati baseline: %s", path);
        return;
    }
    StaticJsonDocument<256> doc;
    doc["valid"] = p->baseline.valid;
    doc["h"]     = p->baseline.h_mm;
    doc["p1"]    = p->baseline.p1_mm;
    doc["p2"]    = p->baseline.p2_mm;
    doc["ts"]    = p->baseline.calibrated_at;
    serializeJson(doc, f);
    f.close();
}

// =============================================================================
// Raw profili na SD — FIFO rotacija
// Format filename: YYYYMMDD_HHMMSS_<modelId>.json (dodatek pogl. 8)
// Streamano pisanje v PSRAM buffer, nato sd_mgr_save_raw_profile()
// =============================================================================

static void save_raw_profile_to_sd(const vr_profile_buf_t* buf,
                                   const char* modelId,
                                   float dtw, float second_dtw,
                                   const char* second_id, bool is_new) {
    if (!sd_mgr_ready()) return;

    char dir[64];
    snprintf(dir, sizeof(dir), "/raw/parking%c/%s", buf->parkingId, modelId);
    sd_mgr_ensure_dir(dir);

    // Filename s timestamp-om in model ID (dodatek pogl. 8)
    time_t now_t = (time_t)buf->scan_start_ms / 1000;
    // Ce NTP ni sinhroniziran, pademo nazaj na millis timestamp
    if (time(nullptr) > 1577836800UL) now_t = time(nullptr);
    struct tm tm_local;
    localtime_r(&now_t, &tm_local);
    char fname[128];
    snprintf(fname, sizeof(fname), "%s/%04d%02d%02d_%02d%02d%02d_%s.json",
             dir,
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
             modelId);

    // Lazy alokacija PSRAM bufferja za JSON
    if (!s_raw_json_buf) {
        s_raw_json_buf = (char*)ps_malloc(RAW_JSON_BUF_SIZE);
        if (!s_raw_json_buf) {
            LOG_WARN(VR_LOG_TAG, "raw JSON PSRAM alloc napaka");
            return;
        }
    }

    // Sestavi JSON v buffferju (streamano — ni DynamicJsonDocument za 120 tock)
    int pos = 0;
    pos += snprintf(s_raw_json_buf + pos, RAW_JSON_BUF_SIZE - pos,
                    "{\"parking\":\"%c\",\"model\":\"%s\",\"isNew\":%s"
                    ",\"dtw\":%.3f,\"secondDtw\":%.3f,\"secondId\":\"%s\""
                    ",\"scanMs\":%u,\"points\":[",
                    buf->parkingId, modelId,
                    is_new ? "true" : "false",
                    dtw, second_dtw,
                    second_id ? second_id : "",
                    (unsigned)buf->scan_duration_ms);

    for (uint16_t i = 0; i < buf->point_count && pos < (int)(RAW_JSON_BUF_SIZE - 80); i++) {
        if (i > 0) s_raw_json_buf[pos++] = ',';
        // Relativen timestamp od scan_start (ne absolutni)
        uint32_t rel_ms = (buf->ts[i] > buf->scan_start_ms)
                          ? (buf->ts[i] - buf->scan_start_ms)
                          : 0;
        pos += snprintf(s_raw_json_buf + pos, RAW_JSON_BUF_SIZE - pos,
                        "[%u,%u,%u,%u]",
                        (unsigned)rel_ms,
                        (unsigned)buf->h[i],
                        (unsigned)buf->p1[i],
                        (unsigned)buf->p2[i]);
    }

    if (pos < (int)(RAW_JSON_BUF_SIZE - 4)) {
        pos += snprintf(s_raw_json_buf + pos, RAW_JSON_BUF_SIZE - pos, "]}");
    }

    sd_mgr_save_raw_profile(fname, s_raw_json_buf);
}

// =============================================================================
// 11. PUBLIC API IMPLEMENTACIJE
// =============================================================================

static vr_place_t* place_ptr(char parkingId) {
    if (parkingId == 'A') return &s_place_A;
    if (parkingId == 'B') return &s_place_B;
    return nullptr;
}

vr_place_state_t vehicle_recog_get_state(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    return p ? p->state : VR_STATE_EMPTY_UNCALIBRATED;
}

vr_phase_t vehicle_recog_get_phase(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    return p ? p->phase : VR_PHASE_IDLE;
}

const char* vehicle_recog_get_vehicle_name(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    return (p && p->state == VR_STATE_OCCUPIED_KNOWN) ? p->current_name : "";
}

const char* vehicle_recog_get_model_id(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    return (p && p->state == VR_STATE_OCCUPIED_KNOWN) ? p->current_model_id : "";
}

float vehicle_recog_get_last_dtw(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    return p ? p->current_dtw : NAN;
}

uint16_t vehicle_recog_get_model_count(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p) return 0;
    if (!p->models_loaded) load_models(p);
    return p->model_count;
}

bool vehicle_recog_get_model_summary(char parkingId, uint16_t index,
                                     vr_model_summary_t* out) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p || !out) return false;
    if (!p->models_loaded) load_models(p);
    if (index >= p->model_count) return false;
    vr_internal_model_t* m = &p->models[index];
    strncpy(out->id,   m->id,   sizeof(out->id) - 1);   out->id[sizeof(out->id)-1] = '\0';
    strncpy(out->name, m->name, sizeof(out->name) - 1); out->name[sizeof(out->name)-1] = '\0';
    out->repetitions    = m->repetitions;
    out->lastSeen       = m->lastSeen;
    out->lastDtwDistance = m->lastDtwDistance;
    return true;
}

bool vehicle_recog_get_model_profile(char parkingId, const char* modelId,
                                     float (*out_data)[VR_PROFILE_DIMS],
                                     float (*out_variance)[VR_PROFILE_DIMS],
                                     uint16_t* out_length) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p || !modelId || !out_data || !out_variance || !out_length) return false;
    if (!p->models_loaded) load_models(p);
    for (uint16_t i = 0; i < p->model_count; i++) {
        if (strcmp(p->models[i].id, modelId) == 0) {
            uint16_t L = p->models[i].norm_length;
            for (uint16_t j = 0; j < L; j++) {
                for (uint16_t d = 0; d < VR_PROFILE_DIMS; d++) {
                    out_data[j][d]     = p->models[i].average_profile[j][d];
                    out_variance[j][d] = p->models[i].variance[j][d];
                }
            }
            *out_length = L;
            return true;
        }
    }
    return false;
}

bool vehicle_recog_rename_model(char parkingId, const char* modelId,
                                const char* newName) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p || !modelId || !newName) return false;
    if (!p->models_loaded) load_models(p);
    for (uint16_t i = 0; i < p->model_count; i++) {
        if (strcmp(p->models[i].id, modelId) == 0) {
            strncpy(p->models[i].name, newName, sizeof(p->models[i].name) - 1);
            p->models[i].name[sizeof(p->models[i].name) - 1] = '\0';
            p->models[i].dirty = true;
            save_model_sync(p, &p->models[i]);
            // Posodobi current_name ce je to trenutno vozilo na mestu
            if (strcmp(p->current_model_id, modelId) == 0) {
                strncpy(p->current_name, newName, sizeof(p->current_name) - 1);
                p->current_name[sizeof(p->current_name) - 1] = '\0';
            }
            LOG_INFO(VR_LOG_TAG, "%c: rename %s -> '%s'", parkingId, modelId, newName);
            return true;
        }
    }
    return false;
}

bool vehicle_recog_delete_model(char parkingId, const char* modelId) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p || !modelId) return false;
    if (!p->models_loaded) load_models(p);
    for (uint16_t i = 0; i < p->model_count; i++) {
        if (strcmp(p->models[i].id, modelId) == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/vehicles/%c/%s.json", parkingId, modelId);
            LittleFS.remove(path);
            // Premakni preostale modele v array (compact)
            for (uint16_t j = i; j + 1 < p->model_count; j++) {
                memcpy(&p->models[j], &p->models[j + 1], sizeof(vr_internal_model_t));
            }
            p->model_count--;
            LOG_INFO(VR_LOG_TAG, "%c: deleted %s", parkingId, modelId);
            return true;
        }
    }
    return false;
}

bool vehicle_recog_calibrate_empty(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p) return false;

    // Samo v EMPTY_* stanjih (dodatek pogl. 11 + pogl. 2)
    if (p->state != VR_STATE_EMPTY_CALIBRATED
        && p->state != VR_STATE_EMPTY_UNCALIBRATED) {
        LOG_WARN(VR_LOG_TAG, "Calibrate %c: blokirano — ni EMPTY (state=%d)",
                 parkingId, (int)p->state);
        return false;
    }
    // Varnostna mreza: ne med aktivnim skeniranjem
    if (p->phase == VR_PHASE_DETECT
        || p->phase == VR_PHASE_SCANNING
        || p->phase == VR_PHASE_DTW_COMPUTE) {
        LOG_WARN(VR_LOG_TAG, "Calibrate %c: blokirano — dinamicna faza", parkingId);
        return false;
    }

    // Izmeri aktualne vrednosti prek sensor_mgr
    uint16_t h, p1, p2;
    if (!sensor_mgr_read_place_now(parkingId, &h, &p1, &p2)) {
        LOG_ERROR(VR_LOG_TAG, "Calibrate %c: sensor_mgr_read_place_now napaka", parkingId);
        return false;
    }
    // Preveri da niso vrednosti TOF_ERR (0xFFFF)
    if (h == TOF_ERR || p1 == TOF_ERR || p2 == TOF_ERR) {
        LOG_ERROR(VR_LOG_TAG, "Calibrate %c: TOF_ERR na vsaj enem senzorju (H=%u P1=%u P2=%u)",
                  parkingId, h, p1, p2);
        return false;
    }

    p->baseline.valid         = true;
    p->baseline.h_mm          = h;
    p->baseline.p1_mm         = p1;
    p->baseline.p2_mm         = p2;
    p->baseline.calibrated_at = (uint32_t)time(nullptr);
    save_baseline(p);
    p->state = VR_STATE_EMPTY_CALIBRATED;

    LOG_INFO(VR_LOG_TAG, "%c: baseline H=%u P1=%u P2=%u", parkingId, h, p1, p2);

    static ParkingPlaceCalibratedEvent_t ev_cal;
    ev_cal.parkingId = parkingId;
    ev_cal.h_mm = h; ev_cal.p1_mm = p1; ev_cal.p2_mm = p2;
    EventBus::publish(EventType::PARKING_PLACE_CALIBRATED, (uint32_t)&ev_cal);
    return true;
}

vr_baseline_t vehicle_recog_get_baseline(char parkingId) {
    vr_place_t* p = place_ptr(parkingId);
    if (p) return p->baseline;
    vr_baseline_t empty = {};
    return empty;
}

void vehicle_recog_on_config_changed(void) {
    const Config cfg = config_get();
    s_cfg.dtw_threshold          = cfg.dtw_threshold;
    s_cfg.sc_radius              = (uint16_t)cfg.sakoe_radius;
    s_cfg.min_points             = (uint16_t)cfg.min_profile_points;
    s_cfg.norm_points            = (uint16_t)cfg.normalize_points;
    s_cfg.raw_profiles_per_model = (uint16_t)cfg.raw_profiles_per_model;
    s_cfg.presence_check_min     = (uint16_t)cfg.presence_check_min;
    s_cfg.empty_tolerance_mm     = (uint16_t)cfg.empty_tolerance_mm;
    // Clamp na hard max
    if (s_cfg.norm_points > VR_PROFILE_NORM_POINTS)
        s_cfg.norm_points = VR_PROFILE_NORM_POINTS;
}

void vehicle_recog_reconcile_state(char parkingId,
                                   uint16_t h, uint16_t p1, uint16_t p2) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p) return;
    // Med dinamicnimi fazami ne posegamo — bi pokvarili tekoco sekvenco
    if (p->phase == VR_PHASE_DETECT
        || p->phase == VR_PHASE_SCANNING
        || p->phase == VR_PHASE_DTW_COMPUTE) return;

    bool empty_now = vehicle_recog_is_empty_reading(parkingId, h, p1, p2);
    vr_place_state_t expected;
    if (empty_now) {
        expected = p->baseline.valid
                   ? VR_STATE_EMPTY_CALIBRATED
                   : VR_STATE_EMPTY_UNCALIBRATED;
    } else {
        // Ce ze vemo kdo je tam, pustimo — ne degradiramo OCCUPIED_KNOWN
        expected = (p->state == VR_STATE_OCCUPIED_KNOWN && p->current_model_id[0])
                   ? VR_STATE_OCCUPIED_KNOWN
                   : VR_STATE_OCCUPIED_UNKNOWN;
    }

    if (expected != p->state) {
        LOG_INFO(VR_LOG_TAG, "%c: reconcile state %d->%d (H=%u P1=%u P2=%u)",
                 parkingId, (int)p->state, (int)expected, h, p1, p2);
        p->state = expected;
        if (empty_now) {
            // Pocistimo current (sensor_mgr bo poslal VEHICLE_DEPARTED po stabilnosti)
            p->current_model_id[0] = '\0';
            p->current_name[0]     = '\0';
            p->current_dtw         = NAN;
            p->park_start_ms       = 0;
            if (p->phase == VR_PHASE_PARKED) p->phase = VR_PHASE_IDLE;
        } else {
            // Vozilo prisotno brez DTW konteksta (npr. zagon z vozilom)
            if (p->phase == VR_PHASE_IDLE) p->phase = VR_PHASE_PARKED;
        }
    }
}

bool vehicle_recog_is_empty_reading(char parkingId,
                                    uint16_t h, uint16_t p1, uint16_t p2) {
    vr_place_t* p = place_ptr(parkingId);
    if (!p) return false;

    // TOF_ERR vrednosti niso "prazno" (senzor ne merI)
    if (h == TOF_ERR || p1 == TOF_ERR || p2 == TOF_ERR) return false;

    if (p->baseline.valid) {
        uint16_t tol = s_cfg.empty_tolerance_mm;
        bool h_ok  = ((uint16_t)abs((int)h  - (int)p->baseline.h_mm)  <= tol);
        bool p1_ok = ((uint16_t)abs((int)p1 - (int)p->baseline.p1_mm) <= tol);
        bool p2_ok = ((uint16_t)abs((int)p2 - (int)p->baseline.p2_mm) <= tol);
        return h_ok && p1_ok && p2_ok;
    }
    // Fallback: vse > 150 cm (dodatek pogl. 2)
    return (h  > VR_FALLBACK_EMPTY_THRESHOLD_MM
         && p1 > VR_FALLBACK_EMPTY_THRESHOLD_MM
         && p2 > VR_FALLBACK_EMPTY_THRESHOLD_MM);
}

vr_diagnostics_t vehicle_recog_get_diagnostics(void) {
    vr_diagnostics_t d = {};
    d.models_A          = s_place_A.model_count;
    d.models_B          = s_place_B.model_count;
    d.baseline_A_valid  = s_place_A.baseline.valid;
    d.baseline_B_valid  = s_place_B.baseline.valid;
    d.state_A           = s_place_A.state;
    d.state_B           = s_place_B.state;
    d.phase_A           = s_place_A.phase;
    d.phase_B           = s_place_B.phase;
    d.total_recognitions = s_total_recognitions;
    d.total_aborted     = s_total_aborted;
    d.total_new_models  = s_total_new_models;
    return d;
}
