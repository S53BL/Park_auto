// ============================================================
// system.js — Stran: Sistem
// Diagnostika (CPU/RAM, WiFi, Firmware), OTA upload, reset/restart
// ============================================================

(function () {
  const DIV = 'page-system';

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Sistem</h1>
  <span class="subtitle" id="sys-ts">–</span>
</div>

<!-- DIAGNOSTIKA ─────────────────────────────── -->
<div class="section-label">Diagnostika</div>
<div class="grid-2">
  <div class="card">
    <div class="card-title">CPU &amp; RAM</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Core 0</span>     <span class="kv-val" id="sys-cpu0">–</span></div>
      <div class="kv-row"><span class="kv-key">Core 1</span>     <span class="kv-val" id="sys-cpu1">–</span></div>
      <div class="kv-row"><span class="kv-key">Free SRAM</span>  <span class="kv-val" id="sys-sram">–</span></div>
      <div class="kv-row"><span class="kv-key">Free PSRAM</span> <span class="kv-val" id="sys-psram">–</span></div>
    </div>
  </div>
  <div class="card">
    <div class="card-title">Čas &amp; WiFi</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Uptime</span>   <span class="kv-val text-mono" id="sys-uptime">–</span></div>
      <div class="kv-row"><span class="kv-key">NTP čas</span>  <span class="kv-val text-mono" id="sys-ntp">–</span></div>
      <div class="kv-row"><span class="kv-key">IP</span>       <span class="kv-val text-mono" id="sys-ip">–</span></div>
      <div class="kv-row"><span class="kv-key">SSID</span>     <span class="kv-val" id="sys-ssid">–</span></div>
      <div class="kv-row"><span class="kv-key">RSSI</span>     <span class="kv-val" id="sys-rssi">–</span></div>
    </div>
  </div>
</div>

<div class="card mt12">
  <div class="card-title">Firmware</div>
  <div class="kv-list">
    <div class="kv-row"><span class="kv-key">Verzija</span>    <span class="kv-val text-mono" id="sys-fw-ver">–</span></div>
    <div class="kv-row"><span class="kv-key">Build datum</span><span class="kv-val text-mono" id="sys-fw-date">–</span></div>
    <div class="kv-row"><span class="kv-key">Build čas</span>  <span class="kv-val text-mono" id="sys-fw-time">–</span></div>
    <div class="kv-row"><span class="kv-key">IDF verzija</span><span class="kv-val text-mono" id="sys-fw-idf">–</span></div>
  </div>
</div>

<!-- OTA ─────────────────────────────────────── -->
<div class="section-label mt20">OTA posodobitev firmware</div>
<div class="card">
  <div class="drop-zone" id="ota-drop" onclick="document.getElementById('ota-file').click()"
       ondragover="event.preventDefault();this.classList.add('drag-over')"
       ondragleave="this.classList.remove('drag-over')"
       ondrop="otaDrop(event)">
    <span class="dz-icon">⬆</span>
    <strong style="font-family:var(--font);font-size:12px;color:var(--text)">Klikni ali povleci .bin datoteko</strong>
    <p>Firmware za ESP32-S3 · max 3 MB</p>
  </div>
  <input type="file" id="ota-file" accept=".bin" style="display:none" onchange="otaFileSelected(this)">

  <div id="ota-progress-wrap" style="display:none;margin-top:12px">
    <div class="flex-row flex-between mb8">
      <span class="text-tiny text-mono" id="ota-filename">–</span>
      <span class="text-tiny text-mono" id="ota-pct">0%</span>
    </div>
    <div class="progress-bar"><div class="progress-fill" id="ota-bar" style="width:0%"></div></div>
    <div class="text-tiny text-dim mt8" id="ota-msg">Nalagam…</div>
  </div>
</div>

<!-- AKCIJE ──────────────────────────────────── -->
<div class="section-label mt20">Upravljanje</div>
<div class="card">
  <div class="flex-row">
    <div>
      <div class="form-label">Ponastavi konfiguracijo</div>
      <div class="form-hint">Vse nastavitve se vrnejo na privzete vrednosti</div>
    </div>
    <button class="btn btn-danger" onclick="sysResetCfg()">Reset na privzeto</button>
  </div>
  <div class="form-row" style="border-top:1px solid var(--border);margin-top:10px;padding-top:10px">
    <div>
      <div class="form-label">Restart ESP32</div>
      <div class="form-hint">Naprava se bo zagnala znova (~5 s)</div>
    </div>
    <button class="btn btn-danger" onclick="sysRestart()">Restart</button>
  </div>
</div>

<!-- Web UI statistika ───────────────────────── -->
<div class="section-label mt20">Web UI statistika</div>
<div class="card">
  <div class="kv-list">
    <div class="kv-row"><span class="kv-key">Skupaj zahtevkov</span> <span class="kv-val text-mono" id="sys-req-total">–</span></div>
    <div class="kv-row"><span class="kv-key">API zahtevkov</span>    <span class="kv-val text-mono" id="sys-req-api">–</span></div>
    <div class="kv-row"><span class="kv-key">Napake</span>           <span class="kv-val text-mono" id="sys-req-err">–</span></div>
    <div class="kv-row"><span class="kv-key">LittleFS</span>         <span class="kv-val" id="sys-lfs">–</span></div>
    <div class="kv-row"><span class="kv-key">Assets (index.html)</span><span class="kv-val" id="sys-assets">–</span></div>
  </div>
</div>
`;
  }

  // ── Posodobi UI ───────────────────────────────────────────
  function _update(d) {
    const set = (id, v) => { const el=document.getElementById(id); if(el) el.textContent=v; };

    set('sys-ts', new Date().toLocaleTimeString('sl-SI'));

    // WiFi / uptime
    const w = d.wifi || {};
    set('sys-uptime', w.uptime_ms !== undefined ? fmt.uptime(w.uptime_ms) : '–');
    set('sys-ntp',    w.ntp_ok ? (w.ntp_time || 'OK') : 'NI sinhronizirano');
    set('sys-ip',     w.ip   || '–');
    set('sys-ssid',   w.ssid || '–');
    set('sys-rssi',   w.rssi !== undefined ? w.rssi + ' dBm' : '–');

    // Firmware
    const fw = d.firmware || {};
    set('sys-fw-ver',  fw.version    || '–');
    set('sys-fw-date', fw.build_date || '–');
    set('sys-fw-time', fw.build_time || '–');
    set('sys-fw-idf',  fw.idf_ver    || '–');

    // CPU/RAM — stub vrednosti (HAL ni implementiran)
    set('sys-cpu0',  '–');
    set('sys-cpu1',  '–');
    set('sys-sram',  '–');
    set('sys-psram', '–');

    // Web UI stats — iz d.webui (web_ui.cpp _handleStatus)
    const wu = d.webui || {};
    set('sys-req-total', wu.req_total  !== undefined ? wu.req_total  : '–');
    set('sys-req-api',   wu.req_api    !== undefined ? wu.req_api    : '–');
    set('sys-req-err',   wu.req_errors !== undefined ? wu.req_errors : '–');
    set('sys-lfs',   wu.littlefs_ok !== undefined ? (wu.littlefs_ok ? 'OK' : 'napaka') : '–');
    set('sys-assets',wu.assets_ok   !== undefined ? (wu.assets_ok  ? 'OK' : 'manjka') : '–');
  }

  async function _poll() {
    try {
      const d = await api.get('/api/status');
      _update(d);
    } catch(e) {
      const el = document.getElementById('sys-ts');
      if (el) el.textContent = 'napaka: ' + e.message;
    }
  }

  // ── OTA ──────────────────────────────────────────────────
  window.otaDrop = function(ev) {
    ev.preventDefault();
    document.getElementById('ota-drop').classList.remove('drag-over');
    const f = ev.dataTransfer.files[0];
    if (f) _otaUpload(f);
  };
  window.otaFileSelected = function(inp) {
    if (inp.files[0]) _otaUpload(inp.files[0]);
    inp.value = '';
  };

  async function _otaUpload(file) {
    if (!file.name.endsWith('.bin')) { alert('Izberi .bin datoteko!'); return; }
    if (file.size > 3 * 1024 * 1024) { alert('Datoteka je prevelika (max 3 MB)!'); return; }

    showConfirm('OTA posodobitev', `Naloži "${file.name}" (${fmt.bytes(file.size)}) na napravo?`, async () => {
      const wrap = document.getElementById('ota-progress-wrap');
      const bar  = document.getElementById('ota-bar');
      const msg  = document.getElementById('ota-msg');
      const pct  = document.getElementById('ota-pct');
      const fn   = document.getElementById('ota-filename');

      if (wrap) wrap.style.display = 'block';
      if (fn)   fn.textContent = file.name;

      const fd = new FormData();
      fd.append('firmware', file, file.name);

      try {
        await new Promise((resolve, reject) => {
          const xhr = new XMLHttpRequest();
          xhr.open('POST', '/api/ota');
          xhr.upload.onprogress = (e) => {
            if (e.lengthComputable) {
              const p = Math.round(e.loaded / e.total * 100);
              if (bar) bar.style.width = p + '%';
              if (pct) pct.textContent = p + '%';
              if (msg) msg.textContent = p < 100 ? 'Nalagam…' : 'Čakam na potrditev…';
            }
          };
          xhr.onload = () => { if (xhr.status < 300) resolve(); else reject(new Error(xhr.status + ' ' + xhr.responseText)); };
          xhr.onerror = () => reject(new Error('Napaka omrežja'));
          xhr.send(fd);
        });
        if (msg) msg.textContent = 'Upload OK — naprava se reštarta…';
        if (bar) bar.className = 'progress-fill ok';
      } catch(e) {
        if (msg) msg.textContent = 'Napaka: ' + e.message;
        if (bar) bar.className = 'progress-fill err';
      }
    }, 'Naloži');
  }

  // ── Reset / Restart ───────────────────────────────────────
  window.sysResetCfg = function() {
    showConfirm('Ponastavi konfiguracijo',
      'Vse nastavitve bodo vrnjene na privzete vrednosti. Naprava se NE bo zagnala znova.',
      async () => {
        try {
          const r = await api.post('/api/config/reset', {});
          alert(r.msg || 'Ponastavitev OK');
        } catch(e) { alert('Napaka: ' + e.message); }
      }, 'Ponastavi');
  };

  window.sysRestart = function() {
    showConfirm('Restart ESP32',
      'Naprava se bo zagnala znova. Web UI bo nedostopen ~5 s.',
      async () => {
        try {
          await api.post('/api/restart', {});
        } catch { /* pričakovano — naprava se je že resetala */ }
        setTimeout(() => { window.location.reload(); }, 6000);
      }, 'Restart');
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_system = function () {
    if (!document.getElementById('sys-ts')) _render();
    registerPoller('system', _poll, 5000);
  };
})();
