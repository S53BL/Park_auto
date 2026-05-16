// ============================================================
// web_ui.cpp — Web UI implementacija (REST API + statične datoteke)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 3.1.0  |  Datum: 2026-05
// ============================================================
//
// v3.1.0: zamenjava ESP32Async → me-no-dev (ram_problem4.md)
// SPREMEMBA v3.0: VRNJENO na ESPAsyncWebServer (ESP32Async/ESPAsyncWebServer)
//
// ZAKAJ JE BIL SINHRONI WebServer NAPAKA (v2.0):
//   Sinhroni WebServer zahteva periodični handleClient() klic.
//   Moderni brskalnik odpre 6 vzporednih HTTP/1.1 konekcij ob nalaganju.
//   LwIP sprejme vse 6 in jih drži dokler handleClient() ne pride do njih.
//   6 konekcij × ~3 KB = ~18 KB > 13 KB prostega SRAM-a → ECONNRESET.
//   To ni bug — je strukturna inkompatibilnost. Nobena koda je ne reši.
//   Skupaj ~15 iteracij in tedni dela brez uspeha.
//
// ZAKAJ DELUJE ZDAJ:
//   SD midnight flush (sd_midnight_flush.cpp) odpravlja originalni vzrok:
//   SD_MMC.open() se kliče samo enkrat na dan ob 00:01 — ni DMA spike.
//   AsyncTCP procesira vsako konekcijo takoj kot callback — brez kopičenja.
//   SRAM ~13 KB je mejno ampak zadostuje brez SD DMA konkurence.
//
// ⚠ OPOZORILO: Ne vračaj sinhroni WebServer. Razlog je dokumentiran zgoraj.
//   Datoteka: ram_problem3.md, Sekcija 3 (Faza 2) in Sekcija 9 (L3).
//
// HANDLER LOGIKA: nespremenjena iz v2.0 — samo adapter layer obrnjen nazaj.
//   _server.arg() / _server.send() → AsyncWebServerRequest* req
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
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

// ============================================================
// SECTION 1 — GLOBALNE SPREMENLJIVKE
// ============================================================

static const char* TAG = "WEBUI";

static AsyncWebServer  _server(WEB_PORT);
static bool            _server_running  = false;
static bool            _littlefs_ok     = false;
static bool            _assets_ok       = false;

static WebUiStats      _stats = {};

// ============================================================
// SECTION 2 — POMOŽNE FUNKCIJE
// ============================================================

static void _sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    _stats.req_errors++;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    req->send(code, "application/json", buf);
}

// Pošlje JsonDocument — stream direktno prek AsyncResponseStream, brez SRAM kopije
static void _sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->addHeader("Cache-Control", "no-cache");
    if (code != 200) resp->setCode(code);
    serializeJson(doc, *resp);
    req->send(resp);
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

    _sendJson(req, 200, doc);
}

static void _handleStatusSensors(AsyncWebServerRequest* req) {
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

    _sendJson(req, 200, doc);
}

static void _handleStatusSystem(AsyncWebServerRequest* req) {
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

    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/logs
// ============================================================

static void _handleLogsGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/logs | SRAM free: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    uint16_t max_lines = WEB_LOG_MAX_LINES;
    if (req->hasParam("lines")) {
        int n = req->getParam("lines")->value().toInt();
        if (n > 0 && n <= 1000) max_lines = (uint16_t)n;
    }

    // Log buffer v PSRAM — ne SRAM
    char* buf = (char*)ps_malloc(WEB_LOG_BUF_SIZE);
    if (!buf) buf = (char*)malloc(WEB_LOG_BUF_SIZE);
    if (!buf) {
        _sendError(req, 503, "out of memory");
        return;
    }

    size_t len = logger_get_recent(buf, WEB_LOG_BUF_SIZE, max_lines);
    buf[len] = '\0';

    bool has_filter = req->hasParam("level");
    String lvl_filter = "";
    if (has_filter) {
        lvl_filter = req->getParam("level")->value();
        lvl_filter.toUpperCase();
    }

    // Streaming prek AsyncResponseStream — brez JsonDocument, 0 B SRAM heap alokacije.
    // FORMAT: ["vrstica1","vrstica2",...] — veljaven JSON array.
    // Optimizacija ohranjena iz v2.0 — veljavna tudi z AsyncTCP.
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->addHeader("Cache-Control", "no-cache");
    resp->print("[");

    bool first = true;
    char* line = buf;
    char* end  = buf + len;

    while (line < end) {
        char* nl    = (char*)memchr(line, '\n', end - line);
        size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);

        if (llen > 0) {
            bool include = true;
            if (has_filter) {
                if      (lvl_filter == "ERROR") include = (strstr(line, ":ERROR]") != nullptr);
                else if (lvl_filter == "WARN")  include = (strstr(line, ":WARN]")  != nullptr ||
                                                           strstr(line, ":ERROR]") != nullptr);
                else if (lvl_filter == "INFO")  include = (strstr(line, ":ERROR]") != nullptr ||
                                                           strstr(line, ":WARN]")  != nullptr ||
                                                           strstr(line, ":INFO]")  != nullptr);
            }

            if (include) {
                if (!first) resp->print(",");
                first = false;
                resp->print("\"");
                for (size_t i = 0; i < llen; i++) {
                    char c = line[i];
                    if      (c == '"')  resp->print("\\\"");
                    else if (c == '\\') resp->print("\\\\");
                    else if (c == '\r') { /* skip */ }
                    else if (c < 0x20)  resp->print(' ');
                    else                resp->write((uint8_t)c);
                }
                resp->print("\"");
            }
        }
        line = nl ? nl + 1 : end;
    }

    resp->print("]");
    free(buf);
    req->send(resp);
}

