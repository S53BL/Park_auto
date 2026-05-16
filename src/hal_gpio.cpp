// ============================================================
// hal_gpio.cpp — MCP23017 GPIO Expander implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// ODGOVORNOST:
//   Implementacija HAL za MCP23017 (0x20) na Wire1 (IO17/IO18).
//   Glej hal_gpio.h za celoten opis arhitekture, pin mape in API.
//
// KLJUČNE IMPLEMENTACIJSKE ODLOČITVE:
//
//   Polling namesto ISR:
//     gpio_install_isr_service() → gpio_isr_register() v IDF 5.3 vedno gre
//     prek esp_ipc_call_blocking(xPortGetCoreID(), ...) → ipc_task (1KB stack)
//     → heap_caps_malloc preseže stack → crash. Rešitev: polling PIN_MCP_INT.
//     hal_gpio_process_queue() preverja digitalRead(PIN_MCP_INT) neposredno.
//
//   INT počistitev — VRSTNI RED JE KRITIČEN (Microchip DS20001952C §3.7):
//     1. Beri INTFA (0x0E) NAJPREJ — kateri pin je sprožil INT.
//        INTFA je veljavno dokler INTCAP ni prebran. Po branju INTCAP se INTFA počisti!
//     2. Beri INTCAPA (0x10) — snapshot vrednosti pinov ob trenutku interrupta.
//        Branje tega registra POČISTI INT signal in INTFA na MCP23017.
//     3. Beri INTCAPB (0x11) — snapshot Port B (za completeness).
//     INTCAP vsebuje vrednosti v TRENUTKU interrupta — ne trenutne vrednosti.
//     To je ključno za kratke signale (fotocelica) ki se vrnejo preden pridemo do branja.
//     NAPAKA ki jo je treba se izogibati: INTCAP pred INTFA → INTFA=0x00 vedno!
//
//   rampaluc retriggerable timeout:
//     Signal fizično utripa ~1 Hz. Vsak LOW prehod (FALLING edge) sproži
//     interrupt in resetira s_rampaluc_timer_ms na millis() + GPIO_RAMPALUC_TIMEOUT_MS.
//     hal_gpio_tick() preveri ali je timeout potekel → pošlje RAMP_MOVING(false).
//     EventBus dobi le en RAMP_MOVING(true) ob začetku in en RAMP_MOVING(false) po koncu.
//     Srednje utripanje → žrtvovano (nobeen event) — to je pravilno obnašanje.
//
//   Shadow register za SSR:
//     hal_gpio_set_ssr() piše v s_ssr_shadow (uint8_t za OLATA, GPB0 za OLATB).
//     Ne beremo MCP nazaj pri vsakem set — to bi podvojilo I2C promet.
//     Ob init() in health-check preverimo ali MCP shadow == fizično stanje.
//
//   Wire1 mutex:
//     Enak mutex kot v hal_tof.cpp in hal_radar.cpp — bsp_get_wire1_mutex().
//     Timeout GPIO_WIRE1_MUTEX_TIMEOUT_MS (200ms). Ob timeout: logiramo WARN,
//     operacija se preskoči. Interrupt event se zavrže — bo prišel zdravstveni check.
//
//   Health-check (watchdog):
//     Vsakih GPIO_WATCHDOG_INTERVAL_MS (10 min) hal_gpio_tick() pokliče
//     do_healthcheck() ki prebere GPIOA/GPIOB in logira stanje vseh pinov.
//     Nameni: potrditev delovanja Wire1+MCP, zaznava fizičnih okvar, diagnostika.
//     Prav tako posodobi s_state z aktualnim stanjem (shadow sync).
//
// LOGGING:
//   INFO : init, SSR spremembe, vhodni prehodi, health-check povzetek
//   WARN : I2C napake, debounce zavrnjeni eventi, Wire1 mutex timeout
//   ERROR: init napaka (MCP ne odgovori), huda I2C okvara
//   DEBUG: raw INTCAP/INTFA vrednosti, per-pin stanje pri health-checku
//
// ============================================================

#include "hal_gpio.h"
#include "event_bus.h"
#include "bsp.h"
#include "logger.h"
#include "config.h"
#include <Wire.h>

// ============================================================
// LOGGING MAKROJI
// ============================================================

#define GPIOI(fmt, ...) LOG_INFO ("GPIO", fmt, ##__VA_ARGS__)
#define GPIOW(fmt, ...) LOG_WARN ("GPIO", fmt, ##__VA_ARGS__)
#define GPIOE(fmt, ...) LOG_ERROR("GPIO", fmt, ##__VA_ARGS__)
#define GPIOD(fmt, ...) LOG_DEBUG("GPIO", fmt, ##__VA_ARGS__)

// ============================================================
// MCP23017 REGISTER NASLOVI (IOCON.BANK=0, privzeto)
// ============================================================
// Dokumentacija: Microchip DS20001952C, tabela 3-1.
// BANK=0 (privzeto): Port A in Port B registri so prepleteni.

