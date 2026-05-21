// ============================================================
// hal_radar.cpp — SC16IS752 Periodical-Polling LD2410C Radar HAL
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 2.0.0  |  Datum: 2026-05
// Vir     : NXP SC16IS752/SC16IS762 Datasheet Rev. 9.1 (5 Feb 2025)
//           https://www.nxp.com/docs/en/data-sheet/SC16IS752_SC16IS762.pdf
// ============================================================
//
// ============================================================
// ZAKAJ SMO ZAMENJALI IRQ/PIN-POLLING Z ČISTIM PERIODIČNIM POLLINGOM
// ============================================================
//
// PREJŠNJA ARHITEKTURA (v1.x — IRQ-pin polling z drain zanko):
//
//   while(true) {
//     if (digitalRead(IO41) == LOW) process_chip_irq(0);  // drain zanka
//     if (digitalRead(IO42) == LOW) process_chip_irq(1);  // drain zanka
//     vTaskDelay(5ms);
//   }
//
//   process_chip_irq() je implementirala Linux kernel sc16is7xx.c vzorec:
//   do { keep_polling |= process_one_channel(); } while (keep_polling && --max_loops);
//
//   Problemi ki so jih povzročili:
//
//   1. IRQ pin ostane LOW ves čas ko je v FIFO kakšen bajt.
//      LD2410C pošilja @ 115200 baud → FIFO nikoli ni popolnoma prazen
//      → IRQ pin je praktično STALNO LOW → pseudo-busy-loop.
//
//   2. max_loops=4 je arbitrarna omejitev brez semantičnega pomena.
//      Ko se doseže → log "max_loops=4 dosežen" → videti kot napaka,
//      čeprav je to normalno delovanje. Napaka ni bila v kodi, ampak v
//      dizajnu: drain zanka po definiciji ne more "zaključiti" pri
//      neprekinjeni stream komunikaciji.
//
//   3. Wire1 mutex je bil pridobivan znotraj drain zanke (per-chip
//      acquire/release v vsaki iteraciji). Pri rr_loops = 2×4 = 8
//      iteracij × ~12ms = do 96ms blokade Wire1 → TOF senzorji čakajo.
//
//   4. Zapletena round-robin logika (chip1_pending, chip2_pending,
//      rr_loops) je povzročala OE! napake ker je bila zakasnitev med
//      chip1 in chip2 servicing nepredvidljiva.
//
// NOVA ARHITEKTURA (v2.0 — čisti periodični polling):
//
//   while(true) {
//     t_start = millis();
//     xSemaphoreTake(Wire1_mutex, poll_interval/2);
//     za vsak kanal (0..3):
//       rxlvl = sc16_read(RXLVL)
//       if rxlvl > 0: burst_read → parse → callback
//       lsr = sc16_read(LSR)
//       if lsr & OE: flush_rx() + increment overflow counter
//     xSemaphoreGive(Wire1_mutex);
//     vTaskDelay(poll_interval - elapsed);
//   }
//
//   Prednosti:
//
//   1. PREDVIDLJIVO obnašanje: polling vsakih `radar_poll_interval_ms`
//      (nastavljivo 10–100ms, default 50ms). Ni zanašanja na IRQ pin,
//      ni drain zank, ni max_loops.
//
//   2. Mutex pridobimo ENKRAT za vse 4 kanale → Wire1 blokada je
//      maksimalno 4×(RXLVL read + burst read + LSR read) = ~8ms worst case.
//      Prejšnja arhitektura: do 96ms. Znižanje za 12×.
//
//   3. OE! overflow je normalen pojav ko je procesor zaseden (TOF,
//      LVGL, WiFi). Zdaj ga obravnavamo: flush FIFO → discard frame →
//      nadaljujemo. Overflow counter per-sensor → minutni log z %.
//
//   4. TOF senzorji (Wire1 mutex odjemalec) imajo prednost:
//      mutex timeout v radarTask = poll_interval/2, TOF timeout = 500ms.
//      Če TOF drži mutex → radarTask preskoci to iteracijo, next tick.
//
//   5. Zmanjšana kompleksnost: odstranjeni process_chip_irq(),
//      process_one_channel(), round-robin blok, rr_loops, keep_polling.
//      Zamenjano z ~30 vrsticami direktnega RXLVL/LSR branja.
//
//   KOMPROMIS: latenca odziva ~poll_interval/2 namesto <1ms z ISR.
//   Za to aplikacijo (osvetlitev, alarm) je 25ms latenca popolnoma
//   sprejemljiva. ISR arhitektura ni dosegljiva na ESP32-S3 Arduino
//   z IDF 5.3 (gpio_isr_register → ipc_task 1KB stack overflow).
//
// ============================================================

#include "hal_radar.h"
#include "bsp.h"
#include "config.h"
#include "config_mgr.h"
#include "logger.h"
#include <Wire.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define RADI(fmt, ...) LOG_INFO ("RADAR", fmt, ##__VA_ARGS__)
#define RADW(fmt, ...) LOG_WARN ("RADAR", fmt, ##__VA_ARGS__)
#define RADE(fmt, ...) LOG_ERROR("RADAR", fmt, ##__VA_ARGS__)
#define RADD(fmt, ...) LOG_DEBUG("RADAR", fmt, ##__VA_ARGS__)

// ============================================================
// SC16IS752 — REGISTER MAPA
// (identično v1.x — register dostop se ni spremenil)
// ============================================================

#define REG_RHR      0x00
#define REG_IER      0x01
#define REG_IIR      0x02
#define REG_FCR      0x02
#define REG_LCR      0x03
#define REG_MCR      0x04
#define REG_LSR      0x05
#define REG_MSR      0x06
#define REG_SPR      0x07
#define REG_TXLVL    0x08
#define REG_RXLVL    0x09
#define REG_IODIR    0x0A
#define REG_IOSTATE  0x0B
#define REG_IOINTENA 0x0C
#define REG_IOCONTROL 0x0E
#define REG_EFCR     0x0F
#define REG_DLL      0x00
#define REG_DLH      0x01
#define REG_EFR      0x02

#define SC16_REG(reg, ch)    ((uint8_t)(((reg) << 3) | ((ch) << 1)))
#define SC16_REG_GLOBAL(reg) ((uint8_t)((reg) << 3))

// IER — samo RX data interrupt (bit 0). V polling načinu line-status
// interrupt (bit 2) ni potreben — LSR beremo direktno v vsaki iteraciji.
// V primerjavi z v1.x (IER=0x05) znižamo interrupt load na SC16IS752.
#define IER_RX_ONLY     0x01    // bit 0: RHR interrupt (RX data available)

// LSR — relevantni biti
#define LSR_RX_DATA_READY 0x01  // bit 0: podatki v RX FIFO
#define LSR_OVERRUN_ERR   0x02  // bit 1: OE! — RX FIFO overflow
#define LSR_PARITY_ERR    0x04  // bit 2: parity error
#define LSR_FRAMING_ERR   0x08  // bit 3: framing error
#define LSR_THR_EMPTY     0x20  // bit 5: TX FIFO pod trigger
#define LSR_TX_EMPTY      0x40  // bit 6: TX + shift register prazna
#define LSR_ANY_ERROR     (LSR_OVERRUN_ERR | LSR_PARITY_ERR | LSR_FRAMING_ERR)

// FCR init — FIFO enable + reset TX + reset RX + trigger 8 znakov
#define FCR_INIT_VALUE    0x07

