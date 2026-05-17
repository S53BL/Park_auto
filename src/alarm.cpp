// ============================================================
// alarm.cpp — Alarm sistem implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================

#include "alarm.h"
#include "event_bus.h"
#include "logger.h"
#include "led_manager.h"
#include <Preferences.h>
#include <freertos/semphr.h>
#include <HTTPClient.h>
#include <WiFi.h>

// ============================================================
// LOGGING
// ============================================================

#define ALI(fmt, ...) LOG_INFO ("ALARM", fmt, ##__VA_ARGS__)
#define ALW(fmt, ...) LOG_WARN ("ALARM", fmt, ##__VA_ARGS__)
#define ALE(fmt, ...) LOG_ERROR("ALARM", fmt, ##__VA_ARGS__)
#define ALD(fmt, ...) LOG_DEBUG("ALARM", fmt, ##__VA_ARGS__)

// ============================================================
// INTERNI TIPI
// ============================================================

// Testno utripanje (web UI "Test" gumb) — ločeno od pravega alarma
#define ALARM_TEST_BLINK_S  5

// Čas utripanja po zadnjem gibanju preden se SSR1 izklopi
// (grace period). Nastavljivo prek alarm_set_grace_s() in web UI.
// Alarm ostane TRIGGERED med grace periodom — ponovni radar event
// resetira grace timer.

// ============================================================
// INTERNO STANJE
// ============================================================

static SemaphoreHandle_t s_mutex        = nullptr;
static bool              s_initialized  = false;

// Celotno stanje v eni strukturi — enostavno za NVS save/load
struct AlarmRuntime {
    AlarmStateEnum  state;
    uint32_t        grace_s;
    uint32_t        duration_s;         // 0 = trajno
    uint32_t        arm_time_ms;        // millis() ob armu
    uint32_t        last_motion_ms;     // millis() zadnjega RADAR_MOTION
    uint32_t        last_callback_ms;   // millis() zadnjega callback klica (throttle)
    char            callback_url[256];  // prazen = brez callbacka
    char            pin[ALARM_PIN_MAX_LEN + 1];
    // Statistika (samo RAM — ne persistira)
    uint32_t        trigger_count;
    uint32_t        callback_sent;
    uint32_t        callback_failed;
    // Test blink
    bool            test_blink_active;
    uint32_t        test_blink_end_ms;
};

static AlarmRuntime s_rt = {};

// ============================================================
// NVS POMOŽNE FUNKCIJE
// ============================================================

static void _nvs_save() {
    Preferences prefs;
    if (!prefs.begin(ALARM_NVS_NAMESPACE, false)) {
        ALW("NVS begin failed — stanje ni shranjeno");
        return;
    }
    prefs.putUChar(ALARM_NVS_KEY_STATE,    (uint8_t)s_rt.state);
    prefs.putUInt (ALARM_NVS_KEY_GRACE,    s_rt.grace_s);
    prefs.putUInt (ALARM_NVS_KEY_DURATION, s_rt.duration_s);
    prefs.putString(ALARM_NVS_KEY_PIN,     s_rt.pin);
    prefs.end();
    ALD("NVS saved: state=%d grace=%lu duration=%lu",
        (int)s_rt.state, (unsigned long)s_rt.grace_s,
        (unsigned long)s_rt.duration_s);
}

