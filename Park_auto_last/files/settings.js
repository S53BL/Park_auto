// ============================================================
// settings.js — Stran: Nastavitve  (v3 — Identifikacija tab razširjen)
// 4 tabi: Osvetlitev · LED animacije · Identifikacija · Radar
// GET /api/config ob nalaganju, POST /api/config ob shranjevanju
// GET+POST /api/radar za radar tab
//
// SPREMEMBE v3 glede na v2:
//   - Identifikacija tab: dodana 3 nova polja
//       raw_profiles_per_model  (10–100, privzeto 30)
//       presence_check_min      (1–60 min, privzeto 10)
//       empty_tolerance_mm      (50–200 mm, privzeto 200)
//   - _collect('ident') vkljucuje nova polja
//   - vehicle_recog_on_config_changed() klice web_ui.cpp ob POST (ne JS)
//   - _stub check odstranjen
// ============================================================

(function () {
  const DIV = 'page-settings';
  let _cfg      = null;
  let _radarCfg = null;
  let _saving   = false;

  // ── Field/toggle template builder-ji ────────────────────
  function _field(key, label, unit, def, hint, step) {
    const s = step !== undefined ? step : 'any';
    return `
<div class="form-row">
  <div class="flex-col">
    <div class="form-label">${label}${unit ? ' <span class="text-dim text-tiny">(${unit})</span>' : ''}</div>
    ${hint ? '<div class="form-hint">' + hint + '</div>' : ''}
  </div>
  <input class="form-input" type="number" id="cfg-${key}"
         value="${def}" step="${s}" autocomplete="off">
</div>`.replace('${unit}', unit || '');
  }

  function _toggle(key, label, def, hint) {
    return `
<div class="form-row">
  <div class="flex-col">
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
      <button class="btn btn-primary" id="save-btn-${tab}"
              onclick="cfgSave('${tab}')">Shrani</button>
    </div>`;
  }

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
  <button class="tab-btn"        id="stab-ident" onclick="cfgTab('ident')">Identifikacija</button>
  <button class="tab-btn"        id="stab-radar" onclick="cfgTab('radar')">Radar</button>
</div>

<!-- ── TAB: OSVETLITEV ──────────────────────── -->
<div class="tab-panel active card" id="spanel-light">
  ${_field('timeout_ssr1_s',      'Timeout SSR1',           's',     180, 'Čas brez gibanja pred ugasnitvijo glavne luči (SSR1)', 1)}
  ${_field('manual_extend_min',   'Ročni podaljšek',        'min',    30, 'Podaljšanje timera ob dotiku gumba na LCD', 1)}
  ${_field('antiforgot_ssr2_min', 'Anti-forget SSR2',       'min',     5, 'Samodejni izklop LED panelov ob odsotnosti gibanja', 1)}
  ${_field('antiforgot_ssr3_min', 'Anti-forget SSR3',       'min',     5, 'Samodejni izklop reflektorja ob odsotnosti gibanja', 1)}
  ${_toggle('ssr2_auto_night',    'SSR2 avtomatski ponoči',  true,       'Samodejni vklop LED panelov ob prehodu v nočni način')}
  <div class="section-label mt16">Hystereza noč / dan</div>
  ${_field('lux_threshold',       'Prag NOČ (lux)',          'lx',     40, 'Pod to vrednostjo → noč (BH1750)', 1)}
  ${_field('lux_day',             'Prag DAN (lux)',          'lx',     70, 'Nad to vrednostjo → dan (mora biti > prag noč)', 1)}
  ${_field('brightness_night',    'Nočni brightness',        '0–255', 120, 'Znižana svetlost LED matrike med 22:00–6:00', 1)}
  ${_saveBtn('light')}
</div>

<!-- ── TAB: LED ANIMACIJE ───────────────────── -->
<div class="tab-panel card" id="spanel-led">
  <div class="section-label">Animacije</div>
  ${_field('fill_speed_ms',       'Fill speed',              'ms',  6000, 'Skupni čas waveFill animacije (LED 0→89)', 100)}
  ${_field('unfill_speed_ms',     'Unfill speed',            'ms',  3000, 'Skupni čas waveUnfill animacije (LED 89→0)', 100)}
  ${_field('fade_duration_ms',    'Fade duration',           'ms',   800, 'Čas naraščanja/padanja posamezne LED', 50)}
  ${_field('target_brightness',   'Target brightness',       '0–255',200, 'Ciljna svetlost LED matrike (0=izklopljeno)', 1)}
  ${_field('ssr2_delay_ms',       'SSR2 zamik',              'ms',   500, 'Zamik med vklopom SSR1 in SSR2 (stabilizacija trafota)', 50)}
  <div class="section-label mt16">Parking assist — razdalje (H senzor)</div>
  ${_field('pa_thresh1_mm',       'Prag 1 — zelena',         'mm', 1500, 'Zelena → oranžna ob tej razdalji', 50)}
  ${_field('pa_thresh2_mm',       'Prag 2 — oranžna',        'mm', 1000, 'Oranžna → rdeča ob tej razdalji', 50)}
  ${_field('pa_thresh3_mm',       'Prag 3 — rdeča',          'mm',  500, 'Rdeča → polno rdeča ob tej razdalji', 50)}
  ${_field('pa_stability_s',      'PA stabilnost',           's',     4, 'Čas mirovanja H senzorja za deaktivacijo parking assist', 1)}
  <div class="section-label mt16">Signalna LED</div>
  ${_field('photocell_timer_min', 'Fotocelice timer',        'min',   5, 'Trajanje utripanja signalne LED ob prekinitvi fotocelice', 1)}
  ${_field('clock_duration_s',    'Prikaz ure',              's',    10, 'Čas prikaza analogne ure po zaznavi gibanja', 1)}
  ${_saveBtn('led')}
</div>

<!-- ── TAB: IDENTIFIKACIJA ──────────────────── -->
<div class="tab-panel card" id="spanel-ident">
  <div class="section-label">DTW algoritem</div>
  ${_field('dtw_threshold',       'DTW prag prepoznave',     '',      18, 'Razdalja < prag → obstoječ model; ≥ prag → nov model (manjše = strožje)', 0.5)}
  ${_field('sakoe_radius',        'Sakoe-Chiba radius',      'točke', 15, 'Širina diagonalnega pasu DTW matrike (večji = počasneje, bolj fleksibilen)', 1)}
  <div class="section-label mt16">Profil vozila</div>
  ${_field('min_profile_points',  'Min. točk profila',       '',      25, 'Profil z manj točkami se zavrže (pešec, lažni prihod)', 1)}
  ${_field('normalize_points',    'Normalizacija N točk',    '',      80, 'Ciljna dolžina profila po interpolaciji pred DTW (20–80)', 1)}
  ${_field('delta_filter_mm',     'Δ-filter H razdalja',     'mm',    15, 'Min. sprememba H za zajem nove točke — odfiltrira šum senzorja', 1)}
  <div class="section-label mt16">TOF fazni avtomat</div>
  ${_field('phase_confirm_cm',    'Faza 1→2 prag',           'cm',   350, 'H razdalja pri kateri potrdimo aktivno parkirno mesto', 10)}
  ${_field('stability_s',         'Stabilnost za zaključek', 's',    1.5, 'H mora biti stabilen ta čas preden se sproži TOF_PROFILE_READY', 0.1)}
  <div class="section-label mt16">Raw profili in prisotnost</div>
  ${_field('raw_profiles_per_model', 'Raw profilov na model', '',    30,
           'Max shranjenih raw profilov na SD na model (FIFO rotacija). Razpon: 10–100.', 1)}
  ${_field('presence_check_min',  'Periodicno preverjanje',  'min',  10,
           'Interval preverjanja prisotnosti vozila (reconcile state machine). Razpon: 1–60.', 1)}
  ${_field('empty_tolerance_mm',  'Toleranca praznega',      'mm',  200,
           'Toleranca ±mm za zaznavo praznega mesta glede na baseline. Razpon: 50–200.', 10)}
  ${_saveBtn('ident')}
</div>

<!-- ── TAB: RADAR ───────────────────────────── -->
<div class="tab-panel card" id="spanel-radar">
  <div class="section-label">Globalne nastavitve</div>
  ${_field('radar_persistence_n',       'Persistence filter',  'frames', 3,
           'N zaporednih frames pred SSR triggerjem (0=izklopljeno)', 1)}
  ${_field('radar_poll_interval_ms',    'Polling interval',    'ms',    50,
           'Interval branja UART FIFO (10–100 ms). Manjši = hitrejši odziv, večji = manj Wire1 obremenitve.', 5)}
  ${_field('radar_max_consec_overflows','Max overflowi',       '',      10,
           'Število zaporednih FIFO overflowov pred WARN logom (1–100). Povečaj če je log poln opozoril.', 1)}
  ${_saveBtn('radar')}

  <div class="section-label mt16">Per-senzor konfiguracija</div>
  <p class="form-hint" style="margin-bottom:12px">
    Sprememba shrani v NVS in takoj pošlje na radar (~300 ms, radar začasno ne poroča).
  </p>
  <div id="radar-sensors-list">
    <div class="empty-state loading">Nalagam radar konfiguracijo…</div>
  </div>
</div>
`;
  }

  // ── Radar per-senzor vrstice ─────────────────────────────
  function _renderRadarSensors(rd) {
    if (!rd || !rd.sensors) return;
    const container = document.getElementById('radar-sensors-list');
    if (!container) return;
    const names = ['Vhod', 'Cesta L', 'Cesta D', 'Garaža'];
    container.innerHTML = rd.sensors.map((s, i) => `
<div class="card" style="margin-bottom:10px;border-color:var(--border2)">
  <div class="flex-row flex-between mb8">
    <div class="card-title" style="margin:0">${names[i]}</div>
    <span class="badge ${s.active ? (s.config_ok ? 'badge-green' : 'badge-amber') : 'badge-gray'}">
      ${s.active ? (s.config_ok ? 'OK' : 'cfg napaka') : 'N/I'}
    </span>
  </div>
  <div class="grid-4" style="gap:8px">
    <div>
      <div class="form-hint">Max razd. (0–8)</div>
      <input class="form-input w100" type="number" min="0" max="8" step="1"
             id="radar-${i}-max_dist" value="${s.max_dist || 2}">
    </div>
    <div>
      <div class="form-hint">Gibanje (0–100)</div>
      <input class="form-input w100" type="number" min="0" max="100" step="1"
             id="radar-${i}-move_sens" value="${s.move_sens || 20}">
    </div>
    <div>
      <div class="form-hint">Statično (0–100)</div>
      <input class="form-input w100" type="number" min="0" max="100" step="1"
             id="radar-${i}-static_sens" value="${s.static_sens || 0}">
    </div>
    <div>
      <div class="form-hint">Unmanned (s)</div>
      <input class="form-input w100" type="number" min="0" max="300" step="1"
             id="radar-${i}-unmanned_s" value="${s.unmanned_s || 5}">
    </div>
  </div>
  <div class="flex-row flex-end mt8">
    <span class="text-dim text-tiny" id="radar-${i}-status"></span>
    <button class="btn btn-sm btn-primary" style="margin-left:8px"
            onclick="radarSave(${i})">Shrani senzor ${i+1}</button>
  </div>
</div>`).join('');
  }

  // ── Polni form iz /api/config odgovora ───────────────────
  function _fill(cfg) {
    if (!cfg) return;
    const flat = Object.assign({},
      cfg.light  || {},
      cfg.led    || {},
      cfg.ident  || {}
    );
    Object.entries(flat).forEach(([k, v]) => {
      const el = document.getElementById('cfg-' + k);
      if (!el) return;
      if (el.type === 'checkbox') el.checked = !!v;
      else el.value = v;
    });
  }

  // ── Zbere vrednosti enega taba ───────────────────────────
  function _collect(tab) {
    const keys = {
      light: ['timeout_ssr1_s','manual_extend_min','antiforgot_ssr2_min','antiforgot_ssr3_min',
              'ssr2_auto_night','lux_threshold','lux_day','brightness_night'],
      led:   ['fill_speed_ms','unfill_speed_ms','fade_duration_ms','target_brightness',
              'ssr2_delay_ms','pa_thresh1_mm','pa_thresh2_mm','pa_thresh3_mm',
              'pa_stability_s','photocell_timer_min','clock_duration_s'],
      // Vsi ident parametri — vkljucno z novimi 3 (B6/B7)
      ident: ['dtw_threshold','sakoe_radius','min_profile_points','normalize_points',
              'delta_filter_mm','phase_confirm_cm','stability_s',
              'raw_profiles_per_model','presence_check_min','empty_tolerance_mm']
    };
    const obj = {};
    (keys[tab] || []).forEach(k => {
      const el = document.getElementById('cfg-' + k);
      if (!el) return;
      if (el.type === 'checkbox') {
        obj[k] = el.checked;
      } else {
        const v = parseFloat(el.value);
        obj[k] = isNaN(v) ? el.value : v;
      }
    });
    return obj;
  }

  // ── Status sporočilo ─────────────────────────────────────
  function _setStatus(msg, ok) {
    const el = document.getElementById('cfg-status');
    if (!el) return;
    el.textContent = msg;
    el.style.color = ok === true
      ? 'var(--green)'
      : ok === false ? 'var(--red)' : 'var(--text3)';
  }

  // ── Globalne funkcije ────────────────────────────────────
  window.cfgTab = function(tab) {
    document.querySelectorAll('#page-settings .tab-btn').forEach(
      b => b.classList.remove('active'));
    document.querySelectorAll('#page-settings .tab-panel').forEach(
      p => p.classList.remove('active'));
    document.getElementById('stab-' + tab).classList.add('active');
    document.getElementById('spanel-' + tab).classList.add('active');
    if (tab === 'radar' && !_radarCfg) _loadRadar();
  };

  window.cfgSave = async function(tab) {
    if (_saving) return;
    _saving = true;
    const btn = document.getElementById('save-btn-' + tab);
    if (btn) { btn.disabled = true; btn.textContent = 'Shranjujem…'; }
    _setStatus('Shranjujem…');
    try {
      const body = {};
      body[tab] = _collect(tab);
      await api.post('/api/config', body);
      _setStatus('Shranjeno ✓', true);
    } catch(e) {
      _setStatus('Napaka: ' + e.message, false);
    } finally {
      _saving = false;
      if (btn) { btn.disabled = false; btn.textContent = 'Shrani'; }
    }
  };

  // ── Radar: naloži ────────────────────────────────────────
  async function _loadRadar() {
    try {
      _radarCfg = await api.get('/api/radar');
      _renderRadarSensors(_radarCfg);
      const fields = {
        'radar_persistence_n':        'persistence_n',
        'radar_poll_interval_ms':     'poll_interval_ms',
        'radar_max_consec_overflows': 'max_consec_overflows'
      };
      Object.entries(fields).forEach(([elId, key]) => {
        const el = document.getElementById('cfg-' + elId);
        if (el && _radarCfg[key] !== undefined) el.value = _radarCfg[key];
      });
    } catch(e) {
      const c = document.getElementById('radar-sensors-list');
      if (c) c.innerHTML = `<div class="text-dim text-tiny" style="padding:10px">
        Napaka pri nalaganju radar config: ${e.message}</div>`;
    }
  }

  window.radarSave = async function(sensorIdx) {
    const stEl = document.getElementById('radar-' + sensorIdx + '-status');
    const setS = (msg, ok) => {
      if (!stEl) return;
      stEl.textContent = msg;
      stEl.style.color = ok === true ? 'var(--green)' :
                         ok === false ? 'var(--red)' : 'var(--text3)';
    };
    setS('Shranjujem…');
    try {
      const body = {
        sensor:      sensorIdx,
        max_dist:    parseInt(document.getElementById('radar-'+sensorIdx+'-max_dist').value, 10),
        move_sens:   parseInt(document.getElementById('radar-'+sensorIdx+'-move_sens').value, 10),
        static_sens: parseInt(document.getElementById('radar-'+sensorIdx+'-static_sens').value, 10),
        unmanned_s:  parseInt(document.getElementById('radar-'+sensorIdx+'-unmanned_s').value, 10)
      };
      const r = await api.post('/api/radar/config', body);
      if (r.warn) {
        setS('⚠ ' + r.warn, false);
      } else {
        setS('Shranjeno ✓', true);
        _radarCfg = await api.get('/api/radar');
        _renderRadarSensors(_radarCfg);
        const fields = {
          'radar_persistence_n':        'persistence_n',
          'radar_poll_interval_ms':     'poll_interval_ms',
          'radar_max_consec_overflows': 'max_consec_overflows'
        };
        Object.entries(fields).forEach(([elId, key]) => {
          const el = document.getElementById('cfg-' + elId);
          if (el && _radarCfg[key] !== undefined) el.value = _radarCfg[key];
        });
      }
    } catch(e) {
      setS('Napaka: ' + e.message, false);
    }
  };

  // Shrani radar globalne nastavitve — prepiše cfgSave za 'radar' tab
  window.cfgSave = (function(_origCfgSave) {
    return async function(tab) {
      if (tab === 'radar') {
        if (_saving) return;
        _saving = true;
        _setStatus('Shranjujem radar nastavitve…');
        try {
          const pn   = parseInt(document.getElementById('cfg-radar_persistence_n').value, 10);
          const piv  = parseInt(document.getElementById('cfg-radar_poll_interval_ms').value, 10);
          const mcov = parseInt(document.getElementById('cfg-radar_max_consec_overflows').value, 10);

          if (isNaN(pn)   || pn   < 0  || pn   > 10)
            { _setStatus('Persistence mora biti 0–10', false); return; }
          if (isNaN(piv)  || piv  < 10 || piv  > 100)
            { _setStatus('Polling interval mora biti 10–100 ms', false); return; }
          if (isNaN(mcov) || mcov < 1  || mcov > 100)
            { _setStatus('Max overflowi mora biti 1–100', false); return; }

          await api.post('/api/radar/config', {
            persistence_n:        pn,
            poll_interval_ms:     piv,
            max_consec_overflows: mcov
          });
          _setStatus('Radar nastavitve shranjene ✓', true);
        } catch(e) {
          _setStatus('Napaka: ' + e.message, false);
        } finally {
          _saving = false;
          const btn = document.getElementById('save-btn-radar');
          if (btn) { btn.disabled = false; btn.textContent = 'Shrani'; }
        }
        return;
      }
      return _origCfgSave(tab);
    };
  })(window.cfgSave);

  // ── Init ────────────────────────────────────────────────
  window.page_settings = async function () {
    if (!document.getElementById('cfg-timeout_ssr1_s')) _render();
    _setStatus('Nalagam…');
    try {
      _cfg = await api.get('/api/config');
      _fill(_cfg);
      _setStatus('Naloženo iz naprave');
    } catch(e) {
      _setStatus('Napaka pri nalaganju: ' + e.message, false);
    }
  };
})();
