// ============================================================
// hal_radar_test.h — SC16IS752 Hardware Test Interface
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0-test  |  Datum: 2026-04
// ============================================================
//
// NAMEN:
//   Standalone hardware test za SC16IS752 I2C→UART bridge čipe.
//   Testira oba čipa (#1 @ 0x48 in #2 @ 0x4C) in oba UART kanala
//   na vsakem čipu — skupaj 4 LD2410C radar kanale.
//
//   Če čip manjka → javi napako in nadaljuje z ostalimi.
//   Če čip najde → inicializira in bere raw podatke.
//
// UPORABA:
//   1. V sensor_mgr.cpp odkomentiraj:
//        #include "hal_radar_test.h"
//      in v sensor_mgr_init():
//        hal_radar_test_run();
//
//   2. Odpri Serial Monitor @ 115200 baud
//
//   3. Rezultat: vidno katere naprave odgovarjajo in ali
//      LD2410C pošilja veljavne frame-e
//
// KAJ TEST DELA:
//   - Wire1 init (IO17/IO18, 100kHz)
//   - I2C scan celotnega naslovnega prostora (0x00–0x7F)
//   - SC16IS752 ping za vsak čip posebej
//   - SC16IS752 UART init (XTAL=1.8432MHz, baud=115200, div=1, 8N1)
//   - IRQ pin stanje (IO41, IO42) — HIGH=idle, LOW=data ready
//   - Branje raw bajtov iz FIFO (do 3 sekunde na kanal)
//   - LD2410C frame parser — poišče FD FC FB FA header
//   - Izpis vsakega veljavnega frame-a (detection, razdalje, energije)
//   - Statistika: bytes, frames, errors, timeouts
//
// KAJ TEST NE DELA:
//   - Nobenega FreeRTOS taska — teče sinhrono v init fazi
//   - Nobenih callbackov ali EventBus integracij
//   - Nobenih sprememb v obstoječi arhitekturi
//   - Nobene integracije v sensor_mgr zanko
//
// PRIČAKOVANI SERIAL IZHOD (ko sta oba čipa prisotna):
//   [RT] ====================================
//   [RT] SC16IS752 Hardware Test v1.0
//   [RT] ====================================
//   [RT] Wire1 init (SDA=IO17 SCL=IO18)... OK
//   [RT] --- I2C Scan ---
//   [RT]   0x48 NAJDEN (SC16IS752 #1)
//   [RT]   0x4C NAJDEN (SC16IS752 #2)
//   [RT]   Skupaj naprav: 2
//   [RT] --- SC16IS752 #1 @ 0x48 ---
//   [RT]   UART-A init (div=1, 115200, 8N1)... OK
//   [RT]   UART-B init (div=1, 115200, 8N1)... OK
//   [RT]   IRQ pin IO41: HIGH (idle)
//   [RT]   Branje UART-A [Vhod]... 13B → frame OK
//   [RT]     det=CLEAR mov=0cm(0%) sta=0cm(0%) dist=0cm
//   [RT]   Branje UART-B [Cesta_L]... 13B → frame OK
//   [RT] --- SC16IS752 #2 @ 0x4C ---
//   [RT]   ...
//   [RT] === POVZETEK ===
//   [RT]   SC16IS752 #1: OK | UART-A: OK | UART-B: OK
//   [RT]   SC16IS752 #2: OK | UART-A: OK | UART-B: OK
//
// PRIČAKOVANI IZHOD (samo #2 priključen):
//   [RT]   0x48 NI NAJDEN (SC16IS752 #1) — priklopi ga kasneje
//   [RT]   0x4C NAJDEN (SC16IS752 #2)
//   [RT] --- SC16IS752 #1 @ 0x48 ---
//   [RT]   PRESKOČEN — čip ne odgovarja na I2C
//   [RT] --- SC16IS752 #2 @ 0x4C ---
//   [RT]   ...
//
// ============================================================

#pragma once
#include <Arduino.h>

// ============================================================
// JAVNA FUNKCIJA
// ============================================================

// Zaženi celoten hardware test.
// Kliči iz sensor_mgr_init() ali direktno iz setup().
// Blokira ~15 sekund (timeout branje iz vseh kanalov).
// Rezultati gredo na Serial in Logger.
void hal_radar_test_run();
