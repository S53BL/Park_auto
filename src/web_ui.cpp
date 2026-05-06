// ============================================================
// web_ui.cpp — Web UI implementacija (REST API + statične datoteke)
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-dev  |  Datum: 2026-04
// ============================================================
//
// STRUKTURA DATOTEKE:
//   1. Includes, globalne spremenljivke, config_stub
//   2. Pomožne funkcije (JSON builder-ji, error odgovori)
//   3. Handler registracija (_registerStaticFiles, _registerApiHandlers,
//      _registerFilesEndpoints, _registerOtaEndpoint)
//   4. Posamezni handler-ji po skupinah:
//      - /api/status
//      - /api/logs + /api/logs/flush
//      - /api/config GET/POST/reset  (STUB — Faza 3 zamenja s config_mgr)
//      - /api/vehicles               (STUB — Faza 3 zamenja z vehicle_recog)
//      - /api/restart
//      - /files (list, download, delete)
//      - /api/ota
//   5. Javne funkcije (web_ui_init, web_ui_begin, web_ui_stop, ...)
//
// ODVISNOSTI — glejte web_ui.h
// ============================================================

#include "web_ui.h"
#include "logger.h"
#include "sd_mgr.h"
#include "wifi_manager.h"
#include "config.h"
#include "config_mgr.h"
#include "hal_radar.h"

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include <freertos/semphr.h>

// ============================================================
// SECTION 1 — GLOBALNE SPREMENLJIVKE
// ============================================================

static const char* TAG = "WEBUI";

static AsyncWebServer  _server(WEB_PORT);
static bool            _server_running  = false;
static bool            _littlefs_ok     = false;
static bool            _assets_ok       = false;

// Statistika zahtevkov (atomarni inkrementi — handler-ji tečejo v istem tasku)
static WebUiStats      _stats = {};

// ============================================================
// SECTION 2 — POMOŽNE FUNKCIJE
// ============================================================

// Pošlje JSON error odgovor
static void _sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    _stats.req_errors++;
    JsonDocument doc;
    doc["error"] = msg;
    String body;
    serializeJson(doc, body);
    req->send(code, "application/json", body);
}

// Pošlje JSON odgovor iz JsonDocument — stream direktno, brez intermediarne String kopije
static void _sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->addHeader("Cache-Control", "no-cache");
    if (code != 200) resp->setCode(code);
    serializeJson(doc, *resp);
    req->send(resp);
}

