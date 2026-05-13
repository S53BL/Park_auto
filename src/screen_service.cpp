// ============================================================
// screen_service.cpp — Servisni zaslon (LCD UI)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// VSEBINA:
//   Read-only diagnostični zaslon, scroll view, 5 sekcij.
//   Vrednosti so "--" / 0 dokler ni zunanjega hardwarea.
//   screen_service_apply_updates() kliče hal_display interno
//   prek s_pending snapshot — enako kot screen_main.
//
// SEKCIJE:
//   1. Stat boxci  — lux, noč/dan, uptime
//   2. Signali     — fotocelici, rampa, vrata
//   3. TOF         — 6 senzorjev, razdalje v mm
//   4. I2C health  — 5 čipov, zelena/rdeča
//   5. Sistem      — SRAM, PSRAM, Core 0/1 %, FPS, IP
//
// BARVE:
//   Ozadje zaslona : 0x0D0D0D  (enako kot glavni zaslon)
//   Sekcija header : 0x1A1A1A
//   Vrednosti OK   : 0x4CAF50 (zelena)
//   Vrednosti WARN : 0xFF9800 (oranžna)
//   Vrednosti ERR  : 0xF44336 (rdeča)
//   Besedilo dim   : 0x666666
//   Besedilo norm  : 0xCCCCCC
//
// ============================================================

#include "screen_service.h"
#include <freertos/semphr.h>
#include "hal_gpio.h"
#include "hal_radar.h"
#include "hal_tof.h"
#include "config_mgr.h"

// ============================================================
// LOGGING
// ============================================================

#include "logger.h"
#define SSVC_I(fmt, ...) LOG_INFO ("SSVC", fmt, ##__VA_ARGS__)
#define SSVC_W(fmt, ...) LOG_WARN ("SSVC", fmt, ##__VA_ARGS__)

// ============================================================
// BARVE — posodobljeno 2026-04 (Tailwind-inspired dark theme)
// ============================================================

#define C_BG            lv_color_hex(0x111827)
#define C_SECTION_BG    lv_color_hex(0x1A2332)   // temnejši od kartic
#define C_CARD_BG       lv_color_hex(0x1F2937)
#define C_BORDER        lv_color_hex(0x334155)
#define C_OK            lv_color_hex(0x34D399)
#define C_WARN          lv_color_hex(0xFBBF24)
#define C_ERR           lv_color_hex(0xEF4444)
#define C_TEXT          lv_color_hex(0xF1F5F9)
#define C_TEXT_DIM      lv_color_hex(0xCBD5E1)
#define C_TEXT_BRIGHT   lv_color_hex(0xF8FAFC)
#define C_HEADER_TEXT   lv_color_hex(0x94A3B8)

// ============================================================
// DIMENZIJE
// ============================================================

#define SCR_W           320
#define PAD             8
#define SECTION_W       (SCR_W - PAD * 2)
#define STAT_BOX_W      ((SECTION_W - PAD * 2) / 3)
#define STAT_BOX_H      52
#define ROW_H           28
#define CHIP_DOT_SIZE   10

// ============================================================
// SEKCIJA 1 — STAT BOXCI
// ============================================================

struct StatBox {
    lv_obj_t* card;
    lv_obj_t* lbl_val;
    lv_obj_t* lbl_unit;
};

static StatBox s_stat[3];   // 0=lux, 1=noč/dan, 2=uptime

