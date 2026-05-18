// ============================================================
// hal_tof.h — VL53L1X TOF senzorji prek TCA9548A I2C MUX
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-04
// ============================================================
//
// ARHITEKTURA — sinhroni polling iz sensorTask:
//
//   sensorTask kliče hal_tof_tick() v zanki.
//   hal_tof_tick() regulira timing z millis() primerjavami —
//   nikoli ne blokira z vTaskDelay(). Kadenca klicanja je odvisna
//   od faze:
//
//     IDLE     → tick() vsakih TOF_POLL_IDLE_MS   (150 ms)
//     DETECT   → tick() vsakih TOF_POLL_DETECT_MS  (90 ms)
//     SCANNING → tick() naravno — čim je prejšnji I2C cikel zaključen
//
//   sensor_mgr.cpp je odgovoren za kadenco klicanja tick().
//
// WIRE1 MUTEX IN PRIORITETA:
//
//   hal_tof ima VIŠJO prioriteto do Wire1 busa od hal_radar.
//   Mutex se vzame z DALJŠIM timeoutom (200 ms) — radarTask bo
//   počakal. hal_tof mutex timeout je namenoma visok ker je TOF
//   operacija časovno kritična med skeniranjem profila vozila.
//
//   radarTask (prio 4) in sensorTask (prio 4) sta enake prioritete,
//   a TOF skeniranje je bounded (~120 ms na cikel) in radar bo dobil
//   mutex takoj po zaključku TOF cikla. V praksi se konflikti
//   pojavljajo redko — radar dobi IRQ ~10x/s, TOF cikel ~120 ms.
//
// TCA9548A PROTOKOL:
//
//   Vsaka meritev = odpri kanal → meri → TAKOJ zapri (0x00).
//   Kanal nikoli ne ostane odprt med meritvami — preprečimo
//   kolizije z drugimi napravami na Wire1 busu.
//
//   Reset pin IO46 se uporabi samo ob recovery — ne med normalnim
//   delovanjem. bsp_tca_reset() je blokirajoč (~15 ms).
//
// VL53L1X KONFIGURACIJA:
//
//   Vsi senzorji: TOF400C (VL53L1XN), Long Distance mode, timing budget 33 ms.
//     setDistanceMode(Long) + setMeasurementTimingBudget(33000) → do ~4 m.
//     setBus(&Wire1) je obvezen pred init() — Pololu lib privzeto dela na Wire.
//
// FAZNI AVTOMAT:
//
//   IDLE     → oba H senzorja vsake 150 ms (pasivno, ne gradi profila)
//   DETECT   → oba H senzorja vsake 90 ms (aktiven, čaka trigger)
//   SCANNING → H + P1 + P2 aktivnega mesta, SMART Δ-filter
//   DTW_WAIT → miruje med DTW burst-om na Core1
//
//   Prehodi:
//     IDLE    → DETECT   : hal_tof_startDetect() — kliče event_bus ob rampagor
//     DETECT  → SCANNING : samodejno ob detekciji (H < VEH_ENTRY_THRESH_MM)
//     SCANNING→ DTW_WAIT : samodejno ob stabilnosti (VEH_STABLE_TIME_MS)
//              ali max točkah (120)
//     DTW_WAIT→ IDLE     : hal_tof_stopScan() — kliče vehicle_recog po DTW
//     katerakoli → IDLE  : hal_tof_stopScan() (reset ob napaki)
//
// FIZIČNE POVEZAVE:
//
//   TCA9548A @ 0x70 (A0=A1=A2=GND) na Wire1 (IO17/IO18)
//   Reset pin: IO46 (aktiven LOW) → bsp_tca_reset()
//
//   CH0  H_A  : zadnja stena, ~50 cm od tal, horizontalno, TOF400C
//   CH1  P1_A : strop, ~4 m od stene, navzdol, TOF400C
//   CH2  P2_A : strop, ~2 m od stene, navzdol, TOF400C
//   CH3  H_B  : zadnja stena, ~50 cm od tal, horizontalno, TOF400C
//   CH4  P1_B : strop, ~4 m od stene, navzdol, TOF400C
//   CH5  P2_B : strop, ~2 m od stene, navzdol, TOF400C
//   CH6–CH7   : rezerva
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// KONSTANTE
// ============================================================

