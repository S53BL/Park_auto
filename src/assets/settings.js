// ============================================================
// settings.js — Stran: Nastavitve  (v3 — Party Mode v2)
// 5 tabi: Osvetlitev · LED animacije · Identifikacija · Radar · Party
// GET /api/config ob nalaganju, POST /api/config ob shranjevanju
// GET+POST /api/radar za radar tab
// GET /api/party/status|slots|schedules, POST /api/party/config|slots|schedules za party tab
// ============================================================

(function () {
  const DIV = 'page-settings';
  let _cfg      = null;
  let _radarCfg = null;
  let _saving   = false;

  // ── Party stanje ─────────────────────────────────────────
  let _partyStatus    = null;   // GET /api/party/status
  let _partySlots     = null;   // GET /api/party/slots
  let _partySchedules = null;   // GET /api/party/schedules
  let _wledEffects    = null;   // iz WLED /json/effects direktno
  let _wledIp         = '';
  const _PCLRS = [
    {hex:'#ffffff',lbl:'Bela'    }, {hex:'#ffa040',lbl:'Topla'    },
    {hex:'#ffff00',lbl:'Rumena'  }, {hex:'#ff0000',lbl:'Rdeča'    },
    {hex:'#00ff00',lbl:'Zelena'  }, {hex:'#0000ff',lbl:'Modra'    },
    {hex:'#aa00ff',lbl:'Vijolična'}
  ];

  // ── Pomožni builder-ji ───────────────────────────────────
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

  // Stil za party sub-tab gumb
  function _pstyl(active) {
    return 'flex:1;padding:9px 4px;font-size:11px;background:none;border:none;' +
      'border-bottom:2px solid ' + (active ? 'var(--accent)' : 'transparent') + ';' +
      'color:' + (active ? 'var(--accent)' : 'var(--text2)') + ';' +
      'cursor:pointer;white-space:nowrap;font-family:var(--font)';
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
  <button class="tab-btn"        id="stab-party" onclick="cfgTab('party')">Party</button>
</div>

<!-- ── TAB: OSVETLITEV ──────────────────────── -->
<div class="tab-panel active card" id="spanel-light">
  ${_field('timeout_ssr1_s',      'Timeout SSR1',           's',     180, 'Čas brez gibanja pred ugasnitvijo glavne luči (SSR1)', 1)}
  ${_field('manual_extend_min',   'Podaljšek SSR1',         'min',    10, 'V auto načinu: dotik podaljša za ta čas. V ročnem: čas vklopa.', 1)}
  ${_field('antiforgot_ssr2_min', 'Anti-forget SSR2',       'min',     5, 'Samodejni izklop LED panelov ob odsotnosti gibanja', 1)}
  ${_field('antiforgot_ssr3_min', 'Anti-forget SSR3',       'min',     5, 'Samodejni izklop reflektorja ob odsotnosti gibanja', 1)}
  ${_toggle('ssr2_auto_night',    'SSR2 sledi SSR1 (slave)', true,       'SSR2 (LED paneli) sledi SSR1 v avtomatiki. Ročno sta vedno neodvisna.')}
  <div class="section-label mt16">DND — Ne moti (časovno okno)</div>
  <div class="form-hint mt4 mb8">Med DND urami: zmanjšan brightness ob prižigu, opcijsko brez SSR2. Zahteva NTP sinhronizacijo.</div>
  <div class="flex-row" style="gap:12px;align-items:flex-end">
    ${_field('dnd_start_h', 'DND začetek', 'h', 22, 'Začetek tihega okna (ura 0–23)', 1)}
    ${_field('dnd_end_h',   'DND konec',   'h',  6, 'Konec tihega okna (ura 0–23)', 1)}
  </div>
  ${_toggle('ssr2_dnd_disable',   'SSR2 izklopi v DND',     false,      'Med DND urami SSR2 ne sledi SSR1 — samo ročno')}
  ${_field('brightness_night',    'Brightness v DND',        '0–255', 120, 'Znižana svetlost LED matrike ob prižigu med DND urami', 1)}
  <div class="section-label mt16">Hystereza noč / dan (BH1750)</div>
  ${_field('lux_threshold',       'Prag NOČ (lux)',          'lx',     40, 'Pod to vrednostjo → noč (BH1750)', 1)}
  ${_field('lux_day',             'Prag DAN (lux)',          'lx',     70, 'Nad to vrednostjo → dan (mora biti > prag noč)', 1)}
  <div class="section-label mt16">SSR gumb labeli (LCD zaslon)</div>
  <div class="form-hint mt8">Dve vrstici — vsaka max 11 znakov. Vrstica 2 je neobvezna.</div>
  ${[0,1,2,3].map(i => `
  <div class="form-row">
    <div class="form-label">SSR${i+1}</div>
    <input class="form-input" id="cfg-ssr_lbl_${i}_a" type="text" maxlength="11" placeholder="vrstica 1" style="width:88px">
    <input class="form-input" id="cfg-ssr_lbl_${i}_b" type="text" maxlength="11" placeholder="vrstica 2" style="width:88px">
  </div>`).join('')}
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
  ${_field('dtw_threshold',       'DTW prag prepoznave',     '',      18, 'Razdalja < prag → obstoječ model; ≥ prag → nov model', 0.5)}
  ${_field('sakoe_radius',        'Sakoe-Chiba radius',      'točke', 15, 'Širina diagonalnega pasu DTW matrike (večji = počasneje)', 1)}
  <div class="section-label mt16">Profil vozila</div>
  ${_field('min_profile_points',  'Min. točk profila',       '',      25, 'Profil z manj točkami se zavrže (pešec, lažni prihod)', 1)}
  ${_field('normalize_points',    'Normalizacija N točk',    '',      80, 'Ciljna dolžina profila po interpolaciji pred DTW', 1)}
  ${_field('delta_filter_mm',     'Δ-filter H razdalja',     'mm',    15, 'Min. sprememba H za zajem nove točke (odfiltrira šum)', 1)}
  <div class="section-label mt16">TOF fazni avtomat</div>
  ${_field('phase_confirm_cm',    'Faza 1→2 prag',           'cm',   350, 'H razdalja pri kateri potrdimo aktivno parkirno mesto', 10)}
  ${_field('stability_s',         'Stabilnost za zaključek', 's',    1.5, 'H mora biti stabilen ta čas preden se sproži TOF_PROFILE_READY', 0.1)}
  ${_saveBtn('ident')}
</div>

<!-- ── TAB: PARTY / WLED ────────────────────── -->
<div class="tab-panel card" id="spanel-party">
  <!-- Sub-tab navigacija -->
  <div style="display:flex;border-bottom:1px solid var(--border);margin:-16px -16px 14px -16px;overflow-x:auto">
    <button id="ptab-control"   onclick="partySubTab('control')"   style="${_pstyl(true)}" >Upravljanje</button>
    <button id="ptab-slots"     onclick="partySubTab('slots')"     style="${_pstyl(false)}">Sloti</button>
    <button id="ptab-schedules" onclick="partySubTab('schedules')" style="${_pstyl(false)}">Urniki</button>
    <button id="ptab-cfg"       onclick="partySubTab('cfg')"       style="${_pstyl(false)}">Nastavitve</button>
  </div>

  <!-- Panel A: Upravljanje -->
  <div id="ppanel-control">
    <div id="party-ctrl"><div class="text-dim text-tiny" style="padding:8px">Nalagam…</div></div>
  </div>

  <!-- Panel B: Sloti -->
  <div id="ppanel-slots" style="display:none">
    <div class="flex-row flex-between mb8" style="flex-wrap:wrap;gap:6px">
      <span class="text-dim text-tiny" id="wled-fx-status"></span>
      <div style="display:flex;gap:6px">
        <a id="wled-adv-link" href="#" target="_blank" class="btn btn-sm"
           style="text-decoration:none">WLED UI ↗</a>
        <button class="btn btn-sm" onclick="partyLoadWledEffects()">Naloži efekte iz WLED</button>
      </div>
    </div>
    <div id="party-slots-list">
      <div class="text-dim text-tiny" style="padding:8px">Nalagam slote…</div>
    </div>
  </div>

  <!-- Panel C: Urniki -->
  <div id="ppanel-schedules" style="display:none">
    <div class="flex-row flex-end mb12">
      <button class="btn btn-sm btn-primary" onclick="partyNewSchedule()">+ Nov urnik</button>
    </div>
    <div id="party-sched-list">
      <div class="text-dim text-tiny" style="padding:8px">Nalagam urnike…</div>
    </div>
  </div>

  <!-- Panel D: Nastavitve -->
  <div id="ppanel-cfg" style="display:none">
    <div class="section-label">WLED Party ESP</div>
    <div class="form-row">
      <div class="flex-col">
        <div class="form-label">IP naslov Party ESP</div>
        <div class="form-hint">IP WLED naprave (npr. 192.168.2.171)</div>
      </div>
      <input class="form-input" type="text" id="cfg-wled_ip"
             autocomplete="off" placeholder="192.168.2.171"
             style="width:155px;text-align:right">
    </div>
    ${_field('party_resume_delay_s', 'Resume zamik', 's', 30,
             'Čas mirovanja pred nadaljevanjem party po prekinitvi (5–300 s)', 1)}
    ${_saveBtn('party')}
  </div>
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
    const lbls = (cfg.light || {}).ssr_labels || [];
    lbls.forEach((lbl, i) => {
      const parts = lbl.split('\n');
      const a = document.getElementById('cfg-ssr_lbl_' + i + '_a');
      const b = document.getElementById('cfg-ssr_lbl_' + i + '_b');
      if (a) a.value = parts[0] || '';
      if (b) b.value = parts[1] || '';
    });
    const lightFlat = Object.assign({}, cfg.light || {});
    delete lightFlat.ssr_labels;
    const flat = Object.assign({}, lightFlat, cfg.led || {}, cfg.ident || {});
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
              'ssr2_auto_night','dnd_start_h','dnd_end_h','ssr2_dnd_disable',
              'brightness_night','lux_threshold','lux_day'],
      led:   ['fill_speed_ms','unfill_speed_ms','fade_duration_ms','target_brightness',
              'ssr2_delay_ms','pa_thresh1_mm','pa_thresh2_mm','pa_thresh3_mm',
              'pa_stability_s','photocell_timer_min','clock_duration_s'],
      ident: ['dtw_threshold','sakoe_radius','min_profile_points','normalize_points',
              'delta_filter_mm','phase_confirm_cm','stability_s']
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
    if (tab === 'light') {
      obj.ssr_labels = [0,1,2,3].map(i => {
        const a = document.getElementById('cfg-ssr_lbl_' + i + '_a');
        const b = document.getElementById('cfg-ssr_lbl_' + i + '_b');
        const v1 = a ? a.value.trim() : '';
        const v2 = b ? b.value.trim() : '';
        return v2 ? v1 + '\n' + v2 : (v1 || ('SSR' + (i + 1)));
      });
    }
    return obj;
  }

  // ── Status sporočilo ─────────────────────────────────────
  function _setStatus(msg, ok) {
    const el = document.getElementById('cfg-status');
    if (!el) return;
    el.textContent = msg;
    el.style.color = ok === true
      ? 'var(--green)'
      : ok === false
        ? 'var(--red)'
        : 'var(--text3)';
  }

  // ── Globalne funkcije (klicane iz HTML) ──────────────────
  window.cfgTab = function(tab) {
    document.querySelectorAll('#page-settings .tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('#page-settings .tab-panel').forEach(p => p.classList.remove('active'));
    document.getElementById('stab-' + tab).classList.add('active');
    document.getElementById('spanel-' + tab).classList.add('active');
    if (tab === 'radar' && !_radarCfg) _loadRadar();
    if (tab === 'party') _loadParty();
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

  // ── Radar: naloži in shrani ──────────────────────────────
  async function _loadRadar() {
    try {
      _radarCfg = await api.get('/api/radar');
      _renderRadarSensors(_radarCfg);
      const pn  = document.getElementById('cfg-radar_persistence_n');
      if (pn && _radarCfg.persistence_n !== undefined) pn.value = _radarCfg.persistence_n;
      const piv = document.getElementById('cfg-radar_poll_interval_ms');
      if (piv && _radarCfg.poll_interval_ms !== undefined) piv.value = _radarCfg.poll_interval_ms;
      const mcov = document.getElementById('cfg-radar_max_consec_overflows');
      if (mcov && _radarCfg.max_consec_overflows !== undefined) mcov.value = _radarCfg.max_consec_overflows;
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
      stEl.style.color = ok === true ? 'var(--green)' : ok === false ? 'var(--red)' : 'var(--text3)';
    };
    setS('Shranjujem…');
    try {
      const body = {
        sensor:      sensorIdx,
        max_dist:    parseInt(document.getElementById('radar-'+sensorIdx+'-max_dist').value,   10),
        move_sens:   parseInt(document.getElementById('radar-'+sensorIdx+'-move_sens').value,  10),
        static_sens: parseInt(document.getElementById('radar-'+sensorIdx+'-static_sens').value,10),
        unmanned_s:  parseInt(document.getElementById('radar-'+sensorIdx+'-unmanned_s').value, 10)
      };
      const r = await api.post('/api/radar/config', body);
      if (r.warn) {
        setS('⚠ ' + r.warn, false);
      } else {
        setS('Shranjeno ✓', true);
        _radarCfg = await api.get('/api/radar');
        _renderRadarSensors(_radarCfg);
        const pn  = document.getElementById('cfg-radar_persistence_n');
        if (pn && _radarCfg.persistence_n !== undefined) pn.value = _radarCfg.persistence_n;
        const piv = document.getElementById('cfg-radar_poll_interval_ms');
        if (piv && _radarCfg.poll_interval_ms !== undefined) piv.value = _radarCfg.poll_interval_ms;
        const mcov = document.getElementById('cfg-radar_max_consec_overflows');
        if (mcov && _radarCfg.max_consec_overflows !== undefined) mcov.value = _radarCfg.max_consec_overflows;
      }
    } catch(e) {
      setS('Napaka: ' + e.message, false);
    }
  };

  // ── cfgSave override za party in radar ────────────────────
  window.cfgSave = (function(_origCfgSave) {
    return async function(tab) {
      if (tab === 'party') {
        if (_saving) return;
        _saving = true;
        const btn = document.getElementById('save-btn-party');
        if (btn) { btn.disabled = true; btn.textContent = 'Shranjujem…'; }
        _setStatus('Shranjujem…');
        try {
          const ip = (document.getElementById('cfg-wled_ip')?.value || '').trim();
          const rd = parseInt(document.getElementById('cfg-party_resume_delay_s')?.value || '30', 10);
          const body = {};
          if (ip) body.wled_ip = ip;
          if (!isNaN(rd) && rd >= 5 && rd <= 300) body.resume_delay_s = rd;
          if (!Object.keys(body).length) {
            _setStatus('Ni sprememb', false); return;
          }
          await api.post('/api/party/config', body);
          _setStatus('Party nastavitve shranjene ✓', true);
          _wledIp = ip || _wledIp;
          const advLink = document.getElementById('wled-adv-link');
          if (advLink && _wledIp) advLink.href = 'http://' + _wledIp;
        } catch(e) {
          _setStatus('Napaka: ' + e.message, false);
        } finally {
          _saving = false;
          if (btn) { btn.disabled = false; btn.textContent = 'Shrani'; }
        }
        return;
      }
      if (tab === 'radar') {
        if (_saving) return;
        _saving = true;
        _setStatus('Shranjujem persistence…');
        try {
          const pn   = parseInt(document.getElementById('cfg-radar_persistence_n').value, 10);
          const piv  = parseInt(document.getElementById('cfg-radar_poll_interval_ms').value, 10);
          const mcov = parseInt(document.getElementById('cfg-radar_max_consec_overflows').value, 10);
          if (isNaN(pn)   || pn   < 0  || pn   > 10)  { _setStatus('Persistence mora biti 0–10', false);         return; }
          if (isNaN(piv)  || piv  < 10 || piv  > 100)  { _setStatus('Polling interval mora biti 10–100 ms', false); return; }
          if (isNaN(mcov) || mcov < 1  || mcov > 100)  { _setStatus('Max overflowi mora biti 1–100', false);         return; }
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
        }
        return;
      }
      return _origCfgSave(tab);
    };
  })(window.cfgSave);

  // ── Party: sub-tab preklop ───────────────────────────────
  window.partySubTab = function(name) {
    const tabs = ['control','slots','schedules','cfg'];
    tabs.forEach(t => {
      const btn = document.getElementById('ptab-' + t);
      const pnl = document.getElementById('ppanel-' + t);
      if (btn) btn.style.cssText = _pstyl(t === name);
      if (pnl) pnl.style.display = t === name ? '' : 'none';
    });
    if (name === 'slots') {
      if (!_partySlots) _loadPartySlots();
      else _renderPartySlots(_partySlots);
    }
    if (name === 'schedules') {
      if (!_partySchedules) _loadPartySchedules();
      else _renderPartySchedules(_partySchedules);
    }
  };

  // ── Panel A: Upravljanje ─────────────────────────────────
  function _renderPartyControl(status, slots) {
    const el = document.getElementById('party-ctrl');
    if (!el) return;
    const partyOn  = !!(status && status.party_on);
    const suspended = !!(status && status.suspended);
    const activeSl = (status && status.active_slot !== undefined) ? status.active_slot : 0xFF;
    const slotsArr = (slots && slots.slots) ? slots.slots : [];

    el.innerHTML = `
<div class="form-row">
  <div class="flex-col">
    <div class="form-label">Party Mode</div>
    ${suspended
      ? '<div class="form-hint" style="color:var(--amber)">&#9208; Prekinjen — čakam mirovanje</div>'
      : (partyOn ? '<div class="form-hint" style="color:var(--green)">Aktiven</div>' : '')}
  </div>
  <label class="form-toggle">
    <input type="checkbox" id="party-toggle" ${partyOn ? 'checked' : ''}
           onchange="partyToggle(this.checked)">
    <span class="form-toggle-track"></span>
  </label>
</div>

<div class="form-row" style="margin-top:10px">
  <div class="flex-col">
    <div class="form-label">Party prioriteta</div>
    <div class="form-hint">Ko ON: gibanje ne prekinja party — SSR luči delujejo neodvisno</div>
  </div>
  <button id="party-prio-btn" onclick="partyTogglePriority()"
    style="padding:6px 10px;border-radius:6px;flex-shrink:0;
           border:1px solid ${(status && status.priority) ? 'var(--amber)' : 'var(--border2)'};
           background:${(status && status.priority) ? 'rgba(245,158,11,0.12)' : 'var(--bg3)'};
           color:${(status && status.priority) ? 'var(--amber)' : 'var(--text2)'};
           cursor:pointer;font-family:var(--font);font-size:11px;white-space:nowrap">
    ${(status && status.priority) ? '&#9733; PRIO ON' : '&#9734; PRIO OFF'}
  </button>
</div>

<div class="section-label mt16">Sloti — klik aktivira</div>
<div class="grid-3" style="gap:5px;margin-bottom:14px">
  ${slotsArr.map((sl, i) => {
    const isActive = i === activeSl && partyOn && !suspended;
    const dis = !sl.enabled;
    return `<button onclick="partyActivateSlot(${i})"
      style="padding:7px 3px;font-size:11px;border-radius:6px;border:1px solid ${isActive ? 'var(--accent)' : 'var(--border2)'};
             background:${isActive ? 'var(--accent2)' : 'var(--bg3)'};color:${dis ? 'var(--text3)' : 'var(--text)'};
             cursor:${dis ? 'default' : 'pointer'};white-space:nowrap;overflow:hidden;text-overflow:ellipsis"
      ${dis ? 'disabled' : ''}>${sl.name || ('Slot ' + i)}</button>`;
  }).join('')}
</div>

<div class="section-label">Barva</div>
<div style="display:flex;gap:9px;flex-wrap:wrap;margin-bottom:14px">
  ${_PCLRS.map(c => `
    <button title="${c.lbl}" onclick="partySetColor('${c.hex}')"
      style="width:30px;height:30px;border-radius:50%;background:${c.hex};
             border:2px solid var(--border2);cursor:pointer;padding:0;flex-shrink:0"></button>`
  ).join('')}
</div>

<div class="form-row">
  <div class="form-label">Svetlost</div>
  <div style="display:flex;align-items:center;gap:8px;flex:1;justify-content:flex-end">
    <input type="range" min="0" max="255" value="191" id="party-bri"
           style="width:110px"
           oninput="document.getElementById('party-bri-v').textContent=this.value"
           onchange="partySetBrightness(this.value)">
    <span class="text-dim" id="party-bri-v" style="min-width:26px;text-align:right">191</span>
  </div>
</div>

<div class="form-row">
  <div class="form-label">Hitrost</div>
  <div style="display:flex;align-items:center;gap:8px;flex:1;justify-content:flex-end">
    <input type="range" min="0" max="255" value="128" id="party-spd"
           style="width:110px"
           oninput="document.getElementById('party-spd-v').textContent=this.value"
           onchange="partySetSpeed(this.value)">
    <span class="text-dim" id="party-spd-v" style="min-width:26px;text-align:right">128</span>
  </div>
</div>`;
  }

  // ── Panel B: Sloti ───────────────────────────────────────
  function _wledFxOptions(curFxId) {
    if (!_wledEffects || !_wledEffects.length)
      return `<option value="${curFxId}" selected>fx=${curFxId} (naloži efekte iz WLED)</option>`;
    return _wledEffects.map((e, i) =>
      `<option value="${i}" ${i === curFxId ? 'selected' : ''}>${i}: ${e.name}</option>`
    ).join('');
  }

  function _hex6(n) {
    return '#' + (n >>> 0).toString(16).padStart(6, '0');
  }

  function _renderPartySlots(slotsData) {
    const el = document.getElementById('party-slots-list');
    if (!el) return;
    const arr = (slotsData && slotsData.slots) ? slotsData.slots : [];

    el.innerHTML = arr.map((sl, i) => `
<div class="card" style="margin-bottom:10px;border-color:var(--border2)">
  <div class="flex-row flex-between mb8">
    <div class="card-title" style="margin:0;font-size:12px">Slot ${i}</div>
    <label class="form-toggle" style="transform:scale(0.8);transform-origin:right center">
      <input type="checkbox" id="slot-${i}-en" ${sl.enabled ? 'checked' : ''}>
      <span class="form-toggle-track"></span>
    </label>
  </div>
  <div class="grid-2" style="gap:8px;margin-bottom:8px">
    <div>
      <div class="form-hint">Ime (max 15)</div>
      <input class="form-input w100" type="text" maxlength="15"
             id="slot-${i}-name" value="${sl.name || ''}">
    </div>
    <div>
      <div class="form-hint">Efekt</div>
      <select class="form-input w100" id="slot-${i}-fx"
              style="padding-right:4px">
        ${_wledFxOptions(sl.fx_id)}
      </select>
    </div>
  </div>
  <div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:8px">
    <div>
      <div class="form-hint">Barva (hex)</div>
      <div style="display:flex;gap:4px;align-items:center">
        <input type="color" id="slot-${i}-cpick" value="${_hex6(sl.color_rgb)}"
               oninput="document.getElementById('slot-${i}-chex').value=this.value"
               style="width:30px;height:30px;border:none;background:none;padding:0;cursor:pointer">
        <input class="form-input" type="text" maxlength="7" placeholder="#000000"
               id="slot-${i}-chex" value="${_hex6(sl.color_rgb)}"
               oninput="document.getElementById('slot-${i}-cpick').value=this.value.length>=7?this.value:'#000000'"
               style="width:72px;font-family:monospace;font-size:11px">
      </div>
      <div class="form-hint" style="margin-top:2px">#000000 = auto</div>
    </div>
    <div>
      <div class="form-hint">Svetlost</div>
      <input class="form-input w100" type="number" min="0" max="255" step="1"
             id="slot-${i}-bri" value="${sl.brightness}">
    </div>
    <div>
      <div class="form-hint">Hitrost</div>
      <input class="form-input w100" type="number" min="0" max="255" step="1"
             id="slot-${i}-spd" value="${sl.speed}">
    </div>
  </div>
  <div class="flex-row flex-end">
    <span class="text-dim text-tiny" id="slot-${i}-st"></span>
    <button class="btn btn-sm btn-primary" style="margin-left:8px"
            onclick="partySaveSlot(${i})">Shrani slot ${i}</button>
  </div>
</div>`).join('');
  }

  // ── Panel C: Urniki ──────────────────────────────────────
  function _fmt2(n) { return String(n || 0).padStart(2, '0'); }

  function _schedRow(s, i) {
    const slotsArr = (_partySlots && _partySlots.slots) ? _partySlots.slots : [];
    const slotOpts = slotsArr.map((sl, idx) =>
      `<option value="${idx}" ${idx === s.slot_idx ? 'selected' : ''}>${idx}: ${sl.name || 'Slot'+idx}</option>`
    ).join('');

    return `
<div class="card" style="margin-bottom:10px;border-color:var(--border2)" id="scard-${i}">
  <div class="flex-row flex-between mb8" style="flex-wrap:wrap;gap:6px">
    <div style="display:flex;gap:6px;flex:1;min-width:0">
      <input class="form-input" type="text" maxlength="23" placeholder="Ime urnika"
             id="sched-${i}-name" value="${s.name || ''}" style="flex:1;min-width:0">
      <select class="form-input" id="sched-${i}-slot" style="width:110px;flex-shrink:0">
        ${slotOpts}
      </select>
    </div>
    <label class="form-toggle" style="transform:scale(0.8);transform-origin:right center;flex-shrink:0">
      <input type="checkbox" id="sched-${i}-en" ${s.enabled ? 'checked' : ''}>
      <span class="form-toggle-track"></span>
    </label>
  </div>

  <div class="grid-2" style="gap:8px;margin-bottom:8px">
    <div>
      <div class="form-hint">Od (MM-DD)</div>
      <input class="form-input w100" type="text" maxlength="5" placeholder="12-01"
             id="sched-${i}-from" value="${_fmt2(s.from_month)}-${_fmt2(s.from_day)}">
    </div>
    <div>
      <div class="form-hint">Do (MM-DD)</div>
      <input class="form-input w100" type="text" maxlength="5" placeholder="01-06"
             id="sched-${i}-to" value="${_fmt2(s.to_month)}-${_fmt2(s.to_day)}">
    </div>
  </div>

  <div class="grid-2" style="gap:8px;margin-bottom:8px">
    <div>
      <div class="form-hint" style="margin-bottom:4px">Vklop</div>
      <div style="display:flex;gap:8px;margin-bottom:4px">
        <label style="display:flex;gap:3px;align-items:center;cursor:pointer;font-size:11px">
          <input type="radio" name="son-${i}" value="time" ${!s.use_lux_on?'checked':''}
                 onchange="_schedOnType(${i})"> Ura
        </label>
        <label style="display:flex;gap:3px;align-items:center;cursor:pointer;font-size:11px">
          <input type="radio" name="son-${i}" value="lux" ${s.use_lux_on?'checked':''}
                 onchange="_schedOnType(${i})"> Lux
        </label>
      </div>
      <div id="son-time-${i}" style="${s.use_lux_on?'display:none':''}">
        <input class="form-input w100" type="time" id="sched-${i}-on-t"
               value="${_fmt2(s.time_on_h)}:${_fmt2(s.time_on_m)}">
      </div>
      <div id="son-lux-${i}" style="${!s.use_lux_on?'display:none':''}">
        <input class="form-input w100" type="number" min="0" max="100000"
               id="sched-${i}-on-lux" value="${s.lux_on || 50}" placeholder="lux">
        <div class="form-hint" style="margin-top:2px">Vklopi ko pade pod</div>
      </div>
    </div>
    <div>
      <div class="form-hint" style="margin-bottom:4px">Izklop</div>
      <div style="display:flex;gap:8px;margin-bottom:4px">
        <label style="display:flex;gap:3px;align-items:center;cursor:pointer;font-size:11px">
          <input type="radio" name="soff-${i}" value="time" ${!s.use_lux_off?'checked':''}
                 onchange="_schedOffType(${i})"> Ura
        </label>
        <label style="display:flex;gap:3px;align-items:center;cursor:pointer;font-size:11px">
          <input type="radio" name="soff-${i}" value="lux" ${s.use_lux_off?'checked':''}
                 onchange="_schedOffType(${i})"> Lux
        </label>
      </div>
      <div id="soff-time-${i}" style="${s.use_lux_off?'display:none':''}">
        <input class="form-input w100" type="time" id="sched-${i}-off-t"
               value="${_fmt2(s.time_off_h)}:${_fmt2(s.time_off_m)}">
      </div>
      <div id="soff-lux-${i}" style="${!s.use_lux_off?'display:none':''}">
        <input class="form-input w100" type="number" min="0" max="100000"
               id="sched-${i}-off-lux" value="${s.lux_off || 200}" placeholder="lux">
        <div class="form-hint" style="margin-top:2px">Izklopi ko dvigne nad</div>
      </div>
    </div>
  </div>

  <div class="flex-row flex-end" style="gap:6px">
    <span class="text-dim text-tiny" id="sched-${i}-st"></span>
    <button class="btn btn-sm btn-dim" onclick="partyDelSched(${i})">Izbriši</button>
    <button class="btn btn-sm btn-primary" onclick="partySaveSched(${i})">Shrani</button>
  </div>
</div>`;
  }

  function _renderPartySchedules(schedData) {
    const el = document.getElementById('party-sched-list');
    if (!el) return;
    const arr = (schedData && schedData.schedules) ? schedData.schedules : [];
    const visible = arr.filter(s => s.enabled || (s.name && s.name.length > 0));
    if (!visible.length) {
      el.innerHTML = '<div class="text-dim text-tiny" style="padding:10px">Ni urnikov. Dodaj z gumbom zgoraj.</div>';
      return;
    }
    el.innerHTML = arr.map((s, i) =>
      (s.enabled || (s.name && s.name.length > 0)) ? _schedRow(s, i) : ''
    ).join('');
  }

  // ── Party: nalaganje podatkov ────────────────────────────
  async function _loadParty() {
    try {
      _partyStatus = await api.get('/api/party/status');
      _wledIp = _partyStatus.wled_ip || '';
      // Nastavi WLED IP in resume delay v nastavitve panelu
      const ipEl = document.getElementById('cfg-wled_ip');
      if (ipEl && _wledIp) ipEl.value = _wledIp;
      const rdEl = document.getElementById('cfg-party_resume_delay_s');
      if (rdEl && _partyStatus.resume_delay_s !== undefined) rdEl.value = _partyStatus.resume_delay_s;
      // Nastavi WLED UI link
      const advLink = document.getElementById('wled-adv-link');
      if (advLink && _wledIp) advLink.href = 'http://' + _wledIp;
      // Naloži slote za control panel
      if (!_partySlots) _partySlots = await api.get('/api/party/slots');
      _renderPartyControl(_partyStatus, _partySlots);
    } catch(e) {
      const el = document.getElementById('party-ctrl');
      if (el) el.innerHTML = `<div class="text-dim text-tiny" style="padding:8px">Napaka: ${e.message}</div>`;
    }
  }

  async function _loadPartySlots() {
    try {
      _partySlots = await api.get('/api/party/slots');
      _renderPartySlots(_partySlots);
    } catch(e) {
      const el = document.getElementById('party-slots-list');
      if (el) el.innerHTML = `<div class="text-dim text-tiny" style="padding:8px">Napaka: ${e.message}</div>`;
    }
  }

  async function _loadPartySchedules() {
    try {
      _partySchedules = await api.get('/api/party/schedules');
      _renderPartySchedules(_partySchedules);
    } catch(e) {
      const el = document.getElementById('party-sched-list');
      if (el) el.innerHTML = `<div class="text-dim text-tiny" style="padding:8px">Napaka: ${e.message}</div>`;
    }
  }

  // ── Party: akcije Panel A ────────────────────────────────
  window.partyToggle = async function(on) {
    try {
      await api.post('/api/party', { on: on ? 1 : 0 });
      _partyStatus = await api.get('/api/party/status');
      _renderPartyControl(_partyStatus, _partySlots);
    } catch(e) { _setStatus('Napaka: ' + e.message, false); }
  };

  window.partyActivateSlot = async function(idx) {
    try {
      await api.post('/api/party/activate', { slot_idx: idx });
      _partyStatus = await api.get('/api/party/status');
      _renderPartyControl(_partyStatus, _partySlots);
    } catch(e) { _setStatus('Napaka: ' + e.message, false); }
  };

  window.partySetColor = async function(hex) {
    try {
      const rgb = parseInt(hex.replace('#', ''), 16);
      await api.post('/api/party/color', { color: isNaN(rgb) ? 0xFFFFFF : rgb });
    } catch(e) { /* tiho */ }
  };

  window.partySetBrightness = async function(v) {
    try { await api.post('/api/party/brightness', { value: parseInt(v, 10) }); } catch { /* tiho */ }
  };

  window.partySetSpeed = async function(v) {
    try { await api.post('/api/party/speed', { value: parseInt(v, 10) }); } catch { /* tiho */ }
  };

  window.partyTogglePriority = async function() {
    const cur = !!((_partyStatus || {}).priority);
    try {
      await api.post('/api/party/priority', { on: cur ? 0 : 1 });
      _partyStatus = await api.get('/api/party/status');
      _renderPartyControl(_partyStatus, _partySlots);
    } catch(e) { _setStatus('Napaka: ' + e.message, false); }
  };

  // ── Party: WLED efekti ───────────────────────────────────
  window.partyLoadWledEffects = async function() {
    const stEl = document.getElementById('wled-fx-status');
    const setSt = (msg, ok) => {
      if (!stEl) return;
      stEl.textContent = msg;
      stEl.style.color = ok === true ? 'var(--green)' : ok === false ? 'var(--red)' : 'var(--text3)';
    };
    setSt('Nalagam…');
    try {
      // Pridobi WLED IP
      const ip = _wledIp || (await api.get('/api/party/status')).wled_ip;
      if (!ip) throw new Error('WLED IP ni nastavljen v nastavitvah');
      const resp = await fetch('http://' + ip + '/json/effects');
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      const names = await resp.json();
      _wledEffects = names.map((name, idx) => ({ id: idx, name }));
      setSt(_wledEffects.length + ' efektov naloženih ✓', true);
      // Posodobi dropdown-e v slot listah
      if (_partySlots) _renderPartySlots(_partySlots);
    } catch(e) {
      setSt('WLED nedosegljiv: ' + e.message, false);
    }
  };

  // ── Party: shrani slot ───────────────────────────────────
  window.partySaveSlot = async function(idx) {
    const stEl = document.getElementById('slot-' + idx + '-st');
    const setSt = (msg, ok) => {
      if (!stEl) return;
      stEl.textContent = msg;
      stEl.style.color = ok === true ? 'var(--green)' : ok === false ? 'var(--red)' : 'var(--text3)';
    };
    setSt('Shranjujem…');
    try {
      const fxEl    = document.getElementById('slot-' + idx + '-fx');
      const hexStr  = (document.getElementById('slot-' + idx + '-chex')?.value || '#000000').replace('#','');
      const colorRgb = parseInt(hexStr, 16);
      const body = {
        idx,
        name:       (document.getElementById('slot-' + idx + '-name')?.value || '').trim().slice(0, 15),
        fx_id:      fxEl ? parseInt(fxEl.value, 10) : 0,
        color_rgb:  isNaN(colorRgb) ? 0 : colorRgb,
        brightness: parseInt(document.getElementById('slot-' + idx + '-bri')?.value || '191', 10),
        speed:      parseInt(document.getElementById('slot-' + idx + '-spd')?.value || '128', 10),
        enabled:    !!(document.getElementById('slot-' + idx + '-en')?.checked)
      };
      await api.post('/api/party/slots', body);
      setSt('Shranjeno ✓', true);
      _partySlots = await api.get('/api/party/slots');
    } catch(e) {
      setSt('Napaka: ' + e.message, false);
    }
  };

  // ── Party: urniki ────────────────────────────────────────
  window._schedOnType = function(i) {
    const isLux = !!document.querySelector(`input[name="son-${i}"][value="lux"]`)?.checked;
    const tEl = document.getElementById('son-time-' + i);
    const lEl = document.getElementById('son-lux-' + i);
    if (tEl) tEl.style.display = isLux ? 'none' : '';
    if (lEl) lEl.style.display = isLux ? '' : 'none';
  };

  window._schedOffType = function(i) {
    const isLux = !!document.querySelector(`input[name="soff-${i}"][value="lux"]`)?.checked;
    const tEl = document.getElementById('soff-time-' + i);
    const lEl = document.getElementById('soff-lux-' + i);
    if (tEl) tEl.style.display = isLux ? 'none' : '';
    if (lEl) lEl.style.display = isLux ? '' : 'none';
  };

  window.partySaveSched = async function(idx) {
    const stEl = document.getElementById('sched-' + idx + '-st');
    const setSt = (msg, ok) => {
      if (!stEl) return;
      stEl.textContent = msg;
      stEl.style.color = ok === true ? 'var(--green)' : ok === false ? 'var(--red)' : 'var(--text3)';
    };
    setSt('Shranjujem…');
    try {
      const fromParts = (document.getElementById('sched-'+idx+'-from')?.value || '01-01').split('-');
      const toParts   = (document.getElementById('sched-'+idx+'-to')?.value   || '12-31').split('-');
      const onT       = (document.getElementById('sched-'+idx+'-on-t')?.value || '20:00').split(':');
      const offT      = (document.getElementById('sched-'+idx+'-off-t')?.value|| '23:00').split(':');
      const useLuxOn  = !!document.querySelector(`input[name="son-${idx}"][value="lux"]`)?.checked;
      const useLuxOff = !!document.querySelector(`input[name="soff-${idx}"][value="lux"]`)?.checked;

      const body = {
        idx,
        name:        (document.getElementById('sched-'+idx+'-name')?.value || '').trim().slice(0,23),
        slot_idx:    parseInt(document.getElementById('sched-'+idx+'-slot')?.value || '0', 10),
        enabled:     !!(document.getElementById('sched-'+idx+'-en')?.checked),
        from_month:  parseInt(fromParts[0], 10) || 1,
        from_day:    parseInt(fromParts[1], 10) || 1,
        to_month:    parseInt(toParts[0], 10)   || 12,
        to_day:      parseInt(toParts[1], 10)   || 31,
        use_lux_on:  useLuxOn,
        lux_on:      parseInt(document.getElementById('sched-'+idx+'-on-lux')?.value || '50', 10),
        time_on_h:   parseInt(onT[0], 10)  || 20,
        time_on_m:   parseInt(onT[1], 10)  || 0,
        use_lux_off: useLuxOff,
        lux_off:     parseInt(document.getElementById('sched-'+idx+'-off-lux')?.value || '200', 10),
        time_off_h:  parseInt(offT[0], 10) || 23,
        time_off_m:  parseInt(offT[1], 10) || 0
      };
      await api.post('/api/party/schedules', body);
      setSt('Shranjeno ✓', true);
      _partySchedules = await api.get('/api/party/schedules');
    } catch(e) {
      setSt('Napaka: ' + e.message, false);
    }
  };

  window.partyDelSched = async function(idx) {
    if (!confirm('Izbriši urnik ' + idx + '?')) return;
    try {
      await fetch('/api/party/schedules?idx=' + idx, { method: 'DELETE' });
      _partySchedules = await api.get('/api/party/schedules');
      _renderPartySchedules(_partySchedules);
    } catch(e) {
      alert('Napaka: ' + e.message);
    }
  };

  window.partyNewSchedule = function() {
    if (!_partySchedules) { _loadPartySchedules(); return; }
    const arr = _partySchedules.schedules || [];
    // Poišči prvi prazen slot (disabled + brez imena)
    let freeIdx = arr.findIndex(s => !s.enabled && !(s.name && s.name.length));
    if (freeIdx === -1) {
      if (arr.length >= 10) { _setStatus('Maksimalno 10 urnikov!', false); return; }
      freeIdx = arr.length;
    }
    const container = document.getElementById('party-sched-list');
    if (!container) return;
    const emptyS = {
      name:'', slot_idx:0, enabled:true,
      from_month:12, from_day:1, to_month:1, to_day:6,
      use_lux_on:false, lux_on:50, time_on_h:20, time_on_m:0,
      use_lux_off:false, lux_off:200, time_off_h:23, time_off_m:0
    };
    if (container.querySelector('.text-dim')) container.innerHTML = '';
    const wrap = document.createElement('div');
    wrap.innerHTML = _schedRow(emptyS, freeIdx);
    container.prepend(wrap.firstElementChild);
    document.getElementById('sched-' + freeIdx + '-name')?.focus();
  };

  // ── Init (kliče ga router) ───────────────────────────────
  window.page_settings = async function () {
    if (!document.getElementById('cfg-timeout_ssr1_s')) _render();
    _setStatus('Nalagam…');
    try {
      _cfg = await api.get('/api/config');
      _fill(_cfg);
      _loadParty();
      _setStatus('Naloženo iz naprave');
    } catch(e) {
      _setStatus('Napaka pri nalaganju: ' + e.message, false);
    }
  };
})();
