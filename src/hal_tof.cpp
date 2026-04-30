// ============================================================
// hal_tof.cpp — VL53L1X TOF senzorji prek TCA9548A I2C MUX
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-04
// ============================================================
//
// ODGOVORNOST:
//   HAL za 6× VL53L1X TOF senzorjev prek TCA9548A na Wire1.
//   Gradi profil vozila med parkiranjem za DTW prepoznavo.
//   Glej hal_tof.h za celoten opis arhitekture in API.
//
// KLJUČNE IMPLEMENTACIJSKE ODLOČITVE:
//
//   Wire1 mutex — višja prioriteta kot radar:
//     Vsak Wire1 dostop (read_channel, init_channel, recovery) vzame
//     mutex z timeoutom WIRE1_MUTEX_TIMEOUT_MS (200 ms). radarTask
//     ima enak timeout (50 ms) → TOF bo vzel mutex in radar bo počakal.
//     V praksi: TOF cikel ~90 ms, radar IRQ ~10x/s → konflikti redki.
//     Mutex se vedno sprosti pred return iz vsake funkcije.
//
//   TCA9548A channel discipline:
//     Open → meri → close (0x00) v eni atomski sekvenci pod mutex.
//     Kanal nikoli ni odprt med dvema meritvama. tca_close_all()
//     se kliče ob vsakem izhodu iz read_channel() ne glede na uspeh.
//
//   Robustnost pri manjkajočih senzorjih:
//     init_channel() napako logira kot WARN, ne FATAL — sistem
//     nadaljuje z degradiranimi zmogljivostmi. s_sensor_ok[ch]=false
//     pomeni da read_channel() za ta kanal vrne TOF_ERR takoj
//     (brez Wire1 prometa). hal_tof_ok() zahteva vsaj en H senzor.
//
//   I2C recovery — konzervativna strategija:
//     Wire1.end() se NE kliče — bus delijo hal_radar, hal_light,
//     hal_gpio. Recovery = bsp_tca_reset() (IO46 LOW→HIGH) + TCA
//     ping preverjanje + reinit samo problematičnega kanala.
//     Recovery se sproži po TOF_RECOVERY_RETRIES (3) zaporednih napakah.
//     Med recovery je mutex držan — preprečimo da bi drug modul
//     dostopal do Wire1 med TCA resetom.
//
//   Long Distance mode — vsi kanali (VL53L1X, TOF400C, do ~4 m):
//     setDistanceMode(VL53L1X::Long) + setMeasurementTimingBudget(33000 µs).
//     setBus(&Wire1) obvezen pred init() — Pololu lib privzeto dela na Wire.
//
//   SMART vzorčenje (Δ-filter):
//     Točka se doda profilu samo če je |ΔH| ≥ VEH_DELTA_FILTER_MM.
//     Ko avto miruje (ΔH < filter) → stabilnost timer → finalize.
//     Prva točka se vedno shrani (referenca za Δ).
//
//   finalize_profile() scan_duration_ms:
//     scan_start_ms je nastavljen ob začetku SCANNING (start_scanning).
//     scan_duration_ms = millis() - scan_start_ms ob finalize.
//     (referenca je imela bug: timestamp nastavljen tik pred odštevanjem)
//
// LOGGING:
//   LOG_INFO/WARN/ERROR/DEBUG("TOF", ...) — centralni logger.
//   DEBUG level za I2C cikel meritve in per-točko log v SCANNING.
//   INFO za fazne prehode in statistiko.
//   WARN za napake kanalov in recovery.
//   ERROR samo za kritične napake inicializacije.
//
// ============================================================

#include "hal_tof.h"
#include "bsp.h"
#include "logger.h"
#include "config.h"
#include <Wire.h>
#include <VL53L1X.h>
#include <freertos/semphr.h>

// ============================================================
// LOGGING MAKROJI
// ============================================================

#define TOFI(fmt, ...) LOG_INFO ("TOF", fmt, ##__VA_ARGS__)
#define TOFW(fmt, ...) LOG_WARN ("TOF", fmt, ##__VA_ARGS__)
#define TOFE(fmt, ...) LOG_ERROR("TOF", fmt, ##__VA_ARGS__)
#define TOFD(fmt, ...) LOG_DEBUG("TOF", fmt, ##__VA_ARGS__)

// ============================================================
// INTERNE KONSTANTE
// ============================================================

// Wire1 mutex timeout — namenoma višji od radarTask timeouata (50 ms)
// da TOF dobi prednost pri konfliktu
#define WIRE1_MUTEX_TIMEOUT_MS      200

// VL53L1X timing budget [µs] — minimum 20000 za Long mode.
// read_channel(): budget + 5ms overhead + I2C = ~40 ms na meritev.
// SCANNING (3 meritve): ~120 ms/cikel. IDLE/DETECT (2 meritve): ~80 ms.
#define TOF_TIMING_BUDGET_US        33000

// Max točk v profilu — zaščitna meja
#define TOF_PROFILE_MAX_PTS         120

// ============================================================
// INTERNO STANJE
// ============================================================

