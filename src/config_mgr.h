// ============================================================
// config_mgr.h — Config Manager: NVS persistenca nastavitev
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ODGOVORNOST:
//   Branje in pisanje nastavitev v NVS (Preferences).
//   Ob zagonu: preberi vsako vrednost iz NVS in preveri ali je
//   v veljavnem razponu. Če ni → vzemi hardkodiran default in
//   zapiši nazaj v NVS. Tako je zagotovljeno konsistentno stanje
//   ob prvem zagonu, po OTA posodobitvi, ali po korupciji NVS.
//
// ARHITEKTURA:
//   - Ena globalna Config struktura v RAM (s_config).
//   - config_mgr_init() jo napolni ob zagonu.
//   - config_get() vrne const referenco — zero-copy branje.
//   - config_set() + config_save() za spremembe (web UI, touch).
//   - config_reset_defaults() ponastavi vse na hardkodirane vrednosti
//     in zapiše v NVS.
//   - Thread-safe: FreeRTOS mutex varuje s_config in NVS dostop.
//
// NVS NAMESPACE: "parking"
//   Vsak parameter ima svoj ključ (max 15 znakov, NVS omejitev).
//   Ključi so definirani kot NVS_KEY_* konstante spodaj.
//
// VALIDACIJA:
//   Vsak parameter ima CFG_MIN_* in CFG_MAX_* mejni vrednosti.
//   Vrednost izven meja → zamenjaj z defaultom + zapiši v NVS.
//   To velja ob vsakem zagonu, ne samo prvem.
//
// ODVISNOSTI:
//   - Preferences (ESP32 Arduino — vgrajen, ni v lib_deps)
//   - logger.h
//   - FreeRTOS (semphr)
//
// ============================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "config.h"

// ============================================================
// PARTY STRUKTURY (pred Config — Config jih referencira)
// ============================================================

struct PartySlot {
    char     name[16];      // vidno na LCD in web, max 15 znakov + \0
    uint8_t  fx_id;         // WLED efekt ID (0-255)
    uint8_t  _pad[3];       // alignment za color_rgb
    uint32_t color_rgb;     // 0xRRGGBB; 0x000000 = "auto" (ne pošlji barve)
    uint8_t  brightness;    // 0-255
    uint8_t  speed;         // 0-255
    uint8_t  enabled;       // 1 = konfiguriran, 0 = gumb siv na LCD
    uint8_t  _pad2;         // alignment
};

struct PartySchedule {
    char     name[24];      // opis, npr. "Božič 2026"
    uint8_t  slot_idx;      // kateri slot aktivira (0-8)
    uint8_t  enabled;       // urnik aktiven
    uint8_t  from_month;    // 1-12
    uint8_t  from_day;      // 1-31
    uint8_t  to_month;      // 1-12
    uint8_t  to_day;        // 1-31
    uint8_t  use_lux_on;    // 1 = vklopi ob lux_on, 0 = ob time_on
    uint8_t  _pad;          // alignment za lux_on
    uint32_t lux_on;        // lux threshold za vklop (padec pod to vrednost)
    uint8_t  time_on_h;     // ura vklopa (0-23)
    uint8_t  time_on_m;     // minuta vklopa
    uint8_t  use_lux_off;   // 1 = ugasni ob lux_off, 0 = ob time_off
    uint8_t  _pad2;         // alignment za lux_off
    uint32_t lux_off;       // lux threshold za izklop (dvig nad to vrednost)
    uint8_t  time_off_h;    // ura izklopa (0-23), podpira čez polnoč
    uint8_t  time_off_m;    // minuta izklopa
    uint8_t  _pad3[2];      // struct alignment
};

// ============================================================
// CONFIG STRUKTURA
// ============================================================
// Vsa polja ki se persistirajo v NVS.
// Razdeljena po skupinah enako kot Web UI zavihki.

struct Config {

    // ----------------------------------------------------------
    // Tab: Osvetlitev
    // ----------------------------------------------------------

