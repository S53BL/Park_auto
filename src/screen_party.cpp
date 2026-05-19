// ============================================================
// screen_party.cpp — Party zaslon (LCD UI)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// TRENUTNO STANJE:
//   UI je popolnoma implementiran. Vsi gumbi, sliderji in toggle
//   delujejo in publishajo pravilne EventBus evente.
//   MUX IO45 preklop je implementiran direktno (digitalWrite).
//
// KAJ MANJKA ZA PRODUKCIJO:
//   1. web_ui.cpp — mora se subscribirati na BUTTON_PARTY_* evente
//      in izvesti dejanske HTTP klice na WLED Party ESP.
//      Glej komentarje "TODO: web_ui.cpp" spodaj.
//   2. config_mgr.cpp — IP naslov Party ESP mora biti nastavljiv
//      prek web vmesnika in shranjen v NVS.
//   3. WiFi mora biti inicializiran (wifiTask) preden WLED klici delujejo.
//
// WLED HTTP IMPLEMENTACIJA (za web_ui.cpp):
//   Vsak BUTTON_PARTY_* event v web_ui.cpp:
//
//   // Primer za BUTTON_PARTY_TOGGLE (payload=1 → on, 0 → off):
//   HTTPClient http;
//   http.begin("http://" + wled_ip + "/json/state");
//   http.addHeader("Content-Type", "application/json");
//   String body = payload ? "{\"on\":true}" : "{\"on\":false}";
//   int code = http.POST(body);
//   http.end();
//
//   // Primer za BUTTON_PARTY_EFFECT (payload=fx_id):
//   String body = "{\"seg\":[{\"fx\":" + String(payload) + "}]}";
//
//   // Primer za BUTTON_PARTY_COLOR (payload=(R<<16)|(G<<8)|B):
//   uint8_t r=(payload>>16)&0xFF, g=(payload>>8)&0xFF, b=payload&0xFF;
//   String body = "{\"seg\":[{\"col\":[[" + String(r) + ","
//                 + String(g) + "," + String(b) + "]]}]}";
//
//   // Primer za BUTTON_PARTY_BRIGHTNESS (payload=0-255):
//   String body = "{\"bri\":" + String(payload) + "}";
//
//   // Primer za BUTTON_PARTY_SPEED (payload=0-255):
//   String body = "{\"seg\":[{\"sx\":" + String(payload) + "}]}";
//
//   // Primer za BUTTON_PARTY_PRESET (payload=preset_id):
//   // Pošlji vse parametre naenkrat iz preset tabele v config_mgr.cpp
//
// MUX ZAMIK (200ms):
//   Ob party ON: najprej IO45=HIGH, delay(200), nato WLED {"on":true}
//   Ob party OFF: najprej WLED {"on":false}, delay(200), nato IO45=LOW
//   To prepreči signal collision na MUX stikalu (74HC257N).
//   Zamik je implementiran v web_ui.cpp (ne tukaj, ker HTTP je blokirajoč).
//
// ============================================================

#include "screen_party.h"
#include "event_bus.h"
#include "config.h"
#include "config_mgr.h"
#include "light_logic.h"
#include <Arduino.h>
#include <freertos/semphr.h>

// ============================================================
// LOGGING
// ============================================================

#include "logger.h"
#define PARTY_I(fmt, ...) LOG_INFO ("PARTY", fmt, ##__VA_ARGS__)
#define PARTY_D(fmt, ...) LOG_DEBUG("PARTY", fmt, ##__VA_ARGS__)

// ============================================================
// BARVE — posodobljeno 2026-04 (Tailwind-inspired dark + cyan party)
// ============================================================

