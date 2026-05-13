// ============================================================
// hal_display.cpp — Hardware Abstraction Layer: AXS15231B display + touch
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.2-dev  |  Datum: 2026-04
// Faza    : 0 — ekran + touch (Wire1 neaktiven)
// ============================================================
//
// SPREMEMBE v2.0.2 (glede na v2.0.0):
//   - LVGL buffer: MALLOC_CAP_SPIRAM → MALLOC_CAP_INTERNAL
//     Skladno z Waveshare demo 10_lvgl_arduino_v9 (partial mode):
//       disp_draw_buf1 = heap_caps_malloc(bufSize*2, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
//     Razlog: partial mode buffer je samo 320×40×2 = 25600 B — gre v interni SRAM.
//     Interni RAM je bistveno hitrejši za DMA transfer pri LVGL flush.
//     PSRAM je OK za večje alokacije (DTW matrika, logi), ne za LVGL render buffer.
//   - Dodan fallback: če MALLOC_CAP_INTERNAL spodleti → poskusi MALLOC_CAP_SPIRAM
//     (varnostna mreža če je interni RAM poln)
//
// HARDWARE INICIALIZACIJSKA SEKVENCA (potrjeno iz demo 10_lvgl_arduino_v9.ino):
//   1. Wire.begin(8, 7) — BSP (SDA=IO8, SCL=IO7, potrjeno demo + axp2101 demo)
//   2. TCA9554(0x20, &Wire).begin() — brez parametrov
//   3. TCA9554.pinMode1(1, OUTPUT)
//   4. write1(1,1) → delay(10) → write1(1,0) → delay(10) → write1(1,1) → delay(200)
//   5. bsp_touch_init(&Wire, -1, 0, 320, 480)
//   6. Arduino_ESP32QSPI(CS=12, CLK=5, D0=1, D1=2, D2=3, D3=4)
//   7. Arduino_AXS15231B(bus, -1, 0, false, 320, 480)
//   8. gfx->begin() + fillScreen(BLACK)
//   9. pinMode(6, OUTPUT); digitalWrite(6, HIGH)  — backlight
//  10. lv_init() + lv_tick_set_cb(millis_cb)
//  11. bufSize = 320*40; malloc INTERNAL 25600 B × 2
//  12. lv_display_create + set_flush_cb + set_buffers PARTIAL
//  13. lv_indev_create + set_type POINTER + set_read_cb
//
// ============================================================

#include "hal_display.h"
#include "bsp.h"
#include "config.h"
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_task_wdt.h>

#include "TCA9554.h"
#include "esp_lcd_touch_axs15231b.h"

// AXS15231B QSPI bug: writeCommand(RAMWR via 0x02) resets the quad-path (0x32) write
// position to absolute y=0, ignoring the current RASET window. With PARTIAL mode each
// tile issues RAMWR before writePixels, overwriting y=0..39 repeatedly. Fix: use
// LV_DISPLAY_RENDER_MODE_FULL so writeAddrWindow is called once (y=0..479), RAMWR
// correctly resets to y=0, and writePixels writes the full frame in one pass.
// _currentX=0xFFFF kept to force CASET resend each call (harmless, belt-and-suspenders).
class Arduino_AXS15231B_Fixed : public Arduino_AXS15231B {
public:
    using Arduino_AXS15231B::Arduino_AXS15231B;
    void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override {
        _currentX = 0xFFFF;
        Arduino_AXS15231B::writeAddrWindow(x, y, w, h);
    }
};

#include "screen_service.h"
#include "screen_party.h"
#include "screen_main.h"
#include "light_logic.h"
#include "hal_radar.h"
#include "hal_tof.h"
extern void screen_main_create(lv_obj_t* parent);
extern void screen_main_apply_updates();

#include "logger.h"
#define DISPI(fmt, ...) LOG_INFO ("DISP", fmt, ##__VA_ARGS__)
#define DISPW(fmt, ...) LOG_WARN ("DISP", fmt, ##__VA_ARGS__)
#define DISPE(fmt, ...) LOG_ERROR("DISP", fmt, ##__VA_ARGS__)
#define DISPD(fmt, ...) LOG_DEBUG("DISP", fmt, ##__VA_ARGS__)