// Vrne CORS header-je (za razvoj z lokalnim dev strežnikom)
static void _addCorsHeaders(AsyncWebServerResponse* resp) {
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/status
// ============================================================
//
// Faza 1 vrne: WiFi info, SD status, uptime, firmware info.
// Faza 3 doda: SSR stanje, senzorji, TOF, radar, parkirišče.

static void _handleStatus(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/status");

    JsonDocument doc;

    // --- WiFi ---
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

    // --- SD kartica ---
    JsonObject sd = doc["sd"].to<JsonObject>();
    sd["ready"]      = sd_mgr_ready();
    sd["status"]     = sd_mgr_status_str();
    uint64_t total   = sd_mgr_total_bytes();
    uint64_t free_b  = sd_mgr_free_bytes();
    sd["total_mb"]   = (uint32_t)(total  / (1024ULL * 1024ULL));
    sd["free_mb"]    = (uint32_t)(free_b / (1024ULL * 1024ULL));

    // --- Logger statistika ---
    LoggerStats ls = logger_get_stats();
    JsonObject lg = doc["logger"].to<JsonObject>();
    lg["total_lines"]   = ls.total_lines;
    lg["dropped_lines"] = ls.dropped_lines;
    lg["sd_flushes"]    = ls.sd_flush_count;
    lg["ntp_synced"]    = ls.ntp_synced;

    // --- Firmware ---
    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"]   = VERSION_STRING;
    fw["uptime_s"]  = millis() / 1000UL;
    const esp_app_desc_t* app = esp_app_get_description();
    if (app) {
        fw["idf_ver"]   = app->idf_ver;
        fw["build_date"]= app->date;
        fw["build_time"]= app->time;
    }

    // --- Web UI statistika ---
    JsonObject wu = doc["webui"].to<JsonObject>();
    wu["req_total"]    = _stats.req_total;
    wu["req_api"]      = _stats.req_api;
    wu["req_files"]    = _stats.req_files;
    wu["req_errors"]   = _stats.req_errors;
    wu["ota_attempts"] = _stats.ota_attempts;
    wu["ota_success"]  = _stats.ota_success;
    wu["littlefs_ok"]  = _littlefs_ok;
    wu["assets_ok"]    = _assets_ok;

    // --- Stubs (Faza 3) ---
    // SSR, senzorji, parking — prazni array-i da frontend ne crashne
    doc["ssr"].to<JsonArray>();
    doc["tof"].to<JsonArray>();
    doc["radar"].to<JsonArray>();
    doc["cells"].to<JsonArray>();
    doc["parking"].to<JsonArray>();
    doc["is_night"]   = false;
    doc["lux"]        = 0;
    doc["party_mode"] = false;
    doc["ramp"]       = "unknown";
    doc["door"]       = "unknown";

    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/logs
// ============================================================

static void _handleLogsGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/logs");

    // Opcijski parametri: ?lines=N, ?level=ERROR|WARN|INFO|DEBUG
    uint16_t max_lines = WEB_LOG_MAX_LINES;
    if (req->hasParam("lines")) {
        int n = req->getParam("lines")->value().toInt();
        if (n > 0 && n <= 1000) max_lines = (uint16_t)n;
    }

    // Alociraj buffer v PSRAM če je na voljo
    char* buf = (char*)ps_malloc(WEB_LOG_BUF_SIZE);
    if (!buf) {
        buf = (char*)malloc(WEB_LOG_BUF_SIZE);
    }
    if (!buf) {
        _sendError(req, 503, "out of memory");
        return;
    }

    size_t len = logger_get_recent(buf, WEB_LOG_BUF_SIZE, max_lines);
    buf[len] = '\0';

    // Vrni kot JSON array vrstic
    // Parsing vrstic in build JSON-a — vsaka \n je ločilnik
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    char* line = buf;
    char* end  = buf + len;
    while (line < end) {
        char* nl = (char*)memchr(line, '\n', end - line);
        size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);
        if (llen > 0) {
            // Optionalni filter po nivoju
            bool include = true;
            if (req->hasParam("level")) {
                String lvl = req->getParam("level")->value();
                lvl.toUpperCase();
                if      (lvl == "ERROR") include = (strstr(line, ":ERROR]") != nullptr);
                else if (lvl == "WARN")  include = (strstr(line, ":WARN]")  != nullptr ||
                                                    strstr(line, ":ERROR]") != nullptr);
                else if (lvl == "INFO")  include = (strstr(line, ":ERROR]") != nullptr ||
                                                    strstr(line, ":WARN]")  != nullptr ||
                                                    strstr(line, ":INFO]")  != nullptr);
                // lvl == "DEBUG" ali prazno → include vse
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
    _sendJson(req, 200, doc);
}

static void _handleLogsFlush(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/logs/flush — manual flush");
    logger_flush();
    JsonDocument doc;
    doc["ok"] = true;
    doc["msg"] = "flushed";
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/config  (STUB — Faza 3)
// ============================================================

static void _handleConfigGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "GET /api/config");

    JsonDocument doc;

    const Config cfg = config_get();

    // Tab: Osvetlitev
    JsonObject light = doc["light"].to<JsonObject>();
    light["timeout_ssr1_s"]      = cfg.timeout_ssr1_s;
    light["manual_extend_min"]   = cfg.manual_extend_min;
    light["antiforgot_ssr2_min"] = cfg.antiforgot_ssr2_min;
    light["antiforgot_ssr3_min"] = cfg.antiforgot_ssr3_min;
    light["ssr2_auto_night"]     = cfg.ssr2_auto_night;
    light["lux_threshold"]       = cfg.lux_night;    // lux_night kot "lux_threshold" za UI kompatibilnost
    light["lux_day"]             = cfg.lux_day;
    light["brightness_night"]    = cfg.brightness_night;

    // Tab: LED animacije
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

    // Tab: Identifikacija
    JsonObject ident = doc["ident"].to<JsonObject>();
    ident["dtw_threshold"]      = cfg.dtw_threshold;
    ident["sakoe_radius"]       = cfg.sakoe_radius;
    ident["min_profile_points"] = cfg.min_profile_points;
    ident["normalize_points"]   = cfg.normalize_points;
    ident["delta_filter_mm"]    = cfg.delta_filter_mm;
    ident["phase_confirm_cm"]   = cfg.phase_confirm_cm;
    ident["stability_s"]        = cfg.stability_s;

    _sendJson(req, 200, doc);
}

static void _handleConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;

    // ESPAsyncWebServer kliče ta handler večkrat za chunked upload.
    // Zberemo vse chunks preden procesiramo.
    if (index + len < total) {
        // Vmesni chunk — ESPAsyncWebServer bo klical znova z naslednjim
        return;
    }

    LOG_INFO(TAG, "POST /api/config (stub), body=%u bytes", (unsigned)total);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        _sendError(req, 400, "invalid JSON");
        return;
    }

    // Preberi trenutni config, posodobi polja iz JSON-a, shrani
    Config cfg = config_get();  // kopija

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
        if (i["dtw_threshold"].is<float>())           cfg.dtw_threshold        = i["dtw_threshold"];
        if (i["sakoe_radius"].is<uint8_t>())          cfg.sakoe_radius         = i["sakoe_radius"];
        if (i["min_profile_points"].is<uint8_t>())    cfg.min_profile_points   = i["min_profile_points"];
        if (i["normalize_points"].is<uint8_t>())      cfg.normalize_points     = i["normalize_points"];
        if (i["delta_filter_mm"].is<uint32_t>())      cfg.delta_filter_mm      = i["delta_filter_mm"];
        if (i["phase_confirm_cm"].is<uint32_t>())     cfg.phase_confirm_cm     = i["phase_confirm_cm"];
        if (i["stability_s"].is<float>())             cfg.stability_s          = i["stability_s"];
    }

    config_set(cfg);
    bool saved = config_save();
    LOG_INFO(TAG, "Config posodobljen in shranjen v NVS (ok=%d)", (int)saved);

    JsonDocument resp;
    resp["ok"]  = true;
    resp["msg"] = saved ? "saved to NVS" : "saved to RAM only (NVS error)";
    _sendJson(req, 200, resp);
}