#define MCP_REG_IODIRA      0x00    // I/O smer Port A (1=IN, 0=OUT)
#define MCP_REG_IODIRB      0x01    // I/O smer Port B
#define MCP_REG_IPOLA       0x02    // Polarnost vhoda Port A (1=invertiran)
#define MCP_REG_IPOLB       0x03    // Polarnost vhoda Port B
#define MCP_REG_GPINTENA    0x04    // Interrupt-on-change enable Port A
#define MCP_REG_GPINTENB    0x05    // Interrupt-on-change enable Port B
#define MCP_REG_DEFVALA     0x06    // Default primerjalna vrednost Port A
#define MCP_REG_DEFVALB     0x07    // Default primerjalna vrednost Port B
#define MCP_REG_INTCONA     0x08    // INT control Port A (0=sprememba, 1=vs DEFVAL)
#define MCP_REG_INTCONB     0x09    // INT control Port B
#define MCP_REG_IOCON       0x0A    // I/O expander konfiguracija
#define MCP_REG_GPPUA       0x0C    // Pull-up Port A
#define MCP_REG_GPPUB       0x0D    // Pull-up Port B
#define MCP_REG_INTFA       0x0E    // Interrupt flag Port A — kateri pin je sprožil
#define MCP_REG_INTFB       0x0F    // Interrupt flag Port B
#define MCP_REG_INTCAPA     0x10    // Interrupt capture Port A — snapshot ob INT (POČISTI INT!)
#define MCP_REG_INTCAPB     0x11    // Interrupt capture Port B
#define MCP_REG_GPIOA       0x12    // GPIO stanje Port A (branje = fizično stanje)
#define MCP_REG_GPIOB       0x13    // GPIO stanje Port B
#define MCP_REG_OLATA       0x14    // Output latch Port A (pisanje = izhodni pin)
#define MCP_REG_OLATB       0x15    // Output latch Port B

// ============================================================
// MCP23017 PIN MASKE — Port A
// ============================================================
// Definirane tudi v config.h — tukaj lokalne za berljivost implementacije.
// Vrednosti morajo biti enake kot v config.h!

#define PIN_RAMPAGOR    0x01    // GPA0 — IN  — rampa dvignjena (LOW = aktivno)
#define PIN_RAMPALUC    0x02    // GPA1 — IN  — opozorilna luč (LOW = aktivno, utripa)
#define PIN_VRATAOD     0x04    // GPA2 — IN  — drsna vrata odprta (LOW = aktivno)
#define PIN_CELICA1     0x08    // GPA3 — IN  — zunanja fotocelica (LOW = prekinjena)
#define PIN_CELICA2     0x10    // GPA4 — IN  — notranja fotocelica (LOW = prekinjena)
#define PIN_SSR1        0x20    // GPA5 — OUT — 12V trafo za LED matriko
#define PIN_SSR2        0x40    // GPA6 — OUT — 3× LED panel
#define PIN_SSR3        0x80    // GPA7 — OUT — reflektor pred garažo

// Maska vseh vhodov Port A (GPA0–4)
#define PORTA_INPUT_MASK    0x1F
// Maska vseh izhodov Port A (GPA5–7)
#define PORTA_OUTPUT_MASK   0xE0

// ============================================================
// MCP23017 PIN MASKA — Port B
// ============================================================

#define PIN_SSR4        0x01    // GPB0 — OUT — reflektor pred lopo
// GPB1–7 rezerva — konfigurirani kot INPUT_PULLUP za varnost

// ============================================================
// INTERNO STANJE
// ============================================================

// Shadow register za izhode — ne beremo MCP nazaj pri vsakem set.
// Bit=1 pomeni SSR vklopljen (HIGH na MCP pinu).
// Inicializirano na 0 ob init() — vsi SSR izklopljeni.
static uint8_t s_ssr_shadow_a = 0;     // GPA5(SSR1), GPA6(SSR2), GPA7(SSR3)
static uint8_t s_ssr_shadow_b = 0;     // GPB0(SSR4)

// Logično stanje vhodov — posodobljeno ob vsakem interrupt + health-check.
// Aktivno = true, neaktivno = false (logična invertzija fizičnega LOW=aktiven)
static volatile bool s_rampagor  = false;
static volatile bool s_rampaluc  = false;   // kolapsirani (ne surovo utripanje)
static volatile bool s_vrataod   = false;
static volatile bool s_celica1   = false;
static volatile bool s_celica2   = false;

// rampaluc retriggerable timeout
// s_rampaluc_deadline_ms: čas ko se rampaluc_active postavi na false.
// 0 = timeout ni aktiven (rampaluc ni aktiven).
static volatile uint32_t s_rampaluc_deadline_ms = 0;

// Debounce timestamp — zadnji potrjeni prehod za vsak vhod.
// Preprečuje lažne sprožitve pri mehansih stikal.
static uint32_t s_last_rampagor_ms = 0;
static uint32_t s_last_vrataod_ms  = 0;
static uint32_t s_last_celica1_ms  = 0;
static uint32_t s_last_celica2_ms  = 0;

// Health-check timer
static uint32_t s_last_watchdog_ms = 0;

// Diagnostični števci
static uint32_t s_interrupt_count = 0;  // število polling zaznav INT LOW
static uint32_t          s_debounce_reject_count  = 0;
static uint32_t          s_i2c_error_count        = 0;