static void _nvs_load() {
    Preferences prefs;
    if (!prefs.begin(ALARM_NVS_NAMESPACE, true)) {
        ALW("NVS begin (RO) failed — defaulti");
        return;
    }
    uint8_t st = prefs.getUChar(ALARM_NVS_KEY_STATE, (uint8_t)AlarmStateEnum::OFF);
    // Ob rebootu: TRIGGERED → ARMED (utripanje se ne more nadaljevati)
    if (st == (uint8_t)AlarmStateEnum::TRIGGERED) {
        st = (uint8_t)AlarmStateEnum::ARMED;
    }
    s_rt.state     = (AlarmStateEnum)st;
    s_rt.grace_s   = prefs.getUInt(ALARM_NVS_KEY_GRACE,    ALARM_DEFAULT_GRACE_S);
    s_rt.duration_s= prefs.getUInt(ALARM_NVS_KEY_DURATION, 0);

    String pin_str = prefs.getString(ALARM_NVS_KEY_PIN, ALARM_DEFAULT_PIN);
    strlcpy(s_rt.pin, pin_str.c_str(), sizeof(s_rt.pin));

    prefs.end();

    // Validacija
    if (s_rt.grace_s < ALARM_MIN_GRACE_S || s_rt.grace_s > ALARM_MAX_GRACE_S) {
        s_rt.grace_s = ALARM_DEFAULT_GRACE_S;
    }
    if (strlen(s_rt.pin) < ALARM_PIN_MIN_LEN) {
        strlcpy(s_rt.pin, ALARM_DEFAULT_PIN, sizeof(s_rt.pin));
    }

    ALI("NVS loaded: state=%d grace=%lu duration=%lu pin_len=%u",
        (int)s_rt.state, (unsigned long)s_rt.grace_s,
        (unsigned long)s_rt.duration_s, (unsigned)strlen(s_rt.pin));
}

// ============================================================
// HTTP CALLBACK
// ============================================================
// Kliče se v appTask kontekstu (iz alarm_tick ali neposredno ob triggerju).
// Ne kliči iz EventBus handlerja (ISR-ish kontekst, Wire1 ni varen).

static void _send_callback(const char* radar_name) {
    if (strlen(s_rt.callback_url) == 0) return;

    uint32_t now = millis();
    if ((now - s_rt.last_callback_ms) < ALARM_CALLBACK_THROTTLE_MS) {
        ALD("Callback throttled (%lu ms od zadnjega)", (unsigned long)(now - s_rt.last_callback_ms));
        return;
    }
    s_rt.last_callback_ms = now;

    if (WiFi.status() != WL_CONNECTED) {
        ALW("Callback preskočen — WiFi ni connected");
        s_rt.callback_failed++;
        return;
    }

    // Sestavi JSON payload
    char body[256];
    snprintf(body, sizeof(body),
        "{\"timestamp\":%lu,\"radar\":\"%s\"}",
        (unsigned long)(millis() / 1000UL),
        radar_name ? radar_name : "unknown");

    ALI("Callback → %s (radar=%s)", s_rt.callback_url, radar_name ? radar_name : "?");

    HTTPClient http;
    http.begin(s_rt.callback_url);
    http.setTimeout(ALARM_CALLBACK_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    http.end();

    if (code > 0 && code < 400) {
        s_rt.callback_sent++;
        ALD("Callback OK (HTTP %d)", code);
    } else {
        s_rt.callback_failed++;
        ALW("Callback NAPAKA (HTTP %d / err=%d)", code, code);
    }
}

// ============================================================
// EVENTBUS HANDLER — RADAR_MOTION
// ============================================================
// Kliče se iz eventBusTask kontekstu.
// Enqueue-a ukaz (callback + blink) za appTask tick.
// Brez direktnih HTTPClient ali Wire1 klicev tukaj!

// Queue za alarm ukaze iz EventBus v appTask
enum class AlarmCmdType : uint8_t {
    RADAR_DETECTED = 0,
    TEST_BLINK     = 1,
};
struct AlarmCmd {
    AlarmCmdType type;
    uint8_t      channel;  // radar kanal 0-3
};

static QueueHandle_t s_cmd_queue = nullptr;

static void on_radar_motion(const Event& e) {
    bool motion     = (e.payload >> 8) & 0x01;
    bool stationary = (e.payload >> 9) & 0x01;
    if (!motion && !stationary) return;

    // Hiter check brez mutexa — samo branje volatile enum
    if (s_rt.state == AlarmStateEnum::OFF) return;

    uint8_t channel = (uint8_t)(e.payload & 0xFF);
    AlarmCmd cmd = { AlarmCmdType::RADAR_DETECTED, channel };
    if (s_cmd_queue) {
        xQueueSend(s_cmd_queue, &cmd, 0);
    }
}

// ============================================================
// POMOŽNA: publish stanje spremembe
// ============================================================
// payload kodiranje:
//   bit 0–1: AlarmStateEnum (0=OFF, 1=ARMED, 2=TRIGGERED)
//   bit 8:   active (convenience)

static void _publish_state_changed() {
    uint32_t payload = (uint32_t)s_rt.state;
    if (s_rt.state != AlarmStateEnum::OFF) payload |= (1 << 8);
    EventBus::publish(EventType::ALARM_STATE_CHANGED, payload);
    ALD("ALARM_STATE_CHANGED published (payload=0x%lx)", (unsigned long)payload);
}

// ============================================================
// alarm_init
// ============================================================

bool alarm_init() {
    ALI("=== alarm_init ===");

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ALE("xSemaphoreCreateMutex napaka!");
        return false;
    }

    s_cmd_queue = xQueueCreate(16, sizeof(AlarmCmd));
    if (!s_cmd_queue) {
        ALE("xQueueCreate napaka!");
        return false;
    }

    // Inicializiraj runtime na varno stanje
    memset(&s_rt, 0, sizeof(s_rt));
    strlcpy(s_rt.pin, ALARM_DEFAULT_PIN, sizeof(s_rt.pin));
    s_rt.grace_s = ALARM_DEFAULT_GRACE_S;

    // Obnovi stanje iz NVS
    _nvs_load();

    // Registriraj EventBus subscriber
    EventBus::subscribe(EventType::RADAR_MOTION, on_radar_motion);

    s_initialized = true;

    // Objavi začetno stanje (LCD in web UI se posodobita)
    _publish_state_changed();

    ALI("alarm_init OK — stanje: %s",
        s_rt.state == AlarmStateEnum::OFF    ? "OFF"       :
        s_rt.state == AlarmStateEnum::ARMED  ? "ARMED"     : "TRIGGERED");
    return true;
}

