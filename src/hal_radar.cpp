// ============================================================
// hal_radar.cpp — SC16IS752 IRQ-driven LD2410C Radar HAL
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-04
// Vir     : NXP SC16IS752/SC16IS762 Datasheet Rev. 9.1 (5 Feb 2025)
//           https://www.nxp.com/docs/en/data-sheet/SC16IS752_SC16IS762.pdf
// ============================================================

#include "hal_radar.h"
#include "bsp.h"
#include "config.h"
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
// Vir: datasheet Table 10 (str. 18-20), Rev. 9.1
//
// I2C naslov bajt = (7-bitni_naslov << 1) — standard Arduino Wire
// Register naslov bajt = (reg_addr << 3) | (channel << 1)
//   - reg_addr: bits [6:3] v subaddress bajtu
//   - channel:  bit  [1]   v subaddress bajtu (0=A, 1=B)
//   - bit [0] je rezerviran (vedno 0)
//
// Datasheet Section 9.1 (I2C interface):
//   "The first byte after the I2C address byte contains the register
//    address in bits [6:3], the channel select in bit [1] and
//    a reserved bit [0]."
//   Subaddress byte = [reg[3:0] | ch[0] | 0]
//   Formulo: subaddr = (reg << 3) | (ch << 1)
// ============================================================

// Register naslovi (pred shiftom — vrednosti iz Table 10)
#define REG_RHR     0x00    // Receive Holding Register (R) / THR Transmit (W)
#define REG_IER     0x01    // Interrupt Enable Register (R/W)
#define REG_IIR     0x02    // Interrupt Identification Register (R) — BERI samo
#define REG_FCR     0x02    // FIFO Control Register (W) — isti naslov kot IIR!
#define REG_LCR     0x03    // Line Control Register (R/W)
#define REG_MCR     0x04    // Modem Control Register (R/W)
#define REG_LSR     0x05    // Line Status Register (R — read only)
#define REG_MSR     0x06    // Modem Status Register (R — read only)
#define REG_SPR     0x07    // Scratchpad Register (R/W) — za I2C test
#define REG_TXLVL   0x08    // Transmit FIFO Level (R — read only)
#define REG_RXLVL   0x09    // Receive FIFO Level (R — read only)
// Registri 0x0A-0x0F so globalni (ne per-channel):
#define REG_IODIR   0x0A    // I/O Direction (R/W) — velja za oba kanala
#define REG_IOSTATE 0x0B    // I/O State (R) — velja za oba kanala
#define REG_IOINTENA 0x0C   // I/O Interrupt Enable (R/W)
// 0x0D rezerviran
#define REG_IOCONTROL 0x0E  // I/O Control (R/W) — software reset bit[2]
#define REG_EFCR    0x0F    // Extra Features Control (R/W)
// Special register set — dostopen samo ko LCR[7]=1, LCR != 0xBF:
#define REG_DLL     0x00    // Divisor Latch LSB (ko DLAB=1)
#define REG_DLH     0x01    // Divisor Latch MSB (ko DLAB=1)
// Enhanced register set — dostopen samo ko LCR=0xBF:
#define REG_EFR     0x02    // Enhanced Features Register

// Makro za I2C subaddress bajt
// Datasheet: subaddr = (reg_addr[3:0] << 3) | (channel[0] << 1) | 0
#define SC16_REG(reg, ch)   ((uint8_t)(((reg) << 3) | ((ch) << 1)))
#define SC16_REG_GLOBAL(reg) ((uint8_t)((reg) << 3))  // za globalne registre

// ============================================================
// IER — Interrupt Enable Register (0x01)
// Datasheet Table 11 (str. 21), Rev. 9.1
//
// Bit 7: CTS interrupt enable (samo z EFR[4]=1)
// Bit 6: RTS interrupt enable (samo z EFR[4]=1)
// Bit 5: Xoff interrupt (samo z EFR[4]=1)
// Bit 4: Sleep mode enable (samo z EFR[4]=1)
// Bit 3: Modem Status interrupt
// Bit 2: Receive Line Status interrupt (OE/FE/PE/BI)
// Bit 1: THR empty interrupt
// Bit 0: RHR interrupt (podatki v RX FIFO)
//
// Za nas: samo bit 2 (line status) in bit 0 (RX data ready)
// IER = 0x05 → RHR interrupt + Receive Line Status interrupt
// OPOMBA: IER[4:7] so dostopni samo ko EFR[4]=1
//         mi EFR[4] ne postavljamo → IER[7:4] so read-only 0
// ============================================================
#define IER_RX_DATA     0x01    // bit 0: RHR interrupt (RX data available)
#define IER_LINE_STATUS 0x04    // bit 2: Receive Line Status interrupt
#define IER_RADAR_INIT  (IER_RX_DATA | IER_LINE_STATUS)  // = 0x05

// ============================================================
// IIR — Interrupt Identification Register (0x02, read only)
// Datasheet Table 6 (str. 13) + Table 13, Rev. 9.1
//
// IIR[0]: interrupt status — 0=interrupt pending, 1=no interrupt
//         POZOR: logika je obrnjena! 0=pending, 1=nič
// IIR[5:1]: interrupt type (prioriteta):
//   IIR[5:0] = 0b000001 (0x01): nič — no interrupt pending
//   IIR[5:0] = 0b000110 (0x06): prio 1 — receiver line status (OE/FE/PE/BI)
//   IIR[5:0] = 0b001100 (0x0C): prio 2 — RX timeout (stale data)
//   IIR[5:0] = 0b000100 (0x04): prio 2 — RHR interrupt (RX data ready)
//   IIR[5:0] = 0b000010 (0x02): prio 3 — THR interrupt (TX empty)
//   IIR[5:0] = 0b000000 (0x00): prio 4 — modem status
//   IIR[5:0] = 0b110000 (0x30): prio 5 — I/O pins
//   IIR[5:0] = 0b010000 (0x10): prio 6 — Xoff interrupt
//   IIR[5:0] = 0b100000 (0x20): prio 7 — CTS/RTS change
//
// KRITIČNO: "Burst reads on IIR should not be performed."
//   (datasheet Table 10, opomba [5])
//   Vsak read IIR = nova I2C transakcija, ne burst!
//
// Kako počistimo interrupt:
//   - RLS (0x06): preberi LSR
//   - RX data (0x04): preberi RHR do praznega
//   - RX timeout (0x0C): preberi RHR do praznega
//   - THR (0x02): samodejno počisti ko pišemo v THR ali LSR bit5=1
//   - Modem (0x00): preberi MSR
// ============================================================
#define IIR_NO_INTERRUPT    0x01    // bit0=1 → ni čakajočega interrupta
#define IIR_TYPE_MASK       0x3F    // bits [5:0]
#define IIR_RLS             0x06    // Receiver Line Status
#define IIR_RX_TIMEOUT      0x0C    // RX Timeout (stale data, 4 char times)
#define IIR_RHR             0x04    // RHR interrupt (data ready)
#define IIR_THR             0x02    // THR interrupt
#define IIR_MODEM           0x00    // Modem Status