// Zadnje raw vrednosti GPIOA/B (za diagnostiko)
static uint8_t s_raw_gpioa = 0xFF;  // 0xFF = neznano (pullup default)
static uint8_t s_raw_gpiob = 0xFF;

// Inicializacijski flag
static bool s_initialized  = false;
static bool s_mcp_detected = false;

// Wire1 mutex timeout napake — skupno število od zagona
// Thread-safe: uint32_t read/write je atomaren na ESP32.
static uint32_t s_wire1_errors = 0;

// ============================================================
// I2C PRIMITIVNE FUNKCIJE — kliči SAMO pod Wire1 mutex
// ============================================================

// Napiši en register MCP23017.
// Vrne true ob uspehu (Wire.endTransmission() == 0).
static bool mcp_write_reg(uint8_t reg, uint8_t value) {
    Wire1.beginTransmission(I2C_ADDR_MCP23017);
    Wire1.write(reg);
    Wire1.write(value);
    uint8_t err = Wire1.endTransmission();
    if (err != 0) {
        s_i2c_error_count++;
        GPIOW("mcp_write_reg(0x%02X, 0x%02X) napaka err=%d", reg, value, (int)err);
        return false;
    }
    return true;
}

// Preberi en register MCP23017.
// Vrne prebrano vrednost ali 0xFF ob napaki (I2C error ali timeout).
// Ob napaki logira WARN in inkrementira error counter.
static uint8_t mcp_read_reg(uint8_t reg) {
    Wire1.beginTransmission(I2C_ADDR_MCP23017);
    Wire1.write(reg);
    uint8_t err = Wire1.endTransmission(false);  // repeated START za branje
    if (err != 0) {
        s_i2c_error_count++;
        GPIOW("mcp_read_reg(0x%02X) endTransmission napaka err=%d", reg, (int)err);
        return 0xFF;
    }
    uint8_t n = Wire1.requestFrom((uint8_t)I2C_ADDR_MCP23017, (uint8_t)1);
    if (n != 1) {
        s_i2c_error_count++;
        GPIOW("mcp_read_reg(0x%02X) requestFrom napaka — vrnjenih %d bajtov", reg, (int)n);
        return 0xFF;
    }
    return Wire1.read();
}

// Ping — preveri ali MCP23017 odgovarja na I2C.
static bool mcp_ping() {
    Wire1.beginTransmission(I2C_ADDR_MCP23017);
    return (Wire1.endTransmission() == 0);
}

// ============================================================
// OBDELAVA VHODA — debounce in EventBus dispatch
// ============================================================
// Kliče se iz hal_gpio_process_queue() ko je ugotovljeno kateri pin je sprožil INT.
//
// intcap_a : snapshot Port A ob trenutku interrupta (iz INTCAPA registra)
// pin_mask : bitna maska pina ki je sprožil (iz INTFA registra)
//
// Logika: LOW = aktiven signal (INPUT_PULLUP). Bit=0 v intcap_a → pin LOW → aktiven.
// Debounce: zavrnemo event če je prišel prekmalu po zadnjem potrjenem eventu.
// Za rampaluc: ne debounce (retriggerable timeout), ne logirajmo vsakega utripa.