// LCR — 8N1 in DLAB
#define LCR_8N1           0x03
#define LCR_DLAB          0x80

// MCR — vse izklopljeno
#define MCR_NORMAL        0x00

// IOCONTROL — software reset
#define IOCONTROL_SW_RESET 0x08

// Baud rate divisor za 115200 @ 1.8432 MHz XTAL
// Formula: divisor = 1843200 / (1 × 16 × 115200) = 1.000 → točno, 0% napaka
#define SC16_DLL_115200   0x01
#define SC16_DLH_115200   0x00

// I2C naslovi SC16IS752 čipov
#define SC16_ADDR_1       0x48   // A0=VCC, A1=GND
#define SC16_ADDR_2       0x4C   // A0=VCC, A1=VCC

// IRQ pini (niso v uporabi za polling aktivacijo, ohranimo za diagnostiko)
#define RADAR_IRQ_CHIP1   41
#define RADAR_IRQ_CHIP2   42

// UART kanali
#define UART_CH_A         0
#define UART_CH_B         1

// FreeRTOS task parametri
#define RADAR_TASK_PRIO   4
#define RADAR_TASK_CORE   1

// LD2410C frame magic bytes — potrjeni iz hardware testa
static const uint8_t LD2410_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t LD2410_FOOTER[4] = {0xF8, 0xF7, 0xF6, 0xF5};
#define LD2410_TYPE_BASIC   0x02
#define LD2410_TYPE_ENG     0x01
#define LD2410_HEAD_MARKER  0xAA
#define LD2410_TAIL_MARKER  0x55
#define LD2410_MAX_DATA_LEN 64

// Meje za radar_poll_interval_ms (NVS parameter)
#define RADAR_POLL_INTERVAL_MIN_MS   10u
#define RADAR_POLL_INTERVAL_MAX_MS  100u

// Stuck sensor detekcija in recovery
#define RADAR_STUCK_TIMEOUT_MS     30000u   // brez framov → recovery
#define RADAR_REINIT_COOLDOWN_MS   15000u   // min čas med recovery poskusi
#define RADAR_STARTUP_GRACE_MS     20000u   // ignoriraj pred 20s (config traja ~10s)
#define RADAR_STUCK_CHECK_MS       10000u   // interval preverjanja stuck kanalov

// ============================================================
// INTERNI TIPI
// ============================================================

typedef struct {
    uint8_t  state;
    uint8_t  hdr_idx;
    uint8_t  ftr_idx;
    uint8_t  len_idx;
    uint16_t data_len;
    uint8_t  data_idx;
    uint8_t  data_buf[LD2410_MAX_DATA_LEN];
} FrameParser;

typedef struct {
    uint8_t         chip_addr;
    uint8_t         uart_ch;
    RadarSensorId   sensor_id;
    FrameParser     parser;
    RadarSensorStatus status;
    // Polling statistika za minutni log
    // (zamenjuje IRQ-driven irq_count statistiko)
    uint32_t poll_attempts;         // skupaj polling iteracij kjer je bil rxlvl > 0
    uint32_t poll_overflows;        // skupaj OE! overflow v tej periodi
    uint32_t frames_ok_last;        // frames_ok ob začetku trenutne minute (za delta)
    uint32_t consec_overflows;      // zaporedni overflowi — za WARN log
    // Stuck recovery stanje
    uint8_t  stuck_level;           // 0=ok, 1=uart_reinit opravljen, čaka potrditev
    uint32_t last_recovery_ms;      // čas zadnjega recovery poskusa
    uint32_t recovery_count;        // skupaj recovery poskusov (za statistiko)
} RadarChannel;

typedef struct {
    RadarChannel      ch[RADAR_SENSOR_COUNT];
    RadarFrameCallback callback;
    TaskHandle_t      task_handle;
    bool              initialized;
    bool              chip1_ok;
    bool              chip2_ok;
} RadarState;

// ============================================================
// STATIČNE SPREMENLJIVKE
// ============================================================

static RadarState s_radar;

// ============================================================
// I2C POMOŽNE FUNKCIJE
// Vse klicati samo iz task konteksta (ne ISR)!
// Wire1 mutex mora biti pridobljen pred klicem.
// (identično v1.x)
// ============================================================

static bool sc16_write(uint8_t addr, uint8_t subaddr, uint8_t val) {
    Wire1.beginTransmission(addr);
    Wire1.write(subaddr);
    Wire1.write(val);
    return (Wire1.endTransmission() == 0);
}

static bool sc16_read(uint8_t addr, uint8_t subaddr, uint8_t& out) {
    Wire1.beginTransmission(addr);
    Wire1.write(subaddr);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(addr, (uint8_t)1) != 1) return false;
    out = Wire1.read();
    return true;
}

// Burst read iz RHR — prebere do max_len bajtov, vrne dejansko prebrano število.
// Direktno na osnovi RXLVL — ni potrebe po IIR vmesnem koraku.
static uint8_t sc16_read_fifo(uint8_t addr, uint8_t ch, uint8_t* buf, uint8_t max_len) {
    uint8_t rxlvl = 0;
    if (!sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl)) return 0;
    if (rxlvl == 0) return 0;
    uint8_t to_read = (rxlvl < max_len) ? rxlvl : max_len;
    Wire1.beginTransmission(addr);
    Wire1.write(SC16_REG(REG_RHR, ch));
    if (Wire1.endTransmission(false) != 0) return 0;
    uint8_t received = Wire1.requestFrom(addr, to_read);
    for (uint8_t i = 0; i < received; i++) buf[i] = Wire1.read();
    return received;
}

static bool sc16_ping(uint8_t addr) {
    Wire1.beginTransmission(addr);
    return (Wire1.endTransmission() == 0);
}

static bool sc16_write_uart(uint8_t addr, uint8_t ch,
                            const uint8_t* data, uint8_t len) {
    if (len == 0 || len > 64) return false;
    uint8_t txlvl = 0;
    sc16_read(addr, SC16_REG(REG_TXLVL, ch), txlvl);
    if (txlvl < len) {
        RADW("sc16_write_uart: TX FIFO premalo prostora (%d < %d)", txlvl, len);
        return false;
    }
    Wire1.beginTransmission(addr);
    Wire1.write(SC16_REG(REG_RHR, ch));
    for (uint8_t i = 0; i < len; i++) Wire1.write(data[i]);
    return (Wire1.endTransmission() == 0);
}

// ============================================================
// FLUSH RX — izprazni SC16IS752 RX FIFO
// Ohranjena iz v1.x — jo kličemo ob overflow za clean state.
// Wire1 mutex mora biti pridobljen pred klicem.
// ============================================================
static void ld2410_flush_rx(uint8_t addr, uint8_t ch) {
    uint8_t rxlvl = 0;
    uint8_t drain_buf[64];
    uint8_t total_drained = 0;
    for (int attempts = 0; attempts < 8; attempts++) {
        sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);
        if (rxlvl == 0) break;
        uint8_t n = (rxlvl > sizeof(drain_buf)) ? sizeof(drain_buf) : rxlvl;
        sc16_read_fifo(addr, ch, drain_buf, n);
        total_drained += n;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    // Drugi flush: počisti bajte ki so prišli med 10ms pavzo
    uint8_t rxlvl2 = 0;
    sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl2);
    if (rxlvl2 > 0) {
        uint8_t drain2[64];
        sc16_read_fifo(addr, ch, drain2, sizeof(drain2));
        total_drained += rxlvl2;
    }
    if (total_drained > 0) {
        RADD("flush_rx 0x%02X ch%c: izpraznjeno %d B", addr, ch==0?'A':'B', total_drained);
    }
}