// ============================================================
// FCR — FIFO Control Register (0x02, write only)
// Datasheet Table 12 (str. 22-23), Rev. 9.1
//
// Bit 7-6: RX trigger level:
//   00 = 8 znakov, 01 = 16 znakov, 10 = 56 znakov, 11 = 60 znakov
// Bit 5-4: TX trigger level (samo z EFR[4]=1):
//   00 = 8 spaces, 01 = 16 spaces, 10 = 32 spaces, 11 = 56 spaces
// Bit 3: rezerviran (vedno 0)
// Bit 2: reset TX FIFO — samodejno se počisti
// Bit 1: reset RX FIFO — samodejno se počisti
//   OPOMBA: po resetu FIFO počakaj min 2×Tclk XTAL1 pred R/W!
//   Pri 1.8432MHz: Tclk = 543ns → 2×Tclk = ~1μs (zanemarljivo)
// Bit 0: FIFO enable — 1=64-byte FIFO aktiven
//
// LD2410C pošlje frame vsakih ~100ms @ 115200 baud.
// Frame je 21 bajtov (4 hdr + 2 len + 13 data + 4 ftr = 23 skupaj z headerji).
// RX trigger = 8 znakov → IRQ pride preden je cel frame, a RX timeout
// bo sprožil 2. IRQ za preostanek. To je OK ker beremo do praznega.
// Alternativa: trigger = 16 → malo počasneje a manj IRQ-jev.
// Izberemo 8 (default) za minimalno latenco.
// ============================================================
#define FCR_FIFO_ENABLE     0x01    // bit 0: FIFO enable
#define FCR_RX_RESET        0x02    // bit 1: reset RX FIFO
#define FCR_TX_RESET        0x04    // bit 2: reset TX FIFO
#define FCR_RX_TRIGGER_8    0x00    // bits 7:6 = 00 → IRQ ko 8 znakov v FIFO
#define FCR_RX_TRIGGER_16   0x40    // bits 7:6 = 01 → IRQ ko 16 znakov v FIFO
// Naša konfiguracija: FIFO enable + reset oba + trigger 8
#define FCR_INIT_VALUE      (FCR_FIFO_ENABLE | FCR_RX_RESET | FCR_TX_RESET | FCR_RX_TRIGGER_8)
// = 0x07

// ============================================================
// LCR — Line Control Register (0x03)
// Datasheet Table 14 (str. 24), Rev. 9.1
//
// Bit 7: DLAB — Divisor Latch Access Bit
//   1 = dostop do DLL/DLH (baud rate divisor)
//   0 = normalno delovanje (dostop do RHR/THR/IER)
//   KRITIČNO: DLAB=1 blokira dostop do RHR/THR/IER!
//   Vrstni red: DLAB=1 → piši DLL/DLH → DLAB=0
// Bit 6: set break
// Bit 5: set parity (0=odd/even določeno z bit4, 1=force)
// Bit 4: even parity (0=odd, 1=even) — samo ko bit3=1
// Bit 3: parity enable (0=no parity, 1=parity)
// Bit 2: stop bit (0=1 stop bit, 1=1.5 za 5-bit ali 2 stop za 6-8 bit)
// Bits 1:0: word length: 00=5bit, 01=6bit, 10=7bit, 11=8bit
//
// Reset vrednost: 0x1D = 0b0001_1101 = 7-bit, 1 stop, odd parity, DLAB=0
// Naša konfiguracija: 8N1 = 8 bit, no parity, 1 stop → LCR = 0x03
// ============================================================
#define LCR_8N1             0x03    // 8 bit, no parity, 1 stop bit
#define LCR_DLAB            0x80    // Divisor Latch Access Bit
#define LCR_ENHANCED        0xBF    // 1011 1111 — dostop do EFR (Enhanced Features)

// ============================================================
// MCR — Modem Control Register (0x04)
// Datasheet Table 15 (str. 25), Rev. 9.1
//
// Bit 7: clock divisor (prescaler ÷4) — samo z EFR[4]=1
//   0 = prescaler ÷1 (default po resetu)
//   1 = prescaler ÷4
// Bit 6: IrDA mode — ne rabimo
// Bit 5: Xon Any — ne rabimo
// Bit 4: loopback enable — samo za test
// Bit 3: rezerviran
// Bit 2: TCR/TLR enable — potrebno za programabilne trigger levele
// Bit 1: RTS
// Bit 0: DTR (GPIO5)
//
// Naša konfiguracija: MCR = 0x00 (vse izklopljeno, prescaler ÷1)
// OPOMBA: MCR[7] je dostopen samo ko EFR[4]=1
// ============================================================
#define MCR_NORMAL          0x00

// ============================================================
// LSR — Line Status Register (0x05, read only)
// Datasheet Table 16 (str. 26), Rev. 9.1
//
// Bit 7: FIFO data error — 1=error v vsaj enem bajtu v RX FIFO
//   Počisti se ko so vsi napačni bajti prebrani iz FIFO.
//   Pogoj za RLS interrupt.
// Bit 6: THR and TSR empty — 1=TX FIFO in shift register prazna
// Bit 5: THR empty — 1=TX FIFO pod trigger levelom
// Bit 4: break interrupt — 1=break condition zaznana
// Bit 3: framing error — 1=napaka pri stop bitu
// Bit 2: parity error
// Bit 1: overrun error — 1=RX FIFO poln, nova data izgubljena
//   TO JE PRIČAKOVANO pri polling načinu! V IRQ načinu se ne sme dogajati.
// Bit 0: data in receiver — 1=v RX FIFO so podatki za branje
//
// Reset vrednost: 0x60 (bit5=1 THR empty, bit6=1 THR+TSR empty) — potrjeno v testu!
// ============================================================
#define LSR_RX_DATA_READY   0x01    // bit 0: podatki v RX FIFO
#define LSR_OVERRUN_ERR     0x02    // bit 1: overrun (RX FIFO poln)
#define LSR_PARITY_ERR      0x04    // bit 2: parity error
#define LSR_FRAMING_ERR     0x08    // bit 3: framing error
#define LSR_BREAK_INT       0x10    // bit 4: break interrupt
#define LSR_THR_EMPTY       0x20    // bit 5: TX FIFO pod trigger
#define LSR_TX_EMPTY        0x40    // bit 6: TX FIFO + shift register prazna
#define LSR_FIFO_ERR        0x80    // bit 7: error v RX FIFO
#define LSR_ANY_ERROR       (LSR_OVERRUN_ERR | LSR_PARITY_ERR | LSR_FRAMING_ERR)

// ============================================================
// RXLVL — Receive FIFO Level Register (0x09, read only)
// Datasheet Table 21 (str. 30), Rev. 9.1
//
// Vrne število bajtov trenutno v RX FIFO (0–64).
// To je KLJUČNI register za IRQ način:
//   Ko IRQ sproži → preberi RXLVL → veš točno koliko bajtov brati
//   → en burst read namesto byte-by-byte polling
// OPOMBA: RXLVL ni per-channel v smislu naslova — register naslov
//         se per-channel določi z SC16_REG(REG_RXLVL, ch) makrom.
// ============================================================

// ============================================================
// I2C NASLOV — SC16IS752
// Datasheet Table 32 (str. 42), Rev. 9.1
//
// 7-bitni I2C naslov format: [1 0 1 A1 A0 x x] kjer xx=00
// Osnova: 0b1010000 = 0x50 >> 1 = 0x28? NE!
// Pravilno: osnova = 0x48 (A1=0, A0=0)
//   A1=GND, A0=GND → 0x48
//   A1=GND, A0=VCC → 0x49
//   A1=VCC, A0=GND → 0x4C  ← SC16IS752 #1 v našem projektu
//   A1=VCC, A0=VCC → 0x4D
// POZOR: datasheet Table 32 prikazuje 8-bitni zapis (z R/W bitom).
//        Arduino Wire uporablja 7-bitni naslov!
//        8-bit naslov 0x90 (write) = 7-bit 0x48
//        8-bit naslov 0x98 (write) = 7-bit 0x4C
// Naša konfiguracija:
//   SC16IS752 #1: A0=VCC, A1=GND → 7-bit I2C = 0x48 ✓ (potrjeno v testu)
//   SC16IS752 #2: A0=VCC, A1=VCC → 7-bit I2C = 0x4C ✓ (potrjeno v testu)
// ============================================================
#define SC16_ADDR_1         0x48
#define SC16_ADDR_2         0x4C

