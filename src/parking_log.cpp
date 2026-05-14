// ============================================================
// parking_log.cpp — CSV log parkirnih dogodkov na SD
// ============================================================

#include "parking_log.h"
#include "event_bus.h"
#include "vehicle_recog.h"
#include "sd_mgr.h"
#include "logger.h"
#include <time.h>
#include <math.h>

#define PL_TAG "PLOG"

#define PL_DIR      "/parking"
#define PL_CSV_PATH "/parking/parking_log.csv"

// RAM krožni buffer za akumulacijo novih vrstic pred SD flushom
#define PL_BUF_SIZE     10240
#define PL_FLUSH_THRESH (PL_BUF_SIZE * 80 / 100)

static char     s_buf[PL_BUF_SIZE];
static int      s_buf_used    = 0;
static bool     s_init_ok     = false;
static uint32_t s_last_flush_ms = 0;

static const uint32_t FLUSH_INTERVAL_MS = 10000;

static const char* CSV_HEADER =
    "timestamp_iso,event,parking_id,model_id,vehicle_name,"
    "dtw_distance,second_best_dtw,second_best_model_id,"
    "profile_points_raw,scan_duration_ms,is_new_model,is_aborted\n";

// -----------------------------------------------------------------------------
// Interne pomožne funkcije
// -----------------------------------------------------------------------------

static void iso_timestamp(char* out, size_t len) {
    time_t now = time(nullptr);
    if (now > 1577836800UL) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(out, len, "M%010lu", (unsigned long)millis());
    }
}

static void buf_append_line(const char* line) {
    int len = strlen(line);
    if (len >= PL_BUF_SIZE - 1) return;

    if (s_buf_used + len >= PL_BUF_SIZE) {
        // Buffer poln — takoj flushiraj na SD
        parking_log_flush();
    }

    if (s_buf_used + len < PL_BUF_SIZE) {
        memcpy(s_buf + s_buf_used, line, len);
        s_buf_used += len;
        s_buf[s_buf_used] = '\0';
    }

    if (s_buf_used >= PL_FLUSH_THRESH) {
        parking_log_flush();
    }
}

static void log_recognized(const VehicleRecognizedEvent_t* ev) {
    char ts[32];
    iso_timestamp(ts, sizeof(ts));

    char dtw_str[16]  = "";
    char dtw2_str[16] = "";
    if (!isnan(ev->dtwDistance))   snprintf(dtw_str,  sizeof(dtw_str),  "%.2f", ev->dtwDistance);
    if (!isnan(ev->secondBestDtw)) snprintf(dtw2_str, sizeof(dtw2_str), "%.2f", ev->secondBestDtw);

    char line[256];
    snprintf(line, sizeof(line),
             "%s,ARRIVED,%c,%s,%s,%s,%s,%s,%u,%u,%s,false\n",
             ts,
             ev->parkingId,
             ev->modelId,
             ev->name,
             dtw_str,
             dtw2_str,
             ev->secondBestModelId,
             (unsigned)ev->profileLength,
             (unsigned)ev->scanDurationMs,
             ev->isNewModel ? "true" : "false");
    buf_append_line(line);
}

static void log_departed(char parkingId) {
    char ts[32];
    iso_timestamp(ts, sizeof(ts));

    // Ob DEPARTED je state vehicle_recog že EMPTY — uporabimo vrednosti pred resetom
    // ki so shranjene v bufferju (parking_log ne hrani lastnega stanja posebej).
    // Payload VEHICLE_DEPARTED ne vsebuje model info — to je omejitev.
    char line[256];
    snprintf(line, sizeof(line),
             "%s,DEPARTED,%c,,,,,,,,false,false\n",
             ts, parkingId);
    buf_append_line(line);
}

static void log_aborted(const ParkingScanAbortedEvent_t* ev) {
    char ts[32];
    iso_timestamp(ts, sizeof(ts));

    char line[256];
    snprintf(line, sizeof(line),
             "%s,ARRIVED,%c,,,%s,,,,%u,%u,false,true\n",
             ts, ev->parkingId,
             ev->reason,
             (unsigned)ev->profileLength,
             (unsigned)ev->scanDurationMs);
    buf_append_line(line);
}

// -----------------------------------------------------------------------------
// EventBus callback
// -----------------------------------------------------------------------------
static void on_event(const Event& evt) {
    switch (evt.type) {
        case EventType::VEHICLE_RECOGNIZED:
        case EventType::VEHICLE_NEW_MODEL: {
            const VehicleRecognizedEvent_t* ev = (const VehicleRecognizedEvent_t*)evt.payload;
            if (ev) log_recognized(ev);
            break;
        }
        case EventType::VEHICLE_DEPARTED: {
            char pid = (evt.payload == 0u) ? 'A' : 'B';
            log_departed(pid);
            break;
        }
        case EventType::PARKING_SCAN_ABORTED: {
            const ParkingScanAbortedEvent_t* ev = (const ParkingScanAbortedEvent_t*)evt.payload;
            if (ev) log_aborted(ev);
            break;
        }
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool parking_log_init(void) {
    s_buf_used = 0;
    s_buf[0] = '\0';
    s_last_flush_ms = millis();

    if (sd_mgr_ready()) {
        sd_mgr_ensure_dir(PL_DIR);
        // Ustvari datoteko z headerjem če ne obstaja
        if (sd_mgr_file_size(PL_CSV_PATH) < 0) {
            sd_mgr_append_file(PL_CSV_PATH, CSV_HEADER, strlen(CSV_HEADER));
            LOG_INFO(PL_TAG, "CSV kreiran: %s", PL_CSV_PATH);
        }
    } else {
        LOG_WARN(PL_TAG, "SD ni ready — parking_log brez SD");
    }

    EventBus::subscribe(EventType::VEHICLE_RECOGNIZED,   on_event);
    EventBus::subscribe(EventType::VEHICLE_NEW_MODEL,    on_event);
    EventBus::subscribe(EventType::VEHICLE_DEPARTED,     on_event);
    EventBus::subscribe(EventType::PARKING_SCAN_ABORTED, on_event);

    s_init_ok = true;
    LOG_INFO(PL_TAG, "parking_log_init OK");
    return true;
}

void parking_log_flush(void) {
    if (!s_init_ok || s_buf_used == 0) return;
    if (!sd_mgr_ready()) {
        // SD ni na voljo — izgubimo vrstice (sprejemljivo v degraded mode)
        s_buf_used = 0;
        s_buf[0] = '\0';
        return;
    }
    // Append buffer v CSV datoteko
    if (sd_mgr_append_file(PL_CSV_PATH, s_buf, s_buf_used)) {
        LOG_INFO(PL_TAG, "flush: %d bajtov v %s", s_buf_used, PL_CSV_PATH);
    } else {
        LOG_WARN(PL_TAG, "flush napaka: %s", PL_CSV_PATH);
    }
    s_buf_used = 0;
    s_buf[0] = '\0';
    s_last_flush_ms = millis();
}

void parking_log_tick(void) {
    if (!s_init_ok) return;
    uint32_t now = millis();
    if ((now - s_last_flush_ms) >= FLUSH_INTERVAL_MS && s_buf_used > 0) {
        parking_log_flush();
    }
}