#define C_BG              lv_color_hex(0x111827)   // ozadje celotnega party zaslona
#define C_SECTION_BG      lv_color_hex(0x1A2332)   // ozadje sekcijskega headerja (temnejši od kartic)
#define C_CARD_BG         lv_color_hex(0x1F2937)   // ozadje kartic / gumbov
#define C_BORDER_OFF      lv_color_hex(0x334155)
#define C_BORDER_ON       lv_color_hex(0x06B6D4)
#define C_PARTY_PRIMARY   lv_color_hex(0x06B6D4)   // cyan — primarna party barva (slider indicator)
#define C_PARTY_ACTIVE    lv_color_hex(0x22D3EE)   // svetel cyan — aktiven element
#define C_PRIO_ACTIVE     lv_color_hex(0xF59E0B)   // amber — priority gumb aktiven
#define C_PRIO_ON_BG      lv_color_hex(0x3D2A08)   // temno ozadje priority gumb ON
#define C_PARTY_DIM_TEXT  lv_color_hex(0x94A3B8)   // tekst sekcijskih headerjev (ločen od ozadja)
#define C_PARTY_DIM_BG    lv_color_hex(0x2D3A4F)   // ozadje aktivnih gumbov (svetlejši odtenek)
#define C_TEXT            lv_color_hex(0xF1F5F9)
#define C_TEXT_DIM        lv_color_hex(0xCBD5E1)
#define C_TEXT_BRIGHT     lv_color_hex(0xFFFFFF)
#define C_TOGGLE_OFF      lv_color_hex(0x1F2937)
#define C_TOGGLE_ON       lv_color_hex(0x06B6D4)

// Prej inline — zdaj makrota
#define C_COLOR_DOT_BORDER_OFF  lv_color_hex(0x334155)

// ============================================================
// DIMENZIJE
// ============================================================

#define SCR_W             320
#define SCR_H             480
#define PAD               8
#define SECTION_W         (SCR_W - PAD * 2)

// Toggle (75%) + Priority gumb (25%), oba v isti vrstici
#define TOGGLE_PRIO_GAP   4
#define PRIO_W            74
#define TOGGLE_W          (SECTION_W - TOGGLE_PRIO_GAP - PRIO_W)
#define TOGGLE_H          52

// Sloti 3×3
#define SLOT_COLS         3
#define SLOT_ROWS         3
#define SLOT_GAP          4
#define SLOT_BTN_W        ((SECTION_W - SLOT_GAP * (SLOT_COLS - 1)) / SLOT_COLS)
#define SLOT_BTN_H        44

// Barve — 7 krogcev
#define CLR_COUNT         7
#define CLR_DOT_SIZE      32
#define CLR_GAP           ((SECTION_W - CLR_COUNT * CLR_DOT_SIZE) / (CLR_COUNT + 1))

// Sliderji
#define SLIDER_H          36
#define SLIDER_ROW_H      52

// ============================================================
// BARVE PALETA (skladno z LCD_UI_Arhitektura.docx sekcija 5.3)
// 7 barv — posodobljeno 2026-04
// ============================================================

static const uint32_t CLR_PALETTE[CLR_COUNT] = {
    0xFFFFFF,   // bela
    0xFF2020,   // rdeča
    0xFF8000,   // oranžna
    0xFFDD00,   // rumena
    0x22CC22,   // zelena
    0x2266FF,   // modra
    0xAA00CC    // vijolična
};

// ============================================================
// WIDGET STRUKTURE
// ============================================================

struct SlotBtn {
    lv_obj_t* btn;
    lv_obj_t* lbl;
    uint8_t   idx;
};

struct ColorDot {
    lv_obj_t* dot;
    uint32_t  rgb;
    bool      selected;
};

// ============================================================
// STATIČNO STANJE
// ============================================================

static bool s_created = false;

// Toggle widget
static lv_obj_t* s_toggle_btn   = nullptr;
static lv_obj_t* s_toggle_lbl   = nullptr;
static lv_obj_t* s_toggle_dot   = nullptr;

// Priority gumb
static lv_obj_t* s_prio_btn     = nullptr;
static lv_obj_t* s_prio_lbl     = nullptr;
static bool      s_prio_on      = false;   // lokalna kopija za takojšen odziv na dotik

// Sloti 3×3
static SlotBtn   s_slot[9]      = {};

// Barve
static ColorDot  s_clr[CLR_COUNT] = {};

// Sliderji
static lv_obj_t* s_slider_bri   = nullptr;
static lv_obj_t* s_lbl_bri_val  = nullptr;
static lv_obj_t* s_slider_spd   = nullptr;
static lv_obj_t* s_lbl_spd_val  = nullptr;