bool alarm_ok() { return s_initialized; }

// ============================================================
// alarm_arm
// ============================================================

bool alarm_arm(const AlarmArmParams& params) {
    if (!s_initialized) return false;

    if (params.duration_s != 0 &&
        (params.duration_s < ALARM_MIN_DURATION_S ||
         params.duration_s > ALARM_MAX_DURATION_S)) {
        ALW("arm: neveljaven duration_s=%lu", (unsigned long)params.duration_s);
        return false;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ALW("arm: mutex timeout");
        return false;
    }

    s_rt.state       = AlarmStateEnum::ARMED;
    s_rt.duration_s  = params.duration_s;
    s_rt.arm_time_ms = millis();
    s_rt.last_motion_ms = 0;
    s_rt.last_callback_ms = 0;

    // Shrani callback_url — dinamičen per-request
    strlcpy(s_rt.callback_url, params.callback_url, sizeof(s_rt.callback_url));

    xSemaphoreGive(s_mutex);

    _nvs_save();
    _publish_state_changed();

    ALI("ARMED (duration=%lu s, callback=%s)",
        (unsigned long)params.duration_s,
        strlen(s_rt.callback_url) > 0 ? s_rt.callback_url : "ni");
    return true;
}

// ============================================================
// alarm_disarm
// ============================================================

void alarm_disarm() {
    if (!s_initialized) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ALW("disarm: mutex timeout");
        return;
    }
    AlarmStateEnum prev = s_rt.state;
    s_rt.state          = AlarmStateEnum::OFF;
    s_rt.duration_s     = 0;
    s_rt.arm_time_ms    = 0;
    s_rt.last_motion_ms = 0;
    memset(s_rt.callback_url, 0, sizeof(s_rt.callback_url));
    xSemaphoreGive(s_mutex);

    _nvs_save();
    _publish_state_changed();

    // Ustavi alarm blink animacijo
    led_mgr_alarm_blink_stop();

    ALI("DISARMED (bil: %s)",
        prev == AlarmStateEnum::ARMED     ? "ARMED" :
        prev == AlarmStateEnum::TRIGGERED ? "TRIGGERED" : "OFF");
}

// ============================================================
// alarm_disarm_pin
// ============================================================

