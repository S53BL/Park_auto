// ============================================================
// web_ui.cpp — Web UI implementacija (REST API + statične datoteke)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 3.4.0  |  Datum: 2026-05
// ============================================================
//
// ⚠ Ne zamenjuj z sinhronim WebServer. Razlog:
//   Moderni brskalnik odpre 6 vzporednih HTTP/1.1 konekcij ob nalaganju.
//   Sinhroni handleClient() ne more servirati 6 konekcij hkrati →
//   LwIP jih drži v SRAM → 6 × ~3 KB > 13 KB prostega SRAM → ECONNRESET.
//   To je strukturna inkompatibilnost, ne bug. Ref: ram_problem3.md.
//
// AsyncTCP procesira vsako konekcijo takoj kot callback — brez kopičenja.
// SD midnight flush (sd_midnight_flush.cpp) preprečuje DMA spike ob polnoči.
//
// HANDLER LOGIKA: _server.arg() / _server.send() → AsyncWebServerRequest* req
// ============================================================

#include "web_ui.h"
#include "alarm.h"
#include "signal_led.h"
#include "logger.h"
#include "sd_mgr.h"
#include "wifi_manager.h"
#include "config.h"
#include "config_mgr.h"
#include "hal_radar.h"
#include "hal_gpio.h"
#include "hal_tof.h"
#include "hal_light.h"
#include "light_logic.h"
#include "event_bus.h"
#include "vehicle_recog.h"

#include "led_manager.h"
#include "screen_party.h"
#include "screen_main.h"    // screen_main_set_ssr_label — live update po spremembi

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>

// ============================================================
// SECTION 1 — GLOBALNE SPREMENLJIVKE
// ============================================================

static const char* TAG = "WEBUI";

static AsyncWebServer* _server          = nullptr;
static bool            _server_running  = false;
static bool            _littlefs_ok     = false;
static bool            _assets_ok       = false;

static WebUiStats      _stats = {};

struct PsramAllocator : public ArduinoJson::Allocator {
    void* allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* ptr)  override { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};
static PsramAllocator s_psram_alloc;
static char*          _json_buf    = nullptr;
static const size_t   _json_buf_sz = 8192;

static uint8_t*        _index_html_buf   = nullptr;  // PSRAM cache za SPA fallback
static size_t          _index_html_sz    = 0;
static SemaphoreHandle_t _json_buf_mutex = nullptr;  // serializa sočasne _sendJson klice

// PSRAM cache za statične assete — brez LittleFS fopen() per-request.
// Brez tega: 6 vzporednih HTTP konekcij brskalnika × ~3 KB AsyncFileResponse =
// ~18 KB SRAM → OOM → konekcije se resetirajo, JS datoteke ne naložijo.
struct AssetEntry {
    const char* url;
    const char* mime;
    uint8_t*    buf;
    size_t      sz;
};
static AssetEntry s_assets[] = {
    { "/settings.js",    "application/javascript", nullptr, 0 },
    { "/alarm.js",       "application/javascript", nullptr, 0 },
    { "/party.js",       "application/javascript", nullptr, 0 },
    { "/alpine.min.js",  "application/javascript", nullptr, 0 },
    { "/style.css",      "text/css",               nullptr, 0 },
    { "/diagnostika.js", "application/javascript", nullptr, 0 },
    { "/logs.js",        "application/javascript", nullptr, 0 },
    { "/system.js",      "application/javascript", nullptr, 0 },
    { "/vehicles.js",    "application/javascript", nullptr, 0 },
};
static constexpr int ASSET_COUNT = (int)(sizeof(s_assets) / sizeof(s_assets[0]));

// ============================================================
// WLED COMMAND QUEUE (A2/A4)
// ============================================================

enum class WledCmdType : uint8_t {
    TOGGLE,      // payload: 1=on, 0=off
    EFFECT,      // payload: fx_id
    COLOR,       // payload: (R<<16)|(G<<8)|B
    BRIGHTNESS,  // payload: 0-255
    SPEED,       // payload: 0-255
    SLOT,        // payload: slot_idx 0-8; bere PartySlot iz config_mgr
    SUSPEND,     // party prekinjen — pošlje {"on":false} in vrne MUX
    RESUME,      // party nadaljuje — MUX že HIGH; pošlje samo {"on":true}
};

struct WledCmd {
    WledCmdType type;
    uint32_t    payload;
};

static QueueHandle_t s_wled_q    = nullptr;
static TaskHandle_t  s_wled_task = nullptr;
static bool          s_wled_on   = false;    // mirror: je WLED prižgan
static uint8_t       s_active_slot = 0xFF;   // kateri slot je aktiven (0xFF = custom)

// ============================================================
// SECTION 2 — POMOŽNE FUNKCIJE
// ============================================================

static void _sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    _stats.req_errors++;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    req->send(code, "application/json", buf);
}

