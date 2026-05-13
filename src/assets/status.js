// ============================================================
// status.js — Stran: Status  (v2 — usklajen z /api/status E3)
// Polling /api/status vsakih 2 s
// Read-only diagnostika: SSR, senzorji, parkirišče
//
// SPREMEMBE glede na v1:
//   - radar: .detection string ("moving"/"static"/"absent"/"both")
//     namesto bool .motion — razširjene kartice z razdaljo+energijo
//   - tof: .ok + .name namesto .status + .label
//   - cells: .broken bool + .name namesto .ok + .label
//   - parking: .vehicle namesto .vehicle_name
//   - countdown: API vrača countdown_s (sekunde), ne countdown_ms
//   - radar kartica: prikazuje dist_cm in energijo kadar je detekcija aktivna
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

<!-- LUX / PARTY / RAMPA+VRATA ──────────────── -->
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
      <div class="kv-row">
        <span class="kv-key">Rampa</span>
        <span class="kv-val" id="st-ramp">–</span>
      </div>
      <div class="kv-row">
        <span class="kv-key">Vrata</span>
        <span class="kv-val" id="st-door">–</span>
      </div>
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
      <div class="kv-row">
        <span class="kv-key">Faza</span>
        <span class="kv-val" id="st-park${m}-phase">–</span>
      </div>
      <div class="kv-row">
        <span class="kv-key">Vozilo</span>
        <span class="kv-val text-mono" id="st-park${m}-vehicle">–</span>
      </div>
      <div class="kv-row">
        <span class="kv-key">DTW dist</span>
        <span class="kv-val text-mono" id="st-park${m}-dtw">–</span>
      </div>
    </div>
  </div>`).join('')}
</div>

<!-- TOF SENZORJI ───────────────────────────── -->
<div class="section-label mt20">TOF senzorji</div>
<div class="card">
  <table class="tbl">
    <thead><tr>
      <th>Senzor</th><th>Razdalja</th><th>Status</th><th class="right">Napake</th>
    </tr></thead>
    <tbody id="st-tof-body">
      <tr><td colspan="4" class="empty-state loading">Pridobivam…</td></tr>
    </tbody>
  </table>
</div>

<!-- RADAR ──────────────────────────────────── -->
<div class="section-label mt20">Radar senzorji</div>
<div class="grid-4" id="st-radar-grid">
  ${['Vhod','Cesta L','Cesta D','Garaža'].map((name, i) => `
  <div class="card" id="st-radar-card${i}">
    <div class="card-title">${name}</div>
    <div class="metric loading" id="st-radar${i}-det">–</div>
    <div class="kv-list mt8" id="st-radar${i}-detail" style="display:none">
      <div class="kv-row">
        <span class="kv-key">Razdalja</span>
        <span class="kv-val text-mono" id="st-radar${i}-dist">–</span>
      </div>
      <div class="kv-row">
        <span class="kv-key">Energija</span>
        <span class="kv-val text-mono" id="st-radar${i}-nrg">–</span>
      </div>
    </div>
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
    <div class="card-title">WiFi &amp; sistem</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">IP</span>     <span class="kv-val text-mono" id="st-ip">–</span></div>
      <div class="kv-row"><span class="kv-key">SSID</span>   <span class="kv-val" id="st-ssid">–</span></div>
      <div class="kv-row"><span class="kv-key">RSSI</span>   <span class="kv-val" id="st-rssi">–</span></div>
      <div class="kv-row"><span class="kv-key">NTP</span>    <span class="kv-val" id="st-ntp">–</span></div>
      <div class="kv-row"><span class="kv-key">Uptime</span> <span class="kv-val" id="st-uptime">–</span></div>
    </div>
  </div>
  <div class="card">
    <div class="card-title">SD kartica</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Status</span> <span class="kv-val" id="st-sd-status">–</span></div>
      <div class="kv-row"><span class="kv-key">Skupaj</span> <span class="kv-val" id="st-sd-total">–</span></div>
      <div class="kv-row"><span class="kv-key">Prosto</span> <span class="kv-val" id="st-sd-free">–</span></div>
    </div>
    <div class="progress-bar mt8">
      <div class="progress-fill" id="st-sd-bar" style="width:0%"></div>
    </div>
  </div>
