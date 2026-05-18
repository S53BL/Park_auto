// ============================================================
// diagnostika.js — Stran: Diagnostika  (v2)
// Pull na klik — NI polling
// Kliče: /api/status/light + /api/status/sensors + /api/status/system
// Prikazuje: SSR, rampa/vrata/svetloba, parking, TOF, radar (+razdalja+energija),
//            fotocelice, WiFi, SD kartica
// ============================================================
(function () {
  const DIV = 'page-diagnostika';

  function _fmtUptime(ms) {
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    const d = Math.floor(h / 24);
    if (d > 0) return d + 'd ' + (h % 24) + 'h';
    if (h > 0) return h + 'h ' + (m % 60) + 'm';
    return m + 'm ' + (s % 60) + 's';
  }

  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Diagnostika</h1>
  <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
    <span class="text-dim text-tiny" id="diag-ts">–</span>
    <button class="btn" onclick="diagRefresh()">↻ Osveži</button>
  </div>
</div>

<!-- SSR ───────────────────────────────────── -->
<div class="section-label">Osvetlitev — SSR</div>
<div class="grid-4" id="diag-ssr">
  ${[1,2,3,4].map(i => `
  <div class="card">
    <div class="card-title">SSR${i}</div>
    <div class="metric loading" id="diag-ssr${i}-state">–</div>
    <div class="text-dim text-tiny mt8" id="diag-ssr${i}-cd"></div>
  </div>`).join('')}
</div>

<!-- RAMPA / VRATA / SVETLOBA / PARKING ─────── -->
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

<!-- TOF ────────────────────────────────────── -->
<div class="section-label mt20">TOF senzorji</div>
<div class="card">
  <table class="tbl">
    <thead><tr><th>Senzor</th><th>Razdalja</th><th>Status</th><th class="right">Napake</th></tr></thead>
    <tbody id="diag-tof-body"><tr><td colspan="4" class="empty-state">Pritisni Osveži</td></tr></tbody>
  </table>
</div>

<!-- RADAR ──────────────────────────────────── -->
<div class="section-label mt20">Radar senzorji</div>
<div class="grid-4" id="diag-radar-grid">
  ${['Vhod','Cesta L','Cesta D','Garaža'].map((name, i) => `
  <div class="card">
    <div class="card-title">${name}</div>
    <div class="metric loading" id="diag-radar${i}">–</div>
    <div class="kv-list mt8" id="diag-radar${i}-detail" style="display:none">
      <div class="kv-row"><span class="kv-key">Razdalja</span><span class="kv-val text-mono" id="diag-radar${i}-dist">–</span></div>
      <div class="kv-row"><span class="kv-key">Energija</span><span class="kv-val text-mono" id="diag-radar${i}-nrg">–</span></div>
    </div>
  </div>`).join('')}
</div>

<!-- FOTOCELICE ─────────────────────────────── -->
<div class="section-label mt20">Fotocelice</div>
<div class="card">
  <table class="tbl">
    <thead><tr><th>Senzor</th><th>Stanje</th></tr></thead>
    <tbody id="diag-cells-body"><tr><td colspan="2" class="empty-state">Pritisni Osveži</td></tr></tbody>
  </table>
</div>

<!-- SISTEM / WiFi / SD ─────────────────────── -->
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
`;
  }

  function _updateLight(d) {
    const set = (id, v) => { const e = document.getElementById(id); if (e) { e.textContent = v; e.classList.remove('loading'); } };
    const cls = (id, c) => { const e = document.getElementById(id); if (e) e.className = 'metric ' + c; };

    // SSR
    (d.ssr || []).forEach((ssr, idx) => {
      const n = idx + 1;
      const on = ssr.on || false;
      set('diag-ssr'+n+'-state', on ? 'ON' : 'OFF');
      cls('diag-ssr'+n+'-state', on ? 'ok' : '');
      set('diag-ssr'+n+'-cd', ssr.countdown_s > 0 ? '⏱ ' + fmt.countdown(ssr.countdown_s) : '');
    });

    // Rampa / vrata
    const rampMap = { up:'GOR', moving:'↕ PREMIKANJE', down:'DOL' };
    const doorMap = { open:'ODPRTA', closed:'zaprta' };
    set('diag-ramp', rampMap[d.ramp] || d.ramp || '–');
    set('diag-door', doorMap[d.door] || d.door || '–');

    // Svetloba / noč-dan
    set('diag-lux', d.lux !== undefined ? d.lux.toFixed(1) + ' lx' : '–');
    set('diag-daynight', d.is_night !== undefined ? (d.is_night ? 'NOC' : 'DAN') : '–');

    // Parking
    (d.parking || []).forEach(p => {
      const m = p.place;
      const occ = p.occupied;
      set('diag-park'+m, occ ? (p.vehicle_name || 'ZASEDENO') : 'PROSTO');
      cls('diag-park'+m, occ ? 'warn' : 'ok');
      set('diag-park'+m+'-phase', p.tof_phase_str || '–');
    });
  }

  function _updateSensors(d) {
    // TOF
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

    // Radar
    (d.radar || []).forEach((r, i) => {
      const el = document.getElementById('diag-radar'+i);
      const detailEl = document.getElementById('diag-radar'+i+'-detail');
      if (!el) return;
      el.classList.remove('loading');
      const det = r.detection || 'absent';
      const hasMotion = det === 'moving' || det === 'both';
      const hasStatic = det === 'static' || det === 'both';

      if (!r.active)   { el.textContent = 'N/I';       el.className = 'metric'; }
      else if (hasMotion) { el.textContent = 'GIBANJE'; el.className = 'metric warn'; }
      else if (hasStatic) { el.textContent = 'statično'; el.className = 'metric'; }
      else             { el.textContent = 'mirovanje'; el.className = 'metric'; }

      // Razdalja + energija ko je detekcija aktivna
      const showDetail = r.active && (hasMotion || hasStatic);
      if (detailEl) detailEl.style.display = showDetail ? '' : 'none';
      if (showDetail) {
        const distEl = document.getElementById('diag-radar'+i+'-dist');
        const nrgEl  = document.getElementById('diag-radar'+i+'-nrg');
        if (hasMotion) {
          if (distEl) distEl.textContent = (r.moving_dist_cm || 0) + ' cm';
          if (nrgEl)  nrgEl.textContent  = (r.moving_energy  || 0) + '%';
        } else {
          if (distEl) distEl.textContent = (r.static_dist_cm || 0) + ' cm';
          if (nrgEl)  nrgEl.textContent  = (r.static_energy  || 0) + '%';
        }
      }
    });

    // Fotocelice
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

  window.diagRefresh = async function() {
    const ts = document.getElementById('diag-ts');
    if (ts) ts.textContent = 'Osveževam…';
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
    }
  };

  window.page_diagnostika = function() {
    if (!document.getElementById('diag-ts')) _render();
    diagRefresh();
  };
})();