// Pošlje JsonDocument prek statičnega PSRAM bufferja — brez cbuf verige v SRAM.
// _json_buf je 8 KB v PSRAM; beginResponse() naredi String kopijo takojšnje (data safe po klicu).
// Mutex serializa sočasne klice (AsyncTCP multi-conn scenarij).
static void _sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
    if (!_json_buf) { _sendError(req, 503, "json buf not initialized"); return; }
    if (_json_buf_mutex) xSemaphoreTake(_json_buf_mutex, portMAX_DELAY);
    size_t len = serializeJson(doc, _json_buf, _json_buf_sz);
    AsyncWebServerResponse* resp = req->beginResponse(code, "application/json", _json_buf);
    if (_json_buf_mutex) xSemaphoreGive(_json_buf_mutex);
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
    LOG_DEBUG(TAG, "HTTP %d → %s [%u B] | SRAM: %lu B",
              code, req->url().c_str(), (unsigned)len,
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void _addCorsHeaders(AsyncWebServerResponse* resp) {
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/status/*
// ============================================================

static void _handleStatusLight(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    JsonDocument doc(&s_psram_alloc);

    JsonArray ssrArr = doc["ssr"].to<JsonArray>();
    if (light_logic_ok()) {
        LightLogicState ll = light_logic_get_state();
        for (uint8_t i = 1; i <= 4; i++) {
            JsonObject s = ssrArr.add<JsonObject>();
            s["id"]          = i;
            s["on"]          = (ll.ssr[i].state == SsrLogicState::ON_AUTO ||
                                ll.ssr[i].state == SsrLogicState::ON_MANUAL);
            s["countdown_s"] = ll.ssr[i].countdown_s;
            s["disabled"]    = ll.ssr[i].disabled;
            const char* ss;
            switch (ll.ssr[i].state) {
                case SsrLogicState::ON_AUTO:      ss = "ON_AUTO";   break;
                case SsrLogicState::ON_MANUAL:    ss = "ON_MANUAL"; break;
                case SsrLogicState::SSR_DISABLED: ss = "DISABLED";  break;
                default:                          ss = "OFF";       break;
            }
            s["state_str"] = ss;
        }
    } else {
        for (uint8_t i = 1; i <= 4; i++) {
            JsonObject s = ssrArr.add<JsonObject>();
            s["id"] = i; s["on"] = false; s["countdown_s"] = 0;
            s["disabled"] = false; s["state_str"] = "UNKNOWN";
        }
    }

    // Lux + noč/dan (za diagnostiko na spletni strani)
    if (light_logic_ok()) {
        LightLogicState st = light_logic_get_state();
        doc["lux"]      = (double)st.lux;
        doc["is_night"] = st.is_night;
    }

    GpioState gpio = hal_gpio_get_state();
    if (gpio.rampaluc)      doc["ramp"] = "moving";
    else if (gpio.rampagor) doc["ramp"] = "up";
    else                    doc["ramp"] = "down";
    doc["door"] = gpio.vrataod ? "open" : "closed";

    // Alarm stanje
    if (alarm_ok()) {
        AlarmState as = alarm_get_state();
        JsonObject alarmObj = doc["alarm"].to<JsonObject>();
        alarmObj["active"] = as.active;
        const char* astate = "OFF";
        if (as.state == AlarmStateEnum::ARMED)     astate = "ARMED";
        if (as.state == AlarmStateEnum::TRIGGERED) astate = "TRIGGERED";
        alarmObj["state"]           = astate;
        alarmObj["callback_url_set"] = as.callback_url_set;
    }

    JsonArray parkArr = doc["parking"].to<JsonArray>();
    const char* vrStateStr[4] = {"EMPTY_CAL","EMPTY_UNCAL","OCC_UNKNOWN","OCC_KNOWN"};
    for (uint8_t p = 0; p < 2; p++) {
        char pid = (p == 0) ? 'A' : 'B';
        JsonObject pk = parkArr.add<JsonObject>();
        pk["place"]        = (p == 0) ? "A" : "B";
        vr_place_state_t vrs = vehicle_recog_get_state(pid);
        pk["occupied"]     = (vrs == VR_STATE_OCCUPIED_KNOWN ||
                              vrs == VR_STATE_OCCUPIED_UNKNOWN);
        pk["vehicle_name"] = vehicle_recog_get_vehicle_name(pid);
        pk["vr_state_str"] = vrStateStr[(uint8_t)vrs];
        TofDiagnostics tofDiag = hal_tof_getDiagnostics();
        bool tof_active = ((uint8_t)tofDiag.active_place == p &&
                           tofDiag.current_phase != TOF_PHASE_IDLE);
        pk["tof_active"] = tof_active;
        const char* phaseStr[4] = {"IDLE","DETECT","SCANNING","DTW_WAIT"};
        pk["tof_phase_str"] = phaseStr[(uint8_t)tofDiag.current_phase];
    }

    _sendJson(req, 200, doc);
}

static void _handleStatusSensors(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    JsonDocument doc(&s_psram_alloc);

    TofDiagnostics tofDiag = hal_tof_getDiagnostics();
    const char* tofNames[6] = {"H_A","P1_A","P2_A","H_B","P1_B","P2_B"};
    JsonArray tofArr = doc["tof"].to<JsonArray>();
    for (uint8_t i = 0; i < 6; i++) {
        JsonObject t = tofArr.add<JsonObject>();
        t["id"]      = i;
        t["name"]    = tofNames[i];
        t["ok"]      = tofDiag.sensor_ok[i];
        t["dist_mm"] = tofDiag.sensor_ok[i] ? tofDiag.last_mm[i] : TOF_ERR;
        t["errors"]  = tofDiag.error_count[i];
    }

    const char* radarNames[4] = {"Vhod","Cesta_L","Cesta_D","Garaza"};
    const char* detStr[4]     = {"absent","moving","static","both"};
    JsonArray radarArr = doc["radar"].to<JsonArray>();
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)i);
        JsonObject r = radarArr.add<JsonObject>();
        r["id"]             = i;
        r["name"]           = radarNames[i];
        r["active"]         = rs.active;
        uint8_t det = rs.last_frame.detection;
        r["detection"]      = det < 4 ? detStr[det] : "unknown";
        r["moving_dist_cm"] = rs.last_frame.moving_dist_cm;
        r["moving_energy"]  = rs.last_frame.moving_energy;
        r["static_dist_cm"] = rs.last_frame.static_dist_cm;
        r["static_energy"]  = rs.last_frame.static_energy;
        r["frames_ok"]      = rs.frames_ok;
        r["errors"]         = rs.parse_errors + rs.i2c_errors;
    }

    GpioState gpio = hal_gpio_get_state();
    JsonArray cellsArr = doc["cells"].to<JsonArray>();
    JsonObject c1 = cellsArr.add<JsonObject>();
    c1["id"] = 1; c1["name"] = "zunanja"; c1["broken"] = gpio.celica1;
    JsonObject c2 = cellsArr.add<JsonObject>();
    c2["id"] = 2; c2["name"] = "notranja"; c2["broken"] = gpio.celica2;

    _sendJson(req, 200, doc);
}

static void _handleStatusSystem(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    JsonDocument doc(&s_psram_alloc);

    doc["free_sram"]       = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doc["min_free_sram"]   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doc["free_psram"]      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    doc["min_free_psram"]  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    doc["config_ok"]       = config_mgr_ok();
    doc["config_replaced"] = (uint8_t)config_mgr_replaced_count();

    WifiStatus wifi = wifi_manager_get_status();
    JsonObject w = doc["wifi"].to<JsonObject>();
    w["connected"]  = wifi_manager_is_connected();
    w["ip"]         = wifi.ip_str;
    w["ssid"]       = wifi.ssid;
    w["rssi"]       = wifi.rssi;
    w["ntp_ok"]     = wifi.ntp_ok;
    w["ntp_time"]   = wifi.ntp_time;
    w["uptime_ms"]  = millis();
    w["reconnects"] = wifi.reconnect_count;

    JsonObject sd = doc["sd"].to<JsonObject>();
    sd["ready"]    = sd_mgr_ready();
    sd["status"]   = sd_mgr_status_str();
    sd["total_mb"] = (uint32_t)(sd_mgr_total_bytes()  / (1024ULL * 1024ULL));
    sd["free_mb"]  = (uint32_t)(sd_mgr_free_bytes()   / (1024ULL * 1024ULL));

    LoggerStats ls = logger_get_stats();
    JsonObject lg = doc["logger"].to<JsonObject>();
    lg["total_lines"]   = ls.total_lines;
    lg["dropped_lines"] = ls.dropped_lines;
    lg["sd_flushes"]    = ls.sd_flush_count;

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"]    = VERSION_STRING;
    fw["uptime_s"]   = millis() / 1000UL;
    const esp_app_desc_t* app = esp_app_get_description();
    if (app) {
        fw["idf_ver"]    = app->idf_ver;
        fw["build_date"] = app->date;
        fw["build_time"] = app->time;
    }

    JsonObject wu = doc["webui"].to<JsonObject>();
    wu["req_total"]    = _stats.req_total;
    wu["req_api"]      = _stats.req_api;
    wu["req_files"]    = _stats.req_files;
    wu["req_errors"]   = _stats.req_errors;
    wu["ota_attempts"] = _stats.ota_attempts;
    wu["ota_success"]  = _stats.ota_success;
    wu["littlefs_ok"]  = _littlefs_ok;
    wu["assets_ok"]    = _assets_ok;

    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/logs
// ============================================================

static void _handleLogsGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    uint16_t max_lines = WEB_LOG_MAX_LINES;
    if (req->hasParam("lines")) {
        int n = req->getParam("lines")->value().toInt();
        if (n > 0 && n <= 1000) max_lines = (uint16_t)n;
    }

    // PSRAM — brez SRAM fallback (PSRAM je v izobilju, SRAM je dragocen).
    char* buf = (char*)heap_caps_malloc(WEB_LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) { _sendError(req, 503, "out of memory"); return; }

    size_t len = logger_get_recent(buf, WEB_LOG_BUF_SIZE - 1, max_lines);
    buf[len] = '\0';

    // Strežemo text/plain direktno iz PSRAM — brez JSON enkodiranja, brez drugega bufferja.
    // Filtriranje, parsiranje in rendering opravi brskalnik.
    // X-Log-Total: monotoni števec → brskalnik ve točno koliko novih vrstic appenda (O(1) cursor).
    // AwsResponseFiller: direkten memcpy PSRAM → TCP buffer, brez String kopije v SRAM.
    uint32_t total = logger_total_lines();
    AsyncWebServerResponse* resp = req->beginResponse(
        "text/plain; charset=utf-8", len,
        [buf, len](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            if (index >= len) { free(buf); return 0; }
            size_t n = (len - index < maxLen) ? (len - index) : maxLen;
            memcpy(out, (const uint8_t*)buf + index, n);
            return n;
        });
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("X-Log-Total", String(total).c_str());
    req->send(resp);
}

static void _handleLogsFlush(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/logs/flush — manual flush");
    logger_flush();
    JsonDocument doc(&s_psram_alloc);
    doc["ok"]  = true;
    doc["msg"] = "flushed";
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/config
// ============================================================

static void _handleConfigGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    JsonDocument doc(&s_psram_alloc);
    const Config& cfg = config_get();

    JsonObject light = doc["light"].to<JsonObject>();
    light["timeout_ssr1_s"]      = cfg.timeout_ssr1_s;
    light["manual_extend_min"]   = cfg.manual_extend_min;
    light["antiforgot_ssr2_min"] = cfg.antiforgot_ssr2_min;
    light["antiforgot_ssr3_min"] = cfg.antiforgot_ssr3_min;
    light["ssr2_auto_night"]     = cfg.ssr2_auto_night;
    light["dnd_start_h"]        = cfg.dnd_start_h;
    light["dnd_end_h"]          = cfg.dnd_end_h;
    light["ssr2_dnd_disable"]   = cfg.ssr2_dnd_disable;
    light["brightness_night"]   = cfg.brightness_night;
    light["lux_threshold"]       = cfg.lux_night;
    light["lux_day"]             = cfg.lux_day;
    JsonArray ssr_lbl = light["ssr_labels"].to<JsonArray>();
    for (int i = 0; i < 4; i++) ssr_lbl.add(cfg.ssr_label[i]);

    JsonObject led = doc["led"].to<JsonObject>();
    led["fill_speed_ms"]       = cfg.fill_speed_ms;
    led["unfill_speed_ms"]     = cfg.unfill_speed_ms;
    led["fade_duration_ms"]    = cfg.fade_duration_ms;
    led["target_brightness"]   = cfg.target_brightness;
    led["ssr2_delay_ms"]       = cfg.ssr2_delay_ms;
    led["pa_thresh1_mm"]       = cfg.pa_thresh_green_mm;
    led["pa_thresh2_mm"]       = cfg.pa_thresh_orange_mm;
    led["pa_thresh3_mm"]       = cfg.pa_thresh_red_mm;
    led["pa_stability_s"]      = cfg.pa_stability_s;
    led["photocell_timer_min"] = cfg.photocell_timer_min;
    led["clock_duration_s"]    = cfg.clock_duration_s;

    JsonObject ident = doc["ident"].to<JsonObject>();
    ident["dtw_threshold"]          = cfg.dtw_threshold;
    ident["sakoe_radius"]           = cfg.sakoe_radius;
    ident["min_profile_points"]     = cfg.min_profile_points;
    ident["normalize_points"]       = cfg.normalize_points;
    ident["delta_filter_mm"]        = cfg.delta_filter_mm;
    ident["phase_confirm_cm"]       = cfg.phase_confirm_cm;
    ident["stability_s"]            = cfg.stability_s;
    ident["raw_profiles_per_model"] = cfg.raw_profiles_per_model;
    ident["presence_check_min"]     = cfg.presence_check_min;
    ident["empty_tolerance_mm"]     = cfg.empty_tolerance_mm;

    _sendJson(req, 200, doc);
}

static void _handleConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;  // čakaj na vse chunke

    LOG_INFO(TAG, "POST /api/config, body=%u bytes", (unsigned)total);

    JsonDocument doc(&s_psram_alloc);
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        _sendError(req, 400, "invalid JSON");
        return;
    }

    // Config (~910B) alociramo na PSRAM — async_tcp stack je samo 4096B
    Config* cfg = (Config*)heap_caps_malloc(sizeof(Config), MALLOC_CAP_SPIRAM);
    if (!cfg) { _sendError(req, 500, "OOM"); return; }
    *cfg = config_get();

    if (doc["light"].is<JsonObject>()) {
        JsonObject l = doc["light"];
        if (l["timeout_ssr1_s"].is<uint32_t>())      cfg->timeout_ssr1_s      = l["timeout_ssr1_s"];
        if (l["manual_extend_min"].is<uint32_t>())    cfg->manual_extend_min   = l["manual_extend_min"];
        if (l["antiforgot_ssr2_min"].is<uint32_t>())  cfg->antiforgot_ssr2_min = l["antiforgot_ssr2_min"];
        if (l["antiforgot_ssr3_min"].is<uint32_t>())  cfg->antiforgot_ssr3_min = l["antiforgot_ssr3_min"];
        if (l["ssr2_auto_night"].is<bool>())           cfg->ssr2_auto_night     = l["ssr2_auto_night"];
        if (l["dnd_start_h"].is<uint8_t>())            cfg->dnd_start_h         = l["dnd_start_h"];
        if (l["dnd_end_h"].is<uint8_t>())              cfg->dnd_end_h           = l["dnd_end_h"];
        if (l["ssr2_dnd_disable"].is<bool>())          cfg->ssr2_dnd_disable    = l["ssr2_dnd_disable"];
        if (l["brightness_night"].is<uint8_t>())       cfg->brightness_night    = l["brightness_night"];
        if (l["lux_threshold"].is<uint32_t>())         cfg->lux_night           = l["lux_threshold"];
        if (l["lux_day"].is<uint32_t>())               cfg->lux_day             = l["lux_day"];
        // SSR labeli — validacija: string, dolžina 1–23 znakov
        if (l["ssr_labels"].is<JsonArray>()) {
            JsonArray arr = l["ssr_labels"].as<JsonArray>();
            for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
                if (!arr[i].is<const char*>()) continue;
                const char* lbl = arr[i].as<const char*>();
                size_t llen = lbl ? strlen(lbl) : 0;
                if (llen == 0 || llen >= 24) continue;
                strncpy(cfg->ssr_label[i], lbl, 23);
                cfg->ssr_label[i][23] = '\0';
            }
        }
    }
    if (doc["led"].is<JsonObject>()) {
        JsonObject a = doc["led"];
        if (a["fill_speed_ms"].is<uint32_t>())        cfg->fill_speed_ms        = a["fill_speed_ms"];
        if (a["unfill_speed_ms"].is<uint32_t>())      cfg->unfill_speed_ms      = a["unfill_speed_ms"];
        if (a["fade_duration_ms"].is<uint32_t>())     cfg->fade_duration_ms     = a["fade_duration_ms"];
        if (a["target_brightness"].is<uint8_t>())     cfg->target_brightness    = a["target_brightness"];
        if (a["ssr2_delay_ms"].is<uint32_t>())        cfg->ssr2_delay_ms        = a["ssr2_delay_ms"];
        if (a["pa_thresh1_mm"].is<uint32_t>())        cfg->pa_thresh_green_mm   = a["pa_thresh1_mm"];
        if (a["pa_thresh2_mm"].is<uint32_t>())        cfg->pa_thresh_orange_mm  = a["pa_thresh2_mm"];
        if (a["pa_thresh3_mm"].is<uint32_t>())        cfg->pa_thresh_red_mm     = a["pa_thresh3_mm"];
        if (a["pa_stability_s"].is<uint32_t>())       cfg->pa_stability_s       = a["pa_stability_s"];
        if (a["photocell_timer_min"].is<uint32_t>())  cfg->photocell_timer_min  = a["photocell_timer_min"];
        if (a["clock_duration_s"].is<uint32_t>())     cfg->clock_duration_s     = a["clock_duration_s"];
    }
    if (doc["ident"].is<JsonObject>()) {
        JsonObject i = doc["ident"];
        if (i["dtw_threshold"].is<float>())            cfg->dtw_threshold           = i["dtw_threshold"];
        if (i["sakoe_radius"].is<uint8_t>())           cfg->sakoe_radius            = i["sakoe_radius"];
        if (i["min_profile_points"].is<uint8_t>())     cfg->min_profile_points      = i["min_profile_points"];
        if (i["normalize_points"].is<uint8_t>())       cfg->normalize_points        = i["normalize_points"];
        if (i["delta_filter_mm"].is<uint32_t>())       cfg->delta_filter_mm         = i["delta_filter_mm"];
        if (i["phase_confirm_cm"].is<uint32_t>())      cfg->phase_confirm_cm        = i["phase_confirm_cm"];
        if (i["stability_s"].is<float>())              cfg->stability_s             = i["stability_s"];
        if (i["raw_profiles_per_model"].is<uint8_t>()) cfg->raw_profiles_per_model  = i["raw_profiles_per_model"];
        if (i["presence_check_min"].is<uint8_t>())     cfg->presence_check_min      = i["presence_check_min"];
        if (i["empty_tolerance_mm"].is<uint32_t>())    cfg->empty_tolerance_mm      = i["empty_tolerance_mm"];
    }

    config_set(*cfg);
    heap_caps_free(cfg);
    bool saved = config_save();
    vehicle_recog_on_config_changed();
    // SSR labeli se posodobijo avtomatično v ui_refresh_cb (LVGL task, ~1s)
    // prek screen_main_set_ssr() ki primerja config label z dejanskim.
    LOG_INFO(TAG, "Config posodobljen in shranjen (ok=%d)", (int)saved);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]  = true;
    resp["msg"] = saved ? "saved to NVS" : "saved to RAM only (NVS error)";
    _sendJson(req, 200, resp);
}

static void _handleConfigReset(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/config/reset");
    config_reset_defaults();
    JsonDocument doc(&s_psram_alloc);
    doc["ok"]  = true;
    doc["msg"] = "config reset to defaults";
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3b — HANDLER-JI: /api/radar
// ============================================================

static void _handleRadarGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());
    JsonDocument doc(&s_psram_alloc);
    JsonArray sensors = doc["sensors"].to<JsonArray>();

    const char* names[4] = {"Vhod","Cesta_L","Cesta_D","Garaza"};
    const Config& cfg = config_get();

    for (uint8_t i = 0; i < 4; i++) {
        const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)i);
        JsonObject s = sensors.add<JsonObject>();
        s["id"]              = i;
        s["name"]            = names[i];
        s["active"]          = rs.active;
        s["config_ok"]       = rs.config_ok;
        s["config_verified"] = rs.config_verified;
        s["detection"]       = rs.last_frame.detection;
        s["dist_cm"]         = rs.last_frame.detect_dist_cm;
        s["moving_dist_cm"]  = rs.last_frame.moving_dist_cm;
        s["static_dist_cm"]  = rs.last_frame.static_dist_cm;
        s["move_energy"]     = rs.last_frame.moving_energy;
        s["static_energy"]   = rs.last_frame.static_energy;
        s["frames_ok"]       = rs.frames_ok;
        s["errors"]          = rs.parse_errors + rs.i2c_errors;
        s["max_dist"]        = rs.configured_max_dist;
        s["move_sens"]       = rs.configured_move_sens;
        s["static_sens"]     = rs.configured_static_sens;
        s["unmanned_s"]      = rs.configured_unmanned_s;
        s["cfg_max_dist"]    = cfg.radar_max_dist[i];
        s["cfg_move_sens"]   = cfg.radar_move_sens[i];
        s["cfg_static_sens"] = cfg.radar_static_sens[i];
        s["cfg_unmanned_s"]  = cfg.radar_unmanned_s[i];
    }
    doc["persistence_n"]        = cfg.radar_persistence_n;
    doc["poll_interval_ms"]     = cfg.radar_poll_interval_ms;
    doc["max_consec_overflows"] = cfg.radar_max_consec_overflows;
    _sendJson(req, 200, doc);
}

static void _handleRadarConfigBody(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index != 0) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "neveljaven JSON");
        return;
    }

    // Config (~910B) alociramo na PSRAM — async_tcp stack je samo 4096B
    Config* cfg = (Config*)heap_caps_malloc(sizeof(Config), MALLOC_CAP_SPIRAM);
    if (!cfg) { _sendError(req, 500, "OOM"); return; }
    *cfg = config_get();

    bool global_changed = false;

    if (doc["persistence_n"].is<int>()) {
        uint8_t pn = (uint8_t)doc["persistence_n"].as<int>();
        if (pn > 10) { heap_caps_free(cfg); _sendError(req, 400, "persistence_n izven obsega (0-10)"); return; }
        cfg->radar_persistence_n = pn;
        global_changed = true;
    }
    if (doc["poll_interval_ms"].is<int>()) {
        uint32_t piv = (uint32_t)doc["poll_interval_ms"].as<int>();
        if (piv < RADAR_POLL_INTERVAL_MIN_MS || piv > RADAR_POLL_INTERVAL_MAX_MS) {
            heap_caps_free(cfg); _sendError(req, 400, "poll_interval_ms izven obsega (10-100)"); return;
        }
        cfg->radar_poll_interval_ms = piv;
        global_changed = true;
    }
    if (doc["max_consec_overflows"].is<int>()) {
        uint32_t mcov = (uint32_t)doc["max_consec_overflows"].as<int>();
        if (mcov < 1 || mcov > 100) {
            heap_caps_free(cfg); _sendError(req, 400, "max_consec_overflows izven obsega (1-100)"); return;
        }
        cfg->radar_max_consec_overflows = mcov;
        global_changed = true;
    }

    if (global_changed) {
        config_set(*cfg);
        config_save();
        JsonDocument resp(&s_psram_alloc);
        resp["ok"]                   = true;
        resp["persistence_n"]        = cfg->radar_persistence_n;
        resp["poll_interval_ms"]     = cfg->radar_poll_interval_ms;
        resp["max_consec_overflows"] = cfg->radar_max_consec_overflows;
        heap_caps_free(cfg);
        _sendJson(req, 200, resp);
        return;
    }

    if (!doc["sensor"].is<int>()) {
        heap_caps_free(cfg); _sendError(req, 400, "sensor manjka"); return;
    }
    uint8_t sid = (uint8_t)doc["sensor"].as<int>();
    if (sid >= 4) { heap_caps_free(cfg); _sendError(req, 400, "sensor izven obsega"); return; }

    if (doc["max_dist"].is<int>())    cfg->radar_max_dist[sid]    = (uint8_t)doc["max_dist"].as<int>();
    if (doc["move_sens"].is<int>())   cfg->radar_move_sens[sid]   = (uint8_t)doc["move_sens"].as<int>();
    if (doc["static_sens"].is<int>()) cfg->radar_static_sens[sid] = (uint8_t)doc["static_sens"].as<int>();
    if (doc["unmanned_s"].is<int>())  cfg->radar_unmanned_s[sid]  = (uint16_t)doc["unmanned_s"].as<int>();

    if (cfg->radar_max_dist[sid] > 8 || cfg->radar_move_sens[sid] > 100 ||
        cfg->radar_static_sens[sid] > 100) {
        heap_caps_free(cfg); _sendError(req, 400, "vrednost izven obsega"); return;
    }

    config_set(*cfg);
    config_save();

    bool reconfig_ok = hal_radar_reconfigure(
        (RadarSensorId)sid,
        cfg->radar_max_dist[sid],
        cfg->radar_move_sens[sid],
        cfg->radar_static_sens[sid],
        cfg->radar_unmanned_s[sid]
    );

    const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)sid);
    JsonDocument resp(&s_psram_alloc);
    resp["ok"]              = true;
    resp["sensor"]          = sid;
    resp["config_ok"]       = rs.config_ok;
    resp["config_verified"] = rs.config_verified;
    if (!reconfig_ok) resp["warn"] = "konfiguracija na radar ni uspela — bo poskusil ob restartu";
    heap_caps_free(cfg);
    _sendJson(req, 200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/vehicles
// ============================================================

static void _handleVehiclesGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    String place_str = "A";
    if (req->hasParam("place")) place_str = req->getParam("place")->value();
    place_str.toUpperCase();
    if (place_str != "A" && place_str != "B") {
        _sendError(req, 400, "place must be A or B"); return;
    }
    char pid = place_str.charAt(0);

    if (req->hasParam("profile")) {
        String mid = req->getParam("profile")->value();
        static float s_prof_data_buf[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS] __attribute__((section(".psram_data")));
        static float s_prof_var_buf [VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS] __attribute__((section(".psram_data")));
        uint16_t plen = 0;
        if (!vehicle_recog_get_model_profile(pid, mid.c_str(), s_prof_data_buf, s_prof_var_buf, &plen)) {
            _sendError(req, 404, "model not found"); return;
        }
        JsonDocument doc(&s_psram_alloc);
        doc["id"]     = mid;
        doc["place"]  = place_str;
        doc["length"] = plen;
        JsonArray prof = doc["profile"].to<JsonArray>();
        JsonArray vari = doc["variance"].to<JsonArray>();
        for (uint16_t i = 0; i < plen; i++) {
            JsonArray pt = prof.add<JsonArray>();
            pt.add(s_prof_data_buf[i][0]); pt.add(s_prof_data_buf[i][1]); pt.add(s_prof_data_buf[i][2]);
            JsonArray vt = vari.add<JsonArray>();
            vt.add(s_prof_var_buf[i][0]);  vt.add(s_prof_var_buf[i][1]);  vt.add(s_prof_var_buf[i][2]);
        }
        _sendJson(req, 200, doc);
        return;
    }

    LOG_DEBUG(TAG, "GET /api/vehicles?place=%c", pid);
    uint16_t cnt = vehicle_recog_get_model_count(pid);
    JsonDocument doc(&s_psram_alloc);
    doc["place"] = place_str;
    JsonArray models = doc["models"].to<JsonArray>();
    for (uint16_t i = 0; i < cnt; i++) {
        vr_model_summary_t s;
        if (!vehicle_recog_get_model_summary(pid, i, &s)) continue;
        JsonObject m = models.add<JsonObject>();
        m["id"]          = s.id;
        m["name"]        = s.name;
        m["repetitions"] = s.repetitions;
        m["lastSeen"]    = s.lastSeen;
        if (!isnan(s.lastDtwDistance)) m["lastDtw"] = s.lastDtwDistance;
        const char* cur_id = vehicle_recog_get_model_id(pid);
        m["on_place"] = (cur_id && strcmp(cur_id, s.id) == 0);
    }
    vr_place_state_t vrs = vehicle_recog_get_state(pid);
    doc["vr_state"]     = (uint8_t)vrs;
    doc["vehicle_name"] = vehicle_recog_get_vehicle_name(pid);
    vr_baseline_t bl = vehicle_recog_get_baseline(pid);
    doc["baseline_valid"] = bl.valid;
    _sendJson(req, 200, doc);
}

static void _handleVehiclesRename(AsyncWebServerRequest* req, uint8_t* data,
                                  size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "invalid JSON"); return;
    }
    if (!doc["place"].is<const char*>() || !doc["id"].is<const char*>() || !doc["name"].is<const char*>()) {
        _sendError(req, 400, "place, id, name required"); return;
    }
    const char* place_s  = doc["place"].as<const char*>();
    const char* model_id = doc["id"].as<const char*>();
    const char* new_name = doc["name"].as<const char*>();

    if (!place_s || (place_s[0] != 'A' && place_s[0] != 'B')) {
        _sendError(req, 400, "place must be A or B"); return;
    }
    if (!model_id || strlen(model_id) == 0 || !new_name || strlen(new_name) == 0) {
        _sendError(req, 400, "id and name must be non-empty"); return;
    }

    bool ok = vehicle_recog_rename_model(place_s[0], model_id, new_name);
    LOG_INFO(TAG, "vehicles/rename %c %s -> '%s' ok=%d", place_s[0], model_id, new_name, (int)ok);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]    = ok;
    resp["place"] = place_s;
    resp["id"]    = model_id;
    resp["name"]  = new_name;
    if (!ok) resp["error"] = "model not found or rename failed";
    _sendJson(req, ok ? 200 : 404, resp);
}

static void _handleVehiclesCalibrate(AsyncWebServerRequest* req, uint8_t* data,
                                     size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }
    const char* place_s = doc["place"].as<const char*>();
    if (!place_s || (place_s[0] != 'A' && place_s[0] != 'B')) {
        _sendError(req, 400, "place must be A or B"); return;
    }
    bool ok = vehicle_recog_calibrate_empty(place_s[0]);
    LOG_INFO(TAG, "vehicles/calibrate %c ok=%d", place_s[0], (int)ok);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]    = ok;
    resp["place"] = place_s;
    if (!ok) resp["error"] = "calibration failed";
    _sendJson(req, ok ? 200 : 500, resp);
}

static void _handleVehiclesDelete(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;

    if (!req->hasParam("id") || !req->hasParam("place")) {
        _sendError(req, 400, "id and place query params required"); return;
    }
    String id_str    = req->getParam("id")->value();
    String place_str = req->getParam("place")->value();
    place_str.toUpperCase();
    if (place_str != "A" && place_str != "B") {
        _sendError(req, 400, "place must be A or B"); return;
    }

    bool ok = vehicle_recog_delete_model(place_str.charAt(0), id_str.c_str());
    LOG_INFO(TAG, "vehicles/delete %c %s ok=%d", place_str.charAt(0), id_str.c_str(), (int)ok);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]    = ok;
    resp["place"] = place_str;
    resp["id"]    = id_str;
    if (!ok) resp["error"] = "model not found";
    _sendJson(req, ok ? 200 : 404, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/restart
// ============================================================

static void _handleRestart(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/restart — restart zahteval web UI");

    JsonDocument doc(&s_psram_alloc);
    doc["ok"]  = true;
    doc["msg"] = "restarting in 500ms";
    _sendJson(req, 200, doc);

    logger_flush();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ssr
// ============================================================

static void _handleSsrGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    if (!light_logic_ok()) {
        _sendError(req, 503, "light_logic not ready"); return;
    }

    LightLogicState st = light_logic_get_state();
    JsonDocument doc(&s_psram_alloc);
    JsonArray arr = doc["ssr"].to<JsonArray>();

    for (uint8_t i = 1; i <= 4; i++) {
        JsonObject s = arr.add<JsonObject>();
        s["id"]       = i;
        s["state"]    = (uint8_t)st.ssr[i].state;
        const char* state_str;
        switch (st.ssr[i].state) {
            case SsrLogicState::ON_AUTO:      state_str = "ON_AUTO";   break;
            case SsrLogicState::ON_MANUAL:    state_str = "ON_MANUAL"; break;
            case SsrLogicState::SSR_DISABLED: state_str = "DISABLED";  break;
            default:                          state_str = "OFF";       break;
        }
        s["state_str"]   = state_str;
        s["on"]          = (st.ssr[i].state == SsrLogicState::ON_AUTO ||
                            st.ssr[i].state == SsrLogicState::ON_MANUAL);
        s["countdown_s"] = st.ssr[i].countdown_s;
        s["disabled"]    = st.ssr[i].disabled;
        s["is_auto"]     = st.ssr[i].is_auto;
    }
    doc["is_night"]   = st.is_night;
    doc["lux"]        = st.lux;
    doc["any_motion"] = st.any_motion;
    _sendJson(req, 200, doc);
}

static void _handleSsrPost(AsyncWebServerRequest* req, uint8_t* data,
                           size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    LOG_INFO(TAG, "HTTP → %s [%u bytes]", req->url().c_str(), (unsigned)len);

    JsonDocument doc(&s_psram_alloc);
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) { _sendError(req, 400, "invalid JSON"); return; }

    if (!doc["ssr"].is<int>()) {
        _sendError(req, 400, "missing 'ssr' field (1-4)"); return;
    }
    if (!doc["action"].is<const char*>()) {
        _sendError(req, 400, "missing 'action' field (toggle|disable|enable)"); return;
    }

    int ssr_id = doc["ssr"].as<int>();
    if (ssr_id < 1 || ssr_id > 4) { _sendError(req, 400, "ssr must be 1-4"); return; }

    String action = doc["action"].as<String>();
    action.toLowerCase();

    uint32_t payload = (uint32_t)(ssr_id - 1);

    if (action == "toggle") {
        LOG_INFO(TAG, "POST /api/ssr: toggle SSR%d", ssr_id);
        EventBus::publish(EventType::BUTTON_SSR, payload);
    } else if (action == "disable") {
        LOG_INFO(TAG, "POST /api/ssr: disable SSR%d", ssr_id);
        EventBus::publish(EventType::BUTTON_SSR_DISABLE, payload);
    } else if (action == "enable") {
        LOG_INFO(TAG, "POST /api/ssr: enable SSR%d", ssr_id);
        EventBus::publish(EventType::BUTTON_SSR_DISABLE, payload);
    } else {
        _sendError(req, 400, "unknown action (use toggle|disable|enable)"); return;
    }

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]     = true;
    resp["ssr"]    = ssr_id;
    resp["action"] = action;
    resp["msg"]    = "queued";
    _sendJson(req, 200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /files
// ============================================================

static void _handleFilesGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_files++;
    LOG_INFO(TAG, "HTTP → %s", req->url().c_str());

    if (!sd_mgr_ready()) {
        _sendError(req, 503, "SD not available"); return;
    }

    if (req->hasParam("path")) {
        String path = req->getParam("path")->value();
        LOG_INFO(TAG, "GET /files?path=%s", path.c_str());

        if (!path.startsWith("/") || path.indexOf("..") >= 0) {
            _sendError(req, 400, "invalid path"); return;
        }

        // Streaming file download — AsyncWebServer streama chunked iz SD_MMC File
        AsyncWebServerResponse* resp = req->beginResponse(
            SD_MMC, path, "application/octet-stream", true /* download */);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
        return;
    }

    String dirPath = WEB_SD_ROOT_PATH;
    if (req->hasParam("dir")) {
        dirPath = req->getParam("dir")->value();
        if (!dirPath.startsWith("/") || dirPath.indexOf("..") >= 0) {
            _sendError(req, 400, "invalid dir"); return;
        }
    }

    SdFileInfo* entries = (SdFileInfo*)ps_malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) entries = (SdFileInfo*)malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) { _sendError(req, 503, "out of memory"); return; }

    int cnt = sd_mgr_list_files(dirPath.c_str(), entries, WEB_FILES_MAX_ENTRIES);

    JsonDocument doc(&s_psram_alloc);
    doc["dir"]   = dirPath;
    doc["count"] = cnt;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (int i = 0; i < cnt; i++) {
        JsonObject f = arr.add<JsonObject>();
        f["name"]       = entries[i].name;
        f["path"]       = entries[i].path;
        f["size_bytes"] = entries[i].size_bytes;
        f["date"]       = entries[i].date;
        f["is_dir"]     = entries[i].is_dir;
    }
    doc["disk_total_mb"] = (uint32_t)(sd_mgr_total_bytes() / (1024ULL * 1024ULL));
    doc["disk_free_mb"]  = (uint32_t)(sd_mgr_free_bytes()  / (1024ULL * 1024ULL));

    free(entries);
    _sendJson(req, 200, doc);
}