static void handle_pin_change(uint8_t intcap_a, uint8_t pin_mask) {
    uint32_t now = millis();

    // Aktivno = LOW (pullup logika) → bit = 0 v intcap_a pomeni aktiven
    bool active = !(intcap_a & pin_mask);

    GPIOD("handle_pin_change: mask=0x%02X intcap_a=0x%02X active=%d",
          pin_mask, intcap_a, (int)active);

    // ------------------------------------------------------------------
    // GPA0 — rampagor
    // ------------------------------------------------------------------
    if (pin_mask & PIN_RAMPAGOR) {
        if ((now - s_last_rampagor_ms) < GPIO_DEBOUNCE_RAMPAGOR_MS) {
            s_debounce_reject_count++;
            GPIOD("rampagor: debounce zavrnjen (%lu ms od zadnjega)", (unsigned long)(now - s_last_rampagor_ms));
            return;
        }
        s_last_rampagor_ms = now;
        s_rampagor = active;

        if (active) {
            GPIOI("RAMPAGOR: rampa DVIGNJENA → RAMP_UP");
            EventBus::publish(EventType::RAMP_UP, 1);
        } else {
            GPIOI("RAMPAGOR: rampa SPUŠČENA → RAMP_UP(0)");
            EventBus::publish(EventType::RAMP_UP, 0);
        }
        return;
    }

    // ------------------------------------------------------------------
    // GPA1 — rampaluc (utripajoči signal — retriggerable timeout)
    // ------------------------------------------------------------------
    if (pin_mask & PIN_RAMPALUC) {
        // Upoštevamo SAMO padajočo fronto (aktiven = LOW = bit=0)
        // Naredimo razliko med "utrip" (aktiven) in "utrip konec" (neaktiven).
        // Za retriggerable logiko nas zanima samo aktiven prehod.
        if (!active) {
            // Rastoča fronta (LOW→HIGH) = konec enega utripa — ignoriraj.
            // Timeout bo zaznao konec utripanja sam.
            GPIOD("rampaluc: HIGH fronta ignorirana (retriggerable timer teče)");
            return;
        }

        // Aktiven utrip (HIGH→LOW):
        bool was_active = (s_rampaluc_deadline_ms != 0);

        // Resetiraj / postavi timeout
        s_rampaluc_deadline_ms = now + GPIO_RAMPALUC_TIMEOUT_MS;
        s_rampaluc = true;

        if (!was_active) {
            // Prve aktivacija — pošlji event RAMP_MOVING(true)
            GPIOI("RAMPALUC: rampa začela gibanje → RAMP_MOVING(true)");
            EventBus::publish(EventType::RAMP_MOVING, 1);
        } else {
            // Vmesni utrip — samo osvežimo timer, brez EventBus klica
            GPIOD("RAMPALUC: vmesni utrip — timer podaljšan na +%dms", GPIO_RAMPALUC_TIMEOUT_MS);
        }
        return;
    }

    // ------------------------------------------------------------------
    // GPA2 — vrataod
    // ------------------------------------------------------------------
    if (pin_mask & PIN_VRATAOD) {
        if ((now - s_last_vrataod_ms) < GPIO_DEBOUNCE_VRATAOD_MS) {
            s_debounce_reject_count++;
            GPIOD("vrataod: debounce zavrnjen (%lu ms)", (unsigned long)(now - s_last_vrataod_ms));
            return;
        }
        s_last_vrataod_ms = now;
        s_vrataod = active;

        if (active) {
            GPIOI("VRATAOD: vrata ODPRTA → DOOR_OPENED(1)");
            EventBus::publish(EventType::DOOR_OPENED, 1);
        } else {
            GPIOI("VRATAOD: vrata ZAPRTA → DOOR_OPENED(0)");
            EventBus::publish(EventType::DOOR_OPENED, 0);
        }
        return;
    }

    // ------------------------------------------------------------------
    // GPA3 — celica1 (zunanja fotocelica)
    // ------------------------------------------------------------------
    if (pin_mask & PIN_CELICA1) {
        if ((now - s_last_celica1_ms) < GPIO_DEBOUNCE_CELICA_MS) {
            s_debounce_reject_count++;
            GPIOD("celica1: debounce zavrnjen (%lu ms)", (unsigned long)(now - s_last_celica1_ms));
            return;
        }
        s_last_celica1_ms = now;
        s_celica1 = active;

        // Payload: bit 0 = celica1, bit 1 = celica2
        // Subscribers (light_logic) morejo vedeti katera celica je sprožila
        uint32_t payload = active ? 0x01 : 0x00;
        if (active) {
            GPIOI("CELICA1: zunanja fotocelica PREKINJENA → CELL_BROKEN(0x01)");
        } else {
            GPIOI("CELICA1: zunanja fotocelica OBNOVLJENA → CELL_BROKEN(0x00)");
        }
        EventBus::publish(EventType::CELL_BROKEN, payload);
        return;
    }

    // ------------------------------------------------------------------
    // GPA4 — celica2 (notranja fotocelica)
    // ------------------------------------------------------------------
    if (pin_mask & PIN_CELICA2) {
        if ((now - s_last_celica2_ms) < GPIO_DEBOUNCE_CELICA_MS) {
            s_debounce_reject_count++;
            GPIOD("celica2: debounce zavrnjen (%lu ms)", (unsigned long)(now - s_last_celica2_ms));
            return;
        }
        s_last_celica2_ms = now;
        s_celica2 = active;

        // bit 1 = celica2
        uint32_t payload = active ? 0x02 : 0x00;
        if (active) {
            GPIOI("CELICA2: notranja fotocelica PREKINJENA → CELL_BROKEN(0x02)");
        } else {
            GPIOI("CELICA2: notranja fotocelica OBNOVLJENA → CELL_BROKEN(0x00)");
        }
        EventBus::publish(EventType::CELL_BROKEN, payload);
        return;
    }

    // Neznan pin — ne bi se zgodilo pri pravilni konfiguraciji GPINTENA
    GPIOW("handle_pin_change: neznan pin mask=0x%02X — preveriti GPINTENA konfiguracijo", pin_mask);
}

// ============================================================
// HEALTH-CHECK — interno
// ============================================================
// Prebere GPIOA in GPIOB ter logira dejansko stanje vseh pinov.
// Posodobi shadow register in s_raw_gpioa/b za diagnostiko.
// Kliče se pod Wire1 mutex — ne kliči ločeno!