static void stat_box_create(uint8_t idx, lv_obj_t* parent, int x, int y,
                             const char* unit_text) {
    StatBox& b = s_stat[idx];

    b.card = lv_obj_create(parent);
    lv_obj_set_size(b.card, STAT_BOX_W, STAT_BOX_H);
    lv_obj_set_pos(b.card, x, y);
    lv_obj_set_style_bg_color(b.card, C_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(b.card, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b.card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(b.card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b.card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b.card, LV_OBJ_FLAG_SCROLLABLE);

    b.lbl_val = lv_label_create(b.card);
    lv_label_set_text(b.lbl_val, "--");
    lv_obj_set_style_text_color(b.lbl_val, C_TEXT_BRIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(b.lbl_val, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(b.lbl_val, LV_ALIGN_CENTER, 0, -6);

    b.lbl_unit = lv_label_create(b.card);
    lv_label_set_text(b.lbl_unit, unit_text);
    lv_obj_set_style_text_color(b.lbl_unit, C_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(b.lbl_unit, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(b.lbl_unit, LV_ALIGN_BOTTOM_MID, 0, -4);
}

// ============================================================
// SEKCIJA 2 — SIGNALI (fotocelice, rampa, vrata)
// ============================================================

struct SignalRow {
    lv_obj_t* lbl_name;
    lv_obj_t* lbl_val;
};

static SignalRow s_sig[4];  // 0=celica1, 1=celica2, 2=rampa, 3=vrata

static const char* SIG_NAMES[4] = {
    "Celica 1 (zunanja)",
    "Celica 2 (notranja)",
    "Rampa",
    "Drsna vrata"
};

static const char* SIG_DEFAULT[4] = {
    "OK", "OK", "Dol", "Zaprta"
};

// ============================================================
// SEKCIJA 3 — TOF SENZORJI
// ============================================================

struct TofRow {
    lv_obj_t* lbl_name;
    lv_obj_t* lbl_val;
};

static TofRow s_tof[6];

static const char* TOF_NAMES[6] = {
    "H_A  (horiz. A)",
    "P1_A (strop A, 4m)",
    "P2_A (strop A, 2m)",
    "H_B  (horiz. B)",
    "P1_B (strop B, 4m)",
    "P2_B (strop B, 2m)"
};

// ============================================================
// SEKCIJA 4 — I2C HEALTH
// ============================================================

struct ChipRow {
    lv_obj_t* dot;
    lv_obj_t* lbl_name;
    lv_obj_t* lbl_status;
};

static ChipRow s_chip[5];

static const char* CHIP_NAMES[5] = {
    "TCA9548A  0x70",
    "MCP23017  0x20",
    "SC16IS752 0x48",
    "SC16IS752 0x4C",
    "BH1750    0x23"
};

// ============================================================
// SEKCIJA 5 — SISTEMSKI PODATKI
// ============================================================

struct SysRow {
    lv_obj_t* lbl_name;
    lv_obj_t* lbl_val;
};

static SysRow s_sys[7];

static const char* SYS_NAMES[7] = {
    "Free SRAM",
    "Free PSRAM",
    "Core 0",
    "Core 1",
    "FPS",
    "Uptime",
    "Cfg zamenjani"
};

// ============================================================
// STANJE
// ============================================================

static bool s_created = false;

// Thread-safe pending data
static SemaphoreHandle_t s_mutex   = nullptr;
static bool   s_lux_dirty          = false;
static float  s_lux_val            = 0.0f;
static bool   s_is_night           = false;
static bool   s_tof_dirty          = false;
static uint16_t s_tof_mm[6]        = {0};
static bool   s_sys_dirty          = false;
static uint32_t s_free_heap        = 0;
static uint32_t s_free_psram       = 0;
static uint8_t  s_core0_pct        = 0;
static uint8_t  s_core1_pct        = 0;
static uint32_t s_uptime_s         = 0;
static bool   s_i2c_dirty          = false;
static I2cHealthData s_i2c_health  = { false, false, false, false, false };

// ============================================================
// POMOŽNE — SEKCIJA HEADER
// ============================================================

static lv_obj_t* make_section_header(lv_obj_t* parent, int y, const char* title) {
    lv_obj_t* hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, SECTION_W, 22);
    lv_obj_set_pos(hdr, PAD, y);
    lv_obj_set_style_bg_color(hdr, C_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(hdr);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, C_HEADER_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);

    return hdr;
}

static lv_obj_t* make_row(lv_obj_t* parent, int y,
                           lv_obj_t** lbl_name_out, lv_obj_t** lbl_val_out,
                           const char* name, const char* val_init,
                           lv_color_t val_color) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, SECTION_W, ROW_H);
    lv_obj_set_pos(row, PAD, y);
    lv_obj_set_style_bg_color(row, C_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ln = lv_label_create(row);
    lv_label_set_text(ln, name);
    lv_obj_set_style_text_color(ln, C_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(ln, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ln, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t* lv2 = lv_label_create(row);
    lv_label_set_text(lv2, val_init);
    lv_obj_set_style_text_color(lv2, val_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv2, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lv2, LV_ALIGN_RIGHT_MID, -6, 0);

    if (lbl_name_out) *lbl_name_out = ln;
    if (lbl_val_out)  *lbl_val_out  = lv2;
    return row;
}

// ============================================================
// APPLY FUNCTIONS — kliče se samo iz lvglTask
// ============================================================

static void apply_lux() {
    // Stat box 0 — lux vrednost
    char buf[12];
    if (s_lux_val < 0.1f) {
        lv_label_set_text(s_stat[0].lbl_val, "--");
    } else {
        snprintf(buf, sizeof(buf), "%.0f", s_lux_val);
        lv_label_set_text(s_stat[0].lbl_val, buf);
    }

    // Stat box 1 — noč/dan
    lv_label_set_text(s_stat[1].lbl_val, s_is_night ? "NOC" : "DAN");
    lv_obj_set_style_text_color(s_stat[1].lbl_val,
                                 s_is_night ? C_WARN : C_OK, LV_PART_MAIN);

    // Signal row 0/1 — fotocelici stanja iz lux/night niso direktno dostopni tukaj,
    // posodobijo se samo prek set_signal() ki ga bo klical hal_gpio callback
    // (implementiran ko pride hardware). Za zdaj ne osvežujemo.
}

static void apply_tof() {
    // E3.4: preberi diagnostiko direktno za error_count
    TofDiagnostics tofDiag = hal_tof_getDiagnostics();

    char buf[20];
    for (int i = 0; i < 6; i++) {
        if (s_tof_mm[i] == 0) {
            if (tofDiag.error_count[i] > 0) {
                snprintf(buf, sizeof(buf), "ERR %u", tofDiag.error_count[i]);
                lv_label_set_text(s_tof[i].lbl_val, buf);
                lv_obj_set_style_text_color(s_tof[i].lbl_val, C_ERR, LV_PART_MAIN);
            } else {
                lv_label_set_text(s_tof[i].lbl_val, "--");
                lv_obj_set_style_text_color(s_tof[i].lbl_val, C_TEXT_DIM, LV_PART_MAIN);
            }
        } else {
            if (tofDiag.error_count[i] > 0) {
                // Razdalja + napake skupaj
                snprintf(buf, sizeof(buf), "%u mm (%ue)", s_tof_mm[i], tofDiag.error_count[i]);
                lv_obj_set_style_text_color(s_tof[i].lbl_val, C_WARN, LV_PART_MAIN);
            } else {
                snprintf(buf, sizeof(buf), "%u mm", s_tof_mm[i]);
                lv_obj_set_style_text_color(s_tof[i].lbl_val, C_TEXT, LV_PART_MAIN);
            }
            lv_label_set_text(s_tof[i].lbl_val, buf);
        }
    }
}

static void apply_sys() {
    char buf[20];

    // SRAM
    if (s_free_heap == 0) {
        lv_label_set_text(s_sys[0].lbl_val, "--");
    } else {
        snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(s_free_heap / 1024));
        lv_label_set_text(s_sys[0].lbl_val, buf);
        lv_obj_set_style_text_color(s_sys[0].lbl_val,
                                     s_free_heap < 30000 ? C_WARN : C_TEXT, LV_PART_MAIN);
    }

    // PSRAM
    if (s_free_psram == 0) {
        lv_label_set_text(s_sys[1].lbl_val, "--");
    } else {
        snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(s_free_psram / 1024));
        lv_label_set_text(s_sys[1].lbl_val, buf);
    }

    // Core 0
    snprintf(buf, sizeof(buf), "%d %%", s_core0_pct);
    lv_label_set_text(s_sys[2].lbl_val, buf);
    lv_obj_set_style_text_color(s_sys[2].lbl_val,
                                 s_core0_pct > 85 ? C_ERR
                               : s_core0_pct > 70 ? C_WARN : C_TEXT, LV_PART_MAIN);

    // Core 1
    snprintf(buf, sizeof(buf), "%d %%", s_core1_pct);
    lv_label_set_text(s_sys[3].lbl_val, buf);
    lv_obj_set_style_text_color(s_sys[3].lbl_val,
                                 s_core1_pct > 85 ? C_ERR
                               : s_core1_pct > 70 ? C_WARN : C_TEXT, LV_PART_MAIN);

    // FPS — direktno pobrano, ni v pending (minor)
    uint32_t fps = hal_display_getFps();
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)fps);
    lv_label_set_text(s_sys[4].lbl_val, buf);
    lv_obj_set_style_text_color(s_sys[4].lbl_val,
                                 fps < 10 ? C_WARN : C_TEXT, LV_PART_MAIN);

    // Uptime
    uint32_t up = s_uptime_s;
    uint32_t d  = up / 86400; up %= 86400;
    uint32_t h  = up / 3600;  up %= 3600;
    uint32_t m  = up / 60;
    uint32_t s2 = up % 60;
    if (d > 0)
        snprintf(buf, sizeof(buf), "%lud %02lu:%02lu", (unsigned long)d,
                 (unsigned long)h, (unsigned long)m);
    else
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s2);
    lv_label_set_text(s_sys[5].lbl_val, buf);
}

static void apply_i2c() {
    // Preberi Wire1 napake iz hal_gpio (E3.4)
    uint32_t wire1_errs = hal_gpio_get_wire1_errors();

    bool ok_arr[5] = {
        s_i2c_health.tca9548a_ok,
        s_i2c_health.mcp23017_ok,
        s_i2c_health.sc16is752_1_ok,
        s_i2c_health.sc16is752_2_ok,
        s_i2c_health.bh1750_ok
    };
    for (int i = 0; i < 5; i++) {
        lv_color_t col = ok_arr[i] ? C_OK : C_ERR;
        lv_obj_set_style_bg_color(s_chip[i].dot, col, LV_PART_MAIN);

        // E3.4: Wire1 napake prikazane ob statusu
        if (i == 1 && wire1_errs > 0) {
            // MCP23017 — pokaži napake
            char buf[20];
            snprintf(buf, sizeof(buf), "ERR %lu", (unsigned long)wire1_errs);
            lv_label_set_text(s_chip[i].lbl_status, buf);
            lv_obj_set_style_text_color(s_chip[i].lbl_status, C_WARN, LV_PART_MAIN);
        } else {
            lv_label_set_text(s_chip[i].lbl_status, ok_arr[i] ? "OK" : "ERR");
            lv_obj_set_style_text_color(s_chip[i].lbl_status, col, LV_PART_MAIN);
        }
    }
}

static void apply_radar_to_log() {
    // Logira radar stanje vsakih 10 minut za diagnostiko
    static uint32_t s_last_radar_log_ms = 0;
    uint32_t now = millis();
    if (now - s_last_radar_log_ms < 600000UL) return;  // 10 min
    s_last_radar_log_ms = now;

    for (uint8_t i = 0; i < 4; i++) {
        const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)i);
        LOG_INFO("SSVC", "Radar[%d]: active=%d ok=%d frames=%lu errs=%lu det=%d e=%d/%d",
                 i, (int)rs.active, (int)rs.config_ok,
                 (unsigned long)rs.frames_ok,
                 (unsigned long)(rs.parse_errors + rs.i2c_errors),
                 (int)rs.last_frame.detection,
                 (int)rs.last_frame.moving_energy,
                 (int)rs.last_frame.static_energy);
    }
}