bool alarm_disarm_pin(const char* entered_pin) {
    if (!s_initialized || !entered_pin) return false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ALW("disarm_pin: mutex timeout");
        return false;
    }
    bool match = (strcmp(entered_pin, s_rt.pin) == 0);
    xSemaphoreGive(s_mutex);

    if (match) {
        ALI("PIN pravilen → disarm");
        alarm_disarm();
        return true;
    } else {
        ALW("PIN napačen (dolžina=%u)", (unsigned)strlen(entered_pin));
        return false;
    }
}

// ============================================================
// alarm_set_grace_s
// ============================================================

bool alarm_set_grace_s(uint32_t grace_s) {
    if (grace_s < ALARM_MIN_GRACE_S || grace_s > ALARM_MAX_GRACE_S) {
        ALW("set_grace_s: vrednost %lu izven obsega", (unsigned long)grace_s);
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    s_rt.grace_s = grace_s;
    xSemaphoreGive(s_mutex);
    _nvs_save();
    ALI("grace_s nastavljeno na %lu", (unsigned long)grace_s);
    return true;
}

// ============================================================
// alarm_set_pin
// ============================================================

bool alarm_set_pin(const char* pin) {
    if (!pin) return false;
    size_t len = strlen(pin);
    if (len < ALARM_PIN_MIN_LEN || len > ALARM_PIN_MAX_LEN) {
        ALW("set_pin: neveljaven PIN (dolžina=%u)", (unsigned)len);
        return false;
    }
    // Preveri da so samo številke
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            ALW("set_pin: PIN vsebuje ne-številčni znak");
            return false;
        }
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    strlcpy(s_rt.pin, pin, sizeof(s_rt.pin));
    xSemaphoreGive(s_mutex);
    _nvs_save();
    ALI("PIN spremenjen (dolžina=%u)", (unsigned)len);
    return true;
}

// ============================================================
// alarm_get_state
// ============================================================

AlarmState alarm_get_state() {
    AlarmState out = {};

    if (!s_initialized) return out;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        // Fallback brez mutexa — samo osnovna polja
        out.state  = s_rt.state;
        out.active = (s_rt.state != AlarmStateEnum::OFF);
        return out;
    }

    out.state           = s_rt.state;
    out.active          = (s_rt.state != AlarmStateEnum::OFF);
    out.duration_s      = s_rt.duration_s;
    out.grace_s         = s_rt.grace_s;
    out.permanent       = (s_rt.duration_s == 0);
    out.callback_url_set = (strlen(s_rt.callback_url) > 0);
    out.last_motion_ms  = s_rt.last_motion_ms;
    out.pin_len         = (uint8_t)strlen(s_rt.pin);
    out.trigger_count   = s_rt.trigger_count;
    out.callback_sent   = s_rt.callback_sent;
    out.callback_failed = s_rt.callback_failed;
    // PIN ne izpostavimo v javni strukturi — samo dolžina
    // (za web UI prikaz "PIN: ****")
    memset(out.pin, '*', out.pin_len);
    out.pin[out.pin_len] = '\0';

    // Preostali čas
    if (!out.permanent && s_rt.arm_time_ms > 0) {
        uint32_t elapsed_s = (millis() - s_rt.arm_time_ms) / 1000UL;
        out.remaining_s = (elapsed_s < s_rt.duration_s)
                          ? (s_rt.duration_s - elapsed_s)
                          : 0;
    }

    xSemaphoreGive(s_mutex);
    return out;
}

// ============================================================
// alarm_test_blink
// ============================================================

void alarm_test_blink() {
    if (!s_initialized) return;
    AlarmCmd cmd = { AlarmCmdType::TEST_BLINK, 0 };
    if (s_cmd_queue) xQueueSend(s_cmd_queue, &cmd, 0);
    ALI("Test blink zahtevano");
}

// ============================================================
// alarm_tick — kliči iz appTask vsakih ~100ms
// ============================================================