static void do_healthcheck_locked() {
    uint8_t gpioa = mcp_read_reg(MCP_REG_GPIOA);
    uint8_t gpiob = mcp_read_reg(MCP_REG_GPIOB);
    uint8_t olata = mcp_read_reg(MCP_REG_OLATA);
    uint8_t olatb = mcp_read_reg(MCP_REG_OLATB);

    if (gpioa == 0xFF && gpiob == 0xFF) {
        // Verjetno I2C napaka (0xFF je privzeta vrednost ob napaki v mcp_read_reg)
        GPIOW("Health-check: I2C napaka (oba porta vrnila 0xFF) — preveriti bus");
        return;
    }

    s_raw_gpioa = gpioa;
    s_raw_gpiob = gpiob;

    // Posodobi logično stanje iz fizičnega — INPUT_PULLUP, LOW=aktiven
    s_rampagor = !(gpioa & PIN_RAMPAGOR);
    // Pozor: s_rampaluc ne beremo iz GPIO direktno — upravljan z retriggerable timerom.
    // Fizično stanje GPA1 samo logiramo za diagnostiko.
    s_vrataod  = !(gpioa & PIN_VRATAOD);
    s_celica1  = !(gpioa & PIN_CELICA1);
    s_celica2  = !(gpioa & PIN_CELICA2);

    // Preveri ujemanje shadow registra z dejanskim stanjem izhodov
    bool shadow_mismatch = false;
    if ((olata & PORTA_OUTPUT_MASK) != (s_ssr_shadow_a & PORTA_OUTPUT_MASK)) {
        GPIOW("Health-check: OLATA shadow mismatch! shadow=0x%02X fizično=0x%02X — popravljam",
              s_ssr_shadow_a, olata);
        // Popravi MCP na shadow vrednost (shadow je resnica)
        mcp_write_reg(MCP_REG_OLATA, s_ssr_shadow_a);
        shadow_mismatch = true;
    }
    if ((olatb & 0x01) != (s_ssr_shadow_b & 0x01)) {
        GPIOW("Health-check: OLATB shadow mismatch! shadow=0x%02X fizično=0x%02X — popravljam",
              s_ssr_shadow_b, olatb);
        mcp_write_reg(MCP_REG_OLATB, s_ssr_shadow_b);
        shadow_mismatch = true;
    }

    // Log — INFO za health-check summary, DEBUG za per-pin detail
    GPIOI("=== GPIO health-check === GPIOA=0x%02X GPIOB=0x%02X uptime:%lus%s",
          gpioa, gpiob,
          (unsigned long)(millis() / 1000),
          shadow_mismatch ? " [SHADOW MISMATCH POPRAVLJEN]" : "");

    GPIOD("  Vhodi: rampagor=%s rampaluc_phy=%s vrataod=%s celica1=%s celica2=%s",
          s_rampagor ? "AKTIVEN" : "neakt.",
          (gpioa & PIN_RAMPALUC) ? "HIGH" : "LOW",     // fizično stanje GPA1
          s_vrataod  ? "AKTIVEN" : "neakt.",
          s_celica1  ? "AKTIVEN" : "neakt.",
          s_celica2  ? "AKTIVEN" : "neakt.");

    GPIOD("  Izhodi: SSR1=%s SSR2=%s SSR3=%s SSR4=%s",
          (s_ssr_shadow_a & PIN_SSR1) ? "ON" : "off",
          (s_ssr_shadow_a & PIN_SSR2) ? "ON" : "off",
          (s_ssr_shadow_a & PIN_SSR3) ? "ON" : "off",
          (s_ssr_shadow_b  & PIN_SSR4) ? "ON" : "off");

    GPIOD("  Stats: int=%lu debReject=%lu i2cErr=%lu",
          (unsigned long)s_interrupt_count,
          (unsigned long)s_debounce_reject_count,
          (unsigned long)s_i2c_error_count);

    // Opozorilo če so vsi SSR vklopljeni — nenavadna situacija
    if ((s_ssr_shadow_a & PORTA_OUTPUT_MASK) == PORTA_OUTPUT_MASK &&
        (s_ssr_shadow_b & PIN_SSR4)) {
        GPIOW("Health-check: VSI SSR vklopljeni hkrati — preveriti light_logic");
    }
}

// ============================================================
// hal_gpio_init
// ============================================================