// Live uptime ticker — kliče se iz LVGL timer, samo Core1
static void uptime_tick_cb(lv_timer_t*) {
    if (!s_created) return;

    // Uptime
    uint32_t up = millis() / 1000;
    uint32_t d  = up / 86400; up %= 86400;
    uint32_t h  = up / 3600;  up %= 3600;
    uint32_t m  = up / 60;
    uint32_t s2 = up % 60;
    char buf[20];
    if (d > 0)
        snprintf(buf, sizeof(buf), "%lud %02lu:%02lu",
                 (unsigned long)d, (unsigned long)h, (unsigned long)m);
    else
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s2);
    lv_label_set_text(s_sys[5].lbl_val, buf);

    // FPS
    uint32_t fps = hal_display_getFps();
    char fpsbuf[8];
    snprintf(fpsbuf, sizeof(fpsbuf), "%lu", (unsigned long)fps);
    lv_label_set_text(s_sys[4].lbl_val, fpsbuf);
    lv_obj_set_style_text_color(s_sys[4].lbl_val,
                                 fps < 10 ? C_WARN : C_TEXT, LV_PART_MAIN);

    // SRAM
    uint32_t fh = ESP.getFreeHeap();
    char sram_buf[16];
    snprintf(sram_buf, sizeof(sram_buf), "%lu KB", (unsigned long)(fh / 1024));
    lv_label_set_text(s_sys[0].lbl_val, sram_buf);
    lv_obj_set_style_text_color(s_sys[0].lbl_val,
                                 fh < 30000 ? C_WARN : C_TEXT, LV_PART_MAIN);

    // PSRAM
    uint32_t fp = ESP.getFreePsram();
    char psram_buf[16];
    snprintf(psram_buf, sizeof(psram_buf), "%lu KB", (unsigned long)(fp / 1024));
    lv_label_set_text(s_sys[1].lbl_val, psram_buf);

    // E3.4 — config replaced count (vrstica 6 v SYS_NAMES)
    if (config_mgr_ok()) {
        uint8_t replaced = (uint8_t)config_mgr_replaced_count();
        char rep_buf[8];
        snprintf(rep_buf, sizeof(rep_buf), "%d", replaced);
        if (replaced > 0) {
            lv_label_set_text(s_sys[6].lbl_val, rep_buf);
            lv_obj_set_style_text_color(s_sys[6].lbl_val, C_WARN, LV_PART_MAIN);
        } else {
            lv_label_set_text(s_sys[6].lbl_val, "0");
            lv_obj_set_style_text_color(s_sys[6].lbl_val, C_TEXT, LV_PART_MAIN);
        }
    }

    // E3.4 — periodičen radar log
    apply_radar_to_log();

    // E3.4 — I2C health refresh vsakih 2s (Wire1 napake)
    uint32_t w1errs = hal_gpio_get_wire1_errors();
    if (w1errs > 0) {
        lv_obj_set_style_bg_color(s_chip[1].dot, C_WARN, LV_PART_MAIN);
        char w1buf[16];
        snprintf(w1buf, sizeof(w1buf), "WARN %lu", (unsigned long)w1errs);
        lv_label_set_text(s_chip[1].lbl_status, w1buf);
        lv_obj_set_style_text_color(s_chip[1].lbl_status, C_WARN, LV_PART_MAIN);
    }
}

