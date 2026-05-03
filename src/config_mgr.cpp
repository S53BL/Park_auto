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

// ============================================================
// MEJNE VREDNOSTI ZA VALIDACIJO
// ============================================================

// Osvetlitev
#define CFG_MIN_TIMEOUT_SSR1    30u
#define CFG_MAX_TIMEOUT_SSR1    3600u
#define CFG_MIN_MAN_EXT         1u
#define CFG_MAX_MAN_EXT         120u
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
        CFGD("  " key " = %lu", (unsigned long)_v);                         \
        if (_v >= (vmin) && _v <= (vmax)) {                                  \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_v != UINT32_MAX) {                                          \
                CFGW("  " key " = %lu izven meja [%lu,%lu] → default %lu",  \
                     (unsigned long)_v, (unsigned long)(vmin),               \
                     (unsigned long)(vmax), (unsigned long)(defval));        \
            } else {                                                         \
                CFGD("  " key " ni v NVS → default %lu",                    \
                     (unsigned long)(defval));                               \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUInt((key), (defval));                                \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

#define VALIDATE_U8(prefs, key, field, vmin, vmax, defval)                  \
    do {                                                                     \
        uint8_t _v = (prefs).getUChar((key), 0xFF);                         \
        CFGD("  " key " = %u", (unsigned)_v);                               \
        if (_v >= (vmin) && _v <= (vmax)) {                                  \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_v != 0xFF) {                                                \
                CFGW("  " key " = %u izven meja [%u,%u] → default %u",      \
                     (unsigned)_v, (unsigned)(vmin),                         \
                     (unsigned)(vmax), (unsigned)(defval));                  \
            } else {                                                         \
                CFGD("  " key " ni v NVS → default %u",                     \
                     (unsigned)(defval));                                    \
            }                                                                \
            s_config.field = (defval);                                       \
            (prefs).putUChar((key), (defval));                               \
            s_replaced_count++;                                              \
        }                                                                    \
    } while(0)

#define VALIDATE_BOOL(prefs, key, field, defval)                            \
    do {                                                                     \
        uint8_t _v = (prefs).getUChar((key), 0xFF);                         \
        CFGD("  " key " = %u", (unsigned)_v);                               \
        if (_v == 0 || _v == 1) {                                            \
            s_config.field = (_v == 1);                                      \
        } else {                                                             \
            if (_v != 0xFF) {                                                \
                CFGW("  " key " = %u neveljavna bool vrednost → default %d", \
                     (unsigned)_v, (int)(defval));                           \
            } else {                                                         \
                CFGD("  " key " ni v NVS → default %d", (int)(defval));     \
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
        CFGD("  " key " = %.3f (raw=0x%08lX)", float_valid(_v) ? _v : 0.0f,\
             (unsigned long)_raw);                                           \
        if (float_valid(_v) && _v >= (vmin) && _v <= (vmax)) {              \
            s_config.field = _v;                                             \
        } else {                                                             \
            if (_raw != UINT32_MAX && float_valid(_v)) {                     \
                CFGW("  " key " = %.3f izven meja [%.1f,%.1f] → default %.3f",\
                     _v, (float)(vmin), (float)(vmax), (float)(defval));    \
            } else {                                                         \
                CFGD("  " key " ni v NVS ali NaN → default %.3f",           \
                     (float)(defval));                                       \
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

    CFGD("Beremo NVS namespace '%s'...", NVS_NS);

    const Config def = config_defaults();

    // --- Osvetlitev ---
    VALIDATE_U32 (prefs, NVS_K_TIMEOUT_SSR1, timeout_ssr1_s,      CFG_MIN_TIMEOUT_SSR1, CFG_MAX_TIMEOUT_SSR1, def.timeout_ssr1_s);
    VALIDATE_U32 (prefs, NVS_K_MAN_EXT,      manual_extend_min,   CFG_MIN_MAN_EXT,      CFG_MAX_MAN_EXT,      def.manual_extend_min);
    VALIDATE_U32 (prefs, NVS_K_AF_SSR2,      antiforgot_ssr2_min, CFG_MIN_AF_SSR,       CFG_MAX_AF_SSR,       def.antiforgot_ssr2_min);
    VALIDATE_U32 (prefs, NVS_K_AF_SSR3,      antiforgot_ssr3_min, CFG_MIN_AF_SSR,       CFG_MAX_AF_SSR,       def.antiforgot_ssr3_min);
    VALIDATE_BOOL(prefs, NVS_K_SSR2_NIGHT,   ssr2_auto_night,                                                 def.ssr2_auto_night);
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
