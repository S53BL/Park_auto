// ============================================================
// web_ui.cpp — Web UI implementacija (REST API + statične datoteke)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0  |  Datum: 2026-05
// ============================================================
//
// SPREMEMBA v2.0: ESPAsyncWebServer → sinhroni WebServer (arduino-esp32)
//
// RAZLOG:
//   ESPAsyncWebServer zahteva:
//     - AsyncTCP task stack: 6144 B SRAM
//     - LwIP socket internali: ~4372 B SRAM
//   Skupaj ~10 KB SRAM ki ga ne moremo prihraniti drugače.
//   Za enega občasnega uporabnika (nadzor logov, nastavitve) je
//   sinhroni WebServer funkcionalno identičen — latenca <200ms
//   na LAN je neopazna.
//
// ARHITEKTURA:
//   WebServer teče in se handlera v wifiTask (Core 0).
//   web_ui_tick() kliče _server.handleClient() — mora se klicati
//   redno iz wifiTask zanke (~vsakih 50ms je dovolj).
//   Brez dedicated TCP taska, brez SRAM connection bufferjev.
//
// OMEJITVE sinhroni vs async:
//   - Samo en request naenkrat (OK — en uporabnik)
//   - File download (/files?path=X) streama chunk po chunk v loop-u —
//     blokira wifiTask med prenosom (log datoteke so ~10-50KB, <1s)
//   - OTA upload: sinhroni multipart — deluje, počasnejši za velike .bin
//
// HANDLER LOGIKA: nespremenjena iz v1.0 — samo adapter layer.
//   AsyncWebServerRequest* → _server.arg(), _server.send()
// ============================================================

#include "web_ui.h"
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

#include <LittleFS.h>
#include <WebServer.h>          // sinhroni WebServer — zamenjava za ESPAsyncWebServer
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <SD_MMC.h>             // za file streaming

// ============================================================
// SECTION 1 — GLOBALNE SPREMENLJIVKE
// ============================================================

static const char* TAG = "WEBUI";

static WebServer   _server(WEB_PORT);
static bool        _server_running  = false;
static bool        _littlefs_ok     = false;
static bool        _assets_ok       = false;

static WebUiStats  _stats = {};

// ============================================================
// SECTION 2 — POMOŽNE FUNKCIJE
// ============================================================

// Pošlje JSON error odgovor
static void _sendError(int code, const char* msg) {
    _stats.req_errors++;
    JsonDocument doc;
    doc["error"] = msg;
    String body;
    serializeJson(doc, body);
    _server.send(code, "application/json", body);
}

// Pošlje JsonDocument kot JSON odgovor
// Serializes directly to String — za dokumente <4KB je to OK.
// Večji odgovori (logi) gredo prek _sendString.
static void _sendJson(int code, JsonDocument& doc) {
    String body;
    body.reserve(512);
    serializeJson(doc, body);
    _server.sendHeader("Cache-Control", "no-cache");
    _server.send(code, "application/json", body);
}

// Pošlje navaden string (za /api/logs ki je že formatiran)
static void _sendString(int code, const char* contentType, const String& body) {
    _server.sendHeader("Cache-Control", "no-cache");
    _server.send(code, contentType, body);
}

