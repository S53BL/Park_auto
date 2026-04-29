// ============================================================
// hal_radar_test.cpp — SC16IS752 Hardware Test Implementacija
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-test  |  Datum: 2026-04
// ============================================================
//
// HARDWARE KI GA TESTIRAMO:
//
//   SC16IS752 #1 @ 0x48  (A0=VCC, A1=GND)
//     UART-A → LD2410C #1  "Vhod"    (vhod hiše)
//     UART-B → LD2410C #2  "Cesta_L" (prihod iz ceste, leva)
//     IRQ    → IO41
//
//   SC16IS752 #2 @ 0x4C  (A0=VCC, A1=VCC)  ← PRIKLJUČEN
//     UART-A → LD2410C #3  "Cesta_D" (prihod iz ceste, desna)
//     UART-B → LD2410C #4  "Garaza"  (garaža / parking)
//     IRQ    → IO42
//
//   Wire1: SDA=IO17, SCL=IO18, 100kHz
//   LD2410C: 115200 baud, 8N1
//   SC16IS752 XTAL: 1.8432 MHz → divisor = 1 (točno, 0% napaka)
//
// LD2410C FRAME FORMAT (basic reporting):
//   Header : FD FC FB FA
//   Len    : 2 bajta LE (tipično 0x0D 0x00 = 13)
//   Data[0]: tip — 0x02 = basic reporting
//   Data[1]: glava — 0xAA
//   Data[2]: detection — 0=nič 1=premikanje 2=statično 3=oboje
//   Data[3-4]: razdalja premikajočega cilja [cm, LE uint16]
//   Data[5]:   energija premikajočega cilja [0-100]
//   Data[6-7]: razdalja statičnega cilja [cm, LE uint16]
//   Data[8]:   energija statičnega cilja [0-100]
//   Data[9-10]: razdalja zaznave [cm, LE uint16]
//   Data[11]:  rep — 0x55
//   Data[12]:  check — 0x00
//   Footer : 04 03 02 01
//
// ============================================================

#include "hal_radar_test.h"
#include "logger.h"
#include <Wire.h>

// ============================================================
// LOG MAKROJI — gredo na Serial direktno (test kontekst)
// Uporabljamo Serial.printf namesto logger_log ker test teče
// v init fazi pred polno logger integracijo.
// ============================================================

#define RT_PRINT(fmt, ...)   Serial.printf("[RT] " fmt "\n", ##__VA_ARGS__)
#define RT_RAW(fmt, ...)     Serial.printf(fmt, ##__VA_ARGS__)

// ============================================================
// KONSTANTE
// ============================================================

// I2C naslovi
#define SC16_ADDR_1         0x48    // SC16IS752 #1 (A0=VCC, A1=GND)
#define SC16_ADDR_2         0x4C    // SC16IS752 #2 (A0=VCC, A1=VCC)

// Wire1 pini
#define TEST_I2C_SDA        17
#define TEST_I2C_SCL        18
#define TEST_I2C_FREQ       100000

// IRQ pini
#define TEST_IRQ_SC16_1     41      // SC16IS752 #1 IRQ → IO41
#define TEST_IRQ_SC16_2     42      // SC16IS752 #2 IRQ → IO42

// SC16IS752 UART kanali
#define UART_CH_A           0
#define UART_CH_B           1

// Baud rate in XTAL
// XTAL = 1.8432 MHz → divisor = 1.8432M / (16 × 115200) = 1.000 → točno!
#define TEST_BAUD_RATE      115200
#define SC16_DIV_HI         0x00
#define SC16_DIV_LO         0x01    // divisor = 1 → 115200 baud točno

// SC16IS752 registri (naslov = (reg << 3) | (ch << 1))
#define SC16_REG(reg, ch)   ((uint8_t)(((reg) << 3) | ((ch) << 1)))
#define SC16_RHR            0x00    // Receive Holding Register
#define SC16_IER            0x01    // Interrupt Enable Register
#define SC16_FCR            0x02    // FIFO Control (write only)
#define SC16_LCR            0x03    // Line Control Register
#define SC16_MCR            0x04    // Modem Control Register
#define SC16_LSR            0x05    // Line Status Register

// LSR bitovi
#define LSR_RDR             0x01    // Receiver Data Ready
#define LSR_OE              0x02    // Overrun Error
#define LSR_PE              0x04    // Parity Error
#define LSR_FE              0x08    // Framing Error
#define LSR_THRE            0x20    // TX Holding Register Empty