// VL53L1X instance — ena na kanal, kanali 6 in 7 niso inicializirani
static VL53L1X s_tof[6];

// Status kanalov
static bool     s_sensor_ok[6]    = {false, false, false, false, false, false};
static uint16_t s_last_mm[6]      = {TOF_ERR, TOF_ERR, TOF_ERR, TOF_ERR, TOF_ERR, TOF_ERR};
static uint32_t s_error_count[6]  = {0, 0, 0, 0, 0, 0};
static uint8_t  s_consec_err[6]   = {0, 0, 0, 0, 0, 0};

// Recovery tracking
static uint32_t s_recovery_count   = 0;
static uint32_t s_last_recovery_ms = 0;

// Fazni avtomat
static TofPhase s_phase        = TOF_PHASE_IDLE;
static TofPlace s_active_place = TOF_PLACE_A;   // veljavno samo v SCANNING

// Profil ki se gradi med SCANNING
static TofProfileResult s_profile;
static bool             s_profile_active = false;

// SMART vzorčenje
static uint16_t s_last_H_mm       = TOF_ERR;
static bool     s_in_stable        = false;
static uint32_t s_stable_start_ms  = 0;

// I2C cikel meritve (za diagnostiko in kalibriranje timing-a)
static uint32_t s_cycle_ms_last  = 0;
static uint32_t s_cycle_ms_sum   = 0;
static uint32_t s_cycle_ms_count = 0;

// Callback za TOF_PROFILE_READY
static TofProfileCallback s_profile_cb = nullptr;

// Watchdog timer — zadnji čas IDLE health-check meritve
// Inicializirano na 0 → prva meritev pride takoj ob zagonu (po ~0ms)
static uint32_t s_last_watchdog_ms = 0;

// Inicializiran flag
static bool s_initialized = false;

// ============================================================
// POMOŽNE FUNKCIJE — mapiranje kanalov
// ============================================================

// Pretvori TofPlace + TofSensorType v TCA kanal (0–5)
static inline uint8_t place_sensor_to_ch(TofPlace place, TofSensorType type) {
    return (place == TOF_PLACE_A)
        ? (uint8_t)type           // CH0=H_A, CH1=P1_A, CH2=P2_A
        : 3 + (uint8_t)type;     // CH3=H_B, CH4=P1_B, CH5=P2_B
}

// Bazni indeks za TofPlace (A→0, B→3)
static inline uint8_t place_base(TofPlace place) {
    return (place == TOF_PLACE_A) ? 0 : 3;
}

// Ime kanala za logging
static const char* ch_name(uint8_t ch) {
    switch (ch) {
        case 0: return "H_A(CH0)";
        case 1: return "P1_A(CH1)";
        case 2: return "P2_A(CH2)";
        case 3: return "H_B(CH3)";
        case 4: return "P1_B(CH4)";
        case 5: return "P2_B(CH5)";
        default: return "??";
    }
}

// ============================================================
// TCA9548A PRIMITIVI — kliči samo pod Wire1 mutex
// ============================================================

// Zapri vse TCA kanale (zapiši 0x00)
// Kliči po vsaki meritvi in ob recovery. Ignoriraj napako — bus
// je morda že v slabem stanju in recovery bo sledil.
static void tca_close_all() {
    Wire1.beginTransmission(I2C_ADDR_TCA9548A);
    Wire1.write(0x00);
    uint8_t err = Wire1.endTransmission();
    if (err != 0) {
        TOFW("TCA close_all napaka (err=%d) — nadaljujem", err);
    }
}

// Odpri en kanal (0–7). Vrne true ob uspehu.
static bool tca_open(uint8_t channel) {
    if (channel > 7) return false;
    Wire1.beginTransmission(I2C_ADDR_TCA9548A);
    Wire1.write((uint8_t)(1 << channel));
    return (Wire1.endTransmission() == 0);
}

// Preveri ali TCA9548A odgovarja na I2C ping
static bool tca_ping() {
    Wire1.beginTransmission(I2C_ADDR_TCA9548A);
    return (Wire1.endTransmission() == 0);
}

// ============================================================
// I2C RECOVERY — kliči samo pod Wire1 mutex
// ============================================================
// Konzervativna strategija — brez Wire1.end() ker bus delijo drugi HAL moduli.
// Strategija: TCA hardware reset (IO46) → TCA ping → reinit kanala.
// Ob neuspehu: s_sensor_ok[channel] = false, sistem teče dalje.