// ============================================================
// sc16_read_response — preberi config ACK odgovor (za LD2410C konfiguriranje)
// Ohranjena iz v1.x — nespremenjena.
// ============================================================
static uint8_t sc16_read_response(uint8_t addr, uint8_t ch,
                                   uint8_t* buf, uint8_t max_len,
                                   uint32_t timeout_ms = 200) {
    static const uint8_t CFG_HEADER[4] = {0xFD, 0xFC, 0xFB, 0xFA};
    static const uint8_t CFG_FOOTER[4] = {0x04, 0x03, 0x02, 0x01};
    uint32_t start = millis();
    uint8_t hdr_matched = 0;
    uint8_t pos = 0;
    uint32_t bytes_skipped = 0;
    bool header_found = false;
    const uint32_t MAX_SKIP = 50;

    while ((millis() - start) < timeout_ms) {
        esp_task_wdt_reset();
        uint8_t rxlvl = 0;
        sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);
        if (rxlvl == 0) { vTaskDelay(pdMS_TO_TICKS(3)); continue; }
        uint8_t b = 0;
        Wire1.beginTransmission(addr);
        Wire1.write(SC16_REG(REG_RHR, ch));
        if (Wire1.endTransmission(false) != 0) break;
        if (Wire1.requestFrom(addr, (uint8_t)1) != 1) break;
        b = Wire1.read();
        if (!header_found) {
            if (b == CFG_HEADER[hdr_matched]) {
                hdr_matched++;
                if (hdr_matched == 4) {
                    header_found = true;
                    if (pos + 4 <= max_len) {
                        buf[pos++] = 0xFD; buf[pos++] = 0xFC;
                        buf[pos++] = 0xFB; buf[pos++] = 0xFA;
                    }
                }
            } else {
                bytes_skipped++;
                hdr_matched = 0;
                if (b == CFG_HEADER[0]) hdr_matched = 1;
                if ((bytes_skipped & 0x0F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
                if (bytes_skipped >= MAX_SKIP) return 0;
            }
        } else {
            if (pos < max_len) buf[pos++] = b;
            if (pos >= 4) {
                if (memcmp(buf + pos - 4, CFG_FOOTER, 4) == 0) return pos;
            }
        }
    }
    RADW("sc16_read_response 0x%02X ch%c: TIMEOUT (%dB, skip=%lu)",
         addr, ch==0?'A':'B', (int)pos, (unsigned long)bytes_skipped);
    return pos;
}

// ============================================================
// LD2410C KONFIGURACIJSKI UKAZI — enako kot v1.x
// ============================================================

static const uint8_t LD2410_CFG_HEADER[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t LD2410_CFG_FOOTER[4] = {0x04, 0x03, 0x02, 0x01};

static bool ld2410_send_cmd(uint8_t chip_addr, uint8_t uart_ch,
                            const uint8_t* cmd_data, uint8_t cmd_len,
                            uint8_t* resp_buf, uint8_t resp_max,
                            uint32_t timeout_ms = 200) {
    uint8_t pkt[80]; uint8_t pos = 0;
    memcpy(pkt + pos, LD2410_CFG_HEADER, 4); pos += 4;
    pkt[pos++] = cmd_len; pkt[pos++] = 0x00;
    memcpy(pkt + pos, cmd_data, cmd_len); pos += cmd_len;
    memcpy(pkt + pos, LD2410_CFG_FOOTER, 4); pos += 4;
    if (!sc16_write_uart(chip_addr, uart_ch, pkt, pos)) return false;
    if (resp_buf && resp_max > 0) {
        uint8_t n = sc16_read_response(chip_addr, uart_ch, resp_buf, resp_max, timeout_ms);
        if (n < 10) { RADW("ld2410 0x%02X ch%c: ACK prekratek (%dB)", chip_addr, uart_ch==0?'A':'B', (int)n); return false; }
        bool ack_ok = (resp_buf[8] == 0x00 && resp_buf[9] == 0x00);
        if (!ack_ok) RADW("ld2410 0x%02X ch%c: ACK status napaka (0x%02X 0x%02X)", chip_addr, uart_ch==0?'A':'B', resp_buf[8], resp_buf[9]);
        return ack_ok;
    }
    return true;
}

static bool ld2410_open_config(uint8_t chip_addr, uint8_t uart_ch) {
    ld2410_flush_rx(chip_addr, uart_ch);
    const uint8_t cmd[] = {0xFF, 0x00, 0x01, 0x00};
    uint8_t resp[20];
    bool ok = ld2410_send_cmd(chip_addr, uart_ch, cmd, 4, resp, sizeof(resp));
    if (!ok) RADW("ld2410 0x%02X ch%c: open_config NAPAKA", chip_addr, uart_ch==0?'A':'B');
    return ok;
}

static bool ld2410_close_config(uint8_t chip_addr, uint8_t uart_ch) {
    const uint8_t cmd_end[] = {0xFE, 0x00};
    uint8_t resp[20];
    bool ok = ld2410_send_cmd(chip_addr, uart_ch, cmd_end, 2, resp, sizeof(resp));
    vTaskDelay(pdMS_TO_TICKS(30));
    const uint8_t cmd_rst[] = {0xA3, 0x00};
    ld2410_send_cmd(chip_addr, uart_ch, cmd_rst, 2, nullptr, 0);
    return ok;
}

static bool ld2410_set_params(uint8_t chip_addr, uint8_t uart_ch,
                              uint8_t max_dist, uint16_t unmanned_s) {
    uint8_t cmd[20];
    cmd[0]=0x60; cmd[1]=0x00;
    cmd[2]=0x00; cmd[3]=0x00; cmd[4]=max_dist; cmd[5]=0x00; cmd[6]=0x00; cmd[7]=0x00;
    cmd[8]=0x01; cmd[9]=0x00; cmd[10]=max_dist; cmd[11]=0x00; cmd[12]=0x00; cmd[13]=0x00;
    cmd[14]=0x02; cmd[15]=0x00;
    cmd[16]=(uint8_t)(unmanned_s & 0xFF); cmd[17]=(uint8_t)(unmanned_s >> 8);
    cmd[18]=0x00; cmd[19]=0x00;
    uint8_t resp[20];
    bool ok = ld2410_send_cmd(chip_addr, uart_ch, cmd, 20, resp, sizeof(resp));
    if (!ok) RADW("ld2410 0x%02X ch%c: set_params NAPAKA", chip_addr, uart_ch==0?'A':'B');
    return ok;
}

static bool ld2410_set_sensitivity(uint8_t chip_addr, uint8_t uart_ch,
                                   uint8_t move_sens, uint8_t static_sens) {
    uint8_t cmd[20];
    cmd[0]=0x64; cmd[1]=0x00;
    cmd[2]=0x00; cmd[3]=0x00; cmd[4]=0xFF; cmd[5]=0xFF; cmd[6]=0x00; cmd[7]=0x00;
    cmd[8]=0x01; cmd[9]=0x00; cmd[10]=move_sens; cmd[11]=0x00; cmd[12]=0x00; cmd[13]=0x00;
    cmd[14]=0x02; cmd[15]=0x00; cmd[16]=static_sens; cmd[17]=0x00; cmd[18]=0x00; cmd[19]=0x00;
    uint8_t resp[20];
    bool ok = ld2410_send_cmd(chip_addr, uart_ch, cmd, 20, resp, sizeof(resp));
    if (!ok) RADW("ld2410 0x%02X ch%c: set_sensitivity NAPAKA", chip_addr, uart_ch==0?'A':'B');
    return ok;
}

static bool ld2410_verify_params(uint8_t chip_addr, uint8_t uart_ch,
                                 uint8_t expected_max_dist,
                                 uint8_t expected_move_sens,
                                 uint8_t expected_static_sens,
                                 uint16_t expected_unmanned_s) {
    const uint8_t cmd[] = {0x61, 0x00};
    uint8_t resp[50];
    ld2410_flush_rx(chip_addr, uart_ch);
    bool ok = ld2410_send_cmd(chip_addr, uart_ch, cmd, 2, resp, sizeof(resp), 250);
    if (!ok) { RADW("ld2410 0x%02X ch%c: verify NAPAKA", chip_addr, uart_ch==0?'A':'B'); return false; }
    if (resp[10] != 0xAA) return false;
    uint8_t  read_motion_gate = resp[12];
    uint8_t  read_static_gate = resp[13];
    uint16_t read_unmanned    = (uint16_t)resp[32] | ((uint16_t)resp[33] << 8);
    bool all_ok = (read_motion_gate == expected_max_dist) &&
                  (read_static_gate == expected_max_dist) &&
                  (read_unmanned    == expected_unmanned_s);
    if (all_ok) {
        RADI("ld2410 0x%02X ch%c: verify OK (gate=%d, unmanned=%ds)",
             chip_addr, uart_ch==0?'A':'B', read_motion_gate, (int)read_unmanned);
    } else {
        RADW("ld2410 0x%02X ch%c: verify NAPAKA (gate exp=%d got=%d/%d, unmanned exp=%d got=%d)",
             chip_addr, uart_ch==0?'A':'B',
             expected_max_dist, read_motion_gate, read_static_gate,
             (int)expected_unmanned_s, (int)read_unmanned);
    }
    return all_ok;
}

// ============================================================
// SC16IS752 — inicializacija enega UART kanala
// (identično v1.x, IER spremenjen na IER_RX_ONLY=0x01)
// ============================================================

static bool sc16_init_channel(uint8_t addr, uint8_t ch) {
    bool ok = true;
    if (!sc16_write(addr, SC16_REG(REG_LCR, ch), LCR_DLAB)) { RADE("sc16 0x%02X ch%c: LCR DLAB fail", addr, ch==0?'A':'B'); return false; }
    if (!sc16_write(addr, SC16_REG(REG_DLL, ch), SC16_DLL_115200)) ok = false;
    if (!sc16_write(addr, SC16_REG(REG_DLH, ch), SC16_DLH_115200)) ok = false;
    if (!sc16_write(addr, SC16_REG(REG_LCR, ch), LCR_8N1)) ok = false;
    if (!sc16_write(addr, SC16_REG(REG_FCR, ch), FCR_INIT_VALUE)) ok = false;
    delayMicroseconds(5);
    if (!sc16_write(addr, SC16_REG(REG_MCR, ch), MCR_NORMAL)) {}  // nekritično
    // V polling načinu IER=0x01 (samo RX data interrupt) namesto 0x05.
    // Line-status interrupt (bit 2) ni potreben ker beremo LSR direktno.
    if (!sc16_write(addr, SC16_REG(REG_IER, ch), IER_RX_ONLY)) ok = false;

    uint8_t lsr = 0;
    if (sc16_read(addr, SC16_REG(REG_LSR, ch), lsr)) {
        RADI("sc16 0x%02X ch%c: LSR=0x%02X %s", addr, ch==0?'A':'B', lsr, ok ? "OK" : "WARN");
    }
    // Počisti pending interrupts po init
    uint8_t iir = 0; uint8_t loops = 0;
    while (loops++ < 8) {
        if (!sc16_read(addr, SC16_REG(REG_IIR, ch), iir)) break;
        if (iir & 0x01) break;
        uint8_t rxlvl = 0; sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);
        if (rxlvl > 0) { uint8_t d[64]; sc16_read_fifo(addr, ch, d, sizeof(d)); }
        uint8_t lsr2 = 0; sc16_read(addr, SC16_REG(REG_LSR, ch), lsr2);
    }
    return ok;
}

// ============================================================
// LD2410C FRAME PARSER — en bajt naenkrat
// (identično v1.x — parser je neodvisen od I/O arhitekture)
// ============================================================

static bool parse_byte(FrameParser& p, uint8_t b, RadarFrame& out, RadarSensorId id) {
    switch (p.state) {
    case 0:
        if (b == LD2410_HEADER[p.hdr_idx]) {
            p.hdr_idx++;
            if (p.hdr_idx == 4) { p.hdr_idx=0; p.state=1; p.data_len=0; p.data_idx=0; p.len_idx=0; }
        } else { p.hdr_idx=0; if (b==LD2410_HEADER[0]) p.hdr_idx=1; }
        break;
    case 1:
        if (p.len_idx == 0) { p.data_len=b; p.len_idx=1; }
        else {
            p.data_len |= ((uint16_t)b << 8);
            if (p.data_len == 0 || p.data_len > LD2410_MAX_DATA_LEN) { p.state=0; p.hdr_idx=0; return false; }
            p.state=2; p.data_idx=0;
        }
        break;
    case 2:
        if (p.data_idx < LD2410_MAX_DATA_LEN) p.data_buf[p.data_idx++] = b;
        if (p.data_idx >= p.data_len) { p.state=3; p.ftr_idx=0; }
        break;
    case 3:
        if (b == LD2410_FOOTER[p.ftr_idx]) {
            p.ftr_idx++;
            if (p.ftr_idx == 4) {
                p.state=0; p.hdr_idx=0; p.ftr_idx=0;
                if (p.data_len >= 13 && p.data_buf[0]==LD2410_TYPE_BASIC && p.data_buf[1]==LD2410_HEAD_MARKER) {
                    out.sensor_id      = id;
                    out.detection      = p.data_buf[2];
                    out.moving_dist_cm = (uint16_t)p.data_buf[3] | ((uint16_t)p.data_buf[4]<<8);
                    out.moving_energy  = p.data_buf[5];
                    out.static_dist_cm = (uint16_t)p.data_buf[6] | ((uint16_t)p.data_buf[7]<<8);
                    out.static_energy  = p.data_buf[8];
                    out.detect_dist_cm = (uint16_t)p.data_buf[9] | ((uint16_t)p.data_buf[10]<<8);
                    out.timestamp_ms   = millis();
                    return true;
                }
            }
        } else { p.ftr_idx=0; if (b==LD2410_FOOTER[0]) p.ftr_idx=1; }
        break;
    }
    return false;
}

// ============================================================
// poll_one_channel — preberi en UART kanal v eni polling iteraciji
//
// ARHITEKTURNA ODLOČITEV: RXLVL-direktni pristop namesto IIR-driven:
//
//   Prejšnja verzija (v1.x):
//     sc16_read(IIR) → preveri tip interrupta → sc16_read(RXLVL) → burst read
//     = 3 I2C transakcije minimum per kanal, plus IIR silicon bug workaround.
//
//   Nova verzija (v2.0):
//     sc16_read(RXLVL) → če > 0: burst read
//     sc16_read(LSR)   → preveri overflow
//     = 2 I2C transakcije normalno, 3 ob overflowu.
//
//   Skupni prihranek: 4 kanali × (3→2) = 4 I2C transakcij manj per iteracija.
//   Pri 50ms intervalu = 80 transakcij/s manj na Wire1 busu.
//
// Kliče se samo iz radarTask, ki drži Wire1 mutex.
// ============================================================

static void poll_one_channel(RadarChannel& rc) {
    uint8_t addr = rc.chip_addr;
    uint8_t ch   = rc.uart_ch;

    // Korak 1: preveri RXLVL — koliko bajtov je v FIFO
    uint8_t rxlvl = 0;
    if (!sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl)) {
        rc.status.i2c_errors++;
        return;
    }

    // Korak 2: če so podatki, preberi in parsiraj
    if (rxlvl > 0) {
        rc.poll_attempts++;
        uint8_t fifo_buf[64];
        uint8_t n = sc16_read_fifo(addr, ch, fifo_buf, sizeof(fifo_buf));
        rc.status.active = true;

        for (uint8_t i = 0; i < n; i++) {
            RadarFrame frame;
            if (parse_byte(rc.parser, fifo_buf[i], frame, rc.sensor_id)) {
                rc.status.frames_ok++;
                rc.status.last_frame    = frame;
                rc.status.last_frame_ms = millis();

                // Throttle callback — max 1× per RADAR_PUBLISH_INTERVAL_MS
                // (definiran v config.h — nespremenjen iz v1.x)
                uint32_t now_ms = millis();
                if (s_radar.callback &&
                    (now_ms - rc.status.last_publish_ms) >= RADAR_PUBLISH_INTERVAL_MS) {
                    rc.status.last_publish_ms = now_ms;
                    s_radar.callback(frame);
                }
            }
        }
        // Ponastavi parser overflow counter ob uspešnem branju
        rc.consec_overflows = 0;
    }

    // Korak 3: preveri LSR za overflow
    // OE! (overrun error) pomeni da je LD2410C pošiljal hitreje kot smo brali.
    // To je normalen pojav ko Wire1 mutex drži TOF ali drug modul.
    // Reakcija: flush FIFO → discard tega "polnega" cikla → naslednji tick normalen.
    uint8_t lsr = 0;
    if (!sc16_read(addr, SC16_REG(REG_LSR, ch), lsr)) return;

    if (lsr & LSR_OVERRUN_ERR) {
        rc.status.parse_errors++;   // štejem v parse_errors enako kot v1.x za konsistentnost
        rc.status.oe_count++;
        rc.poll_overflows++;
        rc.consec_overflows++;

        // Flush FIFO — zavrži vse bajte ki so se nakopičili med zasedenim Wire1.
        // Obstoječi ld2410_flush_rx() je preizkušen in ohranjen iz v1.x.
        ld2410_flush_rx(addr, ch);

        // WARN log — samo ob prekoračitvi praga zaporednih overflowov
        // ali pri prvem overflowu po daljšem mirnem obdobju.
        uint32_t now_oe = millis();
        if (rc.consec_overflows >= RADAR_MAX_CONSECUTIVE_OVERFLOWS) {
            if ((now_oe - rc.status.last_oe_log_ms) >= RADAR_OE_LOG_INTERVAL_MS) {
                rc.status.last_oe_log_ms = now_oe;
                RADW("SC16[0x%02X] ch%c: %lu zaporednih OE! — povečaj radar_poll_interval_ms",
                     addr, ch==0?'A':'B', (unsigned long)rc.consec_overflows);
            }
        }
    } else {
        // Reset consecutive overflow counter ob prvem OK LSR
        if (rc.consec_overflows > 0) {
            RADD("SC16[0x%02X] ch%c: overflow razrešen po %lu iteracijah",
                 addr, ch==0?'A':'B', (unsigned long)rc.consec_overflows);
            rc.consec_overflows = 0;
        }
    }
}