</div>
`;
  }

  // ── Posodobi UI iz podatkov ──────────────────────────────
  function _update(d) {
    const el   = id => document.getElementById(id);
    const set  = (id, val) => { const e = el(id); if (e) { e.textContent = val; e.classList.remove('loading'); } };
    const cls  = (id, c)   => { const e = el(id); if (e) e.className = 'metric ' + c; };
    const show = (id, vis) => { const e = el(id); if (e) e.style.display = vis ? '' : 'none'; };

    // Timestamp
    set('st-ts', new Date().toLocaleTimeString('sl-SI'));

    // ── SSR (countdown_s = sekunde iz light_logic, 0 = ni timera) ──
    const ssrs = d.ssr || [];
    [1,2,3,4].forEach((n, i) => {
      const ssr = ssrs[i] || {};
      const on  = ssr.on || false;
      set('st-ssr'+n+'-state', on ? 'ON' : 'OFF');
      cls('st-ssr'+n+'-state', on ? 'ok' : '');
      const cd = ssr.countdown_s > 0 ? fmt.countdown(ssr.countdown_s) : '';
      set('st-ssr'+n+'-cd', cd ? '⏱ ' + cd : '');
    });

    // ── Lux / noč-dan ──
    const lux   = d.lux !== undefined ? d.lux : null;
    const night = d.is_night;
    set('st-lux',      lux !== null ? lux.toFixed(1) + ' lx' : '–');
    set('st-daynight', night !== undefined ? (night ? '🌙 noč' : '☀ dan') : '–');

    // ── Party ──
    const party = d.party_mode;
    set('st-party', party !== undefined ? (party ? 'AKTIVEN' : 'neaktiven') : '–');
    cls('st-party', party ? 'ok' : '');

    // ── Rampa / vrata ──
    // API vrača: ramp = "up" | "moving" | "down"
    //            door = "open" | "closed"
    const rampMap = { up: 'GOR', moving: '↕ PREMIKANJE', down: 'DOL' };
    const doorMap = { open: 'ODPRTA', closed: 'zaprta' };
    set('st-ramp', rampMap[d.ramp] || d.ramp || '–');
    set('st-door', doorMap[d.door] || d.door || '–');

    const rampEl = el('st-ramp');
    if (rampEl) rampEl.className = 'kv-val' + (d.ramp === 'moving' ? ' warn' : d.ramp === 'up' ? ' ok' : '');
    const doorEl = el('st-door');
    if (doorEl) doorEl.className = 'kv-val' + (d.door === 'open' ? ' warn' : '');

    // ── Parkirišče ──
    // API: parking[].place, .phase, .phase_str, .active, .occupied, .vehicle, .dtw_dist
    const parks = d.parking || [];
    ['A','B'].forEach((m, i) => {
      const p    = parks[i] || {};
      const occ  = p.occupied;
      const name = p.vehicle || '';

      set('st-park'+m+'-state',
          occ !== undefined ? (occ ? (name || 'ZASEDENO') : 'PROSTO') : '–');
      cls('st-park'+m+'-state',
          occ !== undefined ? (occ ? 'warn' : 'ok') : 'loading');

      const phaseLabels = ['IDLE','DETECT','SCANNING','DTW_WAIT'];
      const phaseIdx    = p.phase;
      const phaseLabel  = p.phase_str || (phaseIdx !== undefined ? phaseLabels[phaseIdx] : '–');
      set('st-park'+m+'-phase',   phaseLabel || '–');
      set('st-park'+m+'-vehicle', name || (occ ? 'neznan' : '–'));
      set('st-park'+m+'-dtw',     p.dtw_dist !== undefined && p.dtw_dist > 0
                                    ? p.dtw_dist.toFixed(2) : '–');
    });

    // ── TOF senzorji ──
    // API: tof[].id, .name, .ok, .dist_mm, .errors
    const tofs  = d.tof || [];
    const tbody = el('st-tof-body');
    if (tbody) {
      if (tofs.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="text-dim" style="padding:8px 10px">Ni podatkov</td></tr>';
      } else {
        tbody.innerHTML = tofs.map(t => {
          const ok      = t.ok !== false;
          const err     = t.dist_mm === 0xFFFF || t.dist_mm === 65535;
          const distStr = (!ok || err) ? '–' : (t.dist_mm + ' mm');
          const badge   = !ok
            ? '<span class="badge badge-gray">N/I</span>'
            : err
              ? '<span class="badge badge-red">NAPAKA</span>'
              : '<span class="badge badge-green">OK</span>';
          return `<tr>
            <td class="mono">${t.name || ('TOF-' + t.id)}</td>
            <td class="mono">${distStr}</td>
            <td>${badge}</td>
            <td class="right dim">${t.errors || 0}</td>
          </tr>`;
        }).join('');
      }
    }

    // ── Radar senzorji ──
    // API: radar[].id, .name, .active, .detection ("moving"|"static"|"absent"|"both")
    //             .moving_dist_cm, .moving_energy, .static_dist_cm, .static_energy
    const radars = d.radar || [];
    [0,1,2,3].forEach(i => {
      const r   = radars[i] || {};
      const det = r.detection || 'absent'; // "moving"|"static"|"absent"|"both"

      // Prikazan tekst in barva
      let label = '–', metricCls = 'loading';
      if (!r.active) {
        label = 'N/I'; metricCls = '';
      } else if (det === 'moving' || det === 'both') {
        label = 'GIBANJE'; metricCls = 'warn';
      } else if (det === 'static') {
        label = 'statično'; metricCls = '';
      } else {
        label = 'mirovanje'; metricCls = '';
      }
      set('st-radar'+i+'-det', label);
      cls('st-radar'+i+'-det', metricCls);

      // Detail vrstica — prikaži samo kadar je detekcija aktivna
      const hasMotion = det === 'moving' || det === 'both';
      const hasStatic = det === 'static' || det === 'both';
      const showDetail = r.active && (hasMotion || hasStatic);

      show('st-radar'+i+'-detail', showDetail);
      if (showDetail) {
        if (hasMotion) {
          set('st-radar'+i+'-dist',  r.moving_dist_cm + ' cm');
          set('st-radar'+i+'-nrg',   r.moving_energy + '%');
        } else {
          set('st-radar'+i+'-dist',  r.static_dist_cm + ' cm');
          set('st-radar'+i+'-nrg',   r.static_energy + '%');
        }
      }
    });

    // ── Fotocelice ──
    // API: cells[].id, .name, .broken  (broken=true → prekinjena)
    const cells = d.cells || [];
    const cbody = el('st-cells-body');
    if (cbody) {
      if (cells.length === 0) {
        cbody.innerHTML = '<tr><td colspan="2" class="text-dim" style="padding:8px 10px">Ni podatkov</td></tr>';
      } else {
        cbody.innerHTML = cells.map(c => {
          const broken = c.broken === true;
          const badge  = broken
            ? '<span class="badge badge-red">PREKINJENA</span>'
            : '<span class="badge badge-green">OK</span>';
          return `<tr>
            <td>${c.name || ('Fotocelica ' + c.id)}</td>
            <td>${badge}</td>
          </tr>`;
        }).join('');
      }
    }

    // ── WiFi ──
    const w = d.wifi || {};
    set('st-ip',     w.ip      || '–');
    set('st-ssid',   w.ssid    || '–');
    set('st-rssi',   w.rssi    !== undefined ? w.rssi + ' dBm' : '–');
    set('st-ntp',    w.ntp_ok  !== undefined
                       ? (w.ntp_ok ? (w.ntp_time || 'OK') : 'NI sinhronizirano')
                       : '–');
    set('st-uptime', w.uptime_ms !== undefined ? fmt.uptime(w.uptime_ms) : '–');

    // ── SD ──
    const sd = d.sd || {};
    set('st-sd-status', sd.ready !== undefined ? (sd.ready ? 'READY' : sd.status || 'napaka') : '–');
    set('st-sd-total',  sd.total_mb !== undefined ? sd.total_mb + ' MB' : '–');
    set('st-sd-free',   sd.free_mb  !== undefined ? sd.free_mb  + ' MB' : '–');

    const bar = el('st-sd-bar');
    if (bar && sd.total_mb > 0) {
      const pct = Math.round(100 - (sd.free_mb / sd.total_mb) * 100);
      bar.style.width = pct + '%';
      bar.className   = 'progress-fill' + (pct > 90 ? ' err' : pct > 75 ? ' warn' : '');
    }
  }

  // ── Poll ─────────────────────────────────────────────────
  async function _poll() {
    try {
      const d = await api.get('/api/status');
      _update(d);
    } catch(e) {
      const e2 = document.getElementById('st-ts');
      if (e2) e2.textContent = 'napaka: ' + e.message;
    }
  }

  // ── Init (kliče ga router) ───────────────────────────────
  window.page_status = function () {
    if (!document.getElementById('st-ts')) _render();
    registerPoller('status', _poll, 2000);
  };
})();
