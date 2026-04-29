// ============================================================
// web_ui.h — Web UI vmesnik (REST API + statične datoteke)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// ODGOVORNOST:
//   ESPAsyncWebServer instanca in vse registracije handlerjev.
//   Servi statične datoteke iz LittleFS (/assets/) in implementira
//   vse REST API endpointе opisane v WebUI_Arhitektura.md.
//
//   Modul teče izključno v wifiTask (Core0) — skladno z
//   firmware_arhitektura.md (WiFi stack + web server na Core0).
//
// INICIALIZACIJSKI VRSTNI RED:
//   1. web_ui_init()  — v bsp_init() PRED task kreacijo
//                       (konfigurira LittleFS, preveri assets/)
//   2. web_ui_begin() — v wifiTask TAKOJ PO WiFi.begin()
//                       (zažene strežnik, registrira handlerje)
//   3. web_ui_stop()  — opcijsko ob izpadu WiFi ali OTA resetu
//
// ODVISNOSTI (Faza 1):
//   sd_mgr.h        — /files: list, download (streaming), delete
//   logger.h        — /api/logs: getBuffer, flush
//   wifi_manager.h  — /api/status: IP, RSSI, NTP čas, uptime
//   config.h        — WEB_PORT, WIFI_HOSTNAME
//   ArduinoJson v7  — JsonDocument za vse JSON odgovore
//   LittleFS        — statične datoteke (gzip assets)
//   ESPAsyncWebServer — asinhroni web strežnik
//
// ODVISNOSTI (Faza 3 — stub zdaj):
//   config_mgr.h    — /api/config: GET/POST/reset (NVS + LittleFS)
//   sensor_mgr.h    — /api/status: senzorji, SSR, parking
//   vehicle_recog.h — /api/vehicles: modeli vozil
//
// NITNA VARNOST:
//   ESPAsyncWebServer callback-i tečejo v task kontekstu Core0.
//   sd_mgr je thread-safe (interni mutex).
//   logger je thread-safe (interni mutex).
//   JsonDocument se kreira lokalno v vsakem handlerju — ni deljenega stanja.
//
// FAZA 1 — STUBS:
//   /api/config  → vrne RAM struct s privzetimi vrednostmi, POST shrani v RAM
//   /api/vehicles → vrne prazen JSON array za obe parkirni mesti
//   /api/status  → WiFi + SD + uptime — brez senzorjev (tisti pridejo s HAL fazo)
//
// ============================================================

#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>

// ============================================================
// KONFIGURACIJA
// ============================================================

// HTTP port — privzeto 80, override prek config.h WEB_PORT
#ifndef WEB_PORT
#define WEB_PORT 80
#endif

// Mapa s statičnimi datotekami v LittleFS
#define WEB_ASSETS_PATH "/assets"

// Mapa na SD kartici ki jo browsa /files endpoint
#define WEB_SD_ROOT_PATH "/"

// Max vrstic loga ki jih vrne GET /api/logs
#define WEB_LOG_MAX_LINES 200

// Max RAM za log odgovor (vrstica ~100 znakov × 200 = 20 KB)
#define WEB_LOG_BUF_SIZE  (24 * 1024)

// Max elementov za /files listing na klic
#define WEB_FILES_MAX_ENTRIES 64

// OTA upload buffer (streaming — ne celoten bin v RAM)
// ESPAsyncWebServer podpira chunked multipart upload
#define WEB_OTA_MAX_SIZE (3 * 1024 * 1024)  // 3 MB = max OTA particija

// ============================================================
// INICIALIZACIJA IN LIFECYCLE
// ============================================================

// Inicializacija — kliče bsp_init() PRED task kreacijo.
// Preveri LittleFS in assets/ mapo.
// Ne zažene strežnika — to naredi web_ui_begin().
// Vrne true = LittleFS OK in assets/ vsebuje vsaj index.html.
// Vrne false = LittleFS ni montiran ali assets/ manjka.
//              Strežnik se bo vseeno zagnal, samo statičnih datotek ne bo.
bool web_ui_init();

// Zažene strežnik in registrira vse handlerje.
// Kliče se iz wifiTask, POTEM ko ima sistem IP.
// Vrne true = strežnik uspešno zagnan.
// Vrne false = napaka pri zagonu (npr. port že zaseden).
bool web_ui_begin();

// Zaustavi strežnik (npr. pred OTA restartom).
void web_ui_stop();

// true = strežnik teče
bool web_ui_running();

// Vrne pointer na AsyncWebServer instanco.
// Kliče ga lahko npr. screen_service.cpp za prikaz QR kode.
// Vrne nullptr če web_ui_begin() še ni bil klican.
AsyncWebServer* web_ui_get_server();

// ============================================================
// DIAGNOSTIKA
// ============================================================

struct WebUiStats {
    uint32_t req_total;         // skupno število zahtevkov od zagona
    uint32_t req_api;           // /api/* zahtevki
    uint32_t req_files;         // /files zahtevki
    uint32_t req_static;        // statične datoteke
    uint32_t req_errors;        // 4xx + 5xx odgovori
    uint32_t ota_attempts;      // OTA upload poskusi
    uint32_t ota_success;       // uspešni OTA uploadi
    bool     server_running;    // strežnik teče
    bool     littlefs_ok;       // LittleFS montiran
    bool     assets_ok;         // assets/ mapa vsebuje index.html
};

WebUiStats web_ui_get_stats();
