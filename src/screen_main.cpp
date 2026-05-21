// ============================================================
// screen_main.cpp — Glavni zaslon
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.2.1  |  Datum: 2026-05
//
// RAD WIDGET — vizualna logika:
//
//   INACTIVE     — siva, fill=10% (fiksno), center="–", brez "m"
//   MOVING       — zelena (dinamična svetlost po energy), fill=moving_energy,
//                  center=razdalja (1 decimalna, "0" če =0), "m" vrstico nižje
//   STATIONARY   — modra (identična dinamika svetlosti kot zelena), fill=static_energy,
//                  center=razdalja, "m" vrstico nižje
//   CONFIG_ERROR — rdeča, fill=50% začasna ali 100% trajna, center="–"
//
// Peak arc (samo pri MOVING ko energy=100%):
//   Notranji lv_arc, linearno pada 100%→0 v unmanned_s sekund (privzeto 5s).
//   Timer: PEAK_TICK_MS (100ms). Ob novem 100% se ponastavi.
//   Ko doseže 0% → arc skrit.
//
// Bujenje zaslona:
//   Ko kateri koli senzor doseže 100% energy → hal_display_setBacklight(255)
//   (klic iz LVGL timer konteksta, kar je thread-safe).
//
// Razdalja v sredini: 1 decimalna ("1,6" "0,2"); če =0 → "0"; "m" vrstico nižje.
//
// ============================================================

#include "screen_main.h"
#include "event_bus.h"
#include "hal_tof.h"
#include "hal_display.h"    // hal_display_setBacklight — bujenje ob peaku
#include "hal_radar.h"      // hal_radar_get_status — peak_duration_ms iz unmanned_s
#include "vehicle_recog.h"  // vr_place_state_t, vehicle_recog_get_*
#include "config_mgr.h"     // config_get() za ssr_label
#include <time.h>           // time(), localtime_r() — topbar datum/ura

#include "logger.h"
#define SMNI(fmt, ...) LOG_INFO ("SMAIN", fmt, ##__VA_ARGS__)
#define SMND(fmt, ...) LOG_DEBUG("SMAIN", fmt, ##__VA_ARGS__)

// ============================================================
// BARVE
// ============================================================
#define C_OFF_BG        lv_color_hex(0x1E2A2F)
#define C_OFF_TXT       lv_color_hex(0x91FF66)
#define C_ON_BG         lv_color_hex(0x27241F)
#define C_ON_TXT        lv_color_hex(0xFFD466)
#define C_ON_MANUAL_TXT lv_color_hex(0xFF9B3B)
#define C_DIS_BG        lv_color_hex(0x1A1A1A)
#define C_DIS_TXT       lv_color_hex(0x64748B)
#define C_PKG_EMPTY_BG  lv_color_hex(0x1F2937)
#define C_PKG_OCC_BG    lv_color_hex(0x1C2A25)
#define C_RAD_LOW       lv_color_hex(0x334155)
#define C_RAD_MID       lv_color_hex(0xFBBF24)
#define C_RAD_HIGH      lv_color_hex(0x34D399)
#define C_SCREEN_BG     lv_color_hex(0x111827)
#define C_CAR_OCC       lv_color_hex(0x34D399)
#define C_CAR_EMPTY     lv_color_hex(0x64748B)
#define C_SSR_BORDER    lv_color_hex(0x334155)
#define C_HOLD_BAR_BG   lv_color_hex(0x1F2937)
#define C_PKG_OCC_BORDER   lv_color_hex(0xFBBF24)
#define C_PKG_OCC_NAME     lv_color_hex(0xF1F5F9)
#define C_PKG_EMPTY_BORDER lv_color_hex(0x334155)
#define C_PKG_EMPTY_NAME   lv_color_hex(0xCBD5E1)
#define C_PKG_TITLE     lv_color_hex(0x94A3B8)
#define C_PKG_STATS     lv_color_hex(0xCBD5E1)
#define C_RAD_ARC_BG    lv_color_hex(0x1F2937)
#define C_RAD_LABEL     lv_color_hex(0xCBD5E1)
// E3.3 — noč/dan indikator
#define C_NIGHT_BG      lv_color_hex(0x1A2035)
#define C_NIGHT_TXT     lv_color_hex(0x93C5FD)
#define C_DAY_BG        lv_color_hex(0x1F2A18)
#define C_DAY_TXT       lv_color_hex(0xA3E635)
// E3.2 — TOF faza barve
#define C_PHASE_IDLE    lv_color_hex(0x334155)
#define C_PHASE_DETECT  lv_color_hex(0xFBBF24)
#define C_PHASE_SCAN    lv_color_hex(0x60A5FA)
#define C_PHASE_DTW     lv_color_hex(0xA78BFA)
// E3.1 — ikona badge barve
#define C_BADGE_AUTO    lv_color_hex(0x1D4E3A)
#define C_BADGE_MANUAL  lv_color_hex(0x4A2E12)
#define C_BADGE_DIS     lv_color_hex(0x2A1F1F)

// ARC4x — radar arc barve
// INACTIVE: temno siva (arc komaj viden — nakaže lokacijo)
#define C_RAD_INACTIVE  lv_color_hex(0x2A3040)
// CONFIG_ERROR: rdeča
#define C_RAD_ERROR     lv_color_hex(0xDC2626)
// Besedilo v sredini arc-a: bela za razdaljo, svetlo siva za "m"
#define C_RAD_DIST_TXT  lv_color_hex(0xFFFFFF)
#define C_RAD_M_TXT     lv_color_hex(0x94A3B8)
// arc_over: oranžna (nad pragom) — thr_tick: bela črtice — peak_arc: rumena
#define C_RAD_OVER      lv_color_hex(0xF97316)
#define C_RAD_PEAK      lv_color_hex(0xFBBF24)

// ============================================================
// DIMENZIJE
// ============================================================
#define SCR_W  320
#define PAD    6

#define TOPBAR_H    30

#define SSR_BTN_W   ((SCR_W - PAD * 3) / 2)
#define SSR_BTN_H   88
#define SSR_SEC_H   (SSR_BTN_H * 2 + PAD * 3)
#define SSR_BAR_H   4
#define HOLD_MS     1200

#define SSR_Y_START (TOPBAR_H + PAD)

#define PKG_Y       (SSR_Y_START + SSR_SEC_H + PAD)
#define PKG_W       ((SCR_W - PAD * 3) / 2)
#define PKG_H       140
#define PKG_SEC_H   (PKG_H + PAD * 2)

#define RAD_Y       (PKG_Y + PKG_SEC_H)
#define RAD_W       ((SCR_W - PAD * 5) / 4)
#define RAD_ARC_SZ  62

// ARC4x — debelina arc-a (enaka za glavni in peak notranji arc)
#define RAD_ARC_WIDTH   6

// ARC4x — peak arc timer interval [ms]
// 100ms daje tekoče linearno zmanjševanje brez prekomerne obremenitve
#define PEAK_TICK_MS    100

// ARC4x — privzeti čas peak padanja, kadar unmanned_s ni nastavljen [ms]
#define PEAK_DEFAULT_DURATION_MS  5000

// ============================================================
// WIDGET STRUKTURE
// ============================================================

struct SsrWidget {
    lv_obj_t*      btn;
    lv_obj_t*      lbl_name;
    lv_obj_t*      lbl_countdown;
    lv_obj_t*      lbl_icon;
    lv_obj_t*      bar_hold;
    lv_obj_t*      lbl_lock;
    lv_timer_t*    hold_fire_timer;
    lv_timer_t*    hold_prog_timer;
    uint32_t       press_ms;
    uint8_t        idx;
    SsrDisplayData data;
};

struct CarIcon {
    lv_obj_t* body    = nullptr;
    lv_obj_t* roof    = nullptr;
    lv_obj_t* wheel_l = nullptr;
    lv_obj_t* wheel_r = nullptr;
};

struct PkgWidget {
    lv_obj_t*          card;
    lv_obj_t*          lbl_title;
    lv_obj_t*          lbl_name;
    lv_obj_t*          lbl_stats;
    lv_obj_t*          lbl_phase;
    lv_obj_t*          lbl_horiz;
    lv_obj_t*          lbl_p1;
    lv_obj_t*          lbl_p2;
    CarIcon            car;
    uint8_t            idx;
    ParkingDisplayData data;
};