// ============================================================
// BAUD RATE KONFIGURACIJA
// Datasheet Section 7.8 + Table 7, Rev. 9.1
//
// Formula: divisor = XTAL_freq / (prescaler × 16 × baud_rate)
//   XTAL = 1.8432 MHz (potrjeno na CJMCU modulu)
//   baud = 115200
//   prescaler = 1 (MCR[7]=0, default po resetu)
//   divisor = 1843200 / (1 × 16 × 115200) = 1843200 / 1843200 = 1.000
//   Napaka: 0% — točno!
//
// Datasheet Table 7 (baud rates za 1.8432 MHz):
//   115200 ni v tabeli (presega 56000 max v tabeli)
//   ampak formula je veljavna za katerikoli divisor ≥ 1.
//   divisor = 1 je minimalni možni (DLL=1, DLH=0).
//
// Sekvenca programiranja baud rate (KRITIČNO!):
//   1. LCR = LCR_DLAB (0x80) → aktiviraj dostop do DLL/DLH
//   2. DLL = 0x01 (divisor LSB)
//   3. DLH = 0x00 (divisor MSB)
//   4. LCR = LCR_8N1 (0x03) → deaktiviraj DLAB, nastavi 8N1
//   OPOMBA: med DLAB=1 RHR/THR/IER NISO dostopni!
//   OPOMBA: DLL in DLH nista resetirana z SW/HW resetom (datasheet Table 4)
// ============================================================
#define SC16_DLL_115200     0x01    // divisor = 1 za 115200 @ 1.8432MHz XTAL
#define SC16_DLH_115200     0x00

// ============================================================
// IOCONTROL — I/O Control Register (0x0E, globalen)
// Datasheet Table 26 (str. 34), Rev. 9.1
//
// Bit 3: rezerviran
// Bit 2: UART software reset
//   1 → reset vseh internih registrov (razen DLL/DLH/SPR/XONx/XOFFx)
//        ekvivalentno HW reset — samodejno se vrne na 0
//   OPOMBA: enako kot HW RESET pin ali POR — vsi registri na reset vrednosti
// Bit 1: GPIO[7:4] → 0=GPIO, 1=modem signals (RIA/CDA/DTRA/DSRA)
// Bit 0: GPIO[3:0] → 0=GPIO, 1=modem signals (RIB/CDB/DTRB/DSRB)
//        + latch mode za IOState register
//
// Naša konfiguracija: 0x00 (vse GPIO, ni software reset)
// ============================================================
#define IOCONTROL_SW_RESET  0x08    // bit 3: software reset

// ============================================================
// PROJEKTNE KONSTANTE
// ============================================================

// Wire1 pini (iz config.h / bsp.h)
#define RADAR_I2C_SDA       17
#define RADAR_I2C_SCL       18
#define RADAR_I2C_FREQ      100000  // 100kHz — datasheet max 400kHz, mi 100 za zanesljivost

// IRQ pini
#define RADAR_IRQ_CHIP1     41      // SC16IS752 #1 → IO41
#define RADAR_IRQ_CHIP2     42      // SC16IS752 #2 → IO42

// UART kanali
#define UART_CH_A           0
#define UART_CH_B           1

// FreeRTOS task parametri (RADAR_TASK_STACK je definiran v config.h)
#define RADAR_TASK_PRIO     4       // enako kot sensorTask
#define RADAR_TASK_CORE     1       // Core1 — skupaj z ostalimi taski

// IRQ queue — sprejme chip_id (uint8_t: 0=chip1, 1=chip2)
// Velikost 8: da ne izgubimo IRQ-jev če task zamudi (npr. med I2C branjem)
#define RADAR_IRQ_QUEUE_SIZE 8

// Maksimalna dolžina LD2410C data sekcije
#define LD2410_MAX_DATA_LEN 64

// LD2410C frame magic bytes — potrjeni iz hardware testa (Rev. hardware test v1.0)
// OPOMBA: nekateri online viri navajajo FD/FC/FB/FA in 04/03/02/01
//         Naš test je potrdil F4/F3/F2/F1 in F8/F7/F6/F5 — to je dejanski HW!
static const uint8_t LD2410_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t LD2410_FOOTER[4] = {0xF8, 0xF7, 0xF6, 0xF5};
#define LD2410_TYPE_BASIC   0x02    // basic reporting frame
#define LD2410_TYPE_ENG     0x01    // engineering mode frame (ignoriramo)
#define LD2410_HEAD_MARKER  0xAA
#define LD2410_TAIL_MARKER  0x55

// ============================================================
// INTERNI TIPI
// ============================================================

// Parser stanje za en kanal
typedef struct {
    uint8_t  state;         // 0=wait_hdr, 1=in_len, 2=in_data, 3=wait_ftr
    uint8_t  hdr_idx;       // koliko header bajtov smo že ujeli
    uint8_t  ftr_idx;       // koliko footer bajtov smo že ujeli
    uint8_t  len_idx;       // 0 ali 1 (length je 2 bajta LE)
    uint16_t data_len;      // pričakovana dolžina data sekcije
    uint8_t  data_idx;      // trenutni indeks v data_buf
    uint8_t  data_buf[LD2410_MAX_DATA_LEN];
} FrameParser;

// Stanje enega UART kanala
typedef struct {
    uint8_t         chip_addr;
    uint8_t         uart_ch;    // UART_CH_A ali UART_CH_B
    RadarSensorId   sensor_id;
    FrameParser     parser;
    RadarSensorStatus status;
} RadarChannel;

// Stanje celotnega HAL
typedef struct {
    RadarChannel    ch[RADAR_SENSOR_COUNT]; // 4 kanali
    RadarFrameCallback callback;
    QueueHandle_t   irq_queue;
    TaskHandle_t    task_handle;
    bool            initialized;
    bool            chip1_ok;
    bool            chip2_ok;
} RadarState;

// ============================================================
// STATIČNE SPREMENLJIVKE
// ============================================================

static RadarState s_radar;

// ============================================================
// I2C POMOŽNE FUNKCIJE
// Vse klicati samo iz task konteksta (ne ISR)!
// Wire1 mutex mora biti pridobljen pred klicem.
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

// Burst read iz RHR — učinkoviteje kot byte-by-byte
// Prebere do max_len bajtov, vrne dejansko prebrano število
// Datasheet: burst read IIR ni dovoljen, RHR burst read JE dovoljen
static uint8_t sc16_read_fifo(uint8_t addr, uint8_t ch, uint8_t* buf, uint8_t max_len) {
    uint8_t rxlvl = 0;
    // Preberi RXLVL — koliko bajtov je v FIFO
    // Datasheet Table 21: RXLVL = število bajtov v RX FIFO (0-64)
    if (!sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl)) return 0;
    if (rxlvl == 0) return 0;

    uint8_t to_read = (rxlvl < max_len) ? rxlvl : max_len;

    // Burst read: pošlji subaddress enkrat, beri N bajtov
    Wire1.beginTransmission(addr);
    Wire1.write(SC16_REG(REG_RHR, ch));
    if (Wire1.endTransmission(false) != 0) return 0;

    uint8_t received = Wire1.requestFrom(addr, to_read);
    for (uint8_t i = 0; i < received; i++) {
        buf[i] = Wire1.read();
    }
    return received;
}

static bool sc16_ping(uint8_t addr) {
    Wire1.beginTransmission(addr);
    return (Wire1.endTransmission() == 0);
}

// ============================================================
// SC16IS752 INICIALIZACIJA ENEGA UART KANALA
// Sekvenca po datasheet Section 7.8 in Table 4
// ============================================================