// ============================================================
// recover_stuck_channel — dvonivojski escalating recovery
//
// Level 1 (stuck_level == 0 → 1):
//   flush RX + sc16_init_channel + parser reset
//   Odpravi: zamrzel UART kanal, parser v napačnem stanju
//
// Level 2 (stuck_level == 1, Level 1 ni pomagal):
//   SC16 SW reset → reinit obeh kanalov → LD2410C reconfigure
//   Odpravi: LD2410C v config mode, korupcija FIFO, I2C desync
//
// Kliče se iz radarTask vsakih RADAR_STUCK_CHECK_MS, mutex pridobi sam.
// ============================================================

static const char* s_ch_names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};

static void recover_stuck_channel(uint8_t idx) {
    RadarChannel& rc = s_radar.ch[idx];
    rc.last_recovery_ms = millis();
    rc.recovery_count++;

    if (rc.stuck_level == 0) {
        // Level 1: UART channel reinit + parser reset
        RADW("[%s] STUCK >%us — Level 1: UART reinit (poskus #%lu)",
             s_ch_names[idx],
             (unsigned)(RADAR_STUCK_TIMEOUT_MS / 1000),
             (unsigned long)rc.recovery_count);

        SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            ld2410_flush_rx(rc.chip_addr, rc.uart_ch);
            bool ok = sc16_init_channel(rc.chip_addr, rc.uart_ch);
            xSemaphoreGive(mtx);
            if (!ok) RADW("[%s] Level 1: sc16_init_channel NAPAKA", s_ch_names[idx]);
        } else {
            RADW("[%s] Level 1: Wire1 mutex timeout", s_ch_names[idx]);
        }
        memset(&rc.parser, 0, sizeof(rc.parser));
        rc.stuck_level = 1;
        rc.status.stuck_level = 1;

    } else {
        // Level 2: SW chip reset + reinit obeh kanalov + LD2410C reconfigure
        RADW("[%s] STUCK Level 2: SC16 SW reset + LD2410C reconfigure (skupaj %lu recovery)",
             s_ch_names[idx], (unsigned long)rc.recovery_count);

        uint8_t chip_addr = rc.chip_addr;
        uint8_t base = (chip_addr == SC16_ADDR_1) ? 0 : 2;

        // SW reset čipa in reinit obeh UART kanalov
        SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            sc16_write(chip_addr, SC16_REG_GLOBAL(REG_IOCONTROL), IOCONTROL_SW_RESET);
            vTaskDelay(pdMS_TO_TICKS(10));
            for (uint8_t j = base; j < base + 2; j++) {
                bool ok = sc16_init_channel(chip_addr, s_radar.ch[j].uart_ch);
                s_radar.ch[j].status.active = ok;
                memset(&s_radar.ch[j].parser, 0, sizeof(s_radar.ch[j].parser));
                RADI("[%s] Level 2: sc16_init_channel %s", s_ch_names[j], ok ? "OK" : "NAPAKA");
            }
            xSemaphoreGive(mtx);
        } else {
            RADW("[%s] Level 2: Wire1 mutex timeout pri SW reset", s_ch_names[idx]);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        // LD2410C reconfigure za oba kanala na tem čipu
        const Config cfg = config_get();
        for (uint8_t j = base; j < base + 2; j++) {
            RadarChannel& rj = s_radar.ch[j];
            if (!rj.status.active) continue;

            SemaphoreHandle_t mtx2 = bsp_get_wire1_mutex();
            if (xSemaphoreTake(mtx2, pdMS_TO_TICKS(500)) != pdTRUE) {
                RADW("[%s] Level 2: Wire1 mutex timeout pri reconfigure", s_ch_names[j]);
                continue;
            }
            bool ok = false;
            do {
                if (!ld2410_open_config(rj.chip_addr, rj.uart_ch)) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                if (!ld2410_set_params(rj.chip_addr, rj.uart_ch,
                                       cfg.radar_max_dist[j],
                                       cfg.radar_unmanned_s[j])) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                if (!ld2410_set_sensitivity(rj.chip_addr, rj.uart_ch,
                                            cfg.radar_move_sens[j],
                                            cfg.radar_static_sens[j])) break;
                ok = true;
            } while (false);
            ld2410_close_config(rj.chip_addr, rj.uart_ch);
            vTaskDelay(pdMS_TO_TICKS(500));
            xSemaphoreGive(mtx2);

            rj.status.config_ok = ok;
            RADI("[%s] Level 2 reconfigure: %s", s_ch_names[j], ok ? "OK" : "NAPAKA");
        }

        // Po Level 2 reset stuck_level → naslednji stuck bo znova od Level 1
        rc.stuck_level = 0;
        rc.status.stuck_level = 0;
    }

    rc.status.recovery_count = rc.recovery_count;
}