static void _handleConfigReset(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_INFO(TAG, "POST /api/config/reset (stub) — reset to defaults");
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
    doc["persistence_n"] = cfg.radar_persistence_n;
    _sendJson(req, 200, doc);
}

static void _handleRadarConfigBody(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index != 0) return;  // samo prvi chunk (JSON je kratek)

    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        _sendError(req, 400, "neveljaven JSON");
        return;
    }

    // Globalni parameter
    if (doc["persistence_n"].is<int>()) {
        uint8_t pn = (uint8_t)doc["persistence_n"].as<int>();
        if (pn > 10) { _sendError(req, 400, "persistence_n izven obsega"); return; }
        Config cfg = config_get();
        cfg.radar_persistence_n = pn;
        config_set(cfg);
        config_save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["persistence_n"] = pn;
        _sendJson(req, 200, resp);
        return;
    }

    // Per-senzor konfiguracija
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
        _sendError(req, 400, "vrednost izven obsega");
        return;
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
    if (!reconfig_ok) {
        resp["warn"] = "konfiguracija na radar ni uspela — bo poskusil ob restartu";
    }
    _sendJson(req, 200, resp);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/vehicles  (STUB — Faza 3)
// ============================================================

static void _handleVehiclesGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    String place = "A";
    if (req->hasParam("place")) place = req->getParam("place")->value();
    place.toUpperCase();
    if (place != "A" && place != "B") {
        _sendError(req, 400, "place must be A or B");
        return;
    }
    LOG_DEBUG(TAG, "GET /api/vehicles?place=%s (stub)", place.c_str());
    JsonDocument doc;
    doc["place"]   = place;
    doc["models"]  = JsonArray();  // prazen — vehicle_recog.h ni implementiran
    doc["_stub"]   = true;
    _sendJson(req, 200, doc);
}