// ── RadWidget ─────────────────────────────────────────────────────────────────
// lbl_dist: razdalja (bela, center); lbl_m: "m" enota (siva, vrstico nižje).
// peak_arc: notranji lv_arc, zunanji rob se dotika notranjega roba glavnega arca.
//   Prikazan samo med aktivnim peak efektom.
// peak_pct: float za tekoče linearno padanje (0–100).
// peak_timer: LVGL timer (100ms interval) med peak efektom.
// peak_duration_ms: čas celotnega padanja (iz unmanned_s NVS parametra, privzeto 5000ms).
// ─────────────────────────────────────────────────────────────────────────────
struct RadWidget {
    lv_obj_t*        arc;           // Glavni radar arc (dim zelena, 0–energy%)
    lv_obj_t*        arc_over;      // Oranžni nad-prag arc (75%–energy%, skrit ko < 75)
    lv_obj_t*        thr_tick;      // Bela črtice pri RAD_THRESHOLD_PCT (vedno vidna)
    lv_obj_t*        peak_arc;      // Notranji peak arc, rumena, sproži pri energy>=75
    lv_obj_t*        lbl_dist;      // Razdalja v sredini (npr. "1,6" ali "0")
    lv_obj_t*        lbl_m;         // Enota "m" — vrstico nižje, siva, samo če dist>0
    lv_obj_t*        lbl_name;      // Ime senzorja pod arcem (nespremenjen)
    uint8_t          idx;
    RadarArcData     last_data;     // Zadnji podatki za primerjavo

    // Peak efekt stanje
    float            peak_pct;      // Trenutni fill peak arc-a (0.0–100.0)
    lv_timer_t*      peak_timer;    // LVGL timer za peak padanje (nullptr če ni aktiven)
    uint32_t         peak_duration_ms; // Čas padanja iz 100→0% (iz NVS unmanned_s)
};

// ============================================================
// STATIČNI WIDGETI
// ============================================================

static SsrWidget   s_ssr[4]            = {};
static PkgWidget   s_pkg[2]            = {};
static RadWidget   s_rad[4]            = {};
static lv_timer_t* s_countdown_timer   = nullptr;
static lv_obj_t*   s_topbar            = nullptr;
static lv_obj_t*   s_topbar_date       = nullptr;   // levo: dd.mes.yyyy
static lv_obj_t*   s_topbar_time       = nullptr;   // center: HH:MM:SS
static lv_obj_t*   s_topbar_icon       = nullptr;   // desno: "))) 42lx" / "* 70lx"
static int         s_topbar_last_mday  = -1;        // za redko posodabljanje datuma
static bool        s_created           = false;
static lv_obj_t*   s_parent_scr        = nullptr;  // screen_alarm_hide() se vrne na ta objekt; brez getterja bi moral screen_alarm vključiti screen_main internals

// SSR labeli — ob kreaciji se preberejo iz config_get().ssr_label[]
// (SSR_NAMES[] je odstranjeno — labeli so zdaj nastavljivi prek web)
static const char* RADAR_NAMES[4] = { "Vhod", "Cesta L", "Cesta D", "Garaža" };
static const char* PHASE_NAMES[4] = { "·", "DETECT", "SCAN", "DTW" };

// ============================================================
// POMOŽNE
// ============================================================

