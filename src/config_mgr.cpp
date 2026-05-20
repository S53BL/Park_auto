// ============================================================
// config_mgr.cpp — Config Manager: NVS persistenca nastavitev
// Projekt : Avtomatizacija Pokritega Parkirišča
// Verzija : 1.0.0  |  Datum: 2026-05
// ============================================================
//
// IMPLEMENTACIJSKE ODLOČITVE:
//
//   NVS Preferences namespace "parking":
//     Vsak parameter ima unikaten ključ (max 15 znakov).
//     Preferences knjižnica je vgrajena v ESP32 Arduino — ni v lib_deps.
//
//   Validacija ob vsakem zagonu:
//     Za vsak parameter: preberi iz NVS → preveri razpon →
//     če izven meja: vzemi default, zapiši v NVS, inkrementiraj
//     s_replaced_count. Boolovi: veljavni sta samo 0 in 1.
//
//   Float vrednosti (dtw_threshold, stability_s):
//     NVS ne podpira float direktno. Shranjujemo kot uint32_t
//     z bitcast (memcpy). Validacija po pretvorbi nazaj v float.
//     NaN in Inf sta vedno neveljavna.
//
//   Thread safety:
//     s_mutex (FreeRTOS mutex) varuje s_config in vse NVS operacije.
//     config_get() vrne const ref — caller ne sme hraniti ref-a
//     čez daljše operacije (mutex se sprosti po klicu).
//     Pravilna uporaba: const Config cfg = config_get(); (kopija na stack)
//
//   Degraded mode:
//     Če NVS ne uspe odpreti (redko, hardware napaka), sistem dela
//     samo z RAM defaulti. config_mgr_ok() vrne false.
//     Logger zabeleži ERROR, sistem ne sesuje.
//
// LOGGING:
//   INFO  : init OK, koliko vrednosti zamenjanih
//   WARN  : posamezna vrednost izven meja (katera in kakšna je bila)
//   ERROR : NVS ne uspe odpreti
//   DEBUG : vsaka prebrana vrednost (samo ob DEBUG log nivoju)
//
// ============================================================

#include "config_mgr.h"
#include "logger.h"
#include <Preferences.h>
#include <freertos/semphr.h>
#include <cstring>   // memcpy
#include <cmath>     // isnan, isinf

// ============================================================
// LOGGING MAKROJI
// ============================================================

#define CFGI(fmt, ...) LOG_INFO ("CFGMGR", fmt, ##__VA_ARGS__)
#define CFGW(fmt, ...) LOG_WARN ("CFGMGR", fmt, ##__VA_ARGS__)
#define CFGE(fmt, ...) LOG_ERROR("CFGMGR", fmt, ##__VA_ARGS__)
#define CFGD(fmt, ...) LOG_DEBUG("CFGMGR", fmt, ##__VA_ARGS__)

// ============================================================
// NVS KLJUČI (max 15 znakov — NVS omejitev)
// ============================================================

#define NVS_NS              "parking"       // namespace

// Osvetlitev
#define NVS_K_TIMEOUT_SSR1  "to_ssr1"       // timeout_ssr1_s
#define NVS_K_MAN_EXT       "man_ext"       // manual_extend_min
#define NVS_K_AF_SSR2       "af_ssr2"       // antiforgot_ssr2_min
#define NVS_K_AF_SSR3       "af_ssr3"       // antiforgot_ssr3_min
#define NVS_K_SSR2_NIGHT    "ssr2_night"    // ssr2_auto_night
#define NVS_K_DND_START     "dnd_start"     // dnd_start_h
#define NVS_K_DND_END       "dnd_end"       // dnd_end_h
#define NVS_K_SSR2_DND_DIS  "ssr2_dnd_dis"  // ssr2_dnd_disable
#define NVS_K_LUX_NIGHT     "lux_night"     // lux_night
#define NVS_K_LUX_DAY       "lux_day"       // lux_day
#define NVS_K_BRIGHT_NIGHT  "br_night"      // brightness_night

// LED animacije
#define NVS_K_FILL_SPD      "fill_spd"      // fill_speed_ms
#define NVS_K_UNFILL_SPD    "unfill_spd"    // unfill_speed_ms
#define NVS_K_FADE_DUR      "fade_dur"      // fade_duration_ms
#define NVS_K_TGT_BRIGHT    "tgt_bright"    // target_brightness
#define NVS_K_SSR2_DELAY    "ssr2_delay"    // ssr2_delay_ms
#define NVS_K_PA_GREEN      "pa_green"      // pa_thresh_green_mm
#define NVS_K_PA_ORANGE     "pa_orange"     // pa_thresh_orange_mm
#define NVS_K_PA_RED        "pa_red"        // pa_thresh_red_mm
#define NVS_K_PA_STAB       "pa_stab"       // pa_stability_s
#define NVS_K_CLOCK_DUR     "clock_dur"     // clock_duration_s
#define NVS_K_CELL_TMR      "cell_tmr"      // photocell_timer_min

