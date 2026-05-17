// ============================================================
// screen_alarm.cpp — LCD Alarm zaslon implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================

#include "screen_alarm.h"
#include "alarm.h"
#include "logger.h"
#include "event_bus.h"

#define SAI(fmt, ...) LOG_INFO ("SCR_ALARM", fmt, ##__VA_ARGS__)
#define SAW(fmt, ...) LOG_WARN ("SCR_ALARM", fmt, ##__VA_ARGS__)
#define SAD(fmt, ...) LOG_DEBUG("SCR_ALARM", fmt, ##__VA_ARGS__)

// ============================================================
// LVGL OBJEKTI
// ============================================================

static lv_obj_t* s_scr         = nullptr;   // glavni zaslon
static lv_obj_t* s_bg_rect     = nullptr;   // ozadje (za barvni efekt)
static lv_obj_t* s_lbl_alarm   = nullptr;   // velik napis ALARM
static lv_obj_t* s_lbl_sub     = nullptr;   // manjši napis (stanje)
static lv_obj_t* s_pin_panel   = nullptr;   // PIN vnos panel (skriti ob zagonu)
static lv_obj_t* s_pin_display = nullptr;   // prikaz vnesenih znakov "● ● ● ●"
static lv_obj_t* s_pin_err_lbl = nullptr;   // "Napačna koda" napis
static lv_obj_t* s_kbd         = nullptr;   // numerična tipkovnica

static bool      s_initialized = false;
static bool      s_active      = false;
static AlarmStateEnum s_current_state = AlarmStateEnum::OFF;

// PIN vnos buffer
static char      s_pin_buf[ALARM_PIN_MAX_LEN + 1] = {};
static uint8_t   s_pin_pos = 0;

// Timer za "napačen PIN" efekt (kratko prikaže napako, potem počisti)
static lv_timer_t* s_err_timer = nullptr;

// ============================================================
// BARVE
// ============================================================
// ARMED:    temno rdeče ozadje, bel napis
// TRIGGERED: svetlejše rdeče ozadje + utripajoč okvir

#define COLOR_ARMED_BG      lv_color_hex(0x3D0000)   // temno rdeča
#define COLOR_TRIGGERED_BG  lv_color_hex(0x8B0000)   // srednja rdeča
#define COLOR_TEXT_WHITE    lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_ORANGE   lv_color_hex(0xFF8800)
#define COLOR_PIN_ACTIVE    lv_color_hex(0xFF4444)    // aktivna pika
#define COLOR_PIN_EMPTY     lv_color_hex(0x555555)    // prazna pika
#define COLOR_KEY_BG        lv_color_hex(0x1A0000)    // tipke ozadje
#define COLOR_KEY_PR        lv_color_hex(0xFF2222)    // tipke pritisnjene
#define COLOR_ERR_TEXT      lv_color_hex(0xFF4444)

// ============================================================
// PIN PRIKAZ
// ============================================================

static void _update_pin_display() {
    if (!s_pin_display) return;

    // Pokaži ● za vsak vnesen znak, ○ za prazen
    // Privzeto max 8 znakov — prikažemo samo toliko mest kolikor je dolžina PIN
    // (ne vemo dolžine → prikažemo vnesene pike + preostale prazne do max 8)
    char buf[64] = {};
    char* p = buf;
    uint8_t max_shown = 8;
    for (uint8_t i = 0; i < max_shown; i++) {
        if (i > 0) { *p++ = ' '; }
        if (i < s_pin_pos) {
            // UTF-8 za ● (U+25CF)
            *p++ = (char)0xE2; *p++ = (char)0x97; *p++ = (char)0x8F;
        } else {
            // UTF-8 za ○ (U+25CB)
            *p++ = (char)0xE2; *p++ = (char)0x97; *p++ = (char)0x8B;
        }
    }
    *p = '\0';
    lv_label_set_text(s_pin_display, buf);
}

// ============================================================
// TIPKOVNICA CALLBACK
// ============================================================