// Party stanje + dirty flag
static SemaphoreHandle_t s_mutex = nullptr;
static volatile bool s_dirty          = false;
static volatile bool s_reload_slots   = false;
static PartyState s_state = {
    false,      // party_on
    0xFF,       // active_slot = brez
    0,          // fx_id = Solid
    191,        // brightness
    128,        // speed
    0xFFFFFF    // color = bela
};

// ============================================================
// POMOŽNE FUNKCIJE
// ============================================================

static lv_obj_t* make_section_header(lv_obj_t* parent, int y, const char* title) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, C_PARTY_DIM_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(lbl, PAD + 2, y);
    return lbl;
}

// Pretvori uint32_t RGB v lv_color_t
static lv_color_t rgb_to_lv(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// ============================================================
// TOGGLE
// ============================================================

static void toggle_update_visual() {
    if (!s_toggle_btn) return;
    if (s_state.party_on) {
        lv_obj_set_style_bg_color(s_toggle_btn, C_TOGGLE_ON, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_toggle_btn, C_PARTY_ACTIVE, LV_PART_MAIN);
        lv_label_set_text(s_toggle_lbl, LV_SYMBOL_PLAY "  PARTY  ON");
        lv_obj_set_style_text_color(s_toggle_lbl, C_TEXT_BRIGHT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_toggle_dot, C_PARTY_ACTIVE, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(s_toggle_btn, C_TOGGLE_OFF, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_toggle_btn, C_BORDER_OFF, LV_PART_MAIN);
        lv_label_set_text(s_toggle_lbl, LV_SYMBOL_STOP "  PARTY  OFF");
        lv_obj_set_style_text_color(s_toggle_lbl, C_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_toggle_dot, C_TEXT_DIM, LV_PART_MAIN);
    }
}

static void toggle_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_state.party_on = !s_state.party_on;
    toggle_update_visual();

    // MUX preklop — direktno, brez zunanjega HW (BSP pin)
    // TODO: web_ui.cpp mora ob BUTTON_PARTY_TOGGLE:
    //   ON:  delay(200) → HTTP POST {"on":true} na WLED
    //   OFF: HTTP POST {"on":false} → delay(200) → IO45=LOW
    //   Zamik 200ms (MUX_SWITCH_DELAY_MS) prepreči signal collision na 74HC257N.
    if (s_state.party_on) {
        // MUX HIGH takoj — neblokirajoče; web_ui wledTask počaka
        // MUX_SWITCH_DELAY_MS in nato pošlje HTTP {"on":true} na WLED
        digitalWrite(PIN_MUX_SELECT, HIGH);
        PARTY_I("MUX → PARTY ESP (IO%d HIGH)", PIN_MUX_SELECT);
    } else {
        // MUX LOW NE postavljamo tukaj — wledTask pošlje HTTP {"on":false}
        // in šele nato (po MUX_SWITCH_DELAY_MS) vrne MUX na PRIMARY
    }

    uint32_t payload = s_state.party_on ? 1 : 0;
    EventBus::publish(EventType::BUTTON_PARTY_TOGGLE, payload);
    PARTY_D("BUTTON_PARTY_TOGGLE payload=%lu", (unsigned long)payload);

    // TODO: web_ui.cpp subscriber:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_TOGGLE, [](const Event& ev) {
    //       bool on = ev.payload == 1;
    //       if (on) { setMuxParty(); vTaskDelay(200); wledPost("{\"on\":true}"); }
    //       else    { wledPost("{\"on\":false}"); vTaskDelay(200); setMuxPrimary(); }
    //   });
}

// ============================================================
// PRIORITY GUMB
// ============================================================

static void priority_update_visual() {
    if (!s_prio_btn || !s_prio_lbl) return;
    if (s_prio_on) {
        lv_obj_set_style_bg_color(s_prio_btn, C_PRIO_ON_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_prio_btn, C_PRIO_ACTIVE, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prio_lbl, C_PRIO_ACTIVE, LV_PART_MAIN);
        lv_label_set_text(s_prio_lbl, "PRIO\nON");
    } else {
        lv_obj_set_style_bg_color(s_prio_btn, C_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_prio_btn, C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prio_lbl, C_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(s_prio_lbl, "PRIO\nOFF");
    }
}

static void priority_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_prio_on = !s_prio_on;
    priority_update_visual();
    EventBus::publish(EventType::BUTTON_PARTY_PRIORITY, s_prio_on ? 1 : 0);
    PARTY_D("BUTTON_PARTY_PRIORITY: %s", s_prio_on ? "ON" : "OFF");
}

// ============================================================
// SLOTI
// ============================================================

static void slot_update_visual(uint8_t active_slot) {
    for (int i = 0; i < 9; i++) {
        if (!s_slot[i].btn) continue;
        bool act = (i == active_slot) && s_state.party_on;
        lv_obj_set_style_bg_color(s_slot[i].btn,
            act ? C_PARTY_DIM_BG : C_CARD_BG,   LV_PART_MAIN);
        lv_obj_set_style_border_color(s_slot[i].btn,
            act ? C_PARTY_ACTIVE : C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_slot[i].lbl,
            act ? C_PARTY_ACTIVE : C_TEXT_DIM,   LV_PART_MAIN);
    }
}

static void slot_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    SlotBtn* sb = (SlotBtn*)lv_event_get_user_data(e);
    if (!sb) return;
    if (!s_state.party_on) {
        PARTY_D("Slot ignoriran — party OFF");
        return;
    }

    uint8_t idx = sb->idx;
    s_state.active_slot = idx;
    slot_update_visual(idx);

    EventBus::publish(EventType::BUTTON_PARTY_SLOT, idx);
    PARTY_D("BUTTON_PARTY_SLOT idx=%d", idx);
}