#define DISPLAY_TIMEOUT_MS  (5UL * 60UL * 1000UL)

// ============================================================
// STATIČNE SPREMENLJIVKE
// ============================================================

// TCA9554 na internem busu (IO7/IO8) — &Wire eksplicitno
// Demo: TCA9554 TCA(0x20); → privzeto &Wire — enako
static TCA9554 s_tca(TCA9554_ADDR, &Wire);

static Arduino_DataBus* s_bus = nullptr;
static Arduino_GFX*     s_gfx = nullptr;

static lv_display_t* s_lvgl_disp  = nullptr;
static lv_indev_t*   s_lvgl_touch = nullptr;
static uint8_t*      s_buf1       = nullptr;
static uint8_t*      s_buf2       = nullptr;

static lv_obj_t* s_screen_main    = nullptr;
static lv_obj_t* s_screen_service = nullptr;
static lv_obj_t* s_screen_party   = nullptr;

static bool          s_initialized    = false;
static DisplayScreen s_current_screen = DisplayScreen::MAIN;
static uint8_t       s_backlight_val  = 0;
static uint32_t      s_last_touch_ms  = 0;
static uint32_t      s_fps_count      = 0;
static uint32_t      s_fps_last_ms    = 0;
static uint32_t      s_fps_current    = 0;

struct PendingUpdates {
    SsrDisplayData     ssr[4];         bool ssr_dirty[4];
    ParkingDisplayData parking[2];     bool parking_dirty[2];
    RadarDisplayData   radar[4];       bool radar_dirty[4];
    float              lux;
    bool               is_night;       bool lux_dirty;
    uint16_t           tof_mm[6];      bool tof_dirty;
    uint32_t           free_heap, free_psram;
    uint8_t            core0_pct, core1_pct;
    uint32_t           uptime_s;       bool sysstat_dirty;
    DisplayScreen      pending_screen; bool screen_dirty;
};
static PendingUpdates    s_pending   = {};
static SemaphoreHandle_t s_upd_mutex = nullptr;

// ============================================================
// LVGL CALLBACKS
// ============================================================

static uint32_t lvgl_tick_cb() { return millis(); }

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    static uint32_t s_flush_cnt = 0;
    if (s_flush_cnt < 20) {
        DISPI("flush #%lu x1=%d y1=%d x2=%d y2=%d",
              (unsigned long)s_flush_cnt,
              area->x1, area->y1, area->x2, area->y2);
    }
    s_flush_cnt++;
    if (!s_gfx) { lv_display_flush_ready(disp); return; }
    s_gfx->draw16bitRGBBitmap(
        area->x1, area->y1,
        (uint16_t*)px_map,
        lv_area_get_width(area),
        lv_area_get_height(area));
    lv_display_flush_ready(disp);  // ← pravilno za LVGL v9 (v8: lv_disp_flush_ready)
    s_fps_count++;
    uint32_t now = millis();
    if ((now - s_fps_last_ms) >= 1000) {
        s_fps_current = s_fps_count;
        s_fps_count   = 0;
        s_fps_last_ms = now;
    }
}

static void lvgl_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    bsp_touch_read();
    touch_data_t td;
    if (bsp_touch_get_coordinates(&td)) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = td.coords[0].x;
        data->point.y = td.coords[0].y;
        s_last_touch_ms = millis();
        if (s_backlight_val == 0) hal_display_setBacklight(255);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void swipe_cb(lv_event_t* e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    const char* dir_str = (dir == LV_DIR_LEFT)  ? "LEFT"  :
                          (dir == LV_DIR_RIGHT) ? "RIGHT" :
                          (dir == LV_DIR_TOP)   ? "UP"    :
                          (dir == LV_DIR_BOTTOM)? "DOWN"  : "NONE";
    DISPI("swipe: %s  screen=%d", dir_str, (int)s_current_screen);
    if (dir == LV_DIR_LEFT && s_current_screen == DisplayScreen::MAIN) {
        hal_display_showScreen(DisplayScreen::SERVICE);
    } else if (dir == LV_DIR_RIGHT) {
        if (s_current_screen == DisplayScreen::MAIN)
            hal_display_showScreen(DisplayScreen::PARTY);
        else
            hal_display_showScreen(DisplayScreen::MAIN);
    }
}

