// ============================================================
// screen_main.cpp — Glavni zaslon
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.1-dev  |  Datum: 2026-04
// Faza    : 0 — polna implementacija UI
//
// POPRAVKI v2.0.1:
//   - lv_timer_create za hold_prog_timer: dodan ekspliciten
//     lv_timer_set_repeat_count(..., -1) — neskončno ponavljanje
//     (LVGL 9.x privzeto je prav tako neskončno, ampak ekspliciten
//     klic odpravi morebitne nejasnosti pri različnih verzijah)
//   - screen_main.h zdaj vključuje hal_display.h ki vključuje <lvgl.h>,
//     ni potrebno posebej vključiti <lvgl.h> v tej datoteki
// ============================================================

#include "screen_main.h"
#include "event_bus.h"

// SLOVENSKI FONTI (Montserrat + ČŠŽ) so v src/fonts/.
// PlatformIO jih prevede samodejno kot ločene .c enote.
// Extern deklaracije so v LV_FONT_CUSTOM_DECLARE v lv_conf.h.
// ============================================================

// ============================================================
// LOGGING
// ============================================================
#include "logger.h"
#define SMNI(fmt, ...) LOG_INFO ("SMAIN", fmt, ##__VA_ARGS__)
#define SMND(fmt, ...) LOG_DEBUG("SMAIN", fmt, ##__VA_ARGS__)

// ============================================================
// BARVE — posodobljeno 2026-04 (Tailwind-inspired dark theme)
// ============================================================
#define C_OFF_BG        lv_color_hex(0x1E2A2F)
#define C_OFF_TXT       lv_color_hex(0x91FF66)
#define C_ON_BG         lv_color_hex(0x27241F)
#define C_ON_TXT        lv_color_hex(0xFFD466)
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

// Prej inline — zdaj makroti
#define C_SSR_BORDER        lv_color_hex(0x334155)
#define C_HOLD_BAR_BG       lv_color_hex(0x1F2937)
#define C_PKG_OCC_BORDER    lv_color_hex(0xFBBF24)
#define C_PKG_OCC_NAME      lv_color_hex(0xF1F5F9)
#define C_PKG_EMPTY_BORDER  lv_color_hex(0x334155)
#define C_PKG_EMPTY_NAME    lv_color_hex(0xCBD5E1)
#define C_PKG_TITLE         lv_color_hex(0x94A3B8)
#define C_PKG_STATS         lv_color_hex(0xCBD5E1)
#define C_RAD_ARC_BG        lv_color_hex(0x1F2937)
#define C_RAD_LABEL         lv_color_hex(0xCBD5E1)

// ============================================================
// DIMENZIJE
// ============================================================
#define SCR_W  320
#define PAD    6

#define SSR_BTN_W   ((SCR_W - PAD * 3) / 2)
#define SSR_BTN_H   112
#define SSR_SEC_H   (SSR_BTN_H * 2 + PAD * 3)
#define SSR_BAR_H   4
#define HOLD_MS     1200

#define PKG_Y       (SSR_SEC_H + PAD)
#define PKG_W       ((SCR_W - PAD * 3) / 2)
#define PKG_H       100
#define PKG_SEC_H   (PKG_H + PAD * 2)

#define RAD_Y       (PKG_Y + PKG_SEC_H)
#define RAD_W       ((SCR_W - PAD * 5) / 4)
#define RAD_ARC_SZ  62

// ============================================================
// WIDGET STRUKTURE
// ============================================================

struct SsrWidget {
    lv_obj_t*      btn;
    lv_obj_t*      lbl_name;
    lv_obj_t*      lbl_countdown;
    lv_obj_t*      lbl_status;
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
    CarIcon            car;
    uint8_t            idx;
    ParkingDisplayData data;
};

struct RadWidget {
    lv_obj_t*        arc;
    lv_obj_t*        lbl_pct;
    lv_obj_t*        lbl_name;
    uint8_t          idx;
    RadarDisplayData data;
};

// ============================================================
// STATIČNI WIDGETI
// ============================================================

static SsrWidget   s_ssr[4]            = {};
static PkgWidget   s_pkg[2]            = {};
static RadWidget   s_rad[4]            = {};
static lv_timer_t* s_countdown_timer   = nullptr;
static bool        s_created           = false;