// ============================================================
// BARVE
// ============================================================

static void color_update_visual(uint32_t selected_rgb) {
    for (int i = 0; i < CLR_COUNT; i++) {
        bool sel = (CLR_PALETTE[i] == selected_rgb) && s_state.party_on;
        s_clr[i].selected = sel;
        // Obroba: bela obroba = selected, brez = neselected
        lv_obj_set_style_border_width(s_clr[i].dot, sel ? 3 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_clr[i].dot,
            sel ? lv_color_white() : C_COLOR_DOT_BORDER_OFF, LV_PART_MAIN);
        // Dimmed ko party OFF
        lv_obj_set_style_opa(s_clr[i].dot,
            s_state.party_on ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    }
}

static void color_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ColorDot* cd = (ColorDot*)lv_event_get_user_data(e);
    if (!cd) return;
    if (!s_state.party_on) {
        PARTY_D("Barva ignorirana — party OFF");
        return;
    }

    s_state.color_rgb = cd->rgb;
    s_state.active_slot = 0xFF;
    color_update_visual(s_state.color_rgb);

    EventBus::publish(EventType::BUTTON_PARTY_COLOR, s_state.color_rgb);
    PARTY_D("BUTTON_PARTY_COLOR rgb=0x%06lX", (unsigned long)s_state.color_rgb);

    // TODO: web_ui.cpp subscriber:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_COLOR, [](const Event& ev) {
    //       uint8_t r=(ev.payload>>16)&0xFF, g=(ev.payload>>8)&0xFF, b=ev.payload&0xFF;
    //       wledPost("{\"seg\":[{\"col\":[["+String(r)+","+String(g)+","+String(b)+"]]}"
    //               + "]}");
    //   });
}

// ============================================================
// SLIDERJI
// ============================================================

static void slider_bri_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    if (!s_state.party_on) return;

    lv_obj_t* sld = (lv_obj_t*)lv_event_get_target(e);
    s_state.brightness = (uint8_t)lv_slider_get_value(sld);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_state.brightness);
    lv_label_set_text(s_lbl_bri_val, buf);

    EventBus::publish(EventType::BUTTON_PARTY_BRIGHTNESS, s_state.brightness);
    PARTY_D("BUTTON_PARTY_BRIGHTNESS val=%d", s_state.brightness);

    // TODO: web_ui.cpp subscriber:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_BRIGHTNESS, [](const Event& ev) {
    //       wledPost("{\"bri\":" + String(ev.payload) + "}");
    //   });
    //   Opomba: slider pošilja samo ob RELEASED (ne med drsenjem) —
    //   prepreči prekomerne HTTP klice.
}