static bool do_recovery(uint8_t channel) {
    TOFW("Recovery kanal %s (skupaj napak: %lu, recovery #%lu)...",
         ch_name(channel),
         (unsigned long)s_error_count[channel],
         (unsigned long)(s_recovery_count + 1));

    // Korak 1: Zapri vse TCA kanale
    tca_close_all();
    delay(5);

    // Korak 2: Hardware reset TCA9548A prek IO46
    // bsp_tca_reset() = LOW(5ms) → HIGH → delay(TCA_RECOVERY_WAIT_MS)
    bsp_tca_reset();

    // Korak 3: TCA ping — ali TCA odgovarja po resetu?
    if (!tca_ping()) {
        TOFE("Recovery: TCA9548A ne odgovarja po resetu! (kanal %s)",
             ch_name(channel));
        s_sensor_ok[channel]  = false;
        s_consec_err[channel] = 0;
        s_recovery_count++;
        s_last_recovery_ms = millis();
        return false;
    }
    TOFD("Recovery: TCA9548A OK po resetu");

    // Korak 4: Reinit samo tega kanala
    bool ok = false;

    if (tca_open(channel)) {
        delay(2);
        s_tof[channel].setBus(&Wire1);
        ok = s_tof[channel].init();
        if (ok) {
            s_tof[channel].setTimeout(TOF_I2C_TIMEOUT_MS);
            s_tof[channel].setDistanceMode(VL53L1X::Long);
            s_tof[channel].setMeasurementTimingBudget(TOF_TIMING_BUDGET_US);
        } else {
            TOFW("Recovery: VL53L1X reinit napaka na kanalu %s", ch_name(channel));
        }
    } else {
        TOFW("Recovery: TCA open napaka za kanal %s", ch_name(channel));
    }

    tca_close_all();

    s_sensor_ok[channel]  = ok;
    s_consec_err[channel] = 0;
    s_recovery_count++;
    s_last_recovery_ms = millis();

    if (ok) {
        TOFI("Recovery OK — kanal %s reinicializiran", ch_name(channel));
    } else {
        TOFW("Recovery NAPAKA — kanal %s onemogočen do naslednjega reinita",
             ch_name(channel));
    }
    return ok;
}

// ============================================================
// INICIALIZACIJA ENEGA KANALA — kliči samo pod Wire1 mutex
// ============================================================

static bool init_channel(uint8_t channel) {
    TOFD("Init kanal %s...", ch_name(channel));

    if (!tca_open(channel)) {
        TOFW("Init kanal %s: TCA open napaka — senzor morda ni priključen",
             ch_name(channel));
        tca_close_all();
        return false;
    }

    // VL53L1X potrebuje ~1.2 ms boot čas po TCA open
    delay(2);

    // KRITIČNO: setBus() mora biti klican PRED init()
    // Pololu VL53L1X privzeto uporablja Wire — mi rabimo Wire1
    s_tof[channel].setBus(&Wire1);

    bool ok = s_tof[channel].init();
    if (!ok) {
        TOFW("Init kanal %s: VL53L1X.init() napaka (I2C err=%d) — senzor morda ni priključen",
             ch_name(channel), s_tof[channel].last_status);
        tca_close_all();
        return false;
    }

    s_tof[channel].setTimeout(TOF_I2C_TIMEOUT_MS);

    if (!s_tof[channel].setDistanceMode(VL53L1X::Long)) {
        TOFW("Init kanal %s: setDistanceMode(Long) napaka — nadaljujem s privzetim",
             ch_name(channel));
    }

    if (!s_tof[channel].setMeasurementTimingBudget(TOF_TIMING_BUDGET_US)) {
        TOFW("Init kanal %s: setMeasurementTimingBudget napaka", ch_name(channel));
    }

    tca_close_all();
    TOFI("Kanal %s init OK [Long, budget=%d µs]", ch_name(channel), TOF_TIMING_BUDGET_US);
    return true;
}

// ============================================================
// MERITEV ENEGA KANALA — kliči samo pod Wire1 mutex
// ============================================================
// Vzorec (opcija B): vse pod odprtim TCA kanalom:
//   tca_open → startContinuous(0) → read(true) → stopContinuous() → tca_close_all.
// startContinuous počisti pending interrupt flag + zažene meritev.
// read(true) blokira ~33 ms, atomsko čaka dataReady + prebere + clearInterrupt.
// stopContinuous resetira VHV kalibracijski flag za naslednji cikel.
// Skupaj ~40 ms na meritev.