// LCR vrednosti
#define LCR_8N1             0x03    // 8 data bits, no parity, 1 stop
#define LCR_DLAB            0x80    // Divisor Latch Access Bit

// FCR vrednosti
#define FCR_INIT            0x07    // FIFO enable + reset TX + reset RX

// Timeout branja [ms] na kanal
#define READ_TIMEOUT_MS     3000

// Max bajtov na tick branje
#define MAX_READ_BYTES      128

// Max data bajtov v LD2410C frame
#define LD2410_MAX_DATA     64

// LD2410C frame konstante
static const uint8_t LD2410_HDR[] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t LD2410_FTR[] = {0x04, 0x03, 0x02, 0x01};
#define LD2410_TYPE_BASIC   0x02
#define LD2410_TYPE_ENG     0x01
#define LD2410_HEAD_MARKER  0xAA
#define LD2410_TAIL_MARKER  0x55

// ============================================================
// PODATKOVNE STRUKTURE
// ============================================================

// Konfiguracija enega čipa
struct Sc16Chip {
    uint8_t     addr;
    uint8_t     irq_pin;
    const char* label;      // "SC16IS752 #1" ali "#2"
    bool        present;    // ali odgovori na I2C ping
    bool        uart_a_ok;  // ali UART-A init uspel
    bool        uart_b_ok;  // ali UART-B init uspel
};

// Konfiguracija enega UART kanala
struct Sc16Channel {
    uint8_t     chip_addr;
    uint8_t     uart_ch;    // 0=A, 1=B
    const char* name;       // "Vhod", "Cesta_L", "Cesta_D", "Garaza"

    // Statistika
    uint32_t    bytes_rx;
    uint32_t    frames_valid;
    uint32_t    frames_invalid;
    uint32_t    parse_errors;
    uint32_t    i2c_errors;
    bool        got_any_data;
    bool        got_valid_frame;

    // Parser stanje
    uint8_t     hdr_match;
    uint8_t     ftr_match;
    uint8_t     data_buf[LD2410_MAX_DATA];
    uint8_t     data_len;
    uint8_t     data_idx;
    uint8_t     parse_state; // 0=wait_hdr 1=in_len 2=in_data 3=wait_ftr
    uint8_t     len_idx;
};

// LD2410C parsed frame
struct Ld2410Frame {
    uint8_t  detection;         // 0=nič 1=premik 2=statično 3=oboje
    uint16_t moving_dist_cm;
    uint8_t  moving_energy;
    uint16_t static_dist_cm;
    uint8_t  static_energy;
    uint16_t detect_dist_cm;
};

// Stanje celotnega testa
struct TestState {
    Sc16Chip    chip[2];        // chip[0]=0x48, chip[1]=0x4C
    Sc16Channel ch[4];          // ch[0..3] = Vhod, Cesta_L, Cesta_D, Garaza
    bool        wire1_ok;
    uint8_t     chips_found;
    uint8_t     channels_with_data;
    uint8_t     channels_with_frames;
};

static TestState s_test;

// ============================================================
// SC16IS752 I2C POMOŽNE FUNKCIJE
// ============================================================

static bool sc16_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(addr);
    Wire1.write(reg);
    Wire1.write(val);
    return (Wire1.endTransmission() == 0);
}

static bool sc16_read_reg(uint8_t addr, uint8_t reg, uint8_t& out) {
    Wire1.beginTransmission(addr);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(addr, (uint8_t)1) != 1) return false;
    out = Wire1.read();
    return true;
}

static bool sc16_ping(uint8_t addr) {
    Wire1.beginTransmission(addr);
    return (Wire1.endTransmission() == 0);
}

// ============================================================
// I2C SCAN
// ============================================================