// Identifikacija
#define NVS_K_DTW_THRESH    "dtw_thresh"    // dtw_threshold (float as uint32)
#define NVS_K_SAKOE_RAD     "sakoe_rad"     // sakoe_radius
#define NVS_K_MIN_PROF      "min_prof"      // min_profile_points
#define NVS_K_NORM_PTS      "norm_pts"      // normalize_points
#define NVS_K_DELTA_FLT     "delta_flt"     // delta_filter_mm
#define NVS_K_PHASE_CFM     "phase_cfm"     // phase_confirm_cm
#define NVS_K_STAB_S        "stab_s"        // stability_s (float as uint32)
#define NVS_K_RAW_N         "vr_raw_n"      // raw_profiles_per_model
#define NVS_K_PRES_MIN      "vr_pres_min"   // presence_check_min
#define NVS_K_EMPTY_TOL     "vr_empty_tol"  // empty_tolerance_mm

// Radar — per senzor (indeks 0-3 v imenu ključa)
// "r" = radar, "md" = max_dist, "ms" = move_sens,
// "ss" = static_sens, "us" = unmanned_s
static const char* NVS_RADAR_MD[] = {"r_md_0","r_md_1","r_md_2","r_md_3"};
static const char* NVS_RADAR_MS[] = {"r_ms_0","r_ms_1","r_ms_2","r_ms_3"};
static const char* NVS_RADAR_SS[] = {"r_ss_0","r_ss_1","r_ss_2","r_ss_3"};
static const char* NVS_RADAR_US[] = {"r_us_0","r_us_1","r_us_2","r_us_3"};
#define NVS_K_RADAR_PERSIST  "r_persist"
#define NVS_K_RADAR_POLL_IV  "r_poll_iv"    // radar_poll_interval_ms
#define NVS_K_RADAR_MAX_OVF  "r_max_ovf"    // radar_max_consec_overflows

// Party / WLED
#define NVS_K_WLED_IP        "wled_ip"      // wled_ip (string)
#define NVS_K_PARTY_RESUME   "party_resume_s" // party_resume_delay_s

// Party slots — "pslot_0".."pslot_8" (7 znakov < 15 NVS limit), blob
static const char* NVS_PSLOT[] = {
    "pslot_0","pslot_1","pslot_2","pslot_3","pslot_4",
    "pslot_5","pslot_6","pslot_7","pslot_8"
};

// Party schedules — "psched_0".."psched_9" (8 znakov < 15 NVS limit), blob
static const char* NVS_PSCHED[] = {
    "psched_0","psched_1","psched_2","psched_3","psched_4",
    "psched_5","psched_6","psched_7","psched_8","psched_9"
};

// SSR labeli — "ssr_lbl_0".."ssr_lbl_3" (9 znakov < 15 NVS limit)
static const char* NVS_SSR_LBL[] = {"ssr_lbl_0","ssr_lbl_1","ssr_lbl_2","ssr_lbl_3"};

// ============================================================
// MEJNE VREDNOSTI ZA VALIDACIJO
// ============================================================

// Osvetlitev
#define CFG_MIN_TIMEOUT_SSR1    30u
#define CFG_MAX_TIMEOUT_SSR1    3600u
#define CFG_MIN_MAN_EXT         5u
#define CFG_MAX_MAN_EXT         60u
#define CFG_MIN_AF_SSR          1u
#define CFG_MAX_AF_SSR          60u
#define CFG_MIN_LUX_NIGHT       1u
#define CFG_MAX_LUX_NIGHT       200u
#define CFG_MIN_LUX_DAY         1u
#define CFG_MAX_LUX_DAY         500u
#define CFG_MIN_BRIGHT_NIGHT    10u
#define CFG_MAX_BRIGHT_NIGHT    255u

// LED animacije
#define CFG_MIN_FILL_SPD        500u
#define CFG_MAX_FILL_SPD        30000u
#define CFG_MIN_UNFILL_SPD      500u
#define CFG_MAX_UNFILL_SPD      30000u
#define CFG_MIN_FADE_DUR        100u
#define CFG_MAX_FADE_DUR        5000u
#define CFG_MIN_TGT_BRIGHT      10u
#define CFG_MAX_TGT_BRIGHT      255u
#define CFG_MIN_SSR2_DELAY      0u
#define CFG_MAX_SSR2_DELAY      5000u
#define CFG_MIN_PA_GREEN        200u
#define CFG_MAX_PA_GREEN        4000u
#define CFG_MIN_PA_ORANGE       100u
#define CFG_MAX_PA_ORANGE       3000u
#define CFG_MIN_PA_RED          50u
#define CFG_MAX_PA_RED          2000u
#define CFG_MIN_PA_STAB         1u
#define CFG_MAX_PA_STAB         30u
#define CFG_MIN_CLOCK_DUR       0u
#define CFG_MAX_CLOCK_DUR       60u
#define CFG_MIN_CELL_TMR        1u
#define CFG_MAX_CELL_TMR        30u