static void _handleVehiclesRename(AsyncWebServerRequest* req, uint8_t* data,
                                  size_t len, size_t index, size_t total) {
    _stats.req_total++;
    _stats.req_api++;
    if (index + len < total) return;
    LOG_DEBUG(TAG, "POST /api/vehicles/rename (stub)");
    JsonDocument doc;
    doc["ok"]    = true;
    doc["msg"]   = "rename not implemented (stub)";
    doc["_stub"] = true;
    _sendJson(req, 200, doc);
}

static void _handleVehiclesDelete(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_api++;
    LOG_DEBUG(TAG, "DELETE /api/vehicles (stub)");
    JsonDocument doc;
    doc["ok"]    = true;
    doc["msg"]   = "delete not implemented (stub)";
    doc["_stub"] = true;
    _sendJson(req, 200, doc);
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

    // Flush logov pred restartom, nato restart po kratki pavzi
    logger_flush();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ssr  (ZAZNAMEK — Blok E)
// ============================================================
// NE implementiraj zdaj — implementirati ko se lotimo web integracije (Blok E).
//
//   GET  /api/ssr → vrne stanje vseh 4 SSR
//   POST /api/ssr → {ssr: N, action: "toggle"|"disable"|"enable"}
//
//   Implementacija:
//     #include "light_logic.h"
//     GET handler: LightLogicState st = light_logic_get_state();
//                  JSON array s ssr[1..4]: state/countdown_s/disabled/is_auto
//     POST handler: parsaj action, kliči EventBus::publish(
//                   action=="disable" ? BUTTON_SSR_DISABLE : BUTTON_SSR,
//                   ssr_idx - 1)  // -1 ker EventBus payload je 0-based!
//
//   OPOMBA: web endpoint uporablja ssr indekse 1–4 (human readable).
//   EventBus payload je 0–3 (screen_main konvencija).
//   Pretvorba: EventBus payload = web ssr_idx - 1.

// ============================================================
// SECTION 3 — HANDLER-JI: /files
// ============================================================
// Implementirano prek sd_mgr API (thread-safe).
//
// GET  /files           → JSON seznam datotek in map na SD kartici
// GET  /files?path=X    → streaming download datoteke
// DELETE /files?path=X  → brisanje datoteke

static void _handleFilesGet(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_files++;

    if (!sd_mgr_ready()) {
        _sendError(req, 503, "SD not available");
        return;
    }

    if (req->hasParam("path")) {
        // --- Download datoteke ---
        String path = req->getParam("path")->value();
        LOG_INFO(TAG, "GET /files?path=%s", path.c_str());

        // Varnostna preveritev: pot mora biti absolutna in ne sme vsebovati ".."
        if (!path.startsWith("/") || path.indexOf("..") >= 0) {
            _sendError(req, 400, "invalid path");
            return;
        }

        int32_t fsize = sd_mgr_file_size(path.c_str());
        if (fsize < 0) {
            _sendError(req, 404, "file not found");
            return;
        }

        // Streaming odgovor — ESPAsyncWebServer streama chunked iz File objekta
        AsyncWebServerResponse* resp = req->beginResponse(
            SD_MMC, path, "application/octet-stream", true /* download */);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
        return;
    }

    // --- Listing map ---
    String dirPath = WEB_SD_ROOT_PATH;
    if (req->hasParam("dir")) {
        dirPath = req->getParam("dir")->value();
        if (!dirPath.startsWith("/") || dirPath.indexOf("..") >= 0) {
            _sendError(req, 400, "invalid dir");
            return;
        }
    }
    LOG_DEBUG(TAG, "GET /files dir=%s", dirPath.c_str());

    // Alociraj array za file info (stack bi bil prevelik za 64 elementov)
    SdFileInfo* entries = (SdFileInfo*)ps_malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) entries = (SdFileInfo*)malloc(sizeof(SdFileInfo) * WEB_FILES_MAX_ENTRIES);
    if (!entries) {
        _sendError(req, 503, "out of memory");
        return;
    }

    int cnt = sd_mgr_list_files(dirPath.c_str(), entries, WEB_FILES_MAX_ENTRIES);

    JsonDocument doc;
    doc["dir"]   = dirPath;
    doc["count"] = cnt;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (int i = 0; i < cnt; i++) {
        JsonObject f = arr.add<JsonObject>();
        f["name"]      = entries[i].name;
        f["path"]      = entries[i].path;
        f["size_bytes"]= entries[i].size_bytes;
        f["date"]      = entries[i].date;
    }

    // SD kapaciteta za prikaz v UI
    doc["disk_total_mb"] = (uint32_t)(sd_mgr_total_bytes() / (1024ULL * 1024ULL));
    doc["disk_free_mb"]  = (uint32_t)(sd_mgr_free_bytes()  / (1024ULL * 1024ULL));

    free(entries);
    _sendJson(req, 200, doc);
}