static void run_i2c_scan() {
    RT_PRINT("--- I2C Scan (Wire1, 0x01-0x7E) ---");
    uint8_t found = 0;
    for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
        Wire1.beginTransmission(addr);
        uint8_t err = Wire1.endTransmission();
        if (err == 0) {
            const char* known = "";
            if (addr == 0x48) known = " ← SC16IS752 #1 (pričakovan)";
            else if (addr == 0x4C) known = " ← SC16IS752 #2 (pričakovan)";
            else if (addr == 0x70) known = " ← TCA9548A";
            else if (addr == 0x20) known = " ← MCP23017";
            else if (addr == 0x23) known = " ← BH1750";
            else if (addr == 0x29) known = " ← TOF VL53L0X";
            RT_PRINT("  0x%02X NAJDEN%s", addr, known);
            found++;
        }
        delay(2);
    }
    if (found == 0) {
        RT_PRINT("  !! NOBENE naprave na busu !!");
        RT_PRINT("  Preveri: pull-up upori? Napajanje? SDA/SCL zamenjani?");
    } else {
        RT_PRINT("  Skupaj naprav na Wire1: %d", found);
    }
    s_test.wire1_ok = (found > 0);
}

// ============================================================
// SC16IS752 UART INICIALIZACIJA
// ============================================================

static bool sc16_init_uart_channel(uint8_t addr, uint8_t ch, const char* label) {
    bool ok = true;

    // Korak 1: LCR DLAB=1 → dostop do baud rate registrov
    if (!sc16_write_reg(addr, SC16_REG(SC16_LCR, ch), LCR_DLAB)) {
        RT_PRINT("    !! LCR DLAB set napaka (I2C err)");
        return false;
    }

    // Korak 2: DLL = 1 (divisor low byte)
    // Naslov DLL = SC16_REG(RHR, ch) ko je DLAB=1
    if (!sc16_write_reg(addr, SC16_REG(SC16_RHR, ch), SC16_DIV_LO)) {
        RT_PRINT("    !! DLL zapis napaka");
        ok = false;
    }

    // Korak 3: DLH = 0 (divisor high byte)
    // Naslov DLH = SC16_REG(IER, ch) ko je DLAB=1
    if (!sc16_write_reg(addr, SC16_REG(SC16_IER, ch), SC16_DIV_HI)) {
        RT_PRINT("    !! DLH zapis napaka");
        ok = false;
    }

    // Korak 4: LCR DLAB=0, 8N1
    if (!sc16_write_reg(addr, SC16_REG(SC16_LCR, ch), LCR_8N1)) {
        RT_PRINT("    !! LCR 8N1 set napaka");
        ok = false;
    }

    // Korak 5: FCR — FIFO enable + reset TX + reset RX
    if (!sc16_write_reg(addr, SC16_REG(SC16_FCR, ch), FCR_INIT)) {
        RT_PRINT("    !! FCR init napaka");
        ok = false;
    }

    // Korak 6: IER = 0x00 — brez IRQ (polling test)
    if (!sc16_write_reg(addr, SC16_REG(SC16_IER, ch), 0x00)) {
        RT_PRINT("    !! IER zapis napaka");
        ok = false;
    }

    // Korak 7: MCR = 0x00 — normalni način
    if (!sc16_write_reg(addr, SC16_REG(SC16_MCR, ch), 0x00)) {
        RT_PRINT("    !! MCR zapis napaka (nekritično)");
        // ne prekinemo
    }

    // Korak 8: preberi LSR — diagnostika
    uint8_t lsr = 0;
    if (sc16_read_reg(addr, SC16_REG(SC16_LSR, ch), lsr)) {
        RT_PRINT("    LSR po init: 0x%02X (THRE:%d RDR:%d OE:%d PE:%d FE:%d)",
            lsr,
            (lsr & LSR_THRE) ? 1 : 0,
            (lsr & LSR_RDR)  ? 1 : 0,
            (lsr & LSR_OE)   ? 1 : 0,
            (lsr & LSR_PE)   ? 1 : 0,
            (lsr & LSR_FE)   ? 1 : 0);
        if (lsr & (LSR_OE | LSR_PE | LSR_FE)) {
            RT_PRINT("    !! LSR napaka po init — UART verjetno ne dela");
            RT_PRINT("    !! Preveri: baud rate, kabel, napajanje LD2410C (5V!)");
        }
    } else {
        RT_PRINT("    !! LSR branje napaka");
    }

    if (ok) {
        RT_PRINT("    UART-%s init OK (XTAL=1.8432MHz div=1 → %d baud 8N1 FIFO)",
            ch == 0 ? "A" : "B", TEST_BAUD_RATE);
    }
    return ok;
}

// ============================================================
// IRQ PIN TEST
// ============================================================