static bool sc16_init_channel(uint8_t addr, uint8_t ch) {
    bool ok = true;

    // Korak 1: LCR DLAB=1 → dostop do baud rate divisor registrov
    // Datasheet: "DLL and DLH must be written to in order to program the baud rate"
    // OPOMBA: ko DLAB=1, naslova 0x00 in 0x01 kažeta na DLL/DLH (ne RHR/IER)
    if (!sc16_write(addr, SC16_REG(REG_LCR, ch), LCR_DLAB)) {
        RADE("sc16 0x%02X ch%c: LCR DLAB set fail", addr, ch==0?'A':'B');
        return false;
    }

    // Korak 2: DLL = 1 (divisor LSB)
    // Naslov DLL ko DLAB=1: SC16_REG(REG_DLL=0x00, ch) — isti naslov kot RHR!
    if (!sc16_write(addr, SC16_REG(REG_DLL, ch), SC16_DLL_115200)) {
        RADE("sc16 0x%02X ch%c: DLL fail", addr, ch==0?'A':'B');
        ok = false;
    }

    // Korak 3: DLH = 0 (divisor MSB)
    // Naslov DLH ko DLAB=1: SC16_REG(REG_DLH=0x01, ch) — isti naslov kot IER!
    if (!sc16_write(addr, SC16_REG(REG_DLH, ch), SC16_DLH_115200)) {
        RADE("sc16 0x%02X ch%c: DLH fail", addr, ch==0?'A':'B');
        ok = false;
    }

    // Korak 4: LCR = 8N1, DLAB=0
    // KRITIČNO: takoj po pisanju DLL/DLH postavi DLAB=0!
    // S tem se aktivira 8N1 konfiguracija in deaktivira dostop do DLL/DLH.
    if (!sc16_write(addr, SC16_REG(REG_LCR, ch), LCR_8N1)) {
        RADE("sc16 0x%02X ch%c: LCR 8N1 fail", addr, ch==0?'A':'B');
        ok = false;
    }

    // Korak 5: FCR — FIFO enable + reset TX + reset RX + trigger 8
    // Datasheet: po RX FIFO resetu počakaj 2×Tclk XTAL1 = ~1μs
    // Pri 1.8432MHz je to zanemarljivo, a dodamo kratko pavzo
    if (!sc16_write(addr, SC16_REG(REG_FCR, ch), FCR_INIT_VALUE)) {
        RADE("sc16 0x%02X ch%c: FCR fail", addr, ch==0?'A':'B');
        ok = false;
    }
    delayMicroseconds(5); // počakaj po FIFO resetu (datasheet zahteva 2×Tclk)

    // Korak 6: MCR = 0x00 — prescaler ÷1, vse izklopljeno
    if (!sc16_write(addr, SC16_REG(REG_MCR, ch), MCR_NORMAL)) {
        RADW("sc16 0x%02X ch%c: MCR fail (nekritično)", addr, ch==0?'A':'B');
        // nekritično — nadaljujemo
    }

    // Korak 7: IER = 0x05 — vklopi RX data + RX line status interrupt
    // Datasheet Table 11: IER[0]=1 RHR interrupt, IER[2]=1 RLS interrupt
    // OPOMBA: IER mora biti zadnji — ko ga nastavimo, IRQ pin postane aktiven!
    if (!sc16_write(addr, SC16_REG(REG_IER, ch), IER_RADAR_INIT)) {
        RADE("sc16 0x%02X ch%c: IER fail", addr, ch==0?'A':'B');
        ok = false;
    }

    // Verifikacija — preberi LSR
    // Pričakovano po resetu: 0x60 = bit5(THR empty) + bit6(TX empty) — potrjeno v testu
    uint8_t lsr = 0;
    if (sc16_read(addr, SC16_REG(REG_LSR, ch), lsr)) {
        RADI("sc16 0x%02X ch%c: LSR=0x%02X (THRE:%d TXE:%d RDR:%d %s)",
            addr, ch==0?'A':'B', lsr,
            (lsr & LSR_THR_EMPTY) ? 1 : 0,
            (lsr & LSR_TX_EMPTY)  ? 1 : 0,
            (lsr & LSR_RX_DATA_READY) ? 1 : 0,
            ok ? "OK" : "WARN");
        if (lsr & LSR_ANY_ERROR) {
            RADW("sc16 0x%02X ch%c: LSR napaka OE:%d PE:%d FE:%d — preveri kabel/napajanje",
                addr, ch==0?'A':'B',
                (lsr & LSR_OVERRUN_ERR)  ? 1 : 0,
                (lsr & LSR_PARITY_ERR)   ? 1 : 0,
                (lsr & LSR_FRAMING_ERR)  ? 1 : 0);
        }
    }

    // Počisti vse pending interrupte ki so nastali med init sekvenco.
    // SC16IS752 ima ob resetu THR interrupt pending (TX FIFO prazen, LSR=0x60).
    // Ta interrupt blokira IRQ pin LOW in preprečuje RHR interruptom da pridejo skozi.
    // Sekvenca čiščenja: beri IIR dokler ni 0x01 (no interrupt pending).
    // Branje IIR počisti THR interrupt (datasheet Section 7.5 + IIR clearing rules).
    // Preberi tudi RHR če so kakšni bajti že prišli med init.
    {
        uint8_t iir_clear = 0;
        uint8_t clear_loops = 0;
        while (clear_loops++ < 8) {
            if (!sc16_read(addr, SC16_REG(REG_IIR, ch), iir_clear)) break;
            if (iir_clear & IIR_NO_INTERRUPT) break; // 0x01 = čisto
            // Preberi RHR če so podatki
            uint8_t rxlvl = 0;
            sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);
            if (rxlvl > 0) {
                uint8_t dummy[64];
                sc16_read_fifo(addr, ch, dummy, sizeof(dummy));
            }
            // Preberi LSR za RLS interrupt clearing
            uint8_t lsr2 = 0;
            sc16_read(addr, SC16_REG(REG_LSR, ch), lsr2);
        }
        RADI("sc16 0x%02X ch%c: pending interrupts počiščeni (IIR=0x%02X po %d korakih)",
            addr, ch==0?'A':'B', iir_clear, clear_loops);
    }

    return ok;
}

// ============================================================
// LD2410C FRAME PARSER — en bajt naenkrat
// Vrne true ko je frame poln in veljaven, zapolni out.
// ============================================================

