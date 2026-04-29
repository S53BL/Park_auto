// ============================================================
// wifi_manager.h — WiFi Manager
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// ODGOVORNOST:
//   Celoten WiFi lifecycle v wifiTask (Core0):
//     - Multi-network scan in connect (WIFI_SSID_LIST)
//     - Statična IP konfiguracija
//     - mDNS registracija (parking-esp32.local)
//     - NTP sinhronizacija (configTime, Slovenija CET/CEST)
//     - Periodični watchdog (ping / RSSI check vsakih 30s)
//     - Avtomatski reconnect ob izpadu z eksponentnim backoff
//     - EventBus publish ob vseh statusnih spremembah
//
// ARHITEKTURA:
//   wifi_manager.cpp implementira wifiTask() funkcijo ki
//   zamenja BSP stub (__attribute__((weak))).
//   Vse ostalo je interno — zunanji moduli kličejo samo:
//     wifi_manager_get_status()  — za /api/status
//     wifi_manager_get_ip_str()  — za screen_service.cpp
//     wifi_manager_is_connected()
//
// EVENTBUS EVENTI (publish):
//   WIFI_CONNECTED    payload = local IP kot uint32_t
//   WIFI_DISCONNECTED payload = zadnji disconnect vzrok (0=unknown)
//   NTP_SYNCED        payload = Unix timestamp ob sinhronizaciji
//
// NTP:
//   Strežnika: pool.ntp.org, time.google.com
//   Timezone:  CET-1CEST,M3.5.0,M10.5.0/3 (Slovenija)
//   Timeout:   15s za prvo sinhronizacijo
//   Po sinhronizaciji: logger_set_ntp_synced(true)
//   Periodično preverjanje: vsakih 6h (sntp_restart če drift > 60s)
//
// RECONNECT STRATEGIJA:
//   Ob izpadu: takoj poskusi reconnect (vsi SSID po vrstnem redu)
//   Vsak SSID: max 15s čakanja (30 × 500ms)
//   Cikel vseh SSID: če noben ne uspe → backoff
//   Backoff: 15s → 30s → 60s → 120s (max) — eksponentni
//   Med backoff: EventBus WIFI_DISCONNECTED, logger_flush()
//
// WATCHDOG (znotraj wifiTask):
//   Vsakih 30s: preveri WiFi.status() + RSSI
//   RSSI == 0 ali status != WL_CONNECTED → sproži reconnect
//   Ne-interferira z web_ui.cpp ki teče v istem tasku
//
// ODVISNOSTI:
//   wifi_config.h — credentials, statična IP, WLED IP
//   config.h      — WIFI_HOSTNAME, WEB_PORT
//   logger.h      — LOG_* makroji, logger_set_ntp_synced()
//   event_bus.h   — WIFI_CONNECTED, WIFI_DISCONNECTED, NTP_SYNCED
//
// ============================================================

#pragma once

#include <Arduino.h>
#include <stdint.h>

// ============================================================
// STATUS STRUKTURA
// ============================================================

enum class WifiState : uint8_t {
    IDLE        = 0,    // pred prvim connect poskusom
    CONNECTING  = 1,    // connect v teku
    CONNECTED   = 2,    // povezan, IP dodeljen
    NTP_SYNCED  = 3,    // povezan + NTP sinhroniziran
    DISCONNECTED = 4,   // izgubljena povezava, reconnect čaka
    RECONNECTING = 5,   // reconnect v teku
    FAILED      = 6     // vsi SSID neuspešni, v backoff fazi
};

struct WifiStatus {
    WifiState   state;
    char        ssid[33];           // trenutno ali zadnje omrežje
    char        ip_str[16];         // "192.168.2.170" ali "0.0.0.0"
    int8_t      rssi;               // [dBm] signal moč (0 = ni podatka)
    bool        ntp_ok;             // NTP sinhroniziran
    char        ntp_time[20];       // "2026-04-15 14:32:01" ali "--"
    uint32_t    connect_time_ms;    // millis() ob zadnjem uspešnem connect
    uint16_t    reconnect_count;    // skupno število reconnectov
    uint32_t    backoff_ms;         // trenutni backoff interval [ms]
};

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

// Vrne kopijo trenutnega statusa (thread-safe)
WifiStatus wifi_manager_get_status();

// true = WiFi.status() == WL_CONNECTED
bool wifi_manager_is_connected();

// Vrne IP kot string (npr. "192.168.2.170" ali "0.0.0.0")
// Pointer na interni statični buffer — ne shranjevati!
const char* wifi_manager_get_ip_str();

// Vrne WLED Party ESP IP iz wifi_config.h
// Kliče web_ui.cpp za WLED HTTP klice
const char* wifi_manager_get_wled_ip();

// Ekspliciten flush logov — kliče wifiTask periodično in ob izpadu
// Dostopno tudi web_ui.cpp za flush ob OTA
void wifi_manager_trigger_flush();