static void test_irq_pins() {
    RT_PRINT("--- IRQ Pin stanje ---");

    int irq1 = digitalRead(TEST_IRQ_SC16_1);
    int irq2 = digitalRead(TEST_IRQ_SC16_2);

    RT_PRINT("  IO%d (SC16IS752 #1 IRQ): %s%s",
        TEST_IRQ_SC16_1,
        irq1 == HIGH ? "HIGH (idle — normalno)" : "LOW  (data ready ali float!)",
        !s_test.chip[0].present ? " [čip ni priključen]" : "");

    RT_PRINT("  IO%d (SC16IS752 #2 IRQ): %s%s",
        TEST_IRQ_SC16_2,
        irq2 == HIGH ? "HIGH (idle — normalno)" : "LOW  (data ready ali float!)",
        !s_test.chip[1].present ? " [čip ni priključen]" : "");

    if (irq1 == LOW && !s_test.chip[0].present) {
        RT_PRINT("  !! IO41 LOW brez čipa — preveriti pull-up ali kratki stik");
    }
    if (irq2 == LOW && !s_test.chip[1].present) {
        RT_PRINT("  !! IO42 LOW brez čipa — preveriti pull-up ali kratki stik");
    }
}

// ============================================================
// LD2410C FRAME PARSER — en bajt naenkrat
// ============================================================
// Vrne true ko je frame poln in veljaven, zapolni out.

static bool parse_byte(Sc16Channel& c, uint8_t b, Ld2410Frame& out) {
    switch (c.parse_state) {

    case 0: // WAIT_HEADER
        if (b == LD2410_HDR[c.hdr_match]) {
            c.hdr_match++;
            if (c.hdr_match == 4) {
                c.hdr_match   = 0;
                c.parse_state = 1;
                c.data_len    = 0;
                c.data_idx    = 0;
                c.len_idx     = 0;
            }
        } else {
            if (c.hdr_match > 0) {
                c.hdr_match = 0;
                if (b == LD2410_HDR[0]) c.hdr_match = 1;
            }
        }
        break;

    case 1: // IN_LENGTH (2 bajta LE)
        if (c.len_idx == 0) {
            c.data_len = b;
            c.len_idx  = 1;
        } else {
            c.data_len |= (b << 8);
            c.len_idx   = 0;
            if (c.data_len == 0 || c.data_len > LD2410_MAX_DATA) {
                RT_PRINT("    parser: neveljavna dolžina %d → reset", c.data_len);
                c.parse_state = 0;
                c.hdr_match   = 0;
                c.parse_errors++;
            } else {
                c.parse_state = 2;
                c.data_idx    = 0;
            }
        }
        break;

    case 2: // IN_DATA
        if (c.data_idx < LD2410_MAX_DATA) {
            c.data_buf[c.data_idx++] = b;
        }
        if (c.data_idx >= c.data_len) {
            c.parse_state = 3;
            c.ftr_match   = 0;
        }
        break;

    case 3: // WAIT_FOOTER
        if (b == LD2410_FTR[c.ftr_match]) {
            c.ftr_match++;
            if (c.ftr_match == 4) {
                // Frame poln — parsiraj
                c.ftr_match   = 0;
                c.parse_state = 0;
                c.hdr_match   = 0;

                // Preverimo tip in dolžino
                if (c.data_len >= 13 &&
                    c.data_buf[0] == LD2410_TYPE_BASIC &&
                    c.data_buf[1] == LD2410_HEAD_MARKER) {

                    out.detection      = c.data_buf[2];
                    out.moving_dist_cm = (uint16_t)c.data_buf[3] | ((uint16_t)c.data_buf[4] << 8);
                    out.moving_energy  = c.data_buf[5];
                    out.static_dist_cm = (uint16_t)c.data_buf[6] | ((uint16_t)c.data_buf[7] << 8);
                    out.static_energy  = c.data_buf[8];
                    out.detect_dist_cm = (uint16_t)c.data_buf[9] | ((uint16_t)c.data_buf[10] << 8);

                    if (c.data_buf[11] != LD2410_TAIL_MARKER) {
                        RT_PRINT("    parser [%s]: tail marker napaka 0x%02X",
                            c.name, c.data_buf[11]);
                    }
                    c.frames_valid++;
                    return true;

                } else if (c.data_len >= 1 && c.data_buf[0] == LD2410_TYPE_ENG) {
                    // Engineering mode — ignoriramo, ne javljamo napake
                    c.frames_invalid++;
                } else {
                    RT_PRINT("    parser [%s]: neustrezen frame tip=0x%02X len=%d",
                        c.name,
                        c.data_len > 0 ? c.data_buf[0] : 0xFF,
                        c.data_len);
                    c.frames_invalid++;
                    c.parse_errors++;
                }
            }
        } else {
            RT_PRINT("    parser [%s]: footer prekinjen pri %d (0x%02X)",
                c.name, c.ftr_match, b);
            c.ftr_match   = 0;
            c.parse_state = 0;
            c.hdr_match   = 0;
            c.parse_errors++;
        }
        break;
    }
    return false;
}