    // Čas (sekunde) preden se SSR1 (LED matrika) ugasne po zadnjem gibanju.
    // Retriggerable — vsak RADAR_MOTION resetira timer.
    uint32_t timeout_ssr1_s;        // default: 180,  min: 30,    max: 3600

    // Trajanje ročnega vklopa SSR1 ob dotiku gumba (minute).
    // V avtomatskem načinu: dotik podaljša timer za ta čas.
    uint32_t manual_extend_min;     // default: 10,   min: 5,     max: 60

    // Anti-forget timer za SSR2 (LED paneli) — ugasne po N minutah.
    uint32_t antiforgot_ssr2_min;   // default: 5,    min: 1,     max: 60

    // Anti-forget timer za SSR3 (reflektor) — ugasne po N minutah.
    uint32_t antiforgot_ssr3_min;   // default: 5,    min: 1,     max: 60

    // SSR2 sledi SSR1 v avtomatiki (slave). Ko false: SSR2 samo ročno.
    bool     ssr2_auto_night;       // default: true  (bool — validacija: samo 0/1)

    // DND (Do Not Disturb) — časovno okno ko je SSR2 opcijsko izklopljeno
    // in brightness znižan. Zahteva NTP. Overnight podprt (start > end).
    uint8_t  dnd_start_h;           // default: 22, min: 0, max: 23
    uint8_t  dnd_end_h;             // default: 6,  min: 0, max: 23
    bool     ssr2_dnd_disable;      // default: false — SSR2 ne sledi med DND

    // Prag lux za preklop noč/dan (hystereza: night<lux_night, day>lux_day).
    uint32_t lux_night;             // default: 40,   min: 1,     max: 200
    uint32_t lux_day;               // default: 70,   min: 1,     max: 500

    // Svetlost LED matrike ponoči (0–255).
    uint8_t  brightness_night;      // default: 120,  min: 10,    max: 255

    // ----------------------------------------------------------
    // Tab: LED animacije
    // ----------------------------------------------------------

    // Čas polnjenja LED matrike od vhoda do konca (ms).
    uint32_t fill_speed_ms;         // default: 6000, min: 500,   max: 30000

    // Čas praznjenja LED matrike (ms).
    uint32_t unfill_speed_ms;       // default: 3000, min: 500,   max: 30000

    // Trajanje fade-out animacije (ms).
    uint32_t fade_duration_ms;      // default: 800,  min: 100,   max: 5000

    // Ciljna svetlost LED matrike (0–255).
    uint8_t  target_brightness;     // default: 200,  min: 10,    max: 255

    // Zamik med vklopom SSR1 in SSR2 (ms) — SSR1 stabilizacija.
    uint32_t ssr2_delay_ms;         // default: 500,  min: 0,     max: 5000

    // Parking assist razdalje (mm) — zelena/rumena/rdeča cona.
    uint32_t pa_thresh_green_mm;    // default: 1500, min: 200,   max: 4000
    uint32_t pa_thresh_orange_mm;   // default: 1000, min: 100,   max: 3000
    uint32_t pa_thresh_red_mm;      // default: 500,  min: 50,    max: 2000

    // Čas stabilnosti za parking assist potrditev (sekunde).
    uint32_t pa_stability_s;        // default: 4,    min: 1,     max: 30

    // Čas prikaza ure na signalni LED po parkiranju (sekunde).
    uint32_t clock_duration_s;      // default: 10,   min: 0,     max: 60

    // Timer fotocelice — kdaj se signalna LED ugasne po prekinitvi (minute).
    uint32_t photocell_timer_min;   // default: 5,    min: 1,     max: 30

    // ----------------------------------------------------------
    // Tab: Identifikacija vozil
    // ----------------------------------------------------------

    // DTW razdalja — prag za pozitivno prepoznavo vozila.
    // Manjša vrednost = strožja prepoznava.
    float    dtw_threshold;         // default: 18.0, min: 1.0,   max: 100.0

    // Sakoe-Chiba band za DTW (točke).
    uint8_t  sakoe_radius;          // default: 15,   min: 1,     max: 40

