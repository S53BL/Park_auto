// ============================================================
// alarm.h — Alarm sistem
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ARHITEKTURA:
//   Pasivni izvrševalec. Sistem ne odloča sam kdaj je alarm aktiven.
//   Zunanji sistemi (Home Assistant, mobilna app ...) kličejo
//   POST /api/alarm za arm/disarm. Alarm modul subscribira na
//   RADAR_MOTION in ob zaznavi gibanja (če je ARMED) publishira
//   ALARM_TRIGGERED event ter pošlje HTTP callback notifikacijo.
//
// STATE MACHINE:
//   ALARM_OFF  ──POST on──►  ALARM_ARMED  ──RADAR_MOTION──►  ALARM_TRIGGERED
//                               ▲                                    │
//                               └─────────── PIN / POST off ◄────────┘
//                                            (vrne v OFF)
//
// INTERAKCIJA Z LIGHT_LOGIC:
//   Alarm ne kliče SSR direktno. Publishira ALARM_STATE_CHANGED event.
//   light_logic subscribira in prevzame SSR kontrolo:
//     - ARMED/TRIGGERED  → suspendira normalno logiko
//     - TRIGGERED        → SSR1 ON + led_mgr_alarm_blink()
//     - OFF              → vrne normalno delovanje
//
// NVS PERSISTENCA:
//   Stanje alarma se shrani v NVS ob vsaki spremembi.
//   Ob rebootu se stanje obnovi — če je bil ARMED ostane ARMED.
//   NVS namespace: "alarm" (ločen od "parking" config namespace-a)
//
// THREAD SAFETY:
//   Mutex varuje vsa polja AlarmState.
//   alarm_get_state() je varen iz kateregakoli taska.
//   alarm_disarm_pin() kliče se iz LVGL taska (touch event).
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// KONSTANTE
// ============================================================

// NVS namespace za alarm parametre (ločen od config "parking")
#define ALARM_NVS_NAMESPACE         "alarm"

// NVS ključi (max 15 znakov — NVS omejitev)
#define ALARM_NVS_KEY_STATE         "state"        // uint8_t: AlarmStateEnum
#define ALARM_NVS_KEY_GRACE         "grace_s"      // uint32_t: sekunde
#define ALARM_NVS_KEY_DURATION      "duration_s"   // uint32_t: 0=trajno
#define ALARM_NVS_KEY_ARMED_AT      "armed_at"     // uint32_t: millis snapshot (samo info)
#define ALARM_NVS_KEY_PIN           "pin"          // char[9]: null-terminated

// Defaultne vrednosti
#define ALARM_DEFAULT_GRACE_S       30      // sekunde utripanja po zadnjem gibanju
#define ALARM_DEFAULT_PIN           "1234"  // spremeniti prek web UI
#define ALARM_MIN_GRACE_S           10
#define ALARM_MAX_GRACE_S           600
#define ALARM_MIN_DURATION_S        0       // 0 = trajno
#define ALARM_MAX_DURATION_S        86400   // 24h
#define ALARM_PIN_MIN_LEN           4
#define ALARM_PIN_MAX_LEN           8

// Radar throttle za alarm notifikacije (ms med dvema callback klicema)
// Prepreči flooding Home Assistanta pri intenzivnem gibanju
#define ALARM_CALLBACK_THROTTLE_MS  5000

// Callback timeout (ms) — HTTPClient connect+response timeout
#define ALARM_CALLBACK_TIMEOUT_MS   3000

// ============================================================
// TIPI
// ============================================================

enum class AlarmStateEnum : uint8_t {
    OFF         = 0,    // Alarm izklopljen — normalno delovanje
    ARMED       = 1,    // Alarm aktiven — čaka na gibanje
    TRIGGERED   = 2,    // Alarm sprožen — utripanje + callback
};

struct AlarmState {
    AlarmStateEnum  state;              // trenutno stanje
    bool            active;             // convenience: state != OFF
    uint32_t        duration_s;         // 0 = trajno, >0 = samodejni izklop po N sekundah
    uint32_t        remaining_s;        // preostale sekunde (0 če trajno)
    bool            permanent;          // true če duration_s == 0
    uint32_t        grace_s;            // sekunde utripanja po zadnjem gibanju
    bool            callback_url_set;   // ali je callback_url konfiguriran
    uint32_t        last_motion_ms;     // millis() zadnjega gibanja (0 = ni bilo)
    char            pin[ALARM_PIN_MAX_LEN + 1]; // trenutni PIN (za debug/web — prikaži samo dolžino!)
    uint8_t         pin_len;            // dolžina PIN kode
    // Statistika
    uint32_t        trigger_count;      // koliko-krat je bil alarm sprožen
    uint32_t        callback_sent;      // koliko callbackov je bilo poslanih
    uint32_t        callback_failed;    // koliko callbackov je spodletelo
};

struct AlarmArmParams {
    uint32_t    duration_s;         // 0 = trajno
    char        callback_url[256];  // prazen string = brez callbacka
};

// ============================================================
// JAVNI API
// ============================================================

// Inicializacija — kliči iz appTask (po light_logic_init).
// Obnovi stanje iz NVS, registrira EventBus subscriberje.
bool alarm_init();

// Vrne true če je alarm_init() uspešno zaključil.
bool alarm_ok();

// Arm alarm — kliči iz web UI handler-ja (POST /api/alarm state:on).
// Shrani stanje v NVS. Publishira ALARM_STATE_CHANGED.
// Vrne false če parametri niso veljavni.
bool alarm_arm(const AlarmArmParams& params);

// Disarm alarm — kliči iz web UI ali LCD PIN vnosnika.
// Shrani stanje v NVS. Publishira ALARM_STATE_CHANGED.
void alarm_disarm();

// Preveri PIN in disarma alarm ob ujemanju.
// Vrne true = PIN pravilen in alarm disarman.
// Vrne false = PIN napačen (ne disarma).
// Thread-safe — varno klicati iz LVGL taska (touch event).
bool alarm_disarm_pin(const char* entered_pin);

// Vrne snapshot trenutnega stanja (thread-safe, mutex zaščiten).
AlarmState alarm_get_state();

// Nastavi grace period (sekunde utripanja po zadnjem gibanju).
// Shrani v NVS. Veljavno: ALARM_MIN_GRACE_S – ALARM_MAX_GRACE_S.
bool alarm_set_grace_s(uint32_t grace_s);

// Nastavi PIN kodo.
// Shrani v NVS. Veljavno: 4–8 številk.
// Vrne false za neveljaven PIN (prekratek, predolg, ne-številčni).
bool alarm_set_pin(const char* pin);

// Tick — kliči iz appTask vsakih ~100ms (skupaj z light_logic_tick).
// Preverja:
//   - duration timeout (samodejni disarm po N sekundah)
//   - grace period po zadnjem gibanju (izklop utripanja)
void alarm_tick();

// Sproži testno utripanje brez spremembe stanja alarma.
// Namen: vizualno opozorilo prek web UI brez aktiviranja alarma.
// Utripanje traja ALARM_TEST_BLINK_S sekund.
void alarm_test_blink();
