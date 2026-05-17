// ============================================================
// event_bus.cpp — EventBus implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-05
// ============================================================

#include "event_bus.h"
#include <esp_task_wdt.h>

#include "hal_gpio.h"
#include "hal_radar.h"

#include "logger.h"
#define EBI(fmt, ...) LOG_INFO ("EVBUS", fmt, ##__VA_ARGS__)
#define EBW(fmt, ...) LOG_WARN ("EVBUS", fmt, ##__VA_ARGS__)
#define EBE(fmt, ...) LOG_ERROR("EVBUS", fmt, ##__VA_ARGS__)
#define EBD(fmt, ...) LOG_DEBUG("EVBUS", fmt, ##__VA_ARGS__)

struct HandlerSlot {
    EventType    type     = EventType::RAMP_UP;
    EventHandler handler  = nullptr;
    bool         active   = false;
};

static constexpr uint16_t MAX_TOTAL_HANDLERS = 40;  // 31 subscribe klicev v kodi + 9 rezerva
static HandlerSlot       s_handlers[MAX_TOTAL_HANDLERS];
static uint16_t          s_handler_count    = 0;
static SemaphoreHandle_t s_mutex            = nullptr;
static bool              s_initialized      = false;
static uint32_t          s_publish_count    = 0;
static uint32_t          s_no_handler_count = 0;

bool EventBus::init() {
    EBI("EventBus init...");
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) { EBE("Mutex napaka!"); return false; }
    }
    for (auto& h : s_handlers) h.active = false;
    s_handler_count = s_publish_count = s_no_handler_count = 0;
    s_initialized = true;
    EBI("EventBus OK (max %d handlerjev)", MAX_TOTAL_HANDLERS);
    return true;
}

bool EventBus::ok() { return s_initialized; }

bool EventBus::subscribe(EventType type, EventHandler handler) {
    if (!s_initialized || !handler) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    uint8_t cnt = 0;
    for (uint16_t i = 0; i < MAX_TOTAL_HANDLERS; i++) {
        if (s_handlers[i].active && s_handlers[i].type == type) cnt++;
    }
    if (cnt >= MAX_HANDLERS_PER_TYPE) {
        xSemaphoreGive(s_mutex);
        EBW("max handlerjev za tip 0x%04X", (uint16_t)type);
        return false;
    }

    bool found = false;
    for (uint16_t i = 0; i < MAX_TOTAL_HANDLERS; i++) {
        if (!s_handlers[i].active) {
            s_handlers[i].type    = type;
            s_handlers[i].handler = handler;
            s_handlers[i].active  = true;
            s_handler_count++;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (!found) EBE("ni prostega slota!");
    return found;
}

void EventBus::unsubscribe(EventType type) {
    if (!s_initialized) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (uint16_t i = 0; i < MAX_TOTAL_HANDLERS; i++) {
        if (s_handlers[i].active && s_handlers[i].type == type) {
            s_handlers[i].active  = false;
            s_handlers[i].handler = nullptr;
            s_handler_count--;
        }
    }
    xSemaphoreGive(s_mutex);
}

void EventBus::publish(EventType type, uint32_t payload) {
    if (!s_initialized) return;
    s_publish_count++;

    EventHandler to_call[MAX_HANDLERS_PER_TYPE];
    uint8_t call_count = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (uint16_t i = 0; i < MAX_TOTAL_HANDLERS && call_count < MAX_HANDLERS_PER_TYPE; i++) {
            if (s_handlers[i].active && s_handlers[i].type == type) {
                to_call[call_count++] = s_handlers[i].handler;
            }
        }
        xSemaphoreGive(s_mutex);
    } else {
        EBW("mutex timeout pri publish 0x%04X", (uint16_t)type);
        return;
    }

    if (call_count == 0) { s_no_handler_count++; return; }

    Event ev = { type, payload, millis() };
    for (uint8_t i = 0; i < call_count; i++) {
        if (to_call[i]) to_call[i](ev);
    }
}

void EventBus::processGpioQueue() {
    hal_gpio_process_queue();
}
void EventBus::processRadarQueue() {
    // Radar IRQ queue se procesira interno v radarTask — ni javne funkcije.
}

// ============================================================
// eventBusTask
// ============================================================
void eventBusTask(void* pvParams) {
    EBI("eventBusTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    if (!EventBus::init()) {
        EBE("EventBus::init NAPAKA!");
    }

    if (!hal_gpio_init()) {
        EBE("hal_gpio_init NAPAKA — GPIO modul onemogočen (SSR, rampa, vrata)!");
        // Ne restartamo — sistem teče naprej brez GPIO (degraded mode)
    } else {
        EBI("hal_gpio_init OK");
    }

    EBI("eventBusTask v zanki");
    uint32_t last_stats = 0;
    while (true) {
        esp_task_wdt_reset();

        EventBus::processGpioQueue();   // MCP23017 INT queue — non-blocking
        EventBus::processRadarQueue();  // SC16IS752 IRQ queue — non-blocking
        hal_gpio_tick();                // rampaluc timeout + health-check timer

        uint32_t now = millis();
        if ((now - last_stats) >= 60000) {
            last_stats = now;
            EBI("Stats: %lu publish, %d handler(jev)",
                (unsigned long)s_publish_count, s_handler_count);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
