// ============================================================
// screen_party.cpp — Party zaslon (stub)
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

#define SPTY_LOG(fmt, ...) Serial.printf("[SPTY] " fmt "\n", ##__VA_ARGS__)

static lv_obj_t* s_lbl_info = nullptr;
static bool      s_created  = false;

void screen_party_create(lv_obj_t* parent) {
    SPTY_LOG("screen_party_create (stub)");
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0D0D0D), LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_info = lv_label_create(parent);
    lv_label_set_text(s_lbl_info,
        "PARTY ZASLON\n\n"
        "Swipe levo za Glavni\n\n"
        "Faza 3: WLED kontrola\n"
        "- Vklop / izklop\n"
        "- 6 efektov\n"
        "- 7 barv\n"
        "- Svetlost, hitrost\n"
        "- 4 prednastavitve");
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_info, LV_ALIGN_CENTER, 0, 0);
    s_created = true;
}

void screen_party_apply_updates() {
    // Stub — implementiraj v Fazi 3
}