static void screen_timeout_cb(lv_timer_t*) {
    if (s_backlight_val == 0) return;
    if ((millis() - s_last_touch_ms) >= DISPLAY_TIMEOUT_MS)
        hal_display_setBacklight(0);
}

// ============================================================
// APPLY PENDING UPDATES
// ============================================================

static void apply_pending() {
    if (!s_upd_mutex) return;
    if (xSemaphoreTake(s_upd_mutex, 0) != pdTRUE) return;

    if (s_pending.screen_dirty) {
        DisplayScreen t = s_pending.pending_screen;
        s_pending.screen_dirty = false;
        xSemaphoreGive(s_upd_mutex);
        lv_obj_t* scr = (t == DisplayScreen::MAIN)   ? s_screen_main :
                        (t == DisplayScreen::SERVICE) ? s_screen_service
                                                      : s_screen_party;
        if (scr) {
            lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
            s_current_screen = t;
        }
        return;
    }

    bool need_main = false, need_svc = false;
    for (int i = 0; i < 4; i++) if (s_pending.ssr_dirty[i])     { need_main = true; s_pending.ssr_dirty[i]     = false; }
    for (int i = 0; i < 2; i++) if (s_pending.parking_dirty[i]) { need_main = true; s_pending.parking_dirty[i] = false; }
    for (int i = 0; i < 4; i++) if (s_pending.radar_dirty[i])   { need_main = true; s_pending.radar_dirty[i]   = false; }
    if (s_pending.lux_dirty)     { need_svc = true; }
    if (s_pending.tof_dirty)     { need_svc = true; }
    if (s_pending.sysstat_dirty) { need_svc = true; }

    // Posodobi screen_service lokalni buffer (pod mutex, pred clear dirty flagov)
    if (s_pending.lux_dirty)
        screen_service_update_lux(s_pending.lux, s_pending.is_night);
    if (s_pending.tof_dirty)
        screen_service_update_tof(s_pending.tof_mm);
    if (s_pending.sysstat_dirty)
        screen_service_update_sys(s_pending.free_heap, s_pending.free_psram,
                                   s_pending.core0_pct, s_pending.core1_pct,
                                   s_pending.uptime_s);

    // Zdaj počistimo dirty flage
    if (s_pending.lux_dirty)     s_pending.lux_dirty     = false;
    if (s_pending.tof_dirty)     s_pending.tof_dirty     = false;
    if (s_pending.sysstat_dirty) s_pending.sysstat_dirty = false;

    xSemaphoreGive(s_upd_mutex);

    if (need_main) screen_main_apply_updates();
    if (need_svc)  screen_service_apply_updates();
}