static const char* SSR_NAMES[4]   = { "Glavna\nveriga", "Glavna\ndodatno", "Pred\nlopo", "Pred\ngaražo" };
static const char* RADAR_NAMES[4] = { "Vhod", "Cesta L", "Cesta D", "Garaža" };

// ============================================================
// POMOŽNE
// ============================================================

static void fmt_cd(char* buf, size_t len, uint32_t sec) {
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

// ============================================================
// SSR — STILIZACIJA
// ============================================================

static void ssr_style(SsrWidget& w) {
    lv_color_t bg, tc;
    const char* st;
    switch (w.data.state) {
        case DisplaySsrState::ON:
            bg = C_ON_BG; tc = C_ON_TXT;
            st = w.data.is_manual ? "Ročno · +30 min" : "Vključeno";
            break;
        case DisplaySsrState::SSR_DISABLED:
            bg = C_DIS_BG; tc = C_DIS_TXT;
            st = "Onemogočeno";
            break;
        default:
            bg = C_OFF_BG; tc = C_OFF_TXT;
            st = "Izključeno";
            break;
    }
    lv_obj_set_style_bg_color(w.btn, bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_name,      tc, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_countdown, tc, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.lbl_status,    tc, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.bar_hold, tc, LV_PART_INDICATOR);

    if (w.data.state == DisplaySsrState::ON && w.data.countdown_s > 0) {
        char buf[8]; fmt_cd(buf, sizeof(buf), w.data.countdown_s);
        lv_label_set_text(w.lbl_countdown, buf);
        lv_obj_remove_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(w.lbl_status, st);

    if (w.data.state == DisplaySsrState::SSR_DISABLED)
        lv_obj_remove_flag(w.lbl_lock, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(w.lbl_lock, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// SSR — TIMER CALLBACKI
// ============================================================

static void ssr_hold_fire_cb(lv_timer_t* t) {
    // lv_timer_get_user_data() — obstaja v LVGL 9.2.2 (lv_timer.h)
    SsrWidget* w = (SsrWidget*)lv_timer_get_user_data(t);
    if (!w) return;
    if (w->hold_prog_timer) {
        lv_timer_delete(w->hold_prog_timer);
        w->hold_prog_timer = nullptr;
    }
    w->hold_fire_timer = nullptr;  // LVGL sam izbriše enkraten timer po klicu
    lv_bar_set_value(w->bar_hold, 0, LV_ANIM_OFF);
    lv_obj_add_flag(w->bar_hold, LV_OBJ_FLAG_HIDDEN);
    SMND("SSR[%d] HOLD → BUTTON_SSR_DISABLE payload=%d", w->idx, w->idx);
    EventBus::publish(EventType::BUTTON_SSR_DISABLE, w->idx);
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

    if (code == LV_EVENT_PRESSED) {
        if (w->data.state == DisplaySsrState::SSR_DISABLED) return;
        w->press_ms = millis();
        ssr_cancel_hold(*w);

        // Enkraten timer — sproži BUTTON_SSR_DISABLE po HOLD_MS
        w->hold_fire_timer = lv_timer_create(ssr_hold_fire_cb, HOLD_MS, w);
        lv_timer_set_repeat_count(w->hold_fire_timer, 1);

        // Neskončen timer — osvežuje progress bar vsako 50ms
        w->hold_prog_timer = lv_timer_create(ssr_hold_prog_cb, 50, w);
        lv_timer_set_repeat_count(w->hold_prog_timer, -1);  // eksplicitno neskončno

    } else if (code == LV_EVENT_RELEASED) {
        bool was_short = (w->hold_fire_timer != nullptr);
        ssr_cancel_hold(*w);
        if (was_short && w->data.state != DisplaySsrState::SSR_DISABLED) {
            SMND("SSR[%d] SHORT TAP → BUTTON_SSR payload=%d", w->idx, w->idx);
            EventBus::publish(EventType::BUTTON_SSR, w->idx);
        }

    } else if (code == LV_EVENT_PRESS_LOST) {
        ssr_cancel_hold(*w);
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

    w.lbl_name = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_name, SSR_NAMES[idx]);
    lv_obj_set_style_text_color(w.lbl_name, C_OFF_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_name, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(w.lbl_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w.lbl_name, SSR_BTN_W / 2);

    w.lbl_countdown = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_countdown, "00:00");
    lv_obj_set_style_text_color(w.lbl_countdown, C_OFF_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_countdown, &font_montserrat_24_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_countdown, LV_ALIGN_CENTER, 0, 6);
    lv_obj_add_flag(w.lbl_countdown, LV_OBJ_FLAG_HIDDEN);

    w.lbl_status = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_status, "Izključeno");
    lv_obj_set_style_text_color(w.lbl_status, C_OFF_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_status, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_status, LV_ALIGN_BOTTOM_LEFT, 8, -(SSR_BAR_H + 8));

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

    w.lbl_lock = lv_label_create(w.btn);
    lv_label_set_text(w.lbl_lock, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(w.lbl_lock, C_DIS_TXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_lock, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_lock, LV_ALIGN_TOP_RIGHT, -8, 8);
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
// PARKING — EVENT CALLBACK
// ============================================================

static void pkg_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    PkgWidget* w = (PkgWidget*)lv_event_get_user_data(e);
    if (!w) return;
    EventBus::publish(
        w->idx == 0 ? EventType::BUTTON_EDIT_VEHICLE_A
                    : EventType::BUTTON_EDIT_VEHICLE_B, 0);
}

// ============================================================
// PARKING — KREACIJA + STILIZACIJA
// ============================================================

static void pkg_apply(PkgWidget& w) {
    if (w.data.occupied) {
        lv_obj_set_style_bg_color(w.card, C_PKG_OCC_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(w.card, C_PKG_OCC_BORDER, LV_PART_MAIN);
        lv_label_set_text(w.lbl_name, w.data.vehicle_name[0] ? w.data.vehicle_name : "?");
        lv_obj_set_style_text_color(w.lbl_name, C_PKG_OCC_NAME, LV_PART_MAIN);
        char stats[40];
        snprintf(stats, sizeof(stats), "%lu\xC3\x97  DTW %.1f",
                 (unsigned long)w.data.parking_count, (double)w.data.dtw_distance);
        lv_label_set_text(w.lbl_stats, stats);
        car_set_color(w.car, C_CAR_OCC);
    } else {
        lv_obj_set_style_bg_color(w.card, C_PKG_EMPTY_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(w.card, C_PKG_EMPTY_BORDER, LV_PART_MAIN);
        lv_label_set_text(w.lbl_name, "Prazno");
        lv_obj_set_style_text_color(w.lbl_name, C_PKG_EMPTY_NAME, LV_PART_MAIN);
        lv_label_set_text(w.lbl_stats, "");
        car_set_color(w.car, C_CAR_EMPTY);
    }
}

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

    w.lbl_title = lv_label_create(w.card);
    lv_label_set_text(w.lbl_title, idx == 0 ? "Mesto A" : "Mesto B");
    lv_obj_set_style_text_color(w.lbl_title, C_PKG_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_title, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_title, LV_ALIGN_TOP_LEFT, 8, 6);

    w.lbl_name = lv_label_create(w.card);
    lv_label_set_text(w.lbl_name, "Prazno");
    lv_obj_set_style_text_color(w.lbl_name, C_PKG_EMPTY_NAME, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_18_sl, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_name, 8, 28);
    lv_label_set_long_mode(w.lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(w.lbl_name, PKG_W - 76);

    w.lbl_stats = lv_label_create(w.card);
    lv_label_set_text(w.lbl_stats, "");
    lv_obj_set_style_text_color(w.lbl_stats, C_PKG_STATS, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_stats, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_align(w.lbl_stats, LV_ALIGN_BOTTOM_LEFT, 8, -6);

    car_create(w, PKG_W - 68, 16, C_CAR_EMPTY);
}

// ============================================================
// RADAR — KREACIJA + STILIZACIJA
// ============================================================

static void rad_apply(RadWidget& w) {
    uint8_t    pct = w.data.strength_pct;
    lv_color_t col = (pct >= 50) ? C_RAD_HIGH : (pct >= 20) ? C_RAD_MID : C_RAD_LOW;
    lv_arc_set_value(w.arc, pct);
    lv_obj_set_style_arc_color(w.arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(w.lbl_pct, col, LV_PART_MAIN);
    char buf[6]; snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(w.lbl_pct, buf);
}

static void rad_create(uint8_t idx, lv_obj_t* parent, int x, int y) {
    RadWidget& w = s_rad[idx];
    w.idx  = idx;
    w.data = { 0, false, false };

    w.arc = lv_arc_create(parent);
    lv_obj_set_size(w.arc, RAD_ARC_SZ, RAD_ARC_SZ);
    lv_obj_set_pos(w.arc, x + (RAD_W - RAD_ARC_SZ) / 2, y);
    lv_arc_set_rotation(w.arc, 135);
    lv_arc_set_bg_angles(w.arc, 0, 270);
    lv_arc_set_value(w.arc, 0);
    lv_arc_set_range(w.arc, 0, 100);
    lv_obj_remove_flag(w.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(w.arc, C_RAD_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(w.arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.arc, C_RAD_LOW, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(w.arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(w.arc, 0, LV_PART_KNOB);

    w.lbl_pct = lv_label_create(parent);
    lv_label_set_text(w.lbl_pct, "0%");
    lv_obj_set_style_text_color(w.lbl_pct, C_RAD_LOW, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_pct, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_pct,
                   x + (RAD_W - RAD_ARC_SZ) / 2 + RAD_ARC_SZ / 2 - 10,
                   y + RAD_ARC_SZ / 2 - 8);

    w.lbl_name = lv_label_create(parent);
    lv_label_set_text(w.lbl_name, RADAR_NAMES[idx]);
    lv_obj_set_style_text_color(w.lbl_name, C_RAD_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_font(w.lbl_name, &font_montserrat_14_sl, LV_PART_MAIN);
    lv_obj_set_pos(w.lbl_name, x, y + RAD_ARC_SZ + 2);
    lv_obj_set_width(w.lbl_name, RAD_W);
    lv_obj_set_style_text_align(w.lbl_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

void screen_main_create(lv_obj_t* parent) {
    SMNI("screen_main_create...");
    lv_obj_set_style_bg_color(parent, C_SCREEN_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 2; col++) {
            uint8_t idx = (uint8_t)(row * 2 + col);
            ssr_create(idx, parent,
                       PAD + col * (SSR_BTN_W + PAD),
                       PAD + row * (SSR_BTN_H + PAD));
        }
    s_countdown_timer = lv_timer_create(ssr_countdown_cb, 1000, nullptr);

    for (int i = 0; i < 2; i++)
        pkg_create((uint8_t)i, parent, PAD + i * (PKG_W + PAD), PKG_Y);

    for (int i = 0; i < 4; i++)
        rad_create((uint8_t)i, parent, PAD + i * (RAD_W + PAD), RAD_Y);

    s_created = true;
    SMNI("screen_main_create OK  SSR=%dx%d  PKG=%dx%d  RAD_ARC=%dpx",
         SSR_BTN_W, SSR_BTN_H, PKG_W, PKG_H, RAD_ARC_SZ);
    SMNI("Layout: SSR_SEC_H=%d PKG_Y=%d RAD_Y=%d SCR_total=%d",
         SSR_SEC_H, PKG_Y, RAD_Y, RAD_Y + RAD_ARC_SZ + 20);
}

void screen_main_apply_updates() {
    // Placeholder — implementiraj skupaj z hal_display getter funkcijami
}

void screen_main_set_ssr(uint8_t idx, const SsrDisplayData& data) {
    if (idx >= 4 || !s_created) return;
    s_ssr[idx].data = data;
    ssr_style(s_ssr[idx]);
}

void screen_main_set_parking(uint8_t idx, const ParkingDisplayData& data) {
    if (idx >= 2 || !s_created) return;
    s_pkg[idx].data = data;
    pkg_apply(s_pkg[idx]);
}

void screen_main_set_radar(uint8_t idx, const RadarDisplayData& data) {
    if (idx >= 4 || !s_created) return;
    s_rad[idx].data = data;
    rad_apply(s_rad[idx]);
}