static void _handleFilesDelete(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_files++;

    if (!sd_mgr_ready()) { _sendError(req, 503, "SD not available"); return; }
    if (!req->hasParam("path")) { _sendError(req, 400, "missing path parameter"); return; }

    String path = req->getParam("path")->value();
    LOG_INFO(TAG, "DELETE /files?path=%s", path.c_str());

    if (!path.startsWith("/") || path.indexOf("..") >= 0) {
        _sendError(req, 400, "invalid path"); return;
    }

    bool ok = sd_mgr_delete_file(path.c_str());
    if (!ok) { _sendError(req, 500, "delete failed"); return; }

    JsonDocument doc(&s_psram_alloc);
    doc["ok"]   = true;
    doc["path"] = path;
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ota
// ============================================================

static void _handleOtaUpload(AsyncWebServerRequest* req,
                             const String& filename, size_t index,
                             uint8_t* data, size_t len, bool final) {
    _stats.req_total++;

    if (index == 0) {
        _stats.ota_attempts++;
        LOG_INFO(TAG, "OTA upload start: %s", filename.c_str());
        logger_flush();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            LOG_ERROR(TAG, "OTA Update.begin() failed: %s", Update.errorString());
            return;
        }
    }

    if (Update.write(data, len) != len) {
        LOG_ERROR(TAG, "OTA Update.write() failed at index=%u", (unsigned)index);
        return;
    }

    if (final) {
        if (Update.end(true)) {
            _stats.ota_success++;
            LOG_INFO(TAG, "OTA upload OK: %u bytes — restarting", (unsigned)(index + len));
        } else {
            LOG_ERROR(TAG, "OTA Update.end() failed: %s", Update.errorString());
        }
    }
}