// ============================================================
// BRANJE IN PARSIRANJE ENEGA KANALA (s timeoutom)
// ============================================================

static void read_channel_with_timeout(Sc16Channel& c, uint32_t timeout_ms) {
    RT_PRINT("  Branje [%s] (timeout %lus)...", c.name, (unsigned long)(timeout_ms / 1000));

    // Resetiraj parser
    c.parse_state = 0;
    c.hdr_match   = 0;
    c.ftr_match   = 0;
    c.data_idx    = 0;
    c.data_len    = 0;
    c.len_idx     = 0;

    uint32_t t_start      = millis();
    uint32_t last_log_ms  = 0;
    uint32_t bytes_total  = 0;
    uint32_t frames_found = 0;
    uint8_t  raw_dump[32];
    uint8_t  raw_dump_cnt = 0;
    bool     first_byte   = true;

    while ((millis() - t_start) < timeout_ms) {

        // Preberi LSR
        uint8_t lsr = 0;
        if (!sc16_read_reg(c.chip_addr, SC16_REG(SC16_LSR, c.uart_ch), lsr)) {
            c.i2c_errors++;
            if (c.i2c_errors % 50 == 1) {
                RT_PRINT("    !! I2C napaka pri branju LSR (skupaj: %lu)", (unsigned long)c.i2c_errors);
            }
            delay(5);
            continue;
        }

        // Preverimo LSR napake
        if (lsr & (LSR_OE | LSR_PE | LSR_FE)) {
            RT_PRINT("    !! LSR napaka: OE:%d PE:%d FE:%d",
                (lsr & LSR_OE) ? 1 : 0,
                (lsr & LSR_PE) ? 1 : 0,
                (lsr & LSR_FE) ? 1 : 0);
            RT_PRINT("    !! Možen vzrok: LD2410C nima 5V napajanja, ali napačen baud rate");
        }

        if (!(lsr & LSR_RDR)) {
            // Ni podatkov — periodično izpiši status
            uint32_t elapsed = millis() - t_start;
            if (elapsed - last_log_ms >= 1000) {
                last_log_ms = elapsed;
                RT_PRINT("    ... %lus — čakam (bytes=%lu frames=%lu)",
                    (unsigned long)(elapsed / 1000),
                    (unsigned long)bytes_total,
                    (unsigned long)frames_found);
            }
            delay(5);
            continue;
        }

        // Preberi bajt iz RHR
        uint8_t byte_val = 0;
        if (!sc16_read_reg(c.chip_addr, SC16_REG(SC16_RHR, c.uart_ch), byte_val)) {
            c.i2c_errors++;
            continue;
        }

        bytes_total++;
        c.bytes_rx++;
        c.got_any_data = true;

        // Shrani prvih 32 bajtov za raw dump
        if (raw_dump_cnt < 32) {
            raw_dump[raw_dump_cnt++] = byte_val;
        }

        // Izpiši prve bajte za diagnostiko
        if (first_byte) {
            first_byte = false;
            RT_PRINT("    PODATKI PRIHAJAJO! (prvi bajt: 0x%02X)", byte_val);
        }

        // Parsiraj
        Ld2410Frame frame;
        if (parse_byte(c, byte_val, frame)) {
            frames_found++;
            c.got_valid_frame = true;

            const char* det_str =
                frame.detection == 0 ? "NIČ" :
                frame.detection == 1 ? "PREMIK" :
                frame.detection == 2 ? "STATIČNO" :
                frame.detection == 3 ? "OBOJE" : "???";

            RT_PRINT("    ✓ Frame #%lu: det=%s mov=%dcm(%d%%) sta=%dcm(%d%%) dist=%dcm",
                (unsigned long)frames_found,
                det_str,
                frame.moving_dist_cm,  frame.moving_energy,
                frame.static_dist_cm,  frame.static_energy,
                frame.detect_dist_cm);

            // Po 5 veljavnih frame-ih gremo naprej
            if (frames_found >= 5) {
                RT_PRINT("    5 veljavnih frame-ov → zaključujem kanal");
                break;
            }
        }
    }

    // Povzetek za ta kanal
    RT_PRINT("  --- Povzetek [%s] ---", c.name);
    RT_PRINT("    Bytes: %lu | Frames OK: %lu | Frames ERR: %lu | Parse err: %lu | I2C err: %lu",
        (unsigned long)c.bytes_rx,
        (unsigned long)c.frames_valid,
        (unsigned long)c.frames_invalid,
        (unsigned long)c.parse_errors,
        (unsigned long)c.i2c_errors);

    if (bytes_total == 0) {
        RT_PRINT("    !! NIČ BAJTOV — možni vzroki:");
        RT_PRINT("    !!   1. LD2410C nima 5V napajanja");
        RT_PRINT("    !!   2. UART kabel TX/RX zamenjana");
        RT_PRINT("    !!   3. SC16IS752 UART-A/B ni pravilno inicializiran");
        RT_PRINT("    !!   4. LD2410C je v command mode (pošlji reset ukaz)");
    } else if (c.frames_valid == 0) {
        RT_PRINT("    !! Bajti prihajajo, a ni veljavnih frame-ov — možni vzroki:");
        RT_PRINT("    !!   1. Napačen baud rate (pričakovano 115200)");
        RT_PRINT("    !!   2. LD2410C v engineering mode (samo tip=0x01 frame-i)");
        RT_PRINT("    !!   3. Frame korupcija — preveriti napajanje");
        // Izpiši raw dump
        RT_PRINT("    Raw dump prvih %d bajtov:", raw_dump_cnt);
        RT_RAW("    ");
        for (uint8_t i = 0; i < raw_dump_cnt; i++) {
            RT_RAW("0x%02X ", raw_dump[i]);
            if ((i + 1) % 16 == 0) RT_RAW("\n    ");
        }
        RT_RAW("\n");
    } else {
        RT_PRINT("    ✓ Kanal deluje pravilno!");
    }
}