// ============================================================
// RADAR TASK — nova polling arhitektura v2.0
// ============================================================

static void radarTask(void* pvParams) {
    RADI("radarTask start — Core%d (v2.0 polling)", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    RADI("radarTask stack ob zagonu: %lu B free",
         (unsigned long)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));

    // ============================================================
    // Boot delay — počakaj da Wire1 in TOF inicializacija zaključita
    // preden začnemo polling. TOF init zasede Wire1 ~300ms.
    // ============================================================
    vTaskDelay(pdMS_TO_TICKS(500));

    // Preberi polling interval iz config (NVS-persistiran).
    // Privzeta vrednost: RADAR_POLL_INTERVAL_MS_DEFAULT (50ms).
    // Obseg: RADAR_POLL_INTERVAL_MIN_MS (10) – RADAR_POLL_INTERVAL_MAX_MS (100).
    const Config cfg_boot = config_get();
    uint32_t poll_interval_ms = cfg_boot.radar_poll_interval_ms;
    if (poll_interval_ms < RADAR_POLL_INTERVAL_MIN_MS ||
        poll_interval_ms > RADAR_POLL_INTERVAL_MAX_MS) {
        RADW("radar_poll_interval_ms=%lu izven meja → default %d",
             (unsigned long)poll_interval_ms, RADAR_POLL_INTERVAL_MS_DEFAULT);
        poll_interval_ms = RADAR_POLL_INTERVAL_MS_DEFAULT;
    }
    RADI("Polling interval: %lums (nastavljivo v web UI, NVS-persistiran)",
         (unsigned long)poll_interval_ms);

    // ============================================================
    // ZAGОНSKA KONFIGURACIJA LD2410C RADARJEV
    // Identična v1.x — nespremenjena logika konfiguriranja.
    // ============================================================
    {
        RADI("=== Radar konfiguracija ob zagonu ===");
        const Config cfg = config_get();
        const char* sensor_names[4] = {"Vhod","Cesta_L","Cesta_D","Garaza"};

        for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
            RadarChannel& rc = s_radar.ch[i];
            if (!rc.status.active) {
                RADW("  [%s]: preskočen — neaktiven", sensor_names[i]);
                rc.status.config_ok = false;
                continue;
            }

            RADI("  [%s]: konfiguriram (max_dist=%d, move=%d, static=%d, unmanned=%ds)",
                 sensor_names[i],
                 cfg.radar_max_dist[i], cfg.radar_move_sens[i],
                 cfg.radar_static_sens[i], cfg.radar_unmanned_s[i]);

            SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
                RADW("  [%s]: Wire1 mutex timeout — preskočen", sensor_names[i]);
                rc.status.config_ok = false;
                continue;
            }

            bool ok = false;
            do {
                if (!ld2410_open_config(rc.chip_addr, rc.uart_ch)) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                if (!ld2410_set_params(rc.chip_addr, rc.uart_ch,
                                       cfg.radar_max_dist[i],
                                       cfg.radar_unmanned_s[i])) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                if (!ld2410_set_sensitivity(rc.chip_addr, rc.uart_ch,
                                            cfg.radar_move_sens[i],
                                            cfg.radar_static_sens[i])) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                ok = true;
            } while (false);

            bool verified = false;
            if (ok) {
                verified = ld2410_verify_params(rc.chip_addr, rc.uart_ch,
                                                cfg.radar_max_dist[i],
                                                cfg.radar_move_sens[i],
                                                cfg.radar_static_sens[i],
                                                cfg.radar_unmanned_s[i]);
            }
            ld2410_close_config(rc.chip_addr, rc.uart_ch);
            vTaskDelay(pdMS_TO_TICKS(500));
            xSemaphoreGive(mtx);

            rc.status.config_ok              = ok;
            rc.status.config_verified        = verified;
            rc.status.config_ms              = millis();
            rc.status.configured_max_dist    = cfg.radar_max_dist[i];
            rc.status.configured_move_sens   = cfg.radar_move_sens[i];
            rc.status.configured_static_sens = cfg.radar_static_sens[i];
            rc.status.configured_unmanned_s  = cfg.radar_unmanned_s[i];

            RADI("  [%s]: %s%s", sensor_names[i],
                 ok ? "OK" : "NAPAKA", verified ? " (verificirano)" : "");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        RADI("=== Radar konfiguracija končana ===");
    }

    // Inicializiraj snapshot counters za minutni log
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        s_radar.ch[i].frames_ok_last  = 0;
        s_radar.ch[i].poll_attempts   = 0;
        s_radar.ch[i].poll_overflows  = 0;
    }

    static const char* names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};
    uint32_t last_log_ms = millis();
    bool     first_log   = true;

    // ============================================================
    // GLAVNI POLLING LOOP
    //
    // Struktura:
    //   1. Pridobi Wire1 mutex (timeout = poll_interval/2)
    //   2. Za vsak aktiven kanal: poll_one_channel()
    //      - sc16_read(RXLVL) → burst read če > 0 → parse → callback
    //      - sc16_read(LSR)   → overflow handling z ld2410_flush_rx()
    //   3. Sprosti Wire1 mutex
    //   4. vTaskDelay(preostali čas do naslednjega ticka)
    //   5. Vsakih 10s (ob zagonu) / 60s (runtime): minutni log z %
    //
    // ZAKAJ ENKRATNI MUTEX ZA VSE 4 KANALE:
    //   Mutex acquire/release je drag (FreeRTOS scheduler overhead ~10µs).
    //   4× acquire = 40µs overhead samo za mutex — zanemarljivo ampak
    //   nepotrebno. Resnejši razlog: med štirimi ločenimi mutex sekcijami
    //   bi TOF ali drug modul lahko "vlezlo vmes" in prekinilo branje
    //   četrtega kanala. Z enim mutex-om je branje vseh 4 kanalov atomarno
    //   z vidika Wire1 busa.
    //   Worst-case Wire1 zasedenost: 4 × (~2ms branje) = ~8ms per tick.
    //   TOF timeout za mutex je 500ms → 8ms je zanemarljivo.
    // ============================================================

    while (true) {
        esp_task_wdt_reset();

        uint32_t t_start = millis();

        // --- Polling blok ---
        SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
        // Timeout = poll_interval/2: če je Wire1 zaseden (TOF), preskočimo
        // to iteracijo. TOF ima prednost — to je zaželeno.
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(poll_interval_ms / 2)) == pdTRUE) {
            for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
                if (!s_radar.ch[i].status.active) continue;
                poll_one_channel(s_radar.ch[i]);
            }
            xSemaphoreGive(mtx);
        } else {
            // Wire1 zaseden (TOF ali drug modul) — normalno pri TOF skeniranju.
            // Inkrementiraj i2c_errors samo če se dogaja pretirano pogosto.
            // Za zdaj tiho preskočimo — naslednji tick bo verjetno uspešen.
            RADD("radarTask: Wire1 mutex timeout (TOF/drug modul aktiven) — preskočena iteracija");
        }

        // --- Stuck sensor detekcija in recovery (vsake RADAR_STUCK_CHECK_MS) ---
        {
            static uint32_t last_stuck_check_ms = 0;
            uint32_t now_sc = millis();
            if (now_sc > RADAR_STARTUP_GRACE_MS &&
                (now_sc - last_stuck_check_ms) >= RADAR_STUCK_CHECK_MS) {
                last_stuck_check_ms = now_sc;

                for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
                    RadarChannel& rc = s_radar.ch[i];
                    if (!rc.status.active) continue;

                    uint32_t since_frame = (rc.status.last_frame_ms > 0)
                        ? (now_sc - rc.status.last_frame_ms)
                        : now_sc;   // nikoli ni prejemal → merimo od zagona

                    bool is_stuck = (since_frame >= RADAR_STUCK_TIMEOUT_MS);
                    bool cooldown_ok = (now_sc - rc.last_recovery_ms) >= RADAR_REINIT_COOLDOWN_MS;

                    if (is_stuck && cooldown_ok) {
                        recover_stuck_channel(i);
                    } else if (!is_stuck && rc.stuck_level > 0) {
                        RADI("[%s] Recovery uspešen po %lu poskusih (level bil %d)",
                             s_ch_names[i],
                             (unsigned long)rc.recovery_count,
                             (int)rc.stuck_level);
                        rc.stuck_level = 0;
                        rc.status.stuck_level = 0;
                    }
                }
            }
        }

        // --- Interval timing ---
        uint32_t elapsed = millis() - t_start;
        if (elapsed < poll_interval_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms - elapsed));
        }
        // Posodobi poll interval iz config (live update iz web UI)
        {
            const Config cfg_live = config_get();
            uint32_t new_interval = cfg_live.radar_poll_interval_ms;
            if (new_interval >= RADAR_POLL_INTERVAL_MIN_MS &&
                new_interval <= RADAR_POLL_INTERVAL_MAX_MS &&
                new_interval != poll_interval_ms) {
                RADI("radarTask: poll interval spremenjen %lums → %lums",
                     (unsigned long)poll_interval_ms, (unsigned long)new_interval);
                poll_interval_ms = new_interval;
            }
        }

        // ============================================================
        // MINUTNI LOG — statistika uspešnosti branja per senzor
        //
        // Format:
        //   [RADAR:INFO] Statistika (60s): Vhod=98.2% Cesta_L=97.5% Cesta_D=99.1% Garaza=95.8%
        //
        // Formula:
        //   LD2410C pošilja 1 frame/~100ms = ~10 frames/s.
        //   Pričakovano frameov v 60s = 600.
        //   Dejansko: frames_ok delta v zadnji minuti.
        //   % = (dejansko / pričakovano) × 100
        //
        //   Pričakovano = log_interval_ms / 100ms (LD2410C period)
        //   Vrednost > 100% je možna (polling hitrejši od frame perioda →
        //   isti frame preberem dvakrat). To je informativno.
        //
        // Zakaj je ta log koristen:
        //   - < 90%: polling interval premajhen ali Wire1 prezaseden
        //   - 0%: senzor ne pošilja (preveri 5V napajanje, UART kabel)
        //   - > 100%: normalno pri 50ms polling intervalu (beri 20×/s, frame 10×/s)
        //
        // Prva meritev: 10s po zagonu (kratek preview).
        // Naslednje: vsakih 60s.
        // ============================================================
        uint32_t now_ms = millis();
        uint32_t log_interval = first_log ? 10000 : 60000;
        if (now_ms - last_log_ms >= log_interval) {
            uint32_t period_ms   = now_ms - last_log_ms;
            last_log_ms          = now_ms;
            first_log            = false;

            // Pričakovano frameov v periodi (LD2410C pošilja ~10/s)
            uint32_t expected = period_ms / 100;
            if (expected == 0) expected = 1;

            RADI("--- Radar statistika (uptime %lus, interval=%lums) ---",
                 (unsigned long)(now_ms / 1000),
                 (unsigned long)poll_interval_ms);

            for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
                RadarChannel& rc = s_radar.ch[i];
                if (!rc.status.active) {
                    RADI("  [%-8s]: NEAKTIVEN", names[i]);
                    continue;
                }

                uint32_t frames_delta = rc.status.frames_ok - rc.frames_ok_last;
                uint32_t pct_x10      = (frames_delta * 1000) / expected;  // ×10 za 1 decimalno mesto
                uint32_t ovf_delta    = rc.poll_overflows;

                const RadarFrame& f   = rc.status.last_frame;
                const char* det =
                    f.detection == 0 ? "---" :
                    f.detection == 1 ? "MOV" :
                    f.detection == 2 ? "STA" :
                    f.detection == 3 ? "OBA" : "???";

                uint32_t ago = rc.status.last_frame_ms > 0
                    ? (now_ms - rc.status.last_frame_ms) / 1000 : 9999;

                RADI("  [%-8s]: %lu.%lu%% frames=%lu/%lu OE!=%lu %s "
                     "mov=%dcm sta=%dcm zadnji=%lus%s",
                     names[i],
                     (unsigned long)(pct_x10 / 10),
                     (unsigned long)(pct_x10 % 10),
                     (unsigned long)frames_delta,
                     (unsigned long)expected,
                     (unsigned long)ovf_delta,
                     det,
                     (int)f.moving_dist_cm,
                     (int)f.static_dist_cm,
                     (unsigned long)ago,
                     (rc.consec_overflows >= RADAR_MAX_CONSECUTIVE_OVERFLOWS)
                         ? " !! PREVEČ OVERFLOWOV" : "");

                // WARN če je uspešnost < 50% (izključi prve sekunde po zagonu)
                if (pct_x10 < 500 && now_ms > 30000 && frames_delta > 0) {
                    RADW("  [%s]: nizka uspešnost %lu.%lu%% — "
                         "povečaj radar_poll_interval_ms ali preveri 5V napajanje",
                         names[i],
                         (unsigned long)(pct_x10 / 10),
                         (unsigned long)(pct_x10 % 10));
                } else if (frames_delta == 0 && now_ms > 30000) {
                    RADW("  [%s]: 0 frames v zadnji periodi — "
                         "preveri UART kabel in 5V napajanje LD2410C",
                         names[i]);
                }

                // Reset per-period counters
                rc.frames_ok_last = rc.status.frames_ok;
                rc.poll_overflows = 0;
            }
        }
    } // end while(true)
}