bool hal_gpio_init() {
    GPIOI("=== hal_gpio_init ===");
    GPIOI("MCP23017 @ 0x%02X | Wire1 SDA=IO%d SCL=IO%d | INT=IO%d",
          I2C_ADDR_MCP23017, I2C_SDA, I2C_SCL, PIN_MCP_INT);

    // Predpogoj: Wire1 mora biti inicializiran v bsp_init()
    if (!bsp_wire1_ok()) {
        GPIOE("Wire1 ni inicializiran — bsp_init() mora biti klican prej!");
        return false;
    }

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(GPIO_WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        GPIOE("Wire1 mutex timeout pri init — napaka BSP?");
        return false;
    }

    // --- Ping: preverimo ali MCP23017 odgovarja ---
    if (!mcp_ping()) {
        xSemaphoreGive(mtx);
        GPIOE("MCP23017 (0x%02X) ne odgovarja! Preveriti: napajanje 3.3V, RESET=3.3V, pull-up 4.7kΩ na SDA/SCL, pini IO%d/IO%d",
              I2C_ADDR_MCP23017, I2C_SDA, I2C_SCL);
        return false;
    }
    s_mcp_detected = true;
    GPIOI("MCP23017 ping OK");

    // --- IOCON: konfiguracija razširjevalnika ---
    // Bit 6 (MIRROR=0): INTA in INTB sta neodvisna (mi vežemo samo INTA)
    // Bit 2 (ODR=0): INTA je active-driver (ne open-drain)
    // Bit 1 (INTPOL=0): INTA je aktiven LOW (padajoča fronta na IO47)
    // Privzeta vrednost 0x00 ustreza vsem tem zahtevam — ni treba pisati.
    // Eksplicitno pišemo za jasnost in robustnost (power-cycle recovery):
    if (!mcp_write_reg(MCP_REG_IOCON, 0x00)) {
        xSemaphoreGive(mtx);
        GPIOE("IOCON write napaka");
        return false;
    }

    // --- IODIRA: smer Port A ---
    // GPA0–4 vhodi (1), GPA5–7 izhodi (0)
    // PORTA_INPUT_MASK = 0x1F = 0b00011111
    if (!mcp_write_reg(MCP_REG_IODIRA, PORTA_INPUT_MASK)) {
        xSemaphoreGive(mtx);
        GPIOE("IODIRA write napaka");
        return false;
    }

    // --- IODIRB: smer Port B ---
    // GPB0 izhod (0), GPB1–7 rezerva kot vhodi (1) — varno, ne lebdijo
    // MCP_PORTB_IODIR = 0xFE (iz config.h)
    if (!mcp_write_reg(MCP_REG_IODIRB, MCP_PORTB_IODIR)) {
        xSemaphoreGive(mtx);
        GPIOE("IODIRB write napaka");
        return false;
    }

    // --- GPPUA: pull-up Port A ---
    // Samo na vhodih (GPA0–4) — pullup na izhodih je nevtralen a ga pustimo izklopljeno
    if (!mcp_write_reg(MCP_REG_GPPUA, PORTA_INPUT_MASK)) {
        xSemaphoreGive(mtx);
        GPIOE("GPPUA write napaka");
        return false;
    }

    // --- GPPUB: pull-up Port B ---
    // GPB1–7 rezerva — pullup da ne lebdijo. GPB0 je izhod — brez pull-up.
    if (!mcp_write_reg(MCP_REG_GPPUB, 0xFE)) {
        xSemaphoreGive(mtx);
        GPIOE("GPPUB write napaka");
        return false;
    }

    // --- INTCONA: interrupt control Port A ---
    // 0x00 = interrupt ob spremembi (compare-to-previous, ne compare-to-DEFVAL)
    // Sprememba v katerikoli smeri sproži INT — idealno za vhode z aktivno LOW logiko
    if (!mcp_write_reg(MCP_REG_INTCONA, 0x00)) {
        xSemaphoreGive(mtx);
        GPIOE("INTCONA write napaka");
        return false;
    }

    // --- GPINTENA: interrupt enable Port A ---
    // Samo vhodni pini (GPA0–4) generirajo interrupt.
    // Izhodi (GPA5–7) ne smejo — feedback loop nevarnost.
    if (!mcp_write_reg(MCP_REG_GPINTENA, PORTA_INPUT_MASK)) {
        xSemaphoreGive(mtx);
        GPIOE("GPINTENA write napaka");
        return false;
    }

    // Port B nima vhodov ki bi sprožali INT (GPB0 je izhod, GPB1–7 rezerva)
    if (!mcp_write_reg(MCP_REG_GPINTENB, 0x00)) {
        xSemaphoreGive(mtx);
        GPIOE("GPINTENB write napaka");
        return false;
    }
    GPIOI("MCP23017 registri konfigurirani (IODIRA/B, GPPUA/B, GPINTENA/B)");

    // --- Inicializiraj izhode (vsi SSR izklopljeni) ---
    s_ssr_shadow_a = 0x00;
    s_ssr_shadow_b = 0x00;
    if (!mcp_write_reg(MCP_REG_OLATA, 0x00)) {
        xSemaphoreGive(mtx);
        GPIOE("OLATA init write napaka");
        return false;
    }
    if (!mcp_write_reg(MCP_REG_OLATB, 0x00)) {
        xSemaphoreGive(mtx);
        GPIOE("OLATB init write napaka");
        return false;
    }
    GPIOI("Vsi SSR izklopljeni ob zagonu");

    // --- Počisti morebitni pending INT pred priklopom ISR ---
    // INTCAPA branje počisti INT signal — naredimo to PRED attachInterrupt.
    // Prepreči lažni takoj-interrupt ob zagonu.
    uint8_t intcap_a_clr = mcp_read_reg(MCP_REG_INTCAPA);
    uint8_t intcap_b_clr = mcp_read_reg(MCP_REG_INTCAPB);

    // --- Preberi začetno stanje vhodov ---
    // Sinhroniziramo interno stanje s fizičnim pred prvim interruptom.
    uint8_t gpioa = mcp_read_reg(MCP_REG_GPIOA);
    uint8_t gpiob = mcp_read_reg(MCP_REG_GPIOB);
    s_raw_gpioa = gpioa;
    s_raw_gpiob = gpiob;

    // Logična invertzija: LOW = aktiven (INPUT_PULLUP)
    s_rampagor = !(gpioa & PIN_RAMPAGOR);
    s_rampaluc = false;  // upravljan z retriggerable timerom, ne direktno iz GPIO
    s_vrataod  = !(gpioa & PIN_VRATAOD);
    s_celica1  = !(gpioa & PIN_CELICA1);
    s_celica2  = !(gpioa & PIN_CELICA2);

    xSemaphoreGive(mtx);

    GPIOI("Začetno stanje: GPIOA=0x%02X GPIOB=0x%02X", gpioa, gpiob);
    GPIOI("  rampagor=%s vrataod=%s celica1=%s celica2=%s (rampaluc=neaktiven ob init)",
          s_rampagor ? "AKTIVEN" : "neakt.",
          s_vrataod  ? "AKTIVEN" : "neakt.",
          s_celica1  ? "AKTIVEN" : "neakt.",
          s_celica2  ? "AKTIVEN" : "neakt.");

    // INT pin (IO%d) je INPUT_PULLUP — hal_gpio_process_queue() ga polira.
    // gpio_install_isr_service/attachInterrupt nista možna z IDF 5.3 pioarduino:
    // gpio_isr_register() vedno gre prek esp_ipc_call_blocking → ipc_task (1KB) overflow.
    GPIOI("INT polling aktiven na IO%d (ni attachInterrupt — IDF 5.3 IPC omejitev)", PIN_MCP_INT);

    // Offset +120s: GPIO health-check pride 2 minuti za radarjem in
    // 1 minuto za TOF watchdog-om. Prepreči Wire1 mutex contention.
    // Radar=0s offset, TOF=+60s offset, GPIO=+120s offset.
    s_last_watchdog_ms = millis() + 120000;

    s_initialized = true;
    GPIOI("hal_gpio_init OK");
    return true;
}