// ============================================================
// UI REFRESH TIMER — Opcija B display polling (dogovorjeno 2026-05)
// ============================================================
// Teče v lvglTask kontekstu (edino varno mesto za LVGL klice).
// Prebere stanje iz light_logic in posodobi vse zaslone.
// 500ms interval: dovolj hiter za countdown prikaz, ne obremeni CPU.
// Ko bodo implementirani novi moduli, SAMO DODAJ klice tukaj —
// ne ustvari novih timerjev ali direktnih klicev iz taskov.
static void ui_refresh_cb(lv_timer_t*) {
    if (!light_logic_ok()) return;

    LightLogicState st = light_logic_get_state();

    // ── SSR gumbi (indeksi 0–3 na zaslonu = SSR 1–4) ──────────────
    for (uint8_t i = 1; i <= 4; i++) {
        SsrDisplayData d;
        switch (st.ssr[i].state) {
            case SsrLogicState::ON_AUTO:
                d.state       = DisplaySsrState::ON;
                d.is_manual   = false;
                d.countdown_s = st.ssr[i].countdown_s;
                break;
            case SsrLogicState::ON_MANUAL:
                d.state       = DisplaySsrState::ON;
                d.is_manual   = true;
                d.countdown_s = st.ssr[i].countdown_s;
                break;
            case SsrLogicState::SSR_DISABLED:
                d.state       = DisplaySsrState::SSR_DISABLED;
                d.countdown_s = 0;
                d.is_manual   = false;
                break;
            default:  // OFF
                d.state       = DisplaySsrState::OFF;
                d.countdown_s = 0;
                d.is_manual   = false;
                break;
        }
        screen_main_set_ssr(i - 1, d);
    }

    // ── Radar arci (4 arci spodaj) — ARC4x v2.2.0 ────────────────────────
    // Preslikava: RadarSensorStatus → RadarArcData
    //
    //   !rs.active                  → CONFIG_ERROR + is_permanent_error=true
    //                                 (senzor se ni inicializiral ob zagonu)
    //   rs.active && !rs.config_ok  → CONFIG_ERROR + is_permanent_error=false
    //                                 (senzor deluje, config je začasno padel)
    //   detection=0                 → INACTIVE (nič ne zaznava)
    //   detection=1 ali 3           → MOVING, dist=moving_dist_cm, energy=moving_energy
    //                                 (detection=3: oboje → prednost MOVING)
    //   detection=2                 → STATIONARY, dist=static_dist_cm, energy=static_energy
    //
    // OPOMBA: peak_duration_ms se posodobi v screen_main_set_radar_arc
    //   iz rs.configured_unmanned_s — ni potrebno posredovati prek RadarArcData.
    // ─────────────────────────────────────────────────────────────────────────────
    for (uint8_t ri = 0; ri < 4; ri++) {
        const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)ri);
        RadarArcData arc = {};

        if (!rs.active) {
            // Trajna napaka: senzor se ni inicializiral ob zagonu
            arc.state              = RadarArcState::CONFIG_ERROR;
            arc.energy             = 100;   // Arc 100% za trajno napako
            arc.dist_cm            = 0;
            arc.is_permanent_error = true;

        } else if (!rs.config_ok) {
            // Začasna napaka: senzor deluje, config je padel
            arc.state              = RadarArcState::CONFIG_ERROR;
            arc.energy             = 50;    // Arc 50% za začasno napako
            arc.dist_cm            = 0;
            arc.is_permanent_error = false;

        } else {
            arc.is_permanent_error = false;
            switch (rs.last_frame.detection) {
                case 1:
                    // Samo gibanje
                    arc.state   = RadarArcState::MOVING;
                    arc.energy  = rs.last_frame.moving_energy;
                    arc.dist_cm = rs.last_frame.moving_dist_cm;
                    break;
                case 2:
                    // Samo statično
                    arc.state   = RadarArcState::STATIONARY;
                    arc.energy  = rs.last_frame.static_energy;
                    arc.dist_cm = rs.last_frame.static_dist_cm;
                    break;
                case 3:
                    // Oboje — prednost MOVING (po ARC4x spec)
                    arc.state   = RadarArcState::MOVING;
                    arc.energy  = rs.last_frame.moving_energy;
                    arc.dist_cm = rs.last_frame.moving_dist_cm;
                    break;
                default:
                    // detection=0: nič ne zaznava
                    arc.state   = RadarArcState::INACTIVE;
                    arc.energy  = 0;
                    arc.dist_cm = 0;
                    break;
            }
        }

        screen_main_set_radar_arc(ri, arc);
    }

    // ── Parking kartici (E2) ──────────────────────────────────────
    // TOF faze + zasedeno/prazno iz hal_tof diagnostike.
    // vehicle_recog je placeholder (Blok F — implementira pozneje).
    {
        TofDiagnostics tof = hal_tof_getDiagnostics();
        for (uint8_t p = 0; p < 2; p++) {
            ParkingDisplayData pk = {};
            pk.occupied      = false;    // Blok F: vehicle_recog_is_occupied(p)
            pk.dtw_distance  = 0.0f;     // Blok F: vehicle_recog_dtw_dist(p)
            pk.parking_count = 0;        // Blok F: vehicle_recog_count(p)
            // TOF faza — katera parkirna mesta je aktivna
            pk.tof_phase     = (uint8_t)tof.current_phase;
            pk.tof_active    = ((uint8_t)tof.active_place == p &&
                                tof.current_phase != TOF_PHASE_IDLE);
            // Horizontalni TOF (razdalja do vozila): H_A = idx 0, H_B = idx 3
            pk.horiz_mm      = (p == 0) ? tof.last_mm[0] : tof.last_mm[3];
            strncpy(pk.vehicle_name, "Prazno", sizeof(pk.vehicle_name) - 1);
            screen_main_set_parking(p, pk);
        }
    }

    // ── Noč/dan indikator (E3.3) ─────────────────────────────────
    screen_main_set_daynight(st.is_night, st.lux);
}