static void _handleOtaRequest(AsyncWebServerRequest* req) {
    _stats.req_api++;
    bool ok = !Update.hasError();
    JsonDocument doc(&s_psram_alloc);
    doc["ok"]  = ok;
    doc["msg"] = ok ? "OTA ok, restarting" : Update.errorString();
    _sendJson(req, ok ? 200 : 500, doc);

    if (ok) {
        logger_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    }
}

// ============================================================
// SECTION 3 — NOT FOUND + CORS OPTIONS
// ============================================================

static void _handleNotFound(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_errors++;
    LOG_INFO(TAG, "onNotFound: %s %s (LFS=%d assets=%d SRAM=%lu)",
             req->methodToString(), req->url().c_str(),
             _littlefs_ok, _assets_ok,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // CORS preflight
    if (req->method() == HTTP_OPTIONS) {
        AsyncWebServerResponse* resp = req->beginResponse(204);
        _addCorsHeaders(resp);
        req->send(resp);
        return;
    }

    String url = req->url();

    // Binarni/medijski resursi ki ne obstajajo → 404 (ne SPA fallback).
    // Brskalnik cachira 404, zato po prvem page load-u ne zahteva več.
    // Brez tega: vsak font dobi index.html (napačen Content-Type), brskalnik zavrže in
    // ponavlja zahteve pri vsaki obnovi → nepotrebne VFS operacije.
    static const char* const s_no_spa[] = {
        ".woff", ".woff2", ".ttf", ".otf", ".eot",
        ".ico", ".png", ".jpg", ".jpeg", ".gif", ".svg",
        ".mp3", ".mp4", ".webm", nullptr
    };
    for (int i = 0; s_no_spa[i]; i++) {
        if (url.endsWith(s_no_spa[i])) {
            req->send(404, "text/plain", "not found");
            return;
        }
    }

    // SPA fallback — pot ki ni /api ali /files → index.html (Alpine.js router).
    // Strežemo iz PSRAM bufferja (prednaložen ob startu) — brez VFS fopen() v hot potu.
    // Prej: 3× fopen() na zahtevo × 5 sočasnih fontov → SRAM OOM → abort().
    if (!url.startsWith("/api") && !url.startsWith("/files")) {
        if (!_assets_ok || !_index_html_buf) {
            req->send(503, "text/plain", "LittleFS: index.html not loaded — run uploadfs");
            return;
        }
        LOG_INFO(TAG, "SPA fallback: %s → index.html (PSRAM %u B)", url.c_str(), (unsigned)_index_html_sz);
        AsyncWebServerResponse* resp = req->beginResponse_P(200, "text/html", _index_html_buf, _index_html_sz);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
        return;
    }

    // /api/* ali /files/* pot ki ne obstaja
    LOG_DEBUG(TAG, "404: %s", url.c_str());
    JsonDocument doc(&s_psram_alloc);
    doc["error"] = "not found";
    doc["path"]  = url;
    String body;
    serializeJson(doc, body);
    req->send(404, "application/json", body);
}

// ============================================================
// SECTION 3c — WLED TASK (A2/A3/A4)
// ============================================================

static void _wled_post(const char* ip, const char* body) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/json/state", ip);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(2000);
    int code = http.POST((uint8_t*)body, strlen(body));
    if (code > 0) {
        LOG_DEBUG(TAG, "WLED POST %s → HTTP %d", body, code);
    } else {
        LOG_WARN(TAG, "WLED POST napaka: %d (%s) body=%s",
                 code, HTTPClient::errorToString(code).c_str(), body);
    }
    http.end();
}