// ============================================================
// screen_service_create — kliče samo lvglTask
// ============================================================

void screen_service_create(lv_obj_t* parent) {
    SSVC_I("screen_service_create...");

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) { SSVC_W("Mutex napaka!"); }
    }

    lv_obj_set_style_bg_color(parent, C_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_ACTIVE);

    int cy = PAD;    // current y — sledimo kam smo

    // --------------------------------------------------------
    // SEKCIJA 1 — STAT BOXCI (lux, noč/dan, uptime)
    // --------------------------------------------------------
    make_section_header(parent, cy, "  SVETLOBA  &  CAS");
    cy += 26;

    static const char* stat_units[3] = { "lux", "rezim", "uptime" };
    for (int i = 0; i < 3; i++) {
        int x = PAD + i * (STAT_BOX_W + PAD);
        stat_box_create(i, parent, x, cy, stat_units[i]);
    }
    // Stat box 2 = uptime — samo label, brez vrednosti (timer jo osvežuje)
    lv_label_set_text(s_stat[2].lbl_val, "00:00:00");
    cy += STAT_BOX_H + PAD;

    // --------------------------------------------------------
    // SEKCIJA 2 — SIGNALI
    // --------------------------------------------------------
    make_section_header(parent, cy, "  SIGNALI");
    cy += 26;

    static const lv_color_t sig_colors[4] = { C_OK, C_OK, C_TEXT, C_TEXT };
    for (int i = 0; i < 4; i++) {
        make_row(parent, cy,
                 &s_sig[i].lbl_name, &s_sig[i].lbl_val,
                 SIG_NAMES[i], SIG_DEFAULT[i], sig_colors[i]);
        cy += ROW_H;
    }
    cy += PAD;

    // --------------------------------------------------------
    // SEKCIJA 3 — TOF SENZORJI
    // --------------------------------------------------------
    make_section_header(parent, cy, "  TOF SENZORJI  (6x)");
    cy += 26;

    for (int i = 0; i < 6; i++) {
        make_row(parent, cy,
                 &s_tof[i].lbl_name, &s_tof[i].lbl_val,
                 TOF_NAMES[i], "--", C_TEXT_DIM);
        cy += ROW_H;
    }
    cy += PAD;

    // --------------------------------------------------------
    // SEKCIJA 4 — I2C HEALTH
    // --------------------------------------------------------
    make_section_header(parent, cy, "  I2C HEALTH");
    cy += 26;

    for (int i = 0; i < 5; i++) {
        // Container row
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, SECTION_W, ROW_H);
        lv_obj_set_pos(row, PAD, cy);
        lv_obj_set_style_bg_color(row, C_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, C_BORDER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // LED dot
        s_chip[i].dot = lv_obj_create(row);
        lv_obj_set_size(s_chip[i].dot, CHIP_DOT_SIZE, CHIP_DOT_SIZE);
        lv_obj_align(s_chip[i].dot, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_bg_color(s_chip[i].dot, C_ERR, LV_PART_MAIN);
        lv_obj_set_style_radius(s_chip[i].dot, CHIP_DOT_SIZE / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_chip[i].dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_chip[i].dot, LV_OBJ_FLAG_SCROLLABLE);

        // Chip name
        s_chip[i].lbl_name = lv_label_create(row);
        lv_label_set_text(s_chip[i].lbl_name, CHIP_NAMES[i]);
        lv_obj_set_style_text_color(s_chip[i].lbl_name, C_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_chip[i].lbl_name, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_chip[i].lbl_name, LV_ALIGN_LEFT_MID, 6 + CHIP_DOT_SIZE + 6, 0);

        // Status text
        s_chip[i].lbl_status = lv_label_create(row);
        lv_label_set_text(s_chip[i].lbl_status, "N/A");
        lv_obj_set_style_text_color(s_chip[i].lbl_status, C_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_chip[i].lbl_status, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_chip[i].lbl_status, LV_ALIGN_RIGHT_MID, -6, 0);

        cy += ROW_H;
    }
    cy += PAD;

    // --------------------------------------------------------
    // SEKCIJA 5 — SISTEMSKI PODATKI
    // --------------------------------------------------------
    make_section_header(parent, cy, "  SISTEM");
    cy += 26;

    for (int i = 0; i < 6; i++) {
        make_row(parent, cy,
                 &s_sys[i].lbl_name, &s_sys[i].lbl_val,
                 SYS_NAMES[i], "--", C_TEXT);
        cy += ROW_H;
    }
    make_row(parent, cy,
             &s_sys[6].lbl_name, &s_sys[6].lbl_val,
             SYS_NAMES[6], "--", C_TEXT);
    cy += ROW_H;
    cy += PAD * 2;  // spodnji padding

    // Fiksiramo scroll vsebino
    lv_obj_set_height(parent, LV_SIZE_CONTENT);

    // Timer za live uptime + FPS + SRAM (vsakih 2s, samo Core1)
    lv_timer_create(uptime_tick_cb, 2000, nullptr);

    s_created = true;
    SSVC_I("screen_service_create OK (cy=%d)", cy);
}