static bool parse_byte(FrameParser& p, uint8_t b, RadarFrame& out, RadarSensorId id) {
    switch (p.state) {

    case 0: // WAIT_HEADER — iščemo F4 F3 F2 F1
        if (b == LD2410_HEADER[p.hdr_idx]) {
            p.hdr_idx++;
            if (p.hdr_idx == 4) {
                p.hdr_idx = 0;
                p.state   = 1;
                p.data_len = 0;
                p.data_idx = 0;
                p.len_idx  = 0;
            }
        } else {
            p.hdr_idx = 0;
            if (b == LD2410_HEADER[0]) p.hdr_idx = 1;
        }
        break;

    case 1: // IN_LENGTH — 2 bajta little-endian uint16
        if (p.len_idx == 0) {
            p.data_len = b;
            p.len_idx  = 1;
        } else {
            p.data_len |= ((uint16_t)b << 8);
            if (p.data_len == 0 || p.data_len > LD2410_MAX_DATA_LEN) {
                RADW("radar parser [%d]: neveljavna dolžina %d → reset", (int)id, p.data_len);
                p.state   = 0;
                p.hdr_idx = 0;
                return false;
            }
            p.state    = 2;
            p.data_idx = 0;
        }
        break;

    case 2: // IN_DATA
        if (p.data_idx < LD2410_MAX_DATA_LEN) {
            p.data_buf[p.data_idx++] = b;
        }
        if (p.data_idx >= p.data_len) {
            p.state   = 3;
            p.ftr_idx = 0;
        }
        break;

    case 3: // WAIT_FOOTER — iščemo F8 F7 F6 F5
        if (b == LD2410_FOOTER[p.ftr_idx]) {
            p.ftr_idx++;
            if (p.ftr_idx == 4) {
                // Cel frame prejet — parsiraj
                p.state   = 0;
                p.hdr_idx = 0;
                p.ftr_idx = 0;

                if (p.data_len >= 13 &&
                    p.data_buf[0] == LD2410_TYPE_BASIC &&
                    p.data_buf[1] == LD2410_HEAD_MARKER) {

                    out.sensor_id       = id;
                    out.detection       = p.data_buf[2];
                    out.moving_dist_cm  = (uint16_t)p.data_buf[3] | ((uint16_t)p.data_buf[4] << 8);
                    out.moving_energy   = p.data_buf[5];
                    out.static_dist_cm  = (uint16_t)p.data_buf[6] | ((uint16_t)p.data_buf[7] << 8);
                    out.static_energy   = p.data_buf[8];
                    out.detect_dist_cm  = (uint16_t)p.data_buf[9] | ((uint16_t)p.data_buf[10] << 8);
                    out.timestamp_ms    = millis();
                    return true;

                } else if (p.data_len >= 1 && p.data_buf[0] == LD2410_TYPE_ENG) {
                    // Engineering mode frame — ignoriramo, ni napaka
                    RADD("radar [%d]: engineering frame (ignoriran)", (int)id);
                }
                // drug tip — ignoriramo tiho
            }
        } else {
            // Footer napaka — F vrednosti se pojavijo tudi v data payload-u
            // Samo resetiramo footer counter, ne gre za resno napako
            p.ftr_idx = 0;
            if (b == LD2410_FOOTER[0]) p.ftr_idx = 1;
        }
        break;
    }
    return false;
}

// ============================================================
// process_chip_irq — kernel-style IRQ drain loop
// Vir: Linux kernel sc16is7xx.c (torvalds/linux)
//
// Implementacija sledi Linux kernel sc16is7xx_irq() vzorcu:
//   do {
//     keep_polling = false;
//     za vsak kanal: keep_polling |= obdelaj_kanal();
//   } while (keep_polling);
//
// Zakaj zanka: LD2410C pošilja neprestano @ 115200 baud.
// Med branjem FIFO priteče novih bajtov → IIR ostane aktiven
// → IRQ pin ne gre HIGH → nobene FALLING edge → ISR ne pride.
// Zanka bere dokler IIR=0x01 (no interrupt) na VSEH kanalih.
//
// Silicon bug (errata 18.1.4):
//   SC16IS752 lahko javi RX timeout interrupt ampak RXLVL=0.
//   Fix: preberi 1 bajt iz RHR da počistiš interrupt.
// ============================================================

static bool process_one_channel(RadarChannel& rc, uint8_t addr, uint8_t ch) {
    // Vrne true če je bil interrupt pending (kanal je potreboval obdelavo)

    uint8_t iir = 0;
    // KRITIČNO: IIR read NI burst (datasheet opomba [5])
    if (!sc16_read(addr, SC16_REG(REG_IIR, ch), iir)) {
        rc.status.i2c_errors++;
        return false;
    }

    // IIR[0]=1 → ni čakajočega interrupta → kanal je čist
    if (iir & IIR_NO_INTERRUPT) return false;

    uint8_t iir_type = iir & IIR_TYPE_MASK;

    if (iir_type == IIR_RHR ||
        iir_type == IIR_RX_TIMEOUT ||
        iir_type == IIR_RLS) {

        // Preberi RXLVL
        uint8_t rxlvl = 0;
        sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);

        // SILICON BUG FIX (errata 18.1.4, dokumentirano v Linux kernel sc16is7xx.c):
        // SC16IS752 lahko javi RX_TIMEOUT interrupt ampak RXLVL=0.
        // "When this happens, read one byte from the FIFO to clear the interrupt."
        if (iir_type == IIR_RX_TIMEOUT && rxlvl == 0) {
            rxlvl = 1;
        }

        if (iir_type == IIR_RLS) {
            // Receiver Line Status — preberi LSR da počistimo interrupt
            uint8_t lsr = 0;
            sc16_read(addr, SC16_REG(REG_LSR, ch), lsr);
            if (lsr & LSR_OVERRUN_ERR) {
                rc.status.parse_errors++;
                rc.status.oe_count++;

                // OE! log throttle: max 1× per RADAR_OE_LOG_INTERVAL_MS per kanal.
                // OE! ob zagonu (prvih 10s) je normalen — TOF init blokira Wire1 ~300ms.
                // Med normalnim delovanjem po round-robin popravku OE! ne bi smeli biti pogosti.
                uint32_t now_oe = millis();
                if ((now_oe - rc.status.last_oe_log_ms) >= RADAR_OE_LOG_INTERVAL_MS) {
                    rc.status.last_oe_log_ms = now_oe;
                    if (rc.status.oe_count > 1) {
                        RADW("SC16[0x%02X] ch%c OE! ×%lu (skupno: %lu)",
                             addr, ch==0?'A':'B',
                             (unsigned long)rc.status.oe_count,
                             (unsigned long)rc.status.parse_errors);
                    } else {
                        RADW("SC16[0x%02X] ch%c OE! (skupno: %lu)",
                             addr, ch==0?'A':'B',
                             (unsigned long)rc.status.parse_errors);
                    }
                    rc.status.oe_count = 0;
                }
            }
            // Če je RLS + RHR skupaj, nadaljujemo z branjem FIFO
            if (rxlvl == 0) return true; // RLS brez podatkov — počiščeno
        }

        if (rxlvl > 0) {
            uint8_t fifo_buf[64];
            uint8_t n = sc16_read_fifo(addr, ch, fifo_buf, sizeof(fifo_buf));
            rc.status.active   = true;
            rc.status.irq_count++;

            for (uint8_t i = 0; i < n; i++) {
                RadarFrame frame;
                if (parse_byte(rc.parser, fifo_buf[i], frame, rc.sensor_id)) {
                    rc.status.frames_ok++;
                    rc.status.last_frame    = frame;
                    rc.status.last_frame_ms = millis();

                    // FIFO drain teče nespremenjen → OE! so preprečeni.
                    // Callback (→ EventBus → light_logic) se sproži max
                    // 1× per RADAR_PUBLISH_INTERVAL_MS per kanal.
                    uint32_t now_ms = millis();
                    if (s_radar.callback &&
                        (now_ms - rc.status.last_publish_ms) >= RADAR_PUBLISH_INTERVAL_MS) {
                        rc.status.last_publish_ms = now_ms;
                        s_radar.callback(frame);
                    }
                }
            }
        }
        return true; // interrupt bil pending

    } else if (iir_type == IIR_THR) {
        // THR interrupt — TX FIFO prazen, ne pišemo v TX
        // Branje IIR (že opravljeno) zadostuje za clearing
        // Vrni true da zanka še enkrat preveri stanje
        return true;

    } else {
        // Drugi interrupt tipi (modem, I/O, Xoff, CTS/RTS)
        RADD("SC16[0x%02X] ch%c: neobravnavan IIR=0x%02X",
            addr, ch==0?'A':'B', iir);
        return false;
    }
}