// ============================================================
// JAVNE FUNKCIJE — enake signature kot v1.x
// ============================================================

bool hal_radar_init(RadarFrameCallback cb) {
    RADI("=== hal_radar_init v2.0 (polling) ===");

    memset(&s_radar, 0, sizeof(s_radar));
    s_radar.callback = cb;

    s_radar.ch[0] = { SC16_ADDR_1, UART_CH_A, RADAR_SENSOR_VHOD    };
    s_radar.ch[1] = { SC16_ADDR_1, UART_CH_B, RADAR_SENSOR_CESTA_L };
    s_radar.ch[2] = { SC16_ADDR_2, UART_CH_A, RADAR_SENSOR_CESTA_D };
    s_radar.ch[3] = { SC16_ADDR_2, UART_CH_B, RADAR_SENSOR_GARAZA  };

    // IRQ pini ohranimo kot INPUT_PULLUP za diagnostično branje (log na zagonu)
    pinMode(RADAR_IRQ_CHIP1, INPUT_PULLUP);
    pinMode(RADAR_IRQ_CHIP2, INPUT_PULLUP);

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        RADE("hal_radar_init: Wire1 mutex timeout");
        return false;
    }

    s_radar.chip1_ok = sc16_ping(SC16_ADDR_1);
    s_radar.chip2_ok = sc16_ping(SC16_ADDR_2);
    RADI("SC16IS752 #1 (0x48): %s", s_radar.chip1_ok ? "NAJDEN ✓" : "MANJKA ✗");
    RADI("SC16IS752 #2 (0x4C): %s", s_radar.chip2_ok ? "NAJDEN ✓" : "MANJKA ✗");

    if (!s_radar.chip1_ok && !s_radar.chip2_ok) {
        xSemaphoreGive(mtx);
        RADE("hal_radar_init: nobeden SC16IS752 ne odgovarja — abort");
        return false;
    }

    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        RadarChannel& rc = s_radar.ch[i];
        bool chip_ok = (i < 2) ? s_radar.chip1_ok : s_radar.chip2_ok;
        if (!chip_ok) { RADW("kanal [%d] preskočen — čip manjka", i); continue; }
        bool ok = sc16_init_channel(rc.chip_addr, rc.uart_ch);
        rc.status.active = ok;
        RADI("kanal [%d] init: %s", i, ok ? "OK ✓" : "NAPAKA ✗");
    }

    xSemaphoreGive(mtx);

    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(
        radarTask, "Radar",
        RADAR_TASK_STACK, nullptr,
        RADAR_TASK_PRIO, &s_radar.task_handle,
        RADAR_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (r != pdPASS) {
        RADE("hal_radar_init: radarTask kreacija napaka");
        return false;
    }

    s_radar.initialized = true;
    RADI("hal_radar_init OK — čipi:%d/2 kanali:%d/4",
        (s_radar.chip1_ok ? 1 : 0) + (s_radar.chip2_ok ? 1 : 0),
        (int)(s_radar.ch[0].status.active + s_radar.ch[1].status.active +
              s_radar.ch[2].status.active + s_radar.ch[3].status.active));
    return true;
}