static void _wled_poll(const char* ip) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/json/state", ip);
    HTTPClient http;
    http.begin(url);
    http.setTimeout(2000);
    int code = http.GET();
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc(&s_psram_alloc);
        if (!deserializeJson(doc, resp)) {
            PartyState ps  = screen_party_get_state();
            ps.party_on    = doc["on"].as<bool>();
            ps.brightness  = doc["bri"] | ps.brightness;
            if (doc["seg"].is<JsonArray>() && doc["seg"].as<JsonArray>().size() > 0) {
                ps.fx_id = doc["seg"][0]["fx"] | ps.fx_id;
                ps.speed = doc["seg"][0]["sx"] | ps.speed;
            }
            // active_slot ne more biti določen samo iz fx_id (več slotov ima isti fx)
            ps.active_slot = s_active_slot;
            screen_party_set_state(ps);
        }
    } else if (code > 0) {
        LOG_DEBUG(TAG, "WLED poll HTTP %d", code);
    }
    http.end();
}

static void wledTask(void* pv) {
    static const uint32_t POLL_MS = 5000;
    uint32_t last_poll = millis() - POLL_MS;

    WledCmd cmd;
    while (true) {
        uint32_t now     = millis();
        uint32_t elapsed = now - last_poll;
        uint32_t wait    = (elapsed >= POLL_MS) ? 0 : (POLL_MS - elapsed);

        bool got = (xQueueReceive(s_wled_q, &cmd, pdMS_TO_TICKS(wait)) == pdTRUE);

        const Config& cfg = config_get();
        const char* ip   = cfg.wled_ip;
        char body[160];

        if (got) {
            switch (cmd.type) {
                case WledCmdType::TOGGLE:
                    if (cmd.payload) {
                        // ON: MUX HIGH tukaj — zagotovljeno pred delay (LCD in web pot)
                        digitalWrite(PIN_MUX_SELECT, HIGH);
                        vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
                        _wled_post(ip, "{\"on\":true}");
                        s_wled_on = true;
                        LOG_INFO(TAG, "WLED ON → %s", ip);
                    } else {
                        // OFF: ugasni WLED, nato vrni MUX
                        _wled_post(ip, "{\"on\":false}");
                        vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
                        led_mgr_set_party_mode(false);
                        s_wled_on = false;
                        LOG_INFO(TAG, "WLED OFF, MUX → PRIMARY");
                    }
                    break;
                case WledCmdType::EFFECT:
                    snprintf(body, sizeof(body),
                             "{\"seg\":[{\"fx\":%lu}]}", (unsigned long)cmd.payload);
                    _wled_post(ip, body);
                    break;
                case WledCmdType::COLOR: {
                    uint8_t r = (cmd.payload >> 16) & 0xFF;
                    uint8_t g = (cmd.payload >> 8)  & 0xFF;
                    uint8_t b =  cmd.payload        & 0xFF;
                    snprintf(body, sizeof(body),
                             "{\"seg\":[{\"col\":[[%u,%u,%u]]}]}", r, g, b);
                    _wled_post(ip, body);
                    break;
                }
                case WledCmdType::BRIGHTNESS:
                    snprintf(body, sizeof(body),
                             "{\"bri\":%lu}", (unsigned long)cmd.payload);
                    _wled_post(ip, body);
                    break;
                case WledCmdType::SPEED:
                    snprintf(body, sizeof(body),
                             "{\"seg\":[{\"sx\":%lu}]}", (unsigned long)cmd.payload);
                    _wled_post(ip, body);
                    break;
                case WledCmdType::SLOT: {
                    PartySlot sl = config_get_party_slot((uint8_t)cmd.payload);
                    if (sl.color_rgb != 0x000000) {
                        uint8_t r = (sl.color_rgb >> 16) & 0xFF;
                        uint8_t g = (sl.color_rgb >> 8)  & 0xFF;
                        uint8_t b =  sl.color_rgb        & 0xFF;
                        snprintf(body, sizeof(body),
                                 "{\"on\":true,\"bri\":%u,"
                                 "\"seg\":[{\"fx\":%u,\"sx\":%u,\"col\":[[%u,%u,%u]]}]}",
                                 sl.brightness, sl.fx_id, sl.speed, r, g, b);
                    } else {
                        snprintf(body, sizeof(body),
                                 "{\"on\":true,\"bri\":%u,"
                                 "\"seg\":[{\"fx\":%u,\"sx\":%u}]}",
                                 sl.brightness, sl.fx_id, sl.speed);
                    }
                    _wled_post(ip, body);
                    s_wled_on    = true;
                    s_active_slot = (uint8_t)cmd.payload;
                    LOG_INFO(TAG, "WLED SLOT %u (%s) → %s", (unsigned)cmd.payload, sl.name, ip);
                    break;
                }
                case WledCmdType::SUSPEND:
                    _wled_post(ip, "{\"on\":false}");
                    vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
                    led_mgr_set_party_mode(false);
                    s_wled_on = false;
                    LOG_INFO(TAG, "WLED SUSPEND — MUX → PRIMARY");
                    break;
                case WledCmdType::RESUME:
                    digitalWrite(PIN_MUX_SELECT, HIGH);
                    vTaskDelay(pdMS_TO_TICKS(MUX_SWITCH_DELAY_MS));
                    _wled_post(ip, "{\"on\":true}");
                    s_wled_on = true;
                    LOG_INFO(TAG, "WLED RESUME — MUX HIGH + WLED ON");
                    break;
                default: break;
            }
        }

        now = millis();
        if ((now - last_poll) >= POLL_MS) {
            if (s_wled_on) _wled_poll(ip);
            last_poll = now;
        }
    }
}