static void process_chip_irq(uint8_t chip_idx) {
    uint8_t addr    = (chip_idx == 0) ? SC16_ADDR_1 : SC16_ADDR_2;
    uint8_t ch_base = chip_idx * 2;

    // Kernel-style single-chip drain — beri oba kanala tega čipa
    // dokler noben kanal nima več pending interrupta.
    //
    // POZOR: Ta funkcija draina SAMO en čip (chip_idx parameter).
    // Izmenjavanje med čipoma (round-robin) se izvaja v radarTask
    // while zanki ZUNAJ te funkcije. Razlog: round-robin med čipoma
    // preprečuje OE! ker chip2 čaka max ~3ms (ena chip1 iteracija)
    // namesto 12ms (celoten chip1 drain) — oba čipa sta tako servisirana
    // preden se njuni FIFO zapolnita (5.5ms pri 115200 baud).
    //
    // max_loops je zdaj RADAR_DRAIN_MAX_LOOPS (config.h, default 4).
    // Pri normalnem LD2410C prometu zadostujeta 2-3 iteraciji.
    uint8_t max_loops = RADAR_DRAIN_MAX_LOOPS;
    bool keep_polling;
    do {
        keep_polling = false;
        for (uint8_t ch = 0; ch <= 1; ch++) {
            RadarChannel& rc = s_radar.ch[ch_base + ch];
            if (!rc.status.active) continue;
            keep_polling |= process_one_channel(rc, addr, ch);
        }
    } while (keep_polling && --max_loops > 0);

    if (max_loops == 0) {
        // max_loops dosežen — normalno pri burst prometu ob zagonu.
        // Med normalnim delovanjem se NE sme dogajati pogosto.
        // Če se pojavlja redno → zmanjšaj RADAR_DRAIN_MAX_LOOPS ali
        // preveri ali LD2410C pošilja engineering mode frames.
        RADD("SC16[0x%02X] IRQ drain: max_loops=%d dosežen",
             addr, RADAR_DRAIN_MAX_LOOPS);
    }
}

// ============================================================
// DEAD CODE — ISR funkciji (ohranjeni za referenčno dokumentacijo)
// ============================================================
// attachInterrupt() NI v uporabi — IDF 5.3 pioarduino limitation.
// gpio_isr_register() vedno gre prek esp_ipc_call_blocking() →
// ipc_task (1KB stack) → heap_caps_malloc overflow → crash.
// Dokumentirano v crash_report_solution.md (2026-05).
//
// PRIHODNOST: Ko pioarduino posodobi CONFIG_ESP_IPC_TASK_STACK_SIZE > 1024,
// lahko zamenjamo polling z ISR arhitekturo za nižjo latenco:
//   attachInterrupt(digitalPinToInterrupt(RADAR_IRQ_CHIP1), isr_chip1, FALLING);
//   attachInterrupt(digitalPinToInterrupt(RADAR_IRQ_CHIP2), isr_chip2, FALLING);
// Preveriti: ~/.platformio/packages/framework-arduinoespressif32-libs/
//            esp32s3/sdkconfig → CONFIG_ESP_IPC_TASK_STACK_SIZE
// ============================================================

