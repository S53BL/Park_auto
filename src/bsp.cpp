// ============================================================
// bsp.cpp — Board Support Package implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0-dev  |  Datum: 2026-04
// Faza    : 0 — Ekran + touch (Wire1 izklopljen)
// ============================================================
//
// VRSTNI RED INICIALIZACIJE:
//   1. Serial
//   2. Wire (interni bus, IO8/IO7) — za display + touch
//      ⚠ Wire1 ZAKOMENTIRANO — Faza 0
//   3. GPIO direktni pini (MUX, BL, LED, INT pini)
//   4. Wire1 mutex (kreiran, a Wire1 ni inicializiran)
//   5. TWDT watchdog
//   6. FreeRTOS taski
//
// KAJ JE ZAKOMENTIRANO V FAZI 0 (označeno z // FAZA0):
//   - Wire1.begin() — ni naprav na IO17/IO18
//   - TCA9548A hw reset (IO46) — Wire1 naprava
//   - MCP23017 init — Wire1 naprava
//
// PRIČAKOVANI SERIAL LOG:
//   [BSP] === BSP init v2.0.0-dev ===
//   [BSP] Wire interni bus OK (IO8/IO7)
//   [BSP] FAZA0: Wire1 zakomeniran
//   [BSP] GPIO init OK
//   [BSP] Wire1 mutex OK
//   [BSP] TWDT OK
//   [BSP] Taski OK
//   [BSP] === BSP init OK v XXX ms ===
// ============================================================

#include "bsp.h"
#include "web_ui.h"
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

// BSP log makroji so preusmerjeni na centralni logger.
// IZJEMA: Serial.println("\n\n===== BOOT =====") in klic logger_init()
// v bsp_serial_init() tečeta PRED logger_init() — direkten Serial.
// Po logger_init() vsi LOGI/LOGW/LOGE klici gredo skozi logger_log().
#define LOGI(fmt, ...) LOG_INFO ("BSP", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_WARN ("BSP", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_ERROR("BSP", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) LOG_DEBUG("BSP", fmt, ##__VA_ARGS__)

// ============================================================
// GLOBALNE SPREMENLJIVKE
// ============================================================

TaskHandle_t hTaskEventBus = nullptr;
TaskHandle_t hTaskSensor   = nullptr;
TaskHandle_t hTaskLed      = nullptr;
TaskHandle_t hTaskLvgl     = nullptr;
TaskHandle_t hTaskApp      = nullptr;
TaskHandle_t hTaskWifi     = nullptr;

static SemaphoreHandle_t s_wire1_mutex = nullptr;
static bool     s_wire1_ok  = false;
static bool     s_mcp_ok    = false;
static uint32_t s_boot_time = 0;

// ============================================================
// STUB TASKI — __attribute__((weak)) da jih moduli lahko zamenjajo
// ============================================================
// Taski ki še niso implementirani samo spijo.
// lvglTask je implementiran v hal_display.cpp.
// eventBusTask je implementiran v event_bus.cpp.
// sensorTask je implementiran v sensor_mgr.cpp.

