// ============================================================
// event_bus.h — EventBus: tipi in javni API
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — minimalen, brez GPIO/radar include
// ============================================================
#pragma once
#include <Arduino.h>
#include <functional>

// ============================================================
// EVENT TIPI
// ============================================================
enum class EventType : uint16_t {
    // GPIO (rampa, vrata, fotocelice)
    RAMP_UP             = 0x0001,
    RAMP_MOVING         = 0x0002,
    DOOR_OPENED         = 0x0003,
    CELL_BROKEN         = 0x0004,
    // Radar
    RADAR_MOTION        = 0x0010,
    RADAR_STILL         = 0x0011,
    RADAR_CLEAR         = 0x0012,
    RADAR_ERROR         = 0x0013,
    // TOF / prepoznava
    TOF_PROFILE_READY   = 0x0020,
    VEHICLE_RECOGNIZED  = 0x0021,
    // Svetloba
    NIGHT_THRESHOLD_CHANGED = 0x0030,
    // UI gumbi
    BUTTON_SSR1         = 0x0040,
    BUTTON_SSR2         = 0x0041,
    BUTTON_SSR3         = 0x0042,
    BUTTON_SSR4         = 0x0043,
    BUTTON_SSR_DISABLE  = 0x0044,
    BUTTON_PARTY_TOGGLE     = 0x0045,
    BUTTON_PARTY_EFFECT     = 0x0049,
    BUTTON_PARTY_COLOR      = 0x004A,
    BUTTON_PARTY_BRIGHTNESS = 0x004B,
    BUTTON_PARTY_SPEED      = 0x004C,
    BUTTON_PARTY_PRESET     = 0x004D,
    BUTTON_EDIT_VEHICLE_A = 0x0046,   // ←
    BUTTON_EDIT_VEHICLE_B = 0x0047,   // ← 
    BUTTON_SSR          = 0x0048,     // ← (kratek dotik, payload=ssr_idx)    
    // SSR stanje (za LCD posodobitev)
    SSR_STATE_CHANGED   = 0x0050,
    // Alarm
    ALARM_TRIGGERED     = 0x0060,
};

struct Event {
    EventType type;
    uint32_t  payload;
    uint32_t  timestamp_ms;
};

using EventHandler = std::function<void(const Event&)>;

// ============================================================
// EVENTBUS — statični razred
// ============================================================
class EventBus {
public:
    static bool init();
    static bool ok();
    static bool subscribe(EventType type, EventHandler handler);
    static void unsubscribe(EventType type);
    static void publish(EventType type, uint32_t payload = 0);

    // Procesiranje ISR queue — kliče eventBusTask
    static void processGpioQueue();
    static void processRadarQueue();

    static constexpr uint8_t MAX_HANDLERS_PER_TYPE = 8;
};

// Task funkcija — implementirana v event_bus.cpp
void eventBusTask(void* pvParams);
