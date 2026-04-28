// ============================================================
// wifi_config_template.h — WiFi konfiguracija PREDLOGA (GitHub)
// Projekt : Avtomatizacija Pokritega Parkirišča
// ============================================================
//
// NAVODILO:
//   1. Kopiraj to datoteko: wifi_config_template.h → wifi_config.h
//   2. V wifi_config.h izpolni svoje vrednosti
//   3. wifi_config.h je v .gitignore — ostane samo lokalno
//
// TA DATOTEKA (template) gre na GitHub — brez pravih credentials.
//
// ============================================================

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <IPAddress.h>

// ============================================================
// WiFi omrežja — preizkuša po vrstnem redu, vzame prvo dosegljivo
// Dodaj ali odstrani vnose po potrebi, posodobi WIFI_NETWORK_COUNT.
// ============================================================

static const char* WIFI_SSID_LIST[]     = { "ime_omrezja_1", "ime_omrezja_2" };
static const char* WIFI_PASS_LIST[]     = { "geslo_1",        "geslo_2"       };
static const int   WIFI_NETWORK_COUNT   = 2;

// ============================================================
// Statična IP konfiguracija
// Prilagodi svojemu omrežju (192.168.x.x ali 10.0.x.x)
// ============================================================

static const IPAddress WIFI_LOCAL_IP (192, 168, 1, 111);   // IP ESP32
static const IPAddress WIFI_GATEWAY  (192, 168, 1,   1);   // router
static const IPAddress WIFI_SUBNET   (255, 255, 255,  0);
static const IPAddress WIFI_DNS      (  8,   8,   8,  8);

// ============================================================
// mDNS hostname — dostopen kot http://<hostname>.local
// Mora biti enak WIFI_HOSTNAME v config.h
// ============================================================

static const char* WIFI_MDNS_HOSTNAME = "parking-esp32";

// ============================================================
// Party ESP (WLED) IP naslov
// Nastavi ko bo Party ESP priključen na omrežje.
// ============================================================

static const char* WLED_PARTY_IP = "192.168.2.xxx";   // TODO: nastavi

// ============================================================
// Web server port (mora biti enak WEB_PORT v config.h = 80)
// ============================================================

static const uint16_t WIFI_WEB_PORT = 80;

#endif // WIFI_CONFIG_H
