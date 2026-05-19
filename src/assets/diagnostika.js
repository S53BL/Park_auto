// ============================================================
// diagnostika.js — Stran: Diagnostika  (v3)
// Zavihek 1 "Pregled": pull-na-klik (SSR, TOF, fotocelice, sistem)
// Zavihek 2 "Radar": auto-refresh 1s, bogat prikaz z progress barom
// Kliče: /api/status/light + /api/status/sensors + /api/status/system
//        /api/radar (samo Radar zavihek)
// ============================================================
(function () {
  const DIV = 'page-diagnostika';
  let _activeTab = 'pregled';

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
  // 0..threshold → 0..75   threshold..100 → 75..100
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
    if (tab === 'radar') {
      registerPoller('diagnostika', _radarRefresh, 1000);
    } else {
      clearPoller('diagnostika');
    }
  };

  // ── Render (enkrat ob prvem obisku strani) ────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Diagnostika</h1>
  <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
    <span class="text-dim text-tiny" id="diag-ts">–</span>
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
  </div>`).join('')}
</div>

<!-- RAMPA / VRATA / SVETLOBA / PARKING -->
<div class="grid-3 mt12">
  <div class="card">
    <div class="card-title">Rampa / Vrata / Svetloba</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Rampa</span><span class="kv-val" id="diag-ramp">–</span></div>
      <div class="kv-row"><span class="kv-key">Vrata</span><span class="kv-val" id="diag-door">–</span></div>
      <div class="kv-row"><span class="kv-key">Svetloba</span><span class="kv-val" id="diag-lux">–</span></div>
      <div class="kv-row"><span class="kv-key">Noč/Dan</span><span class="kv-val" id="diag-daynight">–</span></div>
    </div>
  </div>
  ${['A','B'].map(m => `
  <div class="card">
    <div class="card-title">Parking ${m}</div>
    <div class="metric loading" id="diag-park${m}">–</div>
    <div class="text-dim text-tiny mt8" id="diag-park${m}-phase">–</div>
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

<!-- FOTOCELICE -->
<div class="section-label mt20">Fotocelice</div>
<div class="card">
  <table class="tbl">
    <thead><tr><th>Senzor</th><th>Stanje</th></tr></thead>
    <tbody id="diag-cells-body"><tr><td colspan="2" class="empty-state">Pritisni Osveži</td></tr></tbody>
  </table>
</div>

<!-- SISTEM -->
<div class="section-label mt20">Sistem</div>
<div class="grid-2">
  <div class="card">
    <div class="card-title">WiFi</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">IP</span><span class="kv-val text-mono" id="diag-ip">–</span></div>
      <div class="kv-row"><span class="kv-key">RSSI</span><span class="kv-val" id="diag-rssi">–</span></div>
      <div class="kv-row"><span class="kv-key">Uptime</span><span class="kv-val" id="diag-uptime">–</span></div>
    </div>
  </div>
  <div class="card">
    <div class="card-title">SD kartica</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Status</span><span class="kv-val" id="diag-sd-status">–</span></div>
      <div class="kv-row"><span class="kv-key">Skupaj</span><span class="kv-val" id="diag-sd-total">–</span></div>
      <div class="kv-row"><span class="kv-key">Prosto</span><span class="kv-val" id="diag-sd-free">–</span></div>
    </div>
    <div class="progress-bar mt8">
      <div class="progress-fill" id="diag-sd-bar" style="width:0%"></div>
    </div>
  </div>
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
      <div class="kv-row"><span class="kv-key">Razdalja</span><span class="kv-val text-mono" id="diag-rad-dist${i}">–</span></div>
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

    (d.ssr || []).forEach((ssr, idx) => {
      const n = idx + 1;
      const on = ssr.on || false;
      set('diag-ssr'+n+'-state', on ? 'ON' : 'OFF');
      cls('diag-ssr'+n+'-state', on ? 'ok' : '');
      set('diag-ssr'+n+'-cd', ssr.countdown_s > 0 ? '⏱ ' + fmt.countdown(ssr.countdown_s) : '');
    });

    const rampMap = { up:'GOR', moving:'↕ PREMIKANJE', down:'DOL' };
    const doorMap = { open:'ODPRTA', closed:'zaprta' };
    set('diag-ramp', rampMap[d.ramp] || d.ramp || '–');
    set('diag-door', doorMap[d.door] || d.door || '–');
    set('diag-lux', d.lux !== undefined ? d.lux.toFixed(1) + ' lx' : '–');
    set('diag-daynight', d.is_night !== undefined ? (d.is_night ? 'NOC' : 'DAN') : '–');

    (d.parking || []).forEach(p => {
      const m = p.place;
      const occ = p.occupied;
      set('diag-park'+m, occ ? (p.vehicle_name || 'ZASEDENO') : 'PROSTO');
      cls('diag-park'+m, occ ? 'warn' : 'ok');
      set('diag-park'+m+'-phase', p.tof_phase_str || '–');
    });
  }

  function _updateSensors(d) {
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

    const cbody = document.getElementById('diag-cells-body');
    if (cbody && d.cells) {
      cbody.innerHTML = d.cells.map(c => {
        const badge = c.broken
          ? '<span class="badge badge-red">PREKINJENA</span>'
          : '<span class="badge badge-green">OK</span>';
        return `<tr><td>${c.name}</td><td>${badge}</td></tr>`;
      }).join('');
    }
  }

  function _updateSystem(d) {
    const set = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };

    const w = d.wifi || {};
    set('diag-ip',     w.ip   || '–');
    set('diag-rssi',   w.rssi !== undefined ? w.rssi + ' dBm' : '–');
    set('diag-uptime', w.uptime_ms !== undefined ? _fmtUptime(w.uptime_ms) : '–');

    const sd = d.sd || {};
    set('diag-sd-status', sd.ready !== undefined ? (sd.ready ? 'READY' : (sd.status || 'napaka')) : '–');
    set('diag-sd-total',  sd.total_mb !== undefined ? sd.total_mb + ' MB' : '–');
    set('diag-sd-free',   sd.free_mb  !== undefined ? sd.free_mb  + ' MB' : '–');

    const bar = document.getElementById('diag-sd-bar');
    if (bar && sd.total_mb > 0) {
      const pct = Math.round(100 - (sd.free_mb / sd.total_mb) * 100);
      bar.style.width = pct + '%';
      bar.className = 'progress-fill' + (pct > 90 ? ' err' : pct > 75 ? ' warn' : '');
    }
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
        const hasMov = det === 1 || det === 3;
        const hasStat = det === 2;
        const stateEl = document.getElementById('diag-rad-state' + i);
        if (stateEl) {
          stateEl.classList.remove('loading', 'warn', 'ok');
          if (!r.active)    { stateEl.textContent = 'N/I';       }
          else if (hasMov)  { stateEl.textContent = 'GIBANJE';   stateEl.classList.add('warn'); }
          else if (hasStat) { stateEl.textContent = 'statično';  }
          else              { stateEl.textContent = 'mirovanje'; }
        }

        // Progress bar (koščkasto skaliranje, črtice pri 75%)
        const raw   = hasMov ? (r.move_energy || 0) : (hasStat ? (r.static_energy || 0) : 0);
        const thr   = hasMov ? (r.move_sens || 0)   : (hasStat ? (r.static_sens || 0)   : 0);
        const scaled = _radarScale(raw, thr);
        const barEl = document.getElementById('diag-rad-bar' + i);
        if (barEl) {
          barEl.style.width = scaled + '%';
          if (scaled >= 75) {
            // Nad pragom: oranžna za del nad 75%, zelena spodaj — simuliramo z oranžno
            barEl.style.background = 'linear-gradient(to right, #155E1F ' + (75/scaled*100).toFixed(1) + '%, #F97316 ' + (75/scaled*100).toFixed(1) + '%)';
          } else {
            barEl.style.background = '#155E1F';
          }
        }
        const lblEl = document.getElementById('diag-rad-bar-lbl' + i);
        if (lblEl) {
          if (r.active && (hasMov || hasStat)) {
            lblEl.textContent = 'energija: ' + raw + ' / prag: ' + thr + ' (' + scaled + '%)';
          } else {
            lblEl.textContent = r.active ? 'mirovanje' : '–';
          }
        }

        // Razdalja
        const dist = hasMov ? (r.moving_dist_cm || r.dist_cm || 0)
                             : (hasStat ? (r.static_dist_cm || r.dist_cm || 0) : 0);
        set('diag-rad-dist' + i, dist ? _fmtDist(dist) : '–');

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

  // ── Pregled refresh (pull-na-klik) ───────────────────────
  window.diagRefresh = async function() {
    const ts = document.getElementById('diag-ts');
    const btn = document.getElementById('diag-refresh-btn');
    if (ts) ts.textContent = 'Osveževam…';
    if (btn) btn.disabled = true;
    try {
      const [light, sensors, system] = await Promise.all([
        api.get('/api/status/light'),
        api.get('/api/status/sensors'),
        api.get('/api/status/system')
      ]);
      _updateLight(light);
      _updateSensors(sensors);
      _updateSystem(system);
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
    if (_activeTab === 'pregled') diagRefresh();
    else _radarRefresh();
  };

})();