// Prebere body POST zahtevka (sinhroni WebServer ga ima v _server.arg("plain"))
static String _getBody() {
    if (_server.hasArg("plain")) return _server.arg("plain");
    return String();
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/status/*
// ============================================================

static void _handleStatusLight() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/status/light | SRAM: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    JsonDocument doc;

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

    GpioState gpio = hal_gpio_get_state();
    if (gpio.rampaluc)      doc["ramp"] = "moving";
    else if (gpio.rampagor) doc["ramp"] = "up";
    else                    doc["ramp"] = "down";
    doc["door"] = gpio.vrataod ? "open" : "closed";

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

    _sendJson(200, doc);
}

static void _handleStatusSensors() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/status/sensors | SRAM: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    JsonDocument doc;

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

    _sendJson(200, doc);
}

static void _handleStatusSystem() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/status/system | SRAM: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    JsonDocument doc;

    doc["free_heap"]       = (uint32_t)esp_get_free_heap_size();
    doc["min_free_heap"]   = (uint32_t)esp_get_minimum_free_heap_size();
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

    _sendJson(200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/logs
// ============================================================

static void _handleLogsGet() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/logs | SRAM free: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    uint16_t max_lines = WEB_LOG_MAX_LINES;
    if (_server.hasArg("lines")) {
        int n = _server.arg("lines").toInt();
        if (n > 0 && n <= 1000) max_lines = (uint16_t)n;
    }

    char* buf = (char*)ps_malloc(WEB_LOG_BUF_SIZE);
    if (!buf) buf = (char*)malloc(WEB_LOG_BUF_SIZE);
    if (!buf) {
        _sendError(503, "out of memory");
        return;
    }

    size_t len = logger_get_recent(buf, WEB_LOG_BUF_SIZE, max_lines);
    buf[len] = '\0';

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    char* line = buf;
    char* end  = buf + len;
    while (line < end) {
        char* nl   = (char*)memchr(line, '\n', end - line);
        size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);
        if (llen > 0) {
            bool include = true;
            if (_server.hasArg("level")) {
                String lvl = _server.arg("level");
                lvl.toUpperCase();
                if      (lvl == "ERROR") include = (strstr(line, ":ERROR]") != nullptr);
                else if (lvl == "WARN")  include = (strstr(line, ":WARN]")  != nullptr ||
                                                    strstr(line, ":ERROR]") != nullptr);
                else if (lvl == "INFO")  include = (strstr(line, ":ERROR]") != nullptr ||
                                                    strstr(line, ":WARN]")  != nullptr ||
                                                    strstr(line, ":INFO]")  != nullptr);
            }
            if (include) {
                char tmp = nl ? *nl : '\0';
                if (nl) *nl = '\0';
                arr.add(line);
                if (nl) *nl = tmp;
            }
        }
        line = nl ? nl + 1 : end;
    }

    free(buf);
    _sendJson(200, doc);
}

static void _handleLogsFlush() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/logs/flush — manual flush");
    logger_flush();
    JsonDocument doc;
    doc["ok"]  = true;
    doc["msg"] = "flushed";
    _sendJson(200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/config
// ============================================================

static void _handleConfigGet() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/config");

    JsonDocument doc;
    const Config cfg = config_get();

    JsonObject light = doc["light"].to<JsonObject>();
    light["timeout_ssr1_s"]      = cfg.timeout_ssr1_s;
    light["manual_extend_min"]   = cfg.manual_extend_min;
    light["antiforgot_ssr2_min"] = cfg.antiforgot_ssr2_min;
    light["antiforgot_ssr3_min"] = cfg.antiforgot_ssr3_min;
    light["ssr2_auto_night"]     = cfg.ssr2_auto_night;
    light["lux_threshold"]       = cfg.lux_night;
    light["lux_day"]             = cfg.lux_day;
    light["brightness_night"]    = cfg.brightness_night;

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

    _sendJson(200, doc);
}

static void _handleConfigPost() {
    _stats.req_total++;
    _stats.req_api++;

    String body = _getBody();
    LOG_INFO(TAG, "POST /api/config, body=%u bytes", (unsigned)body.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        _sendError(400, "invalid JSON");
        return;
    }

    Config cfg = config_get();

    if (doc["light"].is<JsonObject>()) {
        JsonObject l = doc["light"];
        if (l["timeout_ssr1_s"].is<uint32_t>())      cfg.timeout_ssr1_s      = l["timeout_ssr1_s"];
        if (l["manual_extend_min"].is<uint32_t>())    cfg.manual_extend_min   = l["manual_extend_min"];
        if (l["antiforgot_ssr2_min"].is<uint32_t>())  cfg.antiforgot_ssr2_min = l["antiforgot_ssr2_min"];
        if (l["antiforgot_ssr3_min"].is<uint32_t>())  cfg.antiforgot_ssr3_min = l["antiforgot_ssr3_min"];
        if (l["ssr2_auto_night"].is<bool>())           cfg.ssr2_auto_night     = l["ssr2_auto_night"];
        if (l["lux_threshold"].is<uint32_t>())         cfg.lux_night           = l["lux_threshold"];
        if (l["lux_day"].is<uint32_t>())               cfg.lux_day             = l["lux_day"];
        if (l["brightness_night"].is<uint8_t>())       cfg.brightness_night    = l["brightness_night"];
    }
    if (doc["led"].is<JsonObject>()) {
        JsonObject a = doc["led"];
        if (a["fill_speed_ms"].is<uint32_t>())        cfg.fill_speed_ms        = a["fill_speed_ms"];
        if (a["unfill_speed_ms"].is<uint32_t>())      cfg.unfill_speed_ms      = a["unfill_speed_ms"];
        if (a["fade_duration_ms"].is<uint32_t>())     cfg.fade_duration_ms     = a["fade_duration_ms"];
        if (a["target_brightness"].is<uint8_t>())     cfg.target_brightness    = a["target_brightness"];
        if (a["ssr2_delay_ms"].is<uint32_t>())        cfg.ssr2_delay_ms        = a["ssr2_delay_ms"];
        if (a["pa_thresh1_mm"].is<uint32_t>())        cfg.pa_thresh_green_mm   = a["pa_thresh1_mm"];
        if (a["pa_thresh2_mm"].is<uint32_t>())        cfg.pa_thresh_orange_mm  = a["pa_thresh2_mm"];
        if (a["pa_thresh3_mm"].is<uint32_t>())        cfg.pa_thresh_red_mm     = a["pa_thresh3_mm"];
        if (a["pa_stability_s"].is<uint32_t>())       cfg.pa_stability_s       = a["pa_stability_s"];
        if (a["photocell_timer_min"].is<uint32_t>())  cfg.photocell_timer_min  = a["photocell_timer_min"];
        if (a["clock_duration_s"].is<uint32_t>())     cfg.clock_duration_s     = a["clock_duration_s"];
    }
    if (doc["ident"].is<JsonObject>()) {
        JsonObject i = doc["ident"];
        if (i["dtw_threshold"].is<float>())            cfg.dtw_threshold           = i["dtw_threshold"];
        if (i["sakoe_radius"].is<uint8_t>())           cfg.sakoe_radius            = i["sakoe_radius"];
        if (i["min_profile_points"].is<uint8_t>())     cfg.min_profile_points      = i["min_profile_points"];
        if (i["normalize_points"].is<uint8_t>())       cfg.normalize_points        = i["normalize_points"];
        if (i["delta_filter_mm"].is<uint32_t>())       cfg.delta_filter_mm         = i["delta_filter_mm"];
        if (i["phase_confirm_cm"].is<uint32_t>())      cfg.phase_confirm_cm        = i["phase_confirm_cm"];
        if (i["stability_s"].is<float>())              cfg.stability_s             = i["stability_s"];
        if (i["raw_profiles_per_model"].is<uint8_t>()) cfg.raw_profiles_per_model  = i["raw_profiles_per_model"];
        if (i["presence_check_min"].is<uint8_t>())     cfg.presence_check_min      = i["presence_check_min"];
        if (i["empty_tolerance_mm"].is<uint32_t>())    cfg.empty_tolerance_mm      = i["empty_tolerance_mm"];
    }

    config_set(cfg);
    bool saved = config_save();
    vehicle_recog_on_config_changed();
    LOG_INFO(TAG, "Config posodobljen in shranjen (ok=%d)", (int)saved);

    JsonDocument resp;
    resp["ok"]  = true;
    resp["msg"] = saved ? "saved to NVS" : "saved to RAM only (NVS error)";
    _sendJson(200, resp);
}

static void _handleConfigReset() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/config/reset");
    config_reset_defaults();
    JsonDocument doc;
    doc["ok"]  = true;
    doc["msg"] = "config reset to defaults";
    _sendJson(200, doc);
}

// ============================================================
// SECTION 3b — HANDLER-JI: /api/radar
// ============================================================

static void _handleRadarGet() {
    _stats.req_total++;
    _stats.req_api++;
    JsonDocument doc;
    JsonArray sensors = doc["sensors"].to<JsonArray>();

    const char* names[4] = {"Vhod","Cesta_L","Cesta_D","Garaza"};
    const Config cfg = config_get();

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
    _sendJson(200, doc);
}

static void _handleRadarConfigPost() {
    _stats.req_total++;
    _stats.req_api++;

    String body = _getBody();
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        _sendError(400, "neveljaven JSON");
        return;
    }

    bool global_changed = false;
    Config cfg_global = config_get();

    if (doc["persistence_n"].is<int>()) {
        uint8_t pn = (uint8_t)doc["persistence_n"].as<int>();
        if (pn > 10) { _sendError(400, "persistence_n izven obsega (0-10)"); return; }
        cfg_global.radar_persistence_n = pn;
        global_changed = true;
    }
    if (doc["poll_interval_ms"].is<int>()) {
        uint32_t piv = (uint32_t)doc["poll_interval_ms"].as<int>();
        if (piv < RADAR_POLL_INTERVAL_MIN_MS || piv > RADAR_POLL_INTERVAL_MAX_MS) {
            _sendError(400, "poll_interval_ms izven obsega (10-100)"); return;
        }
        cfg_global.radar_poll_interval_ms = piv;
        global_changed = true;
    }
    if (doc["max_consec_overflows"].is<int>()) {
        uint32_t mcov = (uint32_t)doc["max_consec_overflows"].as<int>();
        if (mcov < 1 || mcov > 100) {
            _sendError(400, "max_consec_overflows izven obsega (1-100)"); return;
        }
        cfg_global.radar_max_consec_overflows = mcov;
        global_changed = true;
    }

    if (global_changed) {
        config_set(cfg_global);
        config_save();
        JsonDocument resp;
        resp["ok"]                   = true;
        resp["persistence_n"]        = cfg_global.radar_persistence_n;
        resp["poll_interval_ms"]     = cfg_global.radar_poll_interval_ms;
        resp["max_consec_overflows"] = cfg_global.radar_max_consec_overflows;
        _sendJson(200, resp);
        return;
    }

    if (!doc["sensor"].is<int>()) {
        _sendError(400, "sensor manjka");
        return;
    }
    uint8_t sid = (uint8_t)doc["sensor"].as<int>();
    if (sid >= 4) { _sendError(400, "sensor izven obsega"); return; }

    Config cfg = config_get();
    if (doc["max_dist"].is<int>())    cfg.radar_max_dist[sid]    = (uint8_t)doc["max_dist"].as<int>();
    if (doc["move_sens"].is<int>())   cfg.radar_move_sens[sid]   = (uint8_t)doc["move_sens"].as<int>();
    if (doc["static_sens"].is<int>()) cfg.radar_static_sens[sid] = (uint8_t)doc["static_sens"].as<int>();
    if (doc["unmanned_s"].is<int>())  cfg.radar_unmanned_s[sid]  = (uint16_t)doc["unmanned_s"].as<int>();

    if (cfg.radar_max_dist[sid] > 8 || cfg.radar_move_sens[sid] > 100 ||
        cfg.radar_static_sens[sid] > 100) {
        _sendError(400, "vrednost izven obsega"); return;
    }

    config_set(cfg);
    config_save();

    bool reconfig_ok = hal_radar_reconfigure(
        (RadarSensorId)sid,
        cfg.radar_max_dist[sid],
        cfg.radar_move_sens[sid],
        cfg.radar_static_sens[sid],
        cfg.radar_unmanned_s[sid]
    );

    const RadarSensorStatus& rs = hal_radar_get_status((RadarSensorId)sid);
    JsonDocument resp;
    resp["ok"]              = true;
    resp["sensor"]          = sid;
    resp["config_ok"]       = rs.config_ok;
    resp["config_verified"] = rs.config_verified;
    if (!reconfig_ok) resp["warn"] = "konfiguracija na radar ni uspela — bo poskusil ob restartu";
    _sendJson(200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/vehicles
// ============================================================

static void _handleVehiclesGet() {
    _stats.req_total++;
    _stats.req_api++;

    String place_str = "A";
    if (_server.hasArg("place")) place_str = _server.arg("place");
    place_str.toUpperCase();
    if (place_str != "A" && place_str != "B") {
        _sendError(400, "place must be A or B"); return;
    }
    char pid = place_str.charAt(0);

    if (_server.hasArg("profile")) {
        String mid = _server.arg("profile");
        static float s_prof_data_buf[VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS] __attribute__((section(".psram_data")));
        static float s_prof_var_buf [VR_PROFILE_NORM_POINTS][VR_PROFILE_DIMS] __attribute__((section(".psram_data")));
        uint16_t plen = 0;
        if (!vehicle_recog_get_model_profile(pid, mid.c_str(), s_prof_data_buf, s_prof_var_buf, &plen)) {
            _sendError(404, "model not found"); return;
        }
        JsonDocument doc;
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
        _sendJson(200, doc);
        return;
    }

    LOG_DEBUG(TAG, "GET /api/vehicles?place=%c", pid);
    uint16_t cnt = vehicle_recog_get_model_count(pid);
    JsonDocument doc;
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
    _sendJson(200, doc);
}

static void _handleVehiclesRename() {
    _stats.req_total++;
    _stats.req_api++;

    String body = _getBody();
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        _sendError(400, "invalid JSON"); return;
    }
    if (!doc["place"].is<const char*>() || !doc["id"].is<const char*>() || !doc["name"].is<const char*>()) {
        _sendError(400, "place, id, name required"); return;
    }
    const char* place_s  = doc["place"].as<const char*>();
    const char* model_id = doc["id"].as<const char*>();
    const char* new_name = doc["name"].as<const char*>();

    if (!place_s || (place_s[0] != 'A' && place_s[0] != 'B')) {
        _sendError(400, "place must be A or B"); return;
    }
    if (!model_id || strlen(model_id) == 0 || !new_name || strlen(new_name) == 0) {
        _sendError(400, "id and name must be non-empty"); return;
    }

    bool ok = vehicle_recog_rename_model(place_s[0], model_id, new_name);
    LOG_INFO(TAG, "vehicles/rename %c %s -> '%s' ok=%d", place_s[0], model_id, new_name, (int)ok);

    JsonDocument resp;
    resp["ok"]    = ok;
    resp["place"] = place_s;
    resp["id"]    = model_id;
    resp["name"]  = new_name;
    if (!ok) resp["error"] = "model not found or rename failed";
    _sendJson(ok ? 200 : 404, resp);
}

static void _handleVehiclesDelete() {
    _stats.req_total++;
    _stats.req_api++;

    if (!_server.hasArg("id") || !_server.hasArg("place")) {
        _sendError(400, "id and place query params required"); return;
    }
    String id_str    = _server.arg("id");
    String place_str = _server.arg("place");
    place_str.toUpperCase();
    if (place_str != "A" && place_str != "B") {
        _sendError(400, "place must be A or B"); return;
    }

    bool ok = vehicle_recog_delete_model(place_str.charAt(0), id_str.c_str());
    LOG_INFO(TAG, "vehicles/delete %c %s ok=%d", place_str.charAt(0), id_str.c_str(), (int)ok);

    JsonDocument resp;
    resp["ok"]    = ok;
    resp["place"] = place_str;
    resp["id"]    = id_str;
    if (!ok) resp["error"] = "model not found";
    _sendJson(ok ? 200 : 404, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/restart
// ============================================================

static void _handleRestart() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/restart — restart zahteval web UI");

    JsonDocument doc;
    doc["ok"]  = true;
    doc["msg"] = "restarting in 500ms";
    _sendJson(200, doc);

    logger_flush();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ssr
// ============================================================

static void _handleSsrGet() {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/ssr");

    if (!light_logic_ok()) {
        _sendError(503, "light_logic not ready"); return;
    }

    LightLogicState st = light_logic_get_state();
    JsonDocument doc;
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
    _sendJson(200, doc);
}

static void _handleSsrPost() {
    _stats.req_total++;
    _stats.req_api++;

    String body = _getBody();
    LOG_DEBUG(TAG, "POST /api/ssr body=%u bytes", (unsigned)body.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { _sendError(400, "invalid JSON"); return; }

    if (!doc["ssr"].is<int>()) {
        _sendError(400, "missing 'ssr' field (1-4)"); return;
    }
    if (!doc["action"].is<const char*>()) {
        _sendError(400, "missing 'action' field (toggle|disable|enable)"); return;
    }

    int ssr_id = doc["ssr"].as<int>();
    if (ssr_id < 1 || ssr_id > 4) { _sendError(400, "ssr must be 1-4"); return; }

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
        _sendError(400, "unknown action (use toggle|disable|enable)"); return;
    }

    JsonDocument resp;
    resp["ok"]     = true;
    resp["ssr"]    = ssr_id;
    resp["action"] = action;
    resp["msg"]    = "queued";
    _sendJson(200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /files
// ============================================================

static void _handleFilesGet() {
    _stats.req_total++;
    _stats.req_files++;
    LOG_DEBUG(TAG, "GET /files | SRAM free: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (!sd_mgr_ready()) {
        _sendError(503, "SD not available"); return;
    }

    if (_server.hasArg("path")) {
        String path = _server.arg("path");
        LOG_INFO(TAG, "GET /files?path=%s", path.c_str());

        if (!path.startsWith("/") || path.indexOf("..") >= 0) {
            _sendError(400, "invalid path"); return;
        }

        // Streaming file download — sinhroni chunked send
        // WebServer.streamFile() je ekvivalent AsyncWebServer beginResponse(SD_MMC, path)
        // ⚠ Drži wifiTask med prenosom — OK za log datoteke (<50KB, <1s na LAN)
        File f = SD_MMC.open(path.c_str(), FILE_READ);
        if (!f) {
            _sendError(404, "file not found"); return;
        }
        _server.sendHeader("Content-Disposition",
                           String("attachment; filename=\"") + path.substring(path.lastIndexOf('/') + 1) + "\"");
        _server.sendHeader("Cache-Control", "no-cache");
        _server.streamFile(f, "application/octet-stream");
        f.close();
        return;
    }

    String dirPath = WEB_SD_ROOT_PATH;
    if (_server.hasArg("dir")) {
        dirPath = _server.arg("dir");
        if (!dirPath.startsWith("/") || dirPath.indexOf("..") >= 0) {
            _sendError(400, "invalid dir"); return;
        }
    }

    SdFileInfo* entries = (SdFileInfo*)ps_malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) entries = (SdFileInfo*)malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) { _sendError(503, "out of memory"); return; }

    int cnt = sd_mgr_list_files(dirPath.c_str(), entries, WEB_FILES_MAX_ENTRIES);

    JsonDocument doc;
    doc["dir"]   = dirPath;
    doc["count"] = cnt;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (int i = 0; i < cnt; i++) {
        JsonObject f = arr.add<JsonObject>();
        f["name"]       = entries[i].name;
        f["path"]       = entries[i].path;
        f["size_bytes"] = entries[i].size_bytes;
        f["date"]       = entries[i].date;
    }
    doc["disk_total_mb"] = (uint32_t)(sd_mgr_total_bytes() / (1024ULL * 1024ULL));
    doc["disk_free_mb"]  = (uint32_t)(sd_mgr_free_bytes()  / (1024ULL * 1024ULL));

    free(entries);
    _sendJson(200, doc);
}

static void _handleFilesDelete() {
    _stats.req_total++;
    _stats.req_files++;

    if (!sd_mgr_ready()) { _sendError(503, "SD not available"); return; }
    if (!_server.hasArg("path")) { _sendError(400, "missing path parameter"); return; }

    String path = _server.arg("path");
    LOG_INFO(TAG, "DELETE /files?path=%s", path.c_str());

    if (!path.startsWith("/") || path.indexOf("..") >= 0) {
        _sendError(400, "invalid path"); return;
    }

    bool ok = sd_mgr_delete_file(path.c_str());
    if (!ok) { _sendError(500, "delete failed"); return; }

    JsonDocument doc;
    doc["ok"]   = true;
    doc["path"] = path;
    _sendJson(200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ota
// ============================================================
// Sinhroni WebServer OTA upload prek HTTP_POST z multipart/form-data.
// Razlika od async: upload handler dobi celoten chunk v eni iteraciji.

static void _handleOtaUpload() {
    // Pokliče se po zaključku uploada
    bool ok = !Update.hasError();
    JsonDocument doc;
    doc["ok"]  = ok;
    doc["msg"] = ok ? "OTA ok, restarting" : Update.errorString();
    _sendJson(ok ? 200 : 500, doc);
    _stats.ota_attempts++;
    if (ok) {
        _stats.ota_success++;
        LOG_INFO(TAG, "OTA OK — restarting");
        logger_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    }
}

static void _handleOtaChunk() {
    // Pokliče se za vsak chunk med uploadom
    HTTPUpload& upload = _server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        LOG_INFO(TAG, "OTA upload start: %s", upload.filename.c_str());
        logger_flush();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            LOG_ERROR(TAG, "OTA Update.begin() failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            LOG_ERROR(TAG, "OTA Update.write() failed");
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            LOG_INFO(TAG, "OTA upload OK: %u bytes", (unsigned)upload.totalSize);
        } else {
            LOG_ERROR(TAG, "OTA Update.end() failed: %s", Update.errorString());
        }
    }
}

// ============================================================
// SECTION 3 — NOT FOUND + CORS OPTIONS
// ============================================================

static void _handleNotFound() {
    _stats.req_total++;
    _stats.req_errors++;

    // CORS preflight
    if (_server.method() == HTTP_OPTIONS) {
        _server.sendHeader("Access-Control-Allow-Origin",  "*");
        _server.sendHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
        _server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
        _server.send(204);
        return;
    }

    // SPA fallback — pot ki ni /api ali /files → index.html (Alpine.js router)
    String url = _server.uri();
    if (!url.startsWith("/api") && !url.startsWith("/files")) {
        if (LittleFS.exists(String(WEB_ASSETS_PATH) + "/index.html")) {
            File f = LittleFS.open(String(WEB_ASSETS_PATH) + "/index.html", "r");
            _server.streamFile(f, "text/html");
            f.close();
        } else {
            _server.send(404, "text/plain", "index.html not found — run: pio run --target uploadfs");
        }
        return;
    }

    JsonDocument doc;
    doc["error"] = "not found";
    doc["path"]  = url;
    String body;
    serializeJson(doc, body);
    _server.send(404, "application/json", body);
}

// ============================================================
// SECTION 4 — REGISTRACIJA HANDLERJEV
// ============================================================

static void _registerHandlers() {
    // Statične datoteke iz LittleFS
    // ⚠ Sinhroni WebServer: serveStatic() vrne void — ni chainable.
    // index.html fallback je pokrit v _handleNotFound() (SPA fallback).
    _server.serveStatic("/", LittleFS, WEB_ASSETS_PATH "/", "max-age=86400");

    // Status
    _server.on("/api/status/light",   HTTP_GET,  _handleStatusLight);
    _server.on("/api/status/sensors", HTTP_GET,  _handleStatusSensors);
    _server.on("/api/status/system",  HTTP_GET,  _handleStatusSystem);

    // Logi
    _server.on("/api/logs",       HTTP_GET,  _handleLogsGet);
    _server.on("/api/logs/flush", HTTP_POST, _handleLogsFlush);

    // Config
    _server.on("/api/config",       HTTP_GET,  _handleConfigGet);
    _server.on("/api/config",       HTTP_POST, _handleConfigPost);
    _server.on("/api/config/reset", HTTP_POST, _handleConfigReset);

    // Radar
    _server.on("/api/radar",        HTTP_GET,  _handleRadarGet);
    _server.on("/api/radar/config", HTTP_POST, _handleRadarConfigPost);

    // Vozila
    _server.on("/api/vehicles",        HTTP_GET,    _handleVehiclesGet);
    _server.on("/api/vehicles/rename", HTTP_POST,   _handleVehiclesRename);
    _server.on("/api/vehicles",        HTTP_DELETE, _handleVehiclesDelete);

    // SSR
    _server.on("/api/ssr", HTTP_GET,  _handleSsrGet);
    _server.on("/api/ssr", HTTP_POST, _handleSsrPost);

    // Restart
    _server.on("/api/restart", HTTP_POST, _handleRestart);

    // OTA
    // ⚠ WebServer OTA: drugi argument je upload chunk handler, tretji je POST done handler
    _server.on("/api/ota", HTTP_POST, _handleOtaUpload, _handleOtaChunk);

    // Files (SD)
    _server.on("/files", HTTP_GET,    _handleFilesGet);
    _server.on("/files", HTTP_DELETE, _handleFilesDelete);

    // Not found + CORS OPTIONS
    _server.onNotFound(_handleNotFound);

    LOG_INFO(TAG, "Handlers registered (sync WebServer v2.0)");
}

// ============================================================
// SECTION 5 — JAVNE FUNKCIJE
// ============================================================

bool web_ui_init() {
    LOG_INFO(TAG, "web_ui_init()");
    memset(&_stats, 0, sizeof(_stats));

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
    } else {
        _assets_ok = false;
        LOG_WARN(TAG, "LittleFS assets: index.html NI najden v %s/", WEB_ASSETS_PATH);
        LOG_WARN(TAG, "Zaženi: pio run --target uploadfs za upload assets");
    }

    return true;
}

bool web_ui_begin() {
    LOG_INFO(TAG, "web_ui_begin() — zaganjam sync WebServer na port %d", WEB_PORT);

    _registerHandlers();

    _server.begin();
    _server_running = true;
    _stats.server_running = true;

    LOG_INFO(TAG, "Web server zagnan: http://%s:%d/",
             wifi_manager_get_ip_str(), WEB_PORT);
    return true;
}

// ============================================================
// web_ui_tick() — MORA se klicati redno iz wifiTask zanke!
//
// Sinhroni WebServer zahteva periodični handleClient() klic.
// Priporočena frekvenca: vsakih 20–50 ms v wifiTask while(true) zanki.
//
// PRIMER v wifi_manager.cpp / wifiTask:
//   while (true) {
//       wifi_manager_tick();    // reconnect logika itd.
//       web_ui_tick();          // HTTP request handling
//       vTaskDelay(pdMS_TO_TICKS(20));
//   }
// ============================================================
void web_ui_tick() {
    if (_server_running) {
        _server.handleClient();
    }
}

void web_ui_stop() {
    if (_server_running) {
        _server.stop();
        _server_running = false;
        _stats.server_running = false;
        LOG_INFO(TAG, "Web server ustavljen");
    }
}

bool web_ui_running() { return _server_running; }

// web_ui_get_server() je bil namenjen ESPAsyncWebServer — v sync verziji ni potreben.
// Ohranjen kot nullptr stub za kompatibilnost s kodo ki ga morda kliče.
void* web_ui_get_server() { return nullptr; }

WebUiStats web_ui_get_stats() {
    _stats.server_running = _server_running;
    _stats.littlefs_ok    = _littlefs_ok;
    _stats.assets_ok      = _assets_ok;
    return _stats;
}