static void _handleLogsFlush(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/logs/flush — manual flush");
    logger_flush();
    JsonDocument doc;
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

    _sendJson(req, 200, doc);
}

static void _handleConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;  // čakaj na vse chunke

    LOG_INFO(TAG, "POST /api/config, body=%u bytes", (unsigned)total);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        _sendError(req, 400, "invalid JSON");
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
    _sendJson(req, 200, resp);
}

static void _handleConfigReset(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/config/reset");
    config_reset_defaults();
    JsonDocument doc;
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
    _sendJson(req, 200, doc);
}

static void _handleRadarConfigBody(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index != 0) return;

    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "neveljaven JSON");
        return;
    }

    bool global_changed = false;
    Config cfg_global = config_get();

    if (doc["persistence_n"].is<int>()) {
        uint8_t pn = (uint8_t)doc["persistence_n"].as<int>();
        if (pn > 10) { _sendError(req, 400, "persistence_n izven obsega (0-10)"); return; }
        cfg_global.radar_persistence_n = pn;
        global_changed = true;
    }
    if (doc["poll_interval_ms"].is<int>()) {
        uint32_t piv = (uint32_t)doc["poll_interval_ms"].as<int>();
        if (piv < RADAR_POLL_INTERVAL_MIN_MS || piv > RADAR_POLL_INTERVAL_MAX_MS) {
            _sendError(req, 400, "poll_interval_ms izven obsega (10-100)"); return;
        }
        cfg_global.radar_poll_interval_ms = piv;
        global_changed = true;
    }
    if (doc["max_consec_overflows"].is<int>()) {
        uint32_t mcov = (uint32_t)doc["max_consec_overflows"].as<int>();
        if (mcov < 1 || mcov > 100) {
            _sendError(req, 400, "max_consec_overflows izven obsega (1-100)"); return;
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
        _sendJson(req, 200, resp);
        return;
    }

    if (!doc["sensor"].is<int>()) {
        _sendError(req, 400, "sensor manjka");
        return;
    }
    uint8_t sid = (uint8_t)doc["sensor"].as<int>();
    if (sid >= 4) { _sendError(req, 400, "sensor izven obsega"); return; }

    Config cfg = config_get();
    if (doc["max_dist"].is<int>())    cfg.radar_max_dist[sid]    = (uint8_t)doc["max_dist"].as<int>();
    if (doc["move_sens"].is<int>())   cfg.radar_move_sens[sid]   = (uint8_t)doc["move_sens"].as<int>();
    if (doc["static_sens"].is<int>()) cfg.radar_static_sens[sid] = (uint8_t)doc["static_sens"].as<int>();
    if (doc["unmanned_s"].is<int>())  cfg.radar_unmanned_s[sid]  = (uint16_t)doc["unmanned_s"].as<int>();

    if (cfg.radar_max_dist[sid] > 8 || cfg.radar_move_sens[sid] > 100 ||
        cfg.radar_static_sens[sid] > 100) {
        _sendError(req, 400, "vrednost izven obsega"); return;
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
    _sendJson(req, 200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/vehicles
// ============================================================

static void _handleVehiclesGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;

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
        _sendJson(req, 200, doc);
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
    _sendJson(req, 200, doc);
}

static void _handleVehiclesRename(AsyncWebServerRequest* req, uint8_t* data,
                                  size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    JsonDocument doc;
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

    JsonDocument resp;
    resp["ok"]    = ok;
    resp["place"] = place_s;
    resp["id"]    = model_id;
    resp["name"]  = new_name;
    if (!ok) resp["error"] = "model not found or rename failed";
    _sendJson(req, ok ? 200 : 404, resp);
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

    JsonDocument resp;
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

    JsonDocument doc;
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
    LOG_DEBUG(TAG, "GET /api/ssr");

    if (!light_logic_ok()) {
        _sendError(req, 503, "light_logic not ready"); return;
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
    _sendJson(req, 200, doc);
}

static void _handleSsrPost(AsyncWebServerRequest* req, uint8_t* data,
                           size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;

    LOG_DEBUG(TAG, "POST /api/ssr body=%u bytes", (unsigned)len);

    JsonDocument doc;
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

    JsonDocument resp;
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
    LOG_DEBUG(TAG, "GET /files | SRAM free: %lu B",
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

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

    JsonDocument doc;
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
    JsonDocument doc;
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

    // CORS preflight
    if (req->method() == HTTP_OPTIONS) {
        AsyncWebServerResponse* resp = req->beginResponse(204);
        _addCorsHeaders(resp);
        req->send(resp);
        return;
    }

    String url = req->url();

    // SPA fallback — pot ki ni /api ali /files → index.html (Alpine.js router)
    if (!url.startsWith("/api") && !url.startsWith("/files")) {
        req->send(LittleFS, WEB_ASSETS_PATH "/index.html", "text/html");
        return;
    }

    // /api/* ali /files/* pot ki ne obstaja
    LOG_DEBUG(TAG, "404: %s", url.c_str());
    JsonDocument doc;
    doc["error"] = "not found";
    doc["path"]  = url;
    String body;
    serializeJson(doc, body);
    req->send(404, "application/json", body);
}

// ============================================================
// SECTION 4 — REGISTRACIJA HANDLERJEV
// ============================================================

static void _registerHandlers() {
    // CORS preflight
    _server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(204);
        _addCorsHeaders(resp);
        req->send(resp);
    });

    // Status
    _server.on("/api/status/light",   HTTP_GET, _handleStatusLight);
    _server.on("/api/status/sensors", HTTP_GET, _handleStatusSensors);
    _server.on("/api/status/system",  HTTP_GET, _handleStatusSystem);

    // Logi
    _server.on("/api/logs",       HTTP_GET,  _handleLogsGet);
    _server.on("/api/logs/flush", HTTP_POST, _handleLogsFlush);

    // Config — POST ima body callback
    _server.on("/api/config",       HTTP_GET,  _handleConfigGet);
    _server.on("/api/config",       HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // onRequest placeholder
        nullptr,                            // onUpload
        _handleConfigPost                   // onBody
    );
    _server.on("/api/config/reset", HTTP_POST, _handleConfigReset);

    // Radar — POST ima body callback
    _server.on("/api/radar",        HTTP_GET,  _handleRadarGet);
    _server.on("/api/radar/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleRadarConfigBody
    );

    // Vozila
    _server.on("/api/vehicles", HTTP_GET,    _handleVehiclesGet);
    _server.on("/api/vehicles/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleVehiclesRename
    );
    _server.on("/api/vehicles", HTTP_DELETE, _handleVehiclesDelete);

    // SSR — POST ima body callback
    _server.on("/api/ssr", HTTP_GET, _handleSsrGet);
    _server.on("/api/ssr", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleSsrPost
    );

    // Restart
    _server.on("/api/restart", HTTP_POST, _handleRestart);

    // OTA — async upload callback
    _server.on("/api/ota", HTTP_POST,
        _handleOtaRequest,
        _handleOtaUpload
    );

    // Files (SD)
    _server.on("/files", HTTP_GET,    _handleFilesGet);
    _server.on("/files", HTTP_DELETE, _handleFilesDelete);

    // Statične datoteke iz LittleFS
    _server.serveStatic("/", LittleFS, WEB_ASSETS_PATH "/")
           .setDefaultFile("index.html")
           .setCacheControl("max-age=86400");

    // Not found
    _server.onNotFound(_handleNotFound);

    LOG_INFO(TAG, "Handlers registered (ESPAsyncWebServer v3.0)");
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
    LOG_INFO(TAG, "web_ui_begin() — zaganjam AsyncWebServer na port %d", WEB_PORT);

    _registerHandlers();

    _server.begin();
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
        _server.end();
        _server_running = false;
        _stats.server_running = false;
        LOG_INFO(TAG, "Web server ustavljen");
    }
}

bool web_ui_running() { return _server_running; }

AsyncWebServer* web_ui_get_server() {
    return _server_running ? &_server : nullptr;
}

WebUiStats web_ui_get_stats() {
    _stats.server_running = _server_running;
    _stats.littlefs_ok    = _littlefs_ok;
    _stats.assets_ok      = _assets_ok;
    return _stats;
}