    // Minimalno število TOF točk v profilu za veljavno skeniranje.
    uint8_t  min_profile_points;    // default: 25,   min: 10,    max: 80

    // Normalizacija profila na N točk pred DTW.
    uint8_t  normalize_points;      // default: 80,   min: 20,    max: 80

    // Delta filter — zavrzi TOF meritev če se razlikuje za več kot N mm.
    uint16_t delta_filter_mm;       // default: 15,   min: 5,     max: 100

    // Razdalja H senzorja (cm) pri kateri potrdimo vstop vozila.
    uint16_t phase_confirm_cm;      // default: 350,  min: 50,    max: 500

    // Čas stabilnosti H senzorja pred začetkom skeniranja (sekunde).
    float    stability_s;           // default: 1.5,  min: 0.5,   max: 10.0

    // Število raw profilov ki jih hranimo na SD na model (FIFO rotacija).
    uint8_t  raw_profiles_per_model; // default: 30,  min: 10,   max: 100

    // Interval periodičnega preverjanja prisotnosti vozila (minute).
    uint8_t  presence_check_min;    // default: 10,   min: 1,    max: 60

    // Toleranca za baseline primerjavo praznega mesta (mm).
    uint16_t empty_tolerance_mm;    // default: 200,  min: 50,   max: 500

    // ----------------------------------------------------------
    // Tab: Radar konfiguracija — per senzor (indeksi 0-3)
    // ----------------------------------------------------------
    // Parametri se ob zagonu pošljejo na vsak LD2410C radar prek
    // SC16IS752 UART. Ob web spremembi: NVS + radar takoj.
    //
    // max_dist: maksimalna razdalja zaznave [0-8, enota 0.75m]
    //   0=0m (izklopljeno), 1=0.75m, ..., 8=6m. Default=2 (1.5m)
    // move_sens: občutljivost gibanja [0-100]. Default=20 (nizka)
    // static_sens: občutljivost statičnih objektov [0-100]. Default=0
    // unmanned_s: čas [s] po katerem radar preklopi v "ni nikogar". Default=5
    uint8_t  radar_max_dist[4];     // default: 2, min: 0, max: 8
    uint8_t  radar_move_sens[4];    // default: 20, min: 0, max: 100
    uint8_t  radar_static_sens[4];  // default: 0, min: 0, max: 100
    uint16_t radar_unmanned_s[4];   // default: 5, min: 0, max: 65535

    // Persistence filter: N zaporednih frames pred SSR triggerjem (v sensor_mgr).
    // 0=izklopljeno, 1=vsak frame. Default=3 (300ms latenca)
    uint8_t  radar_persistence_n;   // default: 3, min: 0, max: 10

    // Radar polling interval in overflow prag (hal_radar v2.0)
    uint32_t radar_poll_interval_ms;      // default: 50, min: 10, max: 100
    uint32_t radar_max_consec_overflows;  // default: 10, min: 1,  max: 100

    // ----------------------------------------------------------
    // Tab: Party / WLED
    // ----------------------------------------------------------

    // IP naslov Party ESP (WLED), dostopen prek /api/party/config.
    char wled_ip[32];   // default: "192.168.4.1"

    // 9 nastavljivih party slotov (shranjeni kot NVS blobs).
    PartySlot party_slots[9];

    // 10 urnikov za avtomatski party vklop/izklop.
    PartySchedule party_schedules[10];

    // Čas mirovanja (sekunde) pred nadaljevanjem party po prekinitvi.
    uint32_t party_resume_delay_s;  // default: 30, min: 5, max: 300

    // ----------------------------------------------------------
    // Tab: Osvetlitev — SSR gumb labeli
    // ----------------------------------------------------------
    // Prikazano besedilo na vsakem SSR gumbu (LCD zaslon).
    // Max 23 znakov; nova vrstica z \n je dovoljena (2 vrstici).
    // Nastavljivo prek /api/config (web UI, zavihek Osvetlitev).
    char ssr_label[4][24];  // default: "SSR1".."SSR4"
};