static void fmt_cd(char* buf, size_t len, uint32_t sec) {
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

// ============================================================
// ARC4x — POMOŽNA: izračun barve z dinamično svetlostjo
// ============================================================
//
// Enaka funkcija za zeleno (MOVING) in modro (STATIONARY) — skladno z ARC4x spec.
// energy 0→100 → svetlost komponente narašča od 0x20 do 0xFF.
// Za zeleno: R=0, G=bright, B=0
// Za modro:  R=0, G=0, B=bright
// Dinamika je identična v obeh primerih — samo kanal se razlikuje.
//
// Zakaj 0x20 kot minimum (ne 0x00):
//   Pri energy=0 bi barva bila popolnoma črna — arc bi izgledal ugasnjen.
//   Minimalni svetlost 0x20 zagotavlja da je arc viden tudi pri nizki energiji,
//   kar je vizualno konsistentno z dinamičnim fill-om.
// ─────────────────────────────────────────────────────────────────────────────
static lv_color_t rad_color_moving(uint8_t energy) {
    // Zelena: G kanal 0x20..0xFF glede na energy
    uint8_t bright = 0x20u + (uint8_t)(((uint16_t)energy * 0xDFu) / 100u);
    return lv_color_make(0, bright, 0);
}

static lv_color_t rad_color_stationary(uint8_t energy) {
    // Modra: B kanal 0x20..0xFF glede na energy — identična dinamika kot zelena
    uint8_t bright = 0x20u + (uint8_t)(((uint16_t)energy * 0xDFu) / 100u);
    return lv_color_make(0, 0, bright);
}

// ============================================================
// ARC4x — POMOŽNA: formatiranje razdalje za prikaz v centru arc-a
// ============================================================
//
// Razdalja dist_cm → prikaz z eno decimalno (decimalna vejica po SI):
//   dist_cm=160 → "1,6"
//   dist_cm=20  → "0,2"
//   dist_cm=300 → "3,0"
//   dist_cm=0   → "0" (brez decimalke)
//
// Vejica (,) namesto pike (.): skladno s slovenskim standardom.
// ─────────────────────────────────────────────────────────────────────────────
static void fmt_dist(char* buf, size_t len, uint16_t dist_cm) {
    if (dist_cm == 0) {
        snprintf(buf, len, "0");
    } else {
        // Pretvori cm → m z eno decimalno: 160cm = 1,6m
        uint16_t m_int  = dist_cm / 100u;
        uint16_t m_dec  = (dist_cm % 100u) / 10u;   // zaokroži na prvo decimalo
        snprintf(buf, len, "%u,%u", m_int, m_dec);
    }
}

// ============================================================
// ARC4x — PEAK TIMER CALLBACK
// ============================================================
//
// Teče vsak PEAK_TICK_MS (100ms) dokler peak_pct > 0.
// Linearno zmanjša peak_pct za delež ki ustreza padanju 100→0 v peak_duration_ms.
// Ko peak_pct doseže 0: timer se ustavi, peak_arc se skrije.
//
// Linearni padec na tick:
//   dec_per_tick = 100.0 / (peak_duration_ms / PEAK_TICK_MS)
//   = 100.0 * PEAK_TICK_MS / peak_duration_ms
// ─────────────────────────────────────────────────────────────────────────────
static void peak_timer_cb(lv_timer_t* t) {
    RadWidget* w = (RadWidget*)lv_timer_get_user_data(t);
    if (!w) return;

    // Izračunaj padec na en tick
    float dec = (100.0f * (float)PEAK_TICK_MS) / (float)w->peak_duration_ms;
    w->peak_pct -= dec;

    if (w->peak_pct <= 0.0f) {
        // Peak je dosegel 0% — skrij notranji arc in ustavi timer
        w->peak_pct   = 0.0f;
        w->peak_timer = nullptr;
        lv_obj_add_flag(w->peak_arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(w->peak_arc, 0);
        lv_timer_delete(t);
        SMND("RAD[%d] peak arc = 0 → skrit", w->idx);
    } else {
        // Posodobi fill notranjega peak arc-a
        lv_arc_set_value(w->peak_arc, (int32_t)w->peak_pct);
    }
}

// ============================================================
// ARC4x — PEAK SPROŽITEV / PONASTAVITEV
// ============================================================
//
// Pokliče se ko energy=100% pri MOVING stanju.
// Če peak timer že teče (padanje v teku), ga ponastavi na 100%.
// Če peak timer ne teče, ustvari novega.
// Pokaže peak_arc in nastavi fill na 100%.
//
// Bujenje zaslona: backlight ON ob vsakem peak sprožitvi.
// Klic hal_display_setBacklight je varno v LVGL timer kontekstu
// (ui_refresh_cb → screen_main_set_radar_arc → peak_trigger).
// ─────────────────────────────────────────────────────────────────────────────
static void peak_trigger(RadWidget& w) {
    // Ponastavi fill na 100%
    w.peak_pct = 100.0f;
    lv_arc_set_value(w.peak_arc, 100);
    lv_obj_remove_flag(w.peak_arc, LV_OBJ_FLAG_HIDDEN);

    if (w.peak_timer) {
        // Timer že teče — samo ponastavi fill, ne ustvarjaj novega timerja
        SMND("RAD[%d] peak RESET na 100%% (timer že teče)", w.idx);
    } else {
        // Nov timer za padanje
        w.peak_timer = lv_timer_create(peak_timer_cb, PEAK_TICK_MS, &w);
        SMND("RAD[%d] peak START (duration=%lu ms)", w.idx, (unsigned long)w.peak_duration_ms);
    }

    // Zbudi zaslon — peak = močno gibanje → uporabnik naj vidi
    hal_display_setBacklight(255);
    SMND("RAD[%d] peak → backlight ON", w.idx);
}

// ============================================================
// ARC4x — PEAK ZAUSTAVITEV
// ============================================================
//
// Pokliče se kadar senzor preide iz MOVING v drugo stanje.
// Ustavi timer in skrije peak arc takoj.
// ─────────────────────────────────────────────────────────────────────────────
static void peak_stop(RadWidget& w) {
    if (w.peak_timer) {
        lv_timer_delete(w.peak_timer);
        w.peak_timer = nullptr;
    }
    w.peak_pct = 0.0f;
    lv_arc_set_value(w.peak_arc, 0);
    lv_obj_add_flag(w.peak_arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(w.arc_over, 0);
    lv_obj_add_flag(w.arc_over, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// ARC4x — RAD_APPLY: posodobitev arc widgeta z novimi podatki
// ============================================================
//
// Vizualna logika po ARC4x spec:
//
//   INACTIVE (detection=0, senzor aktiven):
//     — siva (C_RAD_INACTIVE), fill fiksno 10%
//     — center: "–" (brez "m")
//     — peak: zaustavitev če je tekel
//
//   MOVING (detection=1 ali 3):
//     — zelena (dinamična svetlost po energy)
//     — fill = energy (0–100)
//     — center: razdalja iz moving_dist_cm (že preračunana v dist_cm)
//     — peak: sproži pri energy=100%, ponastavitev če pride znova
//
//   STATIONARY (detection=2):
//     — modra (identična dinamika svetlosti kot zelena)
//     — fill = energy (0–100)
//     — center: razdalja iz static_dist_cm (že preračunana v dist_cm)
//     — peak: zaustavitev (peak je samo za MOVING)
//
//   CONFIG_ERROR:
//     — rdeča (C_RAD_ERROR)
//     — fill: 100% če is_permanent_error=true, 50% če začasna
//     — center: "–" (brez "m")
//     — peak: zaustavitev
//
// dist_cm=0 → prikaže samo "0", brez "m" oznake
// dist_cm>0 → prikaže formatirano razdaljo + "m" vrstico nižje
// ─────────────────────────────────────────────────────────────────────────────
static void rad_apply(RadWidget& w, const RadarArcData& data) {
    lv_color_t arc_color;
    int32_t    fill_val;
    char       dist_buf[8] = "–";
    bool       show_m      = false;

    switch (data.state) {

        // ── INACTIVE ──────────────────────────────────────────
        case RadarArcState::INACTIVE:
            arc_color = C_RAD_INACTIVE;
            fill_val  = 10;   // Fiksno 10% — arc komaj viden, a nakaže lokacijo
            // center = "–", brez "m"
            snprintf(dist_buf, sizeof(dist_buf), "0");  // "0" brez "m"
            show_m = false;
            peak_stop(w);
            break;

        // ── MOVING ────────────────────────────────────────────
        case RadarArcState::MOVING:
            arc_color = rad_color_moving(data.energy);
            fill_val  = data.energy;
            if (data.dist_cm > 0) {
                fmt_dist(dist_buf, sizeof(dist_buf), data.dist_cm);
                show_m = true;
            } else {
                snprintf(dist_buf, sizeof(dist_buf), "0");
                show_m = false;
            }
            // arc_over (oranžna): prikaže prekoračeni del nad RAD_THRESHOLD_PCT
            if (data.energy >= RAD_THRESHOLD_PCT) {
                // Preslika energy RAD_THRESHOLD_PCT..100 → arc_over 0..100
                int32_t over_val = (int32_t)(data.energy - RAD_THRESHOLD_PCT) * 100
                                   / (100 - RAD_THRESHOLD_PCT);
                if (over_val > 100) over_val = 100;
                lv_arc_set_value(w.arc_over, over_val);
                lv_obj_remove_flag(w.arc_over, LV_OBJ_FLAG_HIDDEN);
                peak_trigger(w);
            } else {
                lv_arc_set_value(w.arc_over, 0);
                lv_obj_add_flag(w.arc_over, LV_OBJ_FLAG_HIDDEN);
            }
            break;

        // ── STATIONARY ────────────────────────────────────────
        case RadarArcState::STATIONARY:
            arc_color = rad_color_stationary(data.energy);
            fill_val  = data.energy;
            if (data.dist_cm > 0) {
                fmt_dist(dist_buf, sizeof(dist_buf), data.dist_cm);
                show_m = true;
            } else {
                snprintf(dist_buf, sizeof(dist_buf), "0");
                show_m = false;
            }
            // arc_over ni relevanten za statično zaznavanje — skrij
            lv_arc_set_value(w.arc_over, 0);
            lv_obj_add_flag(w.arc_over, LV_OBJ_FLAG_HIDDEN);
            break;

        // ── CONFIG_ERROR ──────────────────────────────────────
        case RadarArcState::CONFIG_ERROR:
        default:
            arc_color = C_RAD_ERROR;
            // Trajna napaka (senzor ni inicializiran ob zagonu): arc 100%
            // Začasna napaka (senzor deluje, config je padel): arc 50%
            fill_val  = data.is_permanent_error ? 100 : 50;
            snprintf(dist_buf, sizeof(dist_buf), "\xe2\x80\x93"); // "–"
            show_m = false;
            peak_stop(w);
            break;
    }

    // ── Posodobi glavni arc ───────────────────────────────────
    lv_arc_set_value(w.arc, fill_val);
    lv_obj_set_style_arc_color(w.arc, arc_color, LV_PART_INDICATOR);

    // ── Posodobi center tekst (razdalja) ─────────────────────
    lv_label_set_text(w.lbl_dist, dist_buf);

    // ── Posodobi "m" oznako ───────────────────────────────────
    if (show_m) {
        lv_obj_remove_flag(w.lbl_m, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_m, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Shrani zadnje podatke ─────────────────────────────────
    w.last_data = data;
}

// ============================================================
// ARC4x — RAD_CREATE: kreacija widgeta z vsemi novimi elementi
// ============================================================
//
// Notranji peak arc dimenzije:
//   Zunanji rob notranjega arca = notranji rob glavnega arca.
//   Notranji rob = zunanji rob - arc_width × 2 (za oba arca).
//   Radij notranjega arca = RAD_ARC_SZ - RAD_ARC_WIDTH * 2
//   (vsak arc ima RAD_ARC_WIDTH debelino, notranji je znotraj)
//
// Pozicija lbl_dist (center arca):
//   Horizontalni center: arc_x + RAD_ARC_SZ / 2
//   Vertikalni center: arc_y + RAD_ARC_SZ / 2 - font_height/2
//   lbl_m je ena standardna vrstica pod lbl_dist (~16px pri font 14)
//
// Preveriti ali je 4× arc (4 × ~80px) dovolj za razdaljo + "m":
//   RAD_W = (320 - 5×6) / 4 = (320-30)/4 = 72px
//   RAD_ARC_SZ = 62px, center tekst font=14 → ok za 3-4 znake ("1,6")
//   Če "m" ne gre, je LV_OBJ_FLAG_HIDDEN (kontrolira show_m logika zgoraj)
// ─────────────────────────────────────────────────────────────────────────────
static void rad_create(uint8_t idx, lv_obj_t* parent, int x, int y) {
    RadWidget& w = s_rad[idx];
    w.idx             = idx;
    w.last_data       = {};
    w.peak_pct        = 0.0f;
    w.peak_timer      = nullptr;
    // Privzeta vrednost peak padanja — prepisana ob prvem screen_main_set_radar_arc klicu
    // iz NVS unmanned_s parametra (prek RadarArcData.peak_duration_ms ko bo dodan)
    // Za zdaj: PEAK_DEFAULT_DURATION_MS = 5000ms
    w.peak_duration_ms = PEAK_DEFAULT_DURATION_MS;

    // Arc X pozicija (centriran znotraj RAD_W)
    int arc_x = x + (RAD_W - RAD_ARC_SZ) / 2;
    int arc_y = y;

    // ── Glavni arc ────────────────────────────────────────────
    w.arc = lv_arc_create(parent);
    lv_obj_set_size(w.arc, RAD_ARC_SZ, RAD_ARC_SZ);
    lv_obj_set_pos(w.arc, arc_x, arc_y);
    lv_arc_set_rotation(w.arc, 135);        // Začne spodaj levo
    lv_arc_set_bg_angles(w.arc, 0, 270);    // 270° razpon (3/4 kroga)
    lv_arc_set_value(w.arc, 10);            // Privzeto INACTIVE = 10%
    lv_arc_set_range(w.arc, 0, 100);
    lv_obj_remove_flag(w.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(w.arc, C_RAD_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(w.arc, RAD_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.arc, C_RAD_INACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(w.arc, RAD_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(w.arc, 0, LV_PART_KNOB);

    // ── Peak arc (notranji) ───────────────────────────────────
    // Radij: RAD_ARC_SZ - RAD_ARC_WIDTH * 2 (enaka debelina, znotraj glavnega)
    // Pozicija: arc_x + RAD_ARC_WIDTH, arc_y + RAD_ARC_WIDTH
    // Zunanji rob notranjega arca se točno dotika notranjega roba glavnega arca.
    int peak_sz = RAD_ARC_SZ - RAD_ARC_WIDTH * 2;
    // Zagotovi minimalno velikost (pri majhnih arc-ih)
    if (peak_sz < 10) peak_sz = 10;

    w.peak_arc = lv_arc_create(parent);
    lv_obj_set_size(w.peak_arc, peak_sz, peak_sz);
    // Centriranje: pomakni za RAD_ARC_WIDTH znotraj
    lv_obj_set_pos(w.peak_arc, arc_x + RAD_ARC_WIDTH, arc_y + RAD_ARC_WIDTH);
    lv_arc_set_rotation(w.peak_arc, 135);
    lv_arc_set_bg_angles(w.peak_arc, 0, 270);
    lv_arc_set_value(w.peak_arc, 0);
    lv_arc_set_range(w.peak_arc, 0, 100);
    lv_obj_remove_flag(w.peak_arc, LV_OBJ_FLAG_CLICKABLE);
    // Ozadje notranjega arc-a: prozorno (ne prikazujemo nedoseženega dela)
    lv_obj_set_style_arc_opa(w.peak_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.peak_arc, C_RAD_PEAK, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(w.peak_arc, RAD_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w.peak_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(w.peak_arc, 0, LV_PART_KNOB);
    // Privzeto skrit — prikaže se samo ob peak efektu
    lv_obj_add_flag(w.peak_arc, LV_OBJ_FLAG_HIDDEN);

    // ── arc_over: oranžni nad-prag arc (na vrhu glavnega) ────
    // Začne pri kotu črtice (75% od 270°), zapolni razliko do energy.
    // Skrit dokler energy < RAD_THRESHOLD_PCT.
    {
        int32_t tick_ang = (int32_t)270 * RAD_THRESHOLD_PCT / 100;  // 202°

        w.arc_over = lv_arc_create(parent);
        lv_obj_set_size(w.arc_over, RAD_ARC_SZ, RAD_ARC_SZ);
        lv_obj_set_pos(w.arc_over, arc_x, arc_y);
        lv_arc_set_rotation(w.arc_over, 135);
        lv_arc_set_bg_angles(w.arc_over, (uint16_t)tick_ang, 270);
        lv_arc_set_value(w.arc_over, 0);
        lv_arc_set_range(w.arc_over, 0, 100);
        lv_obj_remove_flag(w.arc_over, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(w.arc_over, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(w.arc_over, C_RAD_OVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(w.arc_over, RAD_ARC_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(w.arc_over, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(w.arc_over, 0, LV_PART_KNOB);
        lv_obj_add_flag(w.arc_over, LV_OBJ_FLAG_HIDDEN);

        // ── thr_tick: bela črtice 3° pri poziciji praga ──────
        w.thr_tick = lv_arc_create(parent);
        lv_obj_set_size(w.thr_tick, RAD_ARC_SZ, RAD_ARC_SZ);
        lv_obj_set_pos(w.thr_tick, arc_x, arc_y);
        lv_arc_set_rotation(w.thr_tick, 135);
        lv_arc_set_bg_angles(w.thr_tick, (uint16_t)tick_ang, (uint16_t)(tick_ang + 3));
        lv_arc_set_value(w.thr_tick, 100);
        lv_arc_set_range(w.thr_tick, 0, 100);
        lv_obj_remove_flag(w.thr_tick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(w.thr_tick, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(w.thr_tick, lv_color_white(), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(w.thr_tick, 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(w.thr_tick, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(w.thr_tick, 0, LV_PART_KNOB);
    }

    // ── Razdalja v sredini arca (lbl_dist) ───────────────────
    // Vertikalna pozicija: center arc-a minus polovica fonta (pribl.)
    // font_montserrat_14: višina ~16px → center offset = -8
    // Postavimo malo višje ker je pod njim še lbl_m
    int center_x = arc_x + RAD_ARC_SZ / 2;
    int center_y = arc_y + RAD_ARC_SZ / 2;

    w.lbl_dist = lv_label_create(parent);
    lv_label_set_text(w.lbl_dist, "\xe2\x80\x93");   // "–" = INACTIVE privzeto
    lv_obj_set_style_text_color(w.lbl_dist, C_RAD_DIST_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_dist, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_style_text_align(w.lbl_dist, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    // Centriramo: arc center - 12px (polovica tipičnega teksta "1,6" pri font14)
    lv_obj_set_pos(w.lbl_dist, center_x - 12, center_y - 14);
    lv_obj_set_width(w.lbl_dist, 24);

    // ── "m" oznaka (lbl_m) — vrstico nižje ──────────────────
    // Prikazana samo kadar dist_cm > 0, svetlo siva
    w.lbl_m = lv_label_create(parent);
    lv_label_set_text(w.lbl_m, "m");
    lv_obj_set_style_text_color(w.lbl_m, C_RAD_M_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_m, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_style_text_align(w.lbl_m, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_m, center_x - 6, center_y + 2);   // Ena vrstica nižje
    lv_obj_set_width(w.lbl_m, 12);
    lv_obj_add_flag(w.lbl_m, LV_OBJ_FLAG_HIDDEN);   // Privzeto skrita

    // ── Ime senzorja pod arcem (lbl_name) ────────────────────
    w.lbl_name = lv_label_create(parent);
    lv_label_set_text(w.lbl_name, RADAR_NAMES[idx]);
    lv_obj_set_style_text_color(w.lbl_name, C_RAD_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_name, x, arc_y + RAD_ARC_SZ + 2);
    lv_obj_set_width(w.lbl_name, RAD_W);
    lv_obj_set_style_text_align(w.lbl_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

// ============================================================
// SSR — STILIZACIJA (E3.1)
// ============================================================

static void ssr_style(SsrWidget& w) {
    lv_color_t bg, tc;
    const char* icon_txt;

    switch (w.data.state) {
        case DisplaySsrState::ON:
            bg = C_ON_BG;
            tc = w.data.is_manual ? C_ON_MANUAL_TXT : C_ON_TXT;
            icon_txt = w.data.is_manual ? "\xE2\x9C\x8B" : "\xE2\x96\xB6";   // ✋ / ▶
            break;
        case DisplaySsrState::SSR_DISABLED:
            bg = C_DIS_BG; tc = C_DIS_TXT;
            icon_txt = "\xE2\x9C\x96";   // ✖
            break;
        default:
            bg = C_OFF_BG; tc = C_OFF_TXT;
            icon_txt = "";
            break;
    }

    lv_obj_set_style_bg_color(w.btn, bg, LV_PART_MAIN);

    if (w.data.state == DisplaySsrState::SSR_DISABLED) {
        lv_obj_set_style_bg_color(w.btn, lv_color_hex(0x2A2A2A),
            LV_PART_MAIN | LV_STATE_PRESSED);
    }

    lv_obj_set_style_text_color(w.lbl_name, tc, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.bar_hold, tc, LV_PART_INDICATOR);

    lv_label_set_text(w.lbl_icon, icon_txt);
    lv_obj_set_style_text_color(w.lbl_icon, tc, LV_PART_MAIN);
    if (w.data.state == DisplaySsrState::ON) {
        lv_obj_set_style_bg_color(w.lbl_icon,
            w.data.is_manual ? C_BADGE_MANUAL : C_BADGE_AUTO, LV_PART_MAIN);
        lv_obj_remove_flag(w.lbl_icon, LV_OBJ_FLAG_HIDDEN);
    } else if (w.data.state == DisplaySsrState::SSR_DISABLED) {
        lv_obj_set_style_bg_color(w.lbl_icon, C_BADGE_DIS, LV_PART_MAIN);
        lv_obj_remove_flag(w.lbl_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // Countdown — countdown barva je vedno modra (neodvisno od stanja)
    if (w.data.state == DisplaySsrState::ON && w.data.countdown_s > 0) {
        char buf[8]; fmt_cd(buf, sizeof(buf), w.data.countdown_s);
        lv_label_set_text(w.lbl_countdown, buf);
        if (w.data.countdown_s < 60) {
            lv_obj_set_style_text_font(w.lbl_countdown,
                &font_montserrat_28_sl, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_font(w.lbl_countdown,
                &font_montserrat_24_sl, LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(w.lbl_countdown,
            lv_color_hex(0x60A5FA), LV_PART_MAIN);
        lv_obj_remove_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);
    }

    if (w.data.state == DisplaySsrState::SSR_DISABLED)
        lv_obj_remove_flag(w.lbl_lock, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(w.lbl_lock, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// SSR — TIMER CALLBACKI
// ============================================================

static void ssr_hold_fire_cb(lv_timer_t* t) {
    SsrWidget* w = (SsrWidget*)lv_timer_get_user_data(t);
    if (!w) return;
    if (w->hold_prog_timer) {
        lv_timer_delete(w->hold_prog_timer);
        w->hold_prog_timer = nullptr;
    }
    w->hold_fire_timer = nullptr;
    lv_bar_set_value(w->bar_hold, 0, LV_ANIM_OFF);
    lv_obj_add_flag(w->bar_hold, LV_OBJ_FLAG_HIDDEN);
    SMND("SSR[%d] HOLD → BUTTON_SSR_DISABLE", w->idx);
    EventBus::publish(EventType::BUTTON_SSR_DISABLE, (uint32_t)w->idx);
}

static void ssr_hold_prog_cb(lv_timer_t* t) {
    SsrWidget* w = (SsrWidget*)lv_timer_get_user_data(t);
    if (!w) return;
    uint32_t elapsed = millis() - w->press_ms;
    int32_t  pct     = (int32_t)((elapsed * 100UL) / HOLD_MS);
    if (pct > 100) pct = 100;
    lv_obj_remove_flag(w->bar_hold, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(w->bar_hold, pct, LV_ANIM_OFF);
}

static void ssr_countdown_cb(lv_timer_t*) {
    for (int i = 0; i < 4; i++) {
        SsrWidget& w = s_ssr[i];
        if (w.data.state != DisplaySsrState::ON || w.data.countdown_s == 0) continue;
        w.data.countdown_s--;
        char buf[8]; fmt_cd(buf, sizeof(buf), w.data.countdown_s);
        lv_label_set_text(w.lbl_countdown, buf);
        if (w.data.countdown_s == 59) {
            lv_obj_set_style_text_font(w.lbl_countdown,
                &font_montserrat_28_sl, LV_PART_MAIN);
        }
    }
}

static void ssr_cancel_hold(SsrWidget& w) {
    if (w.hold_fire_timer) {
        lv_timer_delete(w.hold_fire_timer);
        w.hold_fire_timer = nullptr;
    }
    if (w.hold_prog_timer) {
        lv_timer_delete(w.hold_prog_timer);
        w.hold_prog_timer = nullptr;
    }
    lv_bar_set_value(w.bar_hold, 0, LV_ANIM_OFF);
    lv_obj_add_flag(w.bar_hold, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// SSR — EVENT CALLBACK
// ============================================================

static void ssr_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    SsrWidget* w = (SsrWidget*)lv_event_get_user_data(e);
    if (!w) return;

    bool is_disabled = (w->data.state == DisplaySsrState::SSR_DISABLED);

    if (code == LV_EVENT_PRESSED) {
        w->press_ms = millis();
        ssr_cancel_hold(*w);
        w->hold_fire_timer = lv_timer_create(ssr_hold_fire_cb, HOLD_MS, w);
        lv_timer_set_repeat_count(w->hold_fire_timer, 1);
        w->hold_prog_timer = lv_timer_create(ssr_hold_prog_cb, 50, w);
        lv_timer_set_repeat_count(w->hold_prog_timer, -1);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_SHORT_CLICKED) {
        bool was_short = (w->hold_fire_timer != nullptr);
        ssr_cancel_hold(*w);
        if (was_short && !is_disabled) {
            SMND("SSR[%d] SHORT TAP → BUTTON_SSR", w->idx);
            EventBus::publish(EventType::BUTTON_SSR, (uint32_t)w->idx);
        }
    }
    else if (code == LV_EVENT_PRESS_LOST) {
        bool was_short = (w->hold_fire_timer != nullptr);
        ssr_cancel_hold(*w);
        if (was_short && !is_disabled) {
            SMND("SSR[%d] PRESS_LOST→tap → BUTTON_SSR", w->idx);
            EventBus::publish(EventType::BUTTON_SSR, (uint32_t)w->idx);
        }
    }
}

// ============================================================
// SSR — KREACIJA
// ============================================================

static void ssr_create(uint8_t idx, lv_obj_t* parent, int x, int y) {
    SsrWidget& w      = s_ssr[idx];
    w.idx             = idx;
    w.hold_fire_timer = nullptr;
    w.hold_prog_timer = nullptr;
    w.press_ms        = 0;
    w.data            = { DisplaySsrState::OFF, 0, false };

    w.btn = lv_obj_create(parent);
    lv_obj_set_size(w.btn, SSR_BTN_W, SSR_BTN_H);
    lv_obj_set_pos(w.btn, x, y);
    lv_obj_set_style_bg_color(w.btn, C_OFF_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(w.btn, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(w.btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(w.btn, C_SSR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(w.btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(w.btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w.btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w.btn, ssr_event_cb, LV_EVENT_ALL, &w);

    lv_obj_set_style_bg_color(w.btn, lv_color_hex(0x2E3A3F),
        LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(w.btn, LV_OPA_COVER,
        LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(w.btn, lv_color_hex(0x6A8A9A),
        LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(w.btn, 2,
        LV_PART_MAIN | LV_STATE_PRESSED);

    w.lbl_name = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_name, config_get().ssr_label[idx]);
    lv_obj_set_style_text_color(w.lbl_name, C_OFF_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_name, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_label_set_long_mode(w.lbl_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w.lbl_name, SSR_BTN_W / 2);

    // Badge ikona desno zgoraj — font_24 za večjo vidnost
    w.lbl_icon = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_icon, "");
    lv_obj_set_style_text_font(w.lbl_icon, &font_montserrat_24_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_icon, C_OFF_TXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w.lbl_icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.lbl_icon, C_BADGE_AUTO, LV_PART_MAIN);
    lv_obj_set_style_radius(w.lbl_icon, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(w.lbl_icon, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(w.lbl_icon, 3, LV_PART_MAIN);
    lv_obj_align(w.lbl_icon, LV_ALIGN_TOP_RIGHT, -6, 5);
    lv_obj_add_flag(w.lbl_icon, LV_OBJ_FLAG_HIDDEN);

    w.lbl_countdown = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_countdown, "00:00");
    lv_obj_set_style_text_color(w.lbl_countdown, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_countdown, &font_montserrat_24_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_countdown, LV_ALIGN_BOTTOM_MID, 20, -8);
    lv_obj_add_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);

    w.bar_hold = lv_bar_create(w.btn);
    lv_obj_set_size(w.bar_hold, SSR_BTN_W - 2, SSR_BAR_H);
    lv_obj_align(w.bar_hold, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_bar_set_range(w.bar_hold, 0, 100);
    lv_bar_set_value(w.bar_hold, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(w.bar_hold, C_HOLD_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.bar_hold, C_OFF_TXT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(w.bar_hold, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(w.bar_hold, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(w.bar_hold, LV_OBJ_FLAG_HIDDEN);

    // lbl_lock — ✖ badge (lbl_icon) prevzame vlogo disabled indikatorja
    w.lbl_lock = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_lock, "");
    lv_obj_add_flag(w.lbl_lock, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// PARKING — AVTO IKONA
// ============================================================

static lv_obj_t* car_part(lv_obj_t* parent, int x, int y,
                            int w, int h, int r, lv_color_t col) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, col, LV_PART_MAIN);
    lv_obj_set_style_radius(o, r, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void car_create(PkgWidget& w, int ox, int oy, lv_color_t col) {
    w.car.body    = car_part(w.card, ox,      oy + 14, 52, 22, 4, col);
    w.car.roof    = car_part(w.card, ox + 10, oy + 3,  32, 14, 4, col);
    w.car.wheel_l = car_part(w.card, ox + 4,  oy + 28, 12, 12, 6, col);
    w.car.wheel_r = car_part(w.card, ox + 36, oy + 28, 12, 12, 6, col);
}

static void car_set_color(CarIcon& c, lv_color_t col) {
    if (c.body)    lv_obj_set_style_bg_color(c.body,    col, LV_PART_MAIN);
    if (c.roof)    lv_obj_set_style_bg_color(c.roof,    col, LV_PART_MAIN);
    if (c.wheel_l) lv_obj_set_style_bg_color(c.wheel_l, col, LV_PART_MAIN);
    if (c.wheel_r) lv_obj_set_style_bg_color(c.wheel_r, col, LV_PART_MAIN);
}

// ============================================================
// PARKING — RENAME DIALOG (B4)
// ============================================================

static uint8_t s_rename_pkg_idx = 0;

static void pkg_rename_ok_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
    if (!ta) return;
    const char* new_name = lv_textarea_get_text(ta);
    if (!new_name || strlen(new_name) == 0) return;

    char pid = (s_rename_pkg_idx == 0) ? 'A' : 'B';
    const char* model_id = vehicle_recog_get_model_id(pid);
    if (model_id && strlen(model_id) > 0) {
        bool ok = vehicle_recog_rename_model(pid, model_id, new_name);
        SMNI("PKG[%c] rename '%s' ok=%d", pid, new_name, (int)ok);
    }

    lv_obj_t* scr = lv_scr_act();
    uint32_t child_cnt = lv_obj_get_child_count(scr);
    if (child_cnt > 0) lv_obj_del(lv_obj_get_child(scr, child_cnt - 1));
}

static void pkg_open_rename_dialog(uint8_t pkg_idx) {
    s_rename_pkg_idx = pkg_idx;
    char pid = (pkg_idx == 0) ? 'A' : 'B';
    const char* cur_name = vehicle_recog_get_vehicle_name(pid);

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(modal, 230, LV_PART_MAIN);
    lv_obj_set_style_border_width(modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ta = lv_textarea_create(modal);
    lv_obj_set_size(ta, 300, 44);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 10);
    lv_textarea_set_max_length(ta, 31);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, cur_name ? cur_name : "");
    lv_obj_set_style_text_font(ta, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xF1F5F9), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1E2A2F), LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 6, LV_PART_MAIN);

    lv_obj_t* kb = lv_keyboard_create(modal);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(60));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1A2035), LV_PART_MAIN);

    lv_obj_t* ok_btn = lv_btn_create(modal);
    lv_obj_set_size(ok_btn, 60, 36);
    lv_obj_align_to(ok_btn, ta, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x1D4E3A), LV_PART_MAIN);
    lv_obj_set_style_radius(ok_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(ok_btn, pkg_rename_ok_cb, LV_EVENT_CLICKED, ta);
    lv_obj_t* ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);

    lv_obj_t* cancel_btn = lv_btn_create(modal);
    lv_obj_set_size(cancel_btn, 60, 36);
    lv_obj_align_to(cancel_btn, ok_btn, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x2A1F1F), LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* ev) {
        if (lv_event_get_code(ev) != LV_EVENT_CLICKED) return;
        lv_obj_t* scr = lv_scr_act();
        uint32_t n = lv_obj_get_child_count(scr);
        if (n > 0) lv_obj_del(lv_obj_get_child(scr, n - 1));
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "\xC3\x97");
    lv_obj_center(cancel_lbl);
}

// ============================================================
// PARKING — CALIBRATE DIALOG (B4)
// ============================================================

static uint8_t s_cal_pkg_idx = 0;

static void pkg_calibrate_confirm_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool do_cal = (bool)(uintptr_t)lv_event_get_user_data(e);

    if (do_cal) {
        char pid = (s_cal_pkg_idx == 0) ? 'A' : 'B';
        bool ok = vehicle_recog_calibrate_empty(pid);
        SMNI("PKG[%c] calibrate_empty ok=%d", pid, (int)ok);
    }

    lv_obj_t* scr = lv_scr_act();
    uint32_t n = lv_obj_get_child_count(scr);
    if (n > 0) lv_obj_del(lv_obj_get_child(scr, n - 1));
}

static void pkg_open_calibrate_dialog(uint8_t pkg_idx) {
    s_cal_pkg_idx = pkg_idx;
    char pid = (pkg_idx == 0) ? 'A' : 'B';

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(modal, 230, LV_PART_MAIN);
    lv_obj_set_style_border_width(modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(modal);
    lv_obj_set_size(card, 260, 120);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E2A2F), LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "Kalibriraj prazno mesto %c?", pid);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_font(title, &font_montserrat_16_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF1F5F9), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, 240);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, "Mesto mora biti prazno.");
    lv_obj_set_style_text_font(hint, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 10, 36);

    lv_obj_t* yes_btn = lv_btn_create(card);
    lv_obj_set_size(yes_btn, 100, 36);
    lv_obj_align(yes_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(yes_btn, lv_color_hex(0x1D4E3A), LV_PART_MAIN);
    lv_obj_set_style_radius(yes_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(yes_btn, pkg_calibrate_confirm_cb,
                        LV_EVENT_CLICKED, (void*)(uintptr_t)1);
    lv_obj_t* yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "DA \xe2\x80\x94 Kalibriraj");
    lv_obj_set_style_text_font(yes_lbl, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_center(yes_lbl);

    lv_obj_t* no_btn = lv_btn_create(card);
    lv_obj_set_size(no_btn, 80, 36);
    lv_obj_align(no_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x2A1F1F), LV_PART_MAIN);
    lv_obj_set_style_radius(no_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(no_btn, pkg_calibrate_confirm_cb,
                        LV_EVENT_CLICKED, (void*)(uintptr_t)0);
    lv_obj_t* no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "NE");
    lv_obj_set_style_text_font(no_lbl, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_center(no_lbl);
}

// ============================================================
// PARKING — EVENT CALLBACK (B4: 2s + 10s long press)
// ============================================================

static uint32_t s_pkg_press_ms[2]   = {0, 0};
static bool     s_pkg_edit_fired[2] = {false, false};
static bool     s_pkg_cal_fired[2]  = {false, false};

static void pkg_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    PkgWidget* w = (PkgWidget*)lv_event_get_user_data(e);
    if (!w) return;
    uint8_t i = w->idx;
    char pid = (i == 0) ? 'A' : 'B';

    if (code == LV_EVENT_PRESSED) {
        s_pkg_press_ms[i]   = millis();
        s_pkg_edit_fired[i] = false;
        s_pkg_cal_fired[i]  = false;
    }
    else if (code == LV_EVENT_PRESSING) {
        uint32_t held = millis() - s_pkg_press_ms[i];

        if (held >= 2000 && !s_pkg_edit_fired[i]) {
            vr_place_state_t st = vehicle_recog_get_state(pid);
            if (st == VR_STATE_OCCUPIED_KNOWN || st == VR_STATE_OCCUPIED_UNKNOWN) {
                s_pkg_edit_fired[i] = true;
                SMNI("PKG[%c] 2s hold -> BUTTON_EDIT_VEHICLE", pid);
                EventBus::publish(
                    i == 0 ? EventType::BUTTON_EDIT_VEHICLE_A
                           : EventType::BUTTON_EDIT_VEHICLE_B, 0);
                pkg_open_rename_dialog(i);
            }
        }

        if (held >= VR_CALIB_HOLD_MS && !s_pkg_cal_fired[i]) {
            vr_place_state_t st = vehicle_recog_get_state(pid);
            if (st == VR_STATE_EMPTY_CALIBRATED || st == VR_STATE_EMPTY_UNCALIBRATED) {
                s_pkg_cal_fired[i] = true;
                SMNI("PKG[%c] 10s hold -> calibrate dialog", pid);
                pkg_open_calibrate_dialog(i);
            }
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_pkg_press_ms[i]   = 0;
        s_pkg_edit_fired[i] = false;
        s_pkg_cal_fired[i]  = false;
    }
}

// ============================================================
// PARKING — APPLY (E3.2)
// ============================================================

static void pkg_apply(PkgWidget& w) {
    // vr_state: 0=EMPTY_CAL, 1=EMPTY_UNCAL, 2=OCC_UNKNOWN, 3=OCC_KNOWN
    if (w.data.vr_state == VR_STATE_OCCUPIED_KNOWN) {
        lv_obj_set_style_bg_color(w.card, C_PKG_OCC_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(w.card, C_PKG_OCC_BORDER, LV_PART_MAIN);
        lv_label_set_text(w.lbl_name,
            w.data.vehicle_name[0] ? w.data.vehicle_name : "Avto ?");
        lv_obj_set_style_text_color(w.lbl_name, C_PKG_OCC_NAME, LV_PART_MAIN);
        car_set_color(w.car, C_CAR_OCC);
        char stats[40];
        if (!isnan(w.data.last_dtw)) {
            snprintf(stats, sizeof(stats), "%lu\xC3\x97  DTW %.1f",
                     (unsigned long)w.data.parking_count, (double)w.data.last_dtw);
        } else {
            snprintf(stats, sizeof(stats), "novo");
        }
        lv_label_set_text(w.lbl_stats, stats);
    }
    else if (w.data.vr_state == VR_STATE_OCCUPIED_UNKNOWN) {
        lv_obj_set_style_bg_color(w.card, C_PKG_OCC_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(w.card, lv_color_hex(0x475569), LV_PART_MAIN);
        lv_label_set_text(w.lbl_name, "---");
        lv_obj_set_style_text_color(w.lbl_name, C_CAR_EMPTY, LV_PART_MAIN);
        car_set_color(w.car, lv_color_hex(0x475569));
        lv_label_set_text(w.lbl_stats, "");
    }
    else {
        lv_obj_set_style_bg_color(w.card, C_PKG_EMPTY_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(w.card, C_PKG_EMPTY_BORDER, LV_PART_MAIN);
        lv_label_set_text(w.lbl_name, "Prazno");
        lv_obj_set_style_text_color(w.lbl_name, C_PKG_EMPTY_NAME, LV_PART_MAIN);
        car_set_color(w.car, C_CAR_EMPTY);
        if (w.data.vr_state == VR_STATE_EMPTY_UNCALIBRATED) {
            lv_label_set_text(w.lbl_stats, "! ni kalibr.");
            lv_obj_set_style_text_color(w.lbl_stats,
                lv_color_hex(0xFBBF24), LV_PART_MAIN);
        } else {
            lv_label_set_text(w.lbl_stats, "");
        }
    }

    uint8_t phase = w.data.tof_phase;
    lv_color_t phase_col;
    switch (phase) {
        case 1:  phase_col = C_PHASE_DETECT; break;
        case 2:  phase_col = C_PHASE_SCAN;   break;
        case 3:  phase_col = C_PHASE_DTW;    break;
        default: phase_col = C_PHASE_IDLE;   break;
    }
    if (w.data.tof_active && phase > 0) {
        lv_label_set_text(w.lbl_phase, PHASE_NAMES[phase]);
        lv_obj_set_style_text_color(w.lbl_phase, phase_col, LV_PART_MAIN);
        lv_obj_remove_flag(w.lbl_phase, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_phase, LV_OBJ_FLAG_HIDDEN);
    }

    // H senzor (horizontalni/vhodni) — vrednost v cm
    if (w.data.horiz_mm > 0 && w.data.horiz_mm < 8000) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%u cm", w.data.horiz_mm / 10);
        lv_label_set_text(w.lbl_horiz, buf);
        lv_obj_remove_flag(w.lbl_horiz, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_horiz, LV_OBJ_FLAG_HIDDEN);
    }

    // P1 senzor (stropni, spredaj) — vrednost v cm, zgornja vrstica
    if (w.data.p1_mm > 0 && w.data.p1_mm < 8000) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%u cm", w.data.p1_mm / 10);
        lv_label_set_text(w.lbl_p1, buf);
        lv_obj_remove_flag(w.lbl_p1, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_p1, LV_OBJ_FLAG_HIDDEN);
    }

    // P2 senzor (stropni, zadaj) — vrednost v cm, zgornja vrstica
    if (w.data.p2_mm > 0 && w.data.p2_mm < 8000) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%u cm", w.data.p2_mm / 10);
        lv_label_set_text(w.lbl_p2, buf);
        lv_obj_remove_flag(w.lbl_p2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_p2, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================
// PARKING — KREACIJA
// ============================================================

static void pkg_create(uint8_t idx, lv_obj_t* parent, int x, int y) {
    PkgWidget& w = s_pkg[idx];
    w.idx = idx;
    w.data = {};
    strncpy(w.data.vehicle_name, "Prazno", sizeof(w.data.vehicle_name) - 1);

    w.card = lv_obj_create(parent);
    lv_obj_set_size(w.card, PKG_W, PKG_H);
    lv_obj_set_pos(w.card, x, y);
    lv_obj_set_style_bg_color(w.card, C_PKG_EMPTY_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(w.card, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(w.card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(w.card, C_PKG_EMPTY_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(w.card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(w.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w.card, pkg_event_cb, LV_EVENT_ALL, &w);

    // Leva zgornja oznaka — samo "A" ali "B", font_28, amber
    w.lbl_title = lv_label_create(w.card);
    lv_label_set_text(w.lbl_title, idx == 0 ? "A" : "B");
    lv_obj_set_style_text_color(w.lbl_title, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_title, &font_montserrat_28_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_title, LV_ALIGN_TOP_LEFT, 8, 4);

    // Ime vozila / stanje — y=42 (pod font_28 oznako)
    w.lbl_name = lv_label_create(w.card);
    lv_label_set_text(w.lbl_name, "Prazno");
    lv_obj_set_style_text_color(w.lbl_name, C_PKG_EMPTY_NAME, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_name, 8, 42);
    lv_label_set_long_mode(w.lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(w.lbl_name, PKG_W - 76);

    // Statistike DTW — spodaj levo
    w.lbl_stats = lv_label_create(w.card);
    lv_label_set_text(w.lbl_stats, "");
    lv_obj_set_style_text_color(w.lbl_stats, C_PKG_STATS, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_stats, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_stats, LV_ALIGN_BOTTOM_LEFT, 8, -20);

    // TOF faza (DETECT/SCAN/DTW) — spodaj levo
    w.lbl_phase = lv_label_create(w.card);
    lv_label_set_text(w.lbl_phase, "");
    lv_obj_set_style_text_font(w.lbl_phase, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_phase, C_PHASE_IDLE, LV_PART_MAIN);
    lv_obj_align(w.lbl_phase, LV_ALIGN_BOTTOM_LEFT, 8, -5);
    lv_obj_add_flag(w.lbl_phase, LV_OBJ_FLAG_HIDDEN);

    // P1 senzor (sprednji) — zgornja vrstica levo-sredina, bel, font_16
    w.lbl_p1 = lv_label_create(w.card);
    lv_label_set_text(w.lbl_p1, "");
    lv_obj_set_style_text_font(w.lbl_p1, &font_montserrat_16_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_p1, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_p1, 34, 8);
    lv_obj_set_width(w.lbl_p1, 56);
    lv_obj_set_style_text_align(w.lbl_p1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(w.lbl_p1, LV_OBJ_FLAG_HIDDEN);

    // P2 senzor (zadnji) — zgornja vrstica desno, bel, font_16
    w.lbl_p2 = lv_label_create(w.card);
    lv_label_set_text(w.lbl_p2, "");
    lv_obj_set_style_text_font(w.lbl_p2, &font_montserrat_16_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_p2, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_p2, 91, 8);
    lv_obj_set_width(w.lbl_p2, 60);
    lv_obj_set_style_text_align(w.lbl_p2, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_add_flag(w.lbl_p2, LV_OBJ_FLAG_HIDDEN);

    // H senzor (horizontalni/vhodni) — spodaj desno, font_16 bel (kot P1/P2)
    w.lbl_horiz = lv_label_create(w.card);
    lv_label_set_text(w.lbl_horiz, "");
    lv_obj_set_style_text_font(w.lbl_horiz, &font_montserrat_16_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_horiz, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(w.lbl_horiz, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_add_flag(w.lbl_horiz, LV_OBJ_FLAG_HIDDEN);

    car_create(w, PKG_W - 68, 55, C_CAR_EMPTY);
}

// ============================================================
// TOPBAR — TIMER CALLBACK (1s interval, samo LVGL kontekst)
// ============================================================
// Ura se posodablja vsako sekundo.
// Datum se posodablja samo kadar se dan spremeni (s_topbar_last_mday).
// Pred NTP sinhronizacijo (now <= 2020): prikaže placeholder.
// ─────────────────────────────────────────────────────────────
static const char* s_meseci[12] = {
    "jan","feb","mar","apr","maj","jun",
    "jul","avg","sep","okt","nov","dec"
};

static void topbar_tick_cb(lv_timer_t*) {
    if (!s_topbar_time || !s_topbar_date) return;
    time_t now = time(nullptr);
    if (now <= 1577836800UL) {   // NTP ni sinhroniziran
        lv_label_set_text(s_topbar_time, "--:--:--");
        if (s_topbar_last_mday != 0) {
            s_topbar_last_mday = 0;
            lv_label_set_text(s_topbar_date, "--.---.----");
        }
        return;
    }
    struct tm t;
    localtime_r(&now, &t);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_topbar_time, buf);
    if (t.tm_mday != s_topbar_last_mday) {
        s_topbar_last_mday = t.tm_mday;
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "%02d.%s.%04d",
                 t.tm_mday, s_meseci[t.tm_mon], t.tm_year + 1900);
        lv_label_set_text(s_topbar_date, dbuf);
    }
}

// ============================================================
// E3.3 — TOPBAR KREACIJA
// ============================================================

static void topbar_create(lv_obj_t* parent) {
    s_topbar = lv_obj_create(parent);
    lv_obj_set_size(s_topbar, SCR_W, TOPBAR_H);
    lv_obj_set_pos(s_topbar, 0, 0);
    lv_obj_set_style_bg_color(s_topbar, C_NIGHT_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(s_topbar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_topbar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_topbar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_topbar, LV_OBJ_FLAG_SCROLLABLE);

    // Levo: datum  dd.mes.yyyy  (font_16 — malo večji)
    s_topbar_date = lv_label_create(s_topbar);
    lv_label_set_text(s_topbar_date, "--.---.----");
    lv_obj_set_style_text_font(s_topbar_date, &font_montserrat_16_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_topbar_date, C_NIGHT_TXT, LV_PART_MAIN);
    lv_obj_align(s_topbar_date, LV_ALIGN_LEFT_MID, 6, 0);

    // Center: ura  HH:MM:SS  (font_18, fiksna bela — ne sledi dan/noč)
    s_topbar_time = lv_label_create(s_topbar);
    lv_label_set_text(s_topbar_time, "--:--:--");
    lv_obj_set_style_text_font(s_topbar_time, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_topbar_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_topbar_time, LV_ALIGN_CENTER, 0, 0);

    // Desno: dan/noč ikona + lux  "◑ 42lx" / "☀ 70lx"
    s_topbar_icon = lv_label_create(s_topbar);
    lv_label_set_text(s_topbar_icon, "\xE2\x97\x91 --lx");   // ◑ placeholder
    lv_obj_set_style_text_font(s_topbar_icon, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_topbar_icon, C_NIGHT_TXT, LV_PART_MAIN);
    lv_obj_align(s_topbar_icon, LV_ALIGN_RIGHT_MID, -6, 0);

    // Timer za auto-update datuma in ure (1s interval, LVGL task kontekst)
    lv_timer_create(topbar_tick_cb, 1000, nullptr);
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

void screen_main_create(lv_obj_t* parent) {
    s_parent_scr = parent;
    SMNI("screen_main_create...");
    lv_obj_set_style_bg_color(parent, C_SCREEN_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    topbar_create(parent);

    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 2; col++) {
            uint8_t idx = (uint8_t)(row * 2 + col);
            ssr_create(idx, parent,
                       PAD + col * (SSR_BTN_W + PAD),
                       SSR_Y_START + PAD + row * (SSR_BTN_H + PAD));
        }
    s_countdown_timer = lv_timer_create(ssr_countdown_cb, 1000, nullptr);

    for (int i = 0; i < 2; i++)
        pkg_create((uint8_t)i, parent, PAD + i * (PKG_W + PAD), PKG_Y);

    for (int i = 0; i < 4; i++)
        rad_create((uint8_t)i, parent, PAD + i * (RAD_W + PAD), RAD_Y);

    s_created = true;
    SMNI("screen_main_create OK  SSR=%dx%d  PKG=%dx%d  RAD_ARC=%dpx  peak_arc=%dpx",
         SSR_BTN_W, SSR_BTN_H, PKG_W, PKG_H, RAD_ARC_SZ,
         RAD_ARC_SZ - RAD_ARC_WIDTH * 2);
    SMNI("Layout: TOPBAR=%d SSR_Y=%d PKG_Y=%d RAD_Y=%d  RAD_W=%d",
         TOPBAR_H, SSR_Y_START, PKG_Y, RAD_Y, RAD_W);
}

void screen_main_apply_updates() {
    // Ni potrebno — ui_refresh_cb v hal_display.cpp kliče setterje direktno.
}

void screen_main_set_ssr(uint8_t idx, const SsrDisplayData& data) {
    if (idx >= 4 || !s_created) return;
    s_ssr[idx].data = data;
    // Posodobi label iz config če se je spremenil (web UI nastavitve, ~1s latenca)
    const char* cfg_lbl = config_get().ssr_label[idx];
    if (strcmp(cfg_lbl, lv_label_get_text(s_ssr[idx].lbl_name)) != 0)
        lv_label_set_text(s_ssr[idx].lbl_name, cfg_lbl);
    ssr_style(s_ssr[idx]);
}

void screen_main_set_ssr_label(uint8_t idx, const char* label) {
    if (idx >= 4 || !s_created || !label) return;
    lv_label_set_text(s_ssr[idx].lbl_name, label);
}

void screen_main_set_parking(uint8_t idx, const ParkingDisplayData& data) {
    if (idx >= 2 || !s_created) return;
    s_pkg[idx].data = data;
    pkg_apply(s_pkg[idx]);
}

void screen_main_set_radar(uint8_t idx, const RadarDisplayData& data) {
    // Stara funkcija — ohranjena za API kompatibilnost, ne dela nič (arc4x jo nadomesti)
    (void)idx; (void)data;
}

// ============================================================
// screen_main_set_radar_arc — ARC4x glavna vstopna točka
// ============================================================
//
// Kliče se iz hal_display.cpp ui_refresh_cb vsakih ~1000ms.
//
// Preslikava iz RadarSensorStatus (hal_radar) → RadarArcData:
//   !rs.active                  → CONFIG_ERROR + is_permanent_error=true
//   rs.active && !rs.config_ok  → CONFIG_ERROR + is_permanent_error=false
//   detection=0                 → INACTIVE
//   detection=1 ali 3           → MOVING, dist=moving_dist_cm, energy=moving_energy
//   detection=2                 → STATIONARY, dist=static_dist_cm, energy=static_energy
//
// OPOMBA: peak_duration_ms se nastavi iz NVS unmanned_s per senzor.
//   Vir: RadarArcData ne vsebuje tega polja — hal_display.cpp mora
//   prebrati rs.configured_unmanned_s in ga posredovati sem,
//   ALI pa screen_main_set_radar_arc kliče hal_radar_get_status direktno.
//   Trenutna implementacija: posodobitev peak_duration_ms ob vsaki spremembi
//   stanja (ne vsak klic — samo kadar se vrednost razlikuje od shranjene).
// ─────────────────────────────────────────────────────────────────────────────
void screen_main_set_radar_arc(uint8_t idx, const RadarArcData& data) {
    if (idx >= 4 || !s_created) return;
    RadWidget& w = s_rad[idx];

    // Posodobi peak_duration_ms iz hal_radar (configured_unmanned_s per senzor)
    // Direkten klic hal_radar_get_status je varen v LVGL timer kontekstu
    // (read-only dostop do statusa, zaščiten z internim hal_radar mutex-om)
    {
        const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)idx);
        if (rs.configured_unmanned_s > 0) {
            uint32_t dur = (uint32_t)rs.configured_unmanned_s * 1000u;
            if (dur != w.peak_duration_ms) {
                w.peak_duration_ms = dur;
                SMND("RAD[%d] peak_duration → %lu ms (unmanned_s=%u)",
                     idx, (unsigned long)dur, rs.configured_unmanned_s);
            }
        }
    }

    // Posodobi vizualno stanje
    rad_apply(w, data);
}

// ============================================================
// E3.3 — noč/dan indikator
// ============================================================

void screen_main_set_daynight(bool is_night, float lux) {
    if (!s_created || !s_topbar) return;
    lv_color_t tc = is_night ? C_NIGHT_TXT : C_DAY_TXT;
    lv_obj_set_style_bg_color(s_topbar, is_night ? C_NIGHT_BG : C_DAY_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_topbar_date, tc, LV_PART_MAIN);
    // Ura ostane bela — ne sledi dan/noč barvam
    lv_obj_set_style_text_color(s_topbar_icon, tc, LV_PART_MAIN);
    // ◑ (U+25D1) = noč,  ☀ (U+2600) = dan
    const char* icon = is_night ? "\xE2\x97\x91" : "\xE2\x98\x80";
    char buf[24];
    if (lux < 0.1f) {
        snprintf(buf, sizeof(buf), "%s --lx", icon);
    } else {
        snprintf(buf, sizeof(buf), "%s %.0flx", icon, (double)lux);
    }
    lv_label_set_text(s_topbar_icon, buf);
}

lv_obj_t* screen_main_get_screen() {
    return s_parent_scr;
}