static void _on_key_pressed(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
    const char* key_text = lv_btnmatrix_get_btn_text(kb, lv_btnmatrix_get_selected_btn(kb));
    if (!key_text) return;

    // Skrij napako ob novem vnosu
    if (s_pin_err_lbl) lv_obj_add_flag(s_pin_err_lbl, LV_OBJ_FLAG_HIDDEN);

    if (strcmp(key_text, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_pin_pos > 0) {
            s_pin_pos--;
            s_pin_buf[s_pin_pos] = '\0';
        }
        _update_pin_display();
        return;
    }

    if (strcmp(key_text, LV_SYMBOL_OK) == 0 || strcmp(key_text, "OK") == 0) {
        // Preveri PIN
        if (s_pin_pos < ALARM_PIN_MIN_LEN) {
            // Prekratek PIN
            if (s_pin_err_lbl) {
                lv_label_set_text(s_pin_err_lbl, "PIN prekratek");
                lv_obj_clear_flag(s_pin_err_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        SAD("PIN poskus (len=%d)", s_pin_pos);
        bool ok = alarm_disarm_pin(s_pin_buf);
        if (ok) {
            SAI("PIN pravilen → hide alarm screen");
            screen_alarm_hide();
        } else {
            // Napačen PIN — pokaži napako, počisti vnos
            if (s_pin_err_lbl) {
                lv_label_set_text(s_pin_err_lbl, "Napačna koda");
                lv_obj_clear_flag(s_pin_err_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            s_pin_pos = 0;
            memset(s_pin_buf, 0, sizeof(s_pin_buf));
            _update_pin_display();
            SAW("Napačen PIN");
        }
        return;
    }

    // Numerična tipka
    if (key_text[0] >= '0' && key_text[0] <= '9') {
        if (s_pin_pos < ALARM_PIN_MAX_LEN) {
            s_pin_buf[s_pin_pos++] = key_text[0];
            s_pin_buf[s_pin_pos]   = '\0';
        }
        _update_pin_display();
    }
}

// ============================================================
// DOTIK NA OZADJE — odpri PIN vnos
// ============================================================

static void _on_screen_touch(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    // Pokaži PIN panel, skrij sub-label
    if (s_pin_panel) {
        lv_obj_clear_flag(s_pin_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_sub) lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);

    SAD("Zaslon dotaknjen → PIN vnos");
}

// ============================================================
// screen_alarm_init
// ============================================================

void screen_alarm_init() {
    if (s_initialized) return;
    SAI("screen_alarm_init()");

    // Ustvari nov zaslon
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_size(s_scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_scr, COLOR_ARMED_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // Celozaslonski klik handler
    lv_obj_add_event_cb(s_scr, _on_screen_touch, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    // --- Velik napis ALARM ---
    s_lbl_alarm = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_alarm, "ALARM");
    lv_obj_set_style_text_color(s_lbl_alarm, COLOR_TEXT_WHITE, 0);
    lv_obj_set_style_text_font(s_lbl_alarm, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_alarm, LV_ALIGN_CENTER, 0, -60);

    // --- Sub napis (stanje / namig) ---
    s_lbl_sub = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_sub, " ");  // prazen — ob dotiku se skrije
    lv_obj_set_style_text_color(s_lbl_sub, COLOR_TEXT_ORANGE, 0);
    lv_obj_set_style_text_font(s_lbl_sub, &lv_font_montserrat_18, 0);
    lv_obj_align(s_lbl_sub, LV_ALIGN_CENTER, 0, 0);

    // -------------------------------------------------------
    // PIN panel — skrit ob zagonu, pokaže se ob dotiku
    // -------------------------------------------------------
    s_pin_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_pin_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_pin_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pin_panel, COLOR_ARMED_BG, 0);
    lv_obj_set_style_bg_opa(s_pin_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pin_panel, 0, 0);
    lv_obj_set_style_pad_all(s_pin_panel, 8, 0);
    lv_obj_add_flag(s_pin_panel, LV_OBJ_FLAG_HIDDEN);  // ← skriti ob init

    // PIN pika prikaz
    s_pin_display = lv_label_create(s_pin_panel);
    _update_pin_display();
    lv_obj_set_style_text_color(s_pin_display, COLOR_PIN_ACTIVE, 0);
    lv_obj_set_style_text_font(s_pin_display, &lv_font_montserrat_24, 0);
    lv_obj_align(s_pin_display, LV_ALIGN_TOP_MID, 0, 20);

    // Napaka label (skrit ob zagonu)
    s_pin_err_lbl = lv_label_create(s_pin_panel);
    lv_label_set_text(s_pin_err_lbl, "Napačna koda");
    lv_obj_set_style_text_color(s_pin_err_lbl, COLOR_ERR_TEXT, 0);
    lv_obj_set_style_text_font(s_pin_err_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(s_pin_err_lbl, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_flag(s_pin_err_lbl, LV_OBJ_FLAG_HIDDEN);

    // Numerična tipkovnica
    // Layout: 1 2 3 / 4 5 6 / 7 8 9 / ⌫ 0 OK
    static const char* kb_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", "OK", ""
    };
    static const lv_btnmatrix_ctrl_t kb_ctrl[] = {
        (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1,
        (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1,
        (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1,
        (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)1, (lv_btnmatrix_ctrl_t)2  // OK dvakrat širša
    };

    s_kbd = lv_btnmatrix_create(s_pin_panel);
    lv_btnmatrix_set_map(s_kbd, kb_map);
    lv_btnmatrix_set_ctrl_map(s_kbd, kb_ctrl);
    lv_obj_set_size(s_kbd, LV_HOR_RES - 40, 220);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Stili tipkovnice
    lv_obj_set_style_bg_color(s_kbd, COLOR_KEY_BG, 0);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_kbd, COLOR_TEXT_WHITE, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kbd, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kbd, COLOR_KEY_BG, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kbd, COLOR_KEY_PR, (lv_style_selector_t)((uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_PRESSED));
    lv_obj_set_style_border_color(s_kbd, lv_color_hex(0x440000), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kbd, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kbd, 6, LV_PART_ITEMS);

    lv_obj_add_event_cb(s_kbd, _on_key_pressed, LV_EVENT_VALUE_CHANGED, nullptr);

    s_initialized = true;
    SAI("screen_alarm_init OK");
}

// ============================================================
// screen_alarm_show
// ============================================================

void screen_alarm_show(AlarmStateEnum state) {
    if (!s_initialized) screen_alarm_init();

    // Počisti PIN buffer ob vsakem novem prikazu
    s_pin_pos = 0;
    memset(s_pin_buf, 0, sizeof(s_pin_buf));
    _update_pin_display();

    // Skrij PIN panel — pokaže se ob dotiku
    if (s_pin_panel) lv_obj_add_flag(s_pin_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_pin_err_lbl) lv_obj_add_flag(s_pin_err_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_sub) lv_obj_clear_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);

    screen_alarm_update(state);

    lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    s_active = true;
    s_current_state = state;

    SAI("Alarm zaslon prikazan (state=%d)", (int)state);
}

// ============================================================
// screen_alarm_hide
// ============================================================
// Predpostavka: screen_main je bil naložen pred alarm_show.
// Ker ne hranimo reference na screen_main, naloži prek
// extern deklaracije screen_main_get_screen().

extern lv_obj_t* screen_main_get_screen();  // implementirano v screen_main.cpp

void screen_alarm_hide() {
    if (!s_active) return;

    lv_obj_t* main_scr = screen_main_get_screen();
    if (main_scr) {
        lv_scr_load_anim(main_scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    }
    s_active = false;
    s_current_state = AlarmStateEnum::OFF;
    SAI("Alarm zaslon skrit → screen_main");
}

// ============================================================
// screen_alarm_update
// ============================================================

void screen_alarm_update(AlarmStateEnum state) {
    if (!s_initialized) return;
    s_current_state = state;

    if (state == AlarmStateEnum::ARMED) {
        lv_obj_set_style_bg_color(s_scr, COLOR_ARMED_BG, 0);
        if (s_pin_panel) lv_obj_set_style_bg_color(s_pin_panel, COLOR_ARMED_BG, 0);
        lv_label_set_text(s_lbl_alarm, "ALARM AKTIVEN");
        lv_obj_set_style_text_color(s_lbl_alarm, COLOR_TEXT_WHITE, 0);
        // Sub napis je prazen — brez namiga na PIN (namerno)
        lv_label_set_text(s_lbl_sub, " ");

    } else if (state == AlarmStateEnum::TRIGGERED) {
        lv_obj_set_style_bg_color(s_scr, COLOR_TRIGGERED_BG, 0);
        if (s_pin_panel) lv_obj_set_style_bg_color(s_pin_panel, COLOR_TRIGGERED_BG, 0);
        lv_label_set_text(s_lbl_alarm, "!! ALARM !!");
        lv_obj_set_style_text_color(s_lbl_alarm, COLOR_TEXT_ORANGE, 0);
        lv_label_set_text(s_lbl_sub, " ");
    }
}

bool screen_alarm_is_active() { return s_active; }
