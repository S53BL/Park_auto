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
#include <Arduino.h>
#include <freertos/semphr.h>

// ============================================================
// LOGGING
// ============================================================

#define PARTY_I(fmt, ...) Serial.printf("[PARTY][I] " fmt "\n", ##__VA_ARGS__)
#define PARTY_D(fmt, ...) Serial.printf("[PARTY][D] " fmt "\n", ##__VA_ARGS__)

// ============================================================
// BARVE
// ============================================================

#define C_BG              lv_color_hex(0x0D000F)   // zelo temna vijolična ozadje
#define C_SECTION_BG      lv_color_hex(0x150015)
#define C_CARD_BG         lv_color_hex(0x1A001A)
#define C_BORDER_OFF      lv_color_hex(0x2A002A)
#define C_BORDER_ON       lv_color_hex(0x8800AA)
#define C_PARTY_PRIMARY   lv_color_hex(0xAA00CC)   // vijolična — party barva
#define C_PARTY_ACTIVE    lv_color_hex(0xCC44FF)   // svetla vijolična — aktiven element
#define C_PARTY_DIM       lv_color_hex(0x440055)   // dimmed vijolična
#define C_TEXT            lv_color_hex(0xCCCCCC)
#define C_TEXT_DIM        lv_color_hex(0x666666)
#define C_TEXT_BRIGHT     lv_color_hex(0xFFFFFF)
#define C_TOGGLE_OFF      lv_color_hex(0x2A2A2A)
#define C_TOGGLE_ON       lv_color_hex(0x8800AA)

// ============================================================
// DIMENZIJE
// ============================================================

#define SCR_W             320
#define SCR_H             480
#define PAD               8
#define SECTION_W         (SCR_W - PAD * 2)

// Toggle
#define TOGGLE_W          (SECTION_W)
#define TOGGLE_H          52

// Efekti 2×3
#define EFF_COLS          3
#define EFF_ROWS          2
#define EFF_GAP           6
#define EFF_BTN_W         ((SECTION_W - EFF_GAP * (EFF_COLS - 1)) / EFF_COLS)
#define EFF_BTN_H         44

// Barve — 7 krogcev
#define CLR_COUNT         7
#define CLR_DOT_SIZE      32
#define CLR_GAP           ((SECTION_W - CLR_COUNT * CLR_DOT_SIZE) / (CLR_COUNT + 1))

// Sliderji
#define SLIDER_H          36
#define SLIDER_ROW_H      52

// Prednastavitve — 2×2
#define PRE_COLS          2
#define PRE_ROWS          2
#define PRE_BTN_W         ((SECTION_W - EFF_GAP) / 2)
#define PRE_BTN_H         44

// ============================================================
// WLED FX IDS (skladno z LCD_UI_Arhitektura.docx sekcija 5.2)
// ============================================================

static const uint8_t EFF_FX_IDS[6] = { 0, 2, 9, 28, 45, 30 };
static const char*   EFF_NAMES[6]  = {
    "Solid", "Breathe", "Rainbow", "Chase", "Twinkle", "Strobe"
};

// ============================================================
// BARVE PALETA (skladno z LCD_UI_Arhitektura.docx sekcija 5.3)
// 7 barv: bela, rdeča, oranžna, rumena, zelena, modra, vijolična
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
// PREDNASTAVITVE (skladno z LCD_UI_Arhitektura.docx sekcija 5.5)
// ============================================================

struct Preset {
    const char* name;
    uint8_t     fx_id;
    uint32_t    color_rgb;   // 0 = auto (WLED ne pošljemo barve)
    uint8_t     brightness;
    uint8_t     speed;
};

static const Preset PRESETS[4] = {
    { "Novo leto", 30, 0xFFFFFF, 220, 200 },   // Strobe, bela
    { "Zabava",    9,  0x000000, 191, 128 },   // Rainbow, auto
    { "Ambient",   2,  0xFF9944, 80,  60  },   // Breathe, toplo bela
    { "Božič",     28, 0xFF0000, 180, 150 },   // Chase, rdeča (zeleno doda WLED)
};

// ============================================================
// WIDGET STRUKTURE
// ============================================================

struct EffectBtn {
    lv_obj_t* btn;
    lv_obj_t* lbl;
    uint8_t   fx_id;
    bool      active;
};

struct ColorDot {
    lv_obj_t* dot;
    uint32_t  rgb;
    bool      selected;
};