void hal_radar_deinit() {
    if (!s_radar.initialized) return;
    if (s_radar.task_handle) { vTaskDelete(s_radar.task_handle); s_radar.task_handle = nullptr; }
    s_radar.initialized = false;
    RADI("hal_radar_deinit OK");
}

const RadarSensorStatus& hal_radar_get_status(RadarSensorId id) {
    return s_radar.ch[(int)id].status;
}

bool hal_radar_channel_ok(RadarSensorId id) {
    return s_radar.ch[(int)id].status.active;
}

void hal_radar_reset_chip(uint8_t chip_addr) {
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    sc16_write(chip_addr, SC16_REG_GLOBAL(REG_IOCONTROL), IOCONTROL_SW_RESET);
    delay(5);
    uint8_t base = (chip_addr == SC16_ADDR_1) ? 0 : 2;
    for (uint8_t i = base; i < base + 2; i++) {
        s_radar.ch[i].status.active = sc16_init_channel(chip_addr, s_radar.ch[i].uart_ch);
    }
    xSemaphoreGive(mtx);
    RADW("hal_radar_reset_chip 0x%02X OK", chip_addr);
}

void hal_radar_log_stats() {
    const char* names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};
    RADI("=== Radar statistika (on-demand) ===");
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        const RadarSensorStatus& s = s_radar.ch[i].status;
        if (!s.active) { RADI("  [%-8s]: NEAKTIVEN", names[i]); continue; }
        uint32_t ago = (millis() - s.last_frame_ms) / 1000;
        RADI("  [%-8s]: ok=%lu err=%lu(OE) i2c=%lu zadnji=%lus config=%s",
            names[i],
            (unsigned long)s.frames_ok,
            (unsigned long)(s.parse_errors - s.boot_parse_errors),
            (unsigned long)s.i2c_errors,
            (unsigned long)ago,
            s.config_ok ? (s.config_verified ? "OK+verified" : "OK") : "NAPAKA");
    }
}