static uint16_t read_channel(uint8_t channel) {
    if (channel >= 6)          return TOF_ERR;
    if (!s_sensor_ok[channel]) return TOF_ERR;

    if (!tca_open(channel)) {
        s_error_count[channel]++;
        s_consec_err[channel]++;
        TOFW("read_channel %s: TCA open napaka (konsek: %d)",
             ch_name(channel), s_consec_err[channel]);
        tca_close_all();
        if (s_consec_err[channel] >= TOF_RECOVERY_RETRIES) {
            do_recovery(channel);
        }
        return TOF_ERR;
    }

    // VL53L1X zahteva inter-measurement period >= timing budget.
    // startContinuous(0) je neveljavno (vrne mm=0 takoj) — potrebujemo >=38ms.
    s_tof[channel].startContinuous(TOF_TIMING_BUDGET_US / 1000 + 5);

    // read(true) — blokira ~33 ms, atomsko čaka dataReady + bere + clearInterrupt
    uint16_t mm = s_tof[channel].read(true);

    // stopContinuous() — resetira VHV kalibracijski flag pred zaprtjem kanala
    s_tof[channel].stopContinuous();

    tca_close_all();

    if (s_tof[channel].last_status != 0) {
        s_error_count[channel]++;
        s_consec_err[channel]++;
        TOFW("read_channel %s: I2C napaka last_status=%d (konsek: %d)",
             ch_name(channel),
             (int)s_tof[channel].last_status,
             s_consec_err[channel]);
        if (s_consec_err[channel] >= TOF_RECOVERY_RETRIES) {
            do_recovery(channel);
        }
        return TOF_ERR;
    }

    if (s_tof[channel].ranging_data.range_status != VL53L1X::RangeValid) {
        TOFD("read_channel %s: RangeStatus=%d mm=%d (ni veljavnega objekta)",
             ch_name(channel),
             (int)s_tof[channel].ranging_data.range_status,
             mm);
        s_consec_err[channel] = 0;
        return TOF_ERR;
    }

    if (mm < TOF_MIN_RANGE_MM || mm > TOF_MAX_RANGE_MM) {
        TOFD("read_channel %s: mm=%d izven dosega [%d-%d]",
             ch_name(channel), mm, TOF_MIN_RANGE_MM, TOF_MAX_RANGE_MM);
        s_consec_err[channel] = 0;
        return TOF_ERR;
    }

    s_consec_err[channel] = 0;
    s_last_mm[channel]    = mm;
    return mm;
}

// ============================================================
// ZAKLJUČEK PROFILA
// ============================================================

static void finalize_profile() {
    // Preveri minimalno število točk
    if (s_profile.count < VEH_MIN_PROFILE_PTS) {
        TOFW("Profil zavrnjen — premalo točk (%d < %d) — nazaj v DETECT",
             s_profile.count, VEH_MIN_PROFILE_PTS);
        // Lažni vstop ali prekinitev — resetiramo in čakamo znova
        s_profile_active       = false;
        s_profile.count        = 0;
        s_last_H_mm            = TOF_ERR;
        s_in_stable            = false;
        s_phase                = TOF_PHASE_DETECT;
        return;
    }

    // Zapiši čase
    uint32_t now                   = millis();
    s_profile.timestamp_ms         = now;
    s_profile.scan_duration_ms     = now - s_profile.scan_start_ms;

    TOFI("Profil zaključen — mesto:%c točke:%d trajanje:%lu ms",
         (s_profile.place == TOF_PLACE_A) ? 'A' : 'B',
         s_profile.count,
         (unsigned long)s_profile.scan_duration_ms);

    // Pokliči callback (sensor_mgr → EventBus publish TOF_PROFILE_READY)
    if (s_profile_cb != nullptr) {
        s_profile_cb(s_profile);
    } else {
        TOFW("TOF_PROFILE_READY — callback ni registriran!");
    }

    // Prestavi v DTW_WAIT — ne beremo senzorjev med DTW burst-om
    s_phase          = TOF_PHASE_DTW_WAIT;
    s_profile_active = false;
    s_in_stable      = false;
}

// ============================================================
// ZAČETEK SKENIRANJA
// ============================================================

static void start_scanning(TofPlace place) {
    TOFI("Začetek skeniranja — mesto %c",
         (place == TOF_PLACE_A) ? 'A' : 'B');

    s_active_place         = place;
    s_phase                = TOF_PHASE_SCANNING;
    s_profile_active       = true;
    s_in_stable            = false;
    s_last_H_mm            = TOF_ERR;
    s_stable_start_ms      = 0;

    s_profile.place        = place;
    s_profile.count        = 0;
    s_profile.scan_start_ms = millis();
    s_profile.timestamp_ms  = 0;
    s_profile.scan_duration_ms = 0;
}

// ============================================================
// JAVNE FUNKCIJE — inicializacija
// ============================================================

