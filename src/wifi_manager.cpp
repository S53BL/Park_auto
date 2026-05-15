// ============================================================
// wifi_manager.cpp — WiFi Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// IMPLEMENTIRA: wifiTask() — zamenja BSP stub
//
// POTEK wifiTask():
//   1. wifi_connect_best()  — scan vseh SSID, vzame prvega ki odgovori
//   2. web_ui_begin()       — web server start (pred NTP, heap neframentiran)
//   3. wifi_ntp_sync()      — configTime + čakanje na sync (max 15s)
//   4. mDNS ODSTRANJEN      — statična IP 192.168.2.170
//   5. Watchdog zanka       — vsakih 30s preveri, reconnect po potrebi
//
// ============================================================

#include "wifi_manager.h"
#include "web_ui.h"
#include <esp_heap_caps.h>
#include "wifi_config.h"
#include "config.h"
#include "logger.h"
#include "event_bus.h"
#include <WiFi.h>
#include <esp_sntp.h>
#include <freertos/semphr.h>
#include <time.h>

// ============================================================
// LOGGING
// ============================================================

#define WF_I(fmt, ...) LOG_INFO ("WIFI", fmt, ##__VA_ARGS__)
#define WF_W(fmt, ...) LOG_WARN ("WIFI", fmt, ##__VA_ARGS__)
#define WF_E(fmt, ...) LOG_ERROR("WIFI", fmt, ##__VA_ARGS__)
#define WF_D(fmt, ...) LOG_DEBUG("WIFI", fmt, ##__VA_ARGS__)

// ============================================================
// KONSTANTE
// ============================================================

// Connect timeout za en SSID
#define WIFI_CONNECT_TIMEOUT_MS     15000   // [ms] max čakanja na WL_CONNECTED
#define WIFI_CONNECT_POLL_MS        500     // [ms] interval preverjanja med čakanjem

// NTP
#define NTP_SYNC_TIMEOUT_MS         15000   // [ms] max čakanja na prvo NTP sync
#define NTP_SYNC_POLL_MS            500     // [ms] interval preverjanja
#define NTP_CHECK_INTERVAL_MS       (6UL * 60UL * 60UL * 1000UL)  // 6h
#define NTP_MAX_DRIFT_SEC           60      // [s] max dovoljen drift pred restart

// Timezone Slovenija — CET zimski čas, CEST poletni čas
#define TIMEZONE_STR                "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1                "pool.ntp.org"
#define NTP_SERVER_2                "time.google.com"

// Watchdog interval
#define WATCHDOG_INTERVAL_MS        30000   // [ms] interval WiFi health check

// Reconnect backoff
#define BACKOFF_INITIAL_MS          15000   // [ms] začetni backoff
#define BACKOFF_MAX_MS              120000  // [ms] maksimalni backoff (2 min)
#define BACKOFF_MULTIPLIER          2       // eksponentni faktor

// Periodic log flush iz wifiTask
#define LOG_FLUSH_INTERVAL_MS       60000   // [ms] flush logov na SD

// ============================================================
// INTERNO STANJE
// ============================================================

static SemaphoreHandle_t s_mutex    = nullptr;
static WifiStatus        s_status   = {};
static uint32_t          s_last_ntp_check_ms = 0;

// ============================================================
// POMOŽNE FUNKCIJE
// ============================================================

static bool take_mutex() {
    if (!s_mutex) return true;
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}
static void give_mutex() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static void update_status(WifiState state) {
    if (!take_mutex()) return;
    s_status.state = state;

    if (WiFi.status() == WL_CONNECTED) {
        strncpy(s_status.ip_str, WiFi.localIP().toString().c_str(),
                sizeof(s_status.ip_str) - 1);
        strncpy(s_status.ssid, WiFi.SSID().c_str(),
                sizeof(s_status.ssid) - 1);
        s_status.rssi = WiFi.RSSI();
    } else {
        strncpy(s_status.ip_str, "0.0.0.0", sizeof(s_status.ip_str));
        s_status.rssi = 0;
    }

    // NTP čas
    if (s_status.ntp_ok) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        snprintf(s_status.ntp_time, sizeof(s_status.ntp_time),
                 "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        strncpy(s_status.ntp_time, "--", sizeof(s_status.ntp_time));
    }
    give_mutex();
}