void hal_radar_recovery_check() {
    // Health status log — kliči iz sensorTask vsakih 10 minut za nadzor.
    // Dejansko okrevanje se izvaja samodejno v radarTask vsakih 10s.
    if (!s_radar.initialized) return;

    uint32_t now = millis();
    RADI("=== Radar health (recovery avtomatski v radarTask) ===");
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        const RadarChannel& rc = s_radar.ch[i];
        if (!rc.status.active) {
            RADI("  [%-8s]: NEAKTIVEN", s_ch_names[i]);
            continue;
        }
        uint32_t since_s = (rc.status.last_frame_ms > 0)
            ? (now - rc.status.last_frame_ms) / 1000 : 9999;
        RADI("  [%-8s]: frames=%lu zadnji=%lus recovery=%lu%s",
             s_ch_names[i],
             (unsigned long)rc.status.frames_ok,
             (unsigned long)since_s,
             (unsigned long)rc.recovery_count,
             rc.stuck_level ? " [RECOVERY V TEKU]" : "");
    }
}

bool hal_radar_reconfigure(RadarSensorId id,
                           uint8_t max_dist,
                           uint8_t move_sens,
                           uint8_t static_sens,
                           uint16_t unmanned_s) {
    if (!s_radar.initialized) return false;
    RadarChannel& rc = s_radar.ch[(int)id];
    if (!rc.status.active) return false;

    const char* names[4] = {"Vhod","Cesta_L","Cesta_D","Garaza"};
    RADI("hal_radar_reconfigure [%s]: max=%d move=%d static=%d unmanned=%d",
         names[(int)id], max_dist, move_sens, static_sens, unmanned_s);

    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        RADW("hal_radar_reconfigure: Wire1 mutex timeout");
        return false;
    }

    bool ok = false;
    do {
        if (!ld2410_open_config(rc.chip_addr, rc.uart_ch)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!ld2410_set_params(rc.chip_addr, rc.uart_ch, max_dist, unmanned_s)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!ld2410_set_sensitivity(rc.chip_addr, rc.uart_ch, move_sens, static_sens)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
        ok = true;
    } while (false);

    bool verified = false;
    if (ok) verified = ld2410_verify_params(rc.chip_addr, rc.uart_ch,
                                            max_dist, move_sens, static_sens, unmanned_s);
    ld2410_close_config(rc.chip_addr, rc.uart_ch);
    vTaskDelay(pdMS_TO_TICKS(500));
    xSemaphoreGive(mtx);

    rc.status.config_ok              = ok;
    rc.status.config_verified        = verified;
    rc.status.config_ms              = millis();
    rc.status.configured_max_dist    = max_dist;
    rc.status.configured_move_sens   = move_sens;
    rc.status.configured_static_sens = static_sens;
    rc.status.configured_unmanned_s  = unmanned_s;

    RADI("hal_radar_reconfigure [%s]: %s%s",
         names[(int)id], ok ? "OK" : "NAPAKA", verified ? " (verificirano)" : "");
    return ok && verified;
}