// ============================================================
// screen_service_apply_updates — kliče samo lvglTask
// ============================================================

void screen_service_apply_updates() {
    if (!s_created || !s_mutex) return;

    bool do_lux = false, do_tof = false, do_sys = false, do_i2c = false;

    // Snapshot pod mutex
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        do_lux = s_lux_dirty;  s_lux_dirty  = false;
        do_tof = s_tof_dirty;  s_tof_dirty  = false;
        do_sys = s_sys_dirty;  s_sys_dirty  = false;
        do_i2c = s_i2c_dirty;  s_i2c_dirty  = false;
        xSemaphoreGive(s_mutex);
    } else {
        return;  // preskoči ta cikel
    }

    if (do_lux) apply_lux();
    if (do_tof) apply_tof();
    if (do_sys) apply_sys();
    if (do_i2c) apply_i2c();
}

// ============================================================
// THREAD-SAFE SETTERJI — kličejo HAL moduli iz sensorTask
// ============================================================

// Lux in noč/dan — pokliče hal_display_updateLux() ki nato pokliče screen_service_apply_updates()
// prek hal_display pending sistema. Tukaj samo shranjujemo lokalno kopijo.
// hal_display.cpp pokliče screen_service_apply_updates() iz apply_pending() v lvglTask.
// Da se to zgodi, mora hal_display.cpp imeti lux_dirty flag — kar že ima.
// Tukaj samo sinhronizirano preberemo vrednost ki jo hal_display že ima.
//
// IMPLEMENTACIJSKA OPOMBA:
// hal_display.cpp kliče screen_service_apply_updates() ko je lux_dirty.
// screen_service_apply_updates() bere s_lux_val/s_is_night — ki morata biti
// posodobljeni PREDEN apply_pending() pokliče screen_service_apply_updates().
// Zato hal_display_updateLux() pokliče tudi spodnji setter neposredno.