// ============================================================
// CONNECT — poskusi z enim SSID
// ============================================================

static bool wifi_connect_one(const char* ssid, const char* pass) {
    WF_I("Poskušam: SSID='%s' ...", ssid);

    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    uint32_t elapsed = 0;
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_POLL_MS));
        elapsed = millis() - start;
        WF_D("  ... čakam %lu ms (status=%d)", elapsed, WiFi.status());

        if (elapsed >= WIFI_CONNECT_TIMEOUT_MS) {
            WF_W("  Timeout %lu ms — SSID '%s' nedosegljiv", elapsed, ssid);
            WiFi.disconnect(true);
            vTaskDelay(pdMS_TO_TICKS(200));
            return false;
        }
    }

    WF_I("Povezan z '%s' v %lu ms", ssid, millis() - start);
    WF_I("  IP:   %s", WiFi.localIP().toString().c_str());
    WF_I("  GW:   %s", WiFi.gatewayIP().toString().c_str());
    WF_I("  RSSI: %d dBm", WiFi.RSSI());
    return true;
}

// ============================================================
// CONNECT — scan vseh SSID v seznamu
// ============================================================

static bool wifi_connect_best() {
    WF_I("=== WiFi connect — %d omrežij v seznamu ===", WIFI_NETWORK_COUNT);

    // Statična IP konfiguracija pred WiFi.begin()
    if (!WiFi.config(WIFI_LOCAL_IP, WIFI_GATEWAY, WIFI_SUBNET, WIFI_DNS)) {
        WF_W("Statična IP konfiguracija napaka — DHCP bo uporabljen");
    } else {
        WF_I("Statična IP: %s GW: %s",
             WIFI_LOCAL_IP.toString().c_str(),
             WIFI_GATEWAY.toString().c_str());
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);   // mi upravljamo reconnect sami

    update_status(WifiState::CONNECTING);

    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
        WF_I("Omrežje %d/%d: '%s'", i + 1, WIFI_NETWORK_COUNT, WIFI_SSID_LIST[i]);

        if (wifi_connect_one(WIFI_SSID_LIST[i], WIFI_PASS_LIST[i])) {
            if (take_mutex()) {
                strncpy(s_status.ssid, WIFI_SSID_LIST[i], sizeof(s_status.ssid) - 1);
                s_status.connect_time_ms = millis();
                s_status.backoff_ms      = BACKOFF_INITIAL_MS;
                give_mutex();
            }
            return true;
        }

        if (i < WIFI_NETWORK_COUNT - 1) {
            WF_D("Premor 2s pred naslednjim omrežjem...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    WF_E("Nobeno omrežje ni dosegljivo (%d poskusov)", WIFI_NETWORK_COUNT);
    update_status(WifiState::FAILED);
    return false;
}

// ============================================================
// NTP SINHRONIZACIJA
// ============================================================

static bool wifi_ntp_sync() {
    WF_I("NTP sync — strežnika: %s, %s", NTP_SERVER_1, NTP_SERVER_2);
    WF_I("  Timezone: %s", TIMEZONE_STR);

    // configTime nastavi SNTP in timezone
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
    setenv("TZ", TIMEZONE_STR, 1);
    tzset();

    // Čakaj na sinhronizacijo
    uint32_t start   = millis();
    uint32_t elapsed = 0;
    time_t   now     = 0;

    while (elapsed < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(NTP_SYNC_POLL_MS));
        elapsed = millis() - start;

        now = time(nullptr);
        if (now > 1577836800UL) {   // 2020-01-01 = sinhroniziran
            break;
        }
        WF_D("NTP čakam... %lu ms (time=%lu)", elapsed, (unsigned long)now);
    }

    if (now <= 1577836800UL) {
        WF_W("NTP sync TIMEOUT po %lu ms — timestamps bodo millis()", elapsed);
        return false;
    }

    // Sinhronizacija uspešna
    struct tm t;
    localtime_r(&now, &t);
    char time_str[24];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    WF_I("NTP sync OK v %lu ms — lokalni čas: %s (CET/CEST)", elapsed, time_str);

    // Obvesti logger in EventBus
    logger_set_ntp_synced(true);
    EventBus::publish(EventType::NTP_SYNCED, (uint32_t)now);
    logger_flush();   // flush z novimi timestamps

    if (take_mutex()) {
        s_status.ntp_ok = true;
        strncpy(s_status.ntp_time, time_str, sizeof(s_status.ntp_time) - 1);
        give_mutex();
    }

    s_last_ntp_check_ms = millis();
    return true;
}

// ============================================================
// NTP PERIODIČNO PREVERJANJE
// ============================================================

static void wifi_ntp_check() {
    if (!s_status.ntp_ok) return;
    if ((millis() - s_last_ntp_check_ms) < NTP_CHECK_INTERVAL_MS) return;

    s_last_ntp_check_ms = millis();
    WF_D("NTP periodično preverjanje...");

    // Preveri drift med SNTP in lokalnim časom
    // esp_sntp_get_sync_status() vrne SNTP_SYNC_STATUS_COMPLETED ko je sync svež
    sntp_sync_status_t sync_status = sntp_get_sync_status();
    if (sync_status == SNTP_SYNC_STATUS_IN_PROGRESS) {
        WF_D("NTP sync v teku — OK");
        return;
    }

    // Preveri ali je čas smiseln
    time_t now = time(nullptr);
    if (now <= 1577836800UL) {
        WF_W("NTP: čas ni veljaven po 6h — ponovni sync...");
        logger_set_ntp_synced(false);
        wifi_ntp_sync();
    } else {
        struct tm t;
        localtime_r(&now, &t);
        WF_D("NTP OK: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    }
}

// ============================================================
// WATCHDOG — periodično preverjanje povezave
// ============================================================

// Vrne true = povezava je zdrava
static bool wifi_watchdog_check() {
    wl_status_t status = WiFi.status();
    int8_t rssi = WiFi.RSSI();

    WF_D("Watchdog: status=%d RSSI=%d dBm SSID='%s'",
         status, rssi, WiFi.SSID().c_str());

    if (status != WL_CONNECTED) {
        WF_W("Watchdog: WiFi status=%d — ni WL_CONNECTED!", status);
        return false;
    }

    if (rssi == 0) {
        WF_W("Watchdog: RSSI=0 — izgubljena povezava (status lažno OK)");
        return false;
    }

    if (rssi < -85) {
        WF_W("Watchdog: RSSI=%d dBm — slab signal, možen izpad", rssi);
        // Ne prekinemo — samo opozorilo
    }

    // Posodobi status
    update_status(WifiState::NTP_SYNCED);
    return true;
}

// ============================================================
// RECONNECT Z EKSPONENTNIM BACKOFF
// ============================================================

static void wifi_reconnect() {
    uint32_t backoff_ms;
    if (take_mutex()) {
        s_status.reconnect_count++;
        backoff_ms = s_status.backoff_ms;
        give_mutex();
    } else {
        backoff_ms = BACKOFF_INITIAL_MS;
    }

    WF_W("=== Reconnect #%d — backoff %lu ms ===",
         s_status.reconnect_count, (unsigned long)backoff_ms);

    // Publish disconnect event
    EventBus::publish(EventType::WIFI_DISCONNECTED, 0);
    logger_flush();   // flush logov pred reconnectom

    // Čakaj backoff interval (s watchdog reset-i)
    if (backoff_ms > 0) {
        WF_I("Čakam %lu ms pred reconnectom...", (unsigned long)backoff_ms);
        update_status(WifiState::DISCONNECTED);

        uint32_t waited = 0;
        while (waited < backoff_ms) {
            uint32_t sleep_ms = (backoff_ms - waited > 5000) ? 5000 : (backoff_ms - waited);
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
            waited += sleep_ms;
            WF_D("  Backoff: %lu / %lu ms", (unsigned long)waited, (unsigned long)backoff_ms);
        }
    }

    update_status(WifiState::RECONNECTING);
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(500));

    bool ok = wifi_connect_best();

    if (ok) {
        WF_I("Reconnect uspešen — IP: %s", WiFi.localIP().toString().c_str());

        // Reset backoff
        if (take_mutex()) {
            s_status.backoff_ms      = BACKOFF_INITIAL_MS;
            s_status.connect_time_ms = millis();
            give_mutex();
        }

        // EventBus
        uint32_t ip_int = (uint32_t)WiFi.localIP();
        EventBus::publish(EventType::WIFI_CONNECTED, ip_int);

        // NTP resync po reconnectu
        if (!s_status.ntp_ok) {
            wifi_ntp_sync();
        } else {
            // Preveri ali je čas še veljaven
            time_t now = time(nullptr);
            if (now <= 1577836800UL) {
                WF_W("NTP: čas izgubljen ob reconnectu — ponovni sync");
                wifi_ntp_sync();
            }
        }

        update_status(WifiState::NTP_SYNCED);

    } else {
        // Neuspešen reconnect — povečaj backoff
        if (take_mutex()) {
            s_status.backoff_ms = min((uint32_t)(s_status.backoff_ms * BACKOFF_MULTIPLIER),
                                      (uint32_t)BACKOFF_MAX_MS);
            give_mutex();
        }
        WF_W("Reconnect neuspešen — naslednji backoff: %lu ms",
             (unsigned long)s_status.backoff_ms);
    }
}

// ============================================================
// wifiTask — zamenja BSP stub
// ============================================================

void wifiTask(void* pvParams) {
    WF_I("wifiTask start — Core%d prio:%d",
         xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    // wifiTask ni v TWDT — event-driven narava, ne garantira rednih klicev
    // esp_task_wdt_add(NULL) bi povzročil false reset ob dolgem čakanju

    // Mutex kreacija
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        WF_E("Mutex kreacija NAPAKA — wifiTask konča!");
        vTaskDelete(nullptr);
        return;
    }

    // Inicializacija statusa
    memset(&s_status, 0, sizeof(s_status));
    s_status.state      = WifiState::IDLE;
    s_status.backoff_ms = BACKOFF_INITIAL_MS;
    strncpy(s_status.ip_str, "0.0.0.0", sizeof(s_status.ip_str));
    strncpy(s_status.ntp_time, "--", sizeof(s_status.ntp_time));

    // Kratka zakasnitev — daj ostalim taskom čas za init
    WF_D("Čakam 1s da se ostali taski inicializirajo...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // --------------------------------------------------------
    // FAZA 1 — Prva povezava
    // --------------------------------------------------------
    bool connected = wifi_connect_best();

    if (connected) {
        update_status(WifiState::CONNECTED);

        uint32_t ip_int = (uint32_t)WiFi.localIP();
        EventBus::publish(EventType::WIFI_CONNECTED, ip_int);
        WF_I("EventBus: WIFI_CONNECTED IP=%s", WiFi.localIP().toString().c_str());

        // --------------------------------------------------------
        // FAZA 2 — Web UI start (pred NTP — heap je še neframentiran)
        // xTaskCreate za async_tcp zahteva 8 KB zaporednega SRAM.
        // NTP + mDNS alocirata SRAM bufferje ki fragmentirajo heap.
        // --------------------------------------------------------
        WF_I("=== SRAM pred web_ui_begin: %lu B (min-ever: %lu B) ===",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!web_ui_running()) {
            web_ui_begin();
        }
        WF_I("=== SRAM po  web_ui_begin: %lu B (min-ever: %lu B) ===",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        // --------------------------------------------------------
        // FAZA 3 — NTP sinhronizacija
        // --------------------------------------------------------
        WF_D("SRAM pred NTP sync: %lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        wifi_ntp_sync();
        WF_D("SRAM po  NTP sync:  %lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        // FAZA 4 — mDNS ODSTRANJEN (statična IP 192.168.2.170, mDNS ni potreben)

        update_status(WifiState::NTP_SYNCED);

    } else {
        // Prva povezava ni uspela — začni reconnect zanko
        WF_W("Prva WiFi povezava neuspešna — začenjam reconnect zanko");
        EventBus::publish(EventType::WIFI_DISCONNECTED, 0);
    }

    // --------------------------------------------------------
    // WATCHDOG ZANKA
    // --------------------------------------------------------
    WF_I("Vstopam v watchdog zanko (interval: %ds)", WATCHDOG_INTERVAL_MS / 1000);

    uint32_t last_watchdog_ms  = millis();
    uint32_t last_log_flush_ms = millis();

    while (true) {
        uint32_t now_ms = millis();

        // --------------------------------------------------------
        // Periodični log flush
        // --------------------------------------------------------
        if ((now_ms - last_log_flush_ms) >= LOG_FLUSH_INTERVAL_MS) {
            last_log_flush_ms = now_ms;
            WF_D("Periodični log flush...");
            logger_flush();
        }

        // --------------------------------------------------------
        // WiFi watchdog
        // --------------------------------------------------------
        if ((now_ms - last_watchdog_ms) >= WATCHDOG_INTERVAL_MS) {
            last_watchdog_ms = now_ms;
            WF_I("SRAM free: %lu B  min-ever: %lu B  PSRAM free: %lu B",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            WF_I("wifiTask stack: %lu B free / %d B total",
                 (unsigned long)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t),
                 TASK_WIFI_STACK);

            if (!wifi_watchdog_check()) {
                WF_W("Watchdog: izgubljena WiFi povezava — reconnect");
                wifi_reconnect();
                last_watchdog_ms = millis();   // reset po reconnectu
            }

            // NTP periodično preverjanje (vsakih 6h)
            wifi_ntp_check();
        }

        // --------------------------------------------------------
        // TODO: web_ui_handle() — ko bo web_ui.cpp implementiran
        // ESPAsyncWebServer je async — ne potrebuje eksplicitnega handle().
        // Tukaj bo šla logika WLED status polling za screen_party.cpp:
        //   if (connected && (millis() - last_wled_poll > 5000)) {
        //       wled_poll_status();
        //       last_wled_poll = millis();
        //   }
        // --------------------------------------------------------

        // Majhen sleep — ne blokiramo Core0 WiFi stacka
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

WifiStatus wifi_manager_get_status() {
    WifiStatus copy;
    if (take_mutex()) {
        copy = s_status;
        give_mutex();
    } else {
        memset(&copy, 0, sizeof(copy));
        copy.state = WifiState::IDLE;
    }
    return copy;
}

bool wifi_manager_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

const char* wifi_manager_get_ip_str() {
    // Statični buffer — ne shranjuj pointerja!
    static char ip_buf[16];
    if (take_mutex()) {
        strncpy(ip_buf, s_status.ip_str, sizeof(ip_buf) - 1);
        give_mutex();
    } else {
        strncpy(ip_buf, "0.0.0.0", sizeof(ip_buf));
    }
    return ip_buf;
}

const char* wifi_manager_get_wled_ip() {
    return WLED_PARTY_IP;
}

void wifi_manager_trigger_flush() {
    logger_flush();
}