void alarm_tick() {
    if (!s_initialized) return;

    uint32_t now = millis();

    // -------------------------------------------------------
    // Procesiranje command queue (iz EventBus handlerjev)
    // -------------------------------------------------------
    // Tukaj so varni HTTPClient klici (appTask kontekst, ne ISR).
    static const char* radar_names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};

    AlarmCmd cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {

        if (cmd.type == AlarmCmdType::TEST_BLINK) {
            // Testno utripanje — brez spremembe stanja alarma
            if (s_rt.state == AlarmStateEnum::OFF) {
                // Samo ko je alarm OFF (prek web UI Test gumba)
                // Ko je ARMED/TRIGGERED utripanje že deluje normalno
                ALI("Test blink start (%d s)", ALARM_TEST_BLINK_S);
                s_rt.test_blink_active  = true;
                s_rt.test_blink_end_ms  = now + (ALARM_TEST_BLINK_S * 1000UL);
                led_mgr_alarm_blink_start();
            }
            continue;
        }

        if (cmd.type == AlarmCmdType::RADAR_DETECTED) {
            if (s_rt.state == AlarmStateEnum::OFF) continue;

            const char* rname = (cmd.channel < 4) ? radar_names[cmd.channel] : "unknown";

            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_rt.last_motion_ms = now;

                if (s_rt.state == AlarmStateEnum::ARMED) {
                    // Prvo zaznavo gibanja → sprožimo alarm
                    s_rt.state = AlarmStateEnum::TRIGGERED;
                    s_rt.trigger_count++;
                    ALI("TRIGGERED! Radar kanal %d (%s)", cmd.channel, rname);
                    xSemaphoreGive(s_mutex);

                    // Začni utripanje (SSR1 bo prižgal light_logic ob ALARM_STATE_CHANGED)
                    led_mgr_alarm_blink_start();
                    _nvs_save();
                    _publish_state_changed();

                } else if (s_rt.state == AlarmStateEnum::TRIGGERED) {
                    // Nadaljnje gibanje — samo posodobi last_motion timer
                    // (grace period se resetira) in pošlji callback
                    xSemaphoreGive(s_mutex);
                    ALD("TRIGGERED + gibanje (reset grace timer), radar=%s", rname);
                }
            }

            // Pošlji HTTP callback (throttlan)
            _send_callback(rname);
            continue;
        }
    }

    // -------------------------------------------------------
    // Test blink timeout
    // -------------------------------------------------------
    if (s_rt.test_blink_active && now >= s_rt.test_blink_end_ms) {
        s_rt.test_blink_active = false;
        led_mgr_alarm_blink_stop();
        ALI("Test blink končan");
    }

    // -------------------------------------------------------
    // Duration timeout — samodejni disarm po N sekundah
    // -------------------------------------------------------
    if (s_rt.state != AlarmStateEnum::OFF &&
        s_rt.duration_s > 0 &&
        s_rt.arm_time_ms > 0) {
        uint32_t elapsed_s = (now - s_rt.arm_time_ms) / 1000UL;
        if (elapsed_s >= s_rt.duration_s) {
            ALI("Duration timeout (%lu s) — samodejni disarm", (unsigned long)s_rt.duration_s);
            alarm_disarm();
            return;
        }
    }

    // -------------------------------------------------------
    // Grace period — po zadnjem gibanju N sekund utripamo,
    // nato preidemo TRIGGERED → ARMED (čaka na novo gibanje)
    // SSR1 se ne ugasne avtomatično — to naredi light_logic
    // ob ALARM_STATE_CHANGED event
    // -------------------------------------------------------
    if (s_rt.state == AlarmStateEnum::TRIGGERED &&
        s_rt.last_motion_ms > 0) {
        uint32_t since_motion_ms = now - s_rt.last_motion_ms;
        if (since_motion_ms >= (s_rt.grace_s * 1000UL)) {
            ALI("Grace period potekel (%lu s brez gibanja) → ARMED",
                (unsigned long)s_rt.grace_s);
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_rt.state = AlarmStateEnum::ARMED;
                s_rt.last_motion_ms = 0;
                xSemaphoreGive(s_mutex);
            }
            led_mgr_alarm_blink_stop();
            _nvs_save();
            _publish_state_changed();
        }
    }
}