// ============================================================
// hal_gpio_ok
// ============================================================

bool hal_gpio_ok() {
    return s_initialized && s_mcp_detected;
}

// ============================================================
// hal_gpio_process_queue
// ============================================================
// Kliči iz EventBus taska — ne iz ISR!
// Non-blocking: vrne takoj ko ni več čakajočih interruptov v queue.

void hal_gpio_process_queue() {
    if (!s_initialized) return;

    // Polling PIN_MCP_INT — LOW pomeni MCP23017 ima pending interrupt.
    if (digitalRead(PIN_MCP_INT) != LOW) return;

    s_interrupt_count++;

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(GPIO_WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        GPIOW("process_queue: Wire1 mutex timeout — INT event preskočen");
        return;
    }

    // VRSTNI RED BRANJA — Microchip DS20001952C §3.7:
    // 1. INTFA najprej (velja dokler INTCAP ni prebran)
    // 2. INTCAPA — počisti INT signal in INTFA
    // 3. INTCAPB — za completeness
    uint8_t intfa    = mcp_read_reg(MCP_REG_INTFA);
    uint8_t intcap_a = mcp_read_reg(MCP_REG_INTCAPA);
    uint8_t intcap_b = mcp_read_reg(MCP_REG_INTCAPB);

    xSemaphoreGive(mtx);

    GPIOD("INT polling: INTFA=0x%02X INTCAPA=0x%02X INTCAPB=0x%02X",
          intfa, intcap_a, intcap_b);

    uint8_t active_pins = intfa & PORTA_INPUT_MASK;

    if (active_pins == 0 && intcap_a != 0xFF) {
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(GPIO_WIRE1_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            uint8_t current_gpio = mcp_read_reg(MCP_REG_GPIOA);
            xSemaphoreGive(mtx);
            active_pins = (~intcap_a) & PORTA_INPUT_MASK;
            if (active_pins == 0) {
                GPIOD("INT fallback: brez razlike (INTCAPA=0x%02X current=0x%02X) — ignoriram",
                      intcap_a, current_gpio);
                return;
            }
            GPIOW("INT fallback: INTFA=0 a INTCAPA=0x%02X → active_pins=0x%02X",
                  intcap_a, active_pins);
        } else {
            GPIOW("INT fallback: mutex timeout — event preskočen");
            return;
        }
    } else if (active_pins == 0) {
        GPIOD("INT brez aktivnih pinov (INTFA=0x%02X) — ignoriram", intfa);
        return;
    }

    const uint8_t input_pins[] = {
        PIN_RAMPAGOR, PIN_RAMPALUC, PIN_VRATAOD, PIN_CELICA1, PIN_CELICA2
    };
    for (uint8_t mask : input_pins) {
        if (active_pins & mask) {
            handle_pin_change(intcap_a, mask);
        }
    }
}

// ============================================================
// hal_gpio_tick
// ============================================================
// Kliči iz EventBus taska, periodično (~50ms interval).
// Upravlja: rampaluc retriggerable timeout + health-check timer.

void hal_gpio_tick() {
    if (!s_initialized) return;

    uint32_t now = millis();

    // ----------------------------------------------------------
    // rampaluc retriggerable timeout
    // ----------------------------------------------------------
    // Preveri ali je deadline potekel medtem ko timer teče
    if (s_rampaluc_deadline_ms != 0 && now >= s_rampaluc_deadline_ms) {
        // Timeout — rampa se je nehala premikati
        s_rampaluc         = false;
        s_rampaluc_deadline_ms = 0;

        GPIOI("RAMPALUC: timeout — rampa se je ustavila → RAMP_MOVING(false)");
        EventBus::publish(EventType::RAMP_MOVING, 0);
    }

    // ----------------------------------------------------------
    // Health-check timer (10 minut)
    // ----------------------------------------------------------
    if ((now - s_last_watchdog_ms) >= GPIO_WATCHDOG_INTERVAL_MS) {
        s_last_watchdog_ms = now;
        hal_gpio_force_healthcheck();
    }
}