// Vrednost ki pomeni "napaka / ni meritve"
#define TOF_ERR             0xFFFF

// Fizični doseg senzorjev (za validacijo meritev)
// H senzorji (TOF400C): max ~4000 mm v Long Range mode
// P senzorji (TOF200C): max ~1200 mm privzeto
#define TOF_MIN_RANGE_MM    20      // pod tem je meritev neveljavna (odraz)
#define TOF_MAX_RANGE_MM    4500    // nad tem senzor ne meri zanesljivo

// ============================================================
// ENUMERACIJE
// ============================================================

// Parkirno mesto
typedef enum : uint8_t {
    TOF_PLACE_A = 0,
    TOF_PLACE_B = 1
} TofPlace;

// Faze faznega avtomata
typedef enum : uint8_t {
    TOF_PHASE_IDLE     = 0,   // mirovanje — samo H senzorja vsake 100 ms
    TOF_PHASE_DETECT   = 1,   // detekcija vstopa — H senzorja vsake 40 ms
    TOF_PHASE_SCANNING = 2,   // skeniranje profila — H+P1+P2 aktivnega mesta
    TOF_PHASE_DTW_WAIT = 3    // čakanje — DTW burst teče na Core1
} TofPhase;

// Tip senzorja (za interno mapiranje kanal→indeks)
typedef enum : uint8_t {
    TOF_SENSOR_H  = 0,   // horizontalni — trigger + razdalja
    TOF_SENSOR_P1 = 1,   // profilni 1 — prednja polovica
    TOF_SENSOR_P2 = 2    // profilni 2 — zadnja polovica
} TofSensorType;

// ============================================================
// PODATKOVNE STRUKTURE
// ============================================================

// Ena točka profila (3D meritev ob enem trenutku)
// Ref: Identifikacija_Vozil_v1.1.docx sekcija 3
typedef struct {
    uint16_t H_mm;      // horizontalna razdalja [mm] — razdalja do tablice
    uint16_t P1_mm;     // profilna 1 [mm] — hauba/sprednja streha
    uint16_t P2_mm;     // profilna 2 [mm] — prtljažnik/zadnja streha
    uint32_t ts_ms;     // millis() ob meritvi — za DTW timing
} TofProfilePoint;

// Zaključen profil vozila — posredovan v callback ob koncu skeniranja
// Ref: Identifikacija_Vozil_v1.1.docx sekcija 3.1
typedef struct {
    TofPlace         place;             // TOF_PLACE_A ali TOF_PLACE_B
    TofProfilePoint  points[120];       // max 120 točk (zaščitna meja)
    uint8_t          count;             // dejansko število točk
    uint32_t         scan_start_ms;     // millis() ob začetku skeniranja
    uint32_t         scan_duration_ms;  // trajanje skeniranja [ms]
    uint32_t         timestamp_ms;      // millis() ob zaključku
} TofProfileResult;

// Diagnostika — za servisni zaslon in web UI
typedef struct {
    bool     sensor_ok[6];         // kateri kanali so inicializirani
    uint16_t last_mm[6];           // zadnja uspešna meritev [mm] per kanal
    uint32_t error_count[6];       // skupaj napak per kanal od zagona
    uint32_t recovery_count;       // skupaj recovery akcij od zagona
    uint32_t last_recovery_ms;     // millis() zadnje recovery akcije
    TofPhase current_phase;        // trenutna faza avtomata
    TofPlace active_place;         // aktivno parkirno mesto (veljavno samo v SCANNING)
    uint8_t  profile_pts;          // število točk v tekočem profilu
    uint32_t i2c_cycle_ms_last;    // trajanje zadnjega 3-senzorskega cikla [ms]
    uint32_t i2c_cycle_ms_avg;     // tekoče povprečje I2C cikla [ms]
} TofDiagnostics;

// ============================================================
// CALLBACK TIP
// ============================================================

// Pokliče se ob zaključku profila (SCANNING → DTW_WAIT).
// Kliče se iz sensorTask konteksta (Core1, ne ISR).
// Implementirano v sensor_mgr.cpp → EventBus publish TOF_PROFILE_READY.
// POZOR: profil je na stacku hal_tof — callback mora podatke kopirati
//        ali jih takoj obdelati (npr. shraniti v EventBus payload).
typedef void (*TofProfileCallback)(const TofProfileResult& profile);