static void slider_spd_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    if (!s_state.party_on) return;

    lv_obj_t* sld = (lv_obj_t*)lv_event_get_target(e);
    s_state.speed = (uint8_t)lv_slider_get_value(sld);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_state.speed);
    lv_label_set_text(s_lbl_spd_val, buf);

    EventBus::publish(EventType::BUTTON_PARTY_SPEED, s_state.speed);
    PARTY_D("BUTTON_PARTY_SPEED val=%d", s_state.speed);

    // TODO: web_ui.cpp subscriber:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_SPEED, [](const Event& ev) {
    //       wledPost("{\"seg\":[{\"sx\":" + String(ev.payload) + "}]}");
    //   });
}

// ============================================================
// screen_party_create — kliče samo lvglTask
// ============================================================

void screen_party_create(lv_obj_t* parent) {
    PARTY_I("screen_party_create...");

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) { PARTY_I("Mutex napaka!"); }
    }

    lv_obj_set_style_bg_color(parent, C_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_ACTIVE);

    int cy = PAD;

    // --------------------------------------------------------
    // TOGGLE — vklop/izklop party mode
    // --------------------------------------------------------
    s_toggle_btn = lv_obj_create(parent);
    lv_obj_set_size(s_toggle_btn, TOGGLE_W, TOGGLE_H);
    lv_obj_set_pos(s_toggle_btn, PAD, cy);
    lv_obj_set_style_bg_color(s_toggle_btn, C_TOGGLE_OFF, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_toggle_btn, C_BORDER_OFF, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toggle_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toggle_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_toggle_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_toggle_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toggle_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toggle_btn, toggle_event_cb, LV_EVENT_CLICKED, nullptr);

    // Indikator lučka levo
    s_toggle_dot = lv_obj_create(s_toggle_btn);
    lv_obj_set_size(s_toggle_dot, 14, 14);
    lv_obj_align(s_toggle_dot, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_bg_color(s_toggle_dot, C_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toggle_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toggle_dot, 7, LV_PART_MAIN);
    lv_obj_clear_flag(s_toggle_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_toggle_dot, LV_OBJ_FLAG_CLICKABLE);

    s_toggle_lbl = lv_label_create(s_toggle_btn);
    lv_label_set_text(s_toggle_lbl, LV_SYMBOL_STOP "  PARTY  OFF");
    lv_obj_set_style_text_color(s_toggle_lbl, C_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_toggle_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_toggle_lbl, LV_ALIGN_CENTER, 0, 0);

    // --------------------------------------------------------
    // PRIORITY gumb — desno od toggle, 25% širine
    // --------------------------------------------------------
    s_prio_btn = lv_obj_create(parent);
    lv_obj_set_size(s_prio_btn, PRIO_W, TOGGLE_H);
    lv_obj_set_pos(s_prio_btn, PAD + TOGGLE_W + TOGGLE_PRIO_GAP, cy);
    lv_obj_set_style_bg_color(s_prio_btn, C_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_prio_btn, C_BORDER_OFF, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_prio_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_prio_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_prio_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_prio_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_prio_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_prio_btn, priority_event_cb, LV_EVENT_CLICKED, nullptr);

    s_prio_lbl = lv_label_create(s_prio_btn);
    lv_label_set_text(s_prio_lbl, "PRIO\nOFF");
    lv_obj_set_style_text_color(s_prio_lbl, C_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_prio_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_prio_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_prio_lbl, LV_ALIGN_CENTER, 0, 0);

    cy += TOGGLE_H + PAD * 2;

    // --------------------------------------------------------
    // SLOTI — 3×3 grid (9 prednastavitev iz config_mgr)
    // --------------------------------------------------------
    make_section_header(parent, cy, "SLOTI");
    cy += 18;

    for (int row = 0; row < SLOT_ROWS; row++) {
        for (int col = 0; col < SLOT_COLS; col++) {
            int idx = row * SLOT_COLS + col;
            int x = PAD + col * (SLOT_BTN_W + SLOT_GAP);
            int y = cy + row * (SLOT_BTN_H + SLOT_GAP);

            s_slot[idx].idx = (uint8_t)idx;
            PartySlot sl = config_get_party_slot((uint8_t)idx);

            s_slot[idx].btn = lv_obj_create(parent);
            lv_obj_set_size(s_slot[idx].btn, SLOT_BTN_W, SLOT_BTN_H);
            lv_obj_set_pos(s_slot[idx].btn, x, y);
            lv_obj_set_style_bg_color(s_slot[idx].btn, C_CARD_BG, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_slot[idx].btn, C_BORDER_OFF, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_slot[idx].btn, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(s_slot[idx].btn, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_slot[idx].btn, 0, LV_PART_MAIN);
            lv_obj_clear_flag(s_slot[idx].btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_slot[idx].btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_slot[idx].btn, slot_event_cb,
                                LV_EVENT_CLICKED, &s_slot[idx]);

            s_slot[idx].lbl = lv_label_create(s_slot[idx].btn);
            lv_label_set_text(s_slot[idx].lbl, sl.name);
            lv_obj_set_style_text_color(s_slot[idx].lbl, C_TEXT_DIM, LV_PART_MAIN);
            lv_obj_set_style_text_font(s_slot[idx].lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_align(s_slot[idx].lbl, LV_ALIGN_CENTER, 0, 0);

            if (!sl.enabled) {
                lv_obj_add_state(s_slot[idx].btn, LV_STATE_DISABLED);
            }
        }
    }
    cy += SLOT_ROWS * (SLOT_BTN_H + SLOT_GAP) + PAD;

    // --------------------------------------------------------
    // BARVE — 7 krogcev v vrsti
    // --------------------------------------------------------
    make_section_header(parent, cy, "BARVA");
    cy += 18;

    for (int i = 0; i < CLR_COUNT; i++) {
        s_clr[i].rgb      = CLR_PALETTE[i];
        s_clr[i].selected = false;

        int x = PAD + CLR_GAP + i * (CLR_DOT_SIZE + CLR_GAP);

        s_clr[i].dot = lv_obj_create(parent);
        lv_obj_set_size(s_clr[i].dot, CLR_DOT_SIZE, CLR_DOT_SIZE);
        lv_obj_set_pos(s_clr[i].dot, x, cy);
        lv_obj_set_style_bg_color(s_clr[i].dot, rgb_to_lv(CLR_PALETTE[i]), LV_PART_MAIN);
        lv_obj_set_style_radius(s_clr[i].dot, CLR_DOT_SIZE / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_clr[i].dot, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_clr[i].dot, C_COLOR_DOT_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_opa(s_clr[i].dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_clear_flag(s_clr[i].dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_clr[i].dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_clr[i].dot, color_event_cb,
                            LV_EVENT_CLICKED, &s_clr[i]);
    }
    cy += CLR_DOT_SIZE + PAD * 2;

    // --------------------------------------------------------
    // SLIDERJI — svetlost in hitrost
    // --------------------------------------------------------
    make_section_header(parent, cy, "SVETLOST");
    cy += 18;

    {
        // Slider row — slider + vrednost label
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, SECTION_W, SLIDER_ROW_H);
        lv_obj_set_pos(row, PAD, cy);
        lv_obj_set_style_bg_color(row, C_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_slider_bri = lv_slider_create(row);
        lv_obj_set_size(s_slider_bri, SECTION_W - 60, SLIDER_H);
        lv_obj_align(s_slider_bri, LV_ALIGN_LEFT_MID, 0, 0);
        lv_slider_set_range(s_slider_bri, 0, 255);
        lv_slider_set_value(s_slider_bri, s_state.brightness, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_DIM_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_ACTIVE,  LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_bri, slider_bri_event_cb, LV_EVENT_RELEASED, nullptr);

        s_lbl_bri_val = lv_label_create(row);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", s_state.brightness);
        lv_label_set_text(s_lbl_bri_val, buf);
        lv_obj_set_style_text_color(s_lbl_bri_val, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lbl_bri_val, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_lbl_bri_val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    cy += SLIDER_ROW_H + PAD;

    make_section_header(parent, cy, "HITROST");
    cy += 18;

    {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, SECTION_W, SLIDER_ROW_H);
        lv_obj_set_pos(row, PAD, cy);
        lv_obj_set_style_bg_color(row, C_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_slider_spd = lv_slider_create(row);
        lv_obj_set_size(s_slider_spd, SECTION_W - 60, SLIDER_H);
        lv_obj_align(s_slider_spd, LV_ALIGN_LEFT_MID, 0, 0);
        lv_slider_set_range(s_slider_spd, 0, 255);
        lv_slider_set_value(s_slider_spd, s_state.speed, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_DIM_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_ACTIVE,  LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_spd, slider_spd_event_cb, LV_EVENT_RELEASED, nullptr);

        s_lbl_spd_val = lv_label_create(row);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", s_state.speed);
        lv_label_set_text(s_lbl_spd_val, buf);
        lv_obj_set_style_text_color(s_lbl_spd_val, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lbl_spd_val, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_lbl_spd_val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    cy += SLIDER_ROW_H + PAD * 2;

    lv_obj_set_height(parent, LV_SIZE_CONTENT);

    s_created = true;
    PARTY_I("screen_party_create OK (cy=%d)", cy);
}

// ============================================================
// screen_party_apply_updates — kliče samo lvglTask
// ============================================================

void screen_party_apply_updates() {
    if (!s_created) return;

    // Posodobi slot labele če je web UI spremenil slot podatke
    if (s_reload_slots) {
        s_reload_slots = false;
        screen_party_reload_slots();
    }

    // Posodobi vizuale iz dirty stanja (set iz web_ui WLED polling-a)
    if (!s_dirty) return;

    PartyState snap;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snap = s_state;
        s_dirty = false;
        xSemaphoreGive(s_mutex);
    } else {
        return;
    }

    // Sync priority stanje iz light_logic (thread-safe bool read)
    bool prio_now = light_logic_get_party_priority();
    if (prio_now != s_prio_on) {
        s_prio_on = prio_now;
        priority_update_visual();
    }

    toggle_update_visual();
    slot_update_visual(snap.active_slot);
    color_update_visual(snap.color_rgb);

    if (s_slider_bri) lv_slider_set_value(s_slider_bri, snap.brightness, LV_ANIM_OFF);
    if (s_slider_spd) lv_slider_set_value(s_slider_spd, snap.speed,      LV_ANIM_OFF);

    char buf[8];
    if (s_lbl_bri_val) { snprintf(buf, sizeof(buf), "%d", snap.brightness); lv_label_set_text(s_lbl_bri_val, buf); }
    if (s_lbl_spd_val) { snprintf(buf, sizeof(buf), "%d", snap.speed);      lv_label_set_text(s_lbl_spd_val, buf); }
}

// ============================================================
// THREAD-SAFE SETTERJI
// ============================================================

void screen_party_set_state(const PartyState& state) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_state = state;
        s_dirty = true;
        xSemaphoreGive(s_mutex);
    }
}

void screen_party_reload_slots() {
    for (int i = 0; i < 9; i++) {
        if (!s_slot[i].btn || !s_slot[i].lbl) continue;
        PartySlot sl = config_get_party_slot((uint8_t)i);
        lv_label_set_text(s_slot[i].lbl, sl.name);
        if (sl.enabled) {
            lv_obj_clear_state(s_slot[i].btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_slot[i].btn, LV_STATE_DISABLED);
        }
    }
}

void screen_party_request_slot_reload() {
    s_reload_slots = true;
}

PartyState screen_party_get_state() {
    PartyState result;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        result = s_state;
        xSemaphoreGive(s_mutex);
    } else {
        result = s_state;   // fallback brez mutex (read-only, tolerantno)
    }
    return result;
}