// ============================================================
// HARDKODIRANI DEFAULTI
// ============================================================
// Ločena inline funkcija — vrne Config s privzetimi vrednostmi.
// Kliče se ob validaciji vsake vrednosti ki je izven meja,
// ali pri config_reset_defaults().

inline Config config_defaults() {
    Config c;
    // Osvetlitev
    c.timeout_ssr1_s       = 180;
    c.manual_extend_min    = 10;
    c.antiforgot_ssr2_min  = 5;
    c.antiforgot_ssr3_min  = 5;
    c.ssr2_auto_night      = true;
    c.dnd_start_h          = 22;
    c.dnd_end_h            = 6;
    c.ssr2_dnd_disable     = false;
    c.lux_night            = 40;
    c.lux_day              = 70;
    c.brightness_night     = 120;
    // LED animacije
    c.fill_speed_ms        = 6000;
    c.unfill_speed_ms      = 3000;
    c.fade_duration_ms     = 800;
    c.target_brightness    = 200;
    c.ssr2_delay_ms        = 500;
    c.pa_thresh_green_mm   = 1500;
    c.pa_thresh_orange_mm  = 1000;
    c.pa_thresh_red_mm     = 500;
    c.pa_stability_s       = 4;
    c.clock_duration_s     = 10;
    c.photocell_timer_min  = 5;
    // Identifikacija
    c.dtw_threshold        = 18.0f;
    c.sakoe_radius         = 15;
    c.min_profile_points   = 25;
    c.normalize_points     = 80;
    c.delta_filter_mm      = 15;
    c.phase_confirm_cm     = 350;
    c.stability_s          = 1.5f;
    c.raw_profiles_per_model = 30;
    c.presence_check_min   = 10;
    c.empty_tolerance_mm   = 200;
    // Radar — utišane začetne vrednosti (kalibrirati po namestitvi)
    for (int i = 0; i < 4; i++) {
        c.radar_max_dist[i]    = 2;    // 1.5m
        c.radar_move_sens[i]   = 20;   // nizka občutljivost
        c.radar_static_sens[i] = 0;    // statično izklopljeno
        c.radar_unmanned_s[i]  = 5;    // 5s
    }
    c.radar_persistence_n        = 3;
    c.radar_poll_interval_ms     = RADAR_POLL_INTERVAL_MS_DEFAULT;  // 50
    c.radar_max_consec_overflows = RADAR_MAX_CONSECUTIVE_OVERFLOWS;  // 10
    // Party / WLED
    strncpy(c.wled_ip, "192.168.4.1", sizeof(c.wled_ip));
    c.wled_ip[sizeof(c.wled_ip) - 1] = '\0';
    // Party resume delay
    c.party_resume_delay_s = 30;
    // Party slots — 9 prednastavitev
    memset(c.party_slots, 0, sizeof(c.party_slots));
    // slot 0: Solid
    strncpy(c.party_slots[0].name, "Solid",     16); c.party_slots[0].fx_id=0;  c.party_slots[0].color_rgb=0xFFFFFF; c.party_slots[0].brightness=180; c.party_slots[0].speed=0;   c.party_slots[0].enabled=1;
    // slot 1: Breathe
    strncpy(c.party_slots[1].name, "Breathe",   16); c.party_slots[1].fx_id=2;  c.party_slots[1].color_rgb=0xFBBF24; c.party_slots[1].brightness=80;  c.party_slots[1].speed=60;  c.party_slots[1].enabled=1;
    // slot 2: Rainbow
    strncpy(c.party_slots[2].name, "Rainbow",   16); c.party_slots[2].fx_id=9;  c.party_slots[2].color_rgb=0x000000; c.party_slots[2].brightness=191; c.party_slots[2].speed=128; c.party_slots[2].enabled=1;
    // slot 3: Chase
    strncpy(c.party_slots[3].name, "Chase",     16); c.party_slots[3].fx_id=28; c.party_slots[3].color_rgb=0xFF0000; c.party_slots[3].brightness=180; c.party_slots[3].speed=150; c.party_slots[3].enabled=1;
    // slot 4: Twinkle
    strncpy(c.party_slots[4].name, "Twinkle",   16); c.party_slots[4].fx_id=45; c.party_slots[4].color_rgb=0xFFFFFF; c.party_slots[4].brightness=150; c.party_slots[4].speed=100; c.party_slots[4].enabled=1;
    // slot 5: Strobe
    strncpy(c.party_slots[5].name, "Strobe",    16); c.party_slots[5].fx_id=30; c.party_slots[5].color_rgb=0xFFFFFF; c.party_slots[5].brightness=220; c.party_slots[5].speed=200; c.party_slots[5].enabled=1;
    // slot 6: Fire
    strncpy(c.party_slots[6].name, "Fire",      16); c.party_slots[6].fx_id=65; c.party_slots[6].color_rgb=0x000000; c.party_slots[6].brightness=200; c.party_slots[6].speed=128; c.party_slots[6].enabled=1;
    // slot 7: Meteor
    strncpy(c.party_slots[7].name, "Meteor",    16); c.party_slots[7].fx_id=58; c.party_slots[7].color_rgb=0xFFFFFF; c.party_slots[7].brightness=180; c.party_slots[7].speed=150; c.party_slots[7].enabled=1;
    // slot 8: Colorloop
    strncpy(c.party_slots[8].name, "Colorloop", 16); c.party_slots[8].fx_id=38; c.party_slots[8].color_rgb=0x000000; c.party_slots[8].brightness=160; c.party_slots[8].speed=80;  c.party_slots[8].enabled=1;
    // Party schedules — vsi disabled ob prvem zagonu
    memset(c.party_schedules, 0, sizeof(c.party_schedules));
    // SSR labeli
    for (int i = 0; i < 4; i++) snprintf(c.ssr_label[i], 24, "SSR%d", i + 1);
    return c;
}