bool hal_tof_init() {
    TOFI("=== hal_tof_init ===");
    TOFI("TCA9548A @ 0x%02X | Wire1 SDA=IO%d SCL=IO%d @ %d Hz",
         I2C_ADDR_TCA9548A, I2C_SDA, I2C_SCL, I2C_FREQ_HZ);

    // Predpogoj: Wire1 mora biti inicializiran
    if (!bsp_wire1_ok()) {
        TOFE("Wire1 ni inicializiran — bsp_init() mora biti klican prej!");
        return false;
    }

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        TOFE("Wire1 mutex timeout pri init — napaka BSP?");
        return false;
    }

    // Preveri prisotnost TCA9548A
    if (!tca_ping()) {
        xSemaphoreGive(mtx);
        TOFE("TCA9548A (0x%02X) ne odgovarja — preveriti napajanje, pull-up (4.7kΩ), pini IO%d/IO%d",
             I2C_ADDR_TCA9548A, I2C_SDA, I2C_SCL);
        return false;
    }
    TOFI("TCA9548A ping OK");

    // Zapri vse kanale pred inicializacijo (clean state)
    tca_close_all();
    delay(5);

    // Inicializiraj vseh 6 kanalov — napaka na kanalu ni fatalna
    int ok_count = 0;
    for (uint8_t ch = 0; ch < 6; ch++) {
        s_sensor_ok[ch]   = init_channel(ch);
        s_error_count[ch] = 0;
        s_consec_err[ch]  = 0;
        s_last_mm[ch]     = TOF_ERR;
        if (s_sensor_ok[ch]) {
            ok_count++;
        } else {
            TOFW("Kanal %s ni inicializiran — sistem teče z degradiranimi zmogljivostmi",
                 ch_name(ch));
        }
        delay(5);  // kratka pavza med kanali za stabilizacijo
    }

    xSemaphoreGive(mtx);

    // Poroči stanje vseh kanalov
    TOFI("TOF init rezultati: %d/6 kanalov OK", ok_count);
    for (uint8_t ch = 0; ch < 6; ch++) {
        TOFI("  %s: %s", ch_name(ch), s_sensor_ok[ch] ? "OK ✓" : "NAPAKA ✗");
    }

    // Minimalni pogoj: vsaj en H senzor mora delovati
    bool h_a_ok = s_sensor_ok[TCA_CH_TOF_H_A];
    bool h_b_ok = s_sensor_ok[TCA_CH_TOF_H_B];
    if (!h_a_ok && !h_b_ok) {
        TOFE("Nobeden H senzor ne deluje (H_A=%s H_B=%s) — identifikacija vozil onemogočena!",
             h_a_ok ? "OK" : "NAPAKA",
             h_b_ok ? "OK" : "NAPAKA");
        // Ne vrnemo false — sistem teče, identifikacija je samo onemogočena
    } else if (!h_a_ok || !h_b_ok) {
        TOFW("En H senzor ne deluje (H_A=%s H_B=%s) — eno parkirno mesto onemogočeno",
             h_a_ok ? "OK" : "NAPAKA",
             h_b_ok ? "OK" : "NAPAKA");
    }

    // Inicializiraj fazni avtomat
    s_phase          = TOF_PHASE_IDLE;
    s_profile_active = false;
    s_last_H_mm      = TOF_ERR;
    s_in_stable      = false;
    s_initialized    = true;

    TOFI("hal_tof_init OK — faza: IDLE | %d/6 kanalov aktivnih", ok_count);
    return (ok_count > 0);
}

bool hal_tof_ok() {
    return s_initialized &&
           (s_sensor_ok[TCA_CH_TOF_H_A] || s_sensor_ok[TCA_CH_TOF_H_B]);
}

// ============================================================
// JAVNE FUNKCIJE — fazni avtomat
// ============================================================

void hal_tof_startDetect() {
    if (!s_initialized) return;

    if (s_phase == TOF_PHASE_IDLE || s_phase == TOF_PHASE_DTW_WAIT) {
        TOFI("Faza: %s → DETECT",
             (s_phase == TOF_PHASE_IDLE) ? "IDLE" : "DTW_WAIT");
        s_phase = TOF_PHASE_DETECT;
    } else if (s_phase == TOF_PHASE_SCANNING) {
        TOFW("startDetect() med SCANNING — ignoriram");
    }
    // DETECT → DETECT: ignorirano (tiho)
}

void hal_tof_stopScan() {
    if (!s_initialized) return;

    const char* from =
        (s_phase == TOF_PHASE_SCANNING) ? "SCANNING" :
        (s_phase == TOF_PHASE_DETECT)   ? "DETECT"   :
        (s_phase == TOF_PHASE_DTW_WAIT) ? "DTW_WAIT" : "IDLE";

    TOFI("Faza: %s → IDLE (stopScan)", from);

    s_phase          = TOF_PHASE_IDLE;
    s_profile_active = false;
    s_profile.count  = 0;
    s_last_H_mm      = TOF_ERR;
    s_in_stable      = false;
    s_stable_start_ms = 0;
}

TofPhase hal_tof_getPhase() {
    return s_phase;
}

TofPlace hal_tof_getActivePlace() {
    return s_active_place;
}

// ============================================================
// JAVNE FUNKCIJE — callback
// ============================================================

void hal_tof_setProfileCallback(TofProfileCallback cb) {
    s_profile_cb = cb;
    TOFI("Profile callback: %s", cb ? "registriran" : "odregistriran (nullptr)");
}

// ============================================================
// JAVNE FUNKCIJE — tick (fazni avtomat)
// ============================================================

