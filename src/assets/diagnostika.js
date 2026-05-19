// ============================================================
// diagnostika.js — Stran: Diagnostika  (v4)
// Zavihek 1 "Pregled": SSR gumbi, parking gumbi, fotocelice inline
// Zavihek 2 "Radar": auto-refresh 1s, progress bar z črtice
// Kliče: /api/status/light + /api/status/sensors
//        /api/ssr (GET + POST)
//        /api/vehicles (GET) + /api/vehicles/rename + /api/vehicles/calibrate
//        /api/radar (samo Radar zavihek)
// WiFi/SD so na strani Sistem (system.js)
// ============================================================
(function () {
  const DIV = 'page-diagnostika';
  let _activeTab = 'pregled';
  let _autoOn = false;

  function _fmtUptime(ms) {
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    const d = Math.floor(h / 24);
    if (d > 0) return d + 'd ' + (h % 24) + 'h';
    if (h > 0) return h + 'h ' + (m % 60) + 'm';
    return m + 'm ' + (s % 60) + 's';
  }

  // Koščkasto skaliranje energije — enako kot v hal_display.cpp
  function _radarScale(raw, threshold) {
    if (!threshold) return raw;
    if (raw <= threshold) return Math.round(raw * 75 / threshold);
    const over = raw - threshold;
    const room = threshold >= 100 ? 1 : (100 - threshold);
    return Math.min(100, 75 + Math.round(over * 25 / room));
  }

  function _fmtDist(cm) {
    if (!cm) return '–';
    return (cm / 100).toFixed(1).replace('.', ',') + ' m';
  }

  // ── Tab logika ────────────────────────────────────────────
  window._diagTab = function(tab) {
    _activeTab = tab;
    ['pregled', 'radar'].forEach(t => {
      const btn = document.getElementById('diag-tab-' + t);
      const cnt = document.getElementById('diag-content-' + t);
      if (btn) btn.style.opacity = (t === tab) ? '1' : '0.5';
      if (cnt) cnt.style.display = (t === tab) ? '' : 'none';
    });
    if (_autoOn) {
      if (tab === 'radar') registerPoller('diagnostika', _radarRefresh, 1000);
      else                 registerPoller('diagnostika', diagRefresh,   3000);
    } else {
      clearPoller('diagnostika');
    }
  };

  // ── Auto-refresh stikalo ──────────────────────────────────
  window._diagAutoToggle = function(on) {
    _autoOn = on;
    const btn = document.getElementById('diag-refresh-btn');
    if (btn) btn.style.display = on ? 'none' : '';
    if (on) {
      if (_activeTab === 'radar') { _radarRefresh(); registerPoller('diagnostika', _radarRefresh, 1000); }
      else                        { diagRefresh();   registerPoller('diagnostika', diagRefresh,   3000); }
    } else {
      clearPoller('diagnostika');
    }
  };

  // ── SSR POST helper ───────────────────────────────────────
  window._ssrPost = async function(id, action) {
    try {
      await api.post('/api/ssr', { ssr: id, action });
      await _refreshSsr();
    } catch(e) { /* tiho — SSR refresh bo pokazal stanje */ }
  };

  async function _refreshSsr() {
    try {
      const d = await api.get('/api/ssr');
      (d.ssr || []).forEach(ssr => _applySsr(ssr));
    } catch { /* ignoriramo */ }
  }

  function _applySsr(ssr) {
    const n = ssr.id;
    const set = (id, v) => { const e = document.getElementById(id); if (e) { e.textContent = v; e.classList.remove('loading'); } };
    const stateMap = { OFF:'IZKLOP', ON_AUTO:'AUTO', ON_MANUAL:'ROČNO', DISABLED:'ONEMOG.' };
    set('diag-ssr'+n+'-state', stateMap[ssr.state_str] || ssr.state_str || '–');
    const stateEl = document.getElementById('diag-ssr'+n+'-state');
    if (stateEl) {
      stateEl.classList.remove('ok','warn','err');
      if (ssr.state_str === 'ON_AUTO' || ssr.state_str === 'ON_MANUAL') stateEl.classList.add('ok');
      else if (ssr.state_str === 'DISABLED') stateEl.classList.add('err');
    }
    set('diag-ssr'+n+'-cd', ssr.countdown_s > 0 ? '⏱ ' + fmt.countdown(ssr.countdown_s) : '');
    // Toggle gumb — onemogočen ko disabled
    const togBtn = document.getElementById('diag-ssr'+n+'-tog');
    if (togBtn) togBtn.disabled = !!ssr.disabled;
    // Dis/Enable gumb — label glede na stanje
    const disBtn = document.getElementById('diag-ssr'+n+'-dis');
    if (disBtn) {
      disBtn.textContent = ssr.disabled ? '✓ Omogoči' : '⊘ Onemogoči';
      disBtn.className   = 'btn' + (ssr.disabled ? '' : ' btn-dim');
    }
  }

  // ── Parking rename / calibrate ────────────────────────────
  window._parkRename = async function(place) {
    try {
      const d = await api.get('/api/vehicles?place=' + place);
      const model = (d.models || []).find(m => m.on_place);
      const inpId  = 'diag-park' + place + '-rename-inp';
      const rowId  = 'diag-park' + place + '-rename-row';
      const rowEl  = document.getElementById(rowId);
      if (!model) { alert('Na mestu ' + place + ' ni vozila za preimenovanje.'); return; }
      const inpEl = document.getElementById(inpId);
      if (inpEl) inpEl.value = model.name || '';
      if (rowEl) rowEl.style.display = '';
      // shrani model.id za confirm
      if (rowEl) rowEl.dataset.modelId = model.id;
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  window._parkRenameOk = async function(place) {
    const rowId = 'diag-park' + place + '-rename-row';
    const inpId = 'diag-park' + place + '-rename-inp';
    const rowEl = document.getElementById(rowId);
    const inpEl = document.getElementById(inpId);
    if (!rowEl || !inpEl) return;
    const newName = inpEl.value.trim();
    if (!newName) { alert('Ime ne sme biti prazno.'); return; }
    const modelId = rowEl.dataset.modelId;
    try {
      await api.post('/api/vehicles/rename', { place, id: modelId, name: newName });
      rowEl.style.display = 'none';
      // Posodobi metric prikaz
      const metEl = document.getElementById('diag-park' + place);
      if (metEl) metEl.textContent = newName;
    } catch(e) { alert('Napaka preименovanja: ' + e.message); }
  };

  window._parkCalibrate = async function(place) {
    if (!confirm('Mesto ' + place + ' mora biti PRAZNO.\nKalibriraš prazno stanje zdaj?')) return;
    try {
      const r = await api.post('/api/vehicles/calibrate', { place });
      alert(r.ok ? 'Kalibracija mesta ' + place + ' uspešna.' : 'Kalibracija ni uspela.');
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  // ── Render (enkrat ob prvem obisku strani) ────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Diagnostika</h1>
  <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
    <span class="text-dim text-tiny" id="diag-ts">–</span>
    <span class="text-dim text-tiny">Auto</span>
    <label class="form-toggle">
      <input type="checkbox" id="diag-auto-chk" onchange="_diagAutoToggle(this.checked)">
      <span class="form-toggle-track"></span>
    </label>
    <button class="btn" id="diag-refresh-btn" onclick="diagRefresh()">↻ Osveži</button>
  </div>
</div>

<!-- Tab gumbi -->
<div style="display:flex;gap:6px;margin-bottom:14px">
  <button class="btn" id="diag-tab-pregled" onclick="_diagTab('pregled')" style="opacity:1">Pregled</button>
  <button class="btn" id="diag-tab-radar"   onclick="_diagTab('radar')"   style="opacity:0.5">Radar</button>
</div>

<!-- ═══ ZAVIHEK 1: PREGLED ══════════════════════════════════ -->
<div id="diag-content-pregled">

<!-- SSR -->
<div class="section-label">Osvetlitev — SSR</div>
<div class="grid-4" id="diag-ssr">
  ${[1,2,3,4].map(i => `
  <div class="card">
    <div class="card-title">SSR${i}</div>
    <div class="metric loading" id="diag-ssr${i}-state">–</div>
    <div class="text-dim text-tiny mt8" id="diag-ssr${i}-cd"></div>
    <div style="display:flex;gap:4px;margin-top:8px">
      <button class="btn" style="flex:1;font-size:10px;padding:3px 4px"
              id="diag-ssr${i}-tog" onclick="_ssrPost(${i},'toggle')">↑ ON/OFF</button>
      <button class="btn btn-dim" style="flex:1;font-size:10px;padding:3px 4px"
              id="diag-ssr${i}-dis" onclick="_ssrPost(${i}, this.textContent.includes('Onemogoči')?'disable':'enable')">⊘ Onemogoči</button>
    </div>
  </div>`).join('')}
</div>

<!-- RAMPA / VRATA / SVETLOBA + FOTOCELICE / PARKING -->
<div class="grid-3 mt12">
  <div class="card">
    <div class="card-title">Rampa / Vrata / Svetloba</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Rampa</span>      <span class="kv-val" id="diag-ramp">–</span></div>
      <div class="kv-row"><span class="kv-key">Vrata</span>      <span class="kv-val" id="diag-door">–</span></div>
      <div class="kv-row"><span class="kv-key">Svetloba</span>   <span class="kv-val text-mono" id="diag-lux-day">–</span></div>
      <div class="kv-row"><span class="kv-key">FC zunanja</span> <span class="kv-val" id="diag-cell-zun">–</span></div>
      <div class="kv-row"><span class="kv-key">FC notranja</span><span class="kv-val" id="diag-cell-not">–</span></div>
    </div>
  </div>
  ${['A','B'].map(m => `
  <div class="card">
    <div class="card-title">Parking ${m}</div>
    <div class="metric loading" id="diag-park${m}">–</div>
    <div class="text-dim text-tiny mt8" id="diag-park${m}-phase">–</div>
    <!-- Rename inline -->
    <div id="diag-park${m}-rename-row" style="display:none;margin-top:8px" data-model-id="">
      <input type="text" id="diag-park${m}-rename-inp" class="form-input" style="width:100%;margin-bottom:4px" placeholder="Novo ime">
      <div style="display:flex;gap:4px">
        <button class="btn" style="flex:1;font-size:10px" onclick="_parkRenameOk('${m}')">OK</button>
        <button class="btn btn-dim" style="flex:1;font-size:10px" onclick="document.getElementById('diag-park${m}-rename-row').style.display='none'">Prekliči</button>
      </div>
    </div>
    <div style="display:flex;gap:4px;margin-top:8px">
      <button class="btn" style="flex:1;font-size:10px;padding:3px 4px" onclick="_parkRename('${m}')">✎ Preimen.</button>
      <button class="btn btn-dim" style="flex:1;font-size:10px;padding:3px 4px" onclick="_parkCalibrate('${m}')">⊙ Kalibr.</button>
    </div>
  </div>`).join('')}
</div>

<!-- TOF -->
<div class="section-label mt20">TOF senzorji</div>
<div class="card">
  <table class="tbl">
    <thead><tr><th>Senzor</th><th>Razdalja</th><th>Status</th><th class="right">Napake</th></tr></thead>
    <tbody id="diag-tof-body"><tr><td colspan="4" class="empty-state">Pritisni Osveži</td></tr></tbody>
  </table>
</div>

</div><!-- /diag-content-pregled -->

<!-- ═══ ZAVIHEK 2: RADAR ════════════════════════════════════ -->
<div id="diag-content-radar" style="display:none">

<div class="section-label" style="margin-bottom:8px">Radar senzorji
  <span class="text-dim text-tiny" id="diag-radar-ts" style="margin-left:10px">–</span>
</div>
<div class="grid-4" id="diag-radar-grid">
  ${['Vhod','Cesta L','Cesta D','Garaža'].map((name, i) => `
  <div class="card" id="diag-rad-card${i}">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px">
      <div class="card-title" style="margin:0">${name}</div>
      <span class="badge" id="diag-rad-badge${i}">–</span>
    </div>
    <div class="metric loading" id="diag-rad-state${i}" style="font-size:14px;margin-bottom:6px">–</div>

    <!-- Progress bar z črtice pri 75% -->
    <div style="position:relative;height:8px;background:#1F2937;border-radius:4px;margin:4px 0 2px">
      <div id="diag-rad-bar${i}" style="position:absolute;left:0;top:0;height:100%;width:0%;background:#334155;border-radius:4px;transition:width 0.3s"></div>
      <div style="position:absolute;left:75%;top:-3px;bottom:-3px;width:2px;background:#fff;border-radius:1px;opacity:0.7"></div>
    </div>
    <div class="text-dim text-tiny" id="diag-rad-bar-lbl${i}" style="margin-bottom:8px">–</div>

    <!-- Razdalja -->
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Gib. razd.</span><span class="kv-val text-mono" id="diag-rad-mdist${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Stat. razd.</span><span class="kv-val text-mono" id="diag-rad-sdist${i}">–</span></div>
    </div>

    <!-- Nastavitve -->
    <div style="font-size:10px;color:#64748B;margin:8px 0 3px;text-transform:uppercase;letter-spacing:.05em">Nastavitve</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Max razd.</span><span class="kv-val text-mono" id="diag-rad-maxd${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Prag gib.</span><span class="kv-val text-mono" id="diag-rad-ms${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Prag stat.</span><span class="kv-val text-mono" id="diag-rad-ss${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Timeout</span><span class="kv-val text-mono" id="diag-rad-us${i}">–</span></div>
    </div>

    <!-- Statistike -->
    <div style="font-size:10px;color:#64748B;margin:8px 0 3px;text-transform:uppercase;letter-spacing:.05em">Statistike</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Okvirji</span><span class="kv-val text-mono" id="diag-rad-frm${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Napake</span><span class="kv-val text-mono" id="diag-rad-err${i}">–</span></div>
      <div class="kv-row"><span class="kv-key">Config</span><span class="kv-val text-mono" id="diag-rad-cfg${i}">–</span></div>
    </div>
  </div>`).join('')}
</div>

</div><!-- /diag-content-radar -->
`;
  }

  // ── Update: Pregled zavihek ───────────────────────────────
  function _updateLight(d) {
    const set = (id, v) => { const e = document.getElementById(id); if (e) { e.textContent = v; e.classList.remove('loading'); } };
    const cls = (id, c) => { const e = document.getElementById(id); if (e) e.className = 'metric ' + c; };

    // SSR — prikaz stanja (gumbi so posodabljani prek _refreshSsr)
    (d.ssr || []).forEach((ssr, idx) => {
      const n = idx + 1;
      const on = ssr.on || false;
      // Pri /api/status/light nimamo state_str, samo on/countdown — minimalni prikaz
      // Pravi SSR refresh prek _refreshSsr() ki kliče /api/ssr
    });

    const rampMap = { up:'GOR', moving:'↕ PREMIKANJE', down:'DOL' };
    const doorMap = { open:'ODPRTA', closed:'zaprta' };
    set('diag-ramp', rampMap[d.ramp] || d.ramp || '–');
    set('diag-door', doorMap[d.door] || d.door || '–');

    // Lux + noč/dan — v eni vrstici
    const lux  = d.lux !== undefined ? d.lux.toFixed(1) + ' lx' : '–';
    const dstr = d.is_night !== undefined ? (d.is_night ? 'NOC' : 'DAN') : '';
    set('diag-lux-day', dstr ? lux + ' · ' + dstr : lux);

    (d.parking || []).forEach(p => {
      const m = p.place;
      const occ = p.occupied;
      set('diag-park'+m, occ ? (p.vehicle_name || 'ZASEDENO') : 'PROSTO');
      cls('diag-park'+m, occ ? 'warn' : 'ok');
      set('diag-park'+m+'-phase', p.tof_phase_str || '–');
    });
  }

  function _updateSensors(d) {
    // TOF tabela
    const tbody = document.getElementById('diag-tof-body');
    if (tbody && d.tof) {
      tbody.innerHTML = d.tof.map(t => {
        const ok  = t.ok !== false;
        const err = t.dist_mm === 65535 || t.dist_mm === 0xFFFF;
        const distStr = (!ok || err) ? '–' : (t.dist_mm + ' mm');
        const badge = !ok
          ? '<span class="badge badge-gray">N/I</span>'
          : err ? '<span class="badge badge-red">NAPAKA</span>'
                : '<span class="badge badge-green">OK</span>';
        return `<tr>
          <td class="mono">${t.name}</td>
          <td class="mono">${distStr}</td>
          <td>${badge}</td>
          <td class="right dim">${t.errors || 0}</td>
        </tr>`;
      }).join('');
    }

    // Fotocelice — inline v Rampa/Vrata kartica
    const cells = d.cells || [];
    const zun = cells.find(c => c.name === 'zunanja');
    const not = cells.find(c => c.name === 'notranja');
    const cellBadge = (c) => c
      ? (c.broken ? '<span class="badge badge-red">PREKINJENA</span>' : '<span class="badge badge-green">OK</span>')
      : '–';
    const zunEl = document.getElementById('diag-cell-zun');
    const notEl = document.getElementById('diag-cell-not');
    if (zunEl) zunEl.innerHTML = cellBadge(zun);
    if (notEl) notEl.innerHTML = cellBadge(not);
  }

  // ── Update: Radar zavihek ─────────────────────────────────
  async function _radarRefresh() {
    try {
      const data = await api.get('/api/radar');
      const ts = document.getElementById('diag-radar-ts');
      if (ts) ts.textContent = new Date().toLocaleTimeString('sl-SI');

      (data.sensors || []).forEach((r, i) => {
        const set = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };

        // Badge
        const badgeEl = document.getElementById('diag-rad-badge' + i);
        if (badgeEl) {
          if (!r.active)        { badgeEl.textContent = 'N/I';   badgeEl.className = 'badge badge-gray'; }
          else if (!r.config_ok){ badgeEl.textContent = 'ERR';   badgeEl.className = 'badge badge-red'; }
          else                  { badgeEl.textContent = 'OK';    badgeEl.className = 'badge badge-green'; }
        }

        // Detekcijsko stanje
        const det = r.detection || 0;
        const hasMov  = det === 1 || det === 3;
        const hasStat = det === 2;
        const stateEl = document.getElementById('diag-rad-state' + i);
        if (stateEl) {
          stateEl.classList.remove('loading', 'warn', 'ok');
          if (!r.active)    { stateEl.textContent = 'N/I';       }
          else if (hasMov)  { stateEl.textContent = 'GIBANJE';   stateEl.classList.add('warn'); }
          else if (hasStat) { stateEl.textContent = 'statično';  }
          else              { stateEl.textContent = 'mirovanje'; }
        }

        // Progress bar
        const moveThr = r.move_sens   || 0;
        const statThr = r.static_sens || 0;
        let raw, dispThr, colorLo, colorHi;
        if (hasMov) {
          raw = r.move_energy || 0;   dispThr = moveThr;
          colorLo = '#155E1F'; colorHi = '#F97316';
        } else if (hasStat) {
          raw = r.static_energy || 0; dispThr = statThr;
          colorLo = '#1E3A5F'; colorHi = '#3B82F6';
        } else {
          raw = r.move_energy || 0;   dispThr = moveThr;
          colorLo = '#334155'; colorHi = '#334155';
        }
        const scaled = _radarScale(raw, dispThr);
        const pct = dispThr > 0 ? Math.round(raw * 100 / dispThr) : '–';

        const barEl = document.getElementById('diag-rad-bar' + i);
        if (barEl) {
          barEl.style.width = (r.active ? scaled : 0) + '%';
          if (scaled >= 75 && colorHi !== colorLo) {
            const split = (75 / scaled * 100).toFixed(1);
            barEl.style.background = `linear-gradient(to right,${colorLo} ${split}%,${colorHi} ${split}%)`;
          } else {
            barEl.style.background = colorLo;
          }
        }
        const lblEl = document.getElementById('diag-rad-bar-lbl' + i);
        if (lblEl) {
          lblEl.textContent = r.active
            ? ('energija: ' + raw + ' / prag: ' + dispThr + ' (' + pct + '%)')
            : '–';
        }

        // Razdalja (ločeno gib/stat)
        set('diag-rad-mdist' + i, r.moving_dist_cm ? _fmtDist(r.moving_dist_cm) : '–');
        set('diag-rad-sdist' + i, r.static_dist_cm ? _fmtDist(r.static_dist_cm) : '–');

        // Nastavitve
        const maxDistM = r.max_dist !== undefined ? (r.max_dist * 0.75).toFixed(2) + ' m' : '–';
        set('diag-rad-maxd' + i, maxDistM);
        set('diag-rad-ms'   + i, r.move_sens   !== undefined ? r.move_sens + '%'   : '–');
        set('diag-rad-ss'   + i, r.static_sens !== undefined
              ? (r.static_sens === 0 ? '0% (izkl.)' : r.static_sens + '%') : '–');
        set('diag-rad-us'   + i, r.unmanned_s  !== undefined ? r.unmanned_s + ' s' : '–');

        // Statistike
        set('diag-rad-frm' + i, r.frames_ok !== undefined ? r.frames_ok : '–');
        set('diag-rad-err' + i, r.errors    !== undefined ? r.errors    : '–');
        const cfgStr = r.config_ok
          ? (r.config_verified ? 'OK / verif.' : 'OK / čaka')
          : (r.active ? 'NAPAKA' : '–');
        set('diag-rad-cfg' + i, cfgStr);
      });
    } catch(e) {
      const ts = document.getElementById('diag-radar-ts');
      if (ts) ts.textContent = '⚠ ' + e.message;
    }
  }

  // ── Pregled refresh ───────────────────────────────────────
  window.diagRefresh = async function() {
    const ts  = document.getElementById('diag-ts');
    const btn = document.getElementById('diag-refresh-btn');
    if (ts)  ts.textContent = 'Osveževam…';
    if (btn) btn.disabled = true;
    try {
      const [light, sensors] = await Promise.all([
        api.get('/api/status/light'),
        api.get('/api/status/sensors')
      ]);
      _updateLight(light);
      _updateSensors(sensors);
      // SSR gumbi — dobijo pravilne state_str prek /api/ssr
      await _refreshSsr();
      if (ts) ts.textContent = new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      if (ts) ts.textContent = '⚠ ' + e.message;
    } finally {
      if (btn) btn.disabled = false;
    }
  };

  window.page_diagnostika = function() {
    if (!document.getElementById('diag-ts')) {
      _render();
      _diagTab('pregled');
    }
    // Obnovi checkbox + gumb stanje ob povratku
    const chk = document.getElementById('diag-auto-chk');
    if (chk) chk.checked = _autoOn;
    const btn = document.getElementById('diag-refresh-btn');
    if (btn) btn.style.display = _autoOn ? 'none' : '';

    if (_autoOn) {
      if (_activeTab === 'radar') { _radarRefresh(); registerPoller('diagnostika', _radarRefresh, 1000); }
      else                        { diagRefresh();   registerPoller('diagnostika', diagRefresh,   3000); }
    } else {
      if (_activeTab === 'pregled') diagRefresh();
      else _radarRefresh();
    }
  };

})();