// Identifikacija
#define CFG_MIN_DTW_THRESH      1.0f
#define CFG_MAX_DTW_THRESH      100.0f
#define CFG_MIN_SAKOE_RAD       1u
#define CFG_MAX_SAKOE_RAD       40u
#define CFG_MIN_MIN_PROF        10u
#define CFG_MAX_MIN_PROF        80u
#define CFG_MIN_NORM_PTS        20u
#define CFG_MAX_NORM_PTS        80u
#define CFG_MIN_DELTA_FLT       5u
#define CFG_MAX_DELTA_FLT       100u
#define CFG_MIN_PHASE_CFM       50u
#define CFG_MAX_PHASE_CFM       500u
#define CFG_MIN_STAB_S          0.5f
#define CFG_MAX_STAB_S          10.0f
#define CFG_MIN_RAW_N           10u
#define CFG_MAX_RAW_N           100u
#define CFG_MIN_PRES_MIN        1u
#define CFG_MAX_PRES_MIN        60u
#define CFG_MIN_EMPTY_TOL       50u
#define CFG_MAX_EMPTY_TOL       500u

// Radar polling (v2.0)
#define CFG_MIN_RADAR_POLL_IV    10u
#define CFG_MAX_RADAR_POLL_IV   100u
#define CFG_MIN_RADAR_MAX_OVF    1u
#define CFG_MAX_RADAR_MAX_OVF  100u

// Party
#define CFG_MIN_PARTY_RESUME     5u
#define CFG_MAX_PARTY_RESUME   300u

// ============================================================
// INTERNO STANJE
// ============================================================

static Config            s_config;
static SemaphoreHandle_t s_mutex         = nullptr;
static bool              s_initialized   = false;
static bool              s_nvs_ok        = false;
static uint8_t           s_replaced_count = 0;

// Preferences objekt — odprt med init, potem zaprt.
// Za save() ga znova odpremo.
static Preferences       s_prefs;

// ============================================================
// POMOŽNE FUNKCIJE — float ↔ uint32 bitcast
// ============================================================
// NVS (Preferences) ne podpira float direktno.
// Shranjujemo raw bitne vzorce kot uint32_t.