struct PresetBtn {
    lv_obj_t* btn;
    lv_obj_t* lbl;
    uint8_t   idx;
    bool      active;
};

// ============================================================
// STATIČNO STANJE
// ============================================================

static bool s_created = false;

// Toggle widget
static lv_obj_t* s_toggle_btn   = nullptr;
static lv_obj_t* s_toggle_lbl   = nullptr;
static lv_obj_t* s_toggle_dot   = nullptr;   // indikator lučka

// Efekti
static EffectBtn s_eff[6]       = {};

// Barve
static ColorDot  s_clr[CLR_COUNT] = {};

// Sliderji
static lv_obj_t* s_slider_bri   = nullptr;
static lv_obj_t* s_lbl_bri_val  = nullptr;
static lv_obj_t* s_slider_spd   = nullptr;
static lv_obj_t* s_lbl_spd_val  = nullptr;

// Prednastavitve
static PresetBtn s_pre[4]       = {};

// Party stanje
static SemaphoreHandle_t s_mutex = nullptr;
static PartyState s_state = {
    false,      // party_on
    0,          // fx_id = Solid
    191,        // brightness
    128,        // speed
    0xFFFFFF,   // color = bela
    0xFF        // preset = brez
};

// ============================================================
// POMOŽNE FUNKCIJE
// ============================================================

static lv_obj_t* make_section_header(lv_obj_t* parent, int y, const char* title) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, C_PARTY_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
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
        digitalWrite(PIN_MUX_SELECT, HIGH);   // PRIMARY → PARTY ESP
        PARTY_I("MUX → PARTY ESP (IO%d HIGH)", PIN_MUX_SELECT);
    } else {
        // Pri OFF: WLED najprej ugasne (HTTP), šele nato MUX nazaj
        // Ker HTTP ni implementiran, MUX vrnemo takoj — posodobi ko bo web_ui.cpp
        // TODO: web_ui.cpp pokliče HTTP {"on":false}, po delay(200) pokliče
        //       screen_party_set_mux_low() ali prek EventBus ACK
        digitalWrite(PIN_MUX_SELECT, LOW);    // PARTY ESP → PRIMARY
        PARTY_I("MUX → PRIMARY ESP (IO%d LOW)", PIN_MUX_SELECT);
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
// EFEKTI
// ============================================================

static void effect_update_visual(uint8_t active_fx_id) {
    for (int i = 0; i < 6; i++) {
        bool act = (EFF_FX_IDS[i] == active_fx_id) && s_state.party_on;
        s_eff[i].active = act;
        lv_obj_set_style_bg_color(s_eff[i].btn,
            act ? C_PARTY_DIM    : C_CARD_BG,   LV_PART_MAIN);
        lv_obj_set_style_border_color(s_eff[i].btn,
            act ? C_PARTY_ACTIVE : C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_eff[i].lbl,
            act ? C_PARTY_ACTIVE : C_TEXT_DIM,   LV_PART_MAIN);
    }
}

static void effect_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    EffectBtn* eff = (EffectBtn*)lv_event_get_user_data(e);
    if (!eff) return;
    if (!s_state.party_on) {
        PARTY_D("Efekt ignoriran — party OFF");
        return;
    }

    s_state.fx_id = eff->fx_id;
    s_state.preset_id = 0xFF;   // prekini prednastavitev
    effect_update_visual(s_state.fx_id);

    EventBus::publish(EventType::BUTTON_PARTY_EFFECT, s_state.fx_id);
    PARTY_D("BUTTON_PARTY_EFFECT fx_id=%d (%s)",
            s_state.fx_id, eff->lbl ? lv_label_get_text(eff->lbl) : "?");

    // TODO: web_ui.cpp subscriber:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_EFFECT, [](const Event& ev) {
    //       wledPost("{\"seg\":[{\"fx\":" + String(ev.payload) + "}]}");
    //   });
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
            sel ? lv_color_white() : lv_color_hex(0x333333), LV_PART_MAIN);
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
    s_state.preset_id = 0xFF;
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
// PREDNASTAVITVE
// ============================================================

static void preset_update_visual(uint8_t active_preset) {
    for (int i = 0; i < 4; i++) {
        bool act = (i == active_preset) && s_state.party_on;
        lv_obj_set_style_bg_color(s_pre[i].btn,
            act ? C_PARTY_DIM    : C_CARD_BG,   LV_PART_MAIN);
        lv_obj_set_style_border_color(s_pre[i].btn,
            act ? C_PARTY_ACTIVE : C_BORDER_OFF, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_pre[i].lbl,
            act ? C_PARTY_ACTIVE : C_TEXT_DIM,   LV_PART_MAIN);
    }
}

