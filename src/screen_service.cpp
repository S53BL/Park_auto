// ============================================================
// screen_service.cpp — Servisni zaslon (stub)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.1-dev  |  Datum: 2026-04
// Faza    : 0 — stub, kompajlira se
//
// POPRAVEK v2.0.1:
//   - Dodan #include <Arduino.h> — Serial ni definiran brez njega.
//     lvgl.h ne vključuje Arduino headrov.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>

#define SSVC_LOG(fmt, ...) Serial.printf("[SSVC] " fmt "\n", ##__VA_ARGS__)

static lv_obj_t* s_lbl_info = nullptr;
static bool      s_created  = false;

void screen_service_create(lv_obj_t* parent) {
    SSVC_LOG("screen_service_create (stub)");
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0D0D0D), LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_info = lv_label_create(parent);
    lv_label_set_text(s_lbl_info,
        "SERVISNI ZASLON\n\n"
        "Swipe desno za Glavni\n\n"
        "Faza 1: diagnostika\n"
        "- lux / dan-noc\n"
        "- fotocelice, rampa\n"
        "- TOF razdalje\n"
        "- I2C health\n"
        "- heap / PSRAM");
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_info, LV_ALIGN_CENTER, 0, 0);
    s_created = true;
}

void screen_service_apply_updates() {
    // Stub — implementiraj v Fazi 3
}