// ============================================================
// JAVNI API
// ============================================================

// Inicializacija — kliči iz bsp_init() pred ostalimi moduli.
// Odpre NVS, prebere vse vrednosti, validira, zapiše default-e
// za neveljavne vrednosti. Vedno vrne true (degraded mode: samo RAM).
bool config_mgr_init();

// Vrne const referenco na trenutno Config v RAM-u.
// Thread-safe (mutex). Zero-copy za branje.
const Config& config_get();

// Nastavi eno ali več vrednosti v RAM-u.
// Ne zapiše v NVS — kliči config_save() za persistenco.
// Thread-safe (mutex).
void config_set(const Config& c);

// Zapiše trenutni RAM Config v NVS.
// Kliči po config_set() ko hočeš persistenco (web UI POST /api/config).
// Thread-safe (mutex).
bool config_save();

// Ponastavi na hardkodirane defaulte, zapiši v NVS.
// Kliči ob POST /api/config/reset ali na zahtevo uporabnika.
// Thread-safe (mutex).
bool config_reset_defaults();

// Diagnostika — vrne koliko vrednosti je bilo zamenjanih z defaultom
// pri zadnjem config_mgr_init() klicu.
uint8_t config_mgr_replaced_count();

// Vrne true če je config_mgr_init() uspešno zaključil.
bool config_mgr_ok();

// ============================================================
// PARTY SLOT API
// ============================================================

// Vrne kopijo slota (idx 0-8). Thread-safe.
PartySlot    config_get_party_slot(uint8_t idx);

// Nastavi slot v RAM (ne piše v NVS). Kliči config_save_party_slots() za persistenco.
void         config_set_party_slot(uint8_t idx, const PartySlot& slot);

// Shrani vseh 9 slotov v NVS kot blobs. Thread-safe.
bool         config_save_party_slots();

// ============================================================
// PARTY SCHEDULE API
// ============================================================

// Vrne kopijo urnika (idx 0-9). Thread-safe.
PartySchedule config_get_party_schedule(uint8_t idx);

// Nastavi urnik v RAM (ne piše v NVS). Kliči config_save_party_schedules() za persistenco.
void         config_set_party_schedule(uint8_t idx, const PartySchedule& sched);

// Shrani vseh 10 urnikov v NVS kot blobs. Thread-safe.
bool         config_save_party_schedules();