// ============================================================
// SECTION 3d — HANDLER-JI: /api/party/config (A3)
// ============================================================

static void _handlePartyConfigGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/party/config");
    const Config& cfg = config_get();
    JsonDocument doc(&s_psram_alloc);
    doc["wled_ip"] = cfg.wled_ip;
    _sendJson(req, 200, doc);
}

static void _handlePartyConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    LOG_INFO(TAG, "POST /api/party/config [%u B]", (unsigned)len);
    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "invalid JSON"); return;
    }
    // Config (~910B) alociramo na PSRAM — async_tcp stack je samo 4096B
    Config* cfg = (Config*)heap_caps_malloc(sizeof(Config), MALLOC_CAP_SPIRAM);
    if (!cfg) { _sendError(req, 500, "OOM"); return; }
    *cfg = config_get();
    bool changed = false;

    if (doc["wled_ip"].is<const char*>()) {
        const char* ip = doc["wled_ip"].as<const char*>();
        size_t iplen = ip ? strlen(ip) : 0;
        if (iplen > 0 && iplen < 32) {
            strncpy(cfg->wled_ip, ip, sizeof(cfg->wled_ip));
            cfg->wled_ip[sizeof(cfg->wled_ip) - 1] = '\0';
            changed = true;
            LOG_INFO(TAG, "WLED IP nastavljen: %s", cfg->wled_ip);
        }
    }
    if (doc["resume_delay_s"].is<uint32_t>()) {
        uint32_t rd = doc["resume_delay_s"].as<uint32_t>();
        if (rd >= 5 && rd <= 300) {
            cfg->party_resume_delay_s = rd;
            changed = true;
        }
    }
    if (!changed) { heap_caps_free(cfg); _sendError(req, 400, "no valid fields"); return; }

    config_set(*cfg);
    config_save();

    JsonDocument resp(&s_psram_alloc);
    resp["ok"]            = true;
    resp["wled_ip"]       = cfg->wled_ip;
    resp["resume_delay_s"]= cfg->party_resume_delay_s;
    heap_caps_free(cfg);
    _sendJson(req, 200, resp);
}

// ============================================================
// SECTION 3e — HANDLER-JI: /api/party/* (party slots, schedules, status)
// ============================================================

static void _handlePartySlotsGet(AsyncWebServerRequest* req) {
    _stats.req_total++; _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/party/slots");
    JsonDocument doc(&s_psram_alloc);
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (int i = 0; i < 9; i++) {
        PartySlot sl = config_get_party_slot((uint8_t)i);
        JsonObject o = arr.add<JsonObject>();
        o["idx"]        = i;
        o["name"]       = sl.name;
        o["fx_id"]      = sl.fx_id;
        o["color_rgb"]  = sl.color_rgb;
        o["brightness"] = sl.brightness;
        o["speed"]      = sl.speed;
        o["enabled"]    = (sl.enabled != 0);
    }
    _sendJson(req, 200, doc);
}

static void _handlePartySlotsPost(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++; _stats.req_api++;
    if (index + len < total) return;
    LOG_INFO(TAG, "POST /api/party/slots [%u B]", (unsigned)len);

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }

    if (!doc["idx"].is<int>()) { _sendError(req, 400, "missing idx"); return; }
    int idx = doc["idx"].as<int>();
    if (idx < 0 || idx > 8)  { _sendError(req, 400, "idx out of range (0-8)"); return; }

    PartySlot sl = config_get_party_slot((uint8_t)idx);
    if (doc["name"].is<const char*>()) {
        strncpy(sl.name, doc["name"].as<const char*>(), sizeof(sl.name) - 1);
        sl.name[sizeof(sl.name) - 1] = '\0';
    }
    if (doc["fx_id"].is<int>())      sl.fx_id      = (uint8_t)doc["fx_id"].as<int>();
    if (doc["color_rgb"].is<long>()) sl.color_rgb  = (uint32_t)doc["color_rgb"].as<long>();
    if (doc["brightness"].is<int>()) sl.brightness = (uint8_t)doc["brightness"].as<int>();
    if (doc["speed"].is<int>())      sl.speed      = (uint8_t)doc["speed"].as<int>();
    if (doc["enabled"].is<bool>())   sl.enabled    = doc["enabled"].as<bool>() ? 1 : 0;

    config_set_party_slot((uint8_t)idx, sl);
    config_save_party_slots();
    screen_party_request_slot_reload();

    JsonDocument resp(&s_psram_alloc);
    resp["ok"] = true; resp["idx"] = idx;
    _sendJson(req, 200, resp);
}

static void _handlePartyTogglePost(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++; _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }
    if (!doc["on"].is<bool>()) { _sendError(req, 400, "missing 'on' bool"); return; }
    bool on = doc["on"].as<bool>();

    if (!s_wled_q) { _sendError(req, 503, "wled queue nedostopna"); return; }
    EventBus::publish(EventType::BUTTON_PARTY_TOGGLE, on ? 1u : 0u);
    LOG_INFO(TAG, "PARTY toggle → %s (prek web)", on ? "ON" : "OFF");

    JsonDocument resp(&s_psram_alloc);
    resp["ok"] = true; resp["on"] = on;
    _sendJson(req, 200, resp);
}

static void _handlePartyActivatePost(AsyncWebServerRequest* req, uint8_t* data,
                                      size_t len, size_t index, size_t total) {
    _stats.req_total++; _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }
    if (!doc["slot_idx"].is<int>()) { _sendError(req, 400, "missing slot_idx"); return; }
    int si = doc["slot_idx"].as<int>();
    if (si < 0 || si > 8) { _sendError(req, 400, "slot_idx out of range"); return; }
    if (!s_wled_on) { _sendError(req, 400, "party ni vklopljen"); return; }

    EventBus::publish(EventType::BUTTON_PARTY_SLOT, (uint32_t)si);
    LOG_INFO(TAG, "PARTY activate slot %d (prek web)", si);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"] = true; resp["slot_idx"] = si;
    _sendJson(req, 200, resp);
}

static void _handlePartySchedulesGet(AsyncWebServerRequest* req) {
    _stats.req_total++; _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/party/schedules");
    JsonDocument doc(&s_psram_alloc);
    JsonArray arr = doc["schedules"].to<JsonArray>();
    for (int i = 0; i < 10; i++) {
        PartySchedule sc = config_get_party_schedule((uint8_t)i);
        JsonObject o = arr.add<JsonObject>();
        o["idx"]         = i;
        o["name"]        = sc.name;
        o["slot_idx"]    = sc.slot_idx;
        o["enabled"]     = (sc.enabled != 0);
        o["from_month"]  = sc.from_month;
        o["from_day"]    = sc.from_day;
        o["to_month"]    = sc.to_month;
        o["to_day"]      = sc.to_day;
        o["use_lux_on"]  = (sc.use_lux_on != 0);
        o["lux_on"]      = sc.lux_on;
        o["time_on_h"]   = sc.time_on_h;
        o["time_on_m"]   = sc.time_on_m;
        o["use_lux_off"] = (sc.use_lux_off != 0);
        o["lux_off"]     = sc.lux_off;
        o["time_off_h"]  = sc.time_off_h;
        o["time_off_m"]  = sc.time_off_m;
    }
    _sendJson(req, 200, doc);
}

static void _handlePartySchedulesPost(AsyncWebServerRequest* req, uint8_t* data,
                                       size_t len, size_t index, size_t total) {
    _stats.req_total++; _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }
    if (!doc["idx"].is<int>()) { _sendError(req, 400, "missing idx"); return; }
    int idx = doc["idx"].as<int>();
    if (idx < 0 || idx > 9) { _sendError(req, 400, "idx out of range (0-9)"); return; }

    PartySchedule sc = config_get_party_schedule((uint8_t)idx);
    if (doc["name"].is<const char*>()) {
        strncpy(sc.name, doc["name"].as<const char*>(), sizeof(sc.name) - 1);
        sc.name[sizeof(sc.name) - 1] = '\0';
    }
    if (doc["slot_idx"].is<int>()) {
        int sli = doc["slot_idx"].as<int>();
        if (sli < 0 || sli > 8) { _sendError(req, 400, "slot_idx mora biti 0-8"); return; }
        sc.slot_idx = (uint8_t)sli;
    }
    if (doc["enabled"].is<bool>())    sc.enabled     = doc["enabled"].as<bool>() ? 1 : 0;
    if (doc["from_month"].is<int>())  sc.from_month  = (uint8_t)doc["from_month"].as<int>();
    if (doc["from_day"].is<int>())    sc.from_day    = (uint8_t)doc["from_day"].as<int>();
    if (doc["to_month"].is<int>())    sc.to_month    = (uint8_t)doc["to_month"].as<int>();
    if (doc["to_day"].is<int>())      sc.to_day      = (uint8_t)doc["to_day"].as<int>();
    if (doc["use_lux_on"].is<bool>()) sc.use_lux_on  = doc["use_lux_on"].as<bool>() ? 1 : 0;
    if (doc["lux_on"].is<long>())     sc.lux_on      = (uint32_t)doc["lux_on"].as<long>();
    if (doc["time_on_h"].is<int>())   sc.time_on_h   = (uint8_t)doc["time_on_h"].as<int>();
    if (doc["time_on_m"].is<int>())   sc.time_on_m   = (uint8_t)doc["time_on_m"].as<int>();
    if (doc["use_lux_off"].is<bool>())sc.use_lux_off = doc["use_lux_off"].as<bool>() ? 1 : 0;
    if (doc["lux_off"].is<long>())    sc.lux_off     = (uint32_t)doc["lux_off"].as<long>();
    if (doc["time_off_h"].is<int>())  sc.time_off_h  = (uint8_t)doc["time_off_h"].as<int>();
    if (doc["time_off_m"].is<int>())  sc.time_off_m  = (uint8_t)doc["time_off_m"].as<int>();

    config_set_party_schedule((uint8_t)idx, sc);
    config_save_party_schedules();

    JsonDocument resp(&s_psram_alloc);
    resp["ok"] = true; resp["idx"] = idx;
    _sendJson(req, 200, resp);
}