// ============================================================
// JAVNE FUNKCIJE — inicializacija
// ============================================================

// Inicializacija TCA9548A in vseh 6 VL53L1X senzorjev.
// Kliči iz sensor_mgr_init() po Wire1.begin().
//
// Vrne true če vsaj eden od H senzorjev (CH0 ali CH3) deluje.
// Delna inicializacija (nekateri kanali ne odgovarjajo) je normalna
// za fazo razvoja — napake se logirajo, delovanje nadaljuje.
//
// Predpogoj: Wire1 mora biti inicializiran (bsp_wire1_ok() == true).
bool hal_tof_init();

// Preveri ali je HAL inicializiran in vsaj en H senzor deluje.
bool hal_tof_ok();

// Enkratna startup meritev vseh 6 kanalov — kliči iz sensorTask po ~5s stabilizaciji.
// Posodobi s_last_mm[] za vse kanale, kar je potrebno za diagnostiko in servisni zaslon.
// Vrne true ob uspešnem pridobivanju Wire1 mutex-a in izvedbi meritev.
// Vrne false ob mutex timeout (Wire1 zaseden) — v tem primeru sensor_mgr ponovi klic.
bool hal_tof_startup_scan();

// ============================================================
// JAVNE FUNKCIJE — fazni avtomat
// ============================================================

// Sproži prehod IDLE/DTW_WAIT → DETECT.
// Kliči ob dvigu rampe (rampagor event iz MCP23017 IRQ).
// Če je fazni avtomat že v SCANNING — klic se ignorira (logira WARN).
void hal_tof_startDetect();

// Resetiraj fazni avtomat → IDLE in počisti profil.
// Kliči iz vehicle_recog po zaključku DTW burst-a.
// Kliči tudi ob alarmnem resetu ali izhodu vozila brez skeniranja.
void hal_tof_stopScan();

// Vrne trenutno fazo avtomata (za sensor_mgr timing in diagnostiko).
TofPhase hal_tof_getPhase();

// Vrne aktivno parkirno mesto (veljavno samo v SCANNING fazi).
TofPlace hal_tof_getActivePlace();

// ============================================================
// JAVNE FUNKCIJE — tick (kliči iz sensorTask)
// ============================================================

// Izvede en korak faznega avtomata.
// Ne vsebuje vTaskDelay() — timing regulira klic iz sensor_mgr.
// Pridobi Wire1 mutex pred vsako I2C operacijo.
// Varno za klic kadarkoli — vrne takoj če ni inicializiran.
void hal_tof_tick();

// ============================================================
// JAVNE FUNKCIJE — callback registracija
// ============================================================

// Registriraj callback za TOF_PROFILE_READY event.
// Kliči iz sensor_mgr_init() pred prvim hal_tof_tick().
// nullptr → odregistracija callbacka.
void hal_tof_setProfileCallback(TofProfileCallback cb);

// ============================================================
// JAVNE FUNKCIJE — neposredne meritve (za diagnostiko)
// ============================================================

// Preberi posamezen senzor neposredno (zunaj faznega avtomata).
// Pridobi Wire1 mutex. Vrne TOF_ERR ob napaki ali nedosegljivi razdalji.
// Uporabi samo iz servisnega zaslona ali diagnostičnih funkcij —
// ne kliči med normalnim delovanjem faznega avtomata.
uint16_t hal_tof_readH (TofPlace place);
uint16_t hal_tof_readP1(TofPlace place);
uint16_t hal_tof_readP2(TofPlace place);

// Preberi vse 3 senzorje enega mesta v enem bloku (z mutex).
// Vrne strukturo z vsemi tremi vrednostmi in timestamp.
TofProfilePoint hal_tof_readAll(TofPlace place);

// ============================================================
// JAVNE FUNKCIJE — diagnostika
// ============================================================

// Vrne snapshot diagnostičnih podatkov.
TofDiagnostics hal_tof_getDiagnostics();

// Ročni reinit enega kanala (za servisni zaslon).
// Interno sproži recovery sekvenco za ta kanal.
bool hal_tof_reinitChannel(uint8_t channel);

// Ročni reinit vseh kanalov (TCA reset + reinit vsega).
bool hal_tof_reinitAll();

// Dump statistike na Logger — za periodično beleženje.
void hal_tof_logStats();