static uint32_t float_to_u32(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float u32_to_float(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static bool float_valid(float f) {
    return !isnan(f) && !isinf(f);
}

// ============================================================
// VALIDACIJSKI MAKRO
// ============================================================
// Uporaba:
//   VALIDATE_U32(prefs, key, field, min, max, default_val)
//   VALIDATE_U8 (prefs, key, field, min, max, default_val)
//   VALIDATE_BOOL(prefs, key, field, default_val)
//   VALIDATE_FLOAT(prefs, key, field, min, max, default_val)
//
// Vsak makro:
//   1. Prebere vrednost iz NVS (privzeta vrednost = UINT32_MAX/0xFF/2 → vedno izven meja)
//   2. Preveri razpon
//   3. Če je ok → zapiše v config field
//   4. Če ni ok → vzame default → zapiše v config field → zapiše v NVS → inkrement s_replaced_count

#define VALIDATE_U32(prefs, key, field, vmin, vmax, defval)                 \
    do {                                                                     \
        uint32_t _v = (prefs).getUInt((key), UINT32_MAX);                   \
        if (_v >= (vmin) && _v <= (vmax)) {                                  \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_v != UINT32_MAX) {                                          \
                CFGW("  " key " = %lu izven meja [%lu,%lu] → default %lu",  \
                     (unsigned long)_v, (unsigned long)(vmin),               \
                     (unsigned long)(vmax), (unsigned long)(defval));        \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUInt((key), (defval));                                \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

#define VALIDATE_U8(prefs, key, field, vmin, vmax, defval)                  \
    do {                                                                     \
        uint8_t _v = (prefs).getUChar((key), 0xFF);                         \
        if (_v >= (vmin) && _v <= (vmax)) {                                  \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_v != 0xFF) {                                                \
                CFGW("  " key " = %u izven meja [%u,%u] → default %u",      \
                     (unsigned)_v, (unsigned)(vmin),                         \
                     (unsigned)(vmax), (unsigned)(defval));                  \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUChar((key), (defval));                               \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

#define VALIDATE_BOOL(prefs, key, field, defval)                            \
    do {                                                                     \
        uint8_t _v = (prefs).getUChar((key), 0xFF);                         \
        if (_v == 0 || _v == 1) {                                            \
            s_config.field = (_v == 1);                                      \
        } else {                                                             \
            if (_v != 0xFF) {                                                \
                CFGW("  " key " = %u neveljavna bool vrednost → default %d", \
                     (unsigned)_v, (int)(defval));                           \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUChar((key), (defval) ? 1u : 0u);                    \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

#define VALIDATE_FLOAT(prefs, key, field, vmin, vmax, defval)               \
    do {                                                                     \
        uint32_t _raw = (prefs).getUInt((key), UINT32_MAX);                 \
        float    _v   = (_raw == UINT32_MAX) ? NAN : u32_to_float(_raw);    \
        if (float_valid(_v) && _v >= (vmin) && _v <= (vmax)) {              \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_raw != UINT32_MAX && float_valid(_v)) {                     \
                CFGW("  " key " = %.3f izven meja [%.1f,%.1f] → default %.3f",\
                     _v, (float)(vmin), (float)(vmax), (float)(defval));    \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUInt((key), float_to_u32(defval));                   \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

// ============================================================
// INTERNO: load_and_validate
// ============================================================
// Prebere vse vrednosti iz NVS in validira.
// Prefs mora biti že odprt (read-write) pred klicem.

static void load_and_validate(Preferences& prefs) {
    s_replaced_count = 0;

    const Config def = config_defaults();

    // --- Osvetlitev ---
    VALIDATE_U32 (prefs, NVS_K_TIMEOUT_SSR1, timeout_ssr1_s,      CFG_MIN_TIMEOUT_SSR1, CFG_MAX_TIMEOUT_SSR1, def.timeout_ssr1_s);
    VALIDATE_U32 (prefs, NVS_K_MAN_EXT,      manual_extend_min,   CFG_MIN_MAN_EXT,      CFG_MAX_MAN_EXT,      def.manual_extend_min);
    VALIDATE_U32 (prefs, NVS_K_AF_SSR2,      antiforgot_ssr2_min, CFG_MIN_AF_SSR,       CFG_MAX_AF_SSR,       def.antiforgot_ssr2_min);
    VALIDATE_U32 (prefs, NVS_K_AF_SSR3,      antiforgot_ssr3_min, CFG_MIN_AF_SSR,       CFG_MAX_AF_SSR,       def.antiforgot_ssr3_min);
    VALIDATE_BOOL(prefs, NVS_K_SSR2_NIGHT,   ssr2_auto_night,                                                 def.ssr2_auto_night);
    VALIDATE_U8  (prefs, NVS_K_DND_START,    dnd_start_h,         0u,                   23u,                  def.dnd_start_h);
    VALIDATE_U8  (prefs, NVS_K_DND_END,      dnd_end_h,           0u,                   23u,                  def.dnd_end_h);
    VALIDATE_BOOL(prefs, NVS_K_SSR2_DND_DIS, ssr2_dnd_disable,                                                def.ssr2_dnd_disable);
    VALIDATE_U32 (prefs, NVS_K_LUX_NIGHT,    lux_night,           CFG_MIN_LUX_NIGHT,    CFG_MAX_LUX_NIGHT,    def.lux_night);
    VALIDATE_U32 (prefs, NVS_K_LUX_DAY,      lux_day,             CFG_MIN_LUX_DAY,      CFG_MAX_LUX_DAY,      def.lux_day);
    VALIDATE_U8  (prefs, NVS_K_BRIGHT_NIGHT, brightness_night,    CFG_MIN_BRIGHT_NIGHT, CFG_MAX_BRIGHT_NIGHT, def.brightness_night);

    // --- LED animacije ---
    VALIDATE_U32 (prefs, NVS_K_FILL_SPD,   fill_speed_ms,       CFG_MIN_FILL_SPD,   CFG_MAX_FILL_SPD,   def.fill_speed_ms);
    VALIDATE_U32 (prefs, NVS_K_UNFILL_SPD, unfill_speed_ms,     CFG_MIN_UNFILL_SPD, CFG_MAX_UNFILL_SPD, def.unfill_speed_ms);
    VALIDATE_U32 (prefs, NVS_K_FADE_DUR,   fade_duration_ms,    CFG_MIN_FADE_DUR,   CFG_MAX_FADE_DUR,   def.fade_duration_ms);
    VALIDATE_U8  (prefs, NVS_K_TGT_BRIGHT, target_brightness,   CFG_MIN_TGT_BRIGHT, CFG_MAX_TGT_BRIGHT, def.target_brightness);
    VALIDATE_U32 (prefs, NVS_K_SSR2_DELAY, ssr2_delay_ms,       CFG_MIN_SSR2_DELAY, CFG_MAX_SSR2_DELAY, def.ssr2_delay_ms);
    VALIDATE_U32 (prefs, NVS_K_PA_GREEN,   pa_thresh_green_mm,  CFG_MIN_PA_GREEN,   CFG_MAX_PA_GREEN,   def.pa_thresh_green_mm);
    VALIDATE_U32 (prefs, NVS_K_PA_ORANGE,  pa_thresh_orange_mm, CFG_MIN_PA_ORANGE,  CFG_MAX_PA_ORANGE,  def.pa_thresh_orange_mm);
    VALIDATE_U32 (prefs, NVS_K_PA_RED,     pa_thresh_red_mm,    CFG_MIN_PA_RED,     CFG_MAX_PA_RED,     def.pa_thresh_red_mm);
    VALIDATE_U32 (prefs, NVS_K_PA_STAB,    pa_stability_s,      CFG_MIN_PA_STAB,    CFG_MAX_PA_STAB,    def.pa_stability_s);
    VALIDATE_U32 (prefs, NVS_K_CLOCK_DUR,  clock_duration_s,    CFG_MIN_CLOCK_DUR,  CFG_MAX_CLOCK_DUR,  def.clock_duration_s);
    VALIDATE_U32 (prefs, NVS_K_CELL_TMR,   photocell_timer_min, CFG_MIN_CELL_TMR,   CFG_MAX_CELL_TMR,   def.photocell_timer_min);

    // --- Identifikacija ---
    VALIDATE_FLOAT(prefs, NVS_K_DTW_THRESH, dtw_threshold,      CFG_MIN_DTW_THRESH, CFG_MAX_DTW_THRESH, def.dtw_threshold);
    VALIDATE_U8   (prefs, NVS_K_SAKOE_RAD,  sakoe_radius,       CFG_MIN_SAKOE_RAD,  CFG_MAX_SAKOE_RAD,  def.sakoe_radius);
    VALIDATE_U8   (prefs, NVS_K_MIN_PROF,   min_profile_points, CFG_MIN_MIN_PROF,   CFG_MAX_MIN_PROF,   def.min_profile_points);
    VALIDATE_U8   (prefs, NVS_K_NORM_PTS,   normalize_points,   CFG_MIN_NORM_PTS,   CFG_MAX_NORM_PTS,   def.normalize_points);
    VALIDATE_U32  (prefs, NVS_K_DELTA_FLT,  delta_filter_mm,    CFG_MIN_DELTA_FLT,  CFG_MAX_DELTA_FLT,  def.delta_filter_mm);
    VALIDATE_U32  (prefs, NVS_K_PHASE_CFM,  phase_confirm_cm,   CFG_MIN_PHASE_CFM,  CFG_MAX_PHASE_CFM,  def.phase_confirm_cm);
    VALIDATE_FLOAT(prefs, NVS_K_STAB_S,     stability_s,        CFG_MIN_STAB_S,     CFG_MAX_STAB_S,     def.stability_s);
    VALIDATE_U8   (prefs, NVS_K_RAW_N,     raw_profiles_per_model, CFG_MIN_RAW_N,  CFG_MAX_RAW_N,      def.raw_profiles_per_model);
    VALIDATE_U8   (prefs, NVS_K_PRES_MIN,  presence_check_min, CFG_MIN_PRES_MIN,   CFG_MAX_PRES_MIN,   def.presence_check_min);
    VALIDATE_U32  (prefs, NVS_K_EMPTY_TOL, empty_tolerance_mm, CFG_MIN_EMPTY_TOL,  CFG_MAX_EMPTY_TOL,  def.empty_tolerance_mm);

    // --- Radar parametri --- per senzor
    for (int ri = 0; ri < 4; ri++) {
        uint8_t md = prefs.getUChar(NVS_RADAR_MD[ri], 0xFF);
        if (md <= 8) {
            s_config.radar_max_dist[ri] = md;
        } else {
            s_config.radar_max_dist[ri] = def.radar_max_dist[ri];
            prefs.putUChar(NVS_RADAR_MD[ri], def.radar_max_dist[ri]);
            s_replaced_count++;
        }
        uint8_t ms = prefs.getUChar(NVS_RADAR_MS[ri], 0xFF);
        if (ms <= 100) {
            s_config.radar_move_sens[ri] = ms;
        } else {
            s_config.radar_move_sens[ri] = def.radar_move_sens[ri];
            prefs.putUChar(NVS_RADAR_MS[ri], def.radar_move_sens[ri]);
            s_replaced_count++;
        }
        uint8_t ss = prefs.getUChar(NVS_RADAR_SS[ri], 0xFF);
        if (ss <= 100) {
            s_config.radar_static_sens[ri] = ss;
        } else {
            s_config.radar_static_sens[ri] = def.radar_static_sens[ri];
            prefs.putUChar(NVS_RADAR_SS[ri], def.radar_static_sens[ri]);
            s_replaced_count++;
        }
        uint32_t us = prefs.getUInt(NVS_RADAR_US[ri], 0xFFFFFFFF);
        if (us <= 65535) {
            s_config.radar_unmanned_s[ri] = (uint16_t)us;
        } else {
            s_config.radar_unmanned_s[ri] = def.radar_unmanned_s[ri];
            prefs.putUInt(NVS_RADAR_US[ri], def.radar_unmanned_s[ri]);
            s_replaced_count++;
        }
    }
    uint8_t pn = prefs.getUChar(NVS_K_RADAR_PERSIST, 0xFF);
    if (pn <= 10) {
        s_config.radar_persistence_n = pn;
    } else {
        s_config.radar_persistence_n = def.radar_persistence_n;
        prefs.putUChar(NVS_K_RADAR_PERSIST, def.radar_persistence_n);
        s_replaced_count++;
    }

    VALIDATE_U32(prefs, NVS_K_RADAR_POLL_IV,
                 radar_poll_interval_ms,
                 CFG_MIN_RADAR_POLL_IV, CFG_MAX_RADAR_POLL_IV,
                 def.radar_poll_interval_ms);

    VALIDATE_U32(prefs, NVS_K_RADAR_MAX_OVF,
                 radar_max_consec_overflows,
                 CFG_MIN_RADAR_MAX_OVF, CFG_MAX_RADAR_MAX_OVF,
                 def.radar_max_consec_overflows);

    // --- Party / WLED ---
    {
        String ip = prefs.getString(NVS_K_WLED_IP, "");
        if (ip.length() > 0 && ip.length() < (int)sizeof(s_config.wled_ip)) {
            strncpy(s_config.wled_ip, ip.c_str(), sizeof(s_config.wled_ip));
            s_config.wled_ip[sizeof(s_config.wled_ip) - 1] = '\0';
            // Migracija starega defaulta — zamenjamo z novim
            if (strcmp(s_config.wled_ip, "192.168.4.1") == 0) {
                strncpy(s_config.wled_ip, def.wled_ip, sizeof(s_config.wled_ip));
                prefs.putString(NVS_K_WLED_IP, def.wled_ip);
                CFGI("  wled_ip migriran: 192.168.4.1 → %s", def.wled_ip);
            }
        } else {
            if (ip.length() >= sizeof(s_config.wled_ip)) {
                CFGW("  " NVS_K_WLED_IP " predolg (%d znakov) → default", (int)ip.length());
                s_replaced_count++;
            }
            strncpy(s_config.wled_ip, def.wled_ip, sizeof(s_config.wled_ip));
            prefs.putString(NVS_K_WLED_IP, def.wled_ip);
        }
    }

    // --- Party / WLED ---
    VALIDATE_U32(prefs, NVS_K_PARTY_RESUME, party_resume_delay_s,
                 CFG_MIN_PARTY_RESUME, CFG_MAX_PARTY_RESUME, def.party_resume_delay_s);

    // --- Party slots (blobs) ---
    for (int i = 0; i < 9; i++) {
        size_t got = prefs.getBytes(NVS_PSLOT[i], &s_config.party_slots[i], sizeof(PartySlot));
        if (got != sizeof(PartySlot)) {
            s_config.party_slots[i] = def.party_slots[i];
            prefs.putBytes(NVS_PSLOT[i], &def.party_slots[i], sizeof(PartySlot));
            if (got != 0) {
                CFGW("  %s: neveljavna velikost bloba (%u B) → default", NVS_PSLOT[i], (unsigned)got);
                s_replaced_count++;
            }
        }
    }

    // --- Party schedules (blobs) ---
    for (int i = 0; i < 10; i++) {
        size_t got = prefs.getBytes(NVS_PSCHED[i], &s_config.party_schedules[i], sizeof(PartySchedule));
        if (got != sizeof(PartySchedule)) {
            s_config.party_schedules[i] = def.party_schedules[i];
            prefs.putBytes(NVS_PSCHED[i], &def.party_schedules[i], sizeof(PartySchedule));
            if (got != 0) {
                CFGW("  %s: neveljavna velikost bloba (%u B) → default", NVS_PSCHED[i], (unsigned)got);
                s_replaced_count++;
            }
        }
    }

    // --- SSR labeli ---
    for (int i = 0; i < 4; i++) {
        String val = prefs.getString(NVS_SSR_LBL[i], "");
        size_t vlen = val.length();
        if (vlen > 0 && vlen < 24) {
            strncpy(s_config.ssr_label[i], val.c_str(), 23);
            s_config.ssr_label[i][23] = '\0';
        } else {
            if (vlen >= 24) {
                CFGW("  %s predolg (%d znakov) → default", NVS_SSR_LBL[i], (int)vlen);
                s_replaced_count++;
            }
            strncpy(s_config.ssr_label[i], def.ssr_label[i], 23);
            s_config.ssr_label[i][23] = '\0';
            prefs.putString(NVS_SSR_LBL[i], def.ssr_label[i]);
        }
    }

    CFGI("NVS vrednosti naložene (53+ parametrov + party blobs)");
}

// ============================================================
// INTERNO: write_all_to_nvs
// ============================================================
// Zapiše celoten s_config v NVS.
// Prefs mora biti že odprt (read-write) pred klicem.

static void write_all_to_nvs(Preferences& prefs) {
    // Osvetlitev
    prefs.putUInt (NVS_K_TIMEOUT_SSR1, s_config.timeout_ssr1_s);
    prefs.putUInt (NVS_K_MAN_EXT,      s_config.manual_extend_min);
    prefs.putUInt (NVS_K_AF_SSR2,      s_config.antiforgot_ssr2_min);
    prefs.putUInt (NVS_K_AF_SSR3,      s_config.antiforgot_ssr3_min);
    prefs.putUChar(NVS_K_SSR2_NIGHT,   s_config.ssr2_auto_night ? 1u : 0u);
    prefs.putUChar(NVS_K_DND_START,    s_config.dnd_start_h);
    prefs.putUChar(NVS_K_DND_END,      s_config.dnd_end_h);
    prefs.putUChar(NVS_K_SSR2_DND_DIS, s_config.ssr2_dnd_disable ? 1u : 0u);
    prefs.putUInt (NVS_K_LUX_NIGHT,    s_config.lux_night);
    prefs.putUInt (NVS_K_LUX_DAY,      s_config.lux_day);
    prefs.putUChar(NVS_K_BRIGHT_NIGHT, s_config.brightness_night);
    // LED animacije
    prefs.putUInt (NVS_K_FILL_SPD,   s_config.fill_speed_ms);
    prefs.putUInt (NVS_K_UNFILL_SPD, s_config.unfill_speed_ms);
    prefs.putUInt (NVS_K_FADE_DUR,   s_config.fade_duration_ms);
    prefs.putUChar(NVS_K_TGT_BRIGHT, s_config.target_brightness);
    prefs.putUInt (NVS_K_SSR2_DELAY, s_config.ssr2_delay_ms);
    prefs.putUInt (NVS_K_PA_GREEN,   s_config.pa_thresh_green_mm);
    prefs.putUInt (NVS_K_PA_ORANGE,  s_config.pa_thresh_orange_mm);
    prefs.putUInt (NVS_K_PA_RED,     s_config.pa_thresh_red_mm);
    prefs.putUInt (NVS_K_PA_STAB,    s_config.pa_stability_s);
    prefs.putUInt (NVS_K_CLOCK_DUR,  s_config.clock_duration_s);
    prefs.putUInt (NVS_K_CELL_TMR,   s_config.photocell_timer_min);
    // Identifikacija
    prefs.putUInt (NVS_K_DTW_THRESH, float_to_u32(s_config.dtw_threshold));
    prefs.putUChar(NVS_K_SAKOE_RAD,  s_config.sakoe_radius);
    prefs.putUChar(NVS_K_MIN_PROF,   s_config.min_profile_points);
    prefs.putUChar(NVS_K_NORM_PTS,   s_config.normalize_points);
    prefs.putUInt (NVS_K_DELTA_FLT,  s_config.delta_filter_mm);
    prefs.putUInt (NVS_K_PHASE_CFM,  s_config.phase_confirm_cm);
    prefs.putUInt (NVS_K_STAB_S,     float_to_u32(s_config.stability_s));
    prefs.putUChar(NVS_K_RAW_N,     s_config.raw_profiles_per_model);
    prefs.putUChar(NVS_K_PRES_MIN,  s_config.presence_check_min);
    prefs.putUInt (NVS_K_EMPTY_TOL, s_config.empty_tolerance_mm);
    // Radar parametri
    for (int ri = 0; ri < 4; ri++) {
        prefs.putUChar(NVS_RADAR_MD[ri], s_config.radar_max_dist[ri]);
        prefs.putUChar(NVS_RADAR_MS[ri], s_config.radar_move_sens[ri]);
        prefs.putUChar(NVS_RADAR_SS[ri], s_config.radar_static_sens[ri]);
        prefs.putUInt (NVS_RADAR_US[ri], s_config.radar_unmanned_s[ri]);
    }
    prefs.putUChar(NVS_K_RADAR_PERSIST, s_config.radar_persistence_n);
    prefs.putUInt(NVS_K_RADAR_POLL_IV,  s_config.radar_poll_interval_ms);
    prefs.putUInt(NVS_K_RADAR_MAX_OVF,  s_config.radar_max_consec_overflows);
    // Party / WLED
    prefs.putString(NVS_K_WLED_IP, s_config.wled_ip);
    prefs.putUInt(NVS_K_PARTY_RESUME, s_config.party_resume_delay_s);
    // Party slots
    for (int i = 0; i < 9; i++)
        prefs.putBytes(NVS_PSLOT[i], &s_config.party_slots[i], sizeof(PartySlot));
    // Party schedules
    for (int i = 0; i < 10; i++)
        prefs.putBytes(NVS_PSCHED[i], &s_config.party_schedules[i], sizeof(PartySchedule));
    // SSR labeli
    for (int i = 0; i < 4; i++)
        prefs.putString(NVS_SSR_LBL[i], s_config.ssr_label[i]);
}

// ============================================================
// config_mgr_init
// ============================================================

bool config_mgr_init() {
    CFGI("=== config_mgr_init ===");

    // Ustvari mutex
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            CFGE("xSemaphoreCreateMutex napaka — degraded mode (samo RAM defaults)");
            s_config = config_defaults();
            s_replaced_count = 0;
            s_initialized = true;
            s_nvs_ok = false;
            return true;  // degraded mode — ne sesujemo sistema
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Odpri NVS
    bool opened = s_prefs.begin(NVS_NS, false);  // false = read-write
    if (!opened) {
        CFGE("Preferences::begin('%s') napaka — degraded mode (samo RAM defaults)", NVS_NS);
        s_config = config_defaults();
        s_replaced_count = 0;
        s_nvs_ok = false;
        s_initialized = true;
        xSemaphoreGive(s_mutex);
        return true;  // degraded mode
    }
    s_nvs_ok = true;

    // Preberi in validiraj vse vrednosti
    load_and_validate(s_prefs);

    s_prefs.end();

    xSemaphoreGive(s_mutex);

    if (s_replaced_count == 0) {
        CFGI("config_mgr_init OK — vse vrednosti veljavne iz NVS");
    } else {
        CFGI("config_mgr_init OK — %d vrednost(i) zamenjanih z defaultom in zapisanih v NVS",
             (int)s_replaced_count);
    }

    s_initialized = true;
    return true;
}

// ============================================================
// config_get
// ============================================================
// Vrne const ref na s_config.
// POZOR: priporočena uporaba je kopija na stack:
//   const Config cfg = config_get();
// Ne hrani reference čez daljše operacije.

const Config& config_get() {
    // s_config je volatile glede na NVS pisanja, a za branje
    // posameznih polj je dovolj atomarno na ESP32 (32-bit read).
    // Za konsistenten snapshot priporočamo kopijo.
    return s_config;
}

// ============================================================
// config_set
// ============================================================

void config_set(const Config& c) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = c;
    xSemaphoreGive(s_mutex);
    CFGD("config_set — vrednosti posodobljene v RAM (ni še shranjeno v NVS)");
}

// ============================================================
// config_save
// ============================================================

bool config_save() {
    if (!s_mutex || !s_nvs_ok) {
        CFGW("config_save: NVS ni na voljo (nvs_ok=%d)", (int)s_nvs_ok);
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool opened = s_prefs.begin(NVS_NS, false);
    if (!opened) {
        CFGE("config_save: Preferences::begin napaka");
        xSemaphoreGive(s_mutex);
        return false;
    }

    write_all_to_nvs(s_prefs);
    s_prefs.end();

    xSemaphoreGive(s_mutex);

    CFGI("config_save OK — vse vrednosti zapisane v NVS");
    return true;
}

// ============================================================
// config_reset_defaults
// ============================================================

bool config_reset_defaults() {
    CFGI("config_reset_defaults — ponastavljam na hardkodirane vrednosti");

    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_config = config_defaults();

    if (s_nvs_ok) {
        bool opened = s_prefs.begin(NVS_NS, false);
        if (opened) {
            // Počisti celoten namespace in zapiši sveže defaulte.
            // clear() zbriše vse ključe v namespaceu — čistejše od
            // pisanja čez obstoječe vrednosti (prepreči stare sirotske ključe).
            s_prefs.clear();
            write_all_to_nvs(s_prefs);
            s_prefs.end();
            CFGI("config_reset_defaults OK — NVS počiščen in napolnjen z defaulti");
        } else {
            CFGW("config_reset_defaults: NVS ni dostopen — samo RAM reset");
        }
    } else {
        CFGW("config_reset_defaults: NVS ni inicializiran — samo RAM reset");
    }

    xSemaphoreGive(s_mutex);
    return true;
}

// ============================================================
// config_mgr_replaced_count / config_mgr_ok
// ============================================================

uint8_t config_mgr_replaced_count() {
    return s_replaced_count;
}

bool config_mgr_ok() {
    return s_initialized;
}

// ============================================================
// PARTY SLOT API
// ============================================================

PartySlot config_get_party_slot(uint8_t idx) {
    if (idx >= 9) { PartySlot empty{}; return empty; }
    if (!s_mutex) return s_config.party_slots[idx];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    PartySlot sl = s_config.party_slots[idx];
    xSemaphoreGive(s_mutex);
    return sl;
}

void config_set_party_slot(uint8_t idx, const PartySlot& slot) {
    if (idx >= 9) return;
    if (!s_mutex) { s_config.party_slots[idx] = slot; return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.party_slots[idx] = slot;
    xSemaphoreGive(s_mutex);
}

bool config_save_party_slots() {
    if (!s_mutex || !s_nvs_ok) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool opened = s_prefs.begin(NVS_NS, false);
    if (!opened) { xSemaphoreGive(s_mutex); return false; }
    for (int i = 0; i < 9; i++)
        s_prefs.putBytes(NVS_PSLOT[i], &s_config.party_slots[i], sizeof(PartySlot));
    s_prefs.end();
    xSemaphoreGive(s_mutex);
    CFGD("config_save_party_slots OK");
    return true;
}

// ============================================================
// PARTY SCHEDULE API
// ============================================================

PartySchedule config_get_party_schedule(uint8_t idx) {
    if (idx >= 10) { PartySchedule empty{}; return empty; }
    if (!s_mutex) return s_config.party_schedules[idx];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    PartySchedule sc = s_config.party_schedules[idx];
    xSemaphoreGive(s_mutex);
    return sc;
}

void config_set_party_schedule(uint8_t idx, const PartySchedule& sched) {
    if (idx >= 10) return;
    if (!s_mutex) { s_config.party_schedules[idx] = sched; return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.party_schedules[idx] = sched;
    xSemaphoreGive(s_mutex);
}

bool config_save_party_schedules() {
    if (!s_mutex || !s_nvs_ok) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool opened = s_prefs.begin(NVS_NS, false);
    if (!opened) { xSemaphoreGive(s_mutex); return false; }
    for (int i = 0; i < 10; i++)
        s_prefs.putBytes(NVS_PSCHED[i], &s_config.party_schedules[i], sizeof(PartySchedule));
    s_prefs.end();
    xSemaphoreGive(s_mutex);
    CFGD("config_save_party_schedules OK");
    return true;
}