static void _handlePartySchedulesDelete(AsyncWebServerRequest* req) {
    _stats.req_total++; _stats.req_api++;
    if (!req->hasParam("idx")) { _sendError(req, 400, "missing idx"); return; }
    int idx = req->getParam("idx")->value().toInt();
    if (idx < 0 || idx > 9) { _sendError(req, 400, "idx out of range"); return; }

    PartySchedule sc = config_get_party_schedule((uint8_t)idx);
    sc.enabled = 0;
    config_set_party_schedule((uint8_t)idx, sc);
    config_save_party_schedules();
    LOG_INFO(TAG, "Party urnik %d onemogočen", idx);

    JsonDocument resp(&s_psram_alloc);
    resp["ok"] = true; resp["idx"] = idx;
    _sendJson(req, 200, resp);
}

static void _handlePartyPriorityPost(AsyncWebServerRequest* req, uint8_t* data,
                                      size_t len, size_t index, size_t total) {
    _stats.req_total++; _stats.req_api++;
    if (index + len < total) return;
    LOG_INFO(TAG, "POST /api/party/priority [%u B]", (unsigned)len);
    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) { _sendError(req, 400, "invalid JSON"); return; }
    if (!doc["on"].is<int>()) { _sendError(req, 400, "missing on"); return; }
    bool prio = (doc["on"].as<int>() != 0);
    EventBus::publish(EventType::BUTTON_PARTY_PRIORITY, prio ? 1u : 0u);
    LOG_INFO(TAG, "Party priority → %s", prio ? "ON" : "OFF");
    JsonDocument resp(&s_psram_alloc);
    resp["ok"]       = true;
    resp["priority"] = prio;
    _sendJson(req, 200, resp);
}

static void _handlePartyStatusGet(AsyncWebServerRequest* req) {
    _stats.req_total++; _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/party/status");
    const Config& cfg = config_get();
    JsonDocument doc(&s_psram_alloc);
    doc["party_on"]       = s_wled_on;
    doc["suspended"]      = light_logic_is_party_suspended();
    doc["priority"]       = light_logic_get_party_priority();
    doc["active_slot"]    = s_active_slot;
    doc["wled_on"]        = s_wled_on;
    doc["resume_delay_s"] = cfg.party_resume_delay_s;
    doc["wled_ip"]        = cfg.wled_ip;
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 4 — REGISTRACIJA HANDLERJEV
// ============================================================

// ============================================================
// SECTION 3 — HANDLER-JI: /api/alarm
// ============================================================

static void _handleAlarmGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/alarm");

    AlarmState as = alarm_get_state();
    JsonDocument doc(&s_psram_alloc);

    const char* state_str = "OFF";
    if (as.state == AlarmStateEnum::ARMED)     state_str = "ARMED";
    if (as.state == AlarmStateEnum::TRIGGERED) state_str = "TRIGGERED";

    doc["state"]            = state_str;
    doc["active"]           = as.active;
    doc["permanent"]        = as.permanent;
    doc["duration_s"]       = as.duration_s;
    doc["remaining_s"]      = as.remaining_s;
    doc["grace_s"]          = as.grace_s;
    doc["callback_url_set"] = as.callback_url_set;
    doc["trigger_count"]    = as.trigger_count;
    doc["callback_sent"]    = as.callback_sent;
    doc["callback_failed"]  = as.callback_failed;
    doc["pin_len"]          = as.pin_len;

    _sendJson(req, 200, doc);
}

static void _handleAlarmPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "invalid JSON");
        return;
    }

    if (!doc["state"].is<const char*>()) {
        _sendError(req, 400, "missing 'state' field (on|off)");
        return;
    }

    String state = doc["state"].as<String>();
    state.toLowerCase();

    if (state == "off") {
        alarm_disarm();
        LOG_INFO(TAG, "POST /api/alarm: disarm");
        JsonDocument resp(&s_psram_alloc);
        resp["ok"]    = true;
        resp["state"] = "OFF";
        _sendJson(req, 200, resp);
        return;
    }

    if (state == "on") {
        AlarmArmParams params = {};
        params.duration_s = doc["duration_seconds"].is<uint32_t>()
                            ? (uint32_t)doc["duration_seconds"].as<uint32_t>() : 0;

        if (doc["callback_url"].is<const char*>()) {
            strlcpy(params.callback_url, doc["callback_url"].as<const char*>(),
                    sizeof(params.callback_url));
        }

        if (doc["grace_s"].is<uint32_t>()) {
            alarm_set_grace_s(doc["grace_s"].as<uint32_t>());
        }

        if (!alarm_arm(params)) {
            _sendError(req, 400, "arm failed — preverite parametre");
            return;
        }

        LOG_INFO(TAG, "POST /api/alarm: armed (duration=%lu s)", (unsigned long)params.duration_s);
        JsonDocument resp(&s_psram_alloc);
        resp["ok"]          = true;
        resp["state"]       = "ARMED";
        resp["duration_s"]  = params.duration_s;
        resp["permanent"]   = (params.duration_s == 0);
        _sendJson(req, 200, resp);
        return;
    }

    _sendError(req, 400, "state mora biti 'on' ali 'off'");
}

static void _handleAlarmTest(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/alarm/test — test blink");
    alarm_test_blink();
    JsonDocument doc(&s_psram_alloc);
    doc["ok"]  = true;
    doc["msg"] = "test blink start";
    _sendJson(req, 200, doc);
}

static void _handleAlarmPinPost(AsyncWebServerRequest* req, uint8_t* data,
                                size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc(&s_psram_alloc);
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "invalid JSON");
        return;
    }
    if (!doc["pin"].is<const char*>()) {
        _sendError(req, 400, "missing 'pin' field");
        return;
    }

    const char* pin = doc["pin"].as<const char*>();
    if (!alarm_set_pin(pin)) {
        _sendError(req, 400, "neveljaven PIN (4-8 stevilk)");
        return;
    }

    LOG_INFO(TAG, "POST /api/alarm/pin: PIN spremenjen");
    JsonDocument resp(&s_psram_alloc);
    resp["ok"]      = true;
    resp["pin_len"] = strlen(pin);
    _sendJson(req, 200, resp);
}

static void _registerHandlers() {
    // CORS preflight
    _server->on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(204);
        _addCorsHeaders(resp);
        req->send(resp);
    });

    // Status
    _server->on("/api/status/light",   HTTP_GET, _handleStatusLight);
    _server->on("/api/status/sensors", HTTP_GET, _handleStatusSensors);
    _server->on("/api/status/system",  HTTP_GET, _handleStatusSystem);

    // Logi
    _server->on("/api/logs",       HTTP_GET,  _handleLogsGet);
    _server->on("/api/logs/flush", HTTP_POST, _handleLogsFlush);

    // Config — POST ima body callback
    _server->on("/api/config",       HTTP_GET,  _handleConfigGet);
    _server->on("/api/config",       HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // onRequest placeholder
        nullptr,                            // onUpload
        _handleConfigPost                   // onBody
    );
    _server->on("/api/config/reset", HTTP_POST, _handleConfigReset);

    // Radar — POST ima body callback
    _server->on("/api/radar",        HTTP_GET,  _handleRadarGet);
    _server->on("/api/radar/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleRadarConfigBody
    );

    // Vozila
    _server->on("/api/vehicles", HTTP_GET,    _handleVehiclesGet);
    _server->on("/api/vehicles/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleVehiclesRename
    );
    _server->on("/api/vehicles/calibrate", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleVehiclesCalibrate
    );
    _server->on("/api/vehicles", HTTP_DELETE, _handleVehiclesDelete);

    // SSR — POST ima body callback
    _server->on("/api/ssr", HTTP_GET, _handleSsrGet);
    _server->on("/api/ssr", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleSsrPost
    );

    // Alarm
    _server->on("/api/alarm", HTTP_GET, _handleAlarmGet);
    _server->on("/api/alarm", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleAlarmPost
    );
    _server->on("/api/alarm/test", HTTP_POST, _handleAlarmTest);
    _server->on("/api/alarm/pin", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleAlarmPinPost
    );

    // Diagnostika — brez LittleFS, testira ali handlerji odgovarjajo
    _server->on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
        _stats.req_total++;
        _stats.req_api++;
        LOG_INFO(TAG, "GET /api/diag — LFS=%d assets=%d SRAM=%lu blok=%lu",
                 _littlefs_ok, _assets_ok,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        JsonDocument doc(&s_psram_alloc);
        doc["ok"]        = true;
        doc["lfs_ok"]    = _littlefs_ok;
        doc["assets_ok"] = _assets_ok;
        doc["sram_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        doc["sram_blok"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        _sendJson(req, 200, doc);
    });

    // Party / WLED config
    _server->on("/api/party/config", HTTP_GET, _handlePartyConfigGet);
    _server->on("/api/party/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartyConfigPost
    );
    // Party toggle
    _server->on("/api/party/toggle", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartyTogglePost
    );
    // Party slots
    _server->on("/api/party/slots", HTTP_GET, _handlePartySlotsGet);
    _server->on("/api/party/slots", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartySlotsPost
    );
    // Party activate
    _server->on("/api/party/activate", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartyActivatePost
    );
    // Party schedules
    _server->on("/api/party/schedules", HTTP_GET,    _handlePartySchedulesGet);
    _server->on("/api/party/schedules", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartySchedulesPost
    );
    _server->on("/api/party/schedules", HTTP_DELETE, _handlePartySchedulesDelete);
    // Party status + priority
    _server->on("/api/party/status", HTTP_GET, _handlePartyStatusGet);
    _server->on("/api/party/priority", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handlePartyPriorityPost
    );

    // Restart
    _server->on("/api/restart", HTTP_POST, _handleRestart);

    // OTA — async upload callback
    _server->on("/api/ota", HTTP_POST,
        _handleOtaRequest,
        _handleOtaUpload
    );

    // Files (SD)
    _server->on("/files", HTTP_GET,    _handleFilesGet);
    _server->on("/files", HTTP_DELETE, _handleFilesDelete);

    // Statične datoteke iz PSRAM — brez LittleFS fopen() per-request.
    // Preprečuje SRAM OOM: brskalnik odpre 6 vzporednih konekcij, vsaka AsyncFileResponse
    // zasede ~3 KB SRAM → skupaj >18 KB → konekcije se resetirajo, JS ne naloži.
    // beginResponse_P streama direktno iz PSRAM → 0 SRAM alokacij za vsebino datoteke.
    for (int i = 0; i < ASSET_COUNT; i++) {
        AssetEntry* a = &s_assets[i];
        if (!a->buf) continue;
        _server->on(a->url, HTTP_GET, [a](AsyncWebServerRequest* req) {
            _stats.req_total++;
            _stats.req_files++;
            AsyncWebServerResponse* resp = req->beginResponse_P(200, a->mime, a->buf, a->sz);
            resp->addHeader("Content-Encoding", "gzip");
            resp->addHeader("Cache-Control", "no-cache");
            req->send(resp);
        });
    }

    // Statične datoteke iz LittleFS (fallback za morebitne datoteke ki niso v PSRAM cachi)
    _server->serveStatic("/", LittleFS, WEB_ASSETS_PATH "/")
           .setDefaultFile("index.html")
           .setCacheControl("no-cache");

    // Not found
    _server->onNotFound(_handleNotFound);

    LOG_INFO(TAG, "Handlers registered (ESPAsyncWebServer v3.0)");
}

