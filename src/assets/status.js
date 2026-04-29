// ============================================================
// status.js — Stran: Status
// Polling /api/status vsakih 2 s
// Read-only diagnostika: SSR, senzorji, parkirišče
// ============================================================

(function () {
  const DIV = 'page-status';

  // ── Render skeleton (enkrat) ─────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Status</h1>
  <span class="subtitle" id="st-ts">–</span>
</div>

<!-- OSVETLitev / SSR ───────────────────────── -->
<div class="section-label">Osvetlitev — SSR</div>
<div class="grid-4" id="st-ssr">
  ${[1,2,3,4].map(i => `
  <div class="card">
    <div class="card-title">SSR${i}</div>
    <div class="metric loading" id="st-ssr${i}-state">–</div>
    <div class="text-dim text-tiny mt8" id="st-ssr${i}-cd"></div>
  </div>`).join('')}
</div>

<!-- LUX / PARTY ────────────────────────────── -->
<div class="grid-3 mt12">
  <div class="card">
    <div class="card-title">Svetloba</div>
    <div class="metric loading" id="st-lux">–</div>
    <div class="text-dim text-tiny mt8" id="st-daynight">–</div>
  </div>
  <div class="card">
    <div class="card-title">Party mode</div>
    <div class="metric loading" id="st-party">–</div>
  </div>
  <div class="card">
    <div class="card-title">Rampa / Vrata</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Rampa</span><span class="kv-val" id="st-ramp">–</span></div>
      <div class="kv-row"><span class="kv-key">Vrata</span><span class="kv-val" id="st-door">–</span></div>
    </div>
  </div>
</div>

<!-- PARKIRIŠČE ─────────────────────────────── -->
<div class="section-label mt20">Parkirišče</div>
<div class="grid-2">
  ${['A','B'].map(m => `
  <div class="card">
    <div class="card-title">Mesto ${m}</div>
    <div class="metric loading" id="st-park${m}-state">–</div>
    <div class="kv-list mt8">
      <div class="kv-row"><span class="kv-key">Faza</span><span class="kv-val" id="st-park${m}-phase">–</span></div>
      <div class="kv-row"><span class="kv-key">DTW dist</span><span class="kv-val text-mono" id="st-park${m}-dtw">–</span></div>
    </div>
  </div>`).join('')}
</div>

<!-- TOF SENZORJI ───────────────────────────── -->
<div class="section-label mt20">TOF senzorji</div>
<div class="card">
  <table class="tbl" id="st-tof-tbl">
    <thead><tr>
      <th>Senzor</th><th>Razdalja</th><th>Status</th>
    </tr></thead>
    <tbody id="st-tof-body">
      <tr><td colspan="3" class="empty-state loading">Pridobivam…</td></tr>
    </tbody>
  </table>
</div>

<!-- RADAR ──────────────────────────────────── -->
<div class="section-label mt20">Radar senzorji</div>
<div class="grid-4" id="st-radar">
  ${[0,1,2,3].map(i => `
  <div class="card">
    <div class="card-title">Radar ${i+1}</div>
    <div class="metric loading" id="st-radar${i}">–</div>
  </div>`).join('')}
</div>

<!-- FOTOCELICE ─────────────────────────────── -->
<div class="section-label mt20">Fotocelice</div>
<div class="card">
  <table class="tbl">
    <thead><tr><th>Senzor</th><th>Stanje</th></tr></thead>
    <tbody id="st-cells-body">
      <tr><td colspan="2" class="empty-state loading">Pridobivam…</td></tr>
    </tbody>
  </table>
</div>

<!-- WiFi / SD ──────────────────────────────── -->
<div class="grid-2 mt20">
  <div class="card">
    <div class="card-title">WiFi & sistem</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">IP</span>      <span class="kv-val text-mono" id="st-ip">–</span></div>
      <div class="kv-row"><span class="kv-key">SSID</span>    <span class="kv-val" id="st-ssid">–</span></div>
      <div class="kv-row"><span class="kv-key">RSSI</span>    <span class="kv-val" id="st-rssi">–</span></div>
      <div class="kv-row"><span class="kv-key">NTP</span>     <span class="kv-val" id="st-ntp">–</span></div>
      <div class="kv-row"><span class="kv-key">Uptime</span>  <span class="kv-val" id="st-uptime">–</span></div>
    </div>
  </div>
  <div class="card">
    <div class="card-title">SD kartica</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Status</span>  <span class="kv-val" id="st-sd-status">–</span></div>
      <div class="kv-row"><span class="kv-key">Skupaj</span>  <span class="kv-val" id="st-sd-total">–</span></div>
      <div class="kv-row"><span class="kv-key">Prosto</span>  <span class="kv-val" id="st-sd-free">–</span></div>
    </div>
    <div class="progress-bar mt8"><div class="progress-fill" id="st-sd-bar" style="width:0%"></div></div>
  </div>
</div>
`;
  }

  // ── Posodobi UI iz podatkov ──────────────────────────────
  function _update(d) {
    const set = (id, val) => { const el = document.getElementById(id); if (el) { el.textContent = val; el.classList.remove('loading'); } };
    const cls = (id, c)   => { const el = document.getElementById(id); if (el) el.className = 'metric ' + c; };

    // Timestamp
    set('st-ts', new Date().toLocaleTimeString('sl-SI'));

    // SSR
    const ssrs = d.ssr || [];
    [1,2,3,4].forEach((n, i) => {
      const ssr = ssrs[i] || {};
      const on  = ssr.on || false;
      set('st-ssr'+n+'-state', on ? 'ON' : 'OFF');
      cls('st-ssr'+n+'-state', on ? 'ok' : '');
      const cd = ssr.countdown_ms > 0 ? fmt.countdown(ssr.countdown_ms) : '';
      set('st-ssr'+n+'-cd', cd ? '⏱ ' + cd : '');
    });

    // Lux / day-night
    const lux   = d.lux !== undefined ? d.lux : null;
    const night = d.is_night;
    set('st-lux', lux !== null ? lux + ' lx' : (d.ssr ? '–' : '–'));
    set('st-daynight', night !== undefined ? (night ? '🌙 noč' : '☀ dan') : '–');

    // Party
    const party = d.party_mode;
    set('st-party', party !== undefined ? (party ? 'AKTIVEN' : 'neaktiven') : '–');
    cls('st-party', party ? 'ok' : '');

    // Rampa / vrata
    set('st-ramp', d.ramp || '–');
    set('st-door', d.door || '–');

    // Parkirišče
    const parks = d.parking || [];
    ['A','B'].forEach((m, i) => {
      const p   = parks[i] || {};
      const occ = p.occupied;
      const name = p.vehicle_name || (occ ? 'neznan' : '');
      set('st-park'+m+'-state', occ !== undefined ? (occ ? (name || 'ZASEDENO') : 'PROSTO') : '–');
      cls('st-park'+m+'-state', occ !== undefined ? (occ ? 'warn' : 'ok') : 'loading');
      set('st-park'+m+'-phase', p.phase !== undefined ? 'Faza ' + p.phase : '–');
      set('st-park'+m+'-dtw',   p.dtw_dist !== undefined ? p.dtw_dist.toFixed(2) : '–');
    });

    // TOF
    const tofs = d.tof || [];
    const tbody = document.getElementById('st-tof-body');
    if (tbody) {
      if (tofs.length === 0) {
        tbody.innerHTML = '<tr><td colspan="3" class="text-dim" style="padding:8px 10px">Ni podatkov (stub)</td></tr>';
      } else {
        tbody.innerHTML = tofs.map((t, i) => {
          const ok = t.status !== 'error';
          return `<tr>
            <td class="mono">TOF-${i+1} <span class="text-dim">${t.label||''}</span></td>
            <td class="mono">${t.dist_mm !== undefined ? t.dist_mm + ' mm' : '–'}</td>
            <td><span class="badge ${ok ? 'badge-green' : 'badge-red'}">${t.status||'–'}</span></td>
          </tr>`;
        }).join('');
      }
    }

    // Radar
    const radars = d.radar || [];
    [0,1,2,3].forEach(i => {
      const r   = radars[i] || {};
      const mov = r.motion;
      set('st-radar'+i, mov !== undefined ? (mov ? 'GIBANJE' : 'mirovanje') : '–');
      cls('st-radar'+i, mov !== undefined ? (mov ? 'warn' : '') : 'loading');
    });

    // Fotocelice
    const cells = d.cells || [];
    const cbody = document.getElementById('st-cells-body');
    if (cbody) {
      if (cells.length === 0) {
        cbody.innerHTML = '<tr><td colspan="2" class="text-dim" style="padding:8px 10px">Ni podatkov (stub)</td></tr>';
      } else {
        cbody.innerHTML = cells.map((c, i) => {
          const ok = c.ok !== false;
          return `<tr>
            <td>Fotocelica ${i+1} <span class="text-dim">${c.label||''}</span></td>
            <td><span class="badge ${ok ? 'badge-green' : 'badge-red'}">${ok ? 'OK' : 'PREKINJENA'}</span></td>
          </tr>`;
        }).join('');
      }
    }

    // WiFi
    const w = d.wifi || {};
    set('st-ip',     w.ip      || '–');
    set('st-ssid',   w.ssid    || '–');
    set('st-rssi',   w.rssi    !== undefined ? w.rssi + ' dBm' : '–');
    set('st-ntp',    w.ntp_ok  !== undefined ? (w.ntp_ok ? (w.ntp_time || 'OK') : 'NI sinhronizirano') : '–');
    set('st-uptime', w.uptime_ms !== undefined ? fmt.uptime(w.uptime_ms) : '–');

    // SD
    const sd = d.sd || {};
    set('st-sd-status', sd.ready !== undefined ? (sd.ready ? 'READY' : sd.status || 'napaka') : '–');
    set('st-sd-total',  sd.total_mb !== undefined ? sd.total_mb + ' MB' : '–');
    set('st-sd-free',   sd.free_mb  !== undefined ? sd.free_mb  + ' MB' : '–');

    const bar = document.getElementById('st-sd-bar');
    if (bar && sd.total_mb > 0) {
      const pct = Math.round(100 - (sd.free_mb / sd.total_mb) * 100);
      bar.style.width = pct + '%';
      bar.className = 'progress-fill' + (pct > 90 ? ' err' : pct > 75 ? ' warn' : '');
    }
  }

  // ── Poll ─────────────────────────────────────────────────
  async function _poll() {
    try {
      const d = await api.get('/api/status');
      _update(d);
    } catch(e) {
      const el = document.getElementById('st-ts');
      if (el) el.textContent = 'napaka: ' + e.message;
    }
  }

  // ── Init (kliče ga router) ───────────────────────────────
  window.page_status = function () {
    if (!document.getElementById('st-ts')) _render();
    registerPoller('status', _poll, 5000);
  };
})();
