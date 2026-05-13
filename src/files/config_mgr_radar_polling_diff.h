// ============================================================
// config_mgr — DOPOLNITEV za hal_radar v2.0 (polling arhitektura)
// ============================================================
//
// NAVODILO ZA AGENTA: Te dopolnitve vstavi v obstoječe datoteke.
// Celotna sekcija "RADAR parametri" v config_mgr.cpp se razširi
// z dvema novima parametroma. Config struktura dobi eno novo polje.
//
// ============================================================

// ============================================================
// 1. DOPOLNITEV: config_mgr.h → struct Config
// ============================================================
//
// Dodaj PRED "radar_persistence_n" ali za "radar_unmanned_s[4]":
//
//     // Polling interval radarTask v ms (nastavljivo prek web UI).
//     // Default: RADAR_POLL_INTERVAL_MS_DEFAULT (50ms).
//     // Obseg: RADAR_POLL_INTERVAL_MIN_MS (10) – RADAR_POLL_INTERVAL_MAX_MS (100).
//     uint32_t radar_poll_interval_ms;   // default: 50, min: 10, max: 100
//
//     // Prag zaporednih overflowov pred WARN logom (nastavljivo prek web UI).
//     // Default: RADAR_MAX_CONSECUTIVE_OVERFLOWS (10).
//     uint32_t radar_max_consec_overflows;  // default: 10, min: 1, max: 100

// ============================================================
// 2. DOPOLNITEV: config_mgr.h → config_defaults()
// ============================================================
//
// V inline Config config_defaults() dodaj za "c.radar_persistence_n = 3;":
//
//     c.radar_poll_interval_ms      = RADAR_POLL_INTERVAL_MS_DEFAULT;  // 50
//     c.radar_max_consec_overflows  = RADAR_MAX_CONSECUTIVE_OVERFLOWS;  // 10

// ============================================================
// 3. DOPOLNITEV: config_mgr.cpp → NVS ključi (sekcija #define)
// ============================================================
//
// Dodaj za "#define NVS_K_RADAR_PERSIST  "r_persist"":
//
//     #define NVS_K_RADAR_POLL_IV   "r_poll_iv"    // radar_poll_interval_ms
//     #define NVS_K_RADAR_MAX_OVF   "r_max_ovf"    // radar_max_consec_overflows

// ============================================================
// 4. DOPOLNITEV: config_mgr.cpp → MEJNE VREDNOSTI (sekcija #define)
// ============================================================
//
// Dodaj za obstoječe CFG_MIN/MAX_RADAR_* definicije:
//
//     #define CFG_MIN_RADAR_POLL_IV    10u
//     #define CFG_MAX_RADAR_POLL_IV   100u
//     #define CFG_MIN_RADAR_MAX_OVF    1u
//     #define CFG_MAX_RADAR_MAX_OVF  100u

// ============================================================
// 5. DOPOLNITEV: config_mgr.cpp → load_and_validate()
// ============================================================
//
// V funkciji load_and_validate(), sekcija "--- Radar parametri ---",
// dodaj ZA blokom "for (int ri = 0; ri < 4; ri++)" in ZA vrstico
// za radar_persistence_n:
//
//     VALIDATE_U32(prefs, NVS_K_RADAR_POLL_IV,
//                  radar_poll_interval_ms,
//                  CFG_MIN_RADAR_POLL_IV, CFG_MAX_RADAR_POLL_IV,
//                  def.radar_poll_interval_ms);
//
//     VALIDATE_U32(prefs, NVS_K_RADAR_MAX_OVF,
//                  radar_max_consec_overflows,
//                  CFG_MIN_RADAR_MAX_OVF, CFG_MAX_RADAR_MAX_OVF,
//                  def.radar_max_consec_overflows);

// ============================================================
// 6. DOPOLNITEV: config_mgr.cpp → write_all_to_nvs()
// ============================================================
//
// V funkciji write_all_to_nvs(), sekcija "// Radar parametri",
// dodaj ZA "prefs.putUChar(NVS_K_RADAR_PERSIST, ...)":
//
//     prefs.putUInt(NVS_K_RADAR_POLL_IV,  s_config.radar_poll_interval_ms);
//     prefs.putUInt(NVS_K_RADAR_MAX_OVF,  s_config.radar_max_consec_overflows);

// ============================================================
// 7. DOPOLNITEV: web UI — nastavitve Radar zavihek
// ============================================================
//
// Na web UI strani Nastavitve → Radar dodaj dva nova parametra:
//
//   Polling interval (ms):
//     Input type: number, min=10, max=100, step=5
//     NVS ključ: radar_poll_interval_ms
//     Opis: "Interval branja radar senzorjev. Manjši = hitrejši odziv,
//            večji = manj obremenitve. Default: 50ms."
//
//   Max zaporedni overflowi:
//     Input type: number, min=1, max=100, step=1
//     NVS ključ: radar_max_consec_overflows
//     Opis: "Število zaporednih FIFO overflowov pred opozorilom v logu.
//            Default: 10."
//
// Oba parametra se shranita prek obstoječega POST /api/config endpointa
// — ni novega endpointa potreben.