// ============================================================
// SECTION 5 — JAVNE FUNKCIJE
// ============================================================

bool web_ui_init() {
    LOG_INFO(TAG, "web_ui_init()");
    memset(&_stats, 0, sizeof(_stats));

    _json_buf_mutex = xSemaphoreCreateMutex();
    if (!_json_buf_mutex) LOG_WARN(TAG, "web_ui_init: _json_buf_mutex napaka");

    // WLED queue + EventBus subscriptions (A2)
    s_wled_q = xQueueCreate(8, sizeof(WledCmd));
    if (!s_wled_q) {
        LOG_ERROR(TAG, "web_ui_init: WLED queue napaka");
    } else {
        EventBus::subscribe(EventType::BUTTON_PARTY_TOGGLE, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::TOGGLE, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::BUTTON_PARTY_EFFECT, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::EFFECT, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::BUTTON_PARTY_COLOR, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::COLOR, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::BUTTON_PARTY_BRIGHTNESS, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::BRIGHTNESS, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::BUTTON_PARTY_SPEED, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::SPEED, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::BUTTON_PARTY_SLOT, [](const Event& ev) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::SLOT, ev.payload };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::PARTY_SUSPENDED, [](const Event&) {
            if (!s_wled_q) return;
            WledCmd c = { WledCmdType::SUSPEND, 0 };
            xQueueSend(s_wled_q, &c, 0);
        });
        EventBus::subscribe(EventType::PARTY_RESUMED, [](const Event& ev) {
            if (!s_wled_q) return;
            s_active_slot = (uint8_t)ev.payload;
            WledCmd c = { WledCmdType::RESUME, 0 };
            xQueueSend(s_wled_q, &c, 0);
        });
        LOG_INFO(TAG, "WLED EventBus subscribers registrirani");
    }

    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
        LOG_WARN(TAG, "LittleFS mount failed — assets ne bodo na voljo");
        _littlefs_ok = false;
        _assets_ok   = false;
        return false;
    }
    _littlefs_ok = true;
    _stats.littlefs_ok = true;

    if (LittleFS.exists(WEB_ASSETS_PATH "/index.html")) {
        _assets_ok = true;
        _stats.assets_ok = true;
        LOG_INFO(TAG, "LittleFS assets OK (index.html najden)");
        // Pre-load index.html v PSRAM — _handleNotFound hot path ne kliče VFS fopen().
        // Brez tega: 3× fopen() na SPA fallback request; s 5 sočasnimi fonti → SRAM OOM → abort().
        File f = LittleFS.open(WEB_ASSETS_PATH "/index.html", "r");
        if (f) {
            size_t sz = f.size();
            _index_html_buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
            if (_index_html_buf) {
                f.readBytes((char*)_index_html_buf, sz);
                _index_html_sz = sz;
                LOG_INFO(TAG, "index.html prednaložen v PSRAM (%u B)", (unsigned)sz);
            } else {
                LOG_WARN(TAG, "ps_malloc index.html NEUSPEL — SPA fallback bo klical fopen()");
            }
            f.close();
        }
    } else {
        _assets_ok = false;
        LOG_WARN(TAG, "LittleFS assets: index.html NI najden v %s/", WEB_ASSETS_PATH);
        LOG_WARN(TAG, "Zaženi: pio run --target uploadfs za upload assets");
    }

    // Pre-load JS/CSS assetov v PSRAM — preprečuje SRAM OOM ob 6 vzporednih konekcijah
    {
        size_t total = 0;
        int loaded = 0;
        for (int i = 0; i < ASSET_COUNT; i++) {
            char fs_path[64];
            snprintf(fs_path, sizeof(fs_path), "%s%s.gz", WEB_ASSETS_PATH, s_assets[i].url);
            File f = LittleFS.open(fs_path, "r");
            if (!f) {
                LOG_WARN(TAG, "asset skip (ni v LFS): %s", fs_path);
                continue;
            }
            size_t sz = f.size();
            uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
            if (buf) {
                f.readBytes((char*)buf, sz);
                s_assets[i].buf = buf;
                s_assets[i].sz  = sz;
                total += sz;
                loaded++;
            } else {
                LOG_WARN(TAG, "asset PSRAM alloc fail: %s (%u B)", fs_path, (unsigned)sz);
            }
            f.close();
        }
        LOG_INFO(TAG, "Assets v PSRAM: %d/%d naloženih (%u B skupaj)", loaded, ASSET_COUNT, (unsigned)total);
    }

    return true;
}

bool web_ui_begin() {
    LOG_INFO(TAG, "web_ui_begin() — zaganjam AsyncWebServer na port %d", WEB_PORT);

    // Dinamična kreacija AsyncWebServer objekta tik pred begin().
    // RAZLOG: statični globalni objekt se konstruira pred setup() in LwIP init-om.
    //         Me-no-dev AsyncTCP interne strukture niso kompatibilne z LwIP stanjem
    //         ki nastopi pozneje — tcp_accept callback se ne proži.
    //         Dinamična kreacija zagotovi da objekt nastane v kontekstu
    //         aktivnega LwIP stacka (po WiFi connect + NTP + LED delay).
    // DOKUMENTIRANO: ram_problem4.md (Patch 5, 2026-05)
    if (_server != nullptr) {
        LOG_WARN(TAG, "web_ui_begin(): _server že obstaja — cleanup");
        delete _server;
        _server = nullptr;
    }
    _server = new AsyncWebServer(WEB_PORT);
    if (!_server) {
        LOG_ERROR(TAG, "web_ui_begin(): new AsyncWebServer NEUSPEL — out of memory");
        return false;
    }
    LOG_INFO(TAG, "AsyncWebServer objekt kreiran (dinamično, po LwIP init)");

    if (!_json_buf) {
        _json_buf = (char*)ps_malloc(_json_buf_sz);
        if (!_json_buf) {
            LOG_ERROR(TAG, "ps_malloc JSON buf NEUSPEL — fallback SRAM");
            _json_buf = (char*)malloc(_json_buf_sz);
        }
        if (_json_buf) LOG_INFO(TAG, "JSON buf OK (%u B PSRAM)", (unsigned)_json_buf_sz);
    }

    _registerHandlers();

    // Start WLED worker task (A2/A4) — po WiFi connect, na CORE_WIFI
    if (s_wled_q && !s_wled_task) {
        BaseType_t r = xTaskCreatePinnedToCore(
            wledTask, "WLED", 6144, nullptr, 1, &s_wled_task, CORE_WIFI);
        if (r != pdPASS) {
            LOG_ERROR(TAG, "wledTask napaka (%d)", (int)r);
            s_wled_task = nullptr;
        } else {
            LOG_INFO(TAG, "wledTask zagnan (Core%d, 6KB stack)", CORE_WIFI);
        }
    }

    _server->begin();
    // AsyncServer::begin() je void — ne moremo direktno preveriti uspeha.
    // Edini zanesljiv indikator je TCP self-connect test v wifiTask.
    LOG_INFO(TAG, "AsyncServer port 80 — begin() klican (TCP self-connect test sledi)");
    LOG_INFO(TAG, "  SRAM po begin(): %lu B  blok: %lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _server_running = true;
    _stats.server_running = true;

    LOG_INFO(TAG, "Web server zagnan: http://%s:%d/",
             wifi_manager_get_ip_str(), WEB_PORT);
    return true;
}

// web_ui_tick() — NI POTREBEN z ESPAsyncWebServer.
// AsyncTCP procesira konekcije v svojem tasku — ne potrebuje eksplicitnega klica.
// Funkcija je stub za kompatibilnost — web_ui.h deklaracija ostane.
void web_ui_tick() {
    // NOP — AsyncTCP je self-contained
}

void web_ui_stop() {
    if (_server_running) {
        if (_server) {
            _server->end();
        }
        _server_running = false;
        _stats.server_running = false;
        LOG_INFO(TAG, "Web server ustavljen");
    }
}

bool web_ui_running() { return _server_running; }

AsyncWebServer* web_ui_get_server() {
    return _server_running ? _server : nullptr;
}

WebUiStats web_ui_get_stats() {
    _stats.server_running = _server_running;
    _stats.littlefs_ok    = _littlefs_ok;
    _stats.assets_ok      = _assets_ok;
    return _stats;
}

QueueHandle_t web_ui_get_wled_queue() {
    return s_wled_q;
}