void screen_service_update_lux(float lux, bool night) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_lux_val  = lux;
        s_is_night = night;
        s_lux_dirty = true;
        xSemaphoreGive(s_mutex);
    }
}

void screen_service_update_tof(const uint16_t mm[6]) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(s_tof_mm, mm, 6 * sizeof(uint16_t));
        s_tof_dirty = true;
        xSemaphoreGive(s_mutex);
    }
}

void screen_service_update_sys(uint32_t fh, uint32_t fp,
                                uint8_t c0, uint8_t c1, uint32_t up) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_free_heap  = fh;
        s_free_psram = fp;
        s_core0_pct  = c0;
        s_core1_pct  = c1;
        s_uptime_s   = up;
        s_sys_dirty  = true;
        xSemaphoreGive(s_mutex);
    }
}

void screen_service_set_i2c_health(const I2cHealthData& data) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_i2c_health = data;
        s_i2c_dirty  = true;
        xSemaphoreGive(s_mutex);
    }
}

// ============================================================
// SIGNAL POSODOBITEV — kliče hal_gpio callback (ko pride hardware)
// ============================================================

void screen_service_set_signal(uint8_t idx, const char* text, bool ok) {
    // Kliče se iz sensorTask prek EventBus callback — NI thread-safe za LVGL!
    // Implementiraj prek pending mutex ko bo hardware priključen.
    // Za zdaj: direkten LVGL klic je varen SAMO če smo v lvglTask.
    // TODO: dodaj v pending sistem ko pride hardware (enako kot lux/tof).
    if (!s_created || idx >= 4) return;
    lv_label_set_text(s_sig[idx].lbl_val, text);
    lv_obj_set_style_text_color(s_sig[idx].lbl_val,
                                 ok ? C_OK : C_ERR, LV_PART_MAIN);
}