void hal_tof_tick() {
    if (!s_initialized) return;

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();

    switch (s_phase) {

    // ----------------------------------------------------------
    // IDLE — mirovanje
    // ----------------------------------------------------------
    case TOF_PHASE_IDLE:
    {
        // ----------------------------------------------------------------
        // FAZA 0 — MIROVANJE
        // V tej fazi TOF ne meri. Wire1 je popolnoma prost za radarTask.
        //
        // Edina aktivnost: watchdog meritev vsakih TOF_WATCHDOG_INTERVAL_MS
        // (10 minut). Namen:
        //   1. Potrditev da senzorji in Wire1 bus delujejo
        //   2. Pasivna detekcija anomalij (avto brez rampe, fizična okvara)
        //   3. Periodični health-check log za diagnostiko
        //
        // TODO (Faza logike): Če watchdog zazna H < VEH_ENTRY_THRESH_MM
        //   na kateremkoli H senzorju → logirati kot anomalijo (avto morda
        //   parkiran brez normalnega postopka). Ne sprožati DETECT faze
        //   avtomatsko — to je naloga rampagor IRQ triggerja.
        // ----------------------------------------------------------------

        uint32_t now = millis();
        if (now - s_last_watchdog_ms < (uint32_t)TOF_WATCHDOG_INTERVAL_MS) {
            // Ni čas za watchdog — vrni takoj, Wire1 prost za radar
            break;
        }
        s_last_watchdog_ms = now;

        // Watchdog meritev — oba H senzorja
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            TOFW("IDLE watchdog: Wire1 mutex timeout — preskočeno");
            break;
        }
        uint16_t h_a = read_channel(TCA_CH_TOF_H_A);
        uint16_t h_b = read_channel(TCA_CH_TOF_H_B);
        xSemaphoreGive(mtx);

        // Watchdog log — vedno na INFO nivoju (ne DEBUG) da je vidno v normalnem logu
        TOFI("=== IDLE watchdog === H_A:%s H_B:%s | uptime:%lus | bus:OK",
             (h_a == TOF_ERR) ? "ERR" : String(h_a).c_str(),
             (h_b == TOF_ERR) ? "ERR" : String(h_b).c_str(),
             (unsigned long)(now / 1000));

        // Opozorilo ob napaki senzorja
        if (h_a == TOF_ERR) TOFW("IDLE watchdog: H_A napaka — senzor morda odpovedal");
        if (h_b == TOF_ERR) TOFW("IDLE watchdog: H_B napaka — senzor morda odpovedal");

        break;
    }

    // ----------------------------------------------------------
    // DETECT — čakanje na vstop vozila
    // ----------------------------------------------------------
    case TOF_PHASE_DETECT:
    {
        // ----------------------------------------------------------------
        // FAZA 1 — DETEKCIJA MESTA
        // Trigger: hal_tof_startDetect() ob rampagor LOW IRQ iz MCP23017.
        // Meri samo H_A (CH0) in H_B (CH3) — določi activeParking = A ali B.
        //
        // Kriterij prehoda v SCANNING:
        //   Eden od H senzorjev < VEH_ENTRY_THRESH_MM (350 cm)
        //   Drugi H senzor tega ne zazna (vozilo gre na eno mesto)
        //
        // TODO (Faza logike): Dodati timeout — če smo v DETECT fazi dlje
        //   kot TOF_DETECT_TIMEOUT_MS (npr. 5 min) brez detekcije vozila,
        //   se vrnemo v IDLE. To reši scenarij ko rampa ostane dvignjena
        //   dlje časa (ure, dnevi) brez dejanskega parkiranja.
        //   Vrednost TOF_DETECT_TIMEOUT_MS dodati v config.h.
        //
        // TODO (Faza logike): Ob prehodu DETECT → SCANNING obvestiti
        //   sensor_mgr prek EventBus (EVT_TOF_PLACE_DETECTED) z activeParking
        //   vrednostjo da se lahko naloži ustrezna baza modelov za DTW.
        // ----------------------------------------------------------------

        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            TOFW("DETECT: Wire1 mutex timeout");
            break;
        }
        uint16_t h_a = read_channel(TCA_CH_TOF_H_A);
        uint16_t h_b = read_channel(TCA_CH_TOF_H_B);
        xSemaphoreGive(mtx);

        TOFD("DETECT — H_A:%s H_B:%s (prag:%d mm)",
             (h_a == TOF_ERR) ? "---" : String(h_a).c_str(),
             (h_b == TOF_ERR) ? "---" : String(h_b).c_str(),
             VEH_ENTRY_THRESH_MM);

        bool a_hit = (h_a != TOF_ERR && h_a < (uint16_t)VEH_ENTRY_THRESH_MM);
        bool b_hit = (h_b != TOF_ERR && h_b < (uint16_t)VEH_ENTRY_THRESH_MM);

        if (a_hit && !b_hit) {
            TOFI("DETECT: vozilo na mestu A (H_A=%d mm) → SCANNING", h_a);
            start_scanning(TOF_PLACE_A);
        } else if (b_hit && !a_hit) {
            TOFI("DETECT: vozilo na mestu B (H_B=%d mm) → SCANNING", h_b);
            start_scanning(TOF_PLACE_B);
        } else if (a_hit && b_hit) {
            // Oba H pod pragom — nejasen primer, čakamo razrešitev
            // TODO: če traja predolgo, logirati kot anomalijo
            TOFW("DETECT: oba H pod pragom (A=%d B=%d mm) — čakam razrešitev", h_a, h_b);
        }
        // Nobeden → ostanemo v DETECT, čakamo naslednji tick
        break;
    }

    // ----------------------------------------------------------
    // SCANNING — skeniranje profila vozila
    // ----------------------------------------------------------
    case TOF_PHASE_SCANNING:
    {
        // ----------------------------------------------------------------
        // FAZA 2 — SKENIRANJE PROFILA
        // Aktivni senzorji: H + P1 + P2 SAMO določenega mesta (s_active_place).
        // Drugo mesto se popolnoma ignorira — njegovi kanali se nikoli ne odprejo.
        //
        // SMART vzorčenje: točka se doda profilu samo če |ΔH| >= VEH_DELTA_FILTER_MM.
        // Naravni I2C cikel (~120 ms) je throttle — ni potreben dodaten timer.
        //
        // TODO (Faza logike): Zaključek skeniranja ob celica2 LOW + H stabilen
        //   > VEH_STABLE_TIME_MS (1.5s). Trenutno zaključi samo ob stabilnosti
        //   ali max točkah. Celica2 signal bo prišel kot MCP23017 IRQ →
        //   EventBus EVT_GPIO_CHANGED → hal_tof_stopScan() ali finalize_profile().
        //
        // TODO (Faza logike): Ob finalize_profile() callback → sensor_mgr →
        //   EventBus EVT_TOF_PROFILE_READY → vehicle_recog DTW burst na Core1.
        // ----------------------------------------------------------------

        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            TOFW("SCANNING: Wire1 mutex timeout");
            return;
        }

        uint32_t t_start = millis();

        // Izmeri vse 3 senzorje aktivnega mesta
        uint8_t base = place_base(s_active_place);
        uint16_t h_mm  = read_channel(base + 0);  // H
        uint16_t p1_mm = read_channel(base + 1);  // P1
        uint16_t p2_mm = read_channel(base + 2);  // P2

        xSemaphoreGive(mtx);

        // Posodobi I2C cikel statistiko
        uint32_t elapsed = millis() - t_start;
        s_cycle_ms_last  = elapsed;
        s_cycle_ms_sum  += elapsed;
        s_cycle_ms_count++;

        // Enkrat ob 1. meritvi in potem ob 10. — za TODO #1 kalibriranje
        if (s_cycle_ms_count == 1) {
            TOFI("I2C cikel (1. meritev, 3 senzorji): %lu ms", (unsigned long)elapsed);
        } else if (s_cycle_ms_count == 10) {
            TOFI("I2C cikel povprečje (10 meritev): %lu ms",
                 (unsigned long)(s_cycle_ms_sum / s_cycle_ms_count));
        }

        // H mora biti veljaven za vzorčenje (je naša X os profila)
        if (h_mm == TOF_ERR) {
            TOFD("SCAN: H senzor napaka — točka zavrnjena");
            break;
        }

        // SMART vzorčenje — Δ-filter
        bool save_point = false;
        if (s_last_H_mm == TOF_ERR) {
            // Prva točka se vedno shrani
            save_point = true;
        } else {
            uint16_t delta = (h_mm > s_last_H_mm)
                ? (h_mm  - s_last_H_mm)
                : (s_last_H_mm - h_mm);
            save_point = (delta >= (uint16_t)VEH_DELTA_FILTER_MM);
        }

        if (save_point && s_profile.count < TOF_PROFILE_MAX_PTS) {
            // Shrani točko
            TofProfilePoint& pt = s_profile.points[s_profile.count];
            pt.H_mm  = h_mm;
            pt.P1_mm = p1_mm;
            pt.P2_mm = p2_mm;
            pt.ts_ms = t_start;
            s_profile.count++;

            s_last_H_mm       = h_mm;
            s_in_stable       = false;
            s_stable_start_ms = 0;

            TOFD("SCAN[%d] H=%d P1=%s P2=%s",
                 s_profile.count, h_mm,
                 (p1_mm == TOF_ERR) ? "---" : String(p1_mm).c_str(),
                 (p2_mm == TOF_ERR) ? "---" : String(p2_mm).c_str());

        } else if (!save_point) {
            // Točka zavrnjena z Δ-filtrom → morda mirovanje → stabilnost timer
            if (!s_in_stable) {
                s_in_stable       = true;
                s_stable_start_ms = millis();
                TOFD("SCAN: stabilnost začetek (H=%d mm)", h_mm);
            }
            uint32_t stable_ms = millis() - s_stable_start_ms;
            if (stable_ms >= (uint32_t)VEH_STABLE_TIME_MS) {
                TOFI("SCAN: stabilen %lu ms — zaključujem profil",
                     (unsigned long)stable_ms);
                finalize_profile();
            }
        }

        // Zaščitna meja: prisilen zaključek pri max točkah
        if (s_phase == TOF_PHASE_SCANNING && s_profile.count >= TOF_PROFILE_MAX_PTS) {
            TOFW("SCAN: dosežena max točk (%d) — prisilen zaključek", TOF_PROFILE_MAX_PTS);
            finalize_profile();
        }
        break;
    }

    // ----------------------------------------------------------
    // DTW_WAIT — mirovanje med DTW burst-om
    // ----------------------------------------------------------
    // Ne beremo senzorjev. vehicle_recog kliče hal_tof_stopScan()
    // ko je DTW zaključen → prehod v IDLE.
    case TOF_PHASE_DTW_WAIT:
        // Nič — ne motimo DTW izračuna in ne zasedamo Wire1
        break;
    }
}