// ============================================================
// TCA9554 TOUCH RESET
// ============================================================

static bool tca9554_touch_reset() {
    DISPI("TCA9554 touch reset (0x%02X pin%d &Wire)...", TCA9554_ADDR, TCA9554_TOUCH_PIN);

    // Demo: TCA.begin(); (brez parametrov) → privzeto INPUT za vse pine
    if (!s_tca.begin()) {
        DISPW("TCA9554 begin() NAPAKA — nefatalno, nadaljujemo");
        return false;
    }
    // Demo: TCA.pinMode1(1, OUTPUT);
    s_tca.pinMode1(TCA9554_TOUCH_PIN, OUTPUT);
    // Demo: TCA.write1(1,1); delay(10); TCA.write1(1,0); delay(10); TCA.write1(1,1); delay(200);
    s_tca.write1(TCA9554_TOUCH_PIN, 1); delay(10);
    s_tca.write1(TCA9554_TOUCH_PIN, 0); delay(10);
    s_tca.write1(TCA9554_TOUCH_PIN, 1); delay(200);
    DISPI("TCA9554 touch reset OK");
    return true;
}

// ============================================================
// hal_display_init
// ============================================================

bool hal_display_init() {
    DISPI("=== hal_display_init ===");
    DISPI("sizeof lv_color_t=%d lv_color16_t=%d", (int)sizeof(lv_color_t), (int)sizeof(uint16_t));

    if (!s_upd_mutex) {
        s_upd_mutex = xSemaphoreCreateMutex();
        if (!s_upd_mutex) { DISPE("Update mutex napaka!"); return false; }
    }

    // 1. TCA9554 touch reset
    tca9554_touch_reset();

    // 2. Arduino_GFX — QSPI bus + AXS15231B display
    // Demo: new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_CLK, D0, D1, D2, D3)
    //       new Arduino_AXS15231B(bus, -1, 0, false, 320, 480)
    DISPI("Arduino_GFX QSPI init CS=%d CLK=%d D0-D3=%d-%d...",
          PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_D0, PIN_LCD_D3);
    s_bus = new Arduino_ESP32QSPI(
        PIN_LCD_CS, PIN_LCD_CLK,
        PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_AXS15231B_Fixed(
        s_bus, -1, 0, false,
        LCD_HOR_RES, LCD_VER_RES,
        0, 0, 0, 0,
        axs15231b_320480_type1_init_operations,
        sizeof(axs15231b_320480_type1_init_operations));
    if (!s_gfx->begin()) {
        DISPE("GFX begin() NAPAKA!");
        delete s_gfx; delete s_bus;
        s_gfx = nullptr; s_bus = nullptr;
        return false;
    }
    s_gfx->fillScreen(RGB565_BLACK);
    DISPI("Arduino_GFX OK (%dx%d)", LCD_HOR_RES, LCD_VER_RES);

    // 3. Touch init
    // Demo: bsp_touch_init(&Wire, -1, 0, 320, 480)
    bsp_touch_init(&Wire, -1, 0, LCD_HOR_RES, LCD_VER_RES);
    DISPI("bsp_touch_init OK");

    // 4. LVGL init
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);
    DISPI("LVGL %d.%d.%d init OK",
          lv_version_major(), lv_version_minor(), lv_version_patch());

    // 5. LVGL buffer — FULL-SCREEN mode v PSRAM
    // AXS15231B QSPI: writeCommand(RAMWR via 0x02) resetira pozicijo quad-write poti na y=0.
    // Pri PARTIAL mode se RAMWR pošlje pred vsakim tilom → vsak tile prepiše y=0..39.
    // FULL mode: en sam writeAddrWindow(0,0,320,480) + en writePixels za celoten zaslon →
    // RAMWR resetira na y=0 kar je točno pravilno izhodišče, brez nadaljnjih resetov.
    // buf_bytes = 320 * 480 * 2 = 307200 B → v PSRAM (interni RAM premajhen)
    uint32_t buf_bytes = (uint32_t)LCD_HOR_RES * LCD_VER_RES * sizeof(uint16_t);
    s_buf1 = (uint8_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf1) {
        DISPE("LVGL full-screen buffer NAPAKA! PSRAM prosto: %lu B",
              (unsigned long)ESP.getFreePsram());
        return false;
    }
    DISPI("LVGL buffer OK (%lu B PSRAM full-screen)", (unsigned long)buf_bytes);

    // 6. LVGL display konfiguracija
    s_lvgl_disp = lv_display_create(LCD_HOR_RES, LCD_VER_RES);
    if (!s_lvgl_disp) { DISPE("lv_display_create napaka!"); return false; }
    lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_lvgl_disp, s_buf1, nullptr, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_FULL);
    DISPI("LVGL buf_bytes=%lu buf1=%p (FULL mode)", (unsigned long)buf_bytes, (void*)s_buf1);

    // 7. Touch indev
    s_lvgl_touch = lv_indev_create();
    if (s_lvgl_touch) {
        lv_indev_set_type(s_lvgl_touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_lvgl_touch, lvgl_touch_cb);
        DISPI("Touch indev OK");
    } else {
        DISPW("lv_indev_create napaka — touch ne bo delal");
    }

    // 8. Zasloni
    DISPI("Kreacija zaslonov...");
    auto make = [](lv_obj_t** scr) -> bool {
        *scr = lv_obj_create(nullptr);
        if (!*scr) return false;
        lv_obj_add_event_cb(*scr, swipe_cb, LV_EVENT_GESTURE, nullptr);
        return true;
    };
    if (!make(&s_screen_main) || !make(&s_screen_service) || !make(&s_screen_party)) {
        DISPE("Screen kreacija napaka!");
        return false;
    }
    screen_main_create(s_screen_main);
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        DISPI("LVGL heap po screen_main: free=%u B frag=%d%%", mon.free_size, mon.frag_pct);
    }
    screen_service_create(s_screen_service);
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        DISPI("LVGL heap po screen_service: free=%u B frag=%d%%",
              mon.free_size, mon.frag_pct);
    }
    screen_party_create(s_screen_party);
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        DISPI("LVGL heap po screen_party: free=%u B frag=%d%%",
              mon.free_size, mon.frag_pct);
    }
    DISPI("Zasloni OK");

    // UI refresh timer — Opcija B polling (light_logic → zaslon)
    // UI_REFRESH_TIMER_MS = 1000ms (config.h, 2026-05) — zmanjšano iz 500ms.
    // Za SSR countdown prikaz je 1s natančnost povsem sprejemljiva.
    lv_timer_create(ui_refresh_cb, UI_REFRESH_TIMER_MS, nullptr);
    DISPI("UI refresh timer OK (%dms)", UI_REFRESH_TIMER_MS);

    // 9. Prikaži glavni zaslon
    lv_scr_load(s_screen_main);
    s_current_screen = DisplayScreen::MAIN;

    // 10. Screen timeout timer
    lv_timer_create(screen_timeout_cb, 10000, nullptr);
    s_last_touch_ms = millis();

    // 11. Backlight ON
    // Demo: pinMode(GFX_BL, OUTPUT); digitalWrite(GFX_BL, HIGH);
    hal_display_setBacklight(255);

    s_fps_last_ms = millis();
    s_initialized = true;
    DISPI("=== hal_display_init OK ===");
    return true;
}