static void IRAM_ATTR isr_chip1() {
    uint8_t chip_id = 0;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_radar.irq_queue, &chip_id, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void IRAM_ATTR isr_chip2() {
    uint8_t chip_id = 1;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_radar.irq_queue, &chip_id, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// ============================================================
// RADAR TASK
// Čaka na IRQ queue, obdela interrupt, spet čaka.
// Teče na Core1, prio 4 (enako kot sensorTask).
// ============================================================

static void radarTask(void* pvParams) {
    RADI("radarTask start — Core%d", xPortGetCoreID());
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    // Izmeri dejanski stack ob zagonu za debug — primerja z RADAR_TASK_STACK
    // Pričakovano: vsaj 3000B free pri RADAR_TASK_STACK=8192
    // Če je < 2000B → povečaj RADAR_TASK_STACK v config.h
    RADI("radarTask stack ob zagonu: %lu B free / %d B total",
         (unsigned long)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t),
         RADAR_TASK_STACK);

    // Preberi IRQ pin stanje pred polling zagonom — diagnostika
    int irq1_before = digitalRead(RADAR_IRQ_CHIP1);
    int irq2_before = digitalRead(RADAR_IRQ_CHIP2);
    RADI("IRQ pin stanje ob zagonu: IO%d=%s IO%d=%s",
        RADAR_IRQ_CHIP1, irq1_before == HIGH ? "HIGH(idle)" : "LOW(!)",
        RADAR_IRQ_CHIP2, irq2_before == HIGH ? "HIGH(idle)" : "LOW(!)");

    // IRQ polling — gpio_install_isr_service() ni možen z IDF 5.3 pioarduino:
    // gpio_isr_register() vedno gre prek esp_ipc_call_blocking(xPortGetCoreID(), ...)
    // → ipc_task izvede registracijo na 1KB stacku → heap_caps_malloc overflow.
    // Polling z 5ms intervalom zadostuje za SC16IS752 FIFO (64B pri 9600baud ≈ 640ms).
    RADI("IRQ polling aktiven na IO%d/IO%d (ni attachInterrupt — IDF 5.3 IPC omejitev)",
         RADAR_IRQ_CHIP1, RADAR_IRQ_CHIP2);

    // Kratek delay — počakaj da SC16IS752 morda že ima data v FIFO
    vTaskDelay(pdMS_TO_TICKS(200));

    // Preveri IRQ pin stanje po init
    int irq1_after = digitalRead(RADAR_IRQ_CHIP1);
    int irq2_after = digitalRead(RADAR_IRQ_CHIP2);
    RADI("IRQ pin stanje po init: IO%d=%s IO%d=%s",
        RADAR_IRQ_CHIP1, irq1_after == HIGH ? "HIGH(idle)" : "LOW(data!)",
        RADAR_IRQ_CHIP2, irq2_after == HIGH ? "HIGH(idle)" : "LOW(data!)");

    // Če je IRQ že LOW — počisti FIFO ročno (zamujeni IRQ med init)
    if (irq1_after == LOW && s_radar.chip1_ok) {
        RADI("IO%d LOW — ročno čiščenje FIFO chip1...", RADAR_IRQ_CHIP1);
        SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
            process_chip_irq(0);
            xSemaphoreGive(mtx);
        }
    }
    if (irq2_after == LOW && s_radar.chip2_ok) {
        RADI("IO%d LOW — ročno čiščenje FIFO chip2...", RADAR_IRQ_CHIP2);
        SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
            process_chip_irq(1);
            xSemaphoreGive(mtx);
        }
    }

    // Snapshot parse_errors po boot čiščenju — te so nastale med init,
    // ne med normalnim delovanjem. Odštejemo jih pri prikazu statistike.
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        s_radar.ch[i].status.boot_parse_errors = s_radar.ch[i].status.parse_errors;
    }
    RADI("Boot parse_err snapshot: ch0=%lu ch1=%lu ch2=%lu ch3=%lu",
        (unsigned long)s_radar.ch[0].status.boot_parse_errors,
        (unsigned long)s_radar.ch[1].status.boot_parse_errors,
        (unsigned long)s_radar.ch[2].status.boot_parse_errors,
        (unsigned long)s_radar.ch[3].status.boot_parse_errors);

    static const char* names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};
    uint32_t last_status_ms = 0;
    bool     first_status   = true;

    while (true) {
        esp_task_wdt_reset();

        // ============================================================
        // ROUND-ROBIN DRAIN (2026-05, OE! odprava)
        // ============================================================
        // Problem prejšnje implementacije:
        //   chip1 se draina do konca (max 12ms z max_loops=4),
        //   šele nato chip2. Med 12ms chip2 FIFO (64B) se zapolni
        //   v 5.5ms → OE! neizogibni.
        //
        // Rešitev: v vsaki iteraciji 1× chip1, 1× chip2.
        //   Chip2 čaka max ~3ms (ena chip1 iteracija) << 5.5ms FIFO.
        //   OE! izginejo ker nobeden čip ne čaka predolgo.
        //
        // Mutex timeout zmanjšan 50→20ms: z max_loops=4 en
        //   process_chip_irq() traja ~12ms → 20ms je dovolj in
        //   hitreje sprosti Wire1 za TOF in appTask.
        //   Timeout napake → i2c_errors++ (brez LOG_WARN v zanki).
        // ============================================================
        {
            bool any_irq = false;
            uint8_t rr_loops = RADAR_DRAIN_MAX_LOOPS * 2; // ×2 ker sta 2 čipa

            bool chip1_pending = s_radar.chip1_ok && (digitalRead(RADAR_IRQ_CHIP1) == LOW);
            bool chip2_pending = s_radar.chip2_ok && (digitalRead(RADAR_IRQ_CHIP2) == LOW);

            while ((chip1_pending || chip2_pending) && rr_loops-- > 0) {

                // --- Chip 1 --- ena drain iteracija
                if (chip1_pending) {
                    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
                    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
                        process_chip_irq(0);
                        xSemaphoreGive(mtx);
                        any_irq = true;
                    } else {
                        s_radar.ch[0].status.i2c_errors++;
                        s_radar.ch[1].status.i2c_errors++;
                    }
                    chip1_pending = s_radar.chip1_ok && (digitalRead(RADAR_IRQ_CHIP1) == LOW);
                }

                // --- Chip 2 --- ena drain iteracija (takoj za chip1)
                // Chip2 čaka max ~3ms (ena chip1 iteracija) << 5.5ms FIFO zapolnitev
                if (chip2_pending) {
                    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
                    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
                        process_chip_irq(1);
                        xSemaphoreGive(mtx);
                        any_irq = true;
                    } else {
                        s_radar.ch[2].status.i2c_errors++;
                        s_radar.ch[3].status.i2c_errors++;
                    }
                    chip2_pending = s_radar.chip2_ok && (digitalRead(RADAR_IRQ_CHIP2) == LOW);
                }
            }

            if (!any_irq) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } // end round-robin block

        // Recovery watchdog je bil tukaj (v3) — premaknjen v sensorTask
        // kjer se izvaja sinhrono s TOF watchdog-om vsakih 10 minut.
        // Razlog: agresivni 2s recovery je povzročal OE zanko ker je
        // process_chip_irq() drain zanka zasedla mutex do 160ms.
        // IRQ pin LOW med normalnim delovanjem ni napaka — je normalno
        // stanje med polling → process_chip_irq() drain sekvence.

        // Periodični status log — 10s po zagonu, nato vsako minuto
        uint32_t now      = millis();
        uint32_t interval = first_status ? 10000 : 60000;
        if (now - last_status_ms >= interval) {
            last_status_ms = now;
            first_status   = false;

            uint32_t total_frames = 0, total_irq = 0, total_errors = 0;
            for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
                total_frames += s_radar.ch[i].status.frames_ok;
                total_irq    += s_radar.ch[i].status.irq_count;
                // Odštejemo boot_parse_errors — prikazujemo samo runtime napake
                total_errors += (s_radar.ch[i].status.parse_errors
                                - s_radar.ch[i].status.boot_parse_errors)
                              + s_radar.ch[i].status.i2c_errors;
            }

            // Preberi trenutno stanje IRQ pinov — takoj vidimo če je hardware problem
            int pin1 = s_radar.chip1_ok ? digitalRead(RADAR_IRQ_CHIP1) : -1;
            int pin2 = s_radar.chip2_ok ? digitalRead(RADAR_IRQ_CHIP2) : -1;

            RADI("--- Radar status (uptime %lus) čipi:%s%s "
                 "frames:%lu IRQ:%lu err:%lu | pin IO%d=%s IO%d=%s ---",
                (unsigned long)(now / 1000),
                s_radar.chip1_ok ? "1✓" : "1✗",
                s_radar.chip2_ok ? " 2✓" : " 2✗",
                (unsigned long)total_frames,
                (unsigned long)total_irq,
                (unsigned long)total_errors,
                RADAR_IRQ_CHIP1,
                pin1 == HIGH ? "HIGH" : pin1 == LOW ? "LOW!" : "N/A",
                RADAR_IRQ_CHIP2,
                pin2 == HIGH ? "HIGH" : pin2 == LOW ? "LOW!" : "N/A");

            for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
                const RadarSensorStatus& s = s_radar.ch[i].status;
                if (!s.active) {
                    RADI("  [%-8s]: NEAKTIVEN", names[i]);
                    continue;
                }
                const RadarFrame& f = s.last_frame;
                const char* det =
                    f.detection == 0 ? "---  " :
                    f.detection == 1 ? "MOV  " :
                    f.detection == 2 ? "STA  " :
                    f.detection == 3 ? "BOTH " : "???  ";
                uint32_t ago = s.last_frame_ms > 0
                    ? (now - s.last_frame_ms) / 1000 : 9999;
                RADI("  [%-8s]: %s mov=%3dcm sta=%3dcm dist=%3dcm "
                     "frames=%lu err=%lu zadnji=%lus",
                    names[i], det,
                    f.moving_dist_cm,
                    f.static_dist_cm,
                    f.detect_dist_cm,
                    (unsigned long)s.frames_ok,
                    (unsigned long)(s.parse_errors + s.i2c_errors),
                    (unsigned long)ago);
            }

            // Opozorilo če IRQ=0 po dovolj časa
            if (total_irq == 0 && now > 10000) {
                RADW("!! IRQ=0 po %lus — možni vzroki:", (unsigned long)(now/1000));
                RADW("!!   1. SC16IS752 IER ni nastavljen (preveriti init sekvenco)");
                RADW("!!   2. IRQ pin ni v LOW — preveriti hardware (polling aktiven)");
                RADW("!!   3. Hardware: IRQ pin ni povezan ali pull-up manjka");
                RADW("!!   4. LD2410C ne pošilja (preveri 5V napajanje)");
                SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
                if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    for (uint8_t ci = 0; ci < 2; ci++) {
                        uint8_t addr    = (ci == 0) ? SC16_ADDR_1 : SC16_ADDR_2;
                        bool    chip_ok = (ci == 0) ? s_radar.chip1_ok : s_radar.chip2_ok;
                        if (!chip_ok) continue;
                        for (uint8_t ch = 0; ch < 2; ch++) {
                            uint8_t ier = 0, lsr = 0, rxlvl = 0;
                            sc16_read(addr, SC16_REG(REG_IER,   ch), ier);
                            sc16_read(addr, SC16_REG(REG_LSR,   ch), lsr);
                            sc16_read(addr, SC16_REG(REG_RXLVL, ch), rxlvl);
                            RADW("!!   SC16[0x%02X] ch%c: IER=0x%02X LSR=0x%02X RXLVL=%d",
                                addr, ch==0?'A':'B', ier, lsr, rxlvl);
                        }
                    }
                    xSemaphoreGive(mtx);
                }
            }
        }
    }
}

// ============================================================
// JAVNE FUNKCIJE
// ============================================================