// ============================================================
// JAVNE FUNKCIJE — neposredne meritve (diagnostika)
// ============================================================

uint16_t hal_tof_readH(TofPlace place) {
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return TOF_ERR;
    uint16_t v = read_channel(place_sensor_to_ch(place, TOF_SENSOR_H));
    xSemaphoreGive(mtx);
    return v;
}

uint16_t hal_tof_readP1(TofPlace place) {
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return TOF_ERR;
    uint16_t v = read_channel(place_sensor_to_ch(place, TOF_SENSOR_P1));
    xSemaphoreGive(mtx);
    return v;
}

uint16_t hal_tof_readP2(TofPlace place) {
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return TOF_ERR;
    uint16_t v = read_channel(place_sensor_to_ch(place, TOF_SENSOR_P2));
    xSemaphoreGive(mtx);
    return v;
}

TofProfilePoint hal_tof_readAll(TofPlace place) {
    TofProfilePoint pt = {TOF_ERR, TOF_ERR, TOF_ERR, millis()};
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return pt;
    uint8_t base = place_base(place);
    pt.H_mm  = read_channel(base + 0);
    pt.P1_mm = read_channel(base + 1);
    pt.P2_mm = read_channel(base + 2);
    pt.ts_ms = millis();
    xSemaphoreGive(mtx);
    return pt;
}