bool hal_display_ok() { return s_initialized; }

// ============================================================
// lvglTask
// ============================================================

void lvglTask(void* pvParams) {
    DISPI("lvglTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    if (!hal_display_init()) {
        DISPE("hal_display_init NAPAKA!");
        while (true) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    DISPI("lvglTask v zanki");
    uint32_t last_ms = millis();
    while (true) {
        esp_task_wdt_reset();
        uint32_t now = millis();
        if ((now - last_ms) >= (uint32_t)LVGL_HANDLER_MS) {
            last_ms = now;
            if (s_initialized) apply_pending();
            // Demo loop(): lv_task_handler() — v9 je lv_timer_handler()
            uint32_t sleep_ms = lv_timer_handler();
            if (sleep_ms > (uint32_t)LVGL_HANDLER_MS) sleep_ms = LVGL_HANDLER_MS;
            vTaskDelay(pdMS_TO_TICKS(sleep_ms > 0 ? sleep_ms : 1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ============================================================
// THREAD-SAFE UI POSODOBITVE
// ============================================================

#define WITH_MUTEX(body) \
    if (s_upd_mutex && xSemaphoreTake(s_upd_mutex, pdMS_TO_TICKS(10)) == pdTRUE) { \
        body; xSemaphoreGive(s_upd_mutex); \
    }

void hal_display_updateSsr(uint8_t i, const SsrDisplayData& d) {
    if (i >= 4) return;
    WITH_MUTEX(s_pending.ssr[i] = d; s_pending.ssr_dirty[i] = true;)
}
void hal_display_updateParking(uint8_t i, const ParkingDisplayData& d) {
    if (i >= 2) return;
    WITH_MUTEX(s_pending.parking[i] = d; s_pending.parking_dirty[i] = true;)
}
void hal_display_updateRadar(uint8_t i, const RadarDisplayData& d) {
    if (i >= 4) return;
    WITH_MUTEX(s_pending.radar[i] = d; s_pending.radar_dirty[i] = true;)
}
void hal_display_updateLux(float lux, bool night) {
    WITH_MUTEX(s_pending.lux = lux; s_pending.is_night = night; s_pending.lux_dirty = true;)
}
void hal_display_updateTof(const uint16_t mm[6]) {
    WITH_MUTEX(memcpy(s_pending.tof_mm, mm, 12); s_pending.tof_dirty = true;)
}
void hal_display_updateSystemStats(uint32_t fh, uint32_t fp,
                                    uint8_t c0, uint8_t c1, uint32_t up) {
    WITH_MUTEX(
        s_pending.free_heap = fh; s_pending.free_psram = fp;
        s_pending.core0_pct = c0; s_pending.core1_pct  = c1;
        s_pending.uptime_s  = up; s_pending.sysstat_dirty = true;
    )
}
void hal_display_showScreen(DisplayScreen scr) {
    WITH_MUTEX(s_pending.pending_screen = scr; s_pending.screen_dirty = true;)
}
DisplayScreen hal_display_getCurrentScreen() { return s_current_screen; }

void hal_display_setBacklight(uint8_t b) {
    s_backlight_val = b;
    // Demo: pinMode(GFX_BL, OUTPUT); digitalWrite(GFX_BL, HIGH);
    // BSP že nastavi PIN_LCD_BL kot OUTPUT — tu samo pišemo vrednost
    digitalWrite(PIN_LCD_BL, b > 0 ? HIGH : LOW);
}
uint8_t  hal_display_getBacklight() { return s_backlight_val; }
uint32_t hal_display_getFps()       { return s_fps_current; }
bool     hal_display_isTouched()    { return (millis() - s_last_touch_ms) < 200; }
