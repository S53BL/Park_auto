// ============================================================
// settings.js — Stran: Nastavitve
// 3 tabi: Osvetlitev · LED animacije · Identifikacija vozil
// GET /api/config ob nalaganju, POST /api/config ob shranjevanju
// ============================================================

(function () {
  const DIV = 'page-settings';
  let _cfg = null;

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Nastavitve</h1>
  <span class="subtitle" id="cfg-status">–</span>
</div>

<div class="tab-bar">
  <button class="tab-btn active" id="stab-light" onclick="cfgTab('light')">Osvetlitev</button>
  <button class="tab-btn"        id="stab-led"   onclick="cfgTab('led')">LED animacije</button>
  <button class="tab-btn"        id="stab-ident"  onclick="cfgTab('ident')">Identifikacija</button>
</div>

<!-- TAB: OSVETLITEV ────────────────────────── -->
<div class="tab-panel active card" id="spanel-light">
  ${_field('timeout_ssr1_s',      'Timeout SSR1',           's',   180,  'Čas brez triggerja pred ugasnitvijo glavne luči')}
  ${_field('manual_extend_min',   'Ročni podaljšek',        'min',  30,  'Podaljšanje ob pritisku gumba na LCD')}
  ${_field('antiforgot_ssr2_min', 'Anti-forget SSR2',       'min',   5,  'Samodejni izklop ob odsotnosti gibanja')}
  ${_field('antiforgot_ssr3_min', 'Anti-forget SSR3',       'min',   5,  'Samodejni izklop ob odsotnosti gibanja')}
  ${_toggle('ssr2_auto_night',    'SSR2 avtomatski ponoči',         true,  'Ali SSR2 sveti pri avtomatskem prižigu')}
  ${_field('lux_threshold',       'Mejna vrednost lux',     'lx',   50,  'Pod to vrednostjo = noč')}
  ${_field('brightness_night',    'Nočni brightness',       '0–255',120, 'Znižana svetlost LED matrike med 22–6h')}
  ${_saveBtn('light')}
</div>

<!-- TAB: LED animacije ─────────────────────── -->
<div class="tab-panel card" id="spanel-led">
  ${_field('fill_speed_ms',       'Fill speed',             'ms',  6000, 'Skupni čas waveFill animacije')}
  ${_field('unfill_speed_ms',     'Unfill speed',           'ms',  3000, 'Skupni čas waveUnfill animacije')}
  ${_field('fade_duration_ms',    'Fade duration',          'ms',   800, 'Čas naraščanja/padanja posamezne LED')}
  ${_field('target_brightness',   'Target brightness',      '0–255',200, 'Ciljna svetlost LED matrike')}
  ${_field('ssr2_delay_ms',       'SSR2 zamik',             'ms',   500, 'Zamik SSR2 vklopa po zaključku waveFill')}
  <div class="section-label mt16">Parking assist razdalje</div>
  ${_field('pa_thresh1_mm',       'Prag 1 — zelena',        'mm',  1500, 'Razdalja do oranžne cone')}
  ${_field('pa_thresh2_mm',       'Prag 2 — oranžna',       'mm',  1000, 'Razdalja do rdeče cone')}
  ${_field('pa_thresh3_mm',       'Prag 3 — rdeča',         'mm',   500, 'Razdalja do polnega traka')}
  ${_field('pa_stability_s',      'PA stabilnost',          's',      4, 'Čas mirovanja za deaktivacijo parking assist')}
  <div class="section-label mt16">Ostalo</div>
  ${_field('photocell_timer_min', 'Fotocelice timer',       'min',    5, 'Trajanje utripanja ob prekinitvi')}
  ${_field('clock_duration_s',    'Prikaz ure',             's',     10, 'Čas prikaza analogne ure po zaznavi gibanja')}
  ${_saveBtn('led')}
</div>

<!-- TAB: IDENTIFIKACIJA ────────────────────── -->
<div class="tab-panel card" id="spanel-ident">
  ${_field('dtw_threshold',       'DTW prag',               '',     18,  'Razdalja < prag → obstoječ model')}
  ${_field('sakoe_radius',        'Sakoe-Chiba radius',     '',     15,  'Širina diagonalnega pasu DTW matrike')}
  ${_field('min_profile_points',  'Min. točk profila',      '',     25,  'Profil z manj točkami se zavrže')}
  ${_field('normalize_points',    'Normalizacija N točk',   '',     80,  'Ciljna dolžina profila po interpolaciji')}
  ${_field('delta_filter_mm',     'Δ-filter H razdalja',    'mm',   15,  'Min. sprememba H za zajem nove točke')}
  ${_field('phase_confirm_cm',    'Faza 1→2 prag',          'cm',  350,  'H razdalja za potrditev aktivnega mesta')}
  ${_field('stability_s',         'Stabilnost za konec',    's',    1.5, 'H mora biti stabilen ta čas za READY')}
  ${_field('poll_idle_ms',        'Polling — mirovanje',    'ms',  100,  'Interval pollinga v Fazi 0')}
  ${_field('poll_active_ms',      'Polling — detekcija',    'ms',   40,  'Interval pollinga v Fazi 1 in 2')}
  ${_saveBtn('ident')}
</div>
`;
  }

  // ── Field templates ──────────────────────────────────────
  function _field(key, label, unit, def, hint) {
    return `
<div class="form-row">
  <div>
    <div class="form-label">${label} ${unit ? '<span class="text-dim text-tiny">(${unit})</span>' : ''}</div>
    ${hint ? '<div class="form-hint">' + hint + '</div>' : ''}
  </div>
  <input class="form-input" type="number" id="cfg-${key}" value="${def}" step="any">
</div>`.replace('${unit}', unit);
  }

  function _toggle(key, label, def, hint) {
    return `
<div class="form-row">
  <div>
    <div class="form-label">${label}</div>
    ${hint ? '<div class="form-hint">' + hint + '</div>' : ''}
  </div>
  <label class="form-toggle">
    <input type="checkbox" id="cfg-${key}" ${def ? 'checked' : ''}>
    <span class="form-toggle-track"></span>
  </label>
</div>`;
  }

  function _saveBtn(tab) {
    return `<div class="flex-row flex-end mt16">
      <button class="btn btn-primary" onclick="cfgSave('${tab}')">Shrani</button>
    </div>`;
  }

  // ── Polni form iz konfiguracije ──────────────────────────
  function _fill(cfg) {
    if (!cfg) return;
    const all = Object.assign({}, cfg.light || {}, cfg.led || {}, cfg.ident || {});
    Object.entries(all).forEach(([k, v]) => {
      const el = document.getElementById('cfg-' + k);
      if (!el) return;
      if (el.type === 'checkbox') el.checked = !!v;
      else el.value = v;
    });
  }

  // ── Zbere vrednosti za en tab ────────────────────────────
  function _collect(tab) {
    const keys = {
      light: ['timeout_ssr1_s','manual_extend_min','antiforgot_ssr2_min','antiforgot_ssr3_min',
              'ssr2_auto_night','lux_threshold','brightness_night'],
      led:   ['fill_speed_ms','unfill_speed_ms','fade_duration_ms','target_brightness','ssr2_delay_ms',
              'pa_thresh1_mm','pa_thresh2_mm','pa_thresh3_mm','pa_stability_s',
              'photocell_timer_min','clock_duration_s'],
      ident: ['dtw_threshold','sakoe_radius','min_profile_points','normalize_points',
              'delta_filter_mm','phase_confirm_cm','stability_s','poll_idle_ms','poll_active_ms']
    };
    const obj = {};
    (keys[tab] || []).forEach(k => {
      const el = document.getElementById('cfg-' + k);
      if (!el) return;
      if (el.type === 'checkbox') obj[k] = el.checked;
      else {
        const v = parseFloat(el.value);
        obj[k] = isNaN(v) ? el.value : v;
      }
    });
    return obj;
  }

  // ── Globalne funkcije ────────────────────────────────────
  window.cfgTab = function(tab) {
    document.querySelectorAll('#page-settings .tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('#page-settings .tab-panel').forEach(p => p.classList.remove('active'));
    document.getElementById('stab-' + tab).classList.add('active');
    document.getElementById('spanel-' + tab).classList.add('active');
  };

  window.cfgSave = async function(tab) {
    const st = document.getElementById('cfg-status');
    try {
      if (st) st.textContent = 'Shranjujem…';
      const body = {};
      body[tab] = _collect(tab);
      const r = await api.post('/api/config', body);
      if (st) st.textContent = r._stub
        ? 'Shranjeno v RAM (stub — Faza 3 doda NVS)'
        : 'Shranjeno ✓';
    } catch(e) {
      if (st) st.textContent = 'Napaka: ' + e.message;
    }
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_settings = async function () {
    if (!document.getElementById('cfg-timeout_ssr1_s')) _render();
    const st = document.getElementById('cfg-status');
    try {
      if (st) st.textContent = 'Nalagam…';
      _cfg = await api.get('/api/config');
      _fill(_cfg);
      if (st) st.textContent = _cfg._stub
        ? 'Privzete vrednosti (stub)'
        : 'Naloženo iz naprave';
    } catch(e) {
      if (st) st.textContent = 'Napaka: ' + e.message;
    }
  };
})();