// ============================================================
// JAVNE FUNKCIJE — diagnostika
// ============================================================

TofDiagnostics hal_tof_getDiagnostics() {
    TofDiagnostics d;
    for (int i = 0; i < 6; i++) {
        d.sensor_ok[i]    = s_sensor_ok[i];
        d.last_mm[i]      = s_last_mm[i];
        d.error_count[i]  = s_error_count[i];
    }
    d.recovery_count    = s_recovery_count;
    d.last_recovery_ms  = s_last_recovery_ms;
    d.current_phase     = s_phase;
    d.active_place      = s_active_place;
    d.profile_pts       = s_profile.count;
    d.i2c_cycle_ms_last = s_cycle_ms_last;
    d.i2c_cycle_ms_avg  = (s_cycle_ms_count > 0)
                          ? (s_cycle_ms_sum / s_cycle_ms_count)
                          : 0;
    return d;
}

bool hal_tof_reinitChannel(uint8_t channel) {
    if (channel >= 6) return false;
    TOFI("Ročni reinit kanala %s...", ch_name(channel));
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
    // Postavi consec_err na prag → do_recovery se obnese kot reinit
    s_consec_err[channel] = TOF_RECOVERY_RETRIES;
    bool ok = do_recovery(channel);
    xSemaphoreGive(mtx);
    return ok;
}

bool hal_tof_reinitAll() {
    TOFI("Reinit vseh 6 kanalov...");
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

    tca_close_all();
    bsp_tca_reset();
    delay(10);

    bool all_ok = true;
    for (uint8_t ch = 0; ch < 6; ch++) {
        s_sensor_ok[ch]   = init_channel(ch);
        s_consec_err[ch]  = 0;
        s_error_count[ch] = 0;
        s_last_mm[ch]     = TOF_ERR;
        if (!s_sensor_ok[ch]) all_ok = false;
    }
    xSemaphoreGive(mtx);

    TOFI("reinitAll zaključen — %s",
         all_ok ? "vsi OK" : "nekateri NAPAKA (glej loge)");
    return all_ok;
}

void hal_tof_logStats() {
    TOFI("=== TOF statistika ===");
    const char* phase_str[] = {"IDLE", "DETECT", "SCANNING", "DTW_WAIT"};
    TOFI("  Faza: %s | Aktivno mesto: %c | Točke v profilu: %d",
         phase_str[(int)s_phase],
         (s_active_place == TOF_PLACE_A) ? 'A' : 'B',
         s_profile.count);
    TOFI("  Recovery: %lu skupaj | zadnji: %lu ms nazaj",
         (unsigned long)s_recovery_count,
         s_last_recovery_ms > 0 ? (unsigned long)(millis() - s_last_recovery_ms) : 0UL);
    TOFI("  I2C cikel: zadnji=%lu ms povp=%lu ms (%lu meritev)",
         (unsigned long)s_cycle_ms_last,
         s_cycle_ms_count > 0 ? (unsigned long)(s_cycle_ms_sum / s_cycle_ms_count) : 0UL,
         (unsigned long)s_cycle_ms_count);
    for (uint8_t ch = 0; ch < 6; ch++) {
        TOFI("  %s: %s | zadnja=%s mm | napake=%lu",
             ch_name(ch),
             s_sensor_ok[ch] ? "OK " : "---",
             (s_last_mm[ch] == TOF_ERR) ? "----" : String(s_last_mm[ch]).c_str(),
             (unsigned long)s_error_count[ch]);
    }
}