// ============================================================
// TEST ENEGA SC16IS752 ČIPA
// ============================================================

static void test_chip(uint8_t chip_idx) {
    Sc16Chip& chip = s_test.chip[chip_idx];

    RT_PRINT("");
    RT_PRINT("=== SC16IS752 #%d @ 0x%02X (%s) ===",
        chip_idx + 1, chip.addr, chip.label);

    if (!chip.present) {
        RT_PRINT("  PRESKOČEN — čip ne odgovarja na I2C naslov 0x%02X", chip.addr);
        RT_PRINT("  → Priklopi čip in ponovi test");
        RT_PRINT("  → Pričakovan: A0=%s A1=%s",
            chip_idx == 0 ? "VCC" : "VCC",
            chip_idx == 0 ? "GND" : "VCC");
        return;
    }

    RT_PRINT("  Čip prisoten na I2C ✓");

    // IRQ pin stanje
    uint8_t irq_pin = (chip_idx == 0) ? TEST_IRQ_SC16_1 : TEST_IRQ_SC16_2;
    int irq_state = digitalRead(irq_pin);
    RT_PRINT("  IRQ pin IO%d: %s",
        irq_pin,
        irq_state == HIGH ? "HIGH ✓ (idle)" : "LOW (data čaka ali float!)");

    // Init UART-A
    RT_PRINT("  Init UART-A (ch=0)...");
    bool uart_a_ok = sc16_init_uart_channel(chip.addr, UART_CH_A,
        chip_idx == 0 ? "Vhod" : "Cesta_D");
    chip.uart_a_ok = uart_a_ok;

    delay(10);

    // Init UART-B
    RT_PRINT("  Init UART-B (ch=1)...");
    bool uart_b_ok = sc16_init_uart_channel(chip.addr, UART_CH_B,
        chip_idx == 0 ? "Cesta_L" : "Garaza");
    chip.uart_b_ok = uart_b_ok;

    delay(100); // počakaj da LD2410C začne pošiljati

    // Beri UART-A
    if (uart_a_ok) {
        Sc16Channel& ca = s_test.ch[chip_idx * 2 + 0]; // ch[0] ali ch[2]
        read_channel_with_timeout(ca, READ_TIMEOUT_MS);
        if (ca.got_any_data)    s_test.channels_with_data++;
        if (ca.got_valid_frame) s_test.channels_with_frames++;
    } else {
        RT_PRINT("  UART-A: preskočeno (init napaka)");
    }

    delay(200);

    // Beri UART-B
    if (uart_b_ok) {
        Sc16Channel& cb = s_test.ch[chip_idx * 2 + 1]; // ch[1] ali ch[3]
        read_channel_with_timeout(cb, READ_TIMEOUT_MS);
        if (cb.got_any_data)    s_test.channels_with_data++;
        if (cb.got_valid_frame) s_test.channels_with_frames++;
    } else {
        RT_PRINT("  UART-B: preskočeno (init napaka)");
    }
}