// ============================================================
// hal_gpio_set_ssr
// ============================================================

bool hal_gpio_set_ssr(uint8_t ssr_idx, bool on) {
    if (!s_initialized) {
        GPIOW("set_ssr(%d, %s): hal_gpio ni inicializiran", ssr_idx, on ? "ON" : "off");
        return false;
    }
    if (ssr_idx < 1 || ssr_idx > 4) {
        GPIOW("set_ssr: neveljaven indeks %d (veljavno: 1–4)", ssr_idx);
        return false;
    }

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    // Wire1 mutex timeout: 500ms (povečano iz 200ms, 2026-05).
    // Razlog: process_chip_irq() drain zanka (max_loops=32) drži Wire1
    //   do 96ms. WiFi init zasede Wire1 do ~150ms. Skupaj worst-case
    //   ~250ms → 200ms timeout je premalo.
    // 500ms zagotavlja uspešen SSR vklop tudi ob Wire1 contention.
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        s_wire1_errors++;
        GPIOW("set_ssr(%d): Wire1 mutex timeout (skupno: %lu)",
              ssr_idx, (unsigned long)s_wire1_errors);
        return false;
    }

    bool ok = true;

    switch (ssr_idx) {
        case SSR_IDX_1:  // GPA5
            if (on) s_ssr_shadow_a |=  PIN_SSR1;
            else    s_ssr_shadow_a &= ~PIN_SSR1;
            ok = mcp_write_reg(MCP_REG_OLATA, s_ssr_shadow_a);
            break;
        case SSR_IDX_2:  // GPA6
            if (on) s_ssr_shadow_a |=  PIN_SSR2;
            else    s_ssr_shadow_a &= ~PIN_SSR2;
            ok = mcp_write_reg(MCP_REG_OLATA, s_ssr_shadow_a);
            break;
        case SSR_IDX_3:  // GPA7
            if (on) s_ssr_shadow_a |=  PIN_SSR3;
            else    s_ssr_shadow_a &= ~PIN_SSR3;
            ok = mcp_write_reg(MCP_REG_OLATA, s_ssr_shadow_a);
            break;
        case SSR_IDX_4:  // GPB0
            if (on) s_ssr_shadow_b |=  PIN_SSR4;
            else    s_ssr_shadow_b &= ~PIN_SSR4;
            ok = mcp_write_reg(MCP_REG_OLATB, s_ssr_shadow_b);
            break;
    }

    xSemaphoreGive(mtx);

    if (ok) {
        GPIOI("SSR%d: %s (OLAT%s = 0x%02X)",
              ssr_idx,
              on ? "VKLOPLJEN" : "izklopljen",
              (ssr_idx <= 3) ? "A" : "B",
              (ssr_idx <= 3) ? s_ssr_shadow_a : s_ssr_shadow_b);
    } else {
        GPIOW("SSR%d: I2C napaka pri pisanju", ssr_idx);
    }

    return ok;
}

// ============================================================
// hal_gpio_get_state
// ============================================================

GpioState hal_gpio_get_state() {
    GpioState st;
    // Kratka atomska kopija — volatile bool branje je dovolj atomsko na ESP32
    st.rampagor = s_rampagor;
    st.rampaluc = s_rampaluc;
    st.vrataod  = s_vrataod;
    st.celica1  = s_celica1;
    st.celica2  = s_celica2;
    st.ssr[0]   = false;  // indeks 0 neuporabljen
    st.ssr[1]   = (s_ssr_shadow_a & PIN_SSR1) != 0;
    st.ssr[2]   = (s_ssr_shadow_a & PIN_SSR2) != 0;
    st.ssr[3]   = (s_ssr_shadow_a & PIN_SSR3) != 0;
    st.ssr[4]   = (s_ssr_shadow_b  & PIN_SSR4) != 0;
    st.last_update_ms = millis();
    return st;
}

// ============================================================
// hal_gpio_get_diagnostics
// ============================================================

GpioDiagnostics hal_gpio_get_diagnostics() {
    GpioDiagnostics d;
    d.mcp_ok               = s_mcp_detected;
    d.initialized          = s_initialized;
    d.interrupt_count      = s_interrupt_count;
    d.debounce_reject_count = s_debounce_reject_count;
    d.i2c_error_count      = s_i2c_error_count;
    d.last_watchdog_ms     = s_last_watchdog_ms;
    d.raw_gpioa            = s_raw_gpioa;
    d.raw_gpiob            = s_raw_gpiob;
    d.state                = hal_gpio_get_state();
    return d;
}

// ============================================================
// hal_gpio_force_healthcheck
// ============================================================

void hal_gpio_force_healthcheck() {
    if (!s_initialized) {
        GPIOW("force_healthcheck: hal_gpio ni inicializiran");
        return;
    }

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(GPIO_WIRE1_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        GPIOW("force_healthcheck: Wire1 mutex timeout");
        return;
    }

    do_healthcheck_locked();

    xSemaphoreGive(mtx);
}

// ============================================================
// hal_gpio_get_wire1_errors
// ============================================================

uint32_t hal_gpio_get_wire1_errors() {
    return s_wire1_errors;
}