static void _handleFilesDelete(AsyncWebServerRequest* req) {
    _stats.req_total++;
    _stats.req_files++;

    if (!sd_mgr_ready()) {
        _sendError(req, 503, "SD not available");
        return;
    }
    if (!req->hasParam("path")) {
        _sendError(req, 400, "missing path parameter");
        return;
    }

    String path = req->getParam("path")->value();
    LOG_INFO(TAG, "DELETE /files?path=%s", path.c_str());

    if (!path.startsWith("/") || path.indexOf("..") >= 0) {
        _sendError(req, 400, "invalid path");
        return;
    }

    bool ok = sd_mgr_delete_file(path.c_str());
    if (!ok) {
        _sendError(req, 500, "delete failed");
        return;
    }

    JsonDocument doc;
    doc["ok"]   = true;
    doc["path"] = path;
    _sendJson(req, 200, doc);
}

// ============================================================
// SECTION 3 — HANDLER-JI: /api/ota
// ============================================================
// Multipart .bin upload. ESPAsyncWebServer streama chunked —
// Update.write() kliče se za vsak chunk.
// Ko je upload končan, Update.end() in restart.

static void _handleOtaUpload(AsyncWebServerRequest* req,
                             const String& filename, size_t index,
                             uint8_t* data, size_t len, bool final) {
    _stats.req_total++;

    if (index == 0) {
        _stats.ota_attempts++;
        LOG_INFO(TAG, "OTA upload start: %s", filename.c_str());
        logger_flush();  // flush pred OTA — varnost

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            LOG_ERROR(TAG, "OTA Update.begin() failed: %s",
                      Update.errorString());
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
// SECTION 4 — REGISTRACIJA HANDLERJEV
// ============================================================

static void _registerStaticFiles() {
    // Servi gzip-ane statične datoteke iz LittleFS /assets/
    // ESPAsyncWebServer zazna .gz datoteke in doda Content-Encoding: gzip
    _server.serveStatic("/", LittleFS, WEB_ASSETS_PATH "/")
           .setDefaultFile("index.html")
           .setCacheControl("max-age=86400");  // 1 dan cache za assets

    LOG_INFO(TAG, "Static files: LittleFS%s/", WEB_ASSETS_PATH);
}

static void _registerApiHandlers() {
    // --- OPTIONS preflight (CORS za dev) ---
    _server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(204);
        _addCorsHeaders(resp);
        req->send(resp);
    });

    // --- Status ---
    _server.on("/api/status", HTTP_GET, _handleStatus);

    // --- Logi ---
    _server.on("/api/logs", HTTP_GET, _handleLogsGet);
    _server.on("/api/logs/flush", HTTP_POST, _handleLogsFlush);

    // --- Config (stub) ---
    _server.on("/api/config", HTTP_GET, _handleConfigGet);
    _server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // onRequest (placeholder)
        nullptr,                            // onUpload
        _handleConfigPost                   // onBody
    );
    _server.on("/api/config/reset", HTTP_POST, _handleConfigReset);

    // --- Radar ---
    _server.on("/api/radar",        HTTP_GET,  _handleRadarGet);
    _server.on("/api/radar/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleRadarConfigBody
    );

    // --- Vozila (stub) ---
    _server.on("/api/vehicles", HTTP_GET, _handleVehiclesGet);
    _server.on("/api/vehicles/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        _handleVehiclesRename
    );
    _server.on("/api/vehicles", HTTP_DELETE, _handleVehiclesDelete);

    // --- Restart ---
    _server.on("/api/restart", HTTP_POST, _handleRestart);

    // --- OTA ---
    _server.on("/api/ota", HTTP_POST,
        _handleOtaRequest,
        _handleOtaUpload
    );

    LOG_INFO(TAG, "API handlers registered");
}

