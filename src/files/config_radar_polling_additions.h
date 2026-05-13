// ============================================================
// config.h — DOPOLNITEV za hal_radar v2.0 (polling arhitektura)
// ============================================================
//
// NAVODILO ZA AGENTA: Te konstante dodaj v obstoječi config.h.
// Poišči sekcijo z RADAR_ konstantami in jih vstavi za obstoječe.
//
// ============================================================

// ============================================================
// RADAR POLLING PARAMETRI (novo v v2.0)
// ============================================================

// Privzeti polling interval v ms.
// Nastavljiv prek web UI (Config → Radar) in shranjen v NVS.
// Vrednost se naloži ob zagonu radarTask iz config_get().
// Obseg: RADAR_POLL_INTERVAL_MIN_MS do RADAR_POLL_INTERVAL_MAX_MS.
// Pri 50ms: polling 20×/s, LD2410C pošilja 10×/s → vsak frame ujet.
#define RADAR_POLL_INTERVAL_MS_DEFAULT   50u

// Absolutne meje (enforcirane v radarTask in config_mgr validaciji)
#define RADAR_POLL_INTERVAL_MIN_MS       10u
#define RADAR_POLL_INTERVAL_MAX_MS      100u

// Prag zaporednih OE! overflowov pred WARN logom.
// Ob vsakem OE! se consec_overflows++ (reset ob uspešnem LSR branju).
// Ko presežemo ta prag → WARN "povečaj poll interval".
// Default 10: pri 50ms polling = 500ms neprekinjenega overflowa
// preden se sproži WARN. Posamezni overflow je normalen in se ne logira.
#define RADAR_MAX_CONSECUTIVE_OVERFLOWS  10u

// ============================================================
// OBSTOJEČE KONSTANTE — OHRANJENE IZ v1.x
// (navedene tukaj za referenco, ne podvajaj v config.h)
// ============================================================

// #define RADAR_TASK_STACK          8192   // stack radarTask v bajtih
// #define RADAR_OE_LOG_INTERVAL_MS  5000   // throttle za OE! log (ms)
// #define RADAR_PUBLISH_INTERVAL_MS  100   // throttle za callback (ms)
// #define RADAR_DRAIN_MAX_LOOPS        4   // OBSOLETNO v v2.0 — lahko odstranimo