bool hal_radar_init(RadarFrameCallback cb) {
    RADI("=== hal_radar_init ===");

    memset(&s_radar, 0, sizeof(s_radar));
    s_radar.callback = cb;

    // Nastavi kanal konfiguracije
    s_radar.ch[0] = { SC16_ADDR_1, UART_CH_A, RADAR_SENSOR_VHOD    };
    s_radar.ch[1] = { SC16_ADDR_1, UART_CH_B, RADAR_SENSOR_CESTA_L };
    s_radar.ch[2] = { SC16_ADDR_2, UART_CH_A, RADAR_SENSOR_CESTA_D };
    s_radar.ch[3] = { SC16_ADDR_2, UART_CH_B, RADAR_SENSOR_GARAZA  };

    // IRQ queue
    s_radar.irq_queue = xQueueCreate(RADAR_IRQ_QUEUE_SIZE, sizeof(uint8_t));
    if (!s_radar.irq_queue) {
        RADE("hal_radar_init: IRQ queue kreacija napaka");
        return false;
    }

    // IRQ pini — INPUT_PULLUP, FALLING edge trigger
    // Datasheet: IRQ je open-drain, active LOW → FALLING = interrupt pending
    // Zunanji pull-up 1kΩ (za 3.3V) je na CJMCU modulu že vgrajen
    // ESP32 INPUT_PULLUP (~45kΩ) je redundanten a ne škodi
    pinMode(RADAR_IRQ_CHIP1, INPUT_PULLUP);
    pinMode(RADAR_IRQ_CHIP2, INPUT_PULLUP);

    // Pridobi Wire1 mutex za inicializacijo
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        RADE("hal_radar_init: Wire1 mutex timeout");
        return false;
    }

    // Ping obeh čipov
    s_radar.chip1_ok = sc16_ping(SC16_ADDR_1);
    s_radar.chip2_ok = sc16_ping(SC16_ADDR_2);

    RADI("SC16IS752 #1 (0x48): %s", s_radar.chip1_ok ? "NAJDEN ✓" : "MANJKA ✗");
    RADI("SC16IS752 #2 (0x4C): %s", s_radar.chip2_ok ? "NAJDEN ✓" : "MANJKA ✗");

    if (!s_radar.chip1_ok && !s_radar.chip2_ok) {
        xSemaphoreGive(mtx);
        RADE("hal_radar_init: nobeden SC16IS752 ne odgovarja — abort");
        return false;
    }

    // Inicializiraj kanale za prisotne čipe
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        RadarChannel& rc = s_radar.ch[i];
        bool chip_ok = (i < 2) ? s_radar.chip1_ok : s_radar.chip2_ok;
        if (!chip_ok) {
            RADW("kanal [%d] preskočen — čip manjka", i);
            continue;
        }

        bool ok = sc16_init_channel(rc.chip_addr, rc.uart_ch);
        rc.status.active = ok;
        RADI("kanal [%d] init: %s", i, ok ? "OK ✓" : "NAPAKA ✗");
    }

    xSemaphoreGive(mtx);

    // attachInterrupt se izvede ZNOTRAJ radarTask (ne tukaj).
    // Razlog: attachInterrupt iz PSRAM init taska ni zanesljiv
    // na ESP32-S3 Arduino. radarTask ga kliče po zagonu schedulerja.

    // Ustvari radarTask
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
    detachInterrupt(digitalPinToInterrupt(RADAR_IRQ_CHIP1));
    detachInterrupt(digitalPinToInterrupt(RADAR_IRQ_CHIP2));
    if (s_radar.task_handle) {
        vTaskDelete(s_radar.task_handle);
        s_radar.task_handle = nullptr;
    }
    if (s_radar.irq_queue) {
        vQueueDelete(s_radar.irq_queue);
        s_radar.irq_queue = nullptr;
    }
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
    // Datasheet Section 7.4 + Table 26 (IOControl register):
    // IOControl[2] = 1 → software reset — enakovreden HW reset
    // Registri DLL/DLH/SPR/XONx/XOFFx se NE resetirajo
    // Vse ostalo se resetira na vrednosti iz Table 4
    SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;

    sc16_write(chip_addr, SC16_REG_GLOBAL(REG_IOCONTROL), IOCONTROL_SW_RESET);
    delay(5); // počakaj na reset

    // Po resetu reinicializiraj kanale za ta čip
    uint8_t base = (chip_addr == SC16_ADDR_1) ? 0 : 2;
    for (uint8_t i = base; i < base + 2; i++) {
        s_radar.ch[i].status.active = sc16_init_channel(chip_addr, s_radar.ch[i].uart_ch);
    }

    xSemaphoreGive(mtx);
    RADW("hal_radar_reset_chip 0x%02X OK", chip_addr);
}

void hal_radar_log_stats() {
    const char* names[4] = {"Vhod", "Cesta_L", "Cesta_D", "Garaza"};
    RADI("=== Radar statistika ===");
    for (uint8_t i = 0; i < RADAR_SENSOR_COUNT; i++) {
        const RadarSensorStatus& s = s_radar.ch[i].status;
        if (!s.active) {
            RADI("  [%-8s]: NEAKTIVEN", names[i]);
            continue;
        }
        uint32_t ago = (millis() - s.last_frame_ms) / 1000;
        RADI("  [%-8s]: ok=%-5lu err=%-3lu parse_err=%-3lu i2c_err=%-3lu irq=%-5lu zadnji=%lus",
            names[i],
            (unsigned long)s.frames_ok,
            (unsigned long)s.frames_err,
            (unsigned long)(s.parse_errors - s.boot_parse_errors),
            (unsigned long)s.i2c_errors,
            (unsigned long)s.irq_count,
            (unsigned long)ago);
    }
}

void hal_radar_recovery_check() {
    // ----------------------------------------------------------------
    // RADAR RECOVERY CHECK — kliči iz sensorTask, ne iz radarTask.
    // Namen: počisti IRQ pin če je ostal LOW po zamujeni ISR sekvenci.
    //
    // Kliče se vsakih TOF_WATCHDOG_INTERVAL_MS (10 minut) iz sensorTask,
    // takoj po TOF watchdog meritvi. Wire1 je v tistem trenutku že
    // "v uporabi" za TOF — dodaten overhead je zanemarljiv.
    //
    // Zakaj NE v radarTask:
    //   radarTask je IRQ-driven — čaka na queue in procesira takoj.
    //   Dodaten periodični recovery v radarTask povzroča mutex konflikte
    //   z lastno ISR → queue → drain sekvenco.
    //
    // Zakaj je IRQ pin LOW legitimna napaka samo po 10 minutah:
    //   Med normalnim delovanjem IRQ gre LOW → ISR → queue → drain → HIGH.
    //   Ta sekvenca traja <5ms. Če pin ostane LOW 10 minut brez novih
    //   frames v logu — je to dejanska napaka (zamrznjen IIR, I2C napaka).
    //
    // TODO: primerjaj s frames_ok counter — če frames rastejo normalno,
    //   pin LOW ni napaka in recovery preskoči. Implementirati ko bo
    //   EventBus integriran in bomo imeli dostop do frame statistike.
    // ----------------------------------------------------------------

    if (!s_radar.initialized) return;

    for (uint8_t ci = 0; ci < 2; ci++) {
        bool chip_ok = (ci == 0) ? s_radar.chip1_ok : s_radar.chip2_ok;
        int  irq_pin = (ci == 0) ? RADAR_IRQ_CHIP1  : RADAR_IRQ_CHIP2;
        if (!chip_ok) continue;

        if (digitalRead(irq_pin) == LOW) {
            RADW("Recovery check (10min): IO%d LOW — ročno čiščenje chip%d",
                 irq_pin, ci + 1);
            SemaphoreHandle_t mtx = bsp_get_wire1_mutex();
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
                process_chip_irq(ci);
                xSemaphoreGive(mtx);
            } else {
                RADW("Recovery check: Wire1 mutex timeout za chip%d", ci + 1);
            }
        } else {
            RADD("Recovery check (10min): IO%d HIGH — chip%d OK", irq_pin, ci + 1);
        }
    }
}
