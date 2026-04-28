// screen_main.h in screen_service/party se deklarirajo tukaj
// za hal_display.cpp

#pragma once
#include <lvgl.h>
#include "hal_display.h"

// screen_service
void screen_service_create(lv_obj_t* parent);
void screen_service_apply_updates();

// screen_party
void screen_party_create(lv_obj_t* parent);
void screen_party_apply_updates();