__attribute__((weak))
void eventBusTask(void* p) {
    LOGI("eventBusTask stub — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

__attribute__((weak))
void sensorTask(void* p) {
    LOGI("sensorTask stub — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

__attribute__((weak))
void ledTask(void* p) {
    LOGI("ledTask stub — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

__attribute__((weak))
void lvglTask(void* p) {
    LOGI("lvglTask stub — Core%d (zamenjaj z hal_display.cpp)", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

__attribute__((weak))
void appTask(void* p) {
    LOGI("appTask stub — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

__attribute__((weak))
void wifiTask(void* p) {
    LOGI("wifiTask stub — Core%d", xPortGetCoreID());
    // wifiTask ni v TWDT (event-driven)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================
// INTERNE INICIALIZACIJSKE FUNKCIJE
// ============================================================

static void bsp_serial_init() {
    Serial.begin(115200);
    // Čakaj max 3 sekunde da se Serial odpre (USB CDC)
    uint32_t t = millis();
    while (!Serial && (millis() - t) < 3000) {
        delay(10);
    }
    delay(500);  // dodatnih 500ms buffer
    Serial.println("\n\n===== BOOT =====");
    // logger_init() takoj po Serial — Faza 1: Serial + RAM buffer.
    // SD pride pozneje (logger_sd_attach v bsp_sd_init_internal).
    logger_init();
    LOGI("=== BSP init %s ===", FW_VERSION_STRING);
    LOGI("CPU: %d MHz | PSRAM: %lu KB | Flash: %lu KB",
         getCpuFrequencyMhz(),
         (unsigned long)(ESP.getPsramSize() / 1024),
         (unsigned long)(ESP.getFlashChipSize() / 1024));
}

static void bsp_i2c_init() {
    // --- Wire: interni bus (IO8=SDA, IO7=SCL) ---
    // Waveshare BSP naprave: AXS15231B touch, TCA9554, PMU, RTC, IMU, Audio
    // ⚠ IO8=SDA, IO7=SCL — potrjeno iz demo 10_lvgl_arduino_v9.ino
    LOGI("Wire interni bus init (SDA=IO8, SCL=IO7, 400kHz)...");
    Wire.begin(8, 7, 400000);
    LOGI("Wire interni bus OK");

    // --- Wire1: senzorski bus (IO17=SDA, IO18=SCL) ---
    // FAZA0: ZAKOMENTIRANO — TCA9548A, SC16IS752, MCP23017, BH1750
    // niso priklopljeni na IO17/IO18.
    // Odkomentiraj v Fazi 1 ko priklopljiš naprave.
    //
    // LOGI("Wire1 senzorski bus init (SDA=IO17, SCL=IO18, 100kHz)...");
    // Wire1.begin(I2C_SDA, I2C_SCL, I2C_FREQ_HZ);
    // s_wire1_ok = true;
    // LOGI("Wire1 OK");
    //
    s_wire1_ok = false;
    LOGW("FAZA0: Wire1 zakomentirano (IO17/IO18 — nič ni priklopljeno)");

    // Wire1 mutex — kreiran tudi če Wire1 ni aktiven.
    // hal_gpio in hal_light ga pričakujeta. Faza 0: ni Wire1 klicev.
    s_wire1_mutex = xSemaphoreCreateMutex();
    if (!s_wire1_mutex) {
        LOGE("Wire1 mutex NAPAKA — restart!");
        delay(3000);
        ESP.restart();
    }
    LOGI("Wire1 mutex OK");
}

static void bsp_gpio_init() {
    LOGI("GPIO init...");

    // MUX select — LOW = Primary ESP aktiven ob zagonu
    pinMode(PIN_MUX_SELECT, OUTPUT);
    digitalWrite(PIN_MUX_SELECT, LOW);

    // LCD backlight — LOW ob zagonu, hal_display ga prižge ko je ready
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);

    // LED podatkovne linije — OUTPUT LOW
    pinMode(PIN_LED_MAIN,   OUTPUT); digitalWrite(PIN_LED_MAIN,   LOW);
    pinMode(PIN_LED_SIGNAL, OUTPUT); digitalWrite(PIN_LED_SIGNAL, LOW);

    // Interrupt pini — INPUT_PULLUP
    // FAZA0: MCP, SC16 niso priklopljeni, a pine konfiguriramo vseeno
    pinMode(PIN_MCP_INT, INPUT_PULLUP);
    pinMode(PIN_SC1_IRQ, INPUT_PULLUP);
    pinMode(PIN_SC2_IRQ, INPUT_PULLUP);

    // TCA9548A reset pin — HIGH (neaktivno)
    // FAZA0: TCA9548A reset ZAKOMENTIRANO — Wire1 naprava
    // bsp_tca_reset() kliče samo Wire1 zadevno logiko (IO46 je GPIO, OK)
    pinMode(PIN_TCA_RESET, OUTPUT);
    digitalWrite(PIN_TCA_RESET, HIGH);
    // FAZA0: ne kličemo bsp_tca_reset() — nepotrebno brez Wire1

    LOGI("GPIO init OK");
}

static void bsp_sd_init_internal() {
    LOGI("SD_MMC init...");
    bool ok = sd_mgr_init();
    if (ok) {
        LOGI("SD_MMC OK — %llu MB prosto",
             sd_mgr_free_bytes() / (1024ULL * 1024ULL));
    } else {
        LOGW("SD_MMC NAPAKA — sistem dela naprej brez SD (logi samo na Serial)");
        LOGW("  Preveriti: kartica vstavljena? Format FAT32?");
    }
    // Logger Faza 2 — poveži SD. Kreira mutex (scheduler še ne teče).
    // Od tu naprej logger piše: Serial + RAM + SD.
    logger_sd_attach();
    // Web UI — preveri LittleFS assets, ne zažene strežnika (to naredi wifiTask)
    web_ui_init();
}

static void bsp_wdt_init() {
    LOGI("TWDT init (timeout: %ds)...", WDT_TIMEOUT_SEC);
    const esp_task_wdt_config_t cfg = {
        .timeout_ms     = (uint32_t)WDT_TIMEOUT_SEC * 1000U,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOGW("TWDT reconfigure: err=%d", (int)err);
    }
    LOGI("TWDT OK");
}

static void bsp_tasks_create() {
    LOGI("FreeRTOS task kreacija...");
    BaseType_t r;

    #define MAKE_TASK(fn, nm, stk, pri, hdl, core)                              \
        r = xTaskCreatePinnedToCore(fn, nm, stk, nullptr, pri, &hdl, core);     \
        if (r != pdPASS) {                                                       \
            LOGE(nm " task napaka (%d) — restart!", (int)r);                    \
            delay(3000); ESP.restart();                                          \
        }                                                                        \
        LOGI(nm " OK (stack:%d prio:%d Core%d SRAM)", stk, pri, core);

    // Stack v PSRAM — prihranimo ~18KB internega SRAM za WiFi/TCP/SD DMA
    #define MAKE_PSRAM_TASK(fn, nm, stk, pri, hdl, core)                             \
        r = xTaskCreatePinnedToCoreWithCaps(fn, nm, stk, nullptr, pri, &hdl, core,  \
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   \
        if (r != pdPASS) {                                                           \
            LOGE(nm " PSRAM task napaka (%d) — restart!", (int)r);                  \
            delay(3000); ESP.restart();                                              \
        }                                                                            \
        LOGI(nm " OK (stack:%d prio:%d Core%d PSRAM)", stk, pri, core);

    MAKE_PSRAM_TASK(eventBusTask, "EventBus", TASK_EVENTBUS_STACK, TASK_EVENTBUS_PRIO, hTaskEventBus, CORE_APP)
    MAKE_PSRAM_TASK(sensorTask,   "Sensor",   TASK_SENSOR_STACK,   TASK_SENSOR_PRIO,   hTaskSensor,   CORE_APP)
    MAKE_PSRAM_TASK(ledTask,      "LED",      TASK_LED_STACK,      TASK_LED_PRIO,      hTaskLed,      CORE_APP)
    MAKE_TASK      (lvglTask,     "LVGL",     TASK_LVGL_STACK,     TASK_LVGL_PRIO,     hTaskLvgl,     CORE_APP)
    MAKE_PSRAM_TASK(appTask,      "App",      TASK_APP_STACK,      TASK_APP_PRIO,      hTaskApp,      CORE_APP)
    MAKE_TASK      (wifiTask,     "WiFi",     TASK_WIFI_STACK,     TASK_WIFI_PRIO,     hTaskWifi,     CORE_WIFI)

    #undef MAKE_PSRAM_TASK
    #undef MAKE_TASK
    LOGI("Vsi taski OK");
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

void bsp_init() {
    uint32_t t0 = millis();
    bsp_serial_init();
    bsp_i2c_init();
    bsp_gpio_init();
    bsp_sd_init_internal();   // SD_MMC — pred task kreacijo, za GPIO
    bsp_wdt_init();
    bsp_tasks_create();
    s_boot_time = millis() - t0;
    LOGI("Internal SRAM free: %u B  min-ever: %u B",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    LOGI("=== BSP init OK v %lu ms ===", (unsigned long)s_boot_time);
}

bool bsp_wire1_ok()           { return s_wire1_ok; }
bool bsp_mcp_ok()             { return s_mcp_ok; }
SemaphoreHandle_t bsp_get_wire1_mutex() { return s_wire1_mutex; }
uint32_t bsp_boot_time_ms()   { return s_boot_time; }

void bsp_sd_init() {
    // Javni wrapper — omogoča re-init po SD zamenjavi (prihodnja razširitev)
    bsp_sd_init_internal();
}

void bsp_tca_reset() {
    // IO46 je čisti GPIO — dela tudi brez Wire1
    LOGW("TCA9548A reset (IO%d)...", PIN_TCA_RESET);
    digitalWrite(PIN_TCA_RESET, LOW);
    delay(5);
    digitalWrite(PIN_TCA_RESET, HIGH);
    delay(TCA_RECOVERY_WAIT_MS);
    LOGI("TCA9548A reset OK");
}
