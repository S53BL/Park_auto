// ============================================================
// diagnostika.js — Stran: Diagnostika  (v1)
// Pull na klik — NI polling
// Kliče: /api/status/light + /api/status/sensors
// Prikazuje: SSR, ramp/door, parking, TOF, radar, fotocelice
// ============================================================
(function () {
  const DIV = 'page-diagnostika';

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

<!-- RAMPA / VRATA / PARKING ────────────────── -->
<div class="grid-3 mt12">
  <div class="card">
    <div class="card-title">Rampa / Vrata</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Rampa</span><span class="kv-val" id="diag-ramp">–</span></div>
      <div class="kv-row"><span class="kv-key">Vrata</span><span class="kv-val" id="diag-door">–</span></div>
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

    // Ramp / door
    const rampMap = { up:'GOR', moving:'↕ PREMIKANJE', down:'DOL' };
    const doorMap = { open:'ODPRTA', closed:'zaprta' };
    set('diag-ramp', rampMap[d.ramp] || d.ramp || '–');
    set('diag-door', doorMap[d.door] || d.door || '–');

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
        const err = t.dist_mm === 65535;
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
      if (!el) return;
      el.classList.remove('loading');
      const det = r.detection || 'absent';
      if (!r.active)                               { el.textContent = 'N/I';       el.className = 'metric'; }
      else if (det === 'moving' || det === 'both') { el.textContent = 'GIBANJE';   el.className = 'metric warn'; }
      else if (det === 'static')                   { el.textContent = 'statično';  el.className = 'metric'; }
      else                                         { el.textContent = 'mirovanje'; el.className = 'metric'; }
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

  window.diagRefresh = async function() {
    const ts = document.getElementById('diag-ts');
    if (ts) ts.textContent = 'Osveževam…';
    try {
      const [light, sensors] = await Promise.all([
        api.get('/api/status/light'),
        api.get('/api/status/sensors')
      ]);
      _updateLight(light);
      _updateSensors(sensors);
      if (ts) ts.textContent = new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      if (ts) ts.textContent = '⚠ ' + e.message;
    }
  };

  window.page_diagnostika = function() {
    if (!document.getElementById('diag-ts')) _render();
    // Enkratna osvežitev ob odprtju strani — nato ročno
    diagRefresh();
  };
})();