static void preset_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    PresetBtn* pb = (PresetBtn*)lv_event_get_user_data(e);
    if (!pb) return;
    if (!s_state.party_on) {
        PARTY_D("Prednastavitev ignorirana — party OFF");
        return;
    }

    uint8_t idx = pb->idx;
    const Preset& p = PRESETS[idx];

    // Posodobi lokalno stanje
    s_state.fx_id      = p.fx_id;
    s_state.brightness = p.brightness;
    s_state.speed      = p.speed;
    s_state.preset_id  = idx;
    if (p.color_rgb != 0x000000) s_state.color_rgb = p.color_rgb;

    // Posodobi vizuale
    preset_update_visual(idx);
    effect_update_visual(s_state.fx_id);
    if (p.color_rgb != 0x000000) color_update_visual(s_state.color_rgb);
    lv_slider_set_value(s_slider_bri, s_state.brightness, LV_ANIM_ON);
    lv_slider_set_value(s_slider_spd, s_state.speed,      LV_ANIM_ON);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_state.brightness);
    lv_label_set_text(s_lbl_bri_val, buf);
    snprintf(buf, sizeof(buf), "%d", s_state.speed);
    lv_label_set_text(s_lbl_spd_val, buf);

    EventBus::publish(EventType::BUTTON_PARTY_PRESET, idx);
    PARTY_I("BUTTON_PARTY_PRESET idx=%d (%s) fx=%d bri=%d spd=%d",
            idx, p.name, p.fx_id, p.brightness, p.speed);

    // TODO: web_ui.cpp subscriber za prednastavitve:
    //   EventBus::subscribe(EventType::BUTTON_PARTY_PRESET, [](const Event& ev) {
    //       const Preset& p = PRESETS[ev.payload];
    //       // Pošlji vse parametre v enem API klicu:
    //       String body = "{\"on\":true,\"bri\":" + String(p.brightness)
    //                   + ",\"seg\":[{\"fx\":" + String(p.fx_id)
    //                   + ",\"sx\":" + String(p.speed);
    //       if (p.color_rgb != 0) {
    //           uint8_t r=(p.color_rgb>>16)&0xFF,
    //                   g=(p.color_rgb>>8)&0xFF,
    //                   b=p.color_rgb&0xFF;
    //           body += ",\"col\":[["+String(r)+","+String(g)+","+String(b)+"]]";
    //       }
    //       body += "}]}";
    //       wledPost(body);
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
    lv_obj_set_size(s_toggle_btn, SECTION_W, TOGGLE_H);
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

    cy += TOGGLE_H + PAD * 2;

    // --------------------------------------------------------
    // EFEKTI — 2×3 grid
    // --------------------------------------------------------
    make_section_header(parent, cy, "EFEKT");
    cy += 18;

    for (int row = 0; row < EFF_ROWS; row++) {
        for (int col = 0; col < EFF_COLS; col++) {
            int idx = row * EFF_COLS + col;
            int x = PAD + col * (EFF_BTN_W + EFF_GAP);
            int y = cy + row * (EFF_BTN_H + EFF_GAP);

            s_eff[idx].fx_id  = EFF_FX_IDS[idx];
            s_eff[idx].active = false;

            s_eff[idx].btn = lv_obj_create(parent);
            lv_obj_set_size(s_eff[idx].btn, EFF_BTN_W, EFF_BTN_H);
            lv_obj_set_pos(s_eff[idx].btn, x, y);
            lv_obj_set_style_bg_color(s_eff[idx].btn, C_CARD_BG, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_eff[idx].btn, C_BORDER_OFF, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_eff[idx].btn, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(s_eff[idx].btn, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_eff[idx].btn, 0, LV_PART_MAIN);
            lv_obj_clear_flag(s_eff[idx].btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_eff[idx].btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_eff[idx].btn, effect_event_cb,
                                LV_EVENT_CLICKED, &s_eff[idx]);

            s_eff[idx].lbl = lv_label_create(s_eff[idx].btn);
            lv_label_set_text(s_eff[idx].lbl, EFF_NAMES[idx]);
            lv_obj_set_style_text_color(s_eff[idx].lbl, C_TEXT_DIM, LV_PART_MAIN);
            lv_obj_set_style_text_font(s_eff[idx].lbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_align(s_eff[idx].lbl, LV_ALIGN_CENTER, 0, 0);
        }
    }
    cy += EFF_ROWS * (EFF_BTN_H + EFF_GAP) + PAD;

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
        lv_obj_set_style_border_color(s_clr[i].dot, lv_color_hex(0x333333), LV_PART_MAIN);
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
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_DIM,    LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_bri, C_PARTY_ACTIVE,  LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_bri, slider_bri_event_cb, LV_EVENT_RELEASED, nullptr);

        s_lbl_bri_val = lv_label_create(row);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", s_state.brightness);
        lv_label_set_text(s_lbl_bri_val, buf);
        lv_obj_set_style_text_color(s_lbl_bri_val, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lbl_bri_val, &lv_font_montserrat_12, LV_PART_MAIN);
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
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_DIM,    LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_spd, C_PARTY_ACTIVE,  LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_spd, slider_spd_event_cb, LV_EVENT_RELEASED, nullptr);

        s_lbl_spd_val = lv_label_create(row);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", s_state.speed);
        lv_label_set_text(s_lbl_spd_val, buf);
        lv_obj_set_style_text_color(s_lbl_spd_val, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lbl_spd_val, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_align(s_lbl_spd_val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    cy += SLIDER_ROW_H + PAD * 2;

    // --------------------------------------------------------
    // PREDNASTAVITVE — 2×2 grid
    // --------------------------------------------------------
    make_section_header(parent, cy, "PREDNASTAVITVE");
    cy += 18;

    for (int row = 0; row < PRE_ROWS; row++) {
        for (int col = 0; col < PRE_COLS; col++) {
            int idx = row * PRE_COLS + col;
            int x = PAD + col * (PRE_BTN_W + EFF_GAP);
            int y = cy + row * (PRE_BTN_H + EFF_GAP);

            s_pre[idx].idx = idx;

            s_pre[idx].btn = lv_obj_create(parent);
            lv_obj_set_size(s_pre[idx].btn, PRE_BTN_W, PRE_BTN_H);
            lv_obj_set_pos(s_pre[idx].btn, x, y);
            lv_obj_set_style_bg_color(s_pre[idx].btn, C_CARD_BG, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_pre[idx].btn, C_BORDER_OFF, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_pre[idx].btn, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(s_pre[idx].btn, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_pre[idx].btn, 0, LV_PART_MAIN);
            lv_obj_clear_flag(s_pre[idx].btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_pre[idx].btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_pre[idx].btn, preset_event_cb,
                                LV_EVENT_CLICKED, &s_pre[idx]);

            s_pre[idx].lbl = lv_label_create(s_pre[idx].btn);
            lv_label_set_text(s_pre[idx].lbl, PRESETS[idx].name);
            lv_obj_set_style_text_color(s_pre[idx].lbl, C_TEXT_DIM, LV_PART_MAIN);
            lv_obj_set_style_text_font(s_pre[idx].lbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_align(s_pre[idx].lbl, LV_ALIGN_CENTER, 0, 0);
        }
    }
    cy += PRE_ROWS * (PRE_BTN_H + EFF_GAP) + PAD * 2;

    lv_obj_set_height(parent, LV_SIZE_CONTENT);

    s_created = true;
    PARTY_I("screen_party_create OK (cy=%d)", cy);
}

// ============================================================
// screen_party_apply_updates — kliče samo lvglTask
// ============================================================

void screen_party_apply_updates() {
    // Trenutno ni pending sistema ker party zaslon ne prejema
    // zunanjih posodobitev (samo oddaja evente).
    // Ko bo web_ui.cpp implementiran in bo znal prebrati WLED stanje
    // (GET /json/state), bo ta funkcija posodobila UI iz WLED odziva.
    //
    // TODO: web_ui.cpp implementira polling GET /json/state vsakih ~5s
    // in pokliče screen_party_set_state() ki postavi dirty flag.
    // screen_party_apply_updates() nato poberemo dirty snapshot in
    // osvežimo vse vizuale (toggle, efekt, barva, bri, spd).
}

// ============================================================
// THREAD-SAFE SETTERJI
// ============================================================

void screen_party_set_state(const PartyState& state) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_state = state;
        xSemaphoreGive(s_mutex);
    }
    // Vizualna posodobitev mora iti v lvglTask — implementiraj dirty flag
    // ko bo web_ui.cpp klical to funkcijo iz drugega taska.
    // Za zdaj: toggle_update_visual() itd. so varni samo iz lvglTask.
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