static void _registerFilesEndpoints() {
    _server.on("/files", HTTP_GET,    _handleFilesGet);
    _server.on("/files", HTTP_DELETE, _handleFilesDelete);
    LOG_INFO(TAG, "SD /files endpoint registered");
}

static void _registerNotFound() {
    _server.onNotFound([](AsyncWebServerRequest* req) {
        _stats.req_total++;
        _stats.req_errors++;
        // SPA fallback — vse poti ki niso API ali /files serviramo z index.html
        // da Alpine.js router prevzame navigacijo
        if (!req->url().startsWith("/api") && !req->url().startsWith("/files")) {
            req->send(LittleFS, WEB_ASSETS_PATH "/index.html", "text/html");
        } else {
            JsonDocument doc;
            doc["error"] = "not found";
            doc["path"]  = req->url();
            String body;
            serializeJson(doc, body);
            req->send(404, "application/json", body);
        }
    });
}

// ============================================================
// SECTION 5 — JAVNE FUNKCIJE
// ============================================================

bool web_ui_init() {
    LOG_INFO(TAG, "web_ui_init()");
    memset(&_stats, 0, sizeof(_stats));

    // Montiraj LittleFS (bsp.cpp ga morda že montira — Mount je idempotent)
    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
        LOG_WARN(TAG, "LittleFS mount failed — assets ne bodo na voljo");
        _littlefs_ok = false;
        _assets_ok   = false;
        return false;
    }
    _littlefs_ok = true;
    _stats.littlefs_ok = true;

    // Preveri assets/
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
    LOG_INFO(TAG, "web_ui_begin() — zaganjam strežnik na port %d", WEB_PORT);

    _registerStaticFiles();
    _registerApiHandlers();
    _registerFilesEndpoints();
    _registerNotFound();

    DefaultHeaders::Instance().addHeader("X-Parking-ESP32", VERSION_STRING);

    _server.begin();
    _server_running = true;
    _stats.server_running = true;

    LOG_INFO(TAG, "Web server zagnan: http://%s:%d/",
             wifi_manager_get_ip_str(), WEB_PORT);
    LOG_INFO(TAG, "mDNS: http://%s.local/", WIFI_HOSTNAME);

    return true;
}

void web_ui_stop() {
    if (_server_running) {
        _server.end();
        _server_running = false;
        _stats.server_running = false;
        LOG_INFO(TAG, "Web server ustavljen");
    }
}

bool web_ui_running() {
    return _server_running;
}

AsyncWebServer* web_ui_get_server() {
    return _server_running ? &_server : nullptr;
}

WebUiStats web_ui_get_stats() {
    _stats.server_running = _server_running;
    _stats.littlefs_ok    = _littlefs_ok;
    _stats.assets_ok      = _assets_ok;
    return _stats;
}