// ============================================================
// JAVNA FUNKCIJA — hal_radar_test_run()
// ============================================================

void hal_radar_test_run() {

    // --------------------------------------------------------
    // Inicializacija stanja
    // --------------------------------------------------------
    memset(&s_test, 0, sizeof(s_test));

    // Čip konfiguracija
    s_test.chip[0] = { SC16_ADDR_1, TEST_IRQ_SC16_1, "SC16IS752 #1", false, false, false };
    s_test.chip[1] = { SC16_ADDR_2, TEST_IRQ_SC16_2, "SC16IS752 #2", false, false, false };

    // Kanal konfiguracija
    s_test.ch[0] = { SC16_ADDR_1, UART_CH_A, "Vhod"    };
    s_test.ch[1] = { SC16_ADDR_1, UART_CH_B, "Cesta_L" };
    s_test.ch[2] = { SC16_ADDR_2, UART_CH_A, "Cesta_D" };
    s_test.ch[3] = { SC16_ADDR_2, UART_CH_B, "Garaza"  };

    // --------------------------------------------------------
    // Header
    // --------------------------------------------------------
    delay(500);
    RT_PRINT("");
    RT_PRINT("====================================");
    RT_PRINT("  SC16IS752 Hardware Test v1.0");
    RT_PRINT("  LD2410C Radar Bridge Diagnostika");
    RT_PRINT("====================================");
    RT_PRINT("Hardware:");
    RT_PRINT("  SC16IS752 #1 @ 0x48 (A0=VCC A1=GND) IRQ→IO41");
    RT_PRINT("    UART-A → LD2410C #1 [Vhod]");
    RT_PRINT("    UART-B → LD2410C #2 [Cesta_L]");
    RT_PRINT("  SC16IS752 #2 @ 0x4C (A0=VCC A1=VCC) IRQ→IO42");
    RT_PRINT("    UART-A → LD2410C #3 [Cesta_D]");
    RT_PRINT("    UART-B → LD2410C #4 [Garaza]");
    RT_PRINT("UART: XTAL=1.8432MHz div=1 → 115200 baud 8N1");
    RT_PRINT("Wire1: SDA=IO17 SCL=IO18 100kHz");
    RT_PRINT("====================================");

    // --------------------------------------------------------
    // Wire1 init
    // --------------------------------------------------------
    RT_PRINT("");
    RT_PRINT("--- Wire1 init ---");
    Wire1.begin(TEST_I2C_SDA, TEST_I2C_SCL, TEST_I2C_FREQ);
    delay(50);
    RT_PRINT("  Wire1 OK (SDA=IO%d SCL=IO%d %dkHz)",
        TEST_I2C_SDA, TEST_I2C_SCL, TEST_I2C_FREQ / 1000);

    // IRQ pini
    pinMode(TEST_IRQ_SC16_1, INPUT_PULLUP);
    pinMode(TEST_IRQ_SC16_2, INPUT_PULLUP);

    // --------------------------------------------------------
    // I2C Scan
    // --------------------------------------------------------
    RT_PRINT("");
    run_i2c_scan();

    // --------------------------------------------------------
    // Ping vsakega čipa
    // --------------------------------------------------------
    RT_PRINT("");
    RT_PRINT("--- SC16IS752 Ping ---");
    for (uint8_t i = 0; i < 2; i++) {
        s_test.chip[i].present = sc16_ping(s_test.chip[i].addr);
        if (s_test.chip[i].present) {
            s_test.chips_found++;
            RT_PRINT("  0x%02X %-14s NAJDEN ✓", s_test.chip[i].addr, s_test.chip[i].label);
        } else {
            RT_PRINT("  0x%02X %-14s NI NAJDEN ✗ — priklopi ga kasneje", s_test.chip[i].addr, s_test.chip[i].label);
        }
        delay(10);
    }
    RT_PRINT("  Skupaj SC16IS752: %d/2", s_test.chips_found);

    // --------------------------------------------------------
    // IRQ pin stanje
    // --------------------------------------------------------
    RT_PRINT("");
    test_irq_pins();

    // --------------------------------------------------------
    // Test vsakega čipa
    // --------------------------------------------------------
    for (uint8_t i = 0; i < 2; i++) {
        test_chip(i);
        delay(300);
    }

    // --------------------------------------------------------
    // Končni povzetek
    // --------------------------------------------------------
    RT_PRINT("");
    RT_PRINT("====================================");
    RT_PRINT("  POVZETEK HARDWARE TESTA");
    RT_PRINT("====================================");

    RT_PRINT("Wire1 (IO17/IO18):   %s", s_test.wire1_ok ? "OK ✓" : "NAPAKA ✗");
    RT_PRINT("SC16IS752 najdeni:   %d/2", s_test.chips_found);

    for (uint8_t i = 0; i < 2; i++) {
        Sc16Chip& chip = s_test.chip[i];
        RT_PRINT("%s (0x%02X): %s",
            chip.label, chip.addr,
            chip.present ? "PRISOTEN ✓" : "MANJKA   ✗");
        if (chip.present) {
            RT_PRINT("  UART-A: %s", chip.uart_a_ok ? "init OK ✓" : "init NAPAKA ✗");
            RT_PRINT("  UART-B: %s", chip.uart_b_ok ? "init OK ✓" : "init NAPAKA ✗");
        }
    }

    RT_PRINT("");
    RT_PRINT("Kanali z podatki:       %d/4", s_test.channels_with_data);
    RT_PRINT("Kanali z veljavnimi frames: %d/4", s_test.channels_with_frames);

    RT_PRINT("");
    const char* names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};
    for (uint8_t i = 0; i < 4; i++) {
        Sc16Channel& c = s_test.ch[i];
        uint8_t chip_i = i / 2;
        if (!s_test.chip[chip_i].present) {
            RT_PRINT("  [%s]: čip manjka", c.name);
            continue;
        }
        RT_PRINT("  [%-8s]: bytes=%-4lu frames_ok=%-3lu frames_err=%-3lu parse_err=%-3lu i2c_err=%-3lu  %s",
            c.name,
            (unsigned long)c.bytes_rx,
            (unsigned long)c.frames_valid,
            (unsigned long)c.frames_invalid,
            (unsigned long)c.parse_errors,
            (unsigned long)c.i2c_errors,
            c.got_valid_frame ? "✓ OK" :
            c.got_any_data    ? "⚠ BAJTI DA, FRAME NE" :
                                "✗ NIČ");
    }

    RT_PRINT("");
    if (s_test.channels_with_frames == 4) {
        RT_PRINT("✓✓✓ VSI 4 kanali delujejo — radar sistem pripravljen za integracijo!");
    } else if (s_test.channels_with_frames > 0) {
        RT_PRINT("⚠ %d/4 kanalov deluje — preveri preostale (glej napake zgoraj)",
            s_test.channels_with_frames);
    } else if (s_test.channels_with_data > 0) {
        RT_PRINT("⚠ Bajti prihajajo, a ni veljavnih frame-ov");
        RT_PRINT("  → Najverjetneje napačen baud rate ali power issue");
    } else if (s_test.chips_found > 0) {
        RT_PRINT("✗ SC16IS752 najden, a nobenih podatkov iz LD2410C");
        RT_PRINT("  → Preveri: 5V napajanje na LD2410C, UART kabel TX↔RX");
    } else {
        RT_PRINT("✗✗✗ Noben SC16IS752 ne odgovarja na I2C");
        RT_PRINT("  → Preveri: napajanje 3.3V, SDA/SCL pini, pull-up upori");
    }

    RT_PRINT("====================================");
    RT_PRINT("  Test zaključen.");
    RT_PRINT("====================================");
    RT_PRINT("");
